#include <stdio.h>
#include <unistd.h>
#include <string.h>

#define BUFFER_SIZE 4096
int copyFile(const char *sourceFile,const char *targetFile)
{
    FILE *fpR, *fpW;
    char buffer[BUFFER_SIZE];
    int lenR, lenW;
    if ((fpR = fopen(sourceFile, "r")) == NULL)
    {
        printf("The file '%s' can not be opened! \n", sourceFile);
        return -1;
    }
    if ((fpW = fopen(targetFile, "w")) == NULL)
    {
        printf("The file '%s' can not be opened! \n", targetFile);
        fclose(fpR);
        return -1;
    }

    memset(buffer,0,BUFFER_SIZE);
    while ((lenR = fread(buffer, 1, BUFFER_SIZE, fpR)) > 0)
    {
        if ((lenW = fwrite(buffer, 1, lenR, fpW)) != lenR)
        {
            printf("Write to file '%s' failed!\n", targetFile);
            fclose(fpR);
            fclose(fpW);
            return -1;
        }
        memset(buffer, 0, BUFFER_SIZE);
    }

    fclose(fpR);
    fclose(fpW);
    return 0;
}
