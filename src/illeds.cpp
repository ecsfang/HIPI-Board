#include <stdio.h>
#include <cctype>
#include "illeds.h"

#include "PicoSpiTransport.hpp"
#include "RA8875.hpp"
#include "Screen.hpp"

#include "leds.h"

//extern uint8_t LED_PINS[5];
extern CLedParser  parser;

void sendCmd(const char* str) {
    for (const char* p = str; *p; p++)
        parser.feed(*p);
    parser.flush();
}

void CLed::clear(void)
{
    // Clear device 
    sendCmd("0C");
}

// Interface clear
void CLed::ifc(void)
{
    status(STAT_IDLE);
}

void CLed::doListener(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    *rtn = cmd;
    parser.feed(cmd);
}
