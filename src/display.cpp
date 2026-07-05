#include <stdio.h>
#include <cctype>
#include "display.h"

#include "PicoSpiTransport.hpp"
#include "RA8875.hpp"
#include "Screen.hpp"

extern hp82163::Screen *screen;

void CDisplay::clear(void)
{
    // Clear device 
    screen->clear();
    fifo = std::queue<unsigned char>();
}

void CDisplay::idle(void)
{
    if( !fifo.empty() ) {
        screen->pr_char(fifo.front());
        fifo.pop();
    }
}

// Interface clear
void CDisplay::ifc(void)
{
    status(STAT_IDLE);
}

void CDisplay::doListener(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    *rtn = cmd;
    if( cmd < DOE ) {
        // Data - save in queue ...
        fifo.push(cmd & 0xFF);
    }
}
