#include <stdio.h>
#include <ctype.h>

#include "hpil_pio.hpp"

#include "display.h"
#include "drive.h"

std::vector<CDevice*> devices;


void show(CDevice* dev, IL_CMD_t cmd = 0, IL_CMD_t rtn = 0)
{
    char buf[32];
    if( cmd != 0) {
        ilMnemonic(cmd, buf);
        printf("%-6.6s --> ", buf);
    } else
        printf("\t   ");
    dev->show();
    if( rtn != 0) {
        ilMnemonic(rtn, buf);
        printf(" -> %s", buf);
        if( rtn>='A' && rtn <= 'Z' )
            printf(" '%c'", isprint(rtn) ? rtn : '.');
    }
    printf("\n");
}

void hipi_init()
{
    devices.push_back(new CDisplay("TFDISPLAY", 0x3E, 31));
    devices.push_back(new CDrive("TFDRIVE"));
}

bool bDebug = true;

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
                //for(int i=0; i<n; i++) printf("\t");
                show(dev, n?0:rx_word, rtn);
                n++;
            }
            rx_word = rtn;
        }
        if (bDebug && rtn != 0x6C0) {
            ilMnemonic(rtn, buf);
            printf("\t   --> %s\n", buf);
        }
        loop.sendFrame(rtn);
        return rtn;
    } else {
        for (CDevice* dev : devices) {
            dev->idle();
        }
        return 0;
    }
}