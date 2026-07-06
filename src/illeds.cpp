#include <stdio.h>
#include <cctype>
#include "illeds.h"

#include "PicoSpiTransport.hpp"
#include "RA8875.hpp"
#include "Screen.hpp"

#include "leds.h"

extern uint8_t LED_PINS[5];

void CLed::clear(void)
{
    // Clear device 
    for( int i=0; i<5; ++i ) {
        led_off(LED_PINS[i]);
    }
}

// Interface clear
void CLed::ifc(void)
{
    status(STAT_IDLE);
}

void CLed::doListener(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    *rtn = cmd;
    if( cmd < DOE && cmd >= 0x20) {
        for( int i=0; i<5; ++i ) {
            if( cmd & (1<<i) )
                led_on(LED_PINS[i]);
            else
                led_off(LED_PINS[i]);
        }
    }
}
