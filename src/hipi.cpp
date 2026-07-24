#include <stdio.h>
#include <ctype.h>

#include "hpil_pio.hpp"
#include "pilbox.h"
#include "plotter.h"

#include "display.h"
#include "drive.h"
#include "illeds.h"
#include "touch.h"
#include "uidialog.hpp"
#include "config.hpp"

#include "usb_serial.h"

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

    // Apply persisted enabled/disabled state (see Config::isDeviceEnabled()
    // / UiDialog's "Devices" menu) now that the actual instances exist --
    // Config only stores names, since it's loaded before any CDevice does.
    for (CDevice* dev : devices) {
        dev->setEnabled(config.isDeviceEnabled(dev->name()));
    }
}

bool hipi_test(HpIlLoop& loop) {
    uint32_t rx_word    = 0x01BC;
    uint32_t rtn        = 0x0000;
    absolute_time_t cdcTimeout = make_timeout_time_ms(500);

    do {
        tud_task();
        sleep_ms(10);
        loop.sendFrame(rx_word);
    } while (!loop.receiveFrame(rtn) && !time_reached(cdcTimeout));

    LOGF("\r\n\t\t* Loopback ");
    if( rx_word == rtn )
        LOGF("OK! (0x%03X)", rtn);
    else
        LOGF("failed: 0x%03X -> 0x%03X", rx_word, rtn);
    return true;
}

static int nCmd = 0;
bool hipi_loop(HpIlLoop& loop) {
    uint32_t rx_word;
    static uint32_t lastCmd = NO_FRAME;  // was a local re-initialized every
                                          // call, which defeated the "don't
                                          // reprint the same frame" check below
    char buf[32];
    // Check if any HP-IL frame from the PIO interface is available
    if( loop.receiveFrame(rx_word) ) {
        led_on(LED_PIN_2);
        // Got a frame, send to all devices in the loop
        if( bTrace && !IS_ROUTINE(rx_word) ) {
            // Trace if new command and not routine bus housekeeping ...
            LOGF("\r\n@" HILIGHT "%-6.6s" RESET " ", ilMnemonic(rx_word, buf));
            lastCmd = rx_word;
        }
        for (std::size_t i = 0; i < devices.size(); ++i) {
            CDevice* dev = devices[i];
            if (!dev->enabled()) continue;   // as if not physically on the loop
            bool isLast = (i == devices.size() - 1);
            IL_CMD_t rtn = dev->hpil(rx_word);
            if( !IS_ROUTINE(rtn) ) {
                if (bExtTrace ) {
                    // Show device status if extended trace
                    extendedTrace(dev, rx_word, rtn);
                }
                if( bTrace ) {
                    if( !bExtTrace && dev->type() == PILBOX )
                        LOGF("> pilbox ");
                    if (!isLast ) {
                        if( lastCmd != rtn ) {
                            LOGF("> " HILIGHT "%-6.6s" RESET " ", ilMnemonic(rtn, buf));
                            lastCmd = rtn;
                        } else
                        LOGF("> %-6.6s ", ilMnemonic(rtn, buf));
                    }
                }
            }
            rx_word = rtn;
        }
        if( bTrace && !IS_ROUTINE(rx_word) ) {
            LOGF("%s %s", bExtTrace ? "<<<" : ">>>", ilMnemonic(rx_word, buf));
            if( IS_DATA(rx_word) )
                LOGF(" '%c' ", isprint(rx_word&0xFF) ? rx_word&0xFF : '.');
            DBG_LOGF("\r\n");
        }
        // Send the final return value back to the HP-IL loop using the PIO interface
        if( inAddrRange(rx_word, AAD) ) {
            hpilDevices = GET_ADDR(rx_word) - 1;
            DBG_LOGF("\t   <<< %d devices on loop\r\n", hpilDevices);
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