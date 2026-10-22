// display_text.h -- the display's output kept as plain text, to be saved
// as a .txt file (Display menu > Text > Save as text, or "ESC # T" from
// the HP-41).
//
// Everything the display gets over HP-IL since start-up (or since "Start
// anew") is kept, filtered to what a reader wants: the characters, one
// line per line, and nothing of the control codes and escape sequences.
// Meant for program listings and other plain output -- forms drawn with
// cursor positioning come out as their pieces of text, one per line.
//
// The filter, for both models (HP 82163 and HP 92198):
//   - CR, LF and CR LF end a line; trailing spaces are dropped. A line
//     the 92198 wraps on screen stays one line.
//   - BS takes back the last character of the line.
//   - Escape sequences are dropped, with their argument bytes (ESC % c r,
//     ESC # V a). Moving the cursor elsewhere (ESC %, ESC H, ESC A/B)
//     ends the line so far; clearing the screen (ESC E, ESC e, ESC [ / ],
//     ESC H ESC J, Clear device) leaves one empty line.
//   - Other control codes are dropped.
//   - Characters: 82163 -- bit 7 is inverse video, so the character is
//     bits 0..6. 92198 -- the same in ASCII mode; in Roman-8 mode (ESC j)
//     the HP Roman-8 characters, as UTF-8.
//
// Kept in a ring of the store's size (1 MB of the display's SDRAM in the
// firmware): when it's full the oldest lines go.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace hipi {

// Where the text is kept: `capacity()` bytes, written and read back in
// pieces at offsets 0 .. capacity()-1
class DisplayTextStore {
public:
    virtual ~DisplayTextStore() = default;
    virtual std::uint32_t capacity() const = 0;
    virtual void write(std::uint32_t offset, const std::uint8_t* data, std::size_t len) = 0;
    virtual void read(std::uint32_t offset, std::uint8_t* data, std::size_t len) = 0;
};

class DisplayText {
public:
    enum class Model : std::uint8_t { Hp82163, Hp92198 };

    explicit DisplayText(DisplayTextStore* store = nullptr) : store_(store) {}
    void setStore(DisplayTextStore* store) { store_ = store; restart(); }
    // The HIPI "ESC # <c> <arg>" commands that have an argument byte (as
    // given to Screen::setExtCommandCallback())
    void setExtArgCmds(const char* cmds) { extArgCmds_ = cmds != nullptr ? cmds : ""; }

    // A byte as the display gets it
    void put(std::uint8_t c, Model model);
    // The display was cleared (Clear device)
    void cleared();
    // Forget everything kept so far
    void restart();

    // The text, oldest first, in pieces (UTF-8, LF line ends); the line
    // being written comes last, as a whole line
    void read(const std::function<void(const char* text, std::size_t len)>& out) const;

    // Lines kept since start-up / restart() (a ring that has run over
    // still counts the lost ones)
    std::uint32_t lines() const { return lines_ + (lineLen_ > 0 ? 1 : 0); }
    bool empty() const { return lines() == 0; }
    // The ring has run over: the oldest lines are gone
    bool overflowed() const { return store_ != nullptr && written_ > store_->capacity(); }

private:
    enum class State : std::uint8_t { Text, Esc, Pos1, Pos2, Ext, ExtArg };
    static constexpr std::size_t kLineMax = 512;      // longer lines are broken here
    static constexpr std::size_t kPendSize = 256;     // written to the store in pieces of this

    void escape(std::uint8_t c, Model model);
    void character(std::uint8_t c, Model model);
    void addUtf8(std::uint32_t cp);
    void endLine();
    void breakLine() { if (lineLen_ > 0) endLine(); }
    void clearedScreen();
    void emit(const char* s, std::size_t n);
    void flushPend();

    DisplayTextStore* store_ = nullptr;
    const char* extArgCmds_ = "";

    State state_ = State::Text;
    bool roman8_ = false;           // 92198: ESC j .. ESC k
    bool monitor_ = false;          // 92198: ESC Y .. ESC Z (everything shown)
    bool afterCr_ = false;          // the last byte was CR: an LF now ends nothing
    bool lastEscH_ = false;         // the last thing was ESC H (ESC H ESC J = clear)
    bool lastLineEmpty_ = true;     // the last line kept was empty (or none yet)

    char line_[kLineMax + 4] = {};
    std::size_t lineLen_ = 0;

    char pend_[kPendSize] = {};
    std::size_t pendLen_ = 0;
    std::uint64_t written_ = 0;     // bytes written to the store, in all
    std::uint32_t lines_ = 0;
};

// ── The firmware's one (display_text_save.cpp) ─────────────────────────
class LT7683;
DisplayText& displayText();          // (display_text.cpp)
// The store: 1 MB of the display's SDRAM (LT7683::kTextLogAddr), once the
// display is up; until then nothing is kept
void displayText_init(LT7683* display);
// Saves the text to the SD card as "text/display_<n>.txt" (the first free
// number). message: the file name, or what went wrong
bool displayText_save(std::string& message);
// "ESC # T" / the menu: saved from the main loop (boardui_poll())
void displayText_request();
bool displayText_takePending();

}  // namespace hipi
