#include <stdio.h>
#include <ctype.h>

#include "hpil_pio.hpp"
#include "pilbox.h"

#include "display.h"
#include "drive.h"
#include "illeds.h"

#include "usb_serial.h"

std::vector<CDevice*> devices;

#define printf cdc0_printf

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

// Add devices to the HP-IL loop here
void hipi_init()
{
    //CTape *cassette = new CTapeFlash(); // Uses internal flash for testing without SD-card
    CTape *cassette = new CTapeSD(); // Uses SD-card for file storage
    devices.push_back(new CDisplay("TFDISPLAY", 0x3E));
    devices.push_back(new CDrive("TFDRIVE", cassette));
    devices.push_back(new CHipiLed("TFLEDS", 0xEE));
    devices.push_back(new CPilBox("PILBOX"));
}

bool bDebug = false;

bool hipi_loop(HpIlLoop& loop) {
    uint32_t rx_word;
    char buf[32];
    int n = 0;

    // Check if any HP-IL frame from the PIO interface is available
    if( loop.receiveFrame(rx_word) ) {
        // Got a frame, send to all devices in the loop
        for (CDevice* dev : devices) {
            IL_CMD_t rtn = dev->hpil(rx_word);
            if (bDebug && !IS_IDLE(rtn)) {
                show(dev, n ? NO_FRAME : rx_word, rtn);
                n++;
            }
            rx_word = rtn;
        }
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