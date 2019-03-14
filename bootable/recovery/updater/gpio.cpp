#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>

#include "hi_common.h"
#include "hi_unf_gpio.h"
#include "gpio.h"

void flashled() {
    unsigned val;
    HI_SYS_VERSION_S enChipVersion;
    (HI_VOID)HI_SYS_Init();
    int ret = HI_UNF_GPIO_Init();
    if (HI_SUCCESS != ret) {
        printf("%s: %d ErrorCode=0x%x\n", __FILE__, __LINE__, ret);
    }
    memset(&enChipVersion, 0, sizeof(HI_SYS_VERSION_S));
    ret = HI_SYS_GetVersion(&enChipVersion);
    if (HI_FAILURE == ret)
    {
        printf("Can't get chip info!\n");
    }

    if (((HI_CHIP_TYPE_HI3798M == enChipVersion.enChipTypeHardWare)||(HI_CHIP_TYPE_HI3798C == enChipVersion.enChipTypeHardWare))
            && (HI_CHIP_VERSION_V200 == enChipVersion.enChipVersion)) {
        printf("Hi3798(C/M)V200,blink LED ...\n");
        HI_UNF_GPIO_SetDirBit(0x29, HI_FALSE);
        while (1) {
            HI_UNF_GPIO_WriteBit(0x29, HI_FALSE);
            sleep(1);
            HI_UNF_GPIO_WriteBit(0x29, HI_TRUE);
            sleep(1);
        }
    }
    ret = HI_UNF_GPIO_DeInit();
    if (HI_SUCCESS != ret)
    {
        printf("%s: %d ErrorCode=0x%x\n", __FILE__, __LINE__, ret);
    }
    (HI_VOID)HI_SYS_DeInit();
}
void close_led(int pid) {
    if ( 0 <= pid) {
        kill(pid,SIGKILL);
    }
}
