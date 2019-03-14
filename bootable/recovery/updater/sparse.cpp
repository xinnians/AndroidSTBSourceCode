#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include "sparse.h"


#define EMMC_BLKSIZE_SHIFT           (9)
#define SZ_1M_SHIFT                  (20)
#define BUFFER_SIZE                  (32*1024)
#define DEBUG_FOR_SPARESE_EXT4       (0)

#define DEBUG_PRINTF_EXT4INFO        (0)


/******************************************************************************/
static void print_header_info(sparse_header_t *header)
{
#if DEBUG_PRINTF_EXT4INFO
    fprintf(stderr, "sparse header info:\n");
    fprintf(stderr, "   magic: 0x%x\n",header->magic);
    fprintf(stderr, "   major_version: 0x%x\n",header->major_version);
    fprintf(stderr, "   minor_version: 0x%x\n",header->minor_version);
    fprintf(stderr, "   file_hdr_sz: %d\n",header->file_hdr_sz);
    fprintf(stderr, "   chunk_hdr_sz: %d\n",header->chunk_hdr_sz);
    fprintf(stderr, "   blk_sz: %d\n",header->blk_sz);
    fprintf(stderr, "   total_blks: %d\n",header->total_blks);
    fprintf(stderr, "   total_chunks: %d\n",header->total_chunks);
    fprintf(stderr, "   image_checksum: %d\n",header->image_checksum);
#endif
}

/******************************************************************************/
static void print_chunk_info(chunk_header_t *chunk)
{
#if DEBUG_PRINTF_EXT4INFO
    fprintf(stderr, "chunk header info:\n");
    fprintf(stderr, "   chunk_type: 0x%x\n",chunk->chunk_type);
    fprintf(stderr, "   chunk_sz: %d\n",chunk->chunk_sz);
    fprintf(stderr, "   total_sz: %d\n",chunk->total_sz);
#endif
}

int fseek_64(FILE *stream, int64_t offset, int origin) {
    if (feof(stream)) {
        rewind(stream);
    } else {
        setbuf(stream, NULL);
    }
    int fd = fileno(stream);
    if (lseek64(fd, offset, origin) == -1) {
        return errno;
    }
    return 0;
}

int write_ext4sp(const char *filename,const char *partition) {

    U32 i = 0;
    U32 num = 0;
    U64 dense_len = 0;
    U64 unsparse_len = 0;
    U64 show_point = 0;
    U64 show_point_after = 0;
    U64 img_size = 0;
    U64 chunk_len = 0;
    S64 temp_len = 0;
    U64 img_offset = 0;
    U64 ext4usp_offset = 0;
    U32 count = 0;
    chunk_header_t  chunk;
    sparse_header_t header;
    int ret = -1;

    char readbuf[32*1024];
    size_t  readsize,writesize;

    FILE *fdata = fopen(filename,"rb");
    if (fdata == NULL) {
        fprintf(stderr,"Can't open %s:%s\n",filename,strerror(errno));
        return -1;
    }

    FILE *fext4 = fopen(partition,"wb");
    if (fext4 == NULL) {
        fprintf(stderr,"Can't open %s:%s\n",partition,strerror(errno));
        goto error;
    }

    img_offset = 0;
    ext4usp_offset = 0;

    readsize = fread(&header,1,sizeof(header),fdata);
    if (readsize != sizeof(header)) {
        fprintf(stderr, " sparse_header read fail: %d\n ",ferror(fdata));
        goto error;
    }

    img_offset += header.file_hdr_sz; // just jump to the first chunk

    print_header_info((sparse_header_t *)&header);

    if (!IS_SPARSE(((sparse_header_t *)&header))) {
        fprintf(stderr,"the %s is not sparse ext4 fs\n",filename);
        goto error;
    }

    if (header.blk_sz & ((1 << EMMC_BLKSIZE_SHIFT) - 1)) {
        fprintf(stderr,"image blk size %d is not aligned to 512Byte.\n",
                header.blk_sz);
        goto error;
    }

    dense_len = 0;
    unsparse_len = 0;

    for (i = 0; i < header.total_chunks ; i++) {
        ret = fseek_64(fdata,img_offset,SEEK_SET);
        if (ret != 0) {
            printf("line is %d, ret is %d, %s\n",__LINE__,ret,strerror(errno));
            goto error;
        }
        readsize = fread(&chunk,1,sizeof(chunk),fdata);
        if (readsize != sizeof(chunk)) {
            fprintf(stderr, " chunk_header read fail: %d\n ",ferror(fdata));
            goto error;
        }
        img_offset += header.chunk_hdr_sz; // just jump to chunk_data or the next chunk header if the chunk is don't care
        print_chunk_info((chunk_header_t *)&chunk);

        switch (chunk.chunk_type) {
        case CHUNK_TYPE_RAW:
            chunk_len = chunk.chunk_sz * header.blk_sz;
            if (chunk.total_sz
                    != (chunk_len + header.chunk_hdr_sz)) {
                fprintf(stderr, "chunk.total_sz is not right\n");
                print_chunk_info((chunk_header_t *)&chunk);
                goto error;
            }


            temp_len = chunk_len;
            while (temp_len > 0) {
                count = temp_len;
                if (count > BUFFER_SIZE) {
                    count = BUFFER_SIZE;
                }
                memset(readbuf,0x00,sizeof(readbuf));
                ret = fseek_64(fdata,img_offset,SEEK_SET);
                if (ret != 0) {
                    printf("line is %d,ret is %d, %s\n",__LINE__,ret,strerror(errno));
                    goto error;
                }
                readsize = fread(readbuf,1,count,fdata);
                if (readsize != count) {
                    fprintf(stderr," read chunk_data failure at img_offset =0x%llx: %d\n",img_offset,ferror(fdata));
                    goto error;
                }
                writesize = fwrite(readbuf,1,count,fext4);
                if (writesize != count) {
                    fprintf(stderr, "write chunk_data failure %d\n ",ferror(fext4));
                    goto error;
                }
                img_offset += count;
                temp_len -= count;
                ext4usp_offset += count;
                unsparse_len += count;
                dense_len += count;

                show_point = (dense_len >> SZ_1M_SHIFT);
                if ( show_point != show_point_after) {
                    fflush(fext4);
                    fsync(fileno(fext4));
                    show_point_after = show_point;
                    fprintf(stderr, ". ");
                }
            }

            break;

        case  CHUNK_TYPE_DONT_CARE:
            chunk_len = chunk.chunk_sz * header.blk_sz;
            if (chunk.total_sz
                    != header.chunk_hdr_sz) {
                fprintf(stderr ,"chunk.total_sz is not right\n");
                print_chunk_info((chunk_header_t *)&chunk);
                goto error;
            }

            temp_len = chunk_len;
            while (temp_len > 0) {
                count = temp_len;
                if (count > BUFFER_SIZE) {
                    count = BUFFER_SIZE;
                }
                #if DEBUG_FOR_SPARESE_EXT4
                memset(readbuf,0x00,sizeof(readbuf));
                writesize = fwrite(readbuf,1,count,fext4);
                if (writesize != count) {
                    fprintf(stderr, "write chunk_data failure %d\n ",ferror(fext4));
                    goto error;
                }
                #else
                ret = fseek_64(fext4,count,SEEK_CUR);
                if (ret != 0) {
                    fprintf(stderr, "\nunspare_len=0x%llx,ext4usp_offset=0x%llx\n",unsparse_len,ext4usp_offset);
                    printf("line is %d, ret is %d, %s\n",__LINE__,ret,strerror(errno));
                    goto error;
                }
                #endif
                temp_len -= count;
                ext4usp_offset += count;
            }

            #if DEBUG_PRINTF_EXT4INFO
            fprintf(stderr, "\nunspare_len=0x%llx,ext4usp_offset=0x%llx\n",unsparse_len,ext4usp_offset);
            #endif

            unsparse_len += chunk_len;

            break;
        case  CHUNK_TYPE_FILL:
        default:
            break;
        }
    }

    fclose(fdata);
    fflush(fext4);
    fsync(fileno(fext4));
    fclose(fext4);
    fprintf(stderr, "\ndense_len= 0x%llx:%lldMB, unsparse_len =0x%llx:%lldMB\n",dense_len,dense_len >> SZ_1M_SHIFT,unsparse_len,(unsparse_len >> SZ_1M_SHIFT));

    return 0;
    error:
    fclose(fdata);
    fclose(fext4);
    return -1;
}
