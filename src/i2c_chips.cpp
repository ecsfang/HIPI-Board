// i2c_chips.cpp -- see i2c_chips.h
#include "i2c_chips.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hipi {

namespace {

// Splits a line into words; "quoted text" is one word (without the quotes)
std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> w;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        if (i >= line.size()) break;
        if (line[i] == '"') {
            const std::size_t e = line.find('"', i + 1);
            w.push_back(line.substr(i + 1, (e == std::string::npos ? line.size() : e) - i - 1));
            i = e == std::string::npos ? line.size() : e + 1;
            continue;
        }
        std::size_t e = i;
        while (e < line.size() && line[e] != ' ' && line[e] != '\t' && line[e] != '\r') ++e;
        w.push_back(line.substr(i, e - i));
        i = e;
    }
    return w;
}

// The line without its comment ('#' outside quotes)
std::string stripComment(const std::string& line) {
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') quoted = !quoted;
        else if (line[i] == '#' && !quoted) return line.substr(0, i);
    }
    return line;
}

bool number(const std::string& s, long& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtol(s.c_str(), &end, 0);
    return end != nullptr && *end == 0;
}

bool real(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end != nullptr && *end == 0;
}

// "key=value" -> key, value (false if there's no '=')
bool option(const std::string& w, std::string& key, std::string& value) {
    const std::size_t eq = w.find('=');
    if (eq == std::string::npos) return false;
    key = w.substr(0, eq);
    value = w.substr(eq + 1);
    return true;
}

// The type word of a reading (u8 s8 u16 s16 u16le s16le)
bool typeWord(const std::string& t, I2cShow::Type& type) {
    if (t == "u8") type = I2cShow::Type::U8;
    else if (t == "s8") type = I2cShow::Type::S8;
    else if (t == "u16") type = I2cShow::Type::U16;
    else if (t == "s16") type = I2cShow::Type::S16;
    else if (t == "u16le") type = I2cShow::Type::U16LE;
    else if (t == "s16le") type = I2cShow::Type::S16LE;
    else return false;
    return true;
}

// The options of a reading, from word `from` on; value: also bytes=, le, offset=
bool readingOptions(const std::vector<std::string>& w, std::size_t from, I2cShow& s, bool value) {
    for (std::size_t i = from; i < w.size(); ++i) {
        std::string key, val;
        long v = 0;
        double d = 0;
        if (value && w[i] == "le") { s.le = true; continue; }
        if (!option(w[i], key, val)) return false;
        if (key == "mask" && number(val, v)) s.mask = static_cast<std::uint32_t>(v);
        else if (key == "bits" && number(val, v) && v >= 2 && v <= 32) s.bits = static_cast<int>(v);
        else if (key == "shift" && number(val, v) && v >= 0 && v < 32) s.shift = static_cast<int>(v);
        else if (key == "scale" && real(val, d)) s.scale = d;
        else if (key == "unit") s.unit = val;
        else if (key == "dec" && number(val, v) && v >= 0 && v <= 6) s.dec = static_cast<int>(v);
        else if (value && key == "offset" && real(val, d)) s.offset = d;
        else if (value && key == "bytes") {
            const std::size_t dash = val.find('-');
            long a = 0, b = 0;
            if (!number(val.substr(0, dash), a)) return false;
            if (dash == std::string::npos) b = a;
            else if (!number(val.substr(dash + 1), b)) return false;
            if (a < 0 || b < a || b - a > 3) return false;
            s.b0 = static_cast<int>(a);
            s.b1 = static_cast<int>(b);
        } else return false;
    }
    return true;
}

std::uint8_t crc8(const std::uint8_t* p, std::size_t n, std::uint8_t init) {
    std::uint8_t c = init;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int b = 0; b < 8; ++b) c = static_cast<std::uint8_t>(c & 0x80 ? (c << 1) ^ 0x31 : c << 1);
    }
    return c;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

const I2cMeasure* I2cChip::measure(const std::string& name) const {
    const std::string n = upper(name);
    for (const I2cMeasure& m : measures)
        for (const std::string& a : m.names) if (a == n) return &m;
    return nullptr;
}

void I2cChipList::parse(const std::string& text, const std::string& source) {
    I2cChip* chip = nullptr;
    enum class Block { None, Setup, Measure } block = Block::None;
    int lineNo = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = stripComment(text.substr(pos, nl - pos));
        pos = nl + 1;
        ++lineNo;
        const std::vector<std::string> w = words(line);
        if (w.empty()) continue;
        const std::string at = source + ":" + std::to_string(lineNo);
        auto error = [&](const std::string& what) { errors_.push_back(at + ": " + what); };
        const std::string& k = w[0];

        if (k == "chip") {
            chip = nullptr;
            block = Block::None;
            if (w.size() < 3) { error("chip: addresses and a name expected"); continue; }
            long a = 0, b = 0;
            const std::size_t dash = w[1].find('-');
            const bool okA = number(w[1].substr(0, dash), a);
            const bool okB = dash == std::string::npos ? (b = a, true) : number(w[1].substr(dash + 1), b);
            if (!okA || !okB || a < 0x08 || b > 0x77 || a > b) { error("chip: addresses 0x08-0x77 expected, got " + w[1]); continue; }
            I2cChip c;
            c.first = static_cast<std::uint8_t>(a);
            c.last = static_cast<std::uint8_t>(b);
            bool inWhat = false;
            for (std::size_t i = 2; i < w.size(); ++i) {
                if (w[i] == "|") { inWhat = true; continue; }
                std::string& dst = inWhat ? c.what : c.name;
                if (!dst.empty()) dst += ' ';
                dst += w[i];
            }
            if (c.name.empty()) { error("chip: a name expected"); continue; }
            c.source = at;
            chips_.push_back(c);
            chip = &chips_.back();
            continue;
        }
        if (chip == nullptr) { error("'" + k + "' before any 'chip'"); continue; }

        // ── setup / measure blocks ──────────────────────────────────
        if (k == "setup") {
            if (w.size() != 1) { error("setup: nothing after it expected"); continue; }
            block = Block::Setup;
            continue;
        }
        if (k == "measure") {
            I2cMeasure m;
            bool inWhat = false;
            for (std::size_t i = 1; i < w.size(); ++i) {
                if (w[i] == "|") { inWhat = true; continue; }
                if (inWhat) { if (!m.what.empty()) m.what += ' '; m.what += w[i]; continue; }
                std::size_t p0 = 0;                           // names: T,TEMP
                while (p0 <= w[i].size()) {
                    std::size_t c = w[i].find(',', p0);
                    if (c == std::string::npos) c = w[i].size();
                    if (c > p0) m.names.push_back(upper(w[i].substr(p0, c - p0)));
                    p0 = c + 1;
                }
            }
            if (m.names.empty()) { error("measure: a name expected"); continue; }
            chip->measures.push_back(m);
            block = Block::Measure;
            continue;
        }
        if (k == "write" || k == "wait" || k == "read") {
            if (block == Block::None) { error(k + ": only in a 'setup' or 'measure'"); continue; }
            I2cStep st;
            bool bad = false;
            if (k == "write") {
                st.kind = I2cStep::Kind::Write;
                for (std::size_t i = 1; i < w.size(); ++i) {
                    long v = 0;
                    if (!number(w[i], v) || v < 0 || v > 255) { bad = true; break; }
                    st.bytes.push_back(static_cast<std::uint8_t>(v));
                }
                if (bad || st.bytes.empty()) { error("write: bytes (0-255) expected"); continue; }
            } else if (k == "wait") {
                long v = 0;
                if (w.size() != 2 || !number(w[1], v) || v < 0 || v > kMaxWaitMs) { error("wait: milliseconds (0-2000) expected"); continue; }
                st.kind = I2cStep::Kind::Wait;
                st.n = static_cast<int>(v);
            } else {
                std::vector<long> nums;
                for (std::size_t i = 1; i < w.size(); ++i) {
                    std::string key, val;
                    long v = 0;
                    if (option(w[i], key, val) && key == "crc") {
                        if (val == "sensirion") st.crc = I2cStep::Crc::Sensirion;
                        else if (val == "aht") st.crc = I2cStep::Crc::Aht;
                        else if (val == "htu") st.crc = I2cStep::Crc::Htu;
                        else bad = true;
                    } else if (number(w[i], v)) nums.push_back(v);
                    else bad = true;
                }
                if (nums.size() == 1) { st.kind = I2cStep::Kind::Read; st.n = static_cast<int>(nums[0]); }
                else if (nums.size() == 2 && nums[0] >= 0 && nums[0] <= 255) {
                    st.kind = I2cStep::Kind::ReadReg;
                    st.reg = static_cast<std::uint8_t>(nums[0]);
                    st.n = static_cast<int>(nums[1]);
                } else bad = true;
                if (bad || st.n < 1 || st.n > 32) { error("read: [register] count (1-32) [crc=sensirion|aht|htu] expected"); continue; }
            }
            if (block == Block::Setup) chip->setup.push_back(st);
            else chip->measures.back().steps.push_back(st);
            continue;
        }
        if (k == "value" || k == "format" || k == "builtin") {
            if (block != Block::Measure) { error(k + ": only in a 'measure'"); continue; }
            I2cMeasure& m = chip->measures.back();
            if (k == "value") {
                I2cShow v;
                v.on = true;
                std::size_t from = 1;
                if (w.size() > 1 && typeWord(w[1], v.type)) from = 2;
                if (!readingOptions(w, from, v, true)) { error("value: can't read the options (bytes= le mask= bits= shift= scale= offset= dec= unit=)"); continue; }
                m.out = I2cMeasure::Out::Value;
                m.value = v;
            } else if (k == "format") {
                if (w.size() < 3 || w[1] != "bcd") { error("format: bcd <byte>[/<mask>] ... [text=..] [sep=..] expected"); continue; }
                bool bad = false, sepGiven = false;
                m.bcd.clear();
                m.texts.clear();
                for (std::size_t i = 2; i < w.size(); ++i) {
                    std::string key, val;
                    if (option(w[i], key, val) && key == "sep") { m.sep = val; sepGiven = true; continue; }
                    if (option(w[i], key, val) && key == "text") {     // text=20, text=.
                        m.texts.push_back(val);
                        m.bcd.push_back({ -1, 0 });
                        continue;
                    }
                    const std::size_t sl = w[i].find('/');
                    long b = 0, mask = 0xFF;
                    if (!number(w[i].substr(0, sl), b) || b < 0 || b > 31 ||
                        (sl != std::string::npos && !number(w[i].substr(sl + 1), mask))) { bad = true; break; }
                    m.bcd.push_back({ static_cast<int>(b), static_cast<std::uint8_t>(mask) });
                }
                if (bad || m.bcd.empty()) { error("format: bcd <byte>[/<mask>] ... [text=..] [sep=..] expected"); continue; }
                if (!m.texts.empty() && !sepGiven) m.sep.clear();   // the texts say where the separators go
                m.out = I2cMeasure::Out::Bcd;
            } else {
                if (w.size() < 2 || w.size() > 3) { error("builtin: a name [dec=..] expected"); continue; }
                m.builtin = w[1];
                m.out = I2cMeasure::Out::Builtin;
                if (w.size() == 3) {
                    std::string key, val;
                    long v = 0;
                    if (!option(w[2], key, val) || key != "dec" || !number(val, v) || v < 0 || v > 6) { error("builtin: dec=0..6 expected"); continue; }
                    m.value.dec = static_cast<int>(v);
                } else {
                    m.value.dec = 1;
                }
            }
            continue;
        }
        if (k == "range" && block == Block::Measure) {
            double a = 0, b = 0;
            if (w.size() != 3 || !real(w[1], a) || !real(w[2], b) || a > b) { error("range: a lowest and a highest value expected"); continue; }
            I2cShow& v = chip->measures.back().value;
            v.hasRange = true;
            v.min = a;
            v.max = b;
            continue;
        }
        block = Block::None;                                  // a chip-level word ends the block

        if (k == "id" || k == "id16") {
            I2cRule r;
            r.kind = k == "id" ? I2cRule::Kind::Id : I2cRule::Kind::Id16;
            long reg = 0;
            if (w.size() < 3 || !number(w[1], reg) || reg < 0 || reg > 255) { error(k + ": a register and a value expected"); continue; }
            r.reg = static_cast<std::uint8_t>(reg);
            bool bad = false;
            for (std::size_t i = 2; i < w.size(); ++i) {
                std::string key, value;
                long v = 0;
                if (option(w[i], key, value) && key == "mask") {
                    if (!number(value, v)) { bad = true; break; }
                    r.mask = static_cast<std::uint16_t>(v);
                } else if (option(w[i], key, value)) {        // value=name
                    if (!number(key, v)) { bad = true; break; }
                    r.values.push_back({ static_cast<std::uint16_t>(v), value });
                } else {
                    if (!number(w[i], v)) { bad = true; break; }
                    r.values.push_back({ static_cast<std::uint16_t>(v), std::string() });
                }
            }
            if (bad || r.values.empty()) { error(k + ": can't read the values"); continue; }
            if (r.kind == I2cRule::Kind::Id && r.mask == 0xFFFF) r.mask = 0xFF;
            chip->rules.push_back(r);
        } else if (k == "text") {
            long reg = 0;
            if (w.size() != 3 || !number(w[1], reg) || reg < 0 || reg > 255 || w[2].empty() || w[2].size() > 16) {
                error("text: a register and \"characters\" (at most 16) expected");
                continue;
            }
            I2cRule r;
            r.kind = I2cRule::Kind::Text;
            r.reg = static_cast<std::uint8_t>(reg);
            r.text = w[2];
            chip->rules.push_back(r);
        } else if (k == "status") {
            long mask = 0, value = 0;
            if (w.size() != 3 || !number(w[1], mask) || !number(w[2], value)) { error("status: a mask and a value expected"); continue; }
            I2cRule r;
            r.kind = I2cRule::Kind::Status;
            r.mask = static_cast<std::uint16_t>(mask & 0xFF);
            r.values.push_back({ static_cast<std::uint16_t>(value & 0xFF), std::string() });
            chip->rules.push_back(r);
        } else if (k == "probe") {
            if (w.size() != 2) { error("probe: a name expected"); continue; }
            I2cRule r;
            r.kind = I2cRule::Kind::Probe;
            r.text = w[1];
            chip->rules.push_back(r);
        } else if (k == "show") {
            long reg = 0;
            if (w.size() < 3 || !number(w[1], reg) || reg < 0 || reg > 255) { error("show: a register and a type expected"); continue; }
            I2cShow s;
            s.on = true;
            s.reg = static_cast<std::uint8_t>(reg);
            if (!typeWord(w[2], s.type)) { error("show: type u8, s8, u16, s16, u16le or s16le expected, got " + w[2]); continue; }
            if (!readingOptions(w, 3, s, false)) { error("show: can't read the options (mask= bits= shift= scale= unit= dec=)"); continue; }
            chip->show = s;
        } else if (k == "range") {
            double a = 0, b = 0;
            if (w.size() != 3 || !real(w[1], a) || !real(w[2], b) || a > b) { error("range: a lowest and a highest value expected"); continue; }
            chip->show.hasRange = true;
            chip->show.min = a;
            chip->show.max = b;
        } else if (k == "expected") {
            chip->expected = true;
        } else if (k == "panel") {
            long p = 0;
            if (w.size() != 2 || !number(w[1], p) || (p != 5 && p != 7)) { error("panel: 5 or 7 expected"); continue; }
            chip->panel = static_cast<int>(p);
        } else {
            error("don't know '" + k + "'");
        }
    }
}

void I2cChipList::finish(int panel) {
    chips_.erase(std::remove_if(chips_.begin(), chips_.end(),
                                [panel](const I2cChip& c) { return c.panel != 0 && c.panel != panel; }),
                 chips_.end());
    std::stable_partition(chips_.begin(), chips_.end(), [](const I2cChip& c) { return c.checks(); });
}

bool I2cChipList::identify(I2cBus& bus, std::uint8_t addr, I2cFound& f,
                           const std::map<std::string, I2cBuiltin>& builtins, const I2cChip** which) const {
    for (const I2cChip& c : chips_) {
        if (addr < c.first || addr > c.last) continue;
        std::string name = c.name, detail;
        I2cVerdict v = I2cVerdict::Ok;
        const bool noReg = noRegisterWrites(addr);
        for (const I2cRule& r : c.rules) {
            if (noReg && r.kind != I2cRule::Kind::Status && r.kind != I2cRule::Kind::Probe) { v = I2cVerdict::NotIt; break; }
            char b[48];
            switch (r.kind) {
                case I2cRule::Kind::Id:
                case I2cRule::Kind::Id16: {
                    const bool wide = r.kind == I2cRule::Kind::Id16;
                    std::uint8_t buf[2] = { 0, 0 };
                    if (!bus.readRegs(addr, r.reg, buf, wide ? 2 : 1)) { v = I2cVerdict::NotIt; break; }
                    const std::uint16_t raw = wide ? static_cast<std::uint16_t>(buf[0] << 8 | buf[1]) : buf[0];
                    const std::uint16_t val = static_cast<std::uint16_t>(raw & r.mask);
                    auto it = std::find_if(r.values.begin(), r.values.end(), [val](const auto& p) { return p.first == val; });
                    if (it == r.values.end()) { v = I2cVerdict::NotIt; break; }
                    if (!it->second.empty()) name = it->second;
                    std::snprintf(b, sizeof(b), wide ? "ID 0x%04X" : "ID 0x%02X", raw);
                    if (detail.empty()) detail = b;
                    break;
                }
                case I2cRule::Kind::Text: {
                    std::uint8_t buf[16];
                    const std::size_t n = r.text.size();
                    if (!bus.readRegs(addr, r.reg, buf, n) || std::memcmp(buf, r.text.data(), n) != 0) { v = I2cVerdict::NotIt; break; }
                    if (detail.empty()) detail = "ID " + r.text;
                    break;
                }
                case I2cRule::Kind::Status: {
                    std::uint8_t s = 0;
                    if (!bus.readRaw(addr, &s, 1) || (s & r.mask) != r.values[0].first) { v = I2cVerdict::NotIt; break; }
                    std::snprintf(b, sizeof(b), "status 0x%02X", s);
                    if (detail.empty()) detail = b;
                    break;
                }
                case I2cRule::Kind::Probe: {
                    auto it = builtins.find(r.text);
                    if (it == builtins.end()) { v = I2cVerdict::NotIt; break; }
                    std::string d;
                    const I2cVerdict pv = it->second(bus, addr, name, d);
                    if (pv == I2cVerdict::NotIt) { v = I2cVerdict::NotIt; break; }
                    if (!d.empty()) detail = d;
                    if (pv == I2cVerdict::Fault) v = I2cVerdict::Fault;
                    break;
                }
            }
            if (v == I2cVerdict::NotIt) break;
        }
        if (v == I2cVerdict::NotIt) continue;

        // A reading
        if (c.show.on && !noReg) {
            const I2cShow& s = c.show;
            const bool wide = s.type != I2cShow::Type::U8 && s.type != I2cShow::Type::S8;
            std::uint8_t buf[2] = { 0, 0 };
            double value = 0;
            if (!bus.readRegs(addr, s.reg, buf, wide ? 2 : 1) || !decode(s, buf, wide ? 2 : 1, value)) {
                detail = "no reading";
                v = I2cVerdict::Fault;
            } else {
                char b[40];
                std::snprintf(b, sizeof(b), "%.*f %s", s.dec, value, s.unit.c_str());
                std::string reading = b;
                while (!reading.empty() && reading.back() == ' ') reading.pop_back();
                if (s.hasRange && (value < s.min || value > s.max)) {
                    reading += " (odd)";
                    v = I2cVerdict::Fault;
                }
                detail = detail.empty() || detail.rfind("ID", 0) == 0 ? reading : detail + "  " + reading;
            }
        }
        f.state = !c.checks() ? I2cState::Probable : v == I2cVerdict::Ok ? I2cState::Ok : I2cState::Fault;
        std::snprintf(f.name, sizeof(f.name), "%s", name.c_str());
        std::snprintf(f.what, sizeof(f.what), "%s", c.what.c_str());
        std::snprintf(f.detail, sizeof(f.detail), "%s", detail.c_str());
        if (which != nullptr) *which = &c;
        return true;
    }
    return false;
}

const I2cChip* I2cChipList::expectedAt(std::uint8_t addr) const {
    for (const I2cChip& c : chips_)
        if (c.expected && addr >= c.first && addr <= c.last) return &c;
    return nullptr;
}

}  // namespace hipi

namespace hipi {

bool I2cChipList::decode(const I2cShow& s, const std::uint8_t* buf, std::size_t n, double& value) {
    std::int64_t raw = 0;
    if (s.b1 >= 0) {                                   // bytes=b0-b1: an unsigned number
        if (static_cast<std::size_t>(s.b1) >= n) return false;
        for (int i = s.b0; i <= s.b1; ++i) {
            const int k = s.le ? s.b1 - (i - s.b0) : i;
            raw = (raw << 8) | buf[k];
        }
    } else {
        const std::size_t b = static_cast<std::size_t>(s.b0);
        const bool wide = s.type != I2cShow::Type::U8 && s.type != I2cShow::Type::S8;
        if (b + (wide ? 2 : 1) > n) return false;
        switch (s.type) {
            case I2cShow::Type::U8:    raw = buf[b]; break;
            case I2cShow::Type::S8:    raw = static_cast<std::int8_t>(buf[b]); break;
            case I2cShow::Type::U16:   raw = buf[b] << 8 | buf[b + 1]; break;
            case I2cShow::Type::S16:   raw = static_cast<std::int16_t>(buf[b] << 8 | buf[b + 1]); break;
            case I2cShow::Type::U16LE: raw = buf[b + 1] << 8 | buf[b]; break;
            case I2cShow::Type::S16LE: raw = static_cast<std::int16_t>(buf[b + 1] << 8 | buf[b]); break;
        }
    }
    if (s.mask != 0xFFFFFFFF || s.bits != 0) {
        raw &= s.mask;
        if (s.bits > 0 && (raw & (std::int64_t(1) << (s.bits - 1)))) raw -= std::int64_t(1) << s.bits;   // sign
    }
    value = static_cast<double>(raw >> s.shift) * s.scale + s.offset;
    return true;
}

bool I2cChipList::run(I2cBus& bus, std::uint8_t addr, const std::vector<I2cStep>& steps,
                      std::vector<std::uint8_t>& data, std::string& err) {
    int waited = 0;
    for (const I2cStep& st : steps) {
        switch (st.kind) {
            case I2cStep::Kind::Write:
                if (!bus.write(addr, st.bytes.data(), st.bytes.size())) { err = "NOACK"; return false; }
                break;
            case I2cStep::Kind::Wait:
                waited += st.n;
                if (waited > kMaxWaitMs) { err = "TIME"; return false; }
                bus.delayMs(st.n);
                break;
            case I2cStep::Kind::Read:
            case I2cStep::Kind::ReadReg: {
                std::vector<std::uint8_t> b(static_cast<std::size_t>(st.n));
                const bool ok = st.kind == I2cStep::Kind::Read ? bus.readRaw(addr, b.data(), b.size())
                                                               : bus.readRegs(addr, st.reg, b.data(), b.size());
                if (!ok) { err = "NOACK"; return false; }
                // CRC checked, then dropped
                if (st.crc == I2cStep::Crc::Sensirion) {          // per 2 bytes, CRC-8 0x31 from 0xFF
                    std::vector<std::uint8_t> d;
                    for (std::size_t i = 0; i + 2 < b.size(); i += 3) {
                        if (crc8(&b[i], 2, 0xFF) != b[i + 2]) { err = "CRC"; return false; }
                        d.push_back(b[i]);
                        d.push_back(b[i + 1]);
                    }
                    b = d;
                } else if (st.crc == I2cStep::Crc::Aht || st.crc == I2cStep::Crc::Htu) {   // over all, the last byte
                    if (b.size() < 2 || crc8(b.data(), b.size() - 1, st.crc == I2cStep::Crc::Aht ? 0xFF : 0x00) != b.back()) {
                        err = "CRC";
                        return false;
                    }
                    b.pop_back();
                }
                data = b;
                break;
            }
        }
    }
    return true;
}

bool I2cChipList::measure(I2cBus& bus, std::uint8_t addr, const I2cMeasure& m, int dec, std::string& out,
                          std::string& err, const std::map<std::string, I2cMeasureBuiltin>& builtins,
                          double* valueOut) {
    double value = 0;
    if (m.out == I2cMeasure::Out::Builtin) {
        auto it = builtins.find(m.builtin);
        if (it == builtins.end()) { err = "NAME"; return false; }
        if (!it->second(bus, addr, value)) { err = "NOACK"; return false; }
    } else {
        std::vector<std::uint8_t> data;
        if (!run(bus, addr, m.steps, data, err)) return false;
        if (m.out == I2cMeasure::Out::Bcd) {
            out.clear();
            std::size_t text = 0;
            for (std::size_t i = 0; i < m.bcd.size(); ++i) {
                const auto& [b, mask] = m.bcd[i];
                if (b < 0) {                                   // text=..
                    if (text < m.texts.size()) out += m.texts[text++];
                    continue;
                }
                if (static_cast<std::size_t>(b) >= data.size()) { err = "DATA"; return false; }
                const std::uint8_t v = data[static_cast<std::size_t>(b)] & mask;
                const char t[3] = { static_cast<char>('0' + ((v >> 4) & 0x0F)), static_cast<char>('0' + (v & 0x0F)), 0 };
                if (i > 0 && m.bcd[i - 1].first >= 0) out += m.sep;   // between two bytes
                out += t;
            }
            return true;
        }
        if (!decode(m.value, data.data(), data.size(), value)) { err = "DATA"; return false; }
    }
    if (valueOut != nullptr) *valueOut = value;
    char b[32];
    std::snprintf(b, sizeof(b), "%.*f", dec >= 0 ? dec : m.value.dec, value);
    out = b;
    if (out == "-0" || out.rfind("-0.", 0) == 0) {            // no "-0.0"
        bool zero = true;
        for (char c : out) if (c >= '1' && c <= '9') zero = false;
        if (zero) out.erase(0, 1);
    }
    if (m.value.hasRange && (value < m.value.min || value > m.value.max)) { err = "RANGE"; return false; }
    return true;
}

}  // namespace hipi
