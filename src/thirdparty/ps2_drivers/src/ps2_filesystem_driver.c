/* PS2Build names the main executable target "client", which injects -Dclient.
 * PS2SDK uses 'client' as a struct member name in sifrpc-common.h, so remove
 * that target-name macro before any SDK headers are parsed in vendored code. */
#ifdef client
#undef client
#endif

/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright 2005, ps2dev - http://www.ps2dev.org
# Licenced under GNU Library General Public License version 2
# Review ps2sdk README & LICENSE files for further details.
#
# PS2_FILESYSTEM_DRIVER
*/

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include <kernel.h>

#include <ps2_filesystem_driver.h>
#include <irx_common_macros.h>

EXTERN_IRX(iomanX_irx);
EXTERN_IRX(bdm_irx);
EXTERN_IRX(bdmfs_fatfs_irx);
EXTERN_IRX(usbd_irx);
EXTERN_IRX(usbmass_bd_irx);

extern int32_t __iomanX_id;
extern int __iomanX_ret;
extern int32_t __bdm_id;
extern int __bdm_ret;
extern int32_t __bdmfs_fatfs_id;
extern int __bdmfs_fatfs_ret;
extern int32_t __usbd_id;
extern int __usbd_ret;
extern int32_t __usbmass_bd_id;
extern int __usbmass_bd_ret;

#define DEVICE_SLASH "/"

#define DEVICE_MC0   "mc0:"
#define DEVICE_MC1   "mc1:"
#define DEVICE_CDROM "cdrom0:"
#define DEVICE_CDFS  "cdfs:"
#define DEVICE_MASS  "mass:"
#define DEVICE_MASS0 "mass0:"
#define DEVICE_MASS1 "mass1:"
#define DEVICE_MX4SIO "mx4sio:"
#define DEVICE_MX4SIO0 "mx4sio0:"
#define DEVICE_MX4SIO1 "mx4sio1:"
#define DEVICE_HDD   "hdd:"
#define DEVICE_HDD0  "hdd0:"
#define DEVICE_HOST  "host:"
#define DEVICE_HOST0 "host0:"
#define DEVICE_HOST1 "host1:"

#define DEVICE_MC0_PATH   DEVICE_MC0 DEVICE_SLASH
#define DEVICE_MC1_PATH   DEVICE_MC1 DEVICE_SLASH
#define DEVICE_CDFS_PATH  DEVICE_CDFS DEVICE_SLASH
#define DEVICE_CDROM_PATH DEVICE_CDROM DEVICE_SLASH
#define DEVICE_MASS_PATH  DEVICE_MASS DEVICE_SLASH
#define DEVICE_MASS0_PATH DEVICE_MASS0 DEVICE_SLASH
#define DEVICE_MASS1_PATH DEVICE_MASS1 DEVICE_SLASH
#define DEVICE_MX4SIO_PATH  DEVICE_MX4SIO DEVICE_SLASH
#define DEVICE_MX4SIO0_PATH DEVICE_MX4SIO0 DEVICE_SLASH
#define DEVICE_MX4SIO1_PATH DEVICE_MX4SIO1 DEVICE_SLASH
#define DEVICE_HDD_PATH   DEVICE_HDD DEVICE_SLASH
#define DEVICE_HDD0_PATH  DEVICE_HDD0 DEVICE_SLASH
#define DEVICE_HOST_PATH  DEVICE_HOST DEVICE_SLASH
#define DEVICE_HOST0_PATH DEVICE_HOST0 DEVICE_SLASH
#define DEVICE_HOST1_PATH DEVICE_HOST1 DEVICE_SLASH

#if F___internal_deinit_ps2_filesystem_driver
void __internal_deinit_ps2_filesystem_driver(bool deinit_powerOff) {
    deinit_hdd_driver(false);
    deinit_cdfs_driver();
    init_mx4sio_driver(false);
    deinit_usb_driver(true);
    deinit_memcard_driver(true);
    deinit_fileXio_driver();
    deinit_dev9_driver();

    if (deinit_powerOff)
        deinit_poweroff_driver();
}
#else
void __internal_deinit_ps2_filesystem_driver(bool deinit_powerOff);
#endif

#if F_deinit_ps2_filesystem_driver
void deinit_ps2_filesystem_driver() {
    umount_current_hdd_partition();

    __internal_deinit_ps2_filesystem_driver(true);
}
#endif

#if F___internal_deinit_only_boot_ps2_filesystem_driver
enum BootDeviceIDs __boot_device_id = BOOT_DEVICE_UNKNOWN;

void __internal_deinit_only_boot_ps2_filesystem_driver(bool deinit_powerOff) {
    switch (__boot_device_id) {
        case BOOT_DEVICE_MC0:
        case BOOT_DEVICE_MC1:
            deinit_memcard_driver(true);
            break;
        case BOOT_DEVICE_CDROM:
        case BOOT_DEVICE_CDFS:
            deinit_cdfs_driver();
            break;
        case BOOT_DEVICE_MASS:
        case BOOT_DEVICE_MASS0:
        case BOOT_DEVICE_MASS1:
        case BOOT_DEVICE_MX4SIO:
        case BOOT_DEVICE_MX4SIO0:
        case BOOT_DEVICE_MX4SIO1:
            deinit_usb_driver(true);
            deinit_mx4sio_driver(true);
            break;
        case BOOT_DEVICE_HDD:
        case BOOT_DEVICE_HDD0:
            /* 2004sp: DEV9 is already owned by the live network stack. */
            break;
        case BOOT_DEVICE_HOST:
        case BOOT_DEVICE_HOST0:
        case BOOT_DEVICE_HOST1:
            break;
        default:
            break;
    }

    deinit_fileXio_driver();
}
#else
extern enum BootDeviceIDs __boot_device_id;
void __internal_deinit_only_boot_ps2_filesystem_driver(bool deinit_powerOff);
#endif

#if F_deinit_only_boot_ps2_filesystem_driver
void deinit_only_boot_ps2_filesystem_driver() {
    switch (__boot_device_id) {
        case BOOT_DEVICE_HDD:
        case BOOT_DEVICE_HDD0:
            /* 2004sp: HDD boot restore is intentionally not handled here. */
            break;
        default:
            break;
    }

    __internal_deinit_only_boot_ps2_filesystem_driver(true);
}
#endif

#if F_init_ps2_filesystem_driver
static void poweroffHandler(void *arg) {
    __internal_deinit_ps2_filesystem_driver(false);
    poweroffShutdown();
}

void init_ps2_filesystem_driver() {
    char cwd[FILENAME_MAX];

    init_poweroff_driver();
    init_fileXio_driver();
    init_memcard_driver(true);
    init_usb_driver(true);
    init_mx4sio_driver(false);
    init_cdfs_driver();
    init_dev9_driver();
    init_hdd_driver(false, true);

    poweroffSetCallback(&poweroffHandler, NULL);
    mount_current_hdd_partition();

    getcwd(cwd, sizeof(cwd));
    waitUntilDeviceIsReady(cwd);
}
#endif

#if F_init_only_boot_ps2_filesystem_driver
static bool init_proven_usb_mass_stack(void) {
    int modres = -1;

    /* Exact real-hardware-proven order from the pre-ps2_drivers client:
     * IOMANX -> BDM -> BDMFS_FATFS -> USBD -> USBMASS_BD.
     * No FileXio, SIO2MAN, MX4SIO, MMCEMAN, mouse or keyboard modules here.
     */
    if (__iomanX_id < 0) {
        __iomanX_id = SifExecModuleBuffer(
            iomanX_irx, size_iomanX_irx, 0, NULL, &modres);
        __iomanX_ret = modres;
        if (__iomanX_id < 0 || modres < 0) return false;
    }

    modres = -1;
    if (__bdm_id < 0) {
        __bdm_id = SifExecModuleBuffer(
            bdm_irx, size_bdm_irx, 0, NULL, &modres);
        __bdm_ret = modres;
        if (__bdm_id < 0 || modres < 0) return false;
    }

    modres = -1;
    if (__bdmfs_fatfs_id < 0) {
        __bdmfs_fatfs_id = SifExecModuleBuffer(
            bdmfs_fatfs_irx, size_bdmfs_fatfs_irx, 0, NULL, &modres);
        __bdmfs_fatfs_ret = modres;
        if (__bdmfs_fatfs_id < 0 || modres < 0) return false;
    }

    modres = -1;
    if (__usbd_id < 0) {
        __usbd_id = SifExecModuleBuffer(
            usbd_irx, size_usbd_irx, 0, NULL, &modres);
        __usbd_ret = modres;
        if (__usbd_id < 0 || modres < 0) return false;
    }

    modres = -1;
    if (__usbmass_bd_id < 0) {
        __usbmass_bd_id = SifExecModuleBuffer(
            usbmass_bd_irx, size_usbmass_bd_irx, 0, NULL, &modres);
        __usbmass_bd_ret = modres;
        if (__usbmass_bd_id < 0 || modres < 0) return false;
    }

    return true;
}

void init_only_boot_ps2_filesystem_driver() {
    // get current working directory
    char cwd[FILENAME_MAX];
    getcwd(cwd, sizeof(cwd));

    // get current boot device
    enum BootDeviceIDs boot_device_id = getBootDeviceID(cwd);
    __boot_device_id = boot_device_id;

    // MASS is intentionally not handled by the generic ps2_drivers stack. Real hardware was
    // faster and more reliable with the project's proven BDM sequence, and the generic helper
    // unnecessarily initialized MX4SIO on USB boots.
    if (boot_device_id == BOOT_DEVICE_MASS ||
        boot_device_id == BOOT_DEVICE_MASS0 ||
        boot_device_id == BOOT_DEVICE_MASS1) {
        init_proven_usb_mass_stack();
        waitUntilDeviceIsReady(cwd);
        return;
    }

    // Non-USB devices keep the vendored ps2_drivers path.
    init_fileXio_driver();

    switch (boot_device_id) {
        case BOOT_DEVICE_MC0:
        case BOOT_DEVICE_MC1:
            init_memcard_driver(true);
            break;
        case BOOT_DEVICE_CDROM:
        case BOOT_DEVICE_CDFS:
            init_cdfs_driver();
            break;
        case BOOT_DEVICE_MX4SIO:
        case BOOT_DEVICE_MX4SIO0:
        case BOOT_DEVICE_MX4SIO1:
            init_usb_driver(true);
            init_mx4sio_driver(true);
            break;
        case BOOT_DEVICE_HDD:
        case BOOT_DEVICE_HDD0:
            /* 2004sp: DEV9+SMAP are already active for networking before storage.
             * Do not reload or unload DEV9 here. */
            break;
        case BOOT_DEVICE_HOST:
        case BOOT_DEVICE_HOST0:
        case BOOT_DEVICE_HOST1:
            break;
        default:
            break;
    }

    waitUntilDeviceIsReady(cwd);
}
#endif

#if F_waitUntilDeviceIsReady_ps2_filesystem_driver
/* When booting from a USB device, it is not directly ready
 * so we try to open the folder again until it succeeds.
 */
bool waitUntilDeviceIsReady(char *path) {
    struct stat buffer;
    int ret = -1;
    int retries = 500;

    while (ret != 0 && retries > 0) {
        ret = stat(path, &buffer);
        /* Wait untill the device is ready */
        nopdelay();

        retries--;
    }

    return ret == 0;
}
#endif

#if F_rootDevicePath_ps2_filesystem_driver
char *rootDevicePath(enum BootDeviceIDs device_id) {
    switch (device_id) {
        case BOOT_DEVICE_MC0:
            return DEVICE_MC0_PATH;
        case BOOT_DEVICE_MC1:
            return DEVICE_MC1_PATH;
        case BOOT_DEVICE_CDROM:
            return DEVICE_CDROM_PATH;
        case BOOT_DEVICE_CDFS:
            return DEVICE_CDFS_PATH;
        case BOOT_DEVICE_MASS:
            return DEVICE_MASS_PATH;
        case BOOT_DEVICE_MASS0:
            return DEVICE_MASS0_PATH;
        case BOOT_DEVICE_MASS1:
            return DEVICE_MASS1_PATH;
        case BOOT_DEVICE_MX4SIO:
            return DEVICE_MX4SIO_PATH;
        case BOOT_DEVICE_MX4SIO0:
            return DEVICE_MX4SIO0_PATH;
        case BOOT_DEVICE_MX4SIO1:
            return DEVICE_MX4SIO1_PATH;
        case BOOT_DEVICE_HDD:
            return DEVICE_HDD_PATH;
        case BOOT_DEVICE_HDD0:
            return DEVICE_HDD0_PATH;
        case BOOT_DEVICE_HOST:
            return DEVICE_HOST_PATH;
        case BOOT_DEVICE_HOST0:
            return DEVICE_HOST0_PATH;
        case BOOT_DEVICE_HOST1:
            return DEVICE_HOST1_PATH;
        default:
            return "";
    }
}
#endif

#if F_getBootDeviceID_ps2_filesystem_driver
enum BootDeviceIDs getBootDeviceID(char *path) {
    if (!strncmp(path, DEVICE_MC0, 4))
        return BOOT_DEVICE_MC0;
    else if (!strncmp(path, DEVICE_MC1, 4))
        return BOOT_DEVICE_MC1;
    else if (!strncmp(path, DEVICE_CDROM, 7))
        return BOOT_DEVICE_CDROM;
    else if (!strncmp(path, DEVICE_CDFS, 5))
        return BOOT_DEVICE_CDFS;
    else if (!strncmp(path, DEVICE_MASS, 5))
        return BOOT_DEVICE_MASS;
    else if (!strncmp(path, DEVICE_MASS0, 6))
        return BOOT_DEVICE_MASS0;
    else if (!strncmp(path, DEVICE_MASS1, 6))
        return BOOT_DEVICE_MASS1;
    else if (!strncmp(path, DEVICE_MX4SIO, 5))
        return BOOT_DEVICE_MX4SIO;
    else if (!strncmp(path, DEVICE_MX4SIO0, 6))
        return BOOT_DEVICE_MX4SIO0;
    else if (!strncmp(path, DEVICE_MX4SIO1, 6))
        return BOOT_DEVICE_MX4SIO1;
    else if (!strncmp(path, DEVICE_HDD, 4))
        return BOOT_DEVICE_HDD;
    else if (!strncmp(path, DEVICE_HDD0, 5))
        return BOOT_DEVICE_HDD0;
    else if (!strncmp(path, DEVICE_HOST, 5))
        return BOOT_DEVICE_HOST;
    else if (!strncmp(path, DEVICE_HOST0, 6))
        return BOOT_DEVICE_HOST0;
    else if (!strncmp(path, DEVICE_HOST1, 6))
        return BOOT_DEVICE_HOST1;
    else
        return BOOT_DEVICE_UNKNOWN;
}
#endif
