#include <stdio.h>
#include <ctype.h>

#include "hpil_pio.hpp"
#include "pilbox.h"

#include "display.h"
#include "drive.h"

#include "usb_serial.h"

std::vector<CDevice*> devices;

#define printf cdc0_printf

void show(CDevice* dev, IL_CMD_t cmd = 0, IL_CMD_t rtn = 0)
{
    char buf[32];
    if( cmd != 0x6C0) {
        ilMnemonic(cmd, buf);
        printf("%-6.6s --> ", buf);
    } else
        printf("\t   ");
    if( rtn != 0x6C0) {
        ilMnemonic(rtn, buf);
        printf("--> %s ", buf);
        if( rtn < DOE && isprint(rtn) )
            printf(" '%c' ", isprint(rtn) ? rtn : '.');
    }
    dev->show();
    printf("\r\n");
}

// Add devices to the HP-IL loop here
void hipi_init()
{
    CTape *cassette = new CTapeSD();
    devices.push_back(new CDisplay("TFDISPLAY", 0x3E, 31));
    devices.push_back(new CDrive(cassette, "TFDRIVE"));
}

bool bDebug = false;

#define PILBOX
IL_CMD_t hipi_loop(HpIlLoop& loop) {
    uint32_t rx_word;
    uint32_t rtn;
    char buf[32];
    int n = 0;
    if (loop.receiveFrame(rtn)) {
        rx_word = rtn;
        for (CDevice* dev : devices) {
            rtn = dev->hpil(rx_word);
            if (bDebug && rtn != 0x6C0) {
                show(dev, n?0x6C0:rx_word, rtn);
                n++;
            }
            rx_word = rtn;
        }
#ifdef PILBOX
        rtn = ILBOX_SendFrame(rtn);
#endif
        if (bDebug && rtn != 0x6C0) {
            ilMnemonic(rtn, buf);
            printf("\t   >>> %s\r\n", buf);
        }
        loop.sendFrame(rtn);
        return rtn;
    } else {
        // No frame received, call idle() on all devices
        // Check handshake with PILBox ...
#ifdef PILBOX
        ILBOX_ReceiveFrame();
#endif
        for (CDevice* dev : devices) {
            dev->idle();
        }
        return 0;
    }
}