// i2c_command.cpp -- see i2c_command.h
#include "i2c_command.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace hipi {

namespace {

constexpr int kMaxBytes = 32;

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string noSpaces(const std::string& s) {
    std::string r;
    for (char c : s) if (c != ' ' && c != '\t') r += c;
    return r;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Decimal, or hex after '$' (or 0x)
bool num(const std::string& s, long& v, long lo, long hi) {
    if (s.empty()) return false;
    char* end = nullptr;
    if (s[0] == '$') v = std::strtol(s.c_str() + 1, &end, 16);
    else if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) v = std::strtol(s.c_str() + 2, &end, 16);
    else v = std::strtol(s.c_str(), &end, 10);
    return end != nullptr && *end == 0 && end != s.c_str() && v >= lo && v <= hi;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::size_t p = 0;
    while (true) {
        const std::size_t c = s.find(sep, p);
        v.push_back(s.substr(p, c == std::string::npos ? std::string::npos : c - p));
        if (c == std::string::npos) break;
        p = c + 1;
    }
    return v;
}

// A line's commands: separated by ';', or by ',' when a letter comes next
// (the HP-41 hardly types ';'). "A118,MT" = "A118;MT" -- a ',' between
// numbers ("R5,2", "W,172,51") stays part of its command.
std::vector<std::string> commands(const std::string& line) {
    std::vector<std::string> v;
    std::string cur;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        bool sep = c == ';';
        if (c == ',') {
            std::size_t j = i + 1;
            while (j < line.size() && line[j] == ' ') ++j;
            sep = j < line.size() && std::isalpha(static_cast<unsigned char>(line[j]));
        }
        if (sep) {
            v.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    v.push_back(cur);
    return v;
}

std::string joinBytes(const std::uint8_t* b, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0) s += ',';
        s += std::to_string(b[i]);
    }
    return s;
}

const char* stateWord(I2cState s) {
    switch (s) {
        case I2cState::Ok:       return "OK";
        case I2cState::Fault:    return "FAULT";
        case I2cState::Probable: return "PROBABLY";
        case I2cState::Missing:  return "MISSING";
        default:                 return "UNKNOWN";
    }
}

}  // namespace

bool I2cCommander::readOnly(const std::string& line) {
    for (const std::string& c : commands(line)) {
        const std::string t = trim(c);
        if (!t.empty() && std::toupper(static_cast<unsigned char>(t[0])) == 'W') return false;
    }
    return true;
}

bool I2cCommander::settingsOnly(const std::string& line) {
    bool any = false;
    for (const std::string& c : commands(line)) {
        const std::string t = trim(c);
        if (t.empty()) continue;
        const char k = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
        if (k != 'A' && k != 'P') return false;
        any = true;
    }
    return any;
}

void I2cCommander::reset() {
    addr_ = 0;
    dec_ = -1;
    setupDone_.clear();
}

std::string I2cCommander::run(const std::string& line) {
    std::string answer;
    waited_ = 0;
    for (const std::string& c : commands(line)) {
        const std::string t = trim(c);
        if (t.empty()) continue;
        std::string a;
        if (!command(t, a)) return a;              // an error ends the line
        if (!a.empty()) answer = a;
    }
    return answer;
}

const I2cChip* I2cCommander::chipAt(std::uint8_t addr, std::string* name) {
    I2cFound f;
    f.addr = addr;
    const I2cChip* which = nullptr;
    if (!env_.chips().identify(*env_.bus, addr, f, env_.probes, &which)) return nullptr;
    if (name != nullptr) *name = f.name;
    return which;
}

bool I2cCommander::doMeasure(const std::string& name, std::string& answer) {
    std::uint8_t addr = addr_;
    const I2cChip* chip = nullptr;
    const I2cMeasure* m = nullptr;
    if (addr != 0) {
        chip = chipAt(addr);
        if (chip == nullptr) {
            // Not there at all (unplugged): NOACK, and its setup again when it's back
            std::uint8_t b = 0;
            if (!env_.bus->readRaw(addr, &b, 1)) {
                setupDone_.erase(addr);
                answer = "ERR NOACK";
                return false;
            }
        }
        m = chip != nullptr ? chip->measure(name) : nullptr;
    } else {
        // No chip chosen: the first one on the bus that has this measurement
        I2cFound found[40];
        bool answered[128];
        const int n = env_.identifyAll(found, 40, answered);
        for (int i = 0; i < n && m == nullptr; ++i) {
            if (found[i].state == I2cState::Missing || found[i].state == I2cState::Unknown) continue;
            chip = chipAt(found[i].addr);
            if (chip != nullptr && (m = chip->measure(name)) != nullptr) addr = found[i].addr;
        }
    }
    if (m == nullptr) { answer = "ERR NAME"; return false; }
    // Writes to a chip HIPI uses itself: refused
    bool writes = false;
    for (const I2cStep& st : m->steps) if (st.kind == I2cStep::Kind::Write) writes = true;
    if (!chip->setup.empty()) writes = true;
    if (writes && env_.hipiOwns && env_.hipiOwns(addr)) { answer = "ERR HIPI"; return false; }
    std::string err;
    if (!chip->setup.empty() && setupDone_.count(addr) == 0) {
        std::vector<std::uint8_t> data;
        if (!I2cChipList::run(*env_.bus, addr, chip->setup, data, err)) { answer = "ERR " + err; return false; }
        setupDone_.insert(addr);
    }
    std::string out;
    if (!I2cChipList::measure(*env_.bus, addr, *m, dec_, out, err, env_.measures)) {
        answer = "ERR " + err;
        return false;
    }
    answer = out;
    return true;
}

bool I2cCommander::command(const std::string& cmd, std::string& answer) {
    const char k = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd[0])));
    const std::string arg = noSpaces(cmd.substr(1));
    auto fail = [&](const char* what) { answer = std::string("ERR ") + what; return false; };
    auto needChip = [&]() { return addr_ != 0; };
    long v = 0;

    switch (k) {
        case 'A': {
            if (arg.empty()) return fail("SYNTAX");
            if (std::isalpha(static_cast<unsigned char>(arg[0]))) {            // by name
                I2cFound found[40];
                bool answered[128];
                const int n = env_.identifyAll(found, 40, answered);
                const std::string want = upper(arg);
                for (int i = 0; i < n; ++i) {
                    if (found[i].state == I2cState::Missing) continue;
                    if (upper(found[i].name) == want) { addr_ = found[i].addr; return true; }
                }
                return fail("NAME");
            }
            if (!num(arg, v, 0x08, 0x77)) return fail("SYNTAX");
            addr_ = static_cast<std::uint8_t>(v);
            return true;
        }
        case 'R': {
            if (!needChip()) return fail("CHIP");
            const std::size_t colon = arg.find(':');
            if (colon != std::string::npos) {                                  // R<reg>:<type>
                if (!num(arg.substr(0, colon), v, 0, 255)) return fail("SYNTAX");
                const std::string t = upper(arg.substr(colon + 1));
                I2cShow s;
                if (t == "U8") s.type = I2cShow::Type::U8;
                else if (t == "S8") s.type = I2cShow::Type::S8;
                else if (t == "U16") s.type = I2cShow::Type::U16;
                else if (t == "S16") s.type = I2cShow::Type::S16;
                else if (t == "U16L") s.type = I2cShow::Type::U16LE;
                else if (t == "S16L") s.type = I2cShow::Type::S16LE;
                else return fail("SYNTAX");
                const std::size_t n = (s.type == I2cShow::Type::U8 || s.type == I2cShow::Type::S8) ? 1 : 2;
                std::uint8_t b[2];
                if (!env_.bus->readRegs(addr_, static_cast<std::uint8_t>(v), b, n)) return fail("NOACK");
                double value = 0;
                I2cChipList::decode(s, b, n, value);
                answer = std::to_string(static_cast<long>(value));
                return true;
            }
            const std::vector<std::string> p = split(arg, ',');
            long n = 1;
            if (p.empty() || !num(p[0], v, 0, 255) || p.size() > 2 || (p.size() == 2 && !num(p[1], n, 1, kMaxBytes)))
                return fail("SYNTAX");
            std::uint8_t b[kMaxBytes];
            if (!env_.bus->readRegs(addr_, static_cast<std::uint8_t>(v), b, static_cast<std::size_t>(n))) return fail("NOACK");
            answer = joinBytes(b, static_cast<std::size_t>(n));
            return true;
        }
        case 'W': {
            if (!needChip()) return fail("CHIP");
            if (env_.hipiOwns && env_.hipiOwns(addr_)) return fail("HIPI");
            std::vector<std::string> p = split(arg, ',');
            if (!p.empty() && p[0].empty()) p.erase(p.begin());                 // W,<b>...: no register
            if (p.empty() || p.size() > kMaxBytes + 1) return fail("SYNTAX");
            std::vector<std::uint8_t> b;
            for (const std::string& s : p) {
                if (!num(s, v, 0, 255)) return fail("SYNTAX");
                b.push_back(static_cast<std::uint8_t>(v));
            }
            if (!env_.bus->write(addr_, b.data(), b.size())) return fail("NOACK");
            return true;
        }
        case 'Q': {
            if (!needChip()) return fail("CHIP");
            if (!num(arg, v, 1, kMaxBytes)) return fail("SYNTAX");
            std::uint8_t b[kMaxBytes];
            if (!env_.bus->readRaw(addr_, b, static_cast<std::size_t>(v))) return fail("NOACK");
            answer = joinBytes(b, static_cast<std::size_t>(v));
            return true;
        }
        case 'D': {
            if (!num(arg, v, 0, I2cChipList::kMaxWaitMs)) return fail("SYNTAX");
            waited_ += static_cast<int>(v);
            if (waited_ > I2cChipList::kMaxWaitMs) return fail("TIME");
            env_.bus->delayMs(static_cast<int>(v));
            return true;
        }
        case 'M':
            if (arg.empty()) return fail("SYNTAX");
            return doMeasure(arg, answer);
        case 'L': {
            if (!needChip()) return fail("CHIP");
            const I2cChip* c = chipAt(addr_);
            if (c == nullptr) return fail("NAME");
            std::string s;
            for (const I2cMeasure& m : c->measures) {
                if (!s.empty()) s += ',';
                s += m.names[0];
            }
            answer = s.empty() ? "NONE" : s;
            return true;
        }
        case 'P':
            if (arg.empty()) { dec_ = -1; return true; }
            if (!num(arg, v, 0, 6)) return fail("SYNTAX");
            dec_ = static_cast<int>(v);
            return true;
        case 'S': {
            I2cFound found[40];
            bool answered[128];
            env_.identifyAll(found, 40, answered);
            std::string s;
            for (int a = 0x08; a <= 0x77; ++a) {
                if (!answered[a]) continue;
                if (!s.empty()) s += ',';
                s += std::to_string(a);
            }
            answer = s.empty() ? "NONE" : s;
            return true;
        }
        case 'I': {
            if (!needChip()) return fail("CHIP");
            std::uint8_t b;
            if (!env_.bus->readRaw(addr_, &b, 1)) return fail("NOACK");
            I2cFound f;
            f.addr = addr_;
            if (!env_.chips().identify(*env_.bus, addr_, f, env_.probes)) { answer = "UNKNOWN"; return true; }
            answer = std::string(f.name) + " " + stateWord(f.state);
            return true;
        }
        default:
            return fail("SYNTAX");
    }
}

}  // namespace hipi
