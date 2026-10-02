#define MODULE "PILBOX"

#include <stdio.h>
#include <ctype.h>

#include "display_config.h"
#include "hpil.h"
#include "pilbox.h"
#include "tusb.h" 
#include "usb_serial.h"
#include "ui_buttons.hpp"
#include "pico/time.h"

extern hipi::DisplayDriver* display;

/*
 * an improved format is using full 8-bit bytes:
 * 001c ccb0 high byte: higher 4 bits (3 control bits plus data bit 7)
 * 1bbb bbbb low byte: lo
 * When a frame has to be transmited to the serial link, the driver will check if the high byte (control bits and
 * data bit 7) is the same than the previously transmited frame. If yes, it is not transmited again and only the
 * low byte is sent.
 * Conversely, if the receiver gets only the low byte, it will use the last received high byte to rebuild the full
 * frame.
 */
#define P_DEBUG bExtTrace
#define PD_LOGF(...) do { if (P_DEBUG) LOGF(__VA_ARGS__); } while (0)
#define PD_MLOGF(...) do { if (P_DEBUG) MLOGF(__VA_ARGS__); } while (0)
#define PD_IDY_LOGF(frm,...) do {           \
        if( P_DEBUG && !IS_IDLE(frm) ) {    \
            LOGF(__VA_ARGS__);              \
        }                                   \
    } while(0)

#define PL_LOG_MODE(x) PD_LOGF("\r\nPILBOX: " #x )

// Send a single byte to the PILBox serial link and flush
#define PL_SEND(x) do {                     \
        tud_cdc_n_write_char(ITF_HPIL, x);  \
        tud_cdc_n_write_flush(ITF_HPIL);    \
        PD_MLOGF("--> %02X", x);   \
    } while(0)

// Send a complete frame to the PILBox serial link, using the 2-byte format and flush
#define PL_SEND_FRAME() do {                                    \
        tud_cdc_n_write(ITF_HPIL, &PIL_tx_hi,1);                \
        tud_cdc_n_write(ITF_HPIL, &PIL_tx_lo,1);                \
        tud_cdc_n_write_flush(ITF_HPIL);                        \
        PD_MLOGF("--> %02X:%02X", PIL_tx_hi, PIL_tx_lo); \
    } while(0)


// ── Start-up: PILBox commands before the device exists ───────────────
namespace {
IL_CMD_t earlyMode = TDIS;          // last mode command the PC sent
bool earlyConnected = false;        // the PC had the port open
std::uint8_t earlyHi = 0;
bool earlyDone = false;             // CPilBox exists: it takes over
bool earlyEnabled = true;           // PILBOX not switched off (Devices)
}

void pilbox_setEarlyEnabled(bool enabled)
{
    earlyEnabled = enabled;
}

void pilbox_serviceEarly(void)
{
    // Switched off: no PIL-Box -- the PC gets no answers, as with none plugged in
    if (earlyDone || !earlyEnabled || !tud_cdc_n_connected(ITF_HPIL)) return;
    earlyConnected = true;
    while (tud_cdc_n_available(ITF_HPIL) > 0) {
        const std::uint8_t b = static_cast<std::uint8_t>(tud_cdc_n_read_char(ITF_HPIL));
        if ((b & 0xE0) == 0x20) { earlyHi = b; continue; }      // high byte
        if (!(b & 0x80)) continue;                               // only 8-bit low bytes
        const IL_CMD_t frame = static_cast<IL_CMD_t>(((earlyHi & 0x1E) << 6) | (b & 0x7F));
        if (frame == TDIS || frame == CON || frame == COFF || frame == COFI) {
            earlyMode = frame;
            tud_cdc_n_write_char(ITF_HPIL, b);                   // acknowledge, like a PIL-Box
            tud_cdc_n_write_flush(ITF_HPIL);
            MLOGF("start-up: PC command %03X answered", frame);
        }
        // Anything else: no HP-IL yet -- dropped
    }
}

CPilBox::CPilBox(const char *name) : CDevice(name, 0, 0, PILBOX)
{
    earlyDone = true;
    PILBox_mode = earlyMode;
    lastConnected_ = earlyConnected && tud_cdc_n_connected(ITF_HPIL);
}

IL_CMD_t CPilBox::hpil(IL_CMD_t cmd)
{
    IL_CMD_t pil_cmd = NO_FRAME;
    // Got a frame from the HPIL bus
    // in this case the frame is received from the PILBox emulator
    // modelled after functions in V41 (Christoph Giesselink)
    // for the HP-IL Scope
    //HPIL_scope(wFrame, false, true);

    // if the PILBox is in TDIS mode, just return the frame
    if( PILBox_mode == TDIS )
        return cmd;

    // CMD frame and CA (controller active) and adding RFC frame enabled
    // CMD/RFC handshaking is done in the PILBox emulation
    if (((cmd & CMD_MASK) == CMD) ) {
        m_wLastCmd = cmd;    // remember last CMD frame to send later when RFC is received
        m_hadCmd = true;
        PD_LOGF("\t   <== %s (skip)\r\n", ilMnemonic(m_wLastCmd, pbBuf));
        // Return without sending the CMD frame to PyIlPer
        return cmd;
    }

    // CA (controller active) and RFC frame
    // CMD/RFC handshaking done in the PILBox emulation
    //
    // The PIC firmware only translates/forwards RFC to the PC if a CMD
    // frame actually just preceded it (FCMD flag) -- otherwise, unless
    // we're the bus controller (PILBox_mode == CON, never used in this
    // device-only setup), it just retransmits RFC unchanged and never
    // touches the PC link at all. Forwarding+waiting unconditionally (as
    // this used to) waits for a PC reply that was never coming for any
    // "bare" RFC not tied to a pending command.
    if ((cmd == RFC)) {
        if (!m_hadCmd && PILBox_mode != CON) {
            PD_LOGF("\t   <== RFC (bare)\r\n");
            return cmd;
        }        
        PD_LOGF("pilbox: RFC\r\n");
        cmd = m_wLastCmd;                            // use the last CMD frame as answer
        m_hadCmd = false;   // consumed
        sendFrame(cmd);                            // send the RFC frame
        // Waits until the PC answers. NOTE: unbounded -- a 500 ms timeout
        // was tried here but is disabled; if the PC side never answers
        // (dropped byte, app not ready), hipi_loop() doesn't return.
        do {
            tud_task();  // TinyUSB background task
            pil_cmd = receiveFrame();
        } while( pil_cmd == NO_FRAME );
        PD_LOGF("\t   <== RFC!\r\n");
        if( pil_cmd == NO_FRAME ) LOGF("\t   <== Timeout!!!!!!!!!!!!\r\n");
        return RFC;
    }

    // IDY frames circulate constantly on an idle bus. The PIC firmware
    // only forwards them to the PC when PILBox_mode == COFI (TRIDY set)
    // or CON (controller); in COFF mode they're handled purely locally
    // (retransmitted unchanged -- SRQ-bit patching isn't implemented
    // here yet). Falling through to the generic "forward everything"
    // path below would otherwise send+wait for a PC reply to routine
    // bus polling traffic the PC was never going to answer.
    if ( IS_IDLE(cmd) && PILBox_mode == COFF) {
        return cmd;
    }

    // Send all other frames to PyIlPer
    sendFrame(cmd);

    do {
        tud_task();  // TinyUSB background task
        pil_cmd = receiveFrame();
    } while( pil_cmd == NO_FRAME );
    PD_IDY_LOGF(pil_cmd, "\t   <== %s\r\n", ilMnemonic(pil_cmd, pbBuf));

    // return the received frame
    return pil_cmd;
}

// The PC app may have sent TDIS (0x32 0x94) several times while waiting
// for the PILBox to come up -- on the first TDIS, read away any further
// 0x32 0x94 pairs already queued, so it's answered with a single 0x94.
// Anything else stays for normal processing (a byte read ahead is put
// back in pendingRx_).
void CPilBox::drainTdisBacklog(void)
{
    int drained = 0;
    while (tud_cdc_n_available(ITF_HPIL) > 0) {
        uint8_t next = 0;
        if (!tud_cdc_n_peek(ITF_HPIL, &next) || next != 0x32) break;
        tud_cdc_n_read_char(ITF_HPIL);                 // the 0x32
        if (tud_cdc_n_available(ITF_HPIL) == 0) {      // lo byte not here yet
            PIL_tx_hi = 0x32;                          // keep it as a hi byte
            break;
        }
        const int lo = tud_cdc_n_read_char(ITF_HPIL);
        if (lo != 0x94) {                              // another frame: keep it
            PIL_tx_hi = 0x32;
            pendingRx_ = lo;
            break;
        }
        ++drained;
    }
    if (drained > 0) {
        MLOGF("TDIS backlog: %d repeated TDIS dropped, answered once", drained);
    }
}

// The PC side (pyILPER) opened or closed the port: start clean --
// translation off until the PC sends its commands (like a real PIL-Box
// after power-up), nothing old left to send to the PC (frames forwarded to
// a closed port), and the TDIS backlog handling armed again.
// What the PC sent is NOT thrown away on connect: its first TDIS may well
// be here already when the connection is noticed. (On disconnect, unread
// bytes from the PC are dropped.)
void CPilBox::checkConnection(void)
{
    const bool connected = tud_cdc_n_connected(ITF_HPIL);
    if (connected == lastConnected_) return;
    lastConnected_ = connected;
    if (!connected) tud_cdc_n_read_flush(ITF_HPIL);
    tud_cdc_n_write_clear(ITF_HPIL);
    PILBox_mode = TDIS;
    PIL_tx_hi = 0;
    pendingRx_ = -1;
    tdisBacklogDrained_ = false;
    MLOGF("PC %s -- link reset (TDIS)", connected ? "connected" : "disconnected");
}

IL_CMD_t CPilBox::receiveFrame(void)
{
    IL_CMD_t frame;
    checkConnection();
    if (!tud_cdc_n_connected(ITF_HPIL))
    {
        // no valid serial link, loopback mode 
        frame = loopbackFrame;                  // return the last frame sent    
        loopbackFrame = NO_FRAME;                 // reset the loopback frame
        return frame;                      // return no data and get out
    }
    else if (pendingRx_ < 0 && tud_cdc_n_available(ITF_HPIL) == 0)
    {
        // no bytes available
        return NO_FRAME;                      // return no data and get out
    }
    else
    {
        // we get here when:
        // - there is a valid serial link
        // - and there is data available in the serial buffer
        // if a frame arrives we must check for a PILBox command first
        if (pendingRx_ >= 0) {                // a byte put back by drainTdisBacklog()
            pil_recv = static_cast<IL_CMD_t>(pendingRx_);
            pendingRx_ = -1;
        } else {
            pil_recv = tud_cdc_n_read_char(ITF_HPIL);
        }
        MLOGF("<-- %02X", pil_recv);
        // PILBox emulation received a byte from the PILBox designated serial port
        // pil_recv contains the returned byte
        if ((pil_recv & 0xE0) == 0x20)
        {
            // this is the higher byte of a transfer
            PIL_tx_hi = pil_recv;       // save until the lower byte arrives
            return NO_FRAME;              // and return with no data
        }
        if ((pil_recv & 0x80) == 0x80)
        {
            // this is the lower byte of an 8-bit transfer
            PILmode8 = true;                    // set the correct mode to 8 bits
            PIL_rx_lo = pil_recv;               
            // this completes the 2-byte transfer, complete the frame
            PIL_rx_frame = (pil_recv & 0x7F) | ((PIL_tx_hi & 0x1E) << 6);
        }
        else  if ((pil_recv & 0xC0) == 0x40)
        {
            // this is the lower byte of a 7-bit transfer
            PILmode8 = false;            // set the correct mode
            PIL_rx_lo = pil_recv;       
            // this completes the 2-byte transfer, complete the frame
            PIL_rx_frame = (pil_recv & 0x3F) | ((PIL_tx_hi & 0x1F) << 6);
        } else {
            // this is a single byte transfer, the frame is complete
            PIL_rx_frame = pil_recv;
        }

        PD_IDY_LOGF(PIL_rx_frame, "\t   <-- %s\r\n", ilMnemonic(PIL_rx_frame, pbBuf));

        // The frame is now received, first process the PILBox commands
        // send to our scope for debugging
        // PILBox_scope(PIL_rx_frame, PIL_tx_hi, pil_recv, false);

        switch (PIL_rx_frame)
        {
        case TDIS:                          // TDI: Translator DIsabled
            PL_LOG_MODE("TDIS");
            PILBox_mode = TDIS;             // set mode to disabled
                                            // frame is not forwarded to the HP-IL emulation
            if (!tdisBacklogDrained_) {
                tdisBacklogDrained_ = true;
                drainTdisBacklog();         // answer a queued-up series only once
            }
            PL_SEND(pil_recv);              // return command for confirmation
            type( NONE );
            //hipi::setStatusLed(display, hipi::StatusLed::Pil, false);
            break;
        case CON:                           // CON: Controller ON
            PL_LOG_MODE("CON");
            PILBox_mode = CON;              // set mode to controller ON
                                            // default on the HP41
                                            // frame is not forwarded to the HP-IL emulation
            PL_SEND(pil_recv);              // return command for confirmation
            PIL_rx_frame = NO_FRAME;          // and return with no data
            type( PILBOX );
            //hipi::setStatusLed(display, hipi::StatusLed::Pil, true);
            break;
        case COFF:                          // COFF: Controller OFF
            PL_LOG_MODE("COFF");
            PILBox_mode = COFF;             // set mode to controller OFF
                                            // the PILBox is now a device
                                            // not used on the HP41
                                            // frame is not forwarded to the HP-IL emulation
            PL_SEND(pil_recv);              // return command for confirmation
            PIL_rx_frame = NO_FRAME;          // and return with no data
            type( PILBOX );
            //hipi::setStatusLed(display, hipi::StatusLed::Pil, true);
            break;
        case COFI:                          // COFI: Controller OFF with IDY 
            PL_LOG_MODE("COFI");
            PILBox_mode = COFI;             // set mode to COFI
                                            // device with sending IDY frame
                                            // frame is not forwarded to the HP-IL emulation
            PL_SEND(pil_recv);              // return command for confirmation
            PIL_rx_frame = NO_FRAME;          // and return with no data
            type( PILBOX );
            //hipi::setStatusLed(display, hipi::StatusLed::Pil, true);
            break;
        // default:
            // all other frames are sent on to the HP-IL loop
        }
        // if we get here the frame is complete
        return PIL_rx_frame;
    }
}

IL_CMD_t CPilBox::sendFrame(IL_CMD_t cmd)
{
    IL_CMD_t frame = cmd;              // the frame to be sent to the PC

    if (!tud_cdc_n_connected(ITF_HPIL) || (PILBox_mode == TDIS)) {
        loopbackFrame = cmd;           // loopback mode
        return NO_FRAME;               // return with no data
    }

    // we send the full frame here
    // normally we can optimize traffic by not sending the hi byte if it is the same as the previous hi byte
    // with the high speed USB connection this is not an issue anymore
    // to be implemented later
    if (PILmode8) {
        // 8-bit transfer mode
        PIL_tx_lo = (frame & 0x007F) | 0x80;        // lower 7 data bits, msb = 1
        PIL_tx_hi = ((frame >> 6) & 0x1E) | 0x20;   // PILBox hi byte previously sent
    } else {
        // 7-bit transfer mode
        PIL_tx_lo = (frame & 0x003F) | 0x40;        // lower 6 data bits, msb = 1
        PIL_tx_hi = ((frame >> 6) & 0x1F) | 0x20;   // higher byte
    }

    // Send frame and flush
    if( !(PILBox_mode == COFF && IS_IDLE(frame)) )
        PL_SEND_FRAME();

    PD_IDY_LOGF(frame, "\t   ==> %s (pilbox)\r\n", ilMnemonic(frame, pbBuf));

    return frame;
}

void CPilBox::idle(void)
{
    receiveFrame();
}

void CPilBox::show(void)
{
    LOGF("\r\n@@@ " HILIGHT "%s" RESET " status: ", name());
    switch(PILBox_mode) {
    case TDIS: LOGF("TDIS"); break;
    case COFF: LOGF("COFF"); break;
    case COFI: LOGF("COFI"); break;
    case CON: LOGF("CON"); break;
    default:  LOGF("unknown");
    }
    LOGF(" %d bit", PILmode8 ? 8 : 7);
}
