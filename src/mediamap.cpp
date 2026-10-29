// mediamap.cpp -- see mediamap.h
#include "mediamap.h"
#include "lif_create.h"          // the medium types (names)
#include "page_chrome.h"         // the page's X and menu button
#include "lif_types.h"           // the file types
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace hipi {

namespace {

// What a piece of the medium holds. Kinds (Free .. Vol) are what the map
// shows BY TYPE; a file also belongs to a machine (Hp41 .. UnknownMachine),
// what it shows BY MACHINE -- the rest (free, deleted, directory, label)
// is the same in both. The order of importance (a ring step covering
// several shows the most important): see kPriority.
enum Group : std::uint8_t {
    Free, Deleted, Program, Data, Text, State, RomLex, Other, Dir, Vol,
    Hp41, Hp71, Hp75, AnyMachine, UnknownMachine, kGroups
};
constexpr int kKinds = Vol + 1;
constexpr std::uint8_t kPriority[kKinds] = { 0, 1, 3, 2, 2, 2, 2, 2, 4, 4 };

constexpr std::uint16_t kBlack = 0x0000, kWhite = 0xFFFF, kYellow = 0xEDC0, kGrey = 0x7BEF;
constexpr std::uint16_t kGreen = 0x0640, kOrange = 0xF4A5;
constexpr std::uint16_t kDim = 0x2945;      // not the highlighted group
constexpr std::uint16_t kColour[kGroups] = {
    0x1AA7,   // free: a dark green
    0x9186,   // deleted
    0x5E4D,   // programs
    0x555C,   // data
    0xE68F,   // text
    0xE467,   // status, keys, write all
    0xB3DB,   // ROM, LEX
    0x8C71,   // other
    0xEDC0,   // directory
    0x7BEF,   // volume label
    0x5E4D,   // HP-41
    0x555C,   // HP-71
    0xE467,   // HP-75
    0xE68F,   // any machine (TEXT)
    0x8C71,   // unknown type
};

// The rows of the BY TYPE / BY MACHINE card: a group and its name
struct Row { Group g; const char* label; };
constexpr Row kTypeRows[] = { { Program, "programs" }, { Data, "data" }, { Text, "text" },
                              { State, "status, keys, WA" }, { RomLex, "ROM, LEX" }, { Other, "other" },
                              { Deleted, "deleted" }, { Free, "free" }, { Dir, "directory, label" } };
constexpr Row kMachineRows[] = { { Hp41, "HP-41" }, { Hp71, "HP-71" }, { Hp75, "HP-75" },
                                 { AnyMachine, "any machine (TEXT)" }, { UnknownMachine, "unknown type" },
                                 { Deleted, "deleted" }, { Free, "free" }, { Dir, "directory, label" } };
constexpr int kTypeRowCount = sizeof(kTypeRows) / sizeof(kTypeRows[0]);
constexpr int kMachineRowCount = sizeof(kMachineRows) / sizeof(kMachineRows[0]);
constexpr int kRowH = 24;                 // (the cards end above the page's menu button)

constexpr int kTitleH = 26;
constexpr double kPi = 3.14159265358979323846;

// ── The medium, from its LIF directory ─────────────────────────────────

struct Extent {
    std::uint32_t start = 0, len = 0;
    Group kind = Free;
    Group machine = Free;               // a file's machine; else the same as kind
    std::uint16_t type = 0;
    char name[11] = {};
};

struct Model {
    bool ok = false;
    std::string error;
    std::uint32_t total = 0;
    std::uint16_t tracks = 0, heads = 0, sectors = 0;
    std::uint32_t dirStart = 0, dirLen = 0, entriesUsed = 0;
    std::vector<Extent> ext;            // the volume header, the directory, the files
    std::uint32_t blocks[kGroups] = {};
    std::uint32_t lastEnd = 0;          // after the last file: where new files go
    int files = 0, deleted = 0;
    std::uint32_t deletedBlocks = 0;
};

std::uint32_t be16(const std::uint8_t* p) { return (p[0] << 8) | p[1]; }
std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

Group kindOf(std::uint16_t type) {
    const LifType* t = lifType(type);
    if (t == nullptr) return Other;
    switch (t->sort) {
        case LifSort::Program: return Program;
        case LifSort::Data:    return Data;
        case LifSort::Text:    return Text;
        case LifSort::State:   return State;
        case LifSort::RomLex:  return RomLex;
        default:               return Other;
    }
}
Group machineOf(std::uint16_t type) {
    const LifType* t = lifType(type);
    if (t == nullptr) return UnknownMachine;
    switch (t->machine) {
        case LifMachine::Hp41: return Hp41;
        case LifMachine::Hp71: return Hp71;
        case LifMachine::Hp75: return Hp75;
        case LifMachine::Any:  return AnyMachine;
        default:               return UnknownMachine;
    }
}

Model readModel(const std::function<bool(std::uint32_t, std::uint8_t*)>& read) {
    Model m;
    std::uint8_t b[256];
    if (!read || !read(0, b)) { m.error = "Can't read the medium"; return m; }
    if (be16(b) != 0x8000) { m.error = "Not a LIF volume"; return m; }
    m.tracks = static_cast<std::uint16_t>(be32(b + 24));
    m.heads = static_cast<std::uint16_t>(be32(b + 28));
    m.sectors = static_cast<std::uint16_t>(be32(b + 32));
    m.total = static_cast<std::uint32_t>(m.tracks) * m.heads * m.sectors;
    if (m.total == 0 || m.total > 1000000) {             // no geometry: a cassette
        m.tracks = 2; m.heads = 1; m.sectors = 256; m.total = 512;
    }
    m.dirStart = be32(b + 8);
    m.dirLen = be32(b + 16);
    if (m.dirStart == 0 || m.dirStart >= m.total) { m.error = "Not a LIF volume"; return m; }
    m.dirLen = std::min(m.dirLen, m.total - m.dirStart);
    Extent vol; vol.start = 0; vol.len = m.dirStart; vol.kind = vol.machine = Vol;
    Extent dir; dir.start = m.dirStart; dir.len = m.dirLen; dir.kind = dir.machine = Dir;
    m.ext.push_back(vol);
    m.ext.push_back(dir);
    m.lastEnd = m.dirStart + m.dirLen;
    // The entries, up to the end mark (type 0xFFFF)
    bool end = false;
    const std::uint32_t maxDir = std::min<std::uint32_t>(m.dirLen, 4096);
    for (std::uint32_t blk = 0; blk < maxDir && !end; ++blk) {
        if (!read(m.dirStart + blk, b)) { m.error = "Can't read the directory"; return m; }
        for (int i = 0; i < 8; ++i) {
            const std::uint8_t* e = b + i * 32;
            const std::uint32_t type = be16(e + 10);
            if (type == 0xFFFF) { end = true; break; }
            ++m.entriesUsed;
            Extent x;
            x.start = be32(e + 12);
            x.len = be32(e + 16);
            x.type = static_cast<std::uint16_t>(type);
            std::memcpy(x.name, e, 10);
            for (int k = 9; k >= 0 && x.name[k] == ' '; --k) x.name[k] = 0;
            x.kind = type == 0 ? Deleted : kindOf(x.type);
            x.machine = type == 0 ? Deleted : machineOf(x.type);
            if (x.start >= m.total) continue;
            x.len = std::min(x.len, m.total - x.start);
            if (x.kind == Deleted) { ++m.deleted; m.deletedBlocks += x.len; }
            else ++m.files;
            m.lastEnd = std::max(m.lastEnd, x.start + x.len);
            m.ext.push_back(x);
        }
    }
    std::uint32_t covered = 0;
    for (const Extent& x : m.ext) {
        m.blocks[x.kind] += x.len;
        if (x.machine != x.kind) m.blocks[x.machine] += x.len;
        covered += x.len;
    }
    m.blocks[Free] = covered < m.total ? m.total - covered : 0;
    m.ok = true;
    return m;
}

// ── State ───────────────────────────────────────────────────────────────

DisplayDriver* d_ = nullptr;
std::function<bool(MediaMapLive&)> live_;                 // in use: the drive's, or the demo's
std::function<bool(std::uint32_t, std::uint8_t*)> read_;
std::function<bool(MediaMapLive&)> driveLive_;            // the drive's (mediamap_setSource())
std::function<bool(std::uint32_t, std::uint8_t*)> driveRead_;
bool demoSource_ = false;
Model model_;
std::string file_;
bool haveDrive_ = false;

// Rings: rings_ of steps_ steps, bpr_ blocks per ring; per step the kind
// and machine of the most important thing in it
int rings_ = 2, steps_ = 256;
std::uint32_t bpr_ = 256;
std::vector<std::uint8_t> kinds_, machines_;

// What the map shows: BY TYPE or BY MACHINE, and one group highlighted
// (a tap on its row: the rest dimmed), -1 = none
bool byMachine_ = false;
int highlight_ = -1;

std::uint8_t stepGroup(std::size_t g) { return byMachine_ ? machines_[g] : kinds_[g]; }
bool highlighted(std::uint8_t group) {
    return highlight_ < 0 || group == highlight_ || (highlight_ == Dir && group == Vol);
}
std::uint16_t groupColour(std::uint8_t group) { return highlighted(group) ? kColour[group] : kDim; }
std::uint16_t stepColour(std::size_t g) { return groupColour(stepGroup(g)); }
// Where one run of a ring ends: the kind or the machine changes. The same
// in every mode and highlight, so a ring redrawn in other colours covers
// exactly the same pixels (runs cut elsewhere left old pixels at the
// triangles' edges)
bool sameRun(std::size_t a, std::size_t b) { return kinds_[a] == kinds_[b] && machines_[a] == machines_[b]; }

// Layout
int cx_ = 0, cy_ = 0, rOut_ = 0, rIn_ = 0, band_ = 0, ringGap_ = 6, rCentre_ = 0;
int panelX_ = 0, panelW_ = 0;
struct Box { int x, y, w, h; };
Box typeCard_{}, mediumCard_{};

// Live
MediaMapLive shown_;                    // what's drawn of the head
bool headShown_ = false;
std::uint32_t lastWrites_ = 0;
bool reloadPending_ = false;
std::uint32_t writesSeenAt_ = 0;
std::uint32_t nextRetry_ = 0;      // not LIF: when to look again
std::uint32_t nextPoll_ = 0;
// The tapped file's box (instead of MEDIUM) until fileUntil_
int fileShown_ = -1;                    // index in model_.ext, -2 = free space
// Its box's "Save on PC" (mediamap_setSaveOnPc()): shown while hipiview is
// connected; after a tap, what came of it instead
std::function<bool()> saveAvailable_;
std::function<bool(const std::string&, std::string&)> saveFile_;
bool saveShown_ = false;
int saveX_ = 0, saveY_ = 0;
constexpr int kSaveW = 150, kSaveH = 34;
std::string saveMsg_;
std::uint32_t fileUntil_ = 0;
bool fileTimed_ = false;

bool cassette() { return model_.sectors == 256 && model_.tracks * model_.heads <= 4; }
const char* unitWord() { return cassette() ? "records" : "blocks"; }

// ── Geometry ────────────────────────────────────────────────────────────

int stepOf(std::uint32_t block) {
    const std::uint32_t r = block / bpr_;
    const std::uint32_t s = static_cast<std::uint32_t>((static_cast<std::uint64_t>(block % bpr_) * static_cast<std::uint32_t>(steps_)) / bpr_);
    return static_cast<int>(r * static_cast<std::uint32_t>(steps_) + s);
}
int ringOuter(int ring) { return rOut_ - ring * band_; }
int ringInner(int ring) { return ringOuter(ring) - band_ + ringGap_; }
double angleOf(double step) { return -kPi / 2 + step / steps_ * 2 * kPi; }

void layout() {
    rOut_ = (SCREEN_MAX_Y - kTitleH - 24) / 2;
    cx_ = 20 + rOut_;
    cy_ = kTitleH + 14 + rOut_;
    panelX_ = cx_ + rOut_ + 28;
    panelW_ = SCREEN_MAX_X - panelX_ - 12;
    typeCard_ = { panelX_, kTitleH + 20, panelW_, 22 + kTypeRowCount * kRowH + 32 };
    mediumCard_ = { panelX_, typeCard_.y + typeCard_.h + 22, panelW_, kPageContentBottom - (typeCard_.y + typeCard_.h + 22) };
}

void buildSteps() {
    rings_ = cassette() ? model_.tracks * model_.heads : 8;
    rings_ = std::max(rings_, 1);
    bpr_ = (model_.total + static_cast<std::uint32_t>(rings_) - 1) / static_cast<std::uint32_t>(rings_);
    steps_ = static_cast<int>(std::min<std::uint32_t>(bpr_, 400));
    kinds_.assign(static_cast<std::size_t>(rings_ * steps_), Free);
    machines_.assign(kinds_.size(), Free);
    for (const Extent& x : model_.ext) {
        if (x.len == 0) continue;
        const int g0 = stepOf(x.start), g1 = stepOf(x.start + x.len - 1);
        for (int g = g0; g <= g1 && g < static_cast<int>(kinds_.size()); ++g)
            if (kPriority[x.kind] > kPriority[kinds_[static_cast<std::size_t>(g)]]) {
                kinds_[static_cast<std::size_t>(g)] = x.kind;
                machines_[static_cast<std::size_t>(g)] = x.machine;
            }
    }
    rIn_ = cassette() ? rOut_ * 45 / 100 : rOut_ * 27 / 100;
    ringGap_ = cassette() ? 10 : 6;
    band_ = (rOut_ - rIn_) / rings_;
    rCentre_ = rIn_ - 8;
}

// ── Drawing ─────────────────────────────────────────────────────────────

std::int16_t i16(double v) { return static_cast<std::int16_t>(std::lround(v)); }

void text(int x, int y, const std::string& s, int scale, std::uint16_t fg, std::uint16_t bg) {
    d_->selectBuiltinFont();
    d_->txtSize(static_cast<std::uint8_t>(scale));
    d_->txtColor(fg, bg);
    d_->txtSetCursor(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
    d_->txtWrite(s.c_str());
}
void textCentred(int cx, int y, const std::string& s, int scale, std::uint16_t fg, std::uint16_t bg) {
    text(cx - static_cast<int>(s.size()) * 8 * (scale + 1) / 2, y, s, scale, fg, bg);
}

// A piece of a ring, from angle a0 to a1 (radians), in triangles
void fillArc(int r0, int r1, double a0, double a1, std::uint16_t col) {
    const int pieces = std::max(1, static_cast<int>(std::ceil((a1 - a0) / 0.05)));
    for (int i = 0; i < pieces; ++i) {
        const double b0 = a0 + (a1 - a0) * i / pieces, b1 = a0 + (a1 - a0) * (i + 1) / pieces;
        const double c0 = std::cos(b0), s0 = std::sin(b0), c1 = std::cos(b1), s1 = std::sin(b1);
        const std::int16_t ox0 = i16(cx_ + r1 * c0), oy0 = i16(cy_ + r1 * s0), ox1 = i16(cx_ + r1 * c1), oy1 = i16(cy_ + r1 * s1);
        const std::int16_t ix0 = i16(cx_ + r0 * c0), iy0 = i16(cy_ + r0 * s0), ix1 = i16(cx_ + r0 * c1), iy1 = i16(cy_ + r0 * s1);
        d_->fillTriangle(ox0, oy0, ox1, oy1, ix0, iy0, col);
        d_->fillTriangle(ix0, iy0, ox1, oy1, ix1, iy1, col);
    }
}

// Where one file ends and the next begins inside a run of one kind (several
// programs after each other, that looked like one): a hair in a shade of
// the run's own colour, a little darker (a little lighter on a dark one) --
// seen when looked for, without turning a medium of many small files into
// a pattern: one closer than kMinBoundarySteps to the last one drawn, or to
// a change of kind (a black hair already), is left out.
constexpr double kMinBoundarySteps = 2.0;
std::uint16_t boundaryShade(std::uint16_t c) {
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    if (r * 2 + g + b * 2 < 60) {                          // dark: a little lighter
        r += (31 - r) / 4; g += (63 - g) / 4; b += (31 - b) / 4;
    } else {
        r = r * 3 / 4; g = g * 3 / 4; b = b * 3 / 4;
    }
    return static_cast<std::uint16_t>((r << 11) | (g << 5) | b);
}
// The boundaries in a ring, as steps (fractions: where in its step the file
// starts), steps from `from` to `to` only
void drawBoundaries(int ring, double from = -1.0, double to = 1e9) {
    if (bpr_ == 0) return;
    const int r1 = ringOuter(ring), r0 = ringInner(ring);
    const std::size_t base = static_cast<std::size_t>(ring * steps_);
    std::vector<std::pair<double, std::size_t>> at;          // (step, step index)
    for (const Extent& x : model_.ext) {
        if (x.kind == Vol || x.kind == Dir || x.kind == Free || x.start == 0 || x.start >= model_.total) continue;
        if (x.start / bpr_ != static_cast<std::uint32_t>(ring) || x.start % bpr_ == 0) continue;
        const int g = stepOf(x.start), gp = stepOf(x.start - 1);
        if (!sameRun(static_cast<std::size_t>(g), static_cast<std::size_t>(gp))) continue;   // (a cut there already)
        at.emplace_back(static_cast<double>(x.start % bpr_) * steps_ / bpr_, static_cast<std::size_t>(g));
    }
    std::sort(at.begin(), at.end());
    double last = -1e9;
    for (const auto& p : at) {
        const double s = p.first;
        if (s - last < kMinBoundarySteps) continue;
        // Not right beside a change of kind
        bool nearCut = false;
        for (int c = static_cast<int>(s) - 1; c <= static_cast<int>(s) + 2 && !nearCut; ++c) {
            if (c <= 0 || c >= steps_) continue;
            if (!sameRun(base + static_cast<std::size_t>(c - 1), base + static_cast<std::size_t>(c)) && std::fabs(c - s) < 1.0)
                nearCut = true;
        }
        if (nearCut) continue;
        last = s;
        if (s < from || s > to) continue;
        fillArc(r0, r1, angleOf(s) - 0.002, angleOf(s) + 0.002, boundaryShade(stepColour(p.second)));
    }
}

// A ring, straight over what's there (no clearing first: it would flash):
// each run of one kind, then a hair of black where the kind changes, and
// the files' boundaries within a run (drawBoundaries())
void drawRing(int ring) {
    const int r1 = ringOuter(ring), r0 = ringInner(ring);
    const std::size_t base = static_cast<std::size_t>(ring * steps_);
    std::vector<int> cuts;
    int s0 = 0;
    for (int s = 1; s <= steps_; ++s) {
        if (s < steps_ && sameRun(base + static_cast<std::size_t>(s), base + static_cast<std::size_t>(s0))) continue;
        fillArc(r0, r1, angleOf(s0), angleOf(s), stepColour(base + static_cast<std::size_t>(s0)));
        cuts.push_back(s);
        s0 = s;
    }
    if (cuts.size() > 1)
        for (int c : cuts) fillArc(r0, r1, angleOf(c) - 0.0015, angleOf(c) + 0.0015, kBlack);
    drawBoundaries(ring);
}

const char* opWord(MediaOp op) {
    switch (op) {
        case MediaOp::Read:  return "READ";
        case MediaOp::Write: return "WRITE";
        case MediaOp::Seek:  return "SEEK";
        default:             return "IDLE";
    }
}
std::uint16_t opColour(MediaOp op) {
    return op == MediaOp::Read ? kGreen : op == MediaOp::Write ? kOrange : op == MediaOp::Seek ? kWhite : kGrey;
}

// The middle: what the drive does where -- centred
// s centred in a field of n characters (spaces around it): drawn with a
// black background, it covers whatever the field held before
// (the text itself centred on cx, the spaces around it as they fall)
void textField(int cx, int y, const std::string& s, std::size_t n, int scale, std::uint16_t fg) {
    const int cw = 8 * (scale + 1);
    const std::string t = s.size() > n ? s.substr(0, n) : s;
    const std::size_t left = (n - t.size()) / 2;
    const std::string padded = std::string(left, ' ') + t + std::string(n - t.size() - left, ' ');
    text(cx - static_cast<int>(t.size()) * cw / 2 - static_cast<int>(left) * cw, y, padded, scale, fg, kBlack);
}

// The middle: what the drive does where -- centred. full: the ring
// around it too (else only the text, written over itself: no flashing)
void drawCentre(bool full = false) {
    if (full) {
        d_->fillCircle(i16(cx_), i16(cy_), static_cast<std::uint16_t>(rCentre_), kYellow);
        d_->fillCircle(i16(cx_), i16(cy_), static_cast<std::uint16_t>(rCentre_ - 2), kBlack);
    }
    char l2[24] = "", l3[24] = "";
    const bool idle = shown_.op == MediaOp::None;
    if (!idle && cassette()) {
        std::snprintf(l2, sizeof(l2), "track %u", static_cast<unsigned>(shown_.head / model_.sectors));
        std::snprintf(l3, sizeof(l3), "record %u", static_cast<unsigned>(shown_.head % model_.sectors));
    } else if (!idle) {
        std::snprintf(l2, sizeof(l2), "block");
        std::snprintf(l3, sizeof(l3), "%u", static_cast<unsigned>(shown_.head));
    }
    // Idle: the one word in the middle; else three lines. Between the two
    // the middle is cleared first.
    static bool wasIdle = true;
    if (!full && idle != wasIdle)
        d_->fillCircle(i16(cx_), i16(cy_), static_cast<std::uint16_t>(rCentre_ - 2), kBlack);
    wasIdle = idle;
    if (idle) {
        textField(cx_, cy_ - 16, opWord(shown_.op), 6, 1, opColour(shown_.op));
        return;
    }
    const int h = 32 + 8 + 16 + 4 + 16;
    const int y = cy_ - h / 2;
    textField(cx_, y, opWord(shown_.op), 6, 1, opColour(shown_.op));
    textField(cx_, y + 40, l2, 12, 0, kWhite);
    textField(cx_, y + 60, l3, 12, 0, kGrey);
}

// The head: across its ring, and a marker on the rim in the operation's colour
void rimMarker(double a, std::uint16_t col) {
    const double c = std::cos(a), s = std::sin(a), px = -s, py = c;
    const double tip = rOut_ + 3, base = rOut_ + 12;
    d_->fillTriangle(i16(cx_ + tip * c), i16(cy_ + tip * s), i16(cx_ + base * c + 6 * px), i16(cy_ + base * s + 6 * py),
                     i16(cx_ + base * c - 6 * px), i16(cy_ + base * s - 6 * py), col);
}
double headAngle(std::uint32_t block) {
    const int g = stepOf(std::min(block, model_.total - 1));
    return angleOf((g % steps_) + 0.5);
}
// The head's line across its ring, in `col`: white to show it, the
// colour of the step under it to take it away again -- the very same
// pixels, so nothing is left behind (repainting the ring with triangles
// left single pixels of the old line at the triangles' edges)
void headLine(std::uint32_t block, std::uint16_t col) {
    const int ring = stepOf(std::min(block, model_.total - 1)) / steps_;
    const double a = headAngle(block);
    const double c = std::cos(a), s = std::sin(a);
    const int r0 = ringInner(ring) + 1, r1 = ringOuter(ring) - 1;
    for (int t = -1; t <= 1; ++t) {
        const double ox = -s * t, oy = c * t;
        d_->line(i16(cx_ + r0 * c + ox), i16(cy_ + r0 * s + oy), i16(cx_ + r1 * c + ox), i16(cy_ + r1 * s + oy), col);
    }
}
void drawHead() {
    if (shown_.op == MediaOp::None || model_.total == 0) return;
    headLine(shown_.head, kWhite);
    rimMarker(headAngle(shown_.head), opColour(shown_.op));
}
void eraseHead() {
    if (!headShown_ || model_.total == 0) return;
    const int g = stepOf(std::min(shown_.head, model_.total - 1));
    headLine(shown_.head, stepColour(static_cast<std::size_t>(g)));
    rimMarker(headAngle(shown_.head), kBlack);
    // On a narrow step the line reached the steps beside it: those again
    const int ring = g / steps_;
    const int r1 = ringOuter(ring), r0 = ringInner(ring);
    auto idx = [&](int step) { return static_cast<std::size_t>(ring * steps_ + (step + steps_) % steps_); };
    for (int k = g % steps_ - 1; k <= g % steps_ + 1; ++k) {
        const double gl = !sameRun(idx(k - 1), idx(k)) ? 0.0015 : 0.0, gr = !sameRun(idx(k + 1), idx(k)) ? 0.0015 : 0.0;
        fillArc(r0, r1, angleOf(k) + gl, angleOf(k + 1) - gr, stepColour(idx(k)));
    }
    drawBoundaries(ring, g % steps_ - 1, g % steps_ + 2);
}

void card(const Box& c, const char* name) {
    d_->roundRect(i16(c.x), i16(c.y), i16(c.w), i16(c.h), 12, kYellow);
    d_->roundRect(i16(c.x + 1), i16(c.y + 1), i16(c.w - 2), i16(c.h - 2), 11, kYellow);
    if (name == nullptr) return;           // tabs of its own
    const int w = static_cast<int>(std::strlen(name)) * 8 + 16;
    d_->fillRect(i16(c.x + 20), i16(c.y - 9), i16(w), 18, kYellow);
    text(c.x + 28, c.y - 8, name, 0, kBlack, kYellow);
}
void clearInside(const Box& c) {
    d_->fillRect(i16(c.x + 6), i16(c.y + 12), i16(c.w - 12), i16(c.h - 18), kBlack);
}

// The card's two tabs: BY TYPE and BY MACHINE, the one shown filled
int tabW(const char* name) { return static_cast<int>(std::strlen(name)) * 8 + 16; }
constexpr const char* kTabType = "BY TYPE";
constexpr const char* kTabMachine = "BY MACHINE";
int tabMachineX() { return typeCard_.x + 20 + tabW(kTabType) + 8; }
void drawTab(int x, const char* name, bool on) {
    const int y = typeCard_.y - 9, w = tabW(name);
    d_->fillRect(i16(x), i16(y), i16(w), 18, on ? kYellow : kBlack);
    if (!on) d_->rect(i16(x), i16(y), i16(w), 18, kYellow);
    text(x + 8, y + 1, name, 0, on ? kBlack : kYellow, on ? kYellow : kBlack);
}

int rowCount() { return byMachine_ ? kMachineRowCount : kTypeRowCount; }
const Row& rowAt(int i) { return byMachine_ ? kMachineRows[i] : kTypeRows[i]; }

void drawTypes() {
    clearInside(typeCard_);
    drawTab(typeCard_.x + 20, kTabType, !byMachine_);
    drawTab(tabMachineX(), kTabMachine, byMachine_);
    const int x = typeCard_.x + 16;
    int y = typeCard_.y + 22;
    const int barX = x + 268, barW = typeCard_.x + typeCard_.w - 16 - barX;
    for (int i = 0; i < rowCount(); ++i) {
        const Row& r = rowAt(i);
        const bool marked = highlight_ == r.g;
        const std::uint32_t n = model_.blocks[r.g] + (r.g == Dir ? model_.blocks[Vol] : 0);
        if (marked) d_->roundRect(i16(x - 8), i16(y - 5), i16(typeCard_.w - 16), kRowH, 6, kYellow);
        d_->fillRect(i16(x), i16(y + 2), 12, 12, groupColour(r.g));
        if (r.g == Free) d_->rect(i16(x), i16(y + 2), 12, 12, kGrey);
        text(x + 22, y, r.label, 0, marked ? kYellow : highlighted(r.g) ? kWhite : kGrey, kBlack);
        char b[16];
        std::snprintf(b, sizeof(b), "%7lu", static_cast<unsigned long>(n));
        text(x + 196, y, b, 0, highlighted(r.g) ? kWhite : kGrey, kBlack);
        if (barW > 8 && r.g != Dir) {
            d_->fillRect(i16(barX), i16(y + 4), i16(barW), 8, 0x2104);
            const int w = static_cast<int>(static_cast<std::uint64_t>(barW) * n / std::max<std::uint32_t>(model_.total, 1));
            if (w > 0) d_->fillRect(i16(barX), i16(y + 4), i16(w), 8, highlighted(r.g) ? kColour[r.g] : kGrey);
        }
        y += kRowH;
    }
    y = typeCard_.y + 22 + kTypeRowCount * kRowH;
    text(x, y + 2, cassette() ? "records of 256 bytes" : "blocks of 256 bytes", 0, kGrey, kBlack);
    const char* hint = highlight_ >= 0 ? "tap again: all" : "tap a row: find it";
    text(typeCard_.x + typeCard_.w - 16 - static_cast<int>(std::strlen(hint)) * 8, y + 2, hint, 0, kGrey, kBlack);
}

// A small padlock (a secure or private file)
void padlock(int x, int y, std::uint16_t col) {
    d_->roundRect(i16(x + 2), i16(y), 8, 10, 3, col);
    d_->roundRect(i16(x + 3), i16(y + 1), 6, 8, 2, col);
    d_->fillRect(i16(x), i16(y + 6), 12, 9, col);
    d_->fillRect(i16(x + 5), i16(y + 9), 2, 3, kBlack);
}

void drawMedium() {
    clearInside(mediumCard_);
    saveShown_ = false;
    const int x = mediumCard_.x + 16;
    int y = mediumCard_.y + 22;
    auto row = [&](const char* k, const std::string& v, std::uint16_t col = kWhite) {
        text(x, y, k, 0, kGrey, kBlack);
        text(x + 112, y, v, 0, col, kBlack);
        y += kRowH;
    };
    char b[64];
    if (fileShown_ != -1) {
        // The tapped file
        d_->fillRect(i16(mediumCard_.x + 20), i16(mediumCard_.y - 9), 120, 18, kBlack);
        card(mediumCard_, "FILE");
        if (fileShown_ == -2) {
            row("free", "room for new files");
            std::snprintf(b, sizeof(b), "%lu %s", static_cast<unsigned long>(model_.total - model_.lastEnd), unitWord());
            row("size", b);
            return;
        }
        const Extent& e = model_.ext[static_cast<std::size_t>(fileShown_)];
        const std::uint8_t g = byMachine_ ? e.machine : e.kind;
        const std::uint16_t col = g == Deleted || g == Dir || g == Vol ? kWhite : kColour[g];
        if (e.kind == Dir) row("type", "the directory");
        else if (e.kind == Vol) row("type", "the volume label");
        else {
            row("name", e.name[0] ? e.name : "(no name)", kYellow);
            if (e.kind == Deleted) {
                row("type", "deleted (PACK frees it)");
            } else {
                // The type's name, a padlock if secure/private, its code
                const LifType* t = lifType(e.type);
                const std::string name = t != nullptr ? t->name : "?";
                text(x, y, "type", 0, kGrey, kBlack);
                text(x + 112, y, name, 0, col, kBlack);
                int nx = x + 112 + static_cast<int>(name.size()) * 8 + 12;
                const bool locked = t != nullptr && (t->secure || t->priv);
                if (locked) { padlock(nx, y, kYellow); nx += 22; }
                std::snprintf(b, sizeof(b), "%04X", e.type);
                text(nx, y, b, 0, kGrey, kBlack);
                y += kRowH;
                row("", t != nullptr ? t->what : "unknown type");
                if (locked) row("", t->secure && t->priv ? "secure + private" : t->secure ? "secure" : "private");
            }
        }
        if (cassette()) std::snprintf(b, sizeof(b), "track %lu record %lu", static_cast<unsigned long>(e.start / model_.sectors),
                                      static_cast<unsigned long>(e.start % model_.sectors));
        else std::snprintf(b, sizeof(b), "block %lu", static_cast<unsigned long>(e.start));
        row("at", b);
        std::snprintf(b, sizeof(b), "%lu %s", static_cast<unsigned long>(e.len), unitWord());
        row("size", b);
        // A file: Save on PC (hipiview connected) -- or what came of it
        saveShown_ = false;
        const bool aFile = e.kind != Dir && e.kind != Vol && e.kind != Deleted && e.name[0] != 0;
        const int by = mediumCard_.y + mediumCard_.h - 16 - kSaveH;
        if (aFile && !saveMsg_.empty()) {
            const std::size_t n = static_cast<std::size_t>((mediumCard_.w - 32) / 8);
            text(x, by + 9, saveMsg_.size() > n ? saveMsg_.substr(0, n) : saveMsg_, 0, kYellow, kBlack);
        } else if (aFile && saveAvailable_ && saveFile_ && saveAvailable_()) {
            saveX_ = mediumCard_.x + mediumCard_.w - 16 - kSaveW;
            saveY_ = by;
            d_->fillRoundRect(i16(saveX_), i16(saveY_), kSaveW, kSaveH, 8, kYellow);
            text(saveX_ + (kSaveW - 10 * 8) / 2, saveY_ + (kSaveH - 16) / 2, "Save on PC", 0, kBlack, kYellow);
            saveShown_ = true;
        }
        return;
    }
    d_->fillRect(i16(mediumCard_.x + 20), i16(mediumCard_.y - 9), 120, 18, kBlack);
    card(mediumCard_, "MEDIUM");
    const std::uint64_t bytes = static_cast<std::uint64_t>(model_.total) * 256;
    if (bytes >= 1024 * 1024) std::snprintf(b, sizeof(b), "%lu %s, %lu MB", static_cast<unsigned long>(model_.total), unitWord(),
                                            static_cast<unsigned long>((bytes + 512 * 1024) / (1024 * 1024)));
    else std::snprintf(b, sizeof(b), "%lu %s, %lu kB", static_cast<unsigned long>(model_.total), unitWord(),
                       static_cast<unsigned long>(bytes / 1024));
    row("size", b);
    if (model_.deleted > 0) std::snprintf(b, sizeof(b), "%d  (+%d deleted)", model_.files, model_.deleted);
    else std::snprintf(b, sizeof(b), "%d", model_.files);
    row("files", b);
    const std::uint32_t free = model_.total > model_.lastEnd ? model_.total - model_.lastEnd : 0;
    std::snprintf(b, sizeof(b), "%lu %s  %lu %%", static_cast<unsigned long>(free), unitWord(),
                  static_cast<unsigned long>(static_cast<std::uint64_t>(free) * 100 / std::max<std::uint32_t>(model_.total, 1)));
    row("free", b);
    std::snprintf(b, sizeof(b), "%lu of %lu entries", static_cast<unsigned long>(model_.entriesUsed),
                  static_cast<unsigned long>(model_.dirLen * 8));
    row("directory", b);
    if (model_.deletedBlocks > 0) {
        std::snprintf(b, sizeof(b), "PACK would free %lu %s", static_cast<unsigned long>(model_.deletedBlocks), unitWord());
        text(x, y + 2, b, 0, kGrey, kBlack);
    }
}

void drawTitle() {
    d_->fillRect(0, 0, SCREEN_MAX_X, kTitleH, kYellow);
    text(8, 5, "MEDIA MAP", 0, kBlack, kYellow);
    std::string info = file_.empty() ? std::string("no medium") : file_;
    if (model_.ok) {
        const char* kind = "LIF medium";
        for (const LifMediaType& t : kLifMediaTypes)
            if (t.tracks == model_.tracks && t.heads == model_.heads && t.sectors == model_.sectors) kind = t.name;
        char b[96];
        if (cassette()) std::snprintf(b, sizeof(b), "   %s   %d tracks = %d rings", kind, rings_, rings_);
        else std::snprintf(b, sizeof(b), "   %s   %d rings of %lu blocks", kind, rings_, static_cast<unsigned long>(bpr_));
        info += b;
    }
    text(140, 5, info, 0, kBlack, kYellow);
    page_drawClose(d_, kTitleH);
}

void drawRingsAndCentre() {
    d_->fillCircle(i16(cx_), i16(cy_), static_cast<std::uint16_t>(rOut_ + 14), kBlack);
    for (int r = 0; r < rings_; ++r) drawRing(r);
    drawCentre(true);
    drawHead();
    headShown_ = shown_.op != MediaOp::None;
}

void load() {
    MediaMapLive l;
    haveDrive_ = live_ && live_(l);
    file_ = haveDrive_ ? l.file : std::string();
    shown_ = l;
    lastWrites_ = l.writes;
    reloadPending_ = false;
    fileShown_ = -1;
    model_ = Model();
    if (!haveDrive_) model_.error = "No drive";
    else if (file_.empty()) model_.error = "No cassette in the drive";
    else model_ = readModel(read_);
    if (model_.ok) buildSteps();
}

}  // namespace

// ── Interface ───────────────────────────────────────────────────────────

void mediamap_setSource(std::function<bool(MediaMapLive&)> live, std::function<bool(std::uint32_t, std::uint8_t*)> read) {
    driveLive_ = std::move(live);
    driveRead_ = std::move(read);
    if (!demoSource_) { live_ = driveLive_; read_ = driveRead_; }
}

void mediamap_setDemoSource(std::function<bool(MediaMapLive&)> live, std::function<bool(std::uint32_t, std::uint8_t*)> read) {
    demoSource_ = true;
    live_ = std::move(live);
    read_ = std::move(read);
}

void mediamap_clearDemoSource() {
    if (!demoSource_) return;
    demoSource_ = false;
    live_ = driveLive_;
    read_ = driveRead_;
}

void mediamap_init(DisplayDriver* display) {
    d_ = display;
    layout();
}

void mediamap_enter() {
    highlight_ = -1;
    load();
}

void mediamap_drawAll() {
    if (d_ == nullptr) return;
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, kBlack);
    drawTitle();
    page_drawMenu(d_);                       // (page_chrome.h: the strip from here)
    if (!model_.ok) {
        textCentred(SCREEN_MAX_X / 2, SCREEN_MAX_Y / 2 - 16, model_.error, 1, kWhite, kBlack);
        return;
    }
    drawRingsAndCentre();
    card(typeCard_, nullptr);
    drawTypes();
    drawMedium();
}

void mediamap_poll(std::uint32_t nowMs) {
    if (d_ == nullptr) return;
    if (static_cast<std::int32_t>(nowMs - nextPoll_) < 0) return;
    nextPoll_ = nowMs + 150;
    MediaMapLive l;
    const bool have = live_ && live_(l);
    // Another medium (or the drive's gone): everything afresh
    if (have != haveDrive_ || (have && l.file != file_)) {
        highlight_ = -1;
        load();
        mediamap_drawAll();
        return;
    }
    // Written to: the directory again once the drive is quiet
    if (l.writes != lastWrites_) {
        lastWrites_ = l.writes;
        reloadPending_ = true;
        writesSeenAt_ = nowMs;
    }
    if (!model_.ok) {
        // Not readable as LIF (yet): while the medium is being formatted
        // the header comes and goes. Read again once the drive has written
        // and is quiet, and now and then anyway (a block or two, cheap) --
        // the map appears as soon as the medium is LIF again.
        if (!have || file_.empty() || l.busy) return;
        const bool written = reloadPending_ && nowMs - writesSeenAt_ > 600;
        if (!written && static_cast<std::int32_t>(nowMs - nextRetry_) < 0) return;
        nextRetry_ = nowMs + 2000;
        const std::string before = model_.error;
        load();
        shown_ = l;
        if (model_.ok || model_.error != before) mediamap_drawAll();
        return;
    }
    if (reloadPending_ && !l.busy && nowMs - writesSeenAt_ > 600) {
        // Only the rings that changed are drawn again (over themselves:
        // no flashing), the head on top
        eraseHead();
        headShown_ = false;
        std::vector<std::uint16_t> before(kinds_.size());
        for (std::size_t g = 0; g < kinds_.size(); ++g) before[g] = stepColour(g);
        const int ringsBefore = rings_, stepsBefore = steps_;
        load();
        shown_ = l;
        if (!model_.ok) { mediamap_drawAll(); return; }
        if (rings_ != ringsBefore || steps_ != stepsBefore) {
            drawRingsAndCentre();
        } else {
            for (int r = 0; r < rings_; ++r) {
                bool same = true;
                for (int k = 0; k < steps_ && same; ++k) {
                    const std::size_t g = static_cast<std::size_t>(r * steps_ + k);
                    same = before[g] == stepColour(g);
                }
                if (!same) drawRing(r);
            }
            drawCentre();
            drawHead();
            headShown_ = shown_.op != MediaOp::None;
        }
        drawTypes();
        drawMedium();
        return;
    }
    // The head
    if (l.head != shown_.head || l.op != shown_.op) {
        eraseHead();
        shown_.head = l.head;
        shown_.op = l.op;
        drawCentre();
        drawHead();
        headShown_ = shown_.op != MediaOp::None;
    }
    // The tapped file's box goes after a while
    if (fileShown_ != -1) {
        if (!fileTimed_) { fileUntil_ = nowMs + 6000; fileTimed_ = true; }
        else if (static_cast<std::int32_t>(nowMs - fileUntil_) >= 0) { fileShown_ = -1; drawMedium(); }
    }
}

namespace {
// The rings again in their (new) colours, over themselves, the head on top
void redrawRings() {
    eraseHead();
    for (int r = 0; r < rings_; ++r) drawRing(r);
    drawHead();
    headShown_ = shown_.op != MediaOp::None;
}
}  // namespace

MediaMapTap mediamap_tap(int x, int y) {
    if (page_closeHit(x, y)) return MediaMapTap::Close;
    if (!model_.ok) return MediaMapTap::None;
    // The file box's Save on PC: the file to hipiview, the answer in its place
    if (saveShown_ && fileShown_ >= 0 && x >= saveX_ && x < saveX_ + kSaveW && y >= saveY_ && y < saveY_ + kSaveH) {
        const Extent& e = model_.ext[static_cast<std::size_t>(fileShown_)];
        std::string why;
        saveMsg_ = saveFile_(e.name, why) ? std::string(e.name) + " goes to hipiview" : why;
        fileTimed_ = false;                   // (the box stays a while longer)
        drawMedium();
        return MediaMapTap::Handled;
    }
    // The card: its tabs (BY TYPE / BY MACHINE) and its rows (highlight one)
    if (x >= typeCard_.x && x < typeCard_.x + typeCard_.w) {
        if (y >= typeCard_.y - 16 && y < typeCard_.y + 14) {
            const bool machine = x >= tabMachineX() - 4;
            if (machine == byMachine_ && highlight_ < 0) return MediaMapTap::Handled;
            byMachine_ = machine;
            highlight_ = -1;
            redrawRings();
            drawTypes();
            if (fileShown_ != -1) drawMedium();
            return MediaMapTap::Handled;
        }
        const int row = (y - (typeCard_.y + 22 - 5)) / kRowH;
        if (y >= typeCard_.y + 22 - 5 && row < rowCount()) {
            const int g = rowAt(row).g;
            highlight_ = highlight_ == g ? -1 : g;
            redrawRings();
            drawTypes();
            return MediaMapTap::Handled;
        }
        return MediaMapTap::None;
    }
    const double dx = x - cx_, dy = y - cy_;
    const double r = std::sqrt(dx * dx + dy * dy);
    if (r > rOut_ + 4 || r < rIn_ - 4) return MediaMapTap::None;
    const int ring = std::clamp(static_cast<int>((rOut_ - r) / band_), 0, rings_ - 1);
    double a = std::atan2(dy, dx) + kPi / 2;
    if (a < 0) a += 2 * kPi;
    const int step = std::clamp(static_cast<int>(a / (2 * kPi) * steps_), 0, steps_ - 1);
    // The blocks of that step, and the most important thing in them
    const std::uint32_t b0 = static_cast<std::uint32_t>(ring) * bpr_ + static_cast<std::uint32_t>((static_cast<std::uint64_t>(step) * bpr_ + static_cast<std::uint32_t>(steps_) - 1) / static_cast<std::uint32_t>(steps_));
    const std::uint32_t b1 = static_cast<std::uint32_t>(ring) * bpr_ + static_cast<std::uint32_t>((static_cast<std::uint64_t>(step + 1) * bpr_ + static_cast<std::uint32_t>(steps_) - 1) / static_cast<std::uint32_t>(steps_));
    int best = -2;
    std::uint8_t bestPri = 0;
    for (std::size_t i = 0; i < model_.ext.size(); ++i) {
        const Extent& e = model_.ext[i];
        if (e.len == 0 || e.start >= b1 || e.start + e.len <= b0) continue;
        if (best == -2 || kPriority[e.kind] > bestPri) { best = static_cast<int>(i); bestPri = kPriority[e.kind]; }
    }
    fileShown_ = best;
    fileTimed_ = false;
    saveMsg_.clear();
    drawMedium();
    return MediaMapTap::Handled;
}

void mediamap_setSaveOnPc(std::function<bool()> available, std::function<bool(const std::string&, std::string&)> save) {
    saveAvailable_ = std::move(available);
    saveFile_ = std::move(save);
}

bool mediamap_savePoint(int& x, int& y) {
    if (!saveShown_) return false;
    x = saveX_ + kSaveW / 2;
    y = saveY_ + kSaveH / 2;
    return true;
}

void mediamap_tabPoint(bool machine, int& x, int& y) {
    x = machine ? tabMachineX() + tabW(kTabMachine) / 2 : typeCard_.x + 20 + tabW(kTabType) / 2;
    y = typeCard_.y;
}

bool mediamap_rowPoint(int row, int& x, int& y) {
    if (row < 0 || row >= rowCount()) return false;
    x = typeCard_.x + 16 + 80;
    y = typeCard_.y + 22 + row * kRowH + 8;
    return true;
}

bool mediamap_blockPoint(std::uint32_t block, int& x, int& y) {
    if (!model_.ok || block >= model_.total) return false;
    const int g = stepOf(block);
    const double a = angleOf((g % steps_) + 0.5);
    const int ring = g / steps_;
    const double r = (ringOuter(ring) + ringInner(ring)) / 2.0;
    x = static_cast<int>(std::lround(cx_ + r * std::cos(a)));
    y = static_cast<int>(std::lround(cy_ + r * std::sin(a)));
    return true;
}

std::uint32_t mediamap_blocksOf(int group) { return group >= 0 && group < kGroups ? model_.blocks[group] : 0; }
bool mediamap_byMachine() { return byMachine_; }
int mediamap_highlight() { return highlight_; }
int mediamap_rings() { return rings_; }

}  // namespace hipi
