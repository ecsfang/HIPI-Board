#include <stdio.h>
#include <ctype.h>

#include "hpil_pio.hpp"
#include "pilbox.h"

#include "display.h"
#include "drive.h"
#include "illeds.h"
#include "touch.h"
#include "uidialog.hpp"
#include "config.hpp"

#include "usb_serial.h"

std::vector<CDevice*> devices;
CPilBox* pilbox = nullptr;

extern hp82163::Config config;

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

    // Apply persisted enabled/disabled state (see Config::isDeviceEnabled()
    // / UiDialog's "Devices" menu) now that the actual instances exist --
    // Config only stores names, since it's loaded before any CDevice does.
    for (CDevice* dev : devices) {
        dev->setEnabled(config.isDeviceEnabled(dev->name()));
    }
}

bool bDebug = false;
bool bTrace = false;
uint8_t hpilDevices = 0;

bool hipi_loop(HpIlLoop& loop) {
    uint32_t rx_word;
    static uint32_t lastCmd = NO_FRAME;  // was a local re-initialized every
                                          // call, which defeated the "don't
                                          // reprint the same frame" check below
    char buf[32];
    int n = 0;
    // Check if any HP-IL frame from the PIO interface is available
    if( loop.receiveFrame(rx_word) ) {
        led_on(LED_PIN_2);
        // Got a frame, send to all devices in the loop
        if( bTrace && !IS_IDLE(rx_word) ) {
            // Trace if new command and not idle ...
            LOGF("\r\n@" HILIGHT "%-6.6s" RESET " ", ilMnemonic(rx_word, buf));
            lastCmd = rx_word;
        }
        for (std::size_t i = 0; i < devices.size(); ++i) {
            CDevice* dev = devices[i];
            if (!dev->enabled()) continue;   // as if not physically on the loop
            bool isLast = (i == devices.size() - 1);
            IL_CMD_t rtn = dev->hpil(rx_word);
            if( !IS_IDLE(rtn) ) {
                // Show device status if extended trace
                if (bDebug ) {
                    extendedTrace(dev, rx_word, rtn);
                }
                if( bTrace && !bDebug && dev->type() == PILBOX )
                LOGF("> pilbox ");
                if (!isLast && bTrace ) {
                    if( lastCmd != rtn ) {
                        LOGF("> " HILIGHT "%-6.6s" RESET " ", ilMnemonic(rtn, buf));
                        lastCmd = rtn;
                    } else
                    LOGF("> %-6.6s ", ilMnemonic(rtn, buf));
                }
            }
            rx_word = rtn;
        }
        if( bTrace && !IS_IDLE(rx_word) ) {
            LOGF("%s %s", bDebug ? "<<<" : ">>>", ilMnemonic(rx_word, buf));
            if( IS_DATA(rx_word) )
                LOGF(" '%c' ", isprint(rx_word&0xFF) ? rx_word&0xFF : '.');
            if( bDebug )
            LOGF("\r\n");
        }
        // Send the final return value back to the HP-IL loop using the PIO interface
        if( inAddrRange(rx_word, AAD) ) {
            hpilDevices = GET_ADDR(rx_word) - 1;
            if( bDebug )LOGF("\t   <<< %d devices on loop\r\n", hpilDevices);
        }
        led_off(LED_PIN_2);
        loop.sendFrame(rx_word);
        return true;    // Handled a frame
    } else {
        led_on(LED_PIN_3);
        // No frame received, call idle() on all enabled devices
        for (CDevice* dev : devices) {
            if (!dev->enabled()) continue;
            dev->idle();
        }
        led_off(LED_PIN_3);
        return false;   // No frame handled
    }
}