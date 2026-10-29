// printer_paper.h -- the printer's paper (CPrinter, printer.h) kept in the
// display chip's SDRAM: 8192 lines of CPrinter::kRecordBytes (2 MB at
// LT7683::kPrinterPaperAddr) instead of 200 in the MCU's RAM. Written and
// read over SPI a line at a time (writeSdram() / readSdram()); a viewer of
// the display mirror isn't sent any of it (displayMirror_quiet()).
#pragma once
#include "display_config.h"

class CPrinter;

namespace hipi {

constexpr std::uint32_t kPrinterPaperLines = 8192;

// Gives the printer its store (once, after both exist: pico_main.cpp)
void printerPaper_init(LT7683* display, CPrinter* printer);

}  // namespace hipi
