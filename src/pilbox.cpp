#include <stdio.h>
#include <ctype.h>

#include "hpil.h"
#include "pilbox.h"
#include "tusb.h" 
#include "usb_serial.h"

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
#define P_DEBUG false

#define PL_MODE(x) do { if( P_DEBUG) cdc0_printf("\r\nPILBOX: " #x "\r\n"); } while(0)

// Send a single byte to the PILBox serial link and flush
#define PL_SEND(x) do { \
        tud_cdc_n_write_char(ITF_HPIL, x); \
        tud_cdc_n_write_flush(ITF_HPIL); \
    } while(0)

// Send a complete frame to the PILBox serial link, using the 2-byte format and flush
#define PL_SEND_FRAME() do { \
        tud_cdc_n_write(ITF_HPIL, &PIL_tx_hi,1); \
        tud_cdc_n_write(ITF_HPIL, &PIL_tx_lo,1); \
        tud_cdc_n_write_flush(ITF_HPIL);         \
    } while(0)

static unsigned char PIL_rx_lo;             // PILBox lo byte previously received   
static unsigned char PIL_tx_lo;             // PILBox lo byte previously sent   
static unsigned char PIL_tx_hi;             // PILBox hi byte previously sent
static IL_CMD_t PIL_rx_frame;               // PILBox frame just received
static bool PILmode8 = true;                // PILBox transfer mode, true when in 8-bit mode, false when in 7-bit mode
static IL_CMD_t PILBox_mode = TDIS;         // PILBOx mode (TDIS, CON, COFF, COFI)
static IL_CMD_t pil_recv;                   // PILBox received byte from the serial link
static char pbBuf[32];
static IL_CMD_t m_wLastCmd;
static IL_CMD_t loopbackFrame = NO_FRAME;     // loopback frame when no valid serial link is available

static IL_CMD_t send2PILBox(IL_CMD_t cmd)
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
    PL_SEND_FRAME();

    if( frame != 0x6C0) {
        if( P_DEBUG ) {
            ilMnemonic(frame, pbBuf);
            cdc0_printf("\t   ==> %s (pilbox)\r\n", pbBuf);
        }
    }
    return frame;
}

IL_CMD_t ILBOX_ReceiveFrame(void)
{
    IL_CMD_t frame;
    if (!tud_cdc_n_connected(ITF_HPIL))
    {
        // no valid serial link, loopback mode 
        frame = loopbackFrame;                  // return the last frame sent    
        loopbackFrame = NO_FRAME;                 // reset the loopback frame
        return frame;                      // return no data and get out
    }
    else if (tud_cdc_n_available(ITF_HPIL) == 0)
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
        pil_recv = tud_cdc_n_read_char(ITF_HPIL);

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

        if( PIL_rx_frame != 0x6C0) {
            if( P_DEBUG ) {
                ilMnemonic(PIL_rx_frame, pbBuf);
                cdc0_printf("\t   <-- %s\r\n", pbBuf);
            }
        }

        // The frame is now received, first process the PILBox commands
        // send to our scope for debugging
        // PILBox_scope(PIL_rx_frame, PIL_tx_hi, pil_recv, false);

        switch (PIL_rx_frame)
        {
        case TDIS:                          // TDI: Translator DIsabled
            PL_MODE("TDIS");
            PILBox_mode = TDIS;             // set mode to disabled
                                            // frame is not forwarded to the HP-IL emulation
            PL_SEND(pil_recv);              // return command for confirmation
            break;
        case CON:                           // CON: Controller ON
            PL_MODE("CON");
            PILBox_mode = CON;              // set mode to controller ON
                                            // default on the HP41
                                            // frame is not forwarded to the HP-IL emulation
            PL_SEND(pil_recv);              // return command for confirmation
            PIL_rx_frame = NO_FRAME;          // and return with no data
            break;
        case COFF:                          // COFF: Controller OFF
            PL_MODE("COFF");
            PILBox_mode = COFF;             // set mode to controller OFF
                                            // the PILBox is now a device
                                            // not used on the HP41
                                            // frame is not forwarded to the HP-IL emulation
            PL_SEND(pil_recv);              // return command for confirmation
            PIL_rx_frame = NO_FRAME;          // and return with no data
            break;
        case COFI:                          // COFI: Controller OFF with IDY 
            PL_MODE("COFI");
            PILBox_mode = COFI;             // set mode to COFI
                                            // device with sending IDY frame
                                            // frame is not forwarded to the HP-IL emulation
            PL_SEND(pil_recv);              // return command for confirmation
            PIL_rx_frame = NO_FRAME;          // and return with no data
            break;
        // default:
            // all other frames are sent on to the HP-IL loop
        }
        // if we get here the frame is complete
        return PIL_rx_frame;
    }
}

IL_CMD_t ILBOX_SendFrame(IL_CMD_t wFrame)
{
    // Got a frame from the HPIL bus
    // in this case the frame is received from the PILBox emulator
    // modelled after functions in V41 (Christoph Giesselink)
    // for the HP-IL Scope
    //HPIL_scope(wFrame, false, true);

    // if the PILBox is in TDIS mode, just return the frame
    if( PILBox_mode == TDIS )
        return wFrame;

    // CMD frame and CA (controller active) and adding RFC frame enabled
    // CMD/RFC handshaking is done in the PILBox emulation
    if (((wFrame & 0x700) == DOE) ) {
        m_wLastCmd = wFrame;                            // remember last CMD frame
        if( P_DEBUG ) {
            ilMnemonic(m_wLastCmd, pbBuf);
            cdc0_printf("\t   <== %s (skip)\r\n", pbBuf);
        }
        return wFrame;
    }

    // CA (controller active) and RFC frame
    // CMD/RFC handshaking done in the PILBox emulation
    if ((wFrame == RFC)) {
        if( P_DEBUG ) {
            cdc0_printf("pilbox: RFC\r\n");
        }
        wFrame = m_wLastCmd;                            // use the last CMD frame as answer
        send2PILBox(wFrame);                            // send the RFC frame
        IL_CMD_t pil_cmd;
        do {
            tud_task();  // TinyUSB background task
            pil_cmd = ILBOX_ReceiveFrame();
        } while( pil_cmd == NO_FRAME );
        if( P_DEBUG ) {
            cdc0_printf("\t   <== RFC!\r\n");
        }
        return RFC;
    } 
        
    send2PILBox(wFrame);                            // send the frame

    IL_CMD_t pil_cmd;
    do {
        tud_task();  // TinyUSB background task
        pil_cmd = ILBOX_ReceiveFrame();
    } while( pil_cmd == NO_FRAME );
    if( pil_cmd != 0x6C0) {
        if( P_DEBUG ) {
            ilMnemonic(pil_cmd, pbBuf);
            cdc0_printf("\t   <== %s\r\n", pbBuf);
        }
    }
    return pil_cmd;                            // return the received frame
}
