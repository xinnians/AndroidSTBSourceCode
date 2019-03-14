/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>

#include <fs_mgr.h>
#include "mtdutils/mtdutils.h"
#include "mtdutils/mounts.h"
#include "roots.h"
#include "common.h"
#include "make_ext4fs.h"

#include "wipe.h"
#include "cryptfs.h"
#include <dirent.h>
#include "copyfile.h"
#ifndef VMX_ADVANCED_SUPPORT
#include "include/ubiutils.h"
#include "include/uuid.h"
#endif

static struct fstab *fstab = NULL;

extern struct selabel_handle *sehandle;

static const char* PERSISTENT_PATH = "/persistent";


#define SDCARD_MOUNTPOINT   "/sdcard"
#define MAX_DEV_NAME_LENGTH 64
#define MAX_RETRY_TIMES     10

static void wait_for_device(const char* fn) {
    int tries = 0;
    int ret;
    struct stat buf;
    do {
        ++tries;
        ret = stat(fn, &buf);
        if (ret) {
            LOGE("stat %s try %d: %s\n", fn, tries, strerror(errno));
            sleep(1);
        }
    } while (ret && tries < MAX_RETRY_TIMES);
    if (ret) {
        LOGE("failed to stat %s\n", fn);
    }
}


enum {
    NAND_TYPE,
    EMMC_TYPE,
    NULL_TYPE
};
static int check_flash_type()
{
    int ret = NULL_TYPE;
    char buffer[1024];
    FILE *fp;

    fp = fopen("/proc/cmdline","r");
    if (fp !=NULL ) {
        if (fgets(buffer,1024,fp) != NULL) {
            if (strstr(buffer,"hinand")) {
                ret = NAND_TYPE;
            } else if (strstr(buffer,"mmcblk")) {
                ret = EMMC_TYPE;
            } else {
                LOGE("error!!, Can't get Flash Type in Cmdline %s\n",buffer);
            }
        }
        fclose(fp);
    }
    return ret;
}

void load_volume_table()
{
    int i;
    int ret;
    if ( check_flash_type() == NAND_TYPE ) {
        LOGI("Load the recovery.fstab\n");
        fstab = fs_mgr_read_fstab("/etc/recovery.fstab");
    } else if ( check_flash_type() == EMMC_TYPE ) {
        LOGI("Load the recovery.emmc.fstab\n");
        fstab = fs_mgr_read_fstab("/etc/recovery.emmc.fstab");
    } else {
    fstab = fs_mgr_read_fstab("/etc/recovery.fstab");
    }
    if (!fstab) {
        LOGE("failed to read /etc/recovery.fstab\n");
        return;
    }

    ret = fs_mgr_add_entry(fstab, "/tmp", "ramdisk", "ramdisk");
    if (ret < 0 ) {
        LOGE("failed to add /tmp entry to fstab\n");
        fs_mgr_free_fstab(fstab);
        fstab = NULL;
        return;
    }

    printf("recovery filesystem table\n");
    printf("=========================\n");
    
    for (i = 0; i < fstab->num_entries; ++i) {
        Volume* v = &fstab->recs[i];
        printf("  %d %s %s %s %lld\n", i, v->mount_point, v->fs_type,
               v->blk_device, v->length);
    }
    printf("\n");
}

Volume* volume_for_path(const char* path) {
    return fs_mgr_get_entry_for_mount_point(fstab, path);
}

// Mount the volume specified by path at the given mount_point.
int ensure_path_mounted_at(const char* path, const char* mount_point) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        LOGE("unknown volume for path [%s]\n", path);
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 0;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return -1;
    }

    if (!mount_point) {
        mount_point = v->mount_point;
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(mount_point);
    if (mv) {
        // volume is already mounted
        return 0;
    }

    mkdir(mount_point, 0755);  // in case it doesn't already exist
#ifdef VMX_ADVANCED_SUPPORT
    if (strcmp(mount_point, "/system") == 0) {
       strncpy(v->fs_type, "squashfs", sizeof("squashfs"));
    }
#endif

    if (strcmp(v->fs_type, "yaffs2") == 0) {
        // mount an MTD partition as a YAFFS2 filesystem.
        mtd_scan_partitions();
        const MtdPartition* partition;
        partition = mtd_find_partition_by_name(v->blk_device);
        if (partition == NULL) {
            LOGE("failed to find \"%s\" partition to mount at \"%s\"\n",
                 v->blk_device, mount_point);
            return -1;
        }
        return mtd_mount_partition(partition, mount_point, v->fs_type, 0);
    }
#ifndef VMX_ADVANCED_SUPPORT
    else if (strcmp(v->fs_type, "ubifs") == 0) {
        mtd_scan_partitions();
        const MtdPartition* mtd;
        mtd = mtd_find_partition_by_name(v->blk_device);
        if (mtd == NULL) {
            LOGE("failed to find \"%s\" partition to mount at \"%s\"\n",
                 v->blk_device, v->mount_point);
            return -1;
        }
        int dev_num;
        int ret;
        ret = mtd_num2ubi_dev(mtd->device_index,&dev_num);
        if (ret == -1) {
            LOGE("%s do not attach ubi,please check\n",v->blk_device);
            return -1;
        }
        char device[20];
        sprintf(device,"/dev/ubi%d_0", dev_num);
        LOGE("try mount %s to %s\n",device,v->mount_point);
        if (mount(device, v->mount_point, v->fs_type,
                    MS_NOATIME | MS_NODEV | MS_NODIRATIME, "") < 0) {
            LOGE("%s failed to mount %s (%s)\n",device,v->mount_point, strerror(errno));
            return -1;
        }
        return 0;
    }
#endif
    else if (strcmp(v->fs_type, "ext4") == 0 ||
               strcmp(v->fs_type, "vfat") == 0 ||
               strcmp(v->fs_type, "ntfs") == 0 ||
               strcmp(v->fs_type, "squashfs") == 0 ) {
        result = mount(v->blk_device, mount_point, v->fs_type,
                       MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
        if (result == 0) return 0;

        LOGE("failed to mount %s (%s)\n", mount_point, strerror(errno));
        return -1;
    } else if (strcmp(v->fs_type, "auto") == 0) {
        wait_for_device(v->blk_device);
        if ( (result = mount(v->blk_device, mount_point, "vfat", MS_NOATIME | MS_NODEV | MS_NODIRATIME, ""))) {
            if ( (result = mount(v->blk_device, mount_point, "ext4", MS_NOATIME | MS_NODEV | MS_NODIRATIME, ""))) {
                result = mount(v->blk_device, mount_point, "ntfs", MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
            }
        }
        if (result == 0) return 0;
        LOGE("failed to mount %s (%s)\n", mount_point, strerror(errno));
        return -1;
    }

    LOGE("unknown fs_type \"%s\" for %s\n", v->fs_type, mount_point);
    return -1;
}

int ensure_path_mounted(const char* path) {
    // Mount at the default mount point.
    return ensure_path_mounted_at(path, nullptr);
}

int ensure_path_unmounted(const char* path) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        LOGE("unknown volume for path [%s]\n", path);
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted; you can't unmount it.
        return -1;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return -1;
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        // volume is already unmounted
        return 0;
    }

    return unmount_mounted_volume(mv);
}

static int exec_cmd(const char* path, char* const argv[]) {
    int status;
    pid_t child;
    if ((child = vfork()) == 0) {
        execv(path, argv);
        _exit(-1);
    }
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        LOGE("%s failed with status %d\n", path, WEXITSTATUS(status));
    }
    return WEXITSTATUS(status);
}

int format_volume(const char* volume, const char* directory) {
    Volume* v = volume_for_path(volume);
    if (v == NULL) {
        LOGE("unknown volume \"%s\"\n", volume);
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // you can't format the ramdisk.
        LOGE("can't format_volume \"%s\"", volume);
        return -1;
    }
    if (strcmp(v->mount_point, volume) != 0) {
        LOGE("can't give path \"%s\" to format_volume\n", volume);
        return -1;
    }

    if (ensure_path_unmounted(volume) != 0) {
        LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
        return -1;
    }

    if (strcmp(v->fs_type, "yaffs2") == 0 || strcmp(v->fs_type, "mtd") == 0) {
        mtd_scan_partitions();
        const MtdPartition* partition = mtd_find_partition_by_name(v->blk_device);
        if (partition == NULL) {
            LOGE("format_volume: no MTD partition \"%s\"\n", v->blk_device);
            return -1;
        }

        MtdWriteContext *write = mtd_write_partition(partition);
        if (write == NULL) {
            LOGW("format_volume: can't open MTD \"%s\"\n", v->blk_device);
            return -1;
        } else if (mtd_erase_blocks(write, -1) == (off_t) -1) {
            LOGW("format_volume: can't erase MTD \"%s\"\n", v->blk_device);
            mtd_write_close(write);
            return -1;
        } else if (mtd_write_close(write)) {
            LOGW("format_volume: can't close MTD \"%s\"\n", v->blk_device);
            return -1;
        }
        return 0;
    }

    if (strcmp(v->fs_type, "ext4") == 0 || strcmp(v->fs_type, "f2fs") == 0) {
        // if there's a key_loc that looks like a path, it should be a
        // block device for storing encryption metadata.  wipe it too.
        if (v->key_loc != NULL && v->key_loc[0] == '/') {
            LOGI("wiping %s\n", v->key_loc);
            int fd = open(v->key_loc, O_WRONLY | O_CREAT, 0644);
            if (fd < 0) {
                LOGE("format_volume: failed to open %s\n", v->key_loc);
                return -1;
            }
            wipe_block_device(fd, get_file_size(fd));
            close(fd);
        }

        ssize_t length = 0;
        if (v->length != 0) {
            length = v->length;
        } else if (v->key_loc != NULL && strcmp(v->key_loc, "footer") == 0) {
            length = -CRYPT_FOOTER_OFFSET;
        }
        int result;
        if (strcmp(v->fs_type, "ext4") == 0) {
            result = make_ext4fs_directory(v->blk_device, length, volume, sehandle, directory);
        } else {   /* Has to be f2fs because we checked earlier. */
            if (v->key_loc != NULL && strcmp(v->key_loc, "footer") == 0 && length < 0) {
                LOGE("format_volume: crypt footer + negative length (%zd) not supported on %s\n", length, v->fs_type);
                return -1;
            }
            if (length < 0) {
                LOGE("format_volume: negative length (%zd) not supported on %s\n", length, v->fs_type);
                return -1;
            }
            char *num_sectors;
            if (asprintf(&num_sectors, "%zd", length / 512) <= 0) {
                LOGE("format_volume: failed to create %s command for %s\n", v->fs_type, v->blk_device);
                return -1;
            }
            const char *f2fs_path = "/sbin/mkfs.f2fs";
            const char* const f2fs_argv[] = {"mkfs.f2fs", "-t", "-d1", v->blk_device, num_sectors, NULL};

            result = exec_cmd(f2fs_path, (char* const*)f2fs_argv);
            free(num_sectors);
        }
        if (result != 0) {
            LOGE("format_volume: make %s failed on %s with %d(%s)\n", v->fs_type, v->blk_device, result, strerror(errno));
            return -1;
        }
        return 0;
    }
#ifndef VMX_ADVANCED_SUPPORT
    if (strcmp(v->fs_type, "ubifs") == 0) {
        mtd_scan_partitions();
        const MtdPartition* mtd;
        mtd = mtd_find_partition_by_name(v->blk_device);
        if (mtd == NULL) {
            LOGE("failed to find \"%s\" partition to mount at \"%s\"\n",
                 v->blk_device, v->mount_point);
            return -1;
        }

        int dev_num;
        int ret;
        ret = mtd_num2ubi_dev(mtd->device_index,&dev_num);
        if (ret == -1) {
            LOGE("%s do not attach ubi,please check\n",v->blk_device);
            return -1;
        }
        char device[20];
        sprintf(device,"/dev/ubi%d_0", dev_num);

        int result = ubi_clear_volume(device, 0);
        if (result != 0) {
            LOGE("format_volume: clear_volume failed on %s\n", v->blk_device);
            return -1;
        }
        return 0;
    }
#endif

    LOGE("format_volume: fs_type \"%s\" unsupported\n", v->fs_type);
    return -1;
}

int format_volume(const char* volume) {
    return format_volume(volume, NULL);
}

int setup_install_mounts() {
    if (fstab == NULL) {
        LOGE("can't set up install mounts: no fstab loaded\n");
        return -1;
    }
    for (int i = 0; i < fstab->num_entries; ++i) {
        Volume* v = fstab->recs + i;

        if (strcmp(v->mount_point, "/tmp") == 0 ||
            strcmp(v->mount_point, "/cache") == 0) {
            if (ensure_path_mounted(v->mount_point) != 0) {
                LOGE("failed to mount %s\n", v->mount_point);
                return -1;
            }

        } else {
            if (ensure_path_unmounted(v->mount_point) != 0) {
                LOGE("failed to unmount %s\n", v->mount_point);
                return -1;
            }
        }
    }
    return 0;
}

int update_sdcard_volume_by_uuid(const char* uuid, const char* fstype)
{
    LOGI("uuid=%s, fstype=%s\n", uuid, fstype);

    int iRet = 0;

    if (NULL == uuid)
        return -1;

    int i;
    for (i = 0; i < fstab->num_entries; ++i) {
        if (0 == strcmp(fstab->recs[i].mount_point, SDCARD_MOUNTPOINT))
            break;
    }

    if (i == fstab->num_entries)
        return -1;

    int tries = 0;
    char *buf_dev = NULL;

    do {
        ++tries;
#ifndef VMX_ADVANCED_SUPPORT
        buf_dev = get_devname_from_uuid(uuid);
#endif
        if (buf_dev == NULL) {
            LOGW("can not get devname from uuid\n");
            sleep(1);
        } else {
            LOGI("get device name from uuid success\n");
        }
    } while (!buf_dev && tries < MAX_RETRY_TIMES);

    if (NULL != buf_dev) {
        if (fstab->recs[i].blk_device)
            free((void *)fstab->recs[i].blk_device);
        if (fstype) {
            if (fstab->recs[i].fs_type)
                free((void *)fstab->recs[i].fs_type);
            fstab->recs[i].fs_type = strdup(fstype);
        }
        fstab->recs[i].blk_device = buf_dev;
        LOGI("the new sdcard device is  \"%s\", and the new fstype is \"%s\"\n",
             fstab->recs[i].blk_device, fstab->recs[i].fs_type);
    } else {
        iRet = -1;
        LOGE("can not get device name from uuid\n");
    }

    return iRet;
}

int update_sdcard_volume_by_path(const char* path, const char* fstype)
{
    int iRet = 0;

    if (NULL == path)
        return -1;

    int i;
    for (i = 0; i < fstab->num_entries; ++i) {
        if (0 == strcmp(fstab->recs[i].mount_point, SDCARD_MOUNTPOINT))
            break;
    }

    if (i == fstab->num_entries)
        return -1;

    if (fstab->recs[i].blk_device)
        free((char *)fstab->recs[i].blk_device);
    if (fstype) {
        if (fstab->recs[i].fs_type)
            free((char *)fstab->recs[i].fs_type);
        fstab->recs[i].fs_type = strdup(fstype);
    }

    if(0 == strcmp(fstype, "ubifs")) {
         fstab->recs[i].blk_device = strdup(path);
    }else if (0 == strcmp(fstype, "yaffs2")) {
        fstab->recs[i].blk_device = strdup(path);
    } else {
        LOGE("unsupport fstype:%s\n", fstype);
        return -1;
    }

    LOGI("the new sdcard device is  \"%s\", and the new fstype is \"%s\"\n",
        fstab->recs[i].blk_device, fstab->recs[i].fs_type);

    return iRet;
}

#define DEV_DIR "/dev/block/"
#define DEV_MOUNTPOINT "/sdcard/"
//#define DEV_MOUNTPOINT "/update_test/"
static void update_volume_by_dir(const char *dir_buf)
{
    int i;
    for (i = 0; i < fstab->num_entries; ++i) {
        if (0 == strcmp(fstab->recs[i].mount_point, SDCARD_MOUNTPOINT))
            break;
    }
    if (i == fstab->num_entries)
        return ;

    if (NULL != dir_buf) {
        if (fstab->recs[i].blk_device)
            free((void *)fstab->recs[i].blk_device);
        fstab->recs[i].blk_device = strdup(dir_buf);
        fstab->recs[i].fs_type = strdup("auto");
        LOGI("the new sdcard device is  \"%s\", and the new fstype is \"%s\"\n",
        fstab->recs[i].blk_device, fstab->recs[i].fs_type);
    }
    for (i = 0; i < fstab->num_entries; ++i) {
        LOGI("the device is  \"%s\", and the mount_point is \"%s\"\n",
        fstab->recs[i].blk_device, fstab->recs[i].mount_point);
    }
}
int find_upzip_and_update_volume()
{
    DIR * dir;
    struct dirent *dirent;
    int ret,result;
    char dir_buf[255];
    char mountData[255];
    int state = -1 ;
    char temp_buf[64] = {0};
    LOGI("find update.zip \n");
    wait_for_device("/dev/block/sda");

    dir = opendir(DEV_DIR);
    if (dir) {
        while((dirent = readdir(dir))) {
            if (strstr((dirent->d_name),"sd")){
                strcpy(dir_buf,DEV_DIR);
                strcat(dir_buf,dirent->d_name);
                umount(DEV_MOUNTPOINT);
                if((result = mount(dir_buf,DEV_MOUNTPOINT,"vfat", MS_NOATIME | MS_NODEV | MS_NODIRATIME, ""))) {
                    if((result = mount(dir_buf,DEV_MOUNTPOINT,"ntfs", MS_NODEV | MS_NOSUID | MS_NODIRATIME, ""))){
                        if((result = mount(dir_buf,DEV_MOUNTPOINT,"ext4",MS_NOATIME | MS_NODEV | MS_NODIRATIME, ""))){
                            LOGI("can't mount %s\n",dir_buf);
                            continue;
                        }
                    }
                }
                sprintf(temp_buf,"/sdcard/%s",UpdatePackageName);
                ret = access(temp_buf,F_OK);
                if (ret == 0) {
                    LOGI("find update.zip in %s\n",dir_buf);
                    ret = umount(DEV_MOUNTPOINT);
                    if (ret !=0) {
                        LOGE("umount failed %s\n",dir_buf);
                    }
                    update_volume_by_dir(dir_buf);
                    state = 0;
                    break;
                } else {
                    LOGI("there is no update.zip in %s \n ",dir_buf);
                    ret = umount(DEV_MOUNTPOINT);
                    if (ret !=0) {
                        LOGE("umount failed %s\n",dir_buf);
                    }
                }
                sleep(1);
            }
        }
        closedir(dir);
    }
    return state ;
}

int write_spi_nand_clean(const char* partition) {

    mtd_scan_partitions();

    const MtdPartition* mtd = mtd_find_partition_by_name(partition);
    if (mtd == NULL) {
        printf("not find the %s\n",partition);
        LOGE("not find the %s\n",partition);
        return -1;
    }

    MtdWriteContext* ctx = mtd_write_partition(mtd);
    if (ctx == NULL) {
        LOGE("can't write %s", partition);
        return -1;
    }

    if (mtd_erase_blocks(ctx, -1) == -1) {
        printf("error erasing blocks of %s\n", partition);
        LOGE("error erasing blocks of %s\n", partition);
        return -1;
    }

    if (mtd_write_close(ctx) != 0) {
        printf("error closing write of %s\n",partition);
        LOGE("error closing write of %s\n",partition);
        return -1;
    }
    LOGI("Clean %s end\n",partition);
    return 0;

}

int write_emmc_clean(const char* partition) {

    char partition_path[1024];
    memset(partition_path,0,sizeof(partition_path));
    sprintf(partition_path,"/dev/block/platform/soc/by-name/%s",partition);
    FILE* fw = fopen(partition_path, "wb");
    if (fw == NULL) {
        printf("can't fopen %s: %s", partition_path,strerror(errno));
        LOGE("can't fopen %s: %s", partition_path,strerror(errno));
        return -1;
    }
    int fw_fd = fileno(fw);

    unsigned char readbuf[4 * 1024];
    unsigned long  nFileSize = 1;
    /*ssize_t n = 0;
    while (n != -1) {
        memset(readbuf,0xff,sizeof(readbuf));
        n = write(fw_fd,readbuf,sizeof(readbuf));
    }*/
    memset(readbuf, 0x0, sizeof(readbuf));
    write(fw_fd, readbuf, sizeof(readbuf));
    fclose(fw);
    LOGI("Clean %s end\n",partition);
    return 0;
}

int write_clean(const char* partition) {
    LOGI("Clean %s begin\n",partition);
    int ret = -1 ;
    if ( check_flash_type() == NAND_TYPE ) {
        ret = write_spi_nand_clean(partition);
    } else if ( check_flash_type() == EMMC_TYPE ) {
        ret = write_emmc_clean(partition);
    }
    return ret;
}

int update_sdcard_volume_by_dev_node(char* buf_dev) {

    int iRet = -1;
    int result = 0;
    int ret = 0;
    char temp_buf[64] = {0};

    if (NULL == buf_dev)
        return -1;

    if((result = mount(buf_dev,DEV_MOUNTPOINT,"vfat", MS_NOATIME | MS_NODEV | MS_NODIRATIME, ""))) {
        if((result = mount(buf_dev,DEV_MOUNTPOINT,"ntfs", MS_NODEV | MS_NOSUID | MS_NODIRATIME, ""))){
            if((result = mount(buf_dev,DEV_MOUNTPOINT,"ext4",MS_NOATIME | MS_NODEV | MS_NODIRATIME, ""))){
                LOGI("can't mount %s\n",buf_dev);
                return -1;
            }
        }
    }
    sprintf(temp_buf,"/sdcard/%s",UpdatePackageName);
    ret = access(temp_buf,F_OK);
    if (ret == 0) {
        LOGI("find update.zip in %s\n",buf_dev);
        ret = umount(DEV_MOUNTPOINT);
        if (ret !=0) {
            LOGE("umount failed %s\n",buf_dev);
        }
        update_volume_by_dir(buf_dev);
        iRet = 0;
    } else {
        LOGI("there is no update.zip in %s \n ",buf_dev);
        ret = umount(DEV_MOUNTPOINT);
        if (ret !=0) {
            LOGE("umount failed %s\n",buf_dev);
        }
    }
    return iRet;

}
