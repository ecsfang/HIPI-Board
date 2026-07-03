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

IL_CMD_t CDisplay::hpil(IL_CMD_t cmd)
{
    IL_CMD_t rtn = cmd;

    if( bPrt ) printf("DSP:%3X ", cmd);

    // Handle all base commands
    if( base(cmd, &rtn) )
        return rtn;

    // Otherwise handle device specific commands
    if( cmd == IFC  ) {
        status(STAT_IDLE);
    } else if( (cmd == SAI) && isTalker() ) {
        rtn = nSai;
        sai = true;
    } else if( (cmd == SDI) && isTalker() ) {
        sdi = devName;
        rtn = *sdi++;
    } else if( (cmd < DOE) && isListener() ) {
        // Data
        //printf("DSP:%3X (%c)\n", cmd, isprint(cmd) ? cmd : '.');
        fifo.push(cmd & 0xFF);
    } else {
        if( bPrt ) printf("~");
    }
    return rtn;
}
