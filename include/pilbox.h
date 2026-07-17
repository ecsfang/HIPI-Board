#ifndef __PILBOX_H__
#define __PILBOX_H__

#include "hpil.h"

class CPilBox : public CDevice {
    unsigned char PIL_rx_lo;             // PILBox lo byte previously received
    unsigned char PIL_tx_lo;             // PILBox lo byte previously sent
    unsigned char PIL_tx_hi;             // PILBox hi byte previously sent
    IL_CMD_t PIL_rx_frame;               // PILBox frame just received
    bool PILmode8 = true;                // PILBox transfer mode, true when in 8-bit mode, false when in 7-bit mode
    IL_CMD_t PILBox_mode = TDIS;         // PILBOx mode (TDIS, CON, COFF, COFI)
    IL_CMD_t pil_recv;                   // PILBox received byte from the serial link
    char pbBuf[32];
    IL_CMD_t m_wLastCmd;
    IL_CMD_t loopbackFrame = NO_FRAME;    // loopback frame when no valid serial link is available
    bool m_hadCmd = false;               // true right after a CMD frame, until the next RFC consumes it (matches the PIC firmware's FCMD flag)
public:
    CPilBox(const char *name) : CDevice(name, 0, 0, NONE) {
    }
    IL_CMD_t hpil(IL_CMD_t cmd);
    void idle(void);
    void clear(void) {}
    void show(void);

    // True once the PC app has actually put us in an active PILBox mode
    // (CON/COFF/COFI) via a PILBox command frame -- TDIS means "not
    // connected", regardless of whether the underlying serial channel
    // itself is open.
    bool isConnected() const { return PILBox_mode != TDIS; }
private:
    IL_CMD_t sendFrame(IL_CMD_t cmd);
    IL_CMD_t receiveFrame(void);
};

// The single CPilBox instance created in hipi_init() (see hipi.cpp) --
// exposed so other modules (e.g. boardui.cpp's PILBOX status LED) can
// query isConnected() without needing to search the `devices` vector.
extern CPilBox* pilbox;

#endif//__PILBOX_H__