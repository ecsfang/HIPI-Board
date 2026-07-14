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

#define printf cdc0_printf
#define RESET           "\e[0m"
#define HILIGHT         "\e[1;92m"       // Green highlight

extern hp82163::Config config;

// Debug help function to show the command and return value of a device
// Also shows the device name, status and address
void show(CDevice* dev, IL_CMD_t cmd = 0, IL_CMD_t rtn = 0)
{
    char buf[32];
    if( !IS_IDLE(cmd) && cmd != NO_FRAME ) {
        printf("%-6.6s --> ", ilMnemonic(cmd, buf));
    } else
        printf("\t   ");
    if( !IS_IDLE(rtn) ) {
        printf("--> %s ", ilMnemonic(rtn, buf));
        if( IS_DATA(cmd) && isprint(rtn) )
            printf(" '%c' ", isprint(rtn) ? rtn : '.');
    }
    dev->show();
    printf("\r\n");
}

static CTape *cassette = NULL;
extern hp82163::UiDialog *dialog;

// Add devices to the HP-IL loop here
void hipi_init()
{
    cassette = new CTapeSD(config.filename().c_str()); // Uses SD-card for file storage

    dialog->setFileSelectedCallback([&cassette](const std::string& filename) {
        printf("\r\nSelect file: " HILIGHT "%s" RESET " ", filename.c_str());
        cassette->select(filename);
        config.setFilename(filename);
    });

    devices.push_back(new CDisplay("TFDISPLAY", 0x3E));
    devices.push_back(new CDrive("TFDRIVE", cassette));
    devices.push_back(new CHipiLed("TFLEDS", 0xEE));
    devices.push_back(new CPilBox("PILBOX"));

}

bool bDebug = false;
bool bTrace = false;

bool hipi_loop(HpIlLoop& loop) {
    uint32_t rx_word;
    static uint32_t lastCmd = NO_FRAME;  // was a local re-initialized every
                                          // call, which defeated the "don't
                                          // reprint the same frame" check below
    char buf[32];
    int n = 0;
    // Check if any HP-IL frame from the PIO interface is available
    if( loop.receiveFrame(rx_word) ) {
        // Got a frame, send to all devices in the loop
        if (bTrace && !IS_IDLE(rx_word) && lastCmd != rx_word) {
            printf("\r\n" HILIGHT "%-6.6s" RESET " ", ilMnemonic(rx_word, buf));
            lastCmd = rx_word;
        }
        for (CDevice* dev : devices) {
            IL_CMD_t rtn = dev->hpil(rx_word);
            if (bDebug && !IS_IDLE(rtn)) {
                show(dev, n ? NO_FRAME : rx_word, rtn);
                n++;
            }
            rx_word = rtn;
            if (bTrace && !IS_IDLE(rx_word)) {
                if( lastCmd != rx_word ) {
                    printf("> " HILIGHT "%-6.6s" RESET " ", ilMnemonic(rx_word, buf));
                    lastCmd = rx_word;
                } else
                    printf("> %-6.6s ", ilMnemonic(rx_word, buf));
            }
        }
        if( bTrace && IS_DATA(rx_word) )
            printf(" '%c' ", isprint(rx_word&0xFF) ? rx_word&0xFF : '.');
        if (bDebug && !IS_IDLE(rx_word)) {
            printf("\t   <<< %s\r\n", ilMnemonic(rx_word, buf));
        }
        // Send the final return value back to the HP-IL loop using the PIO interface
        loop.sendFrame(rx_word);
        return true;    // Handled a frame
    } else {
        // No frame received, call idle() on all devices
        for (CDevice* dev : devices)
            dev->idle();
        return false;   // No frame handled
    }
}