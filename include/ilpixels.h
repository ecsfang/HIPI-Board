#ifndef __ILPIXELS_H__
#define __ILPIXELS_H__

#include "hpil.h"
#include "pixels.h"

// Thin HP-IL adapter for the pixel strip -- mirrors illeds.h's CHipiLed
// exactly: doListener() just forwards each incoming data byte to the
// parser (CPixelParser, see pixels.h for the full protocol grammar), all
// the actual pixel/protocol logic lives there.
class CHipiPixel : public CDevice {
public:
    CHipiPixel(const char *name, IL_ADDR_t _sai, IL_ADDR_t _aau=31) : CDevice(name, _sai, _aau, PIXEL) {
    }
    void doListener(IL_CMD_t cmd, IL_CMD_t *rtn);
    void clear(void);
    void ifc(void);
};

#endif//__ILPIXELS_H__
