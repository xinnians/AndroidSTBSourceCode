/*
 * Copyright 2008, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>

#include "hardware_legacy/wifi.h"
#ifdef LIBWPA_CLIENT_EXISTS
#include "libwpa_client/wpa_ctrl.h"
#endif

#define LOG_TAG "WifiHW"
#include "cutils/log.h"
#include "cutils/memory.h"
#include "cutils/misc.h"
#include "cutils/properties.h"
#include "private/android_filesystem_config.h"

#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>

extern int do_dhcp();
extern int ifc_init();
extern void ifc_close();
extern char *dhcp_lasterror();
extern void get_dhcp_info();
extern int init_module(void *, unsigned long, const char *);
extern int delete_module(const char *, unsigned int);
void wifi_close_sockets();

#ifndef LIBWPA_CLIENT_EXISTS
#define WPA_EVENT_TERMINATING "CTRL-EVENT-TERMINATING "
struct wpa_ctrl {};
void wpa_ctrl_cleanup(void) {}
struct wpa_ctrl *wpa_ctrl_open(const char *ctrl_path) { return NULL; }
void wpa_ctrl_close(struct wpa_ctrl *ctrl) {}
int wpa_ctrl_request(struct wpa_ctrl *ctrl, const char *cmd, size_t cmd_len,
	char *reply, size_t *reply_len, void (*msg_cb)(char *msg, size_t len))
	{ return 0; }
int wpa_ctrl_attach(struct wpa_ctrl *ctrl) { return 0; }
int wpa_ctrl_detach(struct wpa_ctrl *ctrl) { return 0; }
int wpa_ctrl_recv(struct wpa_ctrl *ctrl, char *reply, size_t *reply_len)
	{ return 0; }
int wpa_ctrl_get_fd(struct wpa_ctrl *ctrl) { return 0; }
#endif

static struct wpa_ctrl *ctrl_conn;
static struct wpa_ctrl *monitor_conn;

/* socket pair used to exit from a blocking read */
static int exit_sockets[2];

static char primary_iface[PROPERTY_VALUE_MAX];
// TODO: use new ANDROID_SOCKET mechanism, once support for multiple
// sockets is in

static const char USB_DIR[] = "/sys/bus/usb/devices";
static const char PCI_DIR[] = "/sys/bus/pci/devices";
static const char SDIO_DIR[] = "/sys/bus/sdio/devices";
static const char SDIO_DETECT_NAME[] = "sdio_detect";
static const char SDIO_DETECT_DRIVER[] = "/system/lib/modules/sdio_detect.ko";

static int device_id = WIFI_INVALID_DEVICE;

#ifdef BOARD_WLAN_DEVICE_64BIT
#define MODULE_PATH "/system/lib64/modules/"
#else
#define MODULE_PATH "/system/lib/modules/"
#endif
/* Product ID of supported WiFi devices */
static wifi_device_s devices[] = {
    {WIFI_RALINK_MT7601U, "148f:7601"},
    {WIFI_REALTEK_RTL8188EUS, "0bda:8179"},
    {WIFI_REALTEK_RTL8188ETV, "0bda:0179"},
    {WIFI_REALTEK_RTL8192EU, "0bda:818b"},
    {WIFI_REALTEK_RTL8812AU, "0bda:8812"},
    {WIFI_REALTEK_RTL8723BU, "0bda:b720"},
    {WIFI_MEDIATEK_MT7632U, "0e8d:7632"},
    {WIFI_MEDIATEK_MT7662U, "0e8d:7662"},
    {WIFI_MEDIATEK_MT7612U, "0e8d:7612"},
    {WIFI_REALTEK_RTL8821AU, "0bda:0823"},
    {WIFI_ATHEROS_QCA6174, "168c:003e"},
    {WIFI_REALTEK_RTL8822BU, "0bda:b82c"},
    {WIFI_REALTEK_RTL8822BE, "10ec:b822"},
    {WIFI_HISI_HI1131C, "0296:5347"},
    {WIFI_AMPAK_AP6356S, "02D0:4356"},
    {WIFI_REALTEK_RTL8188FU, "0bda:f179"},
    {WIFI_MEDIATEK_MT7662TE, "14c3:7662"},
    {WIFI_MEDIATEK_MT7662TU, "0e8d:76a0"}
};

#define DRIVER_MODULE_MT7601U_STA    2, \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"mt7601Usta",MODULE_PATH"mt7601Usta.ko","","mt7601Usta "} \
}

#define DRIVER_MODULE_MT7632U    2, \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"mt7662u_sta",MODULE_PATH"mt7662u_sta.ko","","mt7662u_sta "} \
}

#define DRIVER_MODULE_QCA6174    2, \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"qca6174a",MODULE_PATH"qca6174a.ko","","qca6174a "} \
}

#define DRIVER_MODULE_RTL8188EUS    2, \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"rtl8188eu",MODULE_PATH"rtl8188eu.ko","rtw_channel_plan=0x0C ifname=wlan0 if2name=p2p0","rtl8188eu "} \
}

#define DRIVER_MODULE_RTL8192EU    2, \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"rtl8192eu",MODULE_PATH"rtl8192eu.ko","rtw_channel_plan=0x0C ifname=wlan0 if2name=p2p0","rtl8192eu "} \
}

#define DRIVER_MODULE_RTL8812AU    2,  \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"rtl8812au",MODULE_PATH"rtl8812au.ko","rtw_channel_plan=0x0C ifname=wlan0 if2name=p2p0","rtl8812au "} \
}

#define DRIVER_MODULE_RTL8822BU    2,  \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"rtl8822bu",MODULE_PATH"rtl8822bu.ko","rtw_channel_plan=0x0C ifname=wlan0 if2name=p2p0","rtl8822bu "} \
}


#define DRIVER_MODULE_RTL8723BU    2, \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"rtl8723bu",MODULE_PATH"rtl8723bu.ko","ifname=wlan0 if2name=p2p0","rtl8723bu "} \
}

#define DRIVER_MODULE_RTL8821AU    2,  \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"rtl8821au",MODULE_PATH"rtl8821au.ko","ifname=wlan0 if2name=p2p0","rtl8821au "} \
}

#define DRIVER_MODULE_RTL8822BE    2,  \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"rtl8822be",MODULE_PATH"rtl8822be.ko","ifname=wlan0 if2name=p2p0","rtl8822be "} \
}

#define DRIVER_MODULE_HI1131C    3, \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"plat",MODULE_PATH"plat.ko","","plat "}, \
    {"wifi",MODULE_PATH"wifi.ko","","wifi "} \
}

#define DRIVER_MODULE_AP6356S_STA    2, \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"bcmdhd",MODULE_PATH"bcmdhd.ko","firmware_path=/system/etc/firmware/AP6356S/fw_bcm4356a2_ag.bin nvram_path=/system/etc/firmware/AP6356S/nvram_ap6356s.txt ","bcmdhd "} \
}

#define DRIVER_MODULE_AP6356S_AP    2, \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"bcmdhd",MODULE_PATH"bcmdhd.ko","firmware_path=/system/etc/firmware/AP6356S/fw_bcm4356a2_ag_apsta.bin nvram_path=/system/etc/firmware/AP6356S/nvram_ap6356s.txt ","bcmdhd "} \
}

#define DRIVER_MODULE_RTL8188FU   2, \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"rtl8188fu",MODULE_PATH"rtl8188fu.ko","rtw_channel_plan=0x0C ifname=wlan0 if2name=p2p0","rtl8188fu "} \
}

#define DRIVER_MODULE_MT7662TE     2, \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"mt7662e_sta",MODULE_PATH"mt7662e_sta.ko","","mt7662e_sta "} \
}

#define DRIVER_MODULE_MT7662TU   2,  \
{ \
    {"cfg80211",MODULE_PATH"cfg80211.ko","","cfg80211 "}, \
    {"mt7662u_sta",MODULE_PATH"mt7662u_sta.ko","","mt7662u_sta "} \
}

static wifi_modules_s sta_drivers[] = {
    {DRIVER_MODULE_MT7601U_STA},      // MediaTek MT7601U
    {DRIVER_MODULE_RTL8188EUS},       // RealTek RTL8188EUS
    {DRIVER_MODULE_RTL8188EUS},       // RealTek RTL8188ETV
    {DRIVER_MODULE_RTL8192EU},        // RealTek RTL8192EU
    {DRIVER_MODULE_RTL8812AU},        // RealTek RTL8812AU
    {DRIVER_MODULE_RTL8723BU},        // RealTek RTL8723BU
    {DRIVER_MODULE_MT7632U},          // MediaTek MT7632U
    {DRIVER_MODULE_MT7632U},          // MediaTek MT7662U
    {DRIVER_MODULE_MT7632U},          // MediaTek MT7612U
    {DRIVER_MODULE_RTL8821AU},        // RealTek RTL8821AU
    {DRIVER_MODULE_QCA6174},          // Atheros QCA6174
    {DRIVER_MODULE_RTL8822BU},        // RealTek RTL8822BU
    {DRIVER_MODULE_RTL8822BE},        // RealTek RTL8822BE
    {DRIVER_MODULE_HI1131C},          // Hisi Hi1131C
    {DRIVER_MODULE_AP6356S_STA},     // AMPAK AP6356S
    {DRIVER_MODULE_RTL8188FU},       // RealTek RTL8188FTV
    {DRIVER_MODULE_MT7662TE},         //MediaTek MT7662TE
    {DRIVER_MODULE_MT7662TU}          //MediaTek MT7662TU
};
#ifndef WIFI_DRIVER_MODULE_PATH
#define WIFI_DRIVER_MODULE_PATH         ""
#endif

#ifndef WIFI_DRIVER_MODULE_ARG
#define WIFI_DRIVER_MODULE_ARG          ""
#endif
#ifndef WIFI_FIRMWARE_LOADER
#define WIFI_FIRMWARE_LOADER		""
#endif
#define WIFI_TEST_INTERFACE		"wlan0"

#ifndef WIFI_DRIVER_FW_PATH_STA
#define WIFI_DRIVER_FW_PATH_STA		"/system/etc/firmware/AP6356S/fw_bcm4356a2_ag.bin"
#endif
#ifndef WIFI_DRIVER_FW_PATH_AP
#define WIFI_DRIVER_FW_PATH_AP		"/system/etc/firmware/AP6356S/fw_bcm4356a2_ag_apsta.bin"
#endif
#ifndef WIFI_DRIVER_FW_PATH_P2P
#define WIFI_DRIVER_FW_PATH_P2P		NULL
#endif

#ifndef WIFI_DRIVER_FW_PATH_PARAM
#define WIFI_DRIVER_FW_PATH_PARAM	"/sys/module/bcmdhd/parameters/firmware_path"
#endif

#define WIFI_DRIVER_LOADER_DELAY	1000000

static const char IFACE_DIR[]           = "/data/system/wpa_supplicant";
static const char FIRMWARE_LOADER[]     = WIFI_FIRMWARE_LOADER;
static const char DRIVER_PROP_NAME[]    = "wlan.driver.status";
static const char SUPPLICANT_NAME[]     = "wpa_supplicant";
static const char SUPP_PROP_NAME[]      = "init.svc.wpa_supplicant";
static const char P2P_SUPPLICANT_NAME[] = "p2p_supplicant";
static const char P2P_PROP_NAME[]       = "init.svc.p2p_supplicant";

static const char HISI_SUPPLICANT_NAME[]     = "hisi_supplicant";
static const char HISI_SUPP_PROP_NAME[]      = "init.svc.hisi_supplicant";
static const char HISI_P2P_SUPPLICANT_NAME[] = "hisi_p2p_supp";
static const char HISI_P2P_PROP_NAME[]       = "init.svc.hisi_p2p_supp";
static const char SDIO_DETECT_ID[]           = "wlan.sdio.detect.id";

static const char MTK_SUPPLICANT_NAME[] = "mtk_supplicant";
static const char MTK_PROP_NAME[]       = "init.svc.mtk_supplicant";
static const char BCM_SUPPLICANT_NAME[] = "bcm_supplicant";
static const char BCM_PROP_NAME[]       = "init.svc.bcm_supplicant";
static const char SUPP_CONFIG_TEMPLATE[]= "/system/etc/wifi/wpa_supplicant.conf";
static const char SUPP_CONFIG_FILE[]    = "/data/misc/wifi/wpa_supplicant.conf";
static const char P2P_CONFIG_TEMPLATE[] = "/system/etc/wifi/p2p_supplicant.conf";
static const char P2P_CONFIG_FILE[]     = "/data/misc/wifi/p2p_supplicant.conf";
static const char CONTROL_IFACE_PATH[]  = "/data/misc/wifi/sockets";
static const char MODULE_FILE[]         = "/proc/modules";

static const char IFNAME[]              = "IFNAME=";
#define IFNAMELEN			(sizeof(IFNAME) - 1)
static const char WPA_EVENT_IGNORE[]    = "CTRL-EVENT-IGNORE ";

static const char SUPP_ENTROPY_FILE[]   = WIFI_ENTROPY_FILE;
static unsigned char dummy_key[21] = { 0x02, 0x11, 0xbe, 0x33, 0x43, 0x35,
                                       0x68, 0x47, 0x84, 0x99, 0xa9, 0x2b,
                                       0x1c, 0xd3, 0xee, 0xff, 0xf1, 0xe2,
                                       0xf3, 0xf4, 0xf5 };

/* Is either SUPPLICANT_NAME or P2P_SUPPLICANT_NAME */
static char supplicant_name[PROPERTY_VALUE_MAX];
/* Is either SUPP_PROP_NAME or P2P_PROP_NAME */
static char supplicant_prop_name[PROPERTY_KEY_MAX];

static int insmod(const char *filename, const char *args)
{
    void *module;
    unsigned int size;
    int ret;

    module = load_file(filename, &size);
    if (!module)
        return -1;

    ret = init_module(module, size, args);

    free(module);

    return ret;
}

static int rmmod(const char *modname)
{
    int ret = -1;
    int maxtry = 10;

    while (maxtry-- > 0) {
        ret = delete_module(modname, O_NONBLOCK | O_EXCL);
        if (ret < 0 && errno == EAGAIN)
            usleep(500000);
        else
            break;
    }

    if (ret != 0)
        ALOGD("Unable to unload driver module \"%s\": %s\n",
             modname, strerror(errno));
    return ret;
}

int do_dhcp_request(int *ipaddr, int *gateway, int *mask,
                    int *dns1, int *dns2, int *server, int *lease) {
    /* For test driver, always report success */
    if (strcmp(primary_iface, WIFI_TEST_INTERFACE) == 0)
        return 0;

    if (ifc_init() < 0)
        return -1;

    if (do_dhcp(primary_iface) < 0) {
        ifc_close();
        return -1;
    }
    ifc_close();
    get_dhcp_info(ipaddr, gateway, mask, dns1, dns2, server, lease);
    return 0;
}

const char *get_dhcp_error_string() {
    return dhcp_lasterror();
}

#ifdef WIFI_DRIVER_STATE_CTRL_PARAM
int wifi_change_driver_state(const char *state)
{
    int len;
    int fd;
    int ret = 0;

    if (!state)
        return -1;
    fd = TEMP_FAILURE_RETRY(open(WIFI_DRIVER_STATE_CTRL_PARAM, O_WRONLY));
    if (fd < 0) {
        ALOGE("Failed to open driver state control param (%s)", strerror(errno));
        return -1;
    }
    len = strlen(state) + 1;
    if (TEMP_FAILURE_RETRY(write(fd, state, len)) != len) {
        ALOGE("Failed to write driver state control param (%s)", strerror(errno));
        ret = -1;
    }
    close(fd);
    return ret;
}
#endif

int is_wifi_driver_loaded() {
    char driver_status[PROPERTY_VALUE_MAX];
    int i = 0;
#ifdef WIFI_DRIVER_MODULE_PATH
    FILE *proc;
    char line[DRIVER_MODULE_LEN_MAX+10];
#endif

    if(device_id == WIFI_INVALID_DEVICE)
        return 0;

    if (!property_get(DRIVER_PROP_NAME, driver_status, NULL)
            || strcmp(driver_status, "ok") != 0) {
        return 0;  /* driver not loaded */
    }
#ifdef WIFI_DRIVER_MODULE_PATH
    /*
     * If the property says the driver is loaded, check to
     * make sure that the property setting isn't just left
     * over from a previous manual shutdown or a runtime
     * crash.
     */
    if ((proc = fopen(MODULE_FILE, "r")) == NULL) {
        ALOGW("Could not open %s: %s", MODULE_FILE, strerror(errno));
        property_set(DRIVER_PROP_NAME, "unloaded");
        return 0;
    }

    for (i = 0; i < sta_drivers[device_id].module_num; i++) {
        int found = 0;
        while ((fgets(line, sizeof(line), proc)) != NULL) {
            if (strncmp(line, sta_drivers[device_id].modules[i].module_tag, \
                         strlen(sta_drivers[device_id].modules[i].module_tag)) == 0) {
                found = 1;
                break;
            }
        }
        if (0 == found) {
            fclose(proc);
            property_set(DRIVER_PROP_NAME, "unloaded");
            return 0;
        }
        rewind(proc);
    }
    fclose(proc);
    return 1;
#else
    return 1;
#endif
}

int load_prealloc_module(int device)
{
    FILE *proc;
    char line[DRIVER_MODULE_LEN_MAX+10];

    if ((proc = fopen(MODULE_FILE, "r")) == NULL) {
        ALOGW("Could not open %s: %s", MODULE_FILE, strerror(errno));
        return -1;
    }

    while ((fgets(line, sizeof(line), proc)) != NULL) {
        if (WIFI_RALINK_MT7601U == device) {
            if (strncmp(line, "mtprealloc7601Usta ", 19) == 0) {
                fclose(proc);
                return 0;
            }
        } else if (WIFI_MEDIATEK_MT7632U == device || WIFI_MEDIATEK_MT7662U == device
                    || WIFI_MEDIATEK_MT7612U == device || WIFI_MEDIATEK_MT7662TU == device) {
            if (strncmp(line, "mtprealloc76x2 ", 15) == 0) {
                fclose(proc);
                return 0;
            }
        }
    }

    fclose(proc);

    if (WIFI_RALINK_MT7601U == device) {
        if (insmod(MODULE_PATH"mtprealloc7601Usta.ko", "") < 0)
            return -1;
    } else if (WIFI_MEDIATEK_MT7632U == device || WIFI_MEDIATEK_MT7662U == device
                || WIFI_MEDIATEK_MT7612U == device || WIFI_MEDIATEK_MT7662TU == device) {
        if (insmod(MODULE_PATH"mtprealloc76x2.ko", "") < 0)
            return -1;
    }

    return 0;
}

int wifi_load_driver()
{
#ifdef WIFI_DRIVER_MODULE_PATH
    char driver_status[PROPERTY_VALUE_MAX];
    int count = 100; /* wait at most 20 seconds for completion */
    int i = 0;

    device_id = wifi_get_device_id();

    if (WIFI_INVALID_DEVICE == device_id) {
        ALOGE("Cannot find supported device");
        return -1;
    }
    /* MT7601U STA driver and MT7632U needs pre-alloc memory firstly,
     * don't unload it when close WiFi */
    if (WIFI_RALINK_MT7601U == device_id || WIFI_MEDIATEK_MT7632U == device_id
        || WIFI_MEDIATEK_MT7662U == device_id || WIFI_MEDIATEK_MT7612U == device_id
        || WIFI_MEDIATEK_MT7662TU == device_id) {
        if (load_prealloc_module(device_id)) {
            ALOGE("Cannot load prealloc module");
            return -1;
        }
    }

    if (is_wifi_driver_loaded()) {
        return 0;
    }

    for (i = 0; i < sta_drivers[device_id].module_num; i++) {
        if (insmod(sta_drivers[device_id].modules[i].module_path, \
                   sta_drivers[device_id].modules[i].module_arg) < 0)
            return -1;
    }

    if (strcmp(FIRMWARE_LOADER,"") == 0) {
        /* usleep(WIFI_DRIVER_LOADER_DELAY); */
        property_set(DRIVER_PROP_NAME, "ok");
    }
    else {
        property_set("ctl.start", FIRMWARE_LOADER);
    }
    sched_yield();
    while (count-- > 0) {
        if (property_get(DRIVER_PROP_NAME, driver_status, NULL)) {
            if (strcmp(driver_status, "ok") == 0)
                return 0;
            else if (strcmp(driver_status, "failed") == 0) {
                wifi_unload_driver();
                return -1;
            }
        }
        usleep(200000);
    }
    property_set(DRIVER_PROP_NAME, "timeout");
    wifi_unload_driver();
    return -1;
#else
#ifdef WIFI_DRIVER_STATE_CTRL_PARAM
    if (is_wifi_driver_loaded()) {
        return 0;
    }

    if (wifi_change_driver_state(WIFI_DRIVER_STATE_ON) < 0)
        return -1;
#endif
    property_set(DRIVER_PROP_NAME, "ok");
    return 0;
#endif
}

int wifi_unload_driver()
{
    if(device_id == WIFI_INVALID_DEVICE)
        return -1;

    usleep(200000); /* allow to finish interface down */
#ifdef WIFI_DRIVER_MODULE_PATH
    int i = 0;
        int count = 20; /* wait at most 10 seconds for completion */

    for (i = sta_drivers[device_id].module_num-1; i >= 0; i--) {
        if (rmmod(sta_drivers[device_id].modules[i].module_name) == 0)
            continue;
        else
            return -1;
    }
    while (count-- > 0) {
        if (!is_wifi_driver_loaded())
            break;
        usleep(500000);
    }
    usleep(500000); /* allow card removal */
    if (count) {
        return 0;
    }
    return -1;
#else
#ifdef WIFI_DRIVER_STATE_CTRL_PARAM
    if (is_wifi_driver_loaded()) {
        if (wifi_change_driver_state(WIFI_DRIVER_STATE_OFF) < 0)
            return -1;
    }
#endif
    property_set(DRIVER_PROP_NAME, "unloaded");
    return 0;
#endif
}

int ensure_entropy_file_exists()
{
    int ret;
    int destfd;

    ret = access(SUPP_ENTROPY_FILE, R_OK|W_OK);
    if ((ret == 0) || (errno == EACCES)) {
        if ((ret != 0) &&
            (chmod(SUPP_ENTROPY_FILE, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP) != 0)) {
            ALOGE("Cannot set RW to \"%s\": %s", SUPP_ENTROPY_FILE, strerror(errno));
            return -1;
        }
        return 0;
    }
    destfd = TEMP_FAILURE_RETRY(open(SUPP_ENTROPY_FILE, O_CREAT|O_RDWR, 0660));
    if (destfd < 0) {
        ALOGE("Cannot create \"%s\": %s", SUPP_ENTROPY_FILE, strerror(errno));
        return -1;
    }

    if (TEMP_FAILURE_RETRY(write(destfd, dummy_key, sizeof(dummy_key))) != sizeof(dummy_key)) {
        ALOGE("Error writing \"%s\": %s", SUPP_ENTROPY_FILE, strerror(errno));
        close(destfd);
        return -1;
    }
    close(destfd);

    /* chmod is needed because open() didn't set permisions properly */
    if (chmod(SUPP_ENTROPY_FILE, 0660) < 0) {
        ALOGE("Error changing permissions of %s to 0660: %s",
             SUPP_ENTROPY_FILE, strerror(errno));
        unlink(SUPP_ENTROPY_FILE);
        return -1;
    }

    if (chown(SUPP_ENTROPY_FILE, AID_SYSTEM, AID_WIFI) < 0) {
        ALOGE("Error changing group ownership of %s to %d: %s",
             SUPP_ENTROPY_FILE, AID_WIFI, strerror(errno));
        unlink(SUPP_ENTROPY_FILE);
        return -1;
    }
    return 0;
}

int ensure_config_file_exists(const char *config_file)
{
    char buf[2048];
    int srcfd, destfd;
    struct stat sb;
    int nread;
    int ret;

    ret = access(config_file, R_OK|W_OK);
    if ((ret == 0) || (errno == EACCES)) {
        if ((ret != 0) &&
            (chmod(config_file, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP) != 0)) {
            ALOGE("Cannot set RW to \"%s\": %s", config_file, strerror(errno));
            return -1;
        }
        return 0;
    } else if (errno != ENOENT) {
        ALOGE("Cannot access \"%s\": %s", config_file, strerror(errno));
        return -1;
    }

    srcfd = TEMP_FAILURE_RETRY(open(SUPP_CONFIG_TEMPLATE, O_RDONLY));
    if (srcfd < 0) {
        ALOGE("Cannot open \"%s\": %s", SUPP_CONFIG_TEMPLATE, strerror(errno));
        return -1;
    }

    destfd = TEMP_FAILURE_RETRY(open(config_file, O_CREAT|O_RDWR, 0660));
    if (destfd < 0) {
        close(srcfd);
        ALOGE("Cannot create \"%s\": %s", config_file, strerror(errno));
        return -1;
    }

    while ((nread = TEMP_FAILURE_RETRY(read(srcfd, buf, sizeof(buf)))) != 0) {
        if (nread < 0) {
            ALOGE("Error reading \"%s\": %s", SUPP_CONFIG_TEMPLATE, strerror(errno));
            close(srcfd);
            close(destfd);
            unlink(config_file);
            return -1;
        }
        TEMP_FAILURE_RETRY(write(destfd, buf, nread));
    }

    close(destfd);
    close(srcfd);

    /* chmod is needed because open() didn't set permisions properly */
    if (chmod(config_file, 0660) < 0) {
        ALOGE("Error changing permissions of %s to 0660: %s",
             config_file, strerror(errno));
        unlink(config_file);
        return -1;
    }

    if (chown(config_file, AID_SYSTEM, AID_WIFI) < 0) {
        ALOGE("Error changing group ownership of %s to %d: %s",
             config_file, AID_WIFI, strerror(errno));
        unlink(config_file);
        return -1;
    }
    return 0;
}

int wifi_start_supplicant(int p2p_supported)
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 200; /* wait at most 20 seconds for completion */
    const prop_info *pi;
    unsigned serial = 0, i;

    if (p2p_supported) {
        if (WIFI_RALINK_MT7601U == device_id || WIFI_MEDIATEK_MT7632U == device_id
            || WIFI_MEDIATEK_MT7662U == device_id || WIFI_MEDIATEK_MT7612U == device_id) {
            strcpy(supplicant_name, MTK_SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, MTK_PROP_NAME);
        } else if (WIFI_HISI_HI1131C == device_id) {
            strcpy(supplicant_name, HISI_P2P_SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, HISI_P2P_PROP_NAME);
        } else if (WIFI_AMPAK_AP6356S == device_id) {
            strcpy(supplicant_name, BCM_SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, BCM_PROP_NAME);
        } else {
            strcpy(supplicant_name, P2P_SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, P2P_PROP_NAME);
        }

        /* Ensure p2p config file is created */
        if (ensure_config_file_exists(P2P_CONFIG_FILE) < 0) {
            ALOGE("Failed to create a p2p config file");
            return -1;
        }

    } else {
        if (WIFI_HISI_HI1131C == device_id) {
            strcpy(supplicant_name, HISI_SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, HISI_SUPP_PROP_NAME);
        } else {
            strcpy(supplicant_name, SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, SUPP_PROP_NAME);
        }
    }

    /* Check whether already running */
    if (property_get(supplicant_prop_name, supp_status, NULL)
            && strcmp(supp_status, "running") == 0) {
        return 0;
    }

    /* Before starting the daemon, make sure its config file exists */
    if (ensure_config_file_exists(SUPP_CONFIG_FILE) < 0) {
        ALOGE("Wi-Fi will not be enabled");
        return -1;
    }

    if (ensure_entropy_file_exists() < 0) {
        ALOGE("Wi-Fi entropy file was not created");
    }

    /* Clear out any stale socket files that might be left over. */
    wpa_ctrl_cleanup();

    /* Reset sockets used for exiting from hung state */
    exit_sockets[0] = exit_sockets[1] = -1;

    /*
     * Get a reference to the status property, so we can distinguish
     * the case where it goes stopped => running => stopped (i.e.,
     * it start up, but fails right away) from the case in which
     * it starts in the stopped state and never manages to start
     * running at all.
     */
    pi = __system_property_find(supplicant_prop_name);
    if (pi != NULL) {
        serial = __system_property_serial(pi);
    }
    property_get("wifi.interface", primary_iface, WIFI_TEST_INTERFACE);

    property_set("ctl.start", supplicant_name);
    sched_yield();

    while (count-- > 0) {
        if (pi == NULL) {
            pi = __system_property_find(supplicant_prop_name);
        }
        if (pi != NULL) {
            /*
             * property serial updated means that init process is scheduled
             * after we sched_yield, further property status checking is based on this */
            if (__system_property_serial(pi) != serial) {
                __system_property_read(pi, NULL, supp_status);
                if (strcmp(supp_status, "running") == 0) {
                    return 0;
                } else if (strcmp(supp_status, "stopped") == 0) {
                    return -1;
                }
            }
        }
        usleep(100000);
    }
    return -1;
}

int wifi_stop_supplicant(int p2p_supported)
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 50; /* wait at most 5 seconds for completion */

    if (p2p_supported) {
        if (WIFI_RALINK_MT7601U == device_id || WIFI_MEDIATEK_MT7632U == device_id
            || WIFI_MEDIATEK_MT7662U == device_id || WIFI_MEDIATEK_MT7612U == device_id) {
            strcpy(supplicant_name, MTK_SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, MTK_PROP_NAME);
        } else if (WIFI_HISI_HI1131C == device_id) {
            strcpy(supplicant_name, HISI_P2P_SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, HISI_P2P_PROP_NAME);
        } else if (WIFI_AMPAK_AP6356S == device_id) {
            strcpy(supplicant_name, BCM_SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, BCM_PROP_NAME);
        } else {
            strcpy(supplicant_name, P2P_SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, P2P_PROP_NAME);
        }
    } else {
        if (WIFI_HISI_HI1131C == device_id) {
            strcpy(supplicant_name, HISI_SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, HISI_SUPP_PROP_NAME);
        } else {
            strcpy(supplicant_name, SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, SUPP_PROP_NAME);
        }
    }

    /* Check whether supplicant already stopped */
    if (property_get(supplicant_prop_name, supp_status, NULL)
        && strcmp(supp_status, "stopped") == 0) {
        return 0;
    }

    property_set("ctl.stop", supplicant_name);
    sched_yield();

    while (count-- > 0) {
        if (property_get(supplicant_prop_name, supp_status, NULL)) {
            if (strcmp(supp_status, "stopped") == 0)
                return 0;
        }
        usleep(100000);
    }
    ALOGE("Failed to stop supplicant");
    return -1;
}

int wifi_connect_on_socket_path(const char *path)
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};

    /* Make sure supplicant is running */
    if (!property_get(supplicant_prop_name, supp_status, NULL)
            || strcmp(supp_status, "running") != 0) {
        ALOGE("Supplicant not running, cannot connect");
        return -1;
    }

    ctrl_conn = wpa_ctrl_open(path);
    if (ctrl_conn == NULL) {
        ALOGE("Unable to open connection to supplicant on \"%s\": %s",
             path, strerror(errno));
        return -1;
    }
    monitor_conn = wpa_ctrl_open(path);
    if (monitor_conn == NULL) {
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = NULL;
        return -1;
    }
    if (wpa_ctrl_attach(monitor_conn) != 0) {
        wpa_ctrl_close(monitor_conn);
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = monitor_conn = NULL;
        return -1;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, exit_sockets) == -1) {
        wpa_ctrl_close(monitor_conn);
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = monitor_conn = NULL;
        return -1;
    }

    return 0;
}

/* Establishes the control and monitor socket connections on the interface */
int wifi_connect_to_supplicant()
{
    static char path[PATH_MAX];

    if (access(IFACE_DIR, F_OK) == 0) {
        snprintf(path, sizeof(path), "%s/%s", IFACE_DIR, primary_iface);
    } else {
        snprintf(path, sizeof(path), "@android:wpa_%s", primary_iface);
    }
    return wifi_connect_on_socket_path(path);
}

int wifi_send_command(const char *cmd, char *reply, size_t *reply_len)
{
    int ret;
    if (ctrl_conn == NULL) {
        ALOGV("Not connected to wpa_supplicant - \"%s\" command dropped.\n", cmd);
        return -1;
    }
    ret = wpa_ctrl_request(ctrl_conn, cmd, strlen(cmd), reply, reply_len, NULL);
    if (ret == -2) {
        ALOGD("'%s' command timed out.\n", cmd);
        /* unblocks the monitor receive socket for termination */
        TEMP_FAILURE_RETRY(write(exit_sockets[0], "T", 1));
        return -2;
    } else if (ret < 0 || strncmp(reply, "FAIL", 4) == 0) {
        return -1;
    }
    if (strncmp(cmd, "PING", 4) == 0) {
        reply[*reply_len] = '\0';
    }
    return 0;
}

int wifi_supplicant_connection_active()
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};

    if (property_get(supplicant_prop_name, supp_status, NULL)) {
        if (strcmp(supp_status, "stopped") == 0)
            return -1;
    }

    return 0;
}

int wifi_ctrl_recv(char *reply, size_t *reply_len)
{
    int res;
    int ctrlfd = wpa_ctrl_get_fd(monitor_conn);
    struct pollfd rfds[2];

    memset(rfds, 0, 2 * sizeof(struct pollfd));
    rfds[0].fd = ctrlfd;
    rfds[0].events |= POLLIN;
    rfds[1].fd = exit_sockets[1];
    rfds[1].events |= POLLIN;
    do {
        res = TEMP_FAILURE_RETRY(poll(rfds, 2, 30000));
        if (res < 0) {
            ALOGE("Error poll = %d", res);
            return res;
        } else if (res == 0) {
            /* timed out, check if supplicant is active
             * or not ..
             */
            res = wifi_supplicant_connection_active();
            if (res < 0)
                return -2;
        }
    } while (res == 0);

    if (rfds[0].revents & POLLIN) {
        return wpa_ctrl_recv(monitor_conn, reply, reply_len);
    }

    /* it is not rfds[0], then it must be rfts[1] (i.e. the exit socket)
     * or we timed out. In either case, this call has failed ..
     */
    return -2;
}

int wifi_wait_on_socket(char *buf, size_t buflen)
{
    size_t nread = buflen - 1;
    int result;
    char *match, *match2;

    if (monitor_conn == NULL) {
        return snprintf(buf, buflen, "IFNAME=%s %s - connection closed",
                        primary_iface, WPA_EVENT_TERMINATING);
    }

    result = wifi_ctrl_recv(buf, &nread);

    /* Terminate reception on exit socket */
    if (result == -2) {
        return snprintf(buf, buflen, "IFNAME=%s %s - connection closed",
                        primary_iface, WPA_EVENT_TERMINATING);
    }

    if (result < 0) {
        ALOGD("wifi_ctrl_recv failed: %s\n", strerror(errno));
        return snprintf(buf, buflen, "IFNAME=%s %s - recv error",
                        primary_iface, WPA_EVENT_TERMINATING);
    }
    buf[nread] = '\0';
    /* Check for EOF on the socket */
    if (result == 0 && nread == 0) {
        /* Fabricate an event to pass up */
        ALOGD("Received EOF on supplicant socket\n");
        return snprintf(buf, buflen, "IFNAME=%s %s - signal 0 received",
                        primary_iface, WPA_EVENT_TERMINATING);
    }
    /*
     * Events strings are in the format
     *
     *     IFNAME=iface <N>CTRL-EVENT-XXX 
     *        or
     *     <N>CTRL-EVENT-XXX 
     *
     * where N is the message level in numerical form (0=VERBOSE, 1=DEBUG,
     * etc.) and XXX is the event name. The level information is not useful
     * to us, so strip it off.
     */

    if (strncmp(buf, IFNAME, IFNAMELEN) == 0) {
        match = strchr(buf, ' ');
        if (match != NULL) {
            if (match[1] == '<') {
                match2 = strchr(match + 2, '>');
                if (match2 != NULL) {
                    nread -= (match2 - match);
                    memmove(match + 1, match2 + 1, nread - (match - buf) + 1);
                }
            }
        } else {
            return snprintf(buf, buflen, "%s", WPA_EVENT_IGNORE);
        }
    } else if (buf[0] == '<') {
        match = strchr(buf, '>');
        if (match != NULL) {
            nread -= (match + 1 - buf);
            memmove(buf, match + 1, nread + 1);
            ALOGV("supplicant generated event without interface - %s\n", buf);
        }
    } else {
        /* let the event go as is! */
        ALOGW("supplicant generated event without interface and without message level - %s\n", buf);
    }

    return nread;
}

int wifi_wait_for_event(char *buf, size_t buflen)
{
    return wifi_wait_on_socket(buf, buflen);
}

void wifi_close_sockets()
{
    if (ctrl_conn != NULL) {
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = NULL;
    }

    if (monitor_conn != NULL) {
        wpa_ctrl_close(monitor_conn);
        monitor_conn = NULL;
    }

    if (exit_sockets[0] >= 0) {
        close(exit_sockets[0]);
        exit_sockets[0] = -1;
    }

    if (exit_sockets[1] >= 0) {
        close(exit_sockets[1]);
        exit_sockets[1] = -1;
    }
}

void wifi_close_supplicant_connection()
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 50; /* wait at most 5 seconds to ensure init has stopped stupplicant */

    wifi_close_sockets();

    while (count-- > 0) {
        if (property_get(supplicant_prop_name, supp_status, NULL)) {
            if (strcmp(supp_status, "stopped") == 0)
                return;
        }
        usleep(100000);
    }
}

int wifi_command(const char *command, char *reply, size_t *reply_len)
{
    if (NULL != command && !strstr(command, "psk"))
        ALOGD("wifi command = %s", command);
    return wifi_send_command(command, reply, reply_len);
}

const char *wifi_get_fw_path(int fw_type)
{
    switch (fw_type) {
    case WIFI_GET_FW_PATH_STA:
        return WIFI_DRIVER_FW_PATH_STA;
    case WIFI_GET_FW_PATH_AP:
        return WIFI_DRIVER_FW_PATH_AP;
    case WIFI_GET_FW_PATH_P2P:
        return WIFI_DRIVER_FW_PATH_P2P;
    }
    return NULL;
}

int wifi_change_fw_path(const char *fwpath)
{
    int len;
    int fd;
    int ret = 0;

    if (!fwpath)
        return ret;
    fd = TEMP_FAILURE_RETRY(open(WIFI_DRIVER_FW_PATH_PARAM, O_WRONLY));
    if (fd < 0) {
        ALOGE("Failed to open wlan fw path param (%s)", strerror(errno));
        return -1;
    }
    len = strlen(fwpath) + 1;
    if (TEMP_FAILURE_RETRY(write(fd, fwpath, len)) != len) {
        ALOGE("Failed to write wlan fw path param (%s)", strerror(errno));
        ret = -1;
    }
    close(fd);
    return ret;
}

int wifi_get_usb_device_id(void)
{
    int idnum;
    int i = 0;
    int ret = WIFI_INVALID_DEVICE;
    DIR *dir;
    struct dirent *next;
    FILE *fp = NULL;
    idnum = sizeof(devices) / sizeof(devices[0]);
    dir = opendir(USB_DIR);
    if (!dir)
    {
        return WIFI_INVALID_DEVICE;
    }
    while ((next = readdir(dir)) != NULL)
    {
        char line[256];
        char uevent_file[256] = {0};

        /* read uevent file, uevent's data like below:
         * MAJOR=189
         * MINOR=4
         * DEVNAME=bus/usb/001/005
         * DEVTYPE=usb_device
         * DRIVER=usb
         * DEVICE=/proc/bus/usb/001/005
         * PRODUCT=bda/8176/200
         * TYPE=0/0/0
         * BUSNUM=001
         * DEVNUM=005
         */
        sprintf(uevent_file, "%s/%s/uevent", USB_DIR, next->d_name);

        fp = fopen(uevent_file, "r");
        if (NULL == fp)
        {
            continue;
        }
        while (fgets(line, sizeof(line), fp))
        {
            char *pos = NULL;
            int product_vid;
            int product_did;
            int producd_bcddev;
            char temp[10] = {0};
            pos = strstr(line, "PRODUCT=");
            if(pos != NULL)
            {
                if (sscanf(pos + 8, "%x/%x/%x", &product_vid, &product_did, &producd_bcddev)  <= 0)
                    continue;
                sprintf(temp, "%04x:%04x", product_vid, product_did);
                for (i = 0; i < idnum; i++)
                {
                    if (0 == strncmp(temp, devices[i].product_id, 9))
                    {
                        ret = devices[i].id;
                        break;
                    }
                }
            }
            if (ret != WIFI_INVALID_DEVICE)
                break;
        }
        fclose(fp);
        if (ret != WIFI_INVALID_DEVICE)
            break;
    }
    closedir(dir);

    return ret;
}

int wifi_get_pci_device_id(void)
{
    int idnum;
    int i = 0;
    int ret = WIFI_INVALID_DEVICE;
    DIR *dir;
    struct dirent *next;
    FILE *fp = NULL;
    idnum = sizeof(devices) / sizeof(devices[0]);
    dir = opendir(PCI_DIR);
    if (!dir)
    {
        return WIFI_INVALID_DEVICE;
    }
    while ((next = readdir(dir)) != NULL)
    {
        char line[256];
        char uevent_file[256] = {0};

        /* read uevent file, uevent's data like below:
         * MAJOR=189
         * MINOR=4
         * DEVNAME=bus/usb/001/005
         * DEVTYPE=usb_device
         * DRIVER=usb
         * DEVICE=/proc/bus/usb/001/005
         * PRODUCT=bda/8176/200
         * TYPE=0/0/0
         * BUSNUM=001
         * DEVNUM=005
         */
        sprintf(uevent_file, "%s/%s/uevent", PCI_DIR, next->d_name);

        fp = fopen(uevent_file, "r");
        if (NULL == fp)
        {
            continue;
        }
        while (fgets(line, sizeof(line), fp))
        {
            char *pos = NULL;
            int product_vid;
            int product_did;
            int producd_bcddev;
            char temp[10] = {0};
            pos = strstr(line, "PCI_ID=");
            if(pos != NULL) {
                if (sscanf(pos + 7, "%x:%x", &product_vid, &product_did)  <= 0)
                continue;
                sprintf(temp, "%04x:%04x", product_vid, product_did);

                for (i = 0; i < idnum; i++) {
                    if (0 == strncmp(temp, devices[i].product_id, 9)) {
                        ret = devices[i].id;
                        break;
                    }
                }
            }

            if (ret != WIFI_INVALID_DEVICE)
                break;
        }

        fclose(fp);
        if (ret != WIFI_INVALID_DEVICE)
            break;
    }

    closedir(dir);
    return ret;
}

int wifi_get_sdio_device_id(void)
{
    int idnum;
    int i = 0;
    int ret = WIFI_INVALID_DEVICE;
    char sdio_detect_id[PROPERTY_VALUE_MAX] = {'\0'};
    DIR *dir;
    struct dirent *next;
    FILE *fp = NULL;
    char sdio_detect_status[PROPERTY_VALUE_MAX];

    idnum = sizeof(devices) / sizeof(devices[0]);

#if 0
    if (property_get(SDIO_DETECT_ID, sdio_detect_id, NULL) >= 0) {
        if ( 0 == strlen(sdio_detect_id)) {
            if (insmod(SDIO_DETECT_DRIVER, "") < 0) {
                 ALOGE("wifi_get_sdio_device_id,insmod fail,SDIO_DETECT_DRIVER:%s",SDIO_DETECT_DRIVER);
                 return WIFI_INVALID_DEVICE;
            }

            usleep(200000);
        } else {
           ALOGE("wifi_get_sdio_device_id,sdio_detect_id is not null");
        }
    }
#endif

    dir = opendir(SDIO_DIR);
    if (!dir) {
        return WIFI_INVALID_DEVICE;
    }

    while ((next = readdir(dir)) != NULL)
    {
        char line[256];
        char uevent_file[256] = {0};

        /* read uevent file, uevent's data like below:
         *DRIVER=oal_sdio
         *SDIO_CLASS=07
         *SDIO_ID=0296:5347
         *MODALIAS=sdio:c07v0296d5347
         */

        sprintf(uevent_file, "%s/%s/uevent", SDIO_DIR, next->d_name);

        fp = fopen(uevent_file, "r");
        if (NULL == fp) {
            ALOGE("wifi_get_sdio_device_id,fp is null");
            continue;
        }
        while (fgets(line, sizeof(line), fp))
        {
            char *pos = NULL;
            int product_vid;
            int product_did;
            int producd_bcddev;
            char temp[10] = {0};

            pos = strstr(line, "SDIO_ID=");

            if(pos != NULL) {
                if (sscanf(pos + 8, "%x:%x", &product_vid, &product_did)  <= 0) {
                   continue;
                }

                sprintf(temp, "%04x:%04x", product_vid, product_did);

                for (i = 0; i < idnum; i++) {
                    if (0 == strncmp(temp, devices[i].product_id, 9)) {
                        ret = devices[i].id;
                        ALOGE("wifi_get_sdio_device_id,ret:%d",ret);

                        break;
                    }
                }
            }

            if (ret != WIFI_INVALID_DEVICE)
                break;
        }


        fclose(fp);
        if (ret != WIFI_INVALID_DEVICE)
            break;
    }

    if (property_get(SDIO_DETECT_ID, sdio_detect_id, NULL) >= 0) {
        if ( 0 == strlen(sdio_detect_id)) {
            if(ret != WIFI_INVALID_DEVICE) {
                if (rmmod(SDIO_DETECT_NAME) != 0) {
                    ALOGE("wifi_get_sdio_device_id,rmmod fail ,module name:%s",SDIO_DETECT_NAME);
                    ret = WIFI_INVALID_DEVICE;
                }

                sprintf(sdio_detect_id, "%d", devices[i].id);
                property_set(SDIO_DETECT_ID, sdio_detect_id);
            }
        } else {
            if (atoi(sdio_detect_id) >= 0 && atoi(sdio_detect_id) <= idnum) {
                ret = atoi(sdio_detect_id);
            }
        }
    }

    closedir(dir);
    return ret;
}

int ensure_hi1131c_wifi_chip(void)
{
    int ret = WIFI_INVALID_DEVICE;

    ret = wifi_get_sdio_device_id();
    if (WIFI_HISI_HI1131C == ret) {
        ALOGD("the chip is hi1131c wifi chip");
        return 1;
    } else {
        ALOGD("the chip isn't hi1131c wifi chip");
        return 0;
    }
}

int wifi_get_device_id(void)
{
    int ret = WIFI_INVALID_DEVICE;

    ret = wifi_get_pci_device_id();
    if (WIFI_INVALID_DEVICE != ret)
        return ret;

    ret = wifi_get_usb_device_id();
    if (WIFI_INVALID_DEVICE != ret)
        return ret;

    ret = wifi_get_sdio_device_id();
    if (WIFI_INVALID_DEVICE != ret)
        return ret;

#ifdef BOARD_HAVE_AMPAK_AP6356S
    ret = WIFI_AMPAK_AP6356S;
#endif

    return ret;
}
