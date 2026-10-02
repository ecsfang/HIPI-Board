#include <stdio.h>
#include <ctype.h>

#include "pilbox.h"
#include "boot_service.h"
#include "tusb.h"
#include "plotter.h"
#include "terminal.h"
#include "hipi.h"

#include "display.h"
#include "drive.h"
#include "illeds.h"
#include "ilpixels.h"
#include "iltemp.h"
#include "touch.h"
#include "uidialog.hpp"
#include "analyzer.h"
#include "config.hpp"

#include "usb_serial.h"
#include "Screen.hpp"
#include <cstdarg>
#include <algorithm>

std::vector<CDevice*> devices;
CPilBox* pilbox = nullptr;      // Need to be global for UI indication
CDisplay* videoDisplay = nullptr;  // Same pattern -- needed for pico_main.cpp's
                                     // own periodic fifoSize() diagnostic (see
                                     // display.h's own comment on fifoSize()).
CPlotter* plotter = nullptr;    // Need to be global for plotterview.cpp
CBmp280* bmp280Sensor = nullptr;  // Constructed in hipi_init() rather than as
                                    // a file-scope global (like pixelStrip in
                                    // pixels.cpp) -- its constructor needs
                                    // touch_i2c's own VALUE, and static
                                    // initialization order between two
                                    // different translation units' globals
                                    // isn't guaranteed, so this waits until
                                    // hipi_init() runs (well after all static
                                    // init has completed, and after
                                    // touchInit() has already set up the
                                    // physical bus) instead.

extern hipi::Config config;

bool bTrace = false;
bool bExtTrace = false;

// Debug help function to show the command and return value of a device
// Also shows the device name, status and address
void extendedTrace(CDevice* dev, IL_CMD_t cmd = 0, IL_CMD_t rtn = 0)
{
    char buf[32];
    if( cmd != rtn ) {
        LOGF("-> %s (%X,%X)", ilMnemonic(rtn, buf), cmd, rtn);
        if( IS_DATA(cmd) && isprint(rtn) )
            LOGF(" '%c' ", isprint(rtn) ? rtn : '.');
    }
    dev->show();
    LOGF("\r\n");
}

extern hipi::UiDialog *dialog;
extern hipi::Screen *screen;

// Writes to both the USB debug log and the on-board panel (character by
// character, via Screen::pr_char() -- it already handles CR/LF for line
// breaks the same way a real HP-41/71 display stream would). Used by
// hipi_test()'s device self-check so its results are visible on the
// actual screen at boot, not just over USB.
void logBoth(const char* fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LOGF("%s", buf);
    if (screen) {
        for (const char* p = buf; *p; ++p) {
            screen->pr_char(static_cast<std::uint8_t>(*p));
        }
    }
}

// Number of devices found on the loop
uint8_t hpilDevices = 0;

// Add devices to the HP-IL loop here
static CDrive* driveDev = nullptr;   // TFDRIVE, for its power switch

// Exactly one device -- the last in the loop -- has last() set
static void markLastDevice() {
    for (CDevice* dev : devices) dev->last(false);
    if (!devices.empty()) devices.back()->last(true);
}

// Reorders `devices` by the saved device_order names. Names not found are
// ignored; devices not named keep their default relative order at the end
// (e.g. a device added in a newer version).
static void restoreDeviceOrder() {
    const std::string& csv = config.deviceOrder();
    if (csv.empty()) return;
    std::vector<CDevice*> rest = devices;
    std::vector<CDevice*> ordered;
    std::size_t start = 0;
    while (start <= csv.size()) {
        std::size_t comma = csv.find(',', start);
        if (comma == std::string::npos) comma = csv.size();
        const std::string name = csv.substr(start, comma - start);
        for (auto it = rest.begin(); it != rest.end(); ++it) {
            if (name == (*it)->name()) {
                ordered.push_back(*it);
                rest.erase(it);
                break;
            }
        }
        start = comma + 1;
    }
    ordered.insert(ordered.end(), rest.begin(), rest.end());
    devices = ordered;
}

void hipi_applyDeviceOrder(const std::vector<CDevice*>& order) {
    if (order.size() != devices.size()) return;      // must be the same devices
    devices = order;
    markLastDevice();
    std::string csv;
    for (CDevice* dev : devices) {
        if (!csv.empty()) csv += ",";
        csv += dev->name();
    }
    config.setDeviceOrder(csv);
    LOGF("\r\n * Device order: %s", csv.c_str());
}

// Seen at the last auto-addressing (AAD) passing the PILBox: how many
// devices on the PC side took an address, and the first of them.
// -1 = no AAD seen yet. See hipi_loop().
static int pilboxDevices = -1;
static int pilboxFirstAddr = -1;

void hipi_init()
{
    videoDisplay = new CDisplay("TFDISPLAY", 0x3E);
    devices.push_back(videoDisplay);
    {
        // Each drive has its own cassette file, saved per device name
        // (media.<name>=... in CONFIG.TXT). More drives can be added the
        // same way, with their own names.
        CDrive* drive = new CDrive("TFDRIVE", config.driveMedia("TFDRIVE"));
        drive->setMediaChangedCallback([](CDrive* d) {
            config.setDriveMedia(d->name(), d->mediaFile());
            if (d == hipi::plotterview_drive())
                hipi::plotterview_setTapeFile(d->mediaFile());  // label on the Tape view's cassette
        });
        driveDev = drive;
        devices.push_back(drive);
        hipi::plotterview_setDrive(drive);   // the drive shown in the Tape view
        // The file picker works on this drive (and takes its cassette out
        // while open)
        dialog->setTargetDrive(drive);
    }
    devices.push_back(new CHipiLed("TFLEDS", 0xEE));
    // 0xED -- adjacent to TFLEDS's own 0xEE, both being "indicator"-class
    // accessories; no formal Accessory ID table entry exists for a NeoPixel
    // strip specifically, so this just needs to be distinct from every
    // other device's own SAI, which it is.
    devices.push_back(new CHipiPixel("TFPIXEL", 0xED));

    // 0x51 -- HP-IL Interface Specification, Appendix C.4 "Accessory
    // Identification": class 5x = Electronic Instrumentation, and within
    // it, if bit D3=0 the lower 3 bits are signal-source/signal-switch/
    // signal-measurement flags (D2/D1/D0 respectively) -- 0x51 = D0 only,
    // i.e. a pure measurement device (no source/switch capability),
    // exactly what this is. begin() shares the SD-card style "did it
    // actually come up" check (see initSD()'s own SDOK) -- if it fails
    // (chip not wired up, wrong address, etc.) the device is still
    // registered and answers HP-IL normally, it'll just always report
    // "ERR" for a reading (see CHipiTemp::triggerReading()) rather than
    // silently vanishing from the loop the way a failed SD mount does
    // for CDrive -- there's no other device depending on this one being
    // present the way config-file loading depends on the SD card.
    bmp280Sensor = new CBmp280(touch_i2c);
    if (!bmp280Sensor->begin(TOUCH_SDA, TOUCH_SCL)) {
        LOGF("\r\n * WARNING: BMP280 sensor not responding "
             "(check wiring/address) -- TFTEMP will report ERR");
    }
    devices.push_back(new CHipiTemp("TFTEMP", 0x51, *bmp280Sensor));
    pilbox = new CPilBox("PILBOX");
    devices.push_back(pilbox);
    plotter = new CPlotter("TFPLOT");
    devices.push_back(plotter);
    // SAI 0x3E matches pyILPER's cls_pilterminal exactly (a real, working
    // reference implementation) -- see terminal.h's constructor comment
    // for why that, rather than the formal Accessory ID table's 0x4E
    // "general interface" suggestion we used before.
    devices.push_back(new CTerminal("TFTERM", 0x4E));

    // Saved loop order (Devices -> Change order), then mark the last device
    restoreDeviceOrder();
    markLastDevice();

    // Apply persisted enabled/disabled state (see Config::isDeviceEnabled()
    // / UiDialog's "Devices" menu) now that the actual instances exist --
    // Config only stores names, since it's loaded before any CDevice does.
    for (CDevice* dev : devices) {
        dev->setEnabled(config.isDeviceEnabled(dev->name()));
    }
    // TFDRIVE's OFF-STANDBY-ON switch: Off = disabled above, the config's
    // drive_standby flag tells Standby from On
    if (driveDev != nullptr) {
        const DrivePower onMode = config.driveStandby() ? DrivePower::Standby : DrivePower::On;
        driveDev->initPowerMode(driveDev->enabled() ? onMode : DrivePower::Off, onMode);
    }
}

// See hipi.h. Reports each device's CURRENT address -- the one the
// controller (HP-41) gave it with its own AAU/AAD -- without sending any
// frames. An earlier version ran its own AAU/AAD/TAD/SAI/SDI sequence
// through every device, disabled ones included: disabled devices then
// showed an address, and simply opening the list re-addressed all
// devices behind the controller's back.
std::vector<DeviceInfo> hipi_enumerateDevices() {
    std::vector<DeviceInfo> result;
    for (CDevice* dev : devices) {
        DeviceInfo info{};
        std::snprintf(info.devName, sizeof(info.devName), "%s", dev->name());
        std::snprintf(info.sdiName, sizeof(info.sdiName), "%s", dev->name());
        info.enabled = dev->enabled();
        const int a = static_cast<int>(dev->addr());
        // Only devices on the loop have an address; 31 = unaddressed
        info.addr = (info.enabled && a < 31) ? a : -1;
        info.sai = static_cast<std::uint8_t>(dev->accessoryId());
        info.extDevices = -1;
        if (dev->type() == PILBOX && info.enabled) {
            const IL_CMD_t m = static_cast<CPilBox*>(dev)->mode();
            std::snprintf(info.extMode, sizeof(info.extMode), "%s",
                          m == COFF ? "COFF" : m == COFI ? "COFI" : m == CON ? "CON" : "TDIS");
            if (pilboxDevices >= 0) {
                info.extDevices = pilboxDevices;
                info.extFirstAddr = pilboxFirstAddr;
            }
        }
        result.push_back(info);
    }
    return result;
}

LoopbackResult hipi_loopbackTest(
        HpIlLoop& loop,
        std::function<void(int tested, int total, uint32_t lastCmd)> onProgress) {
    uint32_t rtn  = 0x0000;
    constexpr uint32_t kMinCmd = 0x0001, kMaxCmd = 0x3FF;
    constexpr int kTotal = static_cast<int>(kMaxCmd - kMinCmd + 1);

    int tested = 0;
    // Gates onProgress to roughly once a second rather than every value
    // -- see hipi_loopbackTest()'s own header comment for why.
    absolute_time_t nextProgressReport = make_timeout_time_ms(1000);

    for (uint32_t cmd = kMinCmd; cmd <= kMaxCmd; cmd++) {
        // Drain any stale, unread echo left over from a previous
        // iteration before sending this value -- otherwise an old hit
        // can get picked up here instead of what we're about to send.
        // Re-sending the same cmd repeatedly inside the wait loop below
        // (an earlier version of this test did) produces more than one
        // echo per iteration, only one of which ever gets consumed -- the
        // rest silently carry over and get mistaken for the NEXT
        // iteration's own answer ("always one behind": 0x002 -> 0x001,
        // 0x003 -> 0x002, ...), confirmed on real hardware.
        uint32_t stale;
        while (loop.receiveFrame(stale)) {}

        loop.sendFrame(cmd);   // send exactly once per value
        absolute_time_t cdcTimeout = make_timeout_time_ms(100);
        bool timedOut = false;
        while (!loop.receiveFrame(rtn)) {
            tud_task();
            sleep_ms(10);
            if (time_reached(cdcTimeout)) { timedOut = true; break; }
        }
        ++tested;

        if (timedOut) {
            LOGF("\r\n\t\t* Loopback TIMEOUT waiting for 0x%03X -- stopping", cmd);
            return LoopbackResult{ tested, 1, cmd };
        }
        if (rtn != cmd) {
            LOGF("\r\n\t\t* Loopback failed: 0x%03X -> 0x%03X -- stopping", cmd, rtn);
            return LoopbackResult{ tested, 1, cmd };
        }

        if (onProgress && time_reached(nextProgressReport)) {
            onProgress(tested, kTotal, cmd);
            nextProgressReport = make_timeout_time_ms(1000);
        }
    }
    LOGF("\r\n\t\t* Loopback OK!");
    return LoopbackResult{ tested, 0, 0 };
}

bool hipi_test() {
    // ── Device list at boot ──────────────────────────────────────────
    // Just logs the devices and their current addresses (see
    // hipi_enumerateDevices() -- read-only, no frames are sent). Right
    // after power-up nothing has an address yet; the HP-41 assigns them
    // with its own AAU/AAD.

    // Force the trace to avoid corrupt logging
    bTrace = false;
    bExtTrace = false;

    LOGF("\r\n* Current device list");

    const std::vector<DeviceInfo> infos = hipi_enumerateDevices();
    const int addressedCount = static_cast<int>(std::count_if(
        infos.begin(), infos.end(), [](const DeviceInfo& d) { return d.addr >= 0; }));

    LOGF("\r\n%d device(s) with an address", addressedCount);
    LOGF("\r\nAddr     Name       ID       Enabled"); 
    LOGF("\r\n------------------------------------");

    for (const DeviceInfo& info : infos) {
        if (info.addr < 0) {
            logBoth("\r\n      -: %-10s -- %-10s[%c]",
                info.devName, "", info.enabled ? 'X' : ' ');

            //logBoth("\r\n\t  %6s   %-10s [NOT ADDRESSED]%s",
            //     "--", info.devName, info.enabled ? "" : "  [DISABLED]");
            continue;
        }
        logBoth("\r\naddr %2d: %-10s %02X %-10s[%c]",
             info.addr, info.devName, info.sai & 0xFF, info.sdiName,
             info.enabled ? 'X' : ' ');
        //logBoth("\r\n\t  addr %2d: %-10s SAI=0x%02X  \"%s\"%s",
        //     info.addr, info.devName, info.sai, info.sdiName,
        //     info.enabled ? "" : "  [DISABLED]");
    }
    LOGF("\r\n");
/***
    auto sendAll = [&](uint32_t frame) -> uint32_t {
        for (CDevice* dev : devices) {
            frame = dev->hpil(static_cast<IL_CMD_t>(frame));
        }
        return frame;
    };
 
    sendAll(UNL);
 
    // Auto-address: offer address 1; each device that responds (see
    // CDevice::base()'s AAD handling) takes the next address in sequence
    // and increments the frame by one for the next device, so the final
    // value tells us how many devices answered.
    uint32_t frame = AAD + 1;
    frame = sendAll(frame);
    const int addressedCount = static_cast<int>(frame) - static_cast<int>(AAD + 1);
    LOGF("\r\n%d device(s) with an address", addressedCount);
    LOGF("\r\nAddr     Name       ID       Enabled"); 
    LOGF("\r\n------------------------------------");

    int addr = 1;
    for (CDevice* dev : devices) {
        // Read the address each device actually ended up with, rather
        // than assuming a sequential 1,2,3,... matching its position in
        // the vector -- a device can be transparent during AAD (CPilBox,
        // when nothing's connected on the other side, correctly passes
        // AAD straight through instead of claiming to be a real device
        // it isn't), which shifts every later device's real address down
        // by one relative to its position. 31 is "unaddressed" (see
        // MAX_ADDR / how AAD/LAD/TAD treat it as the broadcast value).
        const IL_CMD_t devAddr = dev->addr();
        if (devAddr >= 31) {
            logBoth("\r\n      -: %-10s -- %-10s[%c]",
                dev->name(), "", dev->enabled() ? 'X' : ' ');
            continue;
        }
 
        // Set the selected device as TALKER
        sendAll(static_cast<uint32_t>(TAD + devAddr));
 
        // Get device ID using SAI command
        const uint32_t sai = sendAll(SAI);
        sendAll(sai);   // confirmation echo, closes out m_sai cleanly
 
        // Get device name using SDI command
        char nameBuf[32];
        std::size_t n = 0;
        uint32_t c = sendAll(SDI);
        while (c != ETO && n < sizeof(nameBuf) - 1) {
            nameBuf[n++] = static_cast<char>(c & 0xFF);
            c = sendAll(0);
        }
        nameBuf[n] = '\0';

        // Add device to log (UART and screen)
        logBoth("\r\naddr %2d: %-10s %02X %-10s[%c]",
             devAddr, dev->name(), sai & 0xFF, nameBuf,
             dev->enabled() ? 'X' : ' ');
    }***/

    LOGF("\r\n------------------------------------");

    // Restore the trace configuration
    bTrace = config.trace();
    bExtTrace = config.extTrace();

    return true;
}

static uint32_t lastCmd = NO_FRAME; // Previous command

// Trace before the frame is sent to the device
inline void preTrace(IL_CMD_t frame) {
    if( bTrace ) {
        if( !IS_ROUTINE(frame) ) {
            char buf[32];
            // Trace if new command and not routine bus housekeeping ...
            LOGF("\r\n@" HILIGHT "%-6.6s" RESET " ", ilMnemonic(frame, buf));
            lastCmd = frame;
        }
    }
}

// Trace of the frame returned from the device
inline void doTrace(CDevice* pDev, IL_CMD_t in, IL_CMD_t out) {
    if( !IS_ROUTINE(out) ) {
        if (bExtTrace ) {
            // Show device status if extended trace
            extendedTrace(pDev, in, out);
        }
        if( bTrace ) {
            char buf[32];
            if( !bExtTrace && pDev->type() == PILBOX )
            LOGF("> pilbox ");  // Show that command is sent to PC as PilBox-command
            // Not the last devices?
            if ( !pDev->isLast() ) {
                // Instruction changed by the device?
                if( lastCmd != out ) {
                    LOGF("> " HILIGHT "%-6.6s" RESET " ", ilMnemonic(out, buf));
                    lastCmd = out;
                } else
                    LOGF("> %-6.6s ", ilMnemonic(out, buf));
            }
        }
    }
}

// Trace after the last device before returning to frame to the loop
inline void postTrace(IL_CMD_t frame) {
    if( bTrace && !IS_ROUTINE(frame) ) {
        char buf[32];
        LOGF("%s %s", bExtTrace ? "<<<" : ">>>", ilMnemonic(frame, buf));
        if( IS_DATA(frame) )
            LOGF(" '%c' ", isprint(frame&0xFF) ? frame&0xFF : '.');
        DBG_LOGF("\r\n");
    }
    if( inAddrRange(frame, AAD) ) {
        hpilDevices = GET_ADDR(frame) - 1;
        DBG_LOGF("\t   <<< %d devices on loop\r\n", hpilDevices);
    }
}

bool hipi_loop(HpIlLoop& loop) {
    uint32_t rx_frame;
    // Check if any HP-IL frame from the PIO interface is available
    if( loop.receiveFrame(rx_frame) ) {
        // Turn on HPIL-active led
        led_on(HPIL_ACT_LED);
        // Got a frame, send to all devices in the loop
        preTrace(rx_frame);
        const IL_CMD_t arrived = static_cast<IL_CMD_t>(rx_frame);   // for the analyzer
        bool absorbed = false;
        for (CDevice* dev : devices) {
            // Check if device is enabled ...
            if( dev->enabled() ) {
                // Let the device handle the frame
                IL_CMD_t rtn = dev->hpil(rx_frame);
                // Devices behind the PILBox (on the PC): during auto-
                // addressing each takes one address, so the AAD frame
                // comes back from the PILBox counted up by their number
                if( dev->type() == PILBOX && inAddrRange(rx_frame, AAD)
                    && inAddrRange(rtn, AAD) ) {
                    pilboxFirstAddr = static_cast<int>(rx_frame - AAD);
                    pilboxDevices   = static_cast<int>(rtn - rx_frame);
                }
                if( rtn == IL_NO_FRAME ) {
                    // Absorbed (see IL_NO_FRAME): the loop is interrupted
                    // here -- later devices don't see it, nothing is sent
                    absorbed = true;
                    break;
                }
                doTrace(dev, rx_frame, rtn);
                rx_frame = rtn;
            } else {
                // Not on the loop -- only notes re-addressing (see offLoopFrame())
                dev->offLoopFrame(rx_frame);
            }
        }
#if HIPI_ANALYZER
        // The analyzer sees what arrived and what leaves (analyzer.h)
        hipi::analyzer_capture(arrived, absorbed ? static_cast<IL_CMD_t>(IL_NO_FRAME)
                                                 : static_cast<IL_CMD_t>(rx_frame));
#else
        (void)arrived;
#endif
        if( absorbed ) {
            TRC_LOGF("\r\n\t   <<< frame absorbed (device powering up)");
        } else {
            postTrace(rx_frame);
            // Send the final frame back to the HP-IL loop using the PIO interface
            loop.sendFrame(rx_frame);
        }
        // Turn off HPIL-active led
        led_off(HPIL_ACT_LED);
        // PILBox commands from the PC (TDIS/COFI/COFF/CON) must be answered
        // even while frames keep coming -- the HP-41 polls with IDY frames
        // non-stop, so idle() below may never run. While translation is off
        // the PC sends nothing but such commands, so reading them here is
        // safe (in translation mode the PC's frames are read in hpil()).
        if (pilbox != nullptr && pilbox->enabled() && !pilbox->isConnected())
            pilbox->idle();
        return true;    // Handled a frame
    } else {
        led_on(HPIL_IDY_LED);
        // No frame received, call idle() on all enabled devices
        for (CDevice* dev : devices) {
            if (dev->enabled())
                dev->idle();
        }
        led_off(HPIL_IDY_LED);
        return false;   // No frame handled
    }
}
// ── Start-up service (boot_service.h) ──────────────────────────────────
static bool bootServiceOn = true;

void hipi_bootService() {
    if (!bootServiceOn) return;
    tud_task();
    if (pilbox == nullptr) {
        pilbox_serviceEarly();              // the device doesn't exist yet
    } else if (pilbox->enabled() && !pilbox->isConnected()) {
        pilbox->idle();                     // commands only, see hipi_loop()
    }
}

void hipi_bootServiceDone() { bootServiceOn = false; }
