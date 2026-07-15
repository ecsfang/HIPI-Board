#include <stdio.h>
#include <cctype>

#include "PicoSpiTransport.hpp"
#include "RA8875.hpp"
#include "Screen.hpp"

#include "leds.h"
#include "illeds.h"

#include "tusb.h"
#include "usb_serial.h"

extern CLedParser  parser;

void sendCmd(const char* str) {
    for (const char* p = str; *p; p++)
        parser.feed(*p);
    parser.flush();
}

void CHipiLed::clear(void)
{
    // Clear device 
    sendCmd("0C");
}

// Interface clear
void CHipiLed::ifc(void)
{
    status(STAT_IDLE);
}

void CHipiLed::doListener(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    *rtn = cmd;
    if( IS_DATA(cmd) ) {
        parser.feed(cmd & 0xFF);
    }
}
