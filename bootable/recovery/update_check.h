#ifndef __UPDATE_CHECK_H__
#define __UPDATE_CHECK_H__

#include "minzip/Zip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KEY_AREA_TOTAL_LEN    (0x400)

bool check_local_fastboot_valid();

int get_local_key_area(unsigned char* key_area);

int verify_fastboot(ZipArchive* zip, unsigned char* key_area);

int special_verify(ZipArchive* zip, unsigned char* key_area, const char* img_name);

int common_verify(ZipArchive* zip, unsigned char* key_area, const char* img_name);
#ifdef __cplusplus
}
#endif

#endif/*__UPDATE_CHECK_H__*/