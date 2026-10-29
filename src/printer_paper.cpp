// printer_paper.cpp -- see printer_paper.h
#include "printer_paper.h"
#include "printer.h"
#include "display_mirror.h"

namespace hipi {

namespace {

class SdramPaperStore : public PaperStore {
public:
    explicit SdramPaperStore(LT7683* d) : d_(d) {}
    std::uint32_t capacity() const override { return kPrinterPaperLines; }
    void write(std::uint32_t record, const std::uint8_t* data) override {
        displayMirror_quiet(true);
        d_->writeSdram(address(record), data, CPrinter::kRecordBytes);
        displayMirror_quiet(false);
    }
    void read(std::uint32_t record, std::uint8_t* data) override {
        displayMirror_quiet(true);
        d_->readSdram(address(record), data, CPrinter::kRecordBytes);
        displayMirror_quiet(false);
    }

private:
    static std::uint32_t address(std::uint32_t record) {
        return LT7683::kPrinterPaperAddr + record * static_cast<std::uint32_t>(CPrinter::kRecordBytes);
    }
    LT7683* d_;
};

static_assert(kPrinterPaperLines * CPrinter::kRecordBytes <= LT7683::kPrinterPaperBytes, "the paper's SDRAM");

}  // namespace

void printerPaper_init(LT7683* display, CPrinter* printer) {
    if (display == nullptr || printer == nullptr) return;
    static SdramPaperStore store(display);
    printer->setPaperStore(&store);
}

}  // namespace hipi
