#ifndef __PLOTTER_H__
#define __PLOTTER_H__

// CPlotter -- TFPLOT, an HP 7470A graphics plotter on HP-IL. Only the
// HP-IL side is here: the device's identity (AID 0x60, default address 5,
// SDI "HP7470A" CR LF), the bytes it's sent as listener go to the HP-GL
// interpreter (hpgl.h), and what that answers (OA, OS, OE ...) goes back
// as talker, byte by byte; SST gives OS's status byte. Drawing HP-GL is
// HpglInterpreter's, showing it the Plotter view's (plotterview.cpp) --
// another plotter (an HP 7475A, another interface) is another device
// class with an HpglInterpreter of its own.

#include "hpil.h"
#include "hpgl.h"
#include <cstdint>
#include <string>
#include <vector>

class CPlotter : public CDevice {
public:
    // sai/aau/device-id match the real HP 7470A: AID 0x60, default
    // address 5, device ID "HP7470A" CR LF (deviceId(), not the name).
    CPlotter(const char *name, IL_ADDR_t _sai = 0x60, IL_ADDR_t _aau = 5);

    ViewKind viewKind() const override { return ViewKind::Plotter; }
    // The plotter's HP-GL: what it has drawn, its pen, its errors
    HpglInterpreter& hpgl() { return hpgl_; }
    const HpglInterpreter& hpgl() const { return hpgl_; }

    void clear(void);   // HP-IL "Clear Device" (DCL/SDC) -- resets everything (as IN)
    // SDI: as the real plotter (manual, chapter 11: "HP7470A" CR LF)
    const char* deviceId() override { return "HP7470A\r\n"; }
    void doListener(IL_CMD_t cmd, IL_CMD_t *rtn);
    void doTalker(IL_CMD_t cmd, IL_CMD_t *rtn);
    void show(void);    // extended-trace detail: HP-GL state, the answer waiting
    // Main loop: a command left open at a line end is carried out when
    // nothing more has come (HpglInterpreter::idle())
    void idle(void) override { hpgl_.idle(); }

    // For the tests: the answer waiting to be sent (an O.. instruction's)
    std::string outputText() const { return std::string(outQueue_.begin(), outQueue_.end()); }

private:
    void resetTalker();                       // nothing to send (IN, a device clear)

    HpglInterpreter hpgl_;

    // ── Talker (query response) output queue ─────────────────────────
    // Filled with an O.. instruction's answer (HpglInterpreter's output
    // callback) and drained one byte per incoming SDA frame while this
    // device is the addressed talker -- mirrors CDrive's doTalker()/
    // doNextTalker() convention (SDA = send next byte, NRD = no more data
    // expected, ETO = end-of-transfer response once the queue is empty).
    std::vector<std::uint8_t> outQueue_;
    bool outEnd_ = true;
    // True only while we're actively mid-transfer (an SDA has already
    // started one) -- lets doTalker() safely recognize the echoed data
    // byte looping back around the physical bus as "continue", without
    // ever mistaking routine bus traffic (RFC, AAU, addressing, IFC, ...)
    // for that, since those never satisfy IS_DATA() anyway.
    bool midTransfer_ = false;
    // HP-IL Send Status (SST): the OS byte, then ETO when it comes back
    bool sstPending_ = false;
};

// The single CPlotter instance created in hipi_init() (see hipi.cpp) --
// exposed so other modules (the views, menus, LEDs) can reach its HP-GL
// (hpgl()) without searching the `devices` vector. Matches the same
// pattern as pilbox.h's `extern CPilBox* pilbox;`.
extern CPlotter* plotter;

#endif//__PLOTTER_H__
