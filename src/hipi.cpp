#include <stdio.h>
#include <ctype.h>

#include "hpil_pio.hpp"
#include "pilbox.h"
#include "plotter.h"
#include "terminal.h"
#include "hipi.h"

#include "display.h"
#include "drive.h"
#include "illeds.h"
#include "touch.h"
#include "uidialog.hpp"
#include "config.hpp"

#include "usb_serial.h"
#include "Screen.hpp"
#include <cstdarg>
#include <algorithm>

std::vector<CDevice*> devices;
CPilBox* pilbox = nullptr;      // Need to be global for UI indication
CPlotter* plotter = nullptr;    // Need to be global for plotterview.cpp

extern hp82163::Config config;

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

static CTape *cassette = NULL;
extern hp82163::UiDialog *dialog;
extern hp82163::Screen *screen;

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
void hipi_init()
{
    cassette = new CTapeSD(config.filename().c_str()); // Uses SD-card for file storage

    dialog->setFileSelectedCallback([&cassette](const std::string& filename) {
        LOGF("\r\nSelect file: " HILIGHT "%s" RESET " ", filename.c_str());
        cassette->select(filename);
        config.setFilename(filename);
    });

    devices.push_back(new CDisplay("TFDISPLAY", 0x3E));
    devices.push_back(new CDrive("TFDRIVE", cassette));
    devices.push_back(new CHipiLed("TFLEDS", 0xEE));
    pilbox = new CPilBox("PILBOX");
    devices.push_back(pilbox);
    plotter = new CPlotter("TFPLOT");
    devices.push_back(plotter);
    // SAI 0x3E matches pyILPER's cls_pilterminal exactly (a real, working
    // reference implementation) -- see terminal.h's constructor comment
    // for why that, rather than the formal Accessory ID table's 0x4E
    // "general interface" suggestion we used before.
    devices.push_back(new CTerminal("TFTERM", 0x4E));

    // Mark the last device in the loop
    devices.back()->last(true);

    // Apply persisted enabled/disabled state (see Config::isDeviceEnabled()
    // / UiDialog's "Devices" menu) now that the actual instances exist --
    // Config only stores names, since it's loaded before any CDevice does.
    for (CDevice* dev : devices) {
        dev->setEnabled(config.isDeviceEnabled(dev->name()));
    }
}

// See hipi.h for the full rationale (shared by hipi_test()'s boot-time
// log and boardui.cpp's on-demand "Devices" dialog).
std::vector<DeviceInfo> hipi_enumerateDevices() {
    std::vector<DeviceInfo> result;
 
    auto sendAll = [](uint32_t frame) -> uint32_t {
        for (CDevice* dev : devices) {
            frame = dev->hpil(static_cast<IL_CMD_t>(frame));
        }
        return frame;
    };
 
    sendAll(UNL);
    sendAll(RFC);
    sendAll(AAU);
    sendAll(RFC);
    sendAll(UNT);
    sendAll(RFC);
 
    // Discover the device count purely from the AAD response -- NOT from
    // the size of our own `devices` vector. This is the part an earlier
    // version of this function got wrong: it looped over `devices` and
    // read dev->addr()/name()/enabled() directly, which only ever shows
    // OUR OWN objects. A real controller has no such list to cheat
    // with -- it only knows what AAD/TAD/SAI/SDI themselves reveal, and
    // that's the only thing that would also correctly surface a genuine
    // external device reachable through CPilBox (e.g. something pyILPER
    // presents on the other side), which has no corresponding CDevice of
    // ours at all.
    const uint32_t afterAad = sendAll(AAD + 1);
    const int deviceCount = static_cast<int>(afterAad) - static_cast<int>(AAD + 1);
 
    for (int addr = 1; addr <= deviceCount; ++addr) {
        DeviceInfo info{};
        info.addr = addr;
        info.devName[0] = '\0';
        info.enabled = true;   // default for anything that isn't one of our own -- see the cross-reference pass below
 
        // RFC ("ready for command") between each step -- gives the
        // addressed device a moment to act before the next command
        // arrives, the same pacing a real controller would use, not just
        // an unbroken burst of frames.
        sendAll(static_cast<uint32_t>(TAD + addr));
        sendAll(RFC);
 
        const uint32_t sai = sendAll(SAI);
        sendAll(sai);   // confirmation echo, closes out m_sai cleanly
        sendAll(RFC);
        info.sai = static_cast<std::uint8_t>(sai & 0xFF);
 
        std::size_t n = 0;
        uint32_t c = sendAll(SDI);
        while (c != ETO && n < sizeof(info.sdiName) - 1) {
            info.sdiName[n++] = static_cast<char>(c & 0xFF);
            // No RFC here -- unlike TAD/SAI/UNT (distinct commands), this
            // is a continuous data stream: base()'s m_sdi residual check
            // advances on ANY incoming frame regardless of its value, so
            // an RFC here consumes a character exactly like the real
            // continuation read does. A real test proved it: inserting
            // one here garbled every name ("TFDISPLAY" -> "TDSLY", every
            // other character silently skipped).
            c = sendAll(0);
        }
        info.sdiName[n] = '\0';
 
        sendAll(UNT);
        sendAll(RFC);
 
        result.push_back(info);
    }
 
    // Cross-reference our own devices vector purely for display purposes
    // (a local name + enabled/disabled status) -- this never feeds back
    // into the address/SAI/SDI values above, which came entirely from
    // the protocol exchange itself. A genuine external device (no match
    // here) just keeps the defaults set above (blank name, enabled=true,
    // i.e. "not applicable").
    for (DeviceInfo& info : result) {
        for (CDevice* dev : devices) {
            if (dev->addr() == info.addr) {
                std::snprintf(info.devName, sizeof(info.devName), "%s", dev->name());
                info.enabled = dev->enabled();
                break;
            }
        }
    }
 
    // Also list any of our OWN devices that didn't take an address at
    // all (addr() left at 31, "unaddressed") -- e.g. CPilBox when
    // nothing's connected on the other side, correctly staying
    // transparent during AAD rather than claiming to be a device it
    // isn't. These aren't part of the protocol-driven loop above (there's
    // no address to query them at), but they're still worth surfacing so
    // the list doesn't just silently omit a disconnected/disabled local
    // device.
    for (CDevice* dev : devices) {
        if (dev->addr() >= 31) {
            DeviceInfo info{};
            info.addr = -1;
            std::snprintf(info.devName, sizeof(info.devName), "%s", dev->name());
            info.enabled = dev->enabled();
            info.sai = 0;
            info.sdiName[0] = '\0';
            result.push_back(info);
        }
    }
 
    return result;
}

bool hipi_test(HpIlLoop& loop) {
    uint32_t rx_frame   = 0x01BC;
    uint32_t rtn        = 0x0000;
    absolute_time_t cdcTimeout = make_timeout_time_ms(500);

    do {
        tud_task();
        sleep_ms(10);
        loop.sendFrame(rx_frame);
    } while (!loop.receiveFrame(rtn) && !time_reached(cdcTimeout));

    LOGF("\r\n\t\t* Loopback ");
    if( rx_frame == rtn )
        LOGF("OK! (0x%03X)", rtn);
    else
        LOGF("failed: 0x%03X -> 0x%03X", rx_frame, rtn);

    // ── Device self-check ────────────────────────────────────────────
    // Deliberately does NOT touch the physical loop at all (no
    // loop.sendFrame()/receiveFrame()) -- we don't know whether a real
    // controller or other physical devices are present, so all we can
    // safely exercise this way is our own built-in devices (and, via
    // CPilBox's own hpil(), whatever it can reach over USB if something's
    // actually connected there). Every frame is instead driven directly
    // through dev->hpil(), chained across the devices vector in order --
    // the exact same dispatch hipi_loop() uses for real bus frames, just
    // sourced by us instead of the physical PIO loop. This mirrors what a
    // real controller's own AAD/TAD/SAI/SDI sequence looks like: offer
    // AAD, see how many devices take an address, then address each one
    // in turn to read its identity (SAI) and name (SDI). A real
    // controller connecting afterward starts its own IFC/AAU/AAD cycle
    // regardless, so this leaves nothing lasting behind to worry about.
    //
    // enabled()/disabled() is purely a *runtime* gate hipi_loop() uses to
    // skip a device on the real bus (simulating it being unplugged) --
    // every device is still a normal, fully-functional CDevice underneath
    // and answers hpil() exactly the same either way, so the self-check
    // queries all of them identically and just notes which ones are
    // currently disabled alongside the result, rather than skipping them.

    // Force the trace to avoid corrupt logging
    bTrace = false;
    bExtTrace = false;

    LOGF("\r\n* Current device list");

    const std::vector<DeviceInfo> infos = hipi_enumerateDevices();
    const int addressedCount = static_cast<int>(std::count_if(
        infos.begin(), infos.end(), [](const DeviceInfo& d) { return d.addr >= 0; }));

    LOGF("\r\n%d device(s) responded to AAD", addressedCount);
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
    LOGF("\r\n%d device(s) responded to AAD", addressedCount);
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
        for (CDevice* dev : devices) {
            // Check if device is enabled ...
            if( dev->enabled() ) {
                // Let the device handle the frame
                IL_CMD_t rtn = dev->hpil(rx_frame);
                doTrace(dev, rx_frame, rtn);
                rx_frame = rtn;
            }
        }
        postTrace(rx_frame);
        // Send the final frame back to the HP-IL loop using the PIO interface
        loop.sendFrame(rx_frame);
        // Turn off HPIL-active led
        led_off(HPIL_ACT_LED);
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