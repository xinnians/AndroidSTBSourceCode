#include <stdio.h>
#include <string.h>
#include "verifytool.h"
#include "minzip/Zip.h"
#include "common.h"
#include "verifier.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "update_check.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASSUMED_FASTBOOT_NAME    "fastboot.img"
#define RSA_KEY_LEN    (0x200)
#define PARAM_AREA_LEN    (0x2ac0)
#define RSA2048_SIGN_LEN    (0x100)
#define SHA256_LEN    (0x20)
#define CAIMGHEAD_MAGICNUMBER    "Hisilicon_ADVCA_ImgHead_MagicNum"
#define MAGIC_NUM_LEN    (0x20)

#define PARAM_SIGN_OFFSET    (KEY_AREA_TOTAL_LEN + PARAM_AREA_LEN)//0x2ec0
#define AUXILIARY_CODE_OFFSET    (PARAM_SIGN_OFFSET + RSA2048_SIGN_LEN + 0x40)//0x3000
#define AUXILIARY_CODE_LEN_OFFSET    (0x218)

#define FLASH_ADVCA_UBOOT_BEGIN_OFFSET    (0x0)
#define FLASH_ADVCA_UBOOT_FLAG_OFFSET    (FLASH_ADVCA_UBOOT_BEGIN_OFFSET + 0x2fc4)

#define LOCAL_FASTBOOT_PARTITION    "/dev/block/platform/soc/f9830000.himciv200.MMC/by-name/fastboot"

#define SIGN_BLOCK_SIZE (0x2000)
#define OFFSET_ACTUAL_DATA_SIZE (0x34)
#define OFFSET_SIG_DATA (0x74)

typedef struct hi_CAImgHead_S
{
    unsigned char u8MagicNumber[32];                    //Magic Number: "Hisilicon_ADVCA_ImgHead_MagicNum"
    unsigned char u8HeaderVersion[8];                         //version: "V000 0003"
    unsigned int u32TotalLen;                          //Total length
    unsigned int u32CodeOffset;                        //Image offset
    unsigned int u32SignedImageLen;                    //Signed Image file size
    unsigned int u32SignatureOffset;                   //Signed Image offset
    unsigned int u32SignatureLen;                      //Signature length
    unsigned int u32BlockNum;                          //Image block number
    unsigned int u32BlockOffset[5];    //Each Block offset
    unsigned int u32BlockLength[5];    //Each Block length
    unsigned int u32SoftwareVerion;                    //Software version
    unsigned int Reserverd[31];
    unsigned int u32CRC32;                             //CRC32 value
} HI_CAImgHead_S;

static void dump_hex(const char*name, unsigned char* data, int len){
    int i;
    char temp[128] = {0};
    char temp_final[128] = {0};
    LOGI("%s length: %d\n", name, len);
    if (data == NULL) {
        LOGI("data is NULL\n");
        return;
    }

    for (i = 0; i < len; i++) {
        if( (i > 0) && (i % 16 == 0))
        {
            LOGI("%s\n", temp_final);
            memset(temp_final, 0, 128);
        }
        sprintf(temp,"%02X ", (char)data[i]);
        strcat(temp_final, temp);
    }
    LOGI("%s\n", temp_final);
    return;
}

bool check_local_fastboot_valid(){
    unsigned char param[4] = {0};
    unsigned char valid_flag[4] = {0x0d, 0x59, 0x5a, 0x43};
    FILE* fp = fopen(LOCAL_FASTBOOT_PARTITION, "rb");
    if (fp == NULL) {
        LOGE("Can't open %s\n", LOCAL_FASTBOOT_PARTITION);
        return false;
    }
    fseek(fp, FLASH_ADVCA_UBOOT_FLAG_OFFSET, SEEK_SET);
    if (fread(param, sizeof(param), 1, fp) <= 0)
    {
        LOGE("Can't read %s\n", LOCAL_FASTBOOT_PARTITION);
        fclose(fp);
        return false;
    }
    //dump_hex("param", param, sizeof(param));
    fclose(fp);

    if (!memcmp(param, valid_flag, sizeof(param)))
    {
        LOGI("Find valid fastboot on flash!\n");
        return true;
    }
    LOGI("No valid fastboot on flash!\n");
    return false;
}

int get_local_key_area(unsigned char* key_area){
    FILE* fp = fopen(LOCAL_FASTBOOT_PARTITION, "rb");
    if (fp == NULL) {
        LOGE("Can't open %s\n", LOCAL_FASTBOOT_PARTITION);
        return -1;
    }
    fseek(fp, FLASH_ADVCA_UBOOT_BEGIN_OFFSET, SEEK_SET);
    if (fread(key_area, KEY_AREA_TOTAL_LEN, 1, fp) <= 0)
    {
        LOGE("Can't read %s\n", LOCAL_FASTBOOT_PARTITION);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

int verify_fastboot(ZipArchive* zip, unsigned char* key_area){
    const ZipEntry* fastboot_entry = NULL;
    const char* update_fastboot = "/tmp/fastboot";
    unsigned char update_key_area[KEY_AREA_TOTAL_LEN] = {0};
    unsigned char update_param_area[PARAM_AREA_LEN] = {0};
    unsigned char* update_auxcode_area = NULL;
    unsigned char* update_boot_area = NULL;
    unsigned char hash[SHA256_LEN] = {0};
    unsigned char sign[RSA2048_SIGN_LEN] = {0};
    unsigned char rsa_key[RSA_KEY_LEN] = {0};
    int fd = -1;
    bool ok = false;
    FILE* fp = NULL;
    int ret = -1;
    int boot_checked_area_offset = -1;
    int boot_checked_area_len = -1;
    int auxcode_area_len = -1;

    fastboot_entry = mzFindZipEntry(zip, ASSUMED_FASTBOOT_NAME);

    if (fastboot_entry != NULL) {
        unlink(update_fastboot);
        fd = creat(update_fastboot, 0755);
        if (fd < 0) {
            LOGE("Can't make %s\n", update_fastboot);
            return VERIFY_FAILURE;
        }
        ok = mzExtractZipEntryToFile(zip, fastboot_entry, fd);
        close(fd);
        if (!ok) {
            LOGE("Can't copy %s\n", ASSUMED_FASTBOOT_NAME);
            return VERIFY_FAILURE;
        }

        fp = fopen(update_fastboot, "rb");
        if (fp == NULL) {
            LOGE("Can't open %s\n", update_fastboot);
            return VERIFY_FAILURE;
        }
        LOGI("Verify fastboot start!\n");
        //verify keyArea
        fseek(fp, 0, SEEK_SET);
        if (fread(update_key_area, KEY_AREA_TOTAL_LEN, 1, fp) <= 0)
        {
            LOGE("Read update_key_area failed!\n");
            fclose(fp);
            return VERIFY_FAILURE;
        }
        //dump_hex("update_key_area", update_key_area, KEY_AREA_TOTAL_LEN);
        if (memcmp(update_key_area, key_area, RSA_KEY_LEN) != 0){
            LOGE("Verify key area failed!\n");
            fclose(fp);
            return VERIFY_FAILURE;
        }
        LOGI("Verify key area OK!\n");

        memcpy(rsa_key, key_area, RSA_KEY_LEN);    //get rsa key

        //verify paramArea
        fseek(fp, KEY_AREA_TOTAL_LEN, SEEK_SET);
        if (fread(update_param_area, PARAM_AREA_LEN, 1, fp) <= 0)    //read param data
        {
            LOGE("Read update_param_area failed!\n");
            fclose(fp);
            return VERIFY_FAILURE;
        }
        Sha256(update_param_area, PARAM_AREA_LEN, hash);    //calculate hash

        fseek(fp, PARAM_SIGN_OFFSET, SEEK_SET);
        if (fread(sign, RSA2048_SIGN_LEN, 1, fp) <= 0)    //read signature
        {
            LOGE("Read update_param_area signature failed!\n");
            fclose(fp);
            return VERIFY_FAILURE;
        }
        //dump_hex("param_sign", sign, RSA2048_SIGN_LEN);
        ret = VerifySignRSA(hash, sign, rsa_key);    //rsa2048 verify
        if (ret != 0){
            LOGE("Verify param area failed!\n");
            fclose(fp);
            return VERIFY_FAILURE;
        }
        LOGI("Verify param area OK!\n");

        //verify auxiliary code area
        auxcode_area_len = *(int*)(update_key_area + AUXILIARY_CODE_LEN_OFFSET);
        update_auxcode_area = new unsigned char[auxcode_area_len];
        fseek(fp, AUXILIARY_CODE_OFFSET, SEEK_SET);
        if (fread(update_auxcode_area, auxcode_area_len, 1, fp) <= 0)    //read auxcode data
        {
            LOGE("Read update_auxcode_area failed!\n");
            fclose(fp);
            delete []update_auxcode_area;
            return VERIFY_FAILURE;
        }
        Sha256(update_auxcode_area, auxcode_area_len - RSA2048_SIGN_LEN, hash);    //calculate hash
        memcpy(sign, update_auxcode_area + auxcode_area_len - RSA2048_SIGN_LEN, RSA2048_SIGN_LEN); // last 256 byte is signature
        //dump_hex("auxcode_sign", sign, RSA2048_SIGN_LEN);
        delete []update_auxcode_area;
        update_auxcode_area = NULL;

        ret = VerifySignRSA(hash, sign, rsa_key);    //rsa2048 verify
        if (ret != 0){
            LOGE("Verify auxcode area failed!\n");
            fclose(fp);
            return VERIFY_FAILURE;
        }
        LOGI("Verify auxcode area OK!\n");

        //verify bootArea
        boot_checked_area_offset = *(int*)update_param_area + AUXILIARY_CODE_OFFSET + auxcode_area_len;
        boot_checked_area_len = *(int *)(update_param_area + 4);

        update_boot_area = new unsigned char[boot_checked_area_len];
        fseek(fp, boot_checked_area_offset, SEEK_SET);
        if (fread(update_boot_area, boot_checked_area_len, 1, fp) <= 0)    //read boot_area data
        {
            LOGE("Read update_boot_area failed!\n");
            fclose(fp);
            delete []update_boot_area;
            return VERIFY_FAILURE;
        }
        Sha256(update_boot_area, boot_checked_area_len, hash);    //calculate hash
        delete []update_boot_area;
        update_boot_area = NULL;

        fseek(fp, boot_checked_area_offset + boot_checked_area_len, SEEK_SET);
        if (fread(sign, RSA2048_SIGN_LEN, 1, fp) <= 0)    //read signature
        {
            LOGE("Read update_boot_area signature failed!\n");
            fclose(fp);
            return VERIFY_FAILURE;
        }
        //dump_hex("boot_sign", sign, RSA2048_SIGN_LEN);
        ret = VerifySignRSA(hash, sign, rsa_key);    //rsa2048 verify
        if (ret != 0){
            LOGE("Verify boot area failed!\n");
            fclose(fp);
            return VERIFY_FAILURE;
        }
        LOGI("Verify boot area OK!\n");

        LOGI("Verify fastboot OK!\n");
        fclose(fp);
    }else{
        LOGI("Can't find fastboot in update.zip, need not verify!\n");
    }
    return VERIFY_SUCCESS;
}

int special_verify(ZipArchive* zip, unsigned char* key_area, const char* img_name){
    const ZipEntry* image_entry = NULL;
    const char* update_image = NULL;
    HI_CAImgHead_S* image_head = NULL;
    unsigned char* image_data = NULL;
    unsigned char hash[SHA256_LEN] = {0};
    unsigned char sign[RSA2048_SIGN_LEN] = {0};
    unsigned char rsa_key[RSA_KEY_LEN] = {0};
    int fd = -1;
    bool ok = false;
    FILE* fp = NULL;
    int ret = -1;

    if (0 == strcmp(img_name, "boot.img")) {
        image_entry = mzFindZipEntry(zip, img_name);
        update_image = "/tmp/kernel";
    } else if (0 == strcmp(img_name, "recovery.img")) {
        image_entry = mzFindZipEntry(zip, img_name);
        update_image = "/tmp/recovery";
    } else if (0 == strcmp(img_name, "trustedcore.img")) {
        image_entry = mzFindZipEntry(zip, img_name);
        update_image = "/tmp/trustedcore";
    }
    if (image_entry != NULL) {
        unlink(update_image);
        fd = creat(update_image, 0755);
        if (fd < 0) {
            LOGE("Can't make %s\n", update_image);
            return VERIFY_FAILURE;
        }
        ok = mzExtractZipEntryToFile(zip, image_entry, fd);
        close(fd);
        if (!ok) {
            LOGE("Can't copy %s\n", img_name);
            return VERIFY_FAILURE;
        }
        fp = fopen(update_image, "rb");
        if (fp == NULL) {
            LOGE("Can't open %s\n", update_image);
            return VERIFY_FAILURE;
        }
        LOGI("Verify %s start!\n", img_name);
        memcpy(rsa_key, key_area, RSA_KEY_LEN);    //get rsa key
        //verify image
        image_head = new HI_CAImgHead_S;

        fseek(fp, 0, SEEK_SET);
        if (fread(image_head, sizeof(HI_CAImgHead_S), 1, fp) <= 0)
        {
            LOGE("Read image_head failed!\n");
            fclose(fp);
            delete image_head;
            return VERIFY_FAILURE;
        }
        //dump_hex("image_head", (unsigned char*)image_head, sizeof(HI_CAImgHead_S));
        if (memcmp(image_head->u8MagicNumber, CAIMGHEAD_MAGICNUMBER, MAGIC_NUM_LEN) != 0){    //check magic number
            LOGE("Verify image head maigc number failed!\n");
            fclose(fp);
            delete image_head;
            return VERIFY_FAILURE;
        }

        image_data = new unsigned char[image_head->u32SignedImageLen];
        fseek(fp, 0, SEEK_SET);
        if (fread(image_data, image_head->u32SignedImageLen, 1, fp) <= 0)    //read image data
        {
            LOGE("Read image_data failed!\n");
            fclose(fp);
            delete image_head;
            delete []image_data;
            return VERIFY_FAILURE;
        }
        Sha256(image_data, image_head->u32SignedImageLen, hash);    //calc hash
        //dump_hex("image_hash", hash, SHA256_LEN);
        delete []image_data;
        image_data = NULL;

        fseek(fp, image_head->u32SignatureOffset, SEEK_SET);
        if (fread(sign, RSA2048_SIGN_LEN, 1, fp) <= 0)    //read signature
        {
            LOGE("Read image_data signature failed!\n");
            fclose(fp);
            delete image_head;
            return VERIFY_FAILURE;
        }
        //dump_hex("image_sign", sign, RSA2048_SIGN_LEN);
        delete image_head;
        image_head = NULL;

        ret = VerifySignRSA(hash, sign, rsa_key);    //rsa2048 verify
        if (ret != 0){
            LOGE("Verify %s failed!\n", img_name);
            fclose(fp);
            return VERIFY_FAILURE;
        }
        LOGI("Verify %s OK!\n", img_name);
        fclose(fp);
    }else{
        LOGI("Can't find %s in update.zip, need not verify!\n", img_name);
    }
    return VERIFY_SUCCESS;
}

int common_verify(ZipArchive* zip, unsigned char* key_area, const char* img_name){
    const ZipEntry* image_entry = NULL;
    const char* update_image = NULL;
    unsigned char* image_data = NULL;
    unsigned char hash[SHA256_LEN] = {0};
    unsigned char sign[RSA2048_SIGN_LEN] = {0};
    unsigned char rsa_key[RSA_KEY_LEN] = {0};
    int fd = -1;
    bool ok = false;
    FILE* fp = NULL;
    int ret = -1;
    unsigned int fileSize = 0;
    unsigned int actualDataLen = 0;
    int offset = 0;

    if (0 == strcmp(img_name, "bootargs.img")) {
        image_entry = mzFindZipEntry(zip, img_name);
        update_image = "/tmp/bootargs";
    }
    if (image_entry != NULL) {
        unlink(update_image);
        fd = creat(update_image, 0755);
        if (fd < 0) {
            LOGE("Can't make %s\n", update_image);
            return VERIFY_FAILURE;
        }
        ok = mzExtractZipEntryToFile(zip, image_entry, fd);
        close(fd);
        if (!ok) {
            LOGE("Can't copy %s\n", img_name);
            return VERIFY_FAILURE;
        }
        fp = fopen(update_image, "rb");
        if (fp == NULL) {
            LOGE("Can't open %s\n", update_image);
            return VERIFY_FAILURE;
        }
        LOGI("Verify %s start!\n", img_name);
        memcpy(rsa_key, key_area, RSA_KEY_LEN);    //get rsa key

        //get file length
        fseek(fp, 0, SEEK_END);
        fileSize = ftell(fp);
        offset = fileSize - SIGN_BLOCK_SIZE + OFFSET_ACTUAL_DATA_SIZE;
        fseek(fp, offset, SEEK_SET);
        if (fread(&actualDataLen, 1, sizeof(int), fp) != sizeof(int))
        {
            LOGE("Read actualDataLen fail\n");
            fclose(fp);
            return VERIFY_FAILURE;
        }
        //read actual data
        image_data = new unsigned char[actualDataLen];
        fseek(fp, 0, SEEK_SET);
        if (fread(image_data, 1, actualDataLen, fp) != actualDataLen)
        {
            LOGE("Read actualData fail\n");
            fclose(fp);
            delete []image_data;
            return VERIFY_FAILURE;
        }

        //calculate data hash
        Sha256(image_data, actualDataLen, hash);
        delete []image_data;

        //read signature
        offset = fileSize - SIGN_BLOCK_SIZE + OFFSET_SIG_DATA;
        fseek(fp, offset, SEEK_SET);
        if (fread(sign, 1, RSA2048_SIGN_LEN, fp) != RSA2048_SIGN_LEN)
        {
            LOGE("Read system_list signature fail\n");
            fclose(fp);
            return VERIFY_FAILURE;
        }

        //verify signature
        ret = VerifySignRSA(hash, sign, rsa_key);    //rsa2048 verify
        if (ret != 0){
            LOGE("Verify %s failed!\n", img_name);
            fclose(fp);
            return VERIFY_FAILURE;
        }
        LOGI("Verify %s OK!\n", img_name);
        fclose(fp);
    }else{
        LOGI("Can't find %s in update.zip, need not verify!\n", img_name);
    }
    return VERIFY_SUCCESS;
}
#ifdef __cplusplus
}
#endif