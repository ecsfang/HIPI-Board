// media_create_ui.cpp -- see media_create_ui.h
#include "media_create_ui.h"
#include "menu_frame.h"
#include <algorithm>
#include <cstdio>

namespace hipi {

namespace {

constexpr std::uint16_t kBlack  = 0x0000;
constexpr std::uint16_t kWhite  = 0xFFFF;
constexpr std::uint16_t kYellow = MenuFrame::Yellow;
constexpr std::uint16_t kGrey   = 0x7BEF;    // labels, hints, outlines
constexpr std::uint16_t kDim    = 0x4208;    // a control that can't be used
constexpr std::uint16_t kKey    = 0x18E3;    // a key / control
constexpr std::uint16_t kFnKey  = 0x31A6;    // Cancel, delete
constexpr std::uint16_t kPress  = 0x5A42;    // a key while pressed
constexpr std::uint16_t kRed    = 0xF800;

// The button strip's width at the right edge (120 px artwork + margin):
// the dialogs stay left of it, so it can still be used
constexpr int kStripW = 124;

// Built-in font: 8x16 at scale 0, 16x32 at scale 1
int charW(int scale) { return 8 * (scale + 1); }
int charH(int scale) { return 16 * (scale + 1); }

std::string sizeText(std::uint64_t bytes) {
    char b[32];
    if (bytes >= 1024ull * 1024 * 1024)
        std::snprintf(b, sizeof(b), "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ull * 1024)
        std::snprintf(b, sizeof(b), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024));
    else
        std::snprintf(b, sizeof(b), "%u kB", static_cast<unsigned>(bytes / 1024));
    return b;
}

}  // namespace

MediaCreateUi::MediaCreateUi(DisplayDriver* d) : d_(d) {
    const bool small = SCREEN_MAX_Y < 600;          // the 5" panel
    const int areaW = SCREEN_MAX_X - kStripW;
    frame_.x = small ? 8 : 20;
    frame_.y = small ? 6 : 20;
    frame_.w = areaW - 2 * frame_.x;
    frame_.h = SCREEN_MAX_Y - 2 * frame_.y;
    cx_ = frame_.x + 24;
    cw_ = frame_.w - 48;
    ty_ = frame_.y + MenuFrame::TitleH + 14;

    // CREATE MEDIA: three rows (a small label, then the control), the
    // numbers, a message line, the buttons
    rowPitch_ = small ? 70 : 80;
    constexpr int kCtlH = 44, kArrowW = 56;
    nameBox_ = { cx_, ty_ + 20, cw_, kCtlH };
    typeL_ = { cx_, ty_ + rowPitch_ + 20, kArrowW, kCtlH };
    typeR_ = { cx_ + cw_ - kArrowW, typeL_.y, kArrowW, kCtlH };
    dirL_ = { cx_, ty_ + 2 * rowPitch_ + 20, kArrowW, kCtlH };
    dirR_ = { cx_ + cw_ - kArrowW, dirL_.y, kArrowW, kCtlH };
    infoY_ = ty_ + 3 * rowPitch_ + 8;
    msgY_ = infoY_ + 4 * 22 + 6;
    const int btnH = small ? 46 : 50, btnW = small ? 150 : 180;
    const int by = frame_.y + frame_.h - (small ? 12 : 20) - btnH;
    createBtn_ = { cx_ + cw_ - btnW, by, btnW, btnH };
    cancelBtn_ = { createBtn_.x - 16 - btnW, by, btnW, btnH };
    progress_ = { cx_, by, cancelBtn_.x - 16 - cx_, btnH };

    // MEDIA NAME: the name, a hint line, five rows of keys
    field_ = { cx_, ty_, cw_, 52 };
    constexpr int kGap = 8;
    const int kw = (cw_ - 9 * kGap) / 10;
    const int ky = ty_ + 84;
    const int avail = frame_.y + frame_.h - 16 - ky;
    const int kh = std::min(62, avail / 5 - kGap);
    const char* rows[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
    auto addRow = [&](const std::vector<std::pair<char, int>>& row, int r) {
        int units = 0;
        for (const auto& k : row) units += k.second;
        const int gaps = static_cast<int>(row.size()) - 1;
        const int rowW = units * kw + (units - static_cast<int>(row.size())) * kGap + gaps * kGap;
        int x = cx_ + (cw_ - rowW) / 2;
        for (const auto& k : row) {
            const int w = k.second * kw + (k.second - 1) * kGap;
            keys_.push_back({ { x, ky + r * (kh + kGap), w, kh }, k.first, k.second });
            x += w + kGap;
        }
    };
    for (int r = 0; r < 4; ++r) {
        std::vector<std::pair<char, int>> row;
        for (const char* p = rows[r]; *p; ++p) row.push_back({ *p, 1 });
        addRow(row, r);
    }
    addRow({ { kCancel, 3 }, { '-', 1 }, { '_', 1 }, { kDel, 2 }, { kOk, 3 } }, 4);
}

void MediaCreateUi::frameRect(int& x, int& y, int& w, int& h) const {
    x = frame_.x; y = frame_.y; w = frame_.w; h = frame_.h;
}

void MediaCreateUi::center(Ctl c, int& x, int& y) const {
    const Rect* r = &nameBox_;
    switch (c) {
        case Ctl::Name: r = &nameBox_; break;
        case Ctl::TypeLeft: r = &typeL_; break;
        case Ctl::TypeRight: r = &typeR_; break;
        case Ctl::DirLeft: r = &dirL_; break;
        case Ctl::DirRight: r = &dirR_; break;
        case Ctl::Cancel: r = &cancelBtn_; break;
        case Ctl::Create: r = &createBtn_; break;
    }
    x = r->x + r->w / 2;
    y = r->y + r->h / 2;
}

bool MediaCreateUi::keyCenter(char code, int& x, int& y) const {
    for (const Key& k : keys_) {
        if (k.code != code) continue;
        x = k.r.x + k.r.w / 2;
        y = k.r.y + k.r.h / 2;
        return true;
    }
    return false;
}

// ── State ───────────────────────────────────────────────────────────────

void MediaCreateUi::open() {
    creator_.cancel();
    page_ = Page::Form;
    focus_ = 0;
    message_.clear();
    created_.clear();
    flashKey_ = -1;
    freeBytes_ = sdFreeBytes();
    nameSuggested_ = true;
    setType(0);
}

int MediaCreateUi::dirEntries() const {
    const std::vector<int> c = lifDirChoices(type());
    return c.empty() ? 8 : c[static_cast<std::size_t>(std::clamp(dirIdx_, 0, static_cast<int>(c.size()) - 1))];
}

void MediaCreateUi::setType(int idx) {
    typeIdx_ = (idx + kLifMediaTypeCount) % kLifMediaTypeCount;
    // The type's usual directory size
    const std::vector<int> c = lifDirChoices(type());
    dirIdx_ = 0;
    for (std::size_t i = 0; i < c.size(); ++i)
        if (c[i] <= type().defaultDirEntries) dirIdx_ = static_cast<int>(i);
    // A suggested name follows the type (CASS01 / DISK01)
    if (nameSuggested_) name_ = lifSuggestName(type().prefix);
}

void MediaCreateUi::stepDir(int delta) {
    const int n = static_cast<int>(lifDirChoices(type()).size());
    if (n > 0) dirIdx_ = (dirIdx_ + delta + n) % n;
}

std::string MediaCreateUi::problem() const {
    if (!lifNameValid(name_)) return "Give the file a name";
    if (lifFileExists(name_)) return name_ + ".DAT already exists";
    if (freeBytes_ < type().bytes() + 64 * 1024) return "Not enough space on the SD card";
    return "";
}

MediaAction MediaCreateUi::create() {
    if (creating()) return MediaAction::None;
    message_ = problem();
    if (!message_.empty()) { drawMessage(); return MediaAction::None; }
    LifDate date{};
    const bool haveDate = dateSource_ && dateSource_(date);
    if (!creator_.begin(name_, type(), dirEntries(), haveDate ? &date : nullptr)) {
        message_ = creator_.error();
        drawMessage();
        return MediaAction::None;
    }
    lastPercent_ = -1;
    message_.clear();
    drawForm();                              // controls dimmed, the progress bar
    return MediaAction::None;
}

MediaAction MediaCreateUi::formOk() {
    switch (focus_) {
        case 0: openKeyboard(); break;
        case 1: setType(typeIdx_ + 1); message_.clear(); drawForm(); break;
        case 2: stepDir(+1); drawChoice(2, std::to_string(dirEntries()) + " files"); drawInfo(); break;
        default: return create();
    }
    return MediaAction::None;
}

void MediaCreateUi::openKeyboard() {
    page_ = Page::Keyboard;
    edit_ = name_;
    flashKey_ = -1;
    draw();
}

MediaAction MediaCreateUi::keyPressed(char code) {
    if (code == kOk) {
        if (edit_.empty()) return MediaAction::None;     // a name is needed
        if (edit_ != name_) nameSuggested_ = false;
        name_ = edit_;
        message_.clear();
        page_ = Page::Form;
        draw();
    } else if (code == kCancel) {
        page_ = Page::Form;
        draw();
    } else if (code == kDel) {
        if (!edit_.empty()) { edit_.pop_back(); drawField(); }
    } else if (lifNameChar(code) && static_cast<int>(edit_.size()) < kLifNameMax) {
        edit_ += code;
        drawField();
    }
    return MediaAction::None;
}

// ── Input ───────────────────────────────────────────────────────────────

MediaAction MediaCreateUi::tap(int x, int y) {
    if (page_ == Page::Keyboard) {
        for (std::size_t i = 0; i < keys_.size(); ++i) {
            if (!keys_[i].r.hit(x, y)) continue;
            const char code = keys_[i].code;
            if (code != kOk && code != kCancel) {
                // Shown pressed for a moment (poll() ends it)
                if (flashKey_ >= 0 && flashKey_ != static_cast<int>(i)) drawKey(keys_[static_cast<std::size_t>(flashKey_)], false);
                flashKey_ = static_cast<int>(i);
                flashTimed_ = false;
                drawKey(keys_[i], true);
            }
            return keyPressed(code);
        }
        return MediaAction::None;
    }
    if (creating()) {
        if (cancelBtn_.hit(x, y)) {
            creator_.cancel();
            message_ = "Cancelled -- nothing was created";
            draw();
        }
        return MediaAction::None;
    }
    if (nameBox_.hit(x, y))  { focus_ = 0; openKeyboard(); return MediaAction::None; }
    if (typeL_.hit(x, y) || typeR_.hit(x, y)) {
        focus_ = 1;
        setType(typeIdx_ + (typeL_.hit(x, y) ? -1 : 1));
        message_.clear();
        drawForm();
        return MediaAction::None;
    }
    if (dirL_.hit(x, y) || dirR_.hit(x, y)) {
        const int old = focus_;
        focus_ = 2;
        stepDir(dirL_.hit(x, y) ? -1 : 1);
        if (old != focus_) drawForm();
        else { drawChoice(2, std::to_string(dirEntries()) + " files"); drawInfo(); }
        return MediaAction::None;
    }
    if (cancelBtn_.hit(x, y)) return MediaAction::Cancel;
    if (createBtn_.hit(x, y)) { focus_ = 3; drawForm(); return create(); }
    return MediaAction::None;
}

MediaAction MediaCreateUi::button(Button b) {
    if (page_ == Page::Keyboard) {
        if (b == Button::Ok) return keyPressed(kOk);
        if (b == Button::X) return keyPressed(edit_.empty() ? kCancel : kDel);
        return MediaAction::None;
    }
    if (creating()) {
        if (b == Button::X) {
            creator_.cancel();
            message_ = "Cancelled -- nothing was created";
            draw();
        }
        return MediaAction::None;
    }
    switch (b) {
        case Button::Up:   focus_ = (focus_ + 3) % 4; drawForm(); break;
        case Button::Down: focus_ = (focus_ + 1) % 4; drawForm(); break;
        case Button::Ok:   return formOk();
        case Button::X:    return MediaAction::Cancel;
        default: break;
    }
    return MediaAction::None;
}

MediaAction MediaCreateUi::poll(std::uint32_t nowMs) {
    if (flashKey_ >= 0) {
        if (!flashTimed_) { flashUntil_ = nowMs + 120; flashTimed_ = true; }
        else if (static_cast<std::int32_t>(nowMs - flashUntil_) >= 0) {
            if (page_ == Page::Keyboard) drawKey(keys_[static_cast<std::size_t>(flashKey_)], false);
            flashKey_ = -1;
        }
    }
    if (!creating()) return MediaAction::None;
    const LifCreator::State s = creator_.step();
    if (s == LifCreator::State::Running) {
        if (creator_.percent() != lastPercent_) drawProgress();
        return MediaAction::None;
    }
    if (s == LifCreator::State::Done) {
        created_ = creator_.fileName();
        return MediaAction::Created;
    }
    message_ = creator_.error();             // failed (the file is gone again)
    freeBytes_ = sdFreeBytes();
    draw();
    return MediaAction::None;
}

// ── Drawing ─────────────────────────────────────────────────────────────

void MediaCreateUi::box(const Rect& r, std::uint16_t outline, std::uint16_t fill, int radius) {
    d_->fillRoundRect(static_cast<std::int16_t>(r.x), static_cast<std::int16_t>(r.y), static_cast<std::int16_t>(r.w),
                      static_cast<std::int16_t>(r.h), static_cast<std::int16_t>(radius), outline);
    d_->fillRoundRect(static_cast<std::int16_t>(r.x + 2), static_cast<std::int16_t>(r.y + 2),
                      static_cast<std::int16_t>(r.w - 4), static_cast<std::int16_t>(r.h - 4),
                      static_cast<std::int16_t>(std::max(radius - 2, 0)), fill);
}

void MediaCreateUi::text(int x, int y, const std::string& s, int scale, std::uint16_t fg, std::uint16_t bg) {
    d_->selectBuiltinFont();
    d_->txtSize(static_cast<std::uint8_t>(scale));
    d_->txtColor(fg, bg);
    d_->txtSetCursor(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
    d_->txtWrite(s.c_str());
}

void MediaCreateUi::textCentered(const Rect& r, const std::string& s, int scale, std::uint16_t fg, std::uint16_t bg) {
    const int w = static_cast<int>(s.size()) * charW(scale);
    text(r.x + (r.w - w) / 2, r.y + (r.h - charH(scale)) / 2, s, scale, fg, bg);
}

void MediaCreateUi::arrow(const Rect& r, bool left, std::uint16_t col) {
    const int cx = r.x + r.w / 2, cy = r.y + r.h / 2, a = 10;
    if (left)
        d_->fillTriangle(static_cast<std::int16_t>(cx - a), static_cast<std::int16_t>(cy), static_cast<std::int16_t>(cx + a / 2),
                         static_cast<std::int16_t>(cy - a), static_cast<std::int16_t>(cx + a / 2), static_cast<std::int16_t>(cy + a), col);
    else
        d_->fillTriangle(static_cast<std::int16_t>(cx + a), static_cast<std::int16_t>(cy), static_cast<std::int16_t>(cx - a / 2),
                         static_cast<std::int16_t>(cy - a), static_cast<std::int16_t>(cx - a / 2), static_cast<std::int16_t>(cy + a), col);
}

void MediaCreateUi::draw() {
    MenuFrame::drawTitled(d_, frame_.x, frame_.y, frame_.w, frame_.h,
                          page_ == Page::Form ? "CREATE MEDIA" : "MEDIA NAME");
    if (page_ == Page::Form) drawForm();
    else drawKeyboard();
}

void MediaCreateUi::drawForm() {
    const bool busyNow = creating();
    const std::uint16_t dimLabel = busyNow ? kDim : kGrey;
    text(cx_, ty_, "Name", 0, dimLabel, kBlack);
    text(cx_, ty_ + rowPitch_, "Type", 0, dimLabel, kBlack);
    text(cx_, ty_ + 2 * rowPitch_, "Directory", 0, dimLabel, kBlack);
    drawName();
    drawChoice(1, type().name);
    drawChoice(2, std::to_string(dirEntries()) + " files");
    drawInfo();
    drawMessage();
    if (busyNow) drawProgress();
    else drawButtons();
}

void MediaCreateUi::drawName() {
    const bool hot = focus_ == 0 && !creating();
    // Red: the name can't be used (empty, or the file exists)
    const bool bad = !creating() && (!lifNameValid(name_) || lifFileExists(name_));
    box(nameBox_, bad ? kRed : hot ? kYellow : kGrey, kKey);
    const int ty = nameBox_.y + (nameBox_.h - 32) / 2;
    text(nameBox_.x + 16, ty, name_, 1, kWhite, kKey);
    text(nameBox_.x + 16 + static_cast<int>(name_.size()) * 16, ty, ".DAT", 1, kGrey, kKey);
    if (!creating()) text(nameBox_.x + nameBox_.w - 16 - 4 * 8, nameBox_.y + (nameBox_.h - 16) / 2, "edit", 0, kYellow, kKey);
}

void MediaCreateUi::drawChoice(int row, const std::string& value) {
    const Rect& l = row == 1 ? typeL_ : dirL_;
    const Rect& r = row == 1 ? typeR_ : dirR_;
    const bool hot = focus_ == row && !creating();
    const std::uint16_t arrowCol = creating() ? kDim : kWhite;
    box(l, hot ? kYellow : kGrey, kKey);
    box(r, hot ? kYellow : kGrey, kKey);
    arrow(l, true, arrowCol);
    arrow(r, false, arrowCol);
    const Rect mid{ l.x + l.w + 4, l.y, r.x - l.x - l.w - 8, l.h };
    d_->fillRect(static_cast<std::int16_t>(mid.x), static_cast<std::int16_t>(mid.y), static_cast<std::int16_t>(mid.w),
                 static_cast<std::int16_t>(mid.h), kBlack);
    textCentered(mid, value, 1, hot ? kYellow : kWhite, kBlack);
}

void MediaCreateUi::drawInfo() {
    const LifMediaType& t = type();
    const int entries = dirEntries();
    char b[4][64];
    std::snprintf(b[0], sizeof(b[0]), "%lu blocks  (%s)", static_cast<unsigned long>(t.blocks()), sizeText(t.bytes()).c_str());
    std::snprintf(b[1], sizeof(b[1]), "%d blocks, up to %d files", lifDirBlocks(entries), entries);
    std::snprintf(b[2], sizeof(b[2]), "%d blocks", lifDataBlocks(t, entries));
    std::snprintf(b[3], sizeof(b[3]), "%s free", sizeText(freeBytes_).c_str());
    const char* labels[4] = { "Size", "Directory", "For files", "SD card" };
    for (int i = 0; i < 4; ++i) {
        const int y = infoY_ + i * 22;
        d_->fillRect(static_cast<std::int16_t>(cx_), static_cast<std::int16_t>(y), static_cast<std::int16_t>(cw_), 18, kBlack);
        text(cx_, y, labels[i], 0, kGrey, kBlack);
        text(cx_ + 160, y, b[i], 0, kWhite, kBlack);
    }
}

void MediaCreateUi::drawMessage() {
    d_->fillRect(static_cast<std::int16_t>(cx_), static_cast<std::int16_t>(msgY_), static_cast<std::int16_t>(cw_), 18, kBlack);
    if (creating()) {
        text(cx_, msgY_, "Creating " + name_ + ".DAT ...", 0, kYellow, kBlack);
        return;
    }
    // The message, or else what's in the way of Create
    const std::string m = message_.empty() ? problem() : message_;
    if (!m.empty()) text(cx_, msgY_, m, 0, kRed, kBlack);
}

void MediaCreateUi::drawButtons() {
    d_->fillRect(static_cast<std::int16_t>(cx_), static_cast<std::int16_t>(createBtn_.y), static_cast<std::int16_t>(cw_),
                 static_cast<std::int16_t>(createBtn_.h), kBlack);
    box(cancelBtn_, kGrey, kFnKey, 10);
    textCentered(cancelBtn_, "Cancel", 1, kWhite, kFnKey);
    const bool can = problem().empty();
    const bool hot = focus_ == 3;
    if (can) {
        box(createBtn_, hot ? kWhite : kYellow, kYellow, 10);
        textCentered(createBtn_, "Create", 1, kBlack, kYellow);
    } else {
        box(createBtn_, hot ? kYellow : kDim, kKey, 10);
        textCentered(createBtn_, "Create", 1, kDim, kKey);
    }
}

void MediaCreateUi::drawProgress() {
    const int pct = creator_.percent();
    lastPercent_ = pct;
    d_->fillRect(static_cast<std::int16_t>(cx_), static_cast<std::int16_t>(createBtn_.y), static_cast<std::int16_t>(cw_),
                 static_cast<std::int16_t>(createBtn_.h), kBlack);
    box(progress_, kGrey, kBlack, 8);
    const int inner = progress_.w - 8;
    const int filled = inner * pct / 100;
    if (filled > 0)
        d_->fillRect(static_cast<std::int16_t>(progress_.x + 4), static_cast<std::int16_t>(progress_.y + 4),
                     static_cast<std::int16_t>(filled), static_cast<std::int16_t>(progress_.h - 8), kYellow);
    char b[8];
    std::snprintf(b, sizeof(b), "%d %%", pct);
    const Rect label{ progress_.x + 4 + filled + 8, progress_.y, 80, progress_.h };
    if (filled + 96 < inner) text(label.x, progress_.y + (progress_.h - 32) / 2, b, 1, kWhite, kBlack);
    else text(progress_.x + 12, progress_.y + (progress_.h - 32) / 2, b, 1, kBlack, kYellow);
    box(cancelBtn_, kGrey, kFnKey, 10);
    textCentered(cancelBtn_, "Cancel", 1, kWhite, kFnKey);
}

void MediaCreateUi::drawKeyboard() {
    drawField();
    text(cx_, ty_ + 60, "A-Z 0-9 - _      OK = done   X = delete last", 0, kGrey, kBlack);
    for (std::size_t i = 0; i < keys_.size(); ++i) drawKey(keys_[i], static_cast<int>(i) == flashKey_);
}

void MediaCreateUi::drawField() {
    box(field_, kYellow, kKey, 10);
    const int ty = field_.y + (field_.h - 32) / 2;
    const int x = field_.x + 16;
    text(x, ty, edit_, 1, kWhite, kKey);
    const int cur = x + static_cast<int>(edit_.size()) * 16;
    // The cursor, then the extension that's added
    if (static_cast<int>(edit_.size()) < kLifNameMax)
        d_->fillRect(static_cast<std::int16_t>(cur + 1), static_cast<std::int16_t>(ty), 14, 32, kYellow);
    text(cur + 18, ty, ".DAT", 1, kGrey, kKey);
    char b[16];
    std::snprintf(b, sizeof(b), "%2d / %d", static_cast<int>(edit_.size()), kLifNameMax);
    text(field_.x + field_.w - 16 - 7 * 8, field_.y + (field_.h - 16) / 2, b, 0,
         static_cast<int>(edit_.size()) >= kLifNameMax ? kYellow : kGrey, kKey);
}

void MediaCreateUi::drawKey(const Key& k, bool pressed) {
    std::uint16_t fill = kKey, outline = kGrey, fg = kWhite;
    if (k.code == kOk) { fill = kYellow; outline = kYellow; fg = kBlack; }
    else if (k.code == kCancel || k.code == kDel) fill = kFnKey;
    if (pressed) { fill = kPress; outline = kYellow; fg = kWhite; }
    box(k.r, outline, fill, 9);
    if (k.code == kDel) {
        // A left arrow, like the strip's
        const int cx = k.r.x + k.r.w / 2, cy = k.r.y + k.r.h / 2;
        d_->fillTriangle(static_cast<std::int16_t>(cx - 22), static_cast<std::int16_t>(cy), static_cast<std::int16_t>(cx - 6),
                         static_cast<std::int16_t>(cy - 12), static_cast<std::int16_t>(cx - 6), static_cast<std::int16_t>(cy + 12), fg);
        d_->fillRect(static_cast<std::int16_t>(cx - 6), static_cast<std::int16_t>(cy - 5), 28, 10, fg);
        return;
    }
    const std::string label = k.code == kOk ? "OK" : k.code == kCancel ? "Cancel" : std::string(1, k.code);
    textCentered(k.r, label, 1, fg, fill);
}

}  // namespace hipi
