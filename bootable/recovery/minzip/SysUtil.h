/*
 * Copyright 2006 The Android Open Source Project
 *
 * System utilities.
 */
#ifndef _MINZIP_SYSUTIL
#define _MINZIP_SYSUTIL

#include <stdio.h>
#include "inline_magic.h"
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MappedRange {
    void* addr;
    size_t length;
} MappedRange;

/*
 * Use this to keep track of mapped segments.
 */
typedef struct MemMapping {
    unsigned char* addr;           /* start of data */
    size_t         length;         /* length of data */

    void*   baseAddr;       /* page-aligned base address */
    size_t  baseLength;     /* length of mapping */
    int            range_count;
    MappedRange*   ranges;
} MemMapping;
INLINE void sysCopyMap(MemMapping* dst, const MemMapping* src) {
    *dst = *src;
}
/*
 * Map a file into a private, read-only memory segment.  If 'fn'
 * begins with an '@' character, it is a map of blocks to be mapped,
 * otherwise it is treated as an ordinary file.
 *
 * On success, "pMap" is filled in, and zero is returned.
 */
int sysMapFileInShmem(int fd, MemMapping* pMap);
int sysMapFile(const char* fn, MemMapping* pMap);

/*
 * Release the pages associated with a shared memory segment.
 *
 * This does not free "pMap"; it just releases the memory.
 */
void sysReleaseShmem(MemMapping* pMap);
void sysReleaseMap(MemMapping* pMap);

#ifdef __cplusplus
}
#endif

#endif /*_MINZIP_SYSUTIL*/
