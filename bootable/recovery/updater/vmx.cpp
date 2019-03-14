#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "common.h"

#include "cutils/properties.h"
#include "edify/expr.h"
#include "mincrypt/sha.h"
#include "minzip/DirUtil.h"
#include "updater.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include "mtdutils/mounts.h"
#include "mtdutils/mtdutils.h"
#include <dirent.h>
#include "sparse.h"
#include "vmx.h"

#include "hi_common.h"
#include "hi_unf_advca.h"

static const int MAX_ARGS = 100;
#define VMX_SIG_OFFSET  (16)
#define VMX_SIG_LENGTH  (256)
#define VMX_IMG_OFFSET  (VMX_SIG_OFFSET + VMX_SIG_LENGTH)
#define VMX_HEAD_LENGTH (VMX_IMG_OFFSET)
#define VMX_HEAD_IMAGE_LEN (4)
#define RECOVERY_MODE 1

int verify_result(unsigned char *data) {
    int ret = -1;
    int result = -1;
    unsigned char errorCode;

    #ifdef HI_ADVCA_VMX3RD_SUPPORT
    printf("support HI_ADVCA_VMX3RD_SUPPORT\n");
    VMX_APPLICATION_HEADER_S stAppHeader;
    memset(&stAppHeader, 0, sizeof(VMX_APPLICATION_HEADER_S));
    memcpy(&stAppHeader, data, VMX_SIG_OFFSET);
    unsigned int len = stAppHeader.u32ImageLen;
    unsigned char enc_ctrl = stAppHeader.enc_ctrl.enc_flag;
    printf("---------enc_ctrl is 0x%x----------\n",enc_ctrl);
    VMX3RD_SetEncFlag(enc_ctrl);
    #else
    printf("support HI_ADVCA_VMX_ORIGIN_SUPPORT\n");
    unsigned int len = *(unsigned int*)data;
    #endif
    printf("---------len is 0x%x----------\n",len);

    if ( 0 != (len & 0xf)) {
        printf("Invlid len is 0x%x\n",len);
	return -1;
    }


    unsigned char signature_data[VMX_SIG_LENGTH];
    memcpy(signature_data, data + VMX_SIG_OFFSET, VMX_SIG_LENGTH);
    int i = 0;
    printf("---------signature_data is: ----------\n");
    for (i =0; i< VMX_SIG_LENGTH; i++) {
        printf("0x%x ", signature_data[i]);

        if((i+1)%16 == 0)
            printf("\n");

    }
    unsigned char *src = data + VMX_IMG_OFFSET;
    unsigned char *buffer = (unsigned char *)malloc(len);

    if (buffer == NULL) {
         printf("failed to allocate 0x%x bytesn",len);
         return -1;
    }

    ret = verifySignature(signature_data, src, buffer, len, len, RECOVERY_MODE, &errorCode);
    if (1 == ret) {
        printf("verify success! ret:0x%x, Continue ...\n", ret);
        result = 1;
    } else if((1 == errorCode) && (0 == ret)) {
        printf("do not start the application, reset! errorCode: 0x%x, ret: 0x%x, Resetting ...\n", errorCode, ret);
        result = 2;
    } else if((2 == errorCode) && (0 == ret)) {
        printf("verify success! && errorCode: 0x%x, ret: 0x%x, Resetting ...\n", errorCode, ret);
        result = 0;
    }
    free(buffer);
    return result;
}

Value* vmx_verify(const char* zip_path, const char* dest_path, ZipArchive* za, State* state) {

    bool success = false;
    printf("---------join into TestVerify -------\n");
    const ZipEntry* entry = mzFindZipEntry(za, zip_path);
    if (entry == NULL) {
        printf("no %s in package\n", zip_path);
    }

    FILE* f = fopen(dest_path, "wb");
    if (f == NULL) {
        printf("can't open %s for write: %s\n",
                dest_path, strerror(errno));
        //goto done2;
    }
    Value* v = reinterpret_cast<Value*>(malloc(sizeof(Value)));
    v->type = VAL_BLOB;
    v->size = -1;
    v->data = NULL;
    v->size = mzGetZipEntryUncompLen(entry);
    v->data = reinterpret_cast<char*>(malloc(v->size));
    if (v->data == NULL) {
         printf("failed to allocate %ld bytes for %s\n",
                  (long)v->size, zip_path);
        free(v->data);
        v->data = NULL;
        v->size = -1;
    }
    success = mzExtractZipEntryToBuffer(za, entry,
                             (unsigned char *)v->data);
    int verfiy = verify_result((unsigned char *)v->data);
    if (verfiy == 0) {
        printf("-------start write data --------\n");
        if (write(fileno(f),v->data,v->size) < 0) {
            printf("write date failed !!!! \n");
            success =false;
            goto vmxDone;
        }
        success = true;
    } else if(verfiy == 1){
        printf("---------verify success but not update image------\n");
        success = true;
        goto vmxDone;
    } else {
        success = false;
        printf("---------verigfy failed------\n");
        free(v->data);
        v->data = NULL;
        v->size = -1;
        return ErrorAbort(state, "%s\" not a valid image", zip_path);
    }
    fclose(f);
    vmxDone:
        free(v->data);
        v->data = NULL;
        v->size = -1;
    return StringValue(strdup(success ? "t" : ""));
}
