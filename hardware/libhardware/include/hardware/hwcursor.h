#ifndef ANDROID_HWCURSOR_INTERFACE_H
#define ANDROID_HWCURSOR_INTERFACE_H
#include <hardware/hardware.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <cutils/native_handle.h>
#include <hardware/hardware.h>


__BEGIN_DECLS

/**
 * The id of this module
 */
#define HWCURSOR_HARDWARE_MODULE_ID "hwcursor"

/**
 * Name of the hwcursor device to open
 */

#define HWCURSOR_HARDWARE_HWCURSOR0 "hwcursor0"

/*****************************************************************************/
typedef struct hwcursor_module {
    struct hw_module_t common;
} hwcursor_module_t;


typedef struct hwcursor_device_t {
    struct hw_device_t common;
    void * pFbAddr;
    float factorX, factorY;
    int (*show)(struct hwcursor_device_t* window);
    int (*hide)(struct hwcursor_device_t* window);
    int (*setPosition)(struct hwcursor_device_t* window, int positionX, int positionY, int hotSpotX, int hotSpotY);
    int (*setAlpha)(struct hwcursor_device_t* window, float alpha);
    int (*getStride)(struct hwcursor_device_t* window);
    bool (*init)(struct hwcursor_device_t* window,int iconWidth, int iconHeight);
    int (*setMatrix)(struct hwcursor_device_t* window, float dsdx, float dtdx, float dsdy, float dtdy);
    int (*setIcon)(struct hwcursor_device_t* window);
} hwcursor_device_t;


/** convenience API for opening and closing a supported device */

static inline int hwcursor_open(const struct hw_module_t* module,
         hwcursor_device_t** device) {
    return module->methods->open(module,
            HWCURSOR_HARDWARE_HWCURSOR0, (struct hw_device_t**)device);
}

static inline int hwcursor_close(struct hwcursor_device_t* device) {
    return device->common.close(&device->common);
}

__END_DECLS

#endif  // ANDROID_HWCURSOR_INTERFACE_H


