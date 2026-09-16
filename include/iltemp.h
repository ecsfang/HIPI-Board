#ifndef __ILTEMP_H__
#define __ILTEMP_H__

#include <vector>
#include <string>
#include "hpil.h"
#include "bmp280.h"

// ─────────────────────────────────────────────────────────────────────────────
//  CHipiTemp -- presents a CBmp280 as an HP-IL "electronic instrumentation"
//  measurement device (Accessory ID class 5x, type 1 = signal measurement
//  only -- see the HP-IL Interface Specification, Appendix C.4, "Classes
//  and Types Defined"). Still 0x51 with pressure added -- the Accessory ID
//  describes CAPABILITY (this is a measurement-only device), not WHICH
//  physical quantity is being measured, and this remains true whichever
//  mode (see below) is currently selected.
//
//  Talker: every Send Data (SDA) triggers a fresh sensor reading in
//  whichever mode is currently selected (temperature by default) and
//  sends it back as a plain ASCII number with one decimal place (e.g.
//  "23.5" for temperature, "1013.2" for pressure in hPa), no sign for
//  positive values, no unit suffix -- easy to ARCL/X<> straight into a
//  number on the controller, matching this project's own established
//  doTalker() pattern (see CPlotter::doTalker()) of queuing the full
//  response string up front and draining it one byte per SDA-or-echo
//  cycle.
//
//  Listener: two simple commands, matching CLedParser/CPixelParser's own
//  single-letter-prefix style elsewhere in this project:
//    'P' + one ASCII digit '1'-'5' -- sets the oversampling ("precision")
//        used by every subsequent reading, in EITHER mode (see
//        CBmp280::setOversampling()'s own comment on why one setting
//        covers both) -- matches CBmp280::Oversampling's own X1..X16
//        values directly, so 'P''3' selects X4. The digit is the ENUM'S
//        own integer value, not "the Nth setting" -- kept deliberately
//        literal rather than renumbering, since there's no simpler
//        mapping that doesn't just move the same lookup somewhere else.
//    'M' + 'T' or 'P' -- selects what the NEXT (and every following)
//        SDA reads: Temperature or Pressure. Defaults to Temperature at
//        construction, so anything already using this device (an
//        existing HP-41C program) keeps working unchanged unless it
//        explicitly asks for pressure.
//  Any other byte with no command pending, or an invalid digit/mode
//  letter, is ignored, matching CLedParser/CPixelParser's own "ignore
//  stray/invalid bytes" convention.
// ─────────────────────────────────────────────────────────────────────────────
class CHipiTemp : public CDevice {
public:
    enum class Mode { Temperature, Pressure };

    CHipiTemp(const char* name, IL_ADDR_t sai, CBmp280& sensor, IL_ADDR_t aau = 31)
        : CDevice(name, sai, aau, PIXEL), sensor_(sensor) {
        // Reuses the PIXEL device-type tag purely for this project's own
        // internal categorization (see hpil.h's IL_Type_e) -- there isn't
        // a more specific tag for "measurement instrument" yet and
        // nothing in this codebase currently branches on PIXEL
        // specifically the way it does on DRIVE (see usb_msc.cpp), so
        // this is safe; add a dedicated INSTRUMENT tag instead if that
        // ever changes.
    }

    void clear() override;
    void ifc();
    void doListener(IL_CMD_t cmd, IL_CMD_t* rtn);
    void doTalker(IL_CMD_t cmd, IL_CMD_t* rtn);

private:
    CBmp280& sensor_;
    Mode mode_ = Mode::Temperature;

    // Outgoing byte queue -- mirrors CPlotter's own outQueue_/outEnd_/
    // midTransfer_ exactly (see plotter.h's own comments for the full
    // reasoning); populated by triggerReading() below instead of an
    // HPGL query command, but drained by doTalker() the identical way.
    std::vector<std::uint8_t> outQueue_;
    bool outEnd_ = true;
    bool midTransfer_ = false;

    // True right after a listener 'P' byte arrives -- the next byte fed
    // to doListener() is taken as the oversampling digit rather than a
    // new command.
    bool awaitingPrecisionDigit_ = false;
    // True right after a listener 'M' byte arrives -- the next byte fed
    // to doListener() is taken as the mode letter ('T'/'P') rather than
    // a new command.
    bool awaitingModeChar_ = false;

    // Reads the sensor (temperature or pressure, per mode_) at the
    // current oversampling setting and queues the formatted result (or
    // "ERR" if the read itself failed, e.g. the chip isn't actually
    // responding) for doTalker() to send out.
    void triggerReading();
};

#endif//__ILTEMP_H__
