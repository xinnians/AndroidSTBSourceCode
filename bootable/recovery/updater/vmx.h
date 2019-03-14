#include "edify/expr.h"
#include "minzip/Zip.h"


Value* vmx_verify(const char* zip_path, const char* dest_path, ZipArchive* za, State* state);
int verify_result(unsigned char* data);
#ifdef __cplusplus
extern "C" {
#endif
int verifySignature
(
    unsigned char*  signature,
    unsigned char*  src,
    unsigned char*  tmp,
    unsigned int    len,
    unsigned int    maxLen,
    unsigned char   mode,
    unsigned char*  errorCode
);
}
