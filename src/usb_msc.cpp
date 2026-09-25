// usb_msc.cpp -- see usb_msc.h for the full story.

#include "usb_msc.h"
#include "tusb.h"
#include "ff.h"
#include "diskio.h"   // disk_read()/disk_write()/disk_ioctl() -- the SAME
                       // low-level block interface FatFs itself uses
                       // internally to talk to the SD card (via this
                       // project's own no-OS-FatFS-SD-SPI library), reused
                       // directly here rather than a separate, less-proven
                       // path -- see usb_msc.h's own comment.
#include "hpil.h"      // CDevice, DRIVE
#include "drive.h"     // CDrive
#include "usb_serial.h" // LOGF
#include <vector>
#include <cstring>

// The same FATFS work area pico_main.cpp's own f_mount() call at startup
// uses -- declared there (as a plain global, not static, so this extern
// picks up the SAME instance) rather than a second, separate one here,
// which would leave FatFs's own internal state split across two objects
// for what's supposed to be the one and only volume.
extern FATFS fs;
extern std::vector<CDevice*> devices;
// Already used throughout this project (drive.cpp/pico_main.cpp) as the
// signal for "is the SD card usable right now" -- CDrive::check() (see
// drive.cpp) already checks this itself before ever touching FatFs, and
// reports HP-IL's own normal "no media" condition when it's false. This
// is the SAME mechanism initSD() already uses if the SD card fails to
// mount at all -- reusing it here means CDrive doesn't need any special
// awareness of USB MSC mode existing at all, it just sees the SD card
// as (temporarily) not present, something it already handles correctly.
static bool usbMscActive_ = false;

// Set from TinyUSB callbacks (inside tud_task()) when the PC ejects the
// drive or the USB connection goes away; handled later from the main loop
// by usbMscPollHostEject(), which also lets the UI update the menu.
// Leaving MSC mode means remounting the SD card, which is better not done
// in the middle of TinyUSB's own SCSI command handling.
static volatile bool hostEjectPending_ = false;

bool usbMscModeActive() { return usbMscActive_; }

bool enterUsbMscMode() {
    if (usbMscActive_) return true;  // already in this mode -- no-op

    // Signals "no media" to CDrive immediately -- see the SDOK extern's
    // own comment above. Set BEFORE the unmount below so any check()
    // racing in in between (vanishingly unlikely, this project has no
    // real concurrency, but cheap to get the order right regardless)
    // sees it first.
    disableSD();

    // Belt-and-braces for the one case SDOK alone doesn't cover: a drive
    // that was ALREADY open (CTapeSD keeps its own FIL handle open
    // across multiple read/write calls for as long as a drive stays
    // selected -- see tape.h) won't notice SDOK at all until it's next
    // closed and reopened. Closing it explicitly here gives FatFs a
    // chance to flush anything still buffered, rather than silently
    // losing it when f_unmount() below invalidates it outright.
    for (CDevice* dev : devices) {
        if (dev != nullptr && dev->type() == DRIVE) {
            static_cast<CDrive*>(dev)->close();
        }
    }

    const FRESULT fr = f_unmount("");
    if (fr != FR_OK) {
        LOGF("\r\n * USB MSC: f_unmount failed (%d), staying in normal mode", fr);
        enableSD();    // roll back -- the SD card is presumably still
                       // genuinely usable normally if unmount itself
                       // didn't work
        return false;
    }
    usbMscActive_ = true;
    LOGF("\r\n * USB MSC: entered \"Connect to PC\" mode -- SD card now "
         "available as a USB drive, HP-IL drive access paused");
    return true;
}

bool usbMscPollHostEject() {
    if (!hostEjectPending_) return false;
    hostEjectPending_ = false;
    if (!usbMscActive_) return false;
    LOGF("\r\n * USB MSC: ejected by the PC");
    exitUsbMscMode();
    return true;
}

void exitUsbMscMode() {
    if (!usbMscActive_) return;  // already in normal mode -- no-op
    usbMscActive_ = false;
    const FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        LOGF("\r\n * USB MSC: f_mount failed re-entering normal mode (%d)", fr);
        // SDOK stays false here -- matches initSD()'s own convention
        // (SDOK only ever becomes true after a CONFIRMED successful
        // mount), so CDrive keeps correctly reporting "no media" rather
        // than being told the card's fine when it isn't.
    } else {
        enableSD();
        LOGF("\r\n * USB MSC: left \"Connect to PC\" mode -- HP-IL drive "
             "access resumed");
    }
}

// ─── TinyUSB MSC callbacks ──────────────────────────────────────────────
// This LUN's own block size -- fixed at the SD card's own native sector
// size. CFG_TUD_MSC_EP_BUFSIZE in tusb_config.h must be a multiple of it;
// read10/write10 below transfer bufsize / kBlockSize sectors per call.
static constexpr uint16_t kBlockSize = 512;

extern "C" {

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4]) {
    (void)lun;
    const char vid[] = "HP82163";
    const char pid[] = "SD Card";
    const char rev[] = "1.0";
    memset(vendor_id, ' ', 8);
    memset(product_id, ' ', 16);
    memset(product_rev, ' ', 4);
    memcpy(vendor_id, vid, strlen(vid));
    memcpy(product_id, pid, strlen(pid));
    memcpy(product_rev, rev, strlen(rev));
}

// Only ever reports "ready" while explicitly in "Connect to PC" mode --
// see usb_msc.h's own comment for why. The PC still sees the MSC
// interface (and drive letter) the rest of the time, it just can't be
// opened/mounted, exactly like an empty card-reader slot.
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return usbMscActive_;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
    (void)lun;
    *block_size = kBlockSize;
    *block_count = 0;
    if (!usbMscActive_) return;
    LBA_t sectorCount = 0;
    if (disk_ioctl(0, GET_SECTOR_COUNT, &sectorCount) == RES_OK) {
        *block_count = static_cast<uint32_t>(sectorCount);
    }
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return usbMscActive_;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun; (void)power_condition; (void)load_eject;
    // The host "ejecting" the drive is the cleanest possible trigger to
    // leave MSC mode automatically, without the user needing to separately
    // dig back into this project's own menu. Any STOP counts, not only
    // with LoEj set: Linux file managers' "Eject"/"Safely remove" (udisks
    // power-off) send START STOP UNIT with START=0 but LoEj=0, whereas
    // eject(1) and Windows set LoEj=1.
    if (!start && usbMscActive_) {
        hostEjectPending_ = true;
    }
    return true;
}

// USB connection gone (bus reset/unplug) while in MSC mode -- the PC
// can't be using the card any more, so go back to normal mode as well.
void tud_umount_cb(void) {
    if (usbMscActive_) hostEjectPending_ = true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer,
                          uint32_t bufsize) {
    (void)lun;
    if (!usbMscActive_ || offset != 0 || bufsize % kBlockSize != 0) return -1;
    const UINT count = static_cast<UINT>(bufsize / kBlockSize);
    const DRESULT res = disk_read(0, static_cast<BYTE*>(buffer), static_cast<LBA_t>(lba), count);
    return (res == RES_OK) ? static_cast<int32_t>(bufsize) : -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer,
                           uint32_t bufsize) {
    (void)lun;
    if (!usbMscActive_ || offset != 0 || bufsize % kBlockSize != 0) return -1;
    const UINT count = static_cast<UINT>(bufsize / kBlockSize);
    const DRESULT res = disk_write(0, static_cast<const BYTE*>(buffer), static_cast<LBA_t>(lba), count);
    return (res == RES_OK) ? static_cast<int32_t>(bufsize) : -1;
}

// Handles any SCSI command not covered by the specific callbacks above
// (READ_CAPACITY10/READ_FORMAT_CAPACITY/INQUIRY/MODE_SENSE6/REQUEST_SENSE/
// READ10/WRITE10/TEST_UNIT_READY/START_STOP_UNIT/PREVENT_ALLOW_MEDIUM_REMOVAL
// are all already handled by TinyUSB itself using the callbacks above) --
// nothing else is needed for a plain, single-LUN USB drive.
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize) {
    (void)lun; (void)scsi_cmd; (void)buffer; (void)bufsize;
    return -1;  // not supported
}

}  // extern "C"
