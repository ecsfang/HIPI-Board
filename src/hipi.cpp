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
    devices.push_back(new CLed("TFLEDS", 0x3D));
    devices.push_back(new CPilBox("PILBOX"));
}

bool bDebug = false;

IL_CMD_t hipi_loop(HpIlLoop& loop) {
    uint32_t rx_word;
    uint32_t rtn;
    char buf[32];
    int n = 0;
    if (loop.receiveFrame(rx_word)) {
        for (CDevice* dev : devices) {
            rtn = dev->hpil(rx_word);
            if (bDebug && !IS_IDLE(rtn)) {
                show(dev, n ? NO_FRAME : rx_word, rtn);
                n++;
            }
            rx_word = rtn;
        }
        if (bDebug && !IS_IDLE(rtn)) {
            printf("\t   >>> %s\r\n", ilMnemonic(rtn, buf));
        }
        loop.sendFrame(rtn);
        return rtn;
    } else {
        // No frame received, call idle() on all devices
        // Also check handshake with PILBox ...
        for (CDevice* dev : devices) {
            dev->idle();
        }
        return 0;
    }
}