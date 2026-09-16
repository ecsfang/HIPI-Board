#include "ilpixels.h"
#include "usb_serial.h"

extern CPixelParser pixelParser;

void CHipiPixel::clear(void)
{
    // Clear device -- all pixels off, matches CHipiLed::clear()'s own
    // "0C" convention exactly.
    if (bTrace) LOGF("\r\n[PIXEL] clear (DCL/SDC)");
    pixelParser.feed('0');
    pixelParser.feed('C');
    pixelParser.flush();
}

// Interface clear
void CHipiPixel::ifc(void)
{
    status(STAT_IDLE);
}

void CHipiPixel::doListener(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    *rtn = cmd;
    if( IS_DATA(cmd) ) {
        LOGF("\r\n[PIXEL] data %02X", cmd);
        pixelParser.feed(cmd & 0xFF);
    }
}
