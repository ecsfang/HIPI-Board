#define MODULE "TEMP"

#include "iltemp.h"
#include "usb_serial.h"
#include <cstdio>


// Formats a value expressed in HUNDREDTHS of the display unit as a plain
// ASCII number with ONE decimal place, no leading '+' for positive
// values, no unit suffix -- e.g. "23.5" or "-4.2". Deliberately as
// simple as this project's own text protocols elsewhere (CLedParser/
// CPixelParser) get: easy to ARCL/X<> straight into the X-register on
// the controller without any parsing beyond stripping CR/LF.
//
// Shared by both temperature (CBmp280::readTemperatureCenti() already
// returns hundredths of a degree C directly) and pressure: whole Pascals
// are ALSO "hundredths of hPa" with no conversion needed at all, since
// 1 hPa = 100 Pa by definition -- 101325 Pa is exactly 1013.25 hPa, so
// CBmp280::readPressurePa()'s own Pa return value plugs into this
// function completely unchanged.
static void formatHundredths(std::int32_t hundredths, char* buf, std::size_t bufSize) {
    std::int32_t tenths = (hundredths >= 0) ? (hundredths + 5) / 10 : -((-hundredths + 5) / 10);
    const bool neg = tenths < 0;
    if (neg) tenths = -tenths;
    std::snprintf(buf, bufSize, "%s%ld.%ld",
                 neg ? "-" : "", static_cast<long>(tenths / 10), static_cast<long>(tenths % 10));
}

void CHipiTemp::triggerReading() {
    std::int32_t hundredths = 0;
    bool okRead;
    const char* what;
    if (mode_ == Mode::Temperature) {
        okRead = sensor_.readTemperatureCenti(hundredths);
        what = "temperature";
    } else {
        okRead = sensor_.readPressurePa(hundredths);
        what = "pressure";
    }

    std::string s;
    if (okRead) {
        char buf[16];
        formatHundredths(hundredths, buf, sizeof(buf));
        s = buf;
        MTRC_LOGF("%s reading -> %s (osrs=%d)", what, buf,
                         static_cast<int>(sensor_.oversampling()));
    } else {
        // Matches this project's own convention elsewhere of sending a
        // short, plainly-not-numeric marker on failure (rather than
        // silently sending nothing, or a stale/zero value that could be
        // mistaken for a real reading) -- e.g. "ERR" fails to parse as a
        // number as loudly on the controller side as it does to a human
        // reading it on a printer/display.
        s = "ERR";
        MTRC_LOGF("%s reading FAILED -- sensor not responding", what);
    }
    s += "\r\n";
    outQueue_.assign(s.begin(), s.end());
    outEnd_ = false;
}

void CHipiTemp::clear(void) {
    MTRC_LOGF("clear (DCL/SDC)");
    outQueue_.clear();
    outEnd_ = true;
    midTransfer_ = false;
    awaitingPrecisionDigit_ = false;
    awaitingModeChar_ = false;
    // Deliberately does NOT reset mode_ -- DCL/SDC clears in-flight
    // transfer state, not configuration (matches CBmp280's own
    // oversampling_, which clear() has never touched either).
}

void CHipiTemp::ifc(void) {
    status(STAT_IDLE);
}

void CHipiTemp::doListener(IL_CMD_t cmd, IL_CMD_t* rtn) {
    *rtn = cmd;
    if (!IS_DATA(cmd)) return;
    const std::uint8_t c = cmd & 0xFF;

    if (awaitingPrecisionDigit_) {
        awaitingPrecisionDigit_ = false;
        if (c >= '1' && c <= '5') {
            const auto osrs = static_cast<CBmp280::Oversampling>(c - '0');
            sensor_.setOversampling(osrs);
            MTRC_LOGF("precision -> osrs=%d", static_cast<int>(osrs));
        } else {
            MDBG_LOGF("ignored invalid precision digit 0x%02X ('%c')",
                 c, (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.');
        }
        return;
    }

    if (awaitingModeChar_) {
        awaitingModeChar_ = false;
        if (c == 'T') {
            mode_ = Mode::Temperature;
            MTRC_LOGF("mode -> Temperature");
        } else if (c == 'P') {
            mode_ = Mode::Pressure;
            MTRC_LOGF("mode -> Pressure");
        } else {
            MDBG_LOGF("ignored invalid mode letter 0x%02X ('%c')",
                 c, (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.');
        }
        return;
    }

    if (c == 'P') {
        awaitingPrecisionDigit_ = true;
        return;
    }
    if (c == 'M') {
        awaitingModeChar_ = true;
        return;
    }

    // Unrecognised byte with no command pending -- ignore silently,
    // matching CLedParser/CPixelParser's own convention elsewhere.
    MDBG_LOGF("ignored stray byte 0x%02X ('%c')",
             c, (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.');
}

// Drains outQueue_ one byte per incoming poll -- identical structure to
// CPlotter::doTalker() (see its own comments for the full reasoning on
// each part); only triggerReading() at the top (populating outQueue_
// fresh on every new request, in whichever mode_ is currently selected)
// differs, since this device always sends exactly one thing per request
// rather than responding to a variety of different query commands first.
void CHipiTemp::doTalker(IL_CMD_t cmd, IL_CMD_t* rtn) {
    if (cmd == NRD) {
        outEnd_ = true;
        return;
    }

    if (cmd != SDA && !(midTransfer_ && IS_DATA(cmd))) {
        return;
    }

    if (cmd == SDA && !midTransfer_) {
        triggerReading();
    }

    if (outQueue_.empty()) {
        *rtn = ETO;
        midTransfer_ = false;
    } else {
        *rtn = static_cast<IL_CMD_t>(outQueue_.front());
        outQueue_.erase(outQueue_.begin());
        midTransfer_ = true;
        if (outQueue_.empty()) outEnd_ = true;
    }
}
