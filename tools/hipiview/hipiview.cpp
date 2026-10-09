// hipiview -- shows HIPI's 7" display on the PC, live, through the
// display mirror port, and records it to a video file.
//
//   hipiview                         find the mirror port and show it
//   hipiview /dev/ttyACM3            ... on this port
//   hipiview --record demo.mp4       record from the start
//   hipiview --log session.hvlog     also save the raw stream
//   hipiview --replay session.hvlog  play a saved stream in the window
//   hipiview --replay session.hvlog --render demo.mp4
//                                    turn a saved stream into a video,
//                                    as fast as possible, no window
//
// Keys: R record on/off, S screenshot (PNG), F full screen,
//       B backlight dimming on/off, Q / Esc quit.
//
// See README.md.
#include "lt7683_emu.hpp"
#include "deframer.hpp"
#include "png.hpp"
#include "input.hpp"
#include "serial_port.hpp"
#include <zlib.h>

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
static constexpr const char* kPipeWrite = "wb";     // binary: the video frames
#else
static constexpr const char* kPipeWrite = "w";
#endif

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

// ── Options ─────────────────────────────────────────────────────────────
struct Options {
    std::string device;          // empty: find it
    std::string record;          // start recording to this file
    std::string log;             // save the raw stream
    std::string replay;          // play a saved stream
    std::string render;          // with replay: render to this video, no window
    int fps = 30;
    int crf = 18;
    float scale = 1.0f;
    bool backlight = false;      // dim the picture like the PWM backlight
    bool marks = true;           // touch rings (T)
    std::string testInput;       // tests: mouse/key events from a file (see TestInput)
};

void usage() {
    std::printf(
        "hipiview -- HIPI's 7\" display on the PC (display mirror)\n\n"
        "usage: hipiview [options] [device]\n\n"
        "  device              the mirror port, e.g. /dev/ttyACM3\n"
        "                      (default: found by its name, \"HIPI Display Mirror\")\n"
        "  --record FILE       record a video from the start (R toggles)\n"
        "  --fps N             video frame rate (default 30)\n"
        "  --crf N             video quality, lower is better (default 18)\n"
        "  --log FILE          save the raw stream, to --replay or --render later\n"
        "  --replay FILE       play a saved stream instead of a port\n"
        "  --render FILE       with --replay: make a video, as fast as possible, no window\n"
        "  --scale X           window size factor (default 1)\n"
        "  --backlight         dim the picture like the real backlight (screen saver)\n\n"
        "keys: R record, S screenshot, F full screen (Esc leaves it), B backlight,\n"
        "      T touch rings on/off, Q quit\n"
        "with Mirror to PC < Control > on HIPI: the mouse is a finger on the screen;\n"
        "      Enter OK, Up/Down, Backspace X, Tab Shift (or hold Shift), Left/Right\n"
        "      previous/next view, mouse wheel Up/Down\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); return false; }
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else if (a == "--record") { if (!next(o.record)) return false; }
        else if (a == "--log") { if (!next(o.log)) return false; }
        else if (a == "--replay") { if (!next(o.replay)) return false; }
        else if (a == "--render") { if (!next(o.render)) return false; }
        else if (a == "--fps") { if (!next(v)) return false; o.fps = std::max(1, std::atoi(v.c_str())); }
        else if (a == "--crf") { if (!next(v)) return false; o.crf = std::atoi(v.c_str()); }
        else if (a == "--scale") { if (!next(v)) return false; o.scale = std::max(0.25f, static_cast<float>(std::atof(v.c_str()))); }
        else if (a == "--backlight") o.backlight = true;
        else if (a == "--test-input") { if (!next(o.testInput)) return false; }
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
        else o.device = a;
    }
    if (!o.render.empty() && o.replay.empty()) { std::fprintf(stderr, "--render needs --replay\n"); return false; }
    return true;
}

std::string timestampName(const char* ext) {
    char buf[64];
    const std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof buf, "hipiview-%Y%m%d-%H%M%S", std::localtime(&t));
    return std::string(buf) + ext;
}

// (Finding the port: findMirrorPort(), serial_port.cpp)

// ── Asset cache ─────────────────────────────────────────────────────────
// Layer pictures HIPI sends only once (the Tape view's pictures, ...) are
// kept in the cache folder as <id>.asset; the next session only names
// them. A changed picture on HIPI's SD card gets a new id.
//   Linux:   ~/.cache/hipiview  ($XDG_CACHE_HOME/hipiview)
//   macOS:   ~/Library/Caches/hipiview
//   Windows: %LOCALAPPDATA%\hipiview
std::string cacheDir() {
    auto env = [](const char* name) { const char* v = std::getenv(name); return std::string(v ? v : ""); };
    fs::path base;
#if defined(_WIN32)
    base = env("LOCALAPPDATA").empty() ? fs::path(".") : fs::path(env("LOCALAPPDATA"));
#elif defined(__APPLE__)
    base = fs::path(env("HOME").empty() ? "." : env("HOME")) / "Library" / "Caches";
#else
    base = !env("XDG_CACHE_HOME").empty() ? fs::path(env("XDG_CACHE_HOME"))
                                          : fs::path(env("HOME").empty() ? "." : env("HOME")) / ".cache";
#endif
    const fs::path dir = base / "hipiview";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}

std::string assetPath(std::uint32_t id) {
    char name[32];
    std::snprintf(name, sizeof name, "%08x.asset", id);
    return (fs::path(cacheDir()) / name).string();
}

// File format: "HVA2" size(4) crc32(4) then the bytes. The checksum
// guards against a file damaged on disk -- and files from hipiview
// versions before the reliable link (no header), which may hold a picture
// with a piece missing, are thrown away and simply sent again.
constexpr char kAssetMagic[4] = { 'H', 'V', 'A', '2' };

std::uint32_t assetCrc(const std::vector<std::uint8_t>& b) {
    return static_cast<std::uint32_t>(crc32(0, b.data(), static_cast<uInt>(b.size())));
}

bool loadAsset(std::uint32_t id, std::vector<std::uint8_t>& out) {
    const std::string path = assetPath(id);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::uint8_t hdr[12];
    bool ok = std::fread(hdr, 1, sizeof hdr, f) == sizeof hdr && std::memcmp(hdr, kAssetMagic, 4) == 0;
    if (ok) {
        const std::uint32_t size = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (static_cast<std::uint32_t>(hdr[7]) << 24);
        const std::uint32_t crc = hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | (static_cast<std::uint32_t>(hdr[11]) << 24);
        out.assign(size, 0);
        ok = size <= (16u << 20) && std::fread(out.data(), 1, size, f) == size && assetCrc(out) == crc;
    }
    std::fclose(f);
    if (!ok) {
        std::printf("hipiview: cached picture %08x is damaged or old -- fetching it again\n", id);
        std::fflush(stdout);
        std::remove(path.c_str());
        out.clear();
    }
    return ok;
}

void saveAsset(std::uint32_t id, const std::vector<std::uint8_t>& bytes) {
    const std::string path = assetPath(id), tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    const std::uint32_t size = static_cast<std::uint32_t>(bytes.size()), crc = assetCrc(bytes);
    std::uint8_t hdr[12];
    std::memcpy(hdr, kAssetMagic, 4);
    for (int k = 0; k < 4; ++k) {
        hdr[4 + k] = static_cast<std::uint8_t>(size >> (8 * k));
        hdr[8 + k] = static_cast<std::uint8_t>(crc >> (8 * k));
    }
    const bool ok = std::fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr &&
                    std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::error_code ec;
    if (std::fclose(f) == 0 && ok) fs::rename(tmp, path, ec);     // (replaces an old one, on Windows too)
    if (ec || !ok) fs::remove(tmp, ec);
}

// The ids in the cache, newest first (at most 32 -- what a request holds)
std::vector<std::uint32_t> cachedAssets() {
    std::vector<std::pair<fs::file_time_type, std::uint32_t>> found;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(cacheDir(), ec)) {
        unsigned id = 0;
        char tail[16] = {};
        const std::string name = e.path().filename().string();
        if (std::sscanf(name.c_str(), "%8x.%15s", &id, tail) == 2 && std::strcmp(tail, "asset") == 0)
            found.emplace_back(e.last_write_time(ec), id);
    }
    std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<std::uint32_t> ids;
    for (auto& f : found) { if (ids.size() >= 32) break; ids.push_back(f.second); }
    return ids;
}

// Set when HIPI named a cached picture we can't use (deleted, damaged):
// the reader then asks for a fresh session, which no longer lists it
std::atomic<bool> g_assetMissing{ false };

void attachCache(LT7683Emu& emu) {
    emu.loadAsset = [](std::uint32_t id, std::vector<std::uint8_t>& out) {
        if (loadAsset(id, out)) return true;
        g_assetMissing = true;
        return false;
    };
    emu.saveAsset = [](std::uint32_t id, const std::vector<std::uint8_t>& b) {
        saveAsset(id, b);
        std::printf("hipiview: cached picture %08x (%zu kB) -- next time it's not sent again\n", id, b.size() / 1024);
        std::fflush(stdout);
    };
}

// The session request: "HIPIVIEW", count, the cached asset ids
std::vector<std::uint8_t> sessionRequest() {
    const std::vector<std::uint32_t> ids = cachedAssets();
    std::vector<std::uint8_t> r = { 'H', 'I', 'P', 'I', 'V', 'I', 'E', 'W', static_cast<std::uint8_t>(ids.size()) };
    for (std::uint32_t id : ids)
        for (int k = 0; k < 4; ++k) r.push_back(static_cast<std::uint8_t>(id >> (8 * k)));
    return r;
}

// ── Incoming data ───────────────────────────────────────────────────────
// ms since start; data; desync: what came before can't be trusted (a new
// session was asked for) -- drop everything until HIPI's next greeting
// input: not from HIPI but the mouse here (a 'T' message, see input.hpp)
// -- kept in the log so a replay or a rendered video shows the touch rings
// restart: the port was opened anew (HIPI restarted, e.g. after a firmware
// update) -- start from a black panel
struct Chunk { std::uint32_t ms; std::vector<std::uint8_t> data; bool desync = false; bool input = false;
               bool restart = false; };

class Inbox {
public:
    void push(Chunk c) { std::lock_guard<std::mutex> l(m_); q_.push_back(std::move(c)); }
    std::vector<Chunk> take() { std::lock_guard<std::mutex> l(m_); std::vector<Chunk> out; out.swap(q_); return out; }
private:
    std::mutex m_;
    std::vector<Chunk> q_;
};

// Raw stream log: "HIPIVIEWLOG4\n" (the raw stream, numbered frames;
// "...LOG3" is the same without touches, "...LOG1" from before frames
// existed), then records of
// [ms since start (u32 LE)] [length (u32 LE)] [bytes]
// A length with bit 31 set is a touch here (Chunk::input), not stream data.
constexpr std::uint32_t kLogInput = 0x80000000u;

class LogWriter {
public:
    bool open(const std::string& path) {
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) return false;
        std::fputs("HIPIVIEWLOG4\n", f_);
        return true;
    }
    ~LogWriter() { if (f_) std::fclose(f_); }
    // (from the reader thread and, for touches, the window's)
    void write(const Chunk& c) {
        std::lock_guard<std::mutex> l(m_);
        if (!f_) return;
        const std::uint32_t hdr[2] = { c.ms, static_cast<std::uint32_t>(c.data.size()) | (c.input ? kLogInput : 0) };
        std::fwrite(hdr, sizeof hdr, 1, f_);
        std::fwrite(c.data.data(), 1, c.data.size(), f_);
    }
private:
    std::mutex m_;
    FILE* f_ = nullptr;
};

bool readLog(const std::string& path, std::vector<Chunk>& out, bool& framed) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[13] = {};
    if (std::fread(magic, 1, 13, f) != 13) { std::fclose(f); return false; }
    if (std::memcmp(magic, "HIPIVIEWLOG4\n", 13) == 0 || std::memcmp(magic, "HIPIVIEWLOG3\n", 13) == 0) framed = true;
    else if (std::memcmp(magic, "HIPIVIEWLOG1\n", 13) == 0) framed = false;
    else {
        std::fprintf(stderr, "hipiview: %s is from another hipiview version\n", path.c_str());
        std::fclose(f);
        return false;
    }
    std::uint32_t hdr[2];
    while (std::fread(hdr, sizeof hdr, 1, f) == 1) {
        const std::uint32_t len = hdr[1] & ~kLogInput;
        Chunk c{ hdr[0], std::vector<std::uint8_t>(len) };
        c.input = (hdr[1] & kLogInput) != 0;
        if (len && std::fread(c.data.data(), 1, len, f) != len) break;
        out.push_back(std::move(c));
    }
    std::fclose(f);
    return true;
}

// Replays a saved raw stream into the emulator (through the same link
// logic as live, minus the messages to HIPI)
class ReplayFeeder {
public:
    ReplayFeeder(LT7683Emu& emu, bool framed) : emu_(emu), framed_(framed) {}
    void feed(const Chunk& c) {
        if (c.input) return;                       // a touch here, not HIPI's
        if (!framed_) { emu_.feed(c.data.data(), c.data.size()); return; }
        link_.feed(c.data.data(), c.data.size(), c.ms / 1000.0,
                   [&](const std::uint8_t* p, std::size_t n) { emu_.feed(p, n); }, nullptr);
    }
private:
    LT7683Emu& emu_;
    bool framed_;
    Link link_;
};

// Reads the port in the background; reopens it when HIPI restarts
class SerialReader {
public:
    SerialReader(std::string dev, Inbox& inbox, LogWriter& log, Clock::time_point t0)
        : dev_(std::move(dev)), inbox_(inbox), log_(log), t0_(t0) {}
    void start() { th_ = std::thread([this] { run(); finished_ = true; }); }
    // Stops the reader. Waits at most half a second: a serial port can
    // hang in read()/close() in the kernel, and quitting must never wait
    // for HIPI to draw something.
    void stop() {
        stop_ = true;
        if (!th_.joinable()) return;
        for (int i = 0; i < 50 && !finished_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (finished_) th_.join();
        else th_.detach();               // the process exits anyway
    }
    bool open() const { return open_; }
    bool everOpen() const { return everOpen_; }
    unsigned long long resends() const { return resends_; }
    unsigned long long restarts() const { return restarts_; }
    bool stopped() const { return finished_; }
    std::string device() const { std::lock_guard<std::mutex> l(m_); return cur_; }
    Clock::time_point lastData() const { return Clock::time_point(Clock::duration(lastData_.load())); }
    std::uint64_t bytes() const { return bytes_; }
    // A message for HIPI (input.hpp); its session byte is filled in here.
    // Dropped while there's no session.
    void sendInput(std::vector<std::uint8_t> msg) {
        std::lock_guard<std::mutex> l(inM_);
        if (inQ_.size() < 256) inQ_.push_back(std::move(msg));
    }

private:
    std::mutex inM_;
    std::vector<std::vector<std::uint8_t>> inQ_;
    // Sends the queued input messages in the current session
    // A touch is lifted no sooner than kMinHold after its press went out:
    // the press may have waited here a moment (the port busy), and HIPI's
    // debouncing needs it held about 60 ms
    void flushInput(SerialPort& port, const Link& link) {
        constexpr auto kMinHold = std::chrono::milliseconds(100);
        std::lock_guard<std::mutex> l(inM_);
        if (!link.inSession()) { inQ_.clear(); return; }
        std::size_t i = 0;
        for (; i < inQ_.size(); ++i) {
            auto& m = inQ_[i];
            const bool touch = m[0] == hipi::mirror::kMsgTouch;
            const bool lift = touch && m[6] == 0;
            if (lift && Clock::now() - touchDownSent_ < kMinHold) break;    // later
            m[1] = static_cast<std::uint8_t>(link.sid());
            if (!port.write(m.data(), m.size())) { ++i; break; }
            if (touch && !lift && !touchDown_) { touchDown_ = true; touchDownSent_ = Clock::now(); }
            if (lift) touchDown_ = false;
        }
        inQ_.erase(inQ_.begin(), inQ_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    bool touchDown_ = false;
    Clock::time_point touchDownSent_;

    void run() {
        while (!stop_) {
            std::string dev = dev_.empty() ? findMirrorPort() : dev_;
            SerialPort port;
            if (dev.empty() || !port.open(dev)) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
            { std::lock_guard<std::mutex> l(m_); cur_ = dev; }
            open_ = true;
            everOpen_ = true;
            std::printf("hipiview: %s open\n", dev.c_str());
            std::fflush(stdout);
            std::vector<std::uint8_t> buf(65536);
            // Ask HIPI for a session -- and again every 3 s until data comes
            // (e.g. HIPI was still starting up)
            auto lastAsk = Clock::now() - std::chrono::seconds(10);
            bool asked = false;
            Link link;
            const Link::Send send = [&port](const std::uint8_t* m, std::size_t n) {
                port.write(m, n);                // (a gone port is noticed below)
            };
            auto secs = [this] { return std::chrono::duration<double>(Clock::now() - t0_).count(); };
            auto ms = [this] { return static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0_).count()); };
            {
                // A (re)opened port: possibly a restarted HIPI -- start from
                // a black panel (the old picture stays until here)
                Chunk c{ ms(), {}, true };
                c.restart = true;
                inbox_.push(std::move(c));
            }
            while (!stop_) {
                // Ask HIPI for a session -- again every 3 s until it starts
                // (e.g. HIPI was still starting up), and anew if a lost
                // piece doesn't come back (HIPI gave up on this session)
                if (g_assetMissing.exchange(false) && link.inSession()) {
                    asked = false;
                    lastAsk = Clock::now() - std::chrono::seconds(10);   // at once
                }
                if (link.stuck(secs()) || link.silent(secs())) {
                    std::printf(link.stuck(secs()) ? "hipiview: data lost for good -- asking for the screen again\n"
                                                   : "hipiview: HIPI has gone quiet -- asking for the screen again\n");
                    std::fflush(stdout);
                    ++restarts_;
                    asked = false;
                    lastAsk = Clock::now() - std::chrono::seconds(10);   // at once
                }
                if ((!asked || !link.inSession()) && Clock::now() - lastAsk > std::chrono::seconds(3)) {
                    const std::vector<std::uint8_t> req = sessionRequest();
                    if (!port.write(req.data(), req.size())) break;
                    std::printf("hipiview: asked HIPI for the screen\n");
                    std::fflush(stdout);
                    lastAsk = Clock::now();
                    if (link.inSession()) inbox_.push(Chunk{ ms(), {}, true });
                    link.reset();
                    asked = true;
                }
                flushInput(port, link);
                // Wait for data at most 10 ms (acknowledgements and input go
                // out from here, and stop_ is noticed promptly)
                const int n = port.read(buf.data(), buf.size(), 10);
                if (n < 0) break;                                  // unplugged / reset
                if (n == 0) {
                    if (!port.stillThere()) break;
                    link.tick(secs(), send);
                    continue;
                }
                bytes_ += static_cast<std::uint64_t>(n);
                const auto now = Clock::now();
                lastData_ = now.time_since_epoch().count();
                const std::uint32_t t = ms();
                log_.write(Chunk{ t, std::vector<std::uint8_t>(buf.begin(), buf.begin() + n) });
                const bool was = link.inSession();
                const unsigned long long rec = link.recovered();
                Chunk payload{ t, {} };
                link.feed(buf.data(), static_cast<std::size_t>(n), secs(),
                          [&](const std::uint8_t* p, std::size_t k) { payload.data.insert(payload.data.end(), p, p + k); },
                          send);
                if (!was && link.inSession()) { std::printf("hipiview: data is coming\n"); std::fflush(stdout); }
                resends_ += link.recovered() - rec;
                if (!payload.data.empty()) inbox_.push(std::move(payload));
            }
            port.close();                    // (drops what's unsent: close() won't wait)
            open_ = false;
            std::printf("hipiview: %s closed\n", dev.c_str());
            std::fflush(stdout);
        }
    }

    std::string dev_;
    Inbox& inbox_;
    LogWriter& log_;
    Clock::time_point t0_;
    std::atomic<unsigned long long> resends_{ 0 }, restarts_{ 0 };
    std::thread th_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> finished_{ false };
    std::atomic<bool> open_{ false };
    std::atomic<bool> everOpen_{ false };
    std::atomic<Clock::rep> lastData_{ 0 };
    std::atomic<std::uint64_t> bytes_{ 0 };
    mutable std::mutex m_;
    std::string cur_;
};

// ── Video ───────────────────────────────────────────────────────────────
// Frames go to ffmpeg as raw BGRA (the emulator's 0xAARRGGBB pixels as
// they lie in memory on a little-endian PC)
class Recorder {
public:
    bool start(const std::string& path, int w, int h, int fps, int crf) {
        char cmd[1024];
        std::snprintf(cmd, sizeof cmd,
                      "ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgra -s %dx%d -r %d -i - "
                      "-c:v libx264 -preset veryfast -crf %d -pix_fmt yuv420p \"%s\"",
                      w, h, fps, crf, path.c_str());
        p_ = popen(cmd, kPipeWrite);
        if (!p_) { std::fprintf(stderr, "hipiview: can't run ffmpeg\n"); return false; }
        path_ = path;
        frames_ = 0;
        std::printf("hipiview: recording to %s\n", path.c_str());
        std::fflush(stdout);
        return true;
    }
    bool active() const { return p_ != nullptr; }
    void frame(const std::uint32_t* px, std::size_t count) {
        if (!p_) return;
        if (std::fwrite(px, 4, count, p_) != count) { std::fprintf(stderr, "hipiview: ffmpeg stopped\n"); stop(); return; }
        ++frames_;
    }
    long frames() const { return frames_; }
    void stop() {
        if (!p_) return;
        pclose(p_);
        p_ = nullptr;
        std::printf("hipiview: saved %s (%ld frames)\n", path_.c_str(), frames_);
        std::fflush(stdout);
    }
    ~Recorder() { stop(); }
private:
    FILE* p_ = nullptr;
    std::string path_;
    long frames_ = 0;
};

// ── Test input ──────────────────────────────────────────────────────────
// For tests (link_sim): mouse and key events from a file, played into SDL's
// event queue as if they came from the user, timed from when the screen is
// in step ("live"). One per line:
//   <seconds> down <x> <y> | move <x> <y> | up | key <name> | keyup <name> | wheel <n>
// (key names as SDL_GetKeyFromName() knows them, e.g. Return, Up, Tab)
class TestInput {
public:
    bool load(const std::string& path) {
        std::ifstream f(path);
        if (!f) return false;
        std::string line;
        while (std::getline(f, line)) {
            Item it;
            char what[32] = {}, arg[32] = {};
            if (std::sscanf(line.c_str(), "%lf %31s %31s %d", &it.t, what, arg, &it.b) < 2) continue;
            it.what = what;
            it.arg = arg;
            it.a = std::atoi(arg);
            items_.push_back(it);
        }
        return true;
    }
    // Pushes the events due at `sinceLive` seconds
    void play(double sinceLive, int& x, int& y) {
        while (next_ < items_.size() && items_[next_].t <= sinceLive) {
            const Item& it = items_[next_++];
            SDL_Event e{};
            if (it.what == "down" || it.what == "up") {
                if (it.what == "down") { x = it.a; y = it.b; }
                e.type = it.what == "down" ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
                e.button.button = SDL_BUTTON_LEFT;
                e.button.x = x;
                e.button.y = y;
            } else if (it.what == "move") {
                x = it.a; y = it.b;
                e.type = SDL_MOUSEMOTION;
                e.motion.x = x;
                e.motion.y = y;
            } else if (it.what == "key" || it.what == "keyup") {
                e.type = it.what == "key" ? SDL_KEYDOWN : SDL_KEYUP;
                e.key.keysym.sym = SDL_GetKeyFromName(it.arg.c_str());
            } else if (it.what == "wheel") {
                e.type = SDL_MOUSEWHEEL;
                e.wheel.y = it.a;
            } else {
                continue;
            }
            SDL_PushEvent(&e);
        }
    }
    bool done() const { return next_ >= items_.size(); }
private:
    struct Item { double t = 0; std::string what, arg; int a = 0, b = 0; };
    std::vector<Item> items_;
    std::size_t next_ = 0;
};

// ── Offline rendering ───────────────────────────────────────────────────
int renderVideo(const Options& o) {
    std::vector<Chunk> log;
    bool framed = false;
    if (!readLog(o.replay, log, framed)) { std::fprintf(stderr, "hipiview: can't read %s\n", o.replay.c_str()); return 1; }
    if (log.empty()) { std::fprintf(stderr, "hipiview: %s is empty\n", o.replay.c_str()); return 1; }
    LT7683Emu emu;
    attachCache(emu);
    std::size_t next = 0;
    // Start at the greeting, end a second after the last data
    ReplayFeeder feeder(emu, framed);
    while (next < log.size() && !emu.connected()) { feeder.feed(log[next]); ++next; }
    const double start = next > 0 ? log[next - 1].ms : 0;
    const double end = log.back().ms + 1000.0;
    Recorder rec;
    if (!rec.start(o.render, emu.width(), emu.height(), o.fps, o.crf)) return 1;
    std::vector<std::uint32_t> px(static_cast<std::size_t>(emu.width()) * emu.height());
    TouchMarks marks;
    for (long k = 0;; ++k) {
        const double t = start + k * 1000.0 / o.fps;
        if (t > end) break;
        while (next < log.size() && log[next].ms <= t) {
            if (log[next].input) marks.apply(log[next].data.data(), log[next].data.size(), log[next].ms / 1000.0);
            feeder.feed(log[next]);
            ++next;
        }
        emu.render(px.data(), (t - start) / 1000.0, o.backlight);
        if (o.marks) marks.draw(px.data(), emu.width(), emu.height(), t / 1000.0);
        rec.frame(px.data(), px.size());
        if (!rec.active()) return 1;
    }
    rec.stop();
    return 0;
}

std::string mmss(double sec) {
    char b[32];
    const int s = static_cast<int>(sec);
    std::snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60);
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o)) { usage(); return 2; }
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);       // ffmpeg gone: a failed write, not death
#endif
    if (!o.render.empty()) return renderVideo(o);

    const auto t0 = Clock::now();
    Inbox inbox;
    LogWriter log;
    if (!o.log.empty() && !log.open(o.log)) { std::fprintf(stderr, "hipiview: can't write %s\n", o.log.c_str()); return 1; }

    std::vector<Chunk> replay;
    bool replayFramed = true;
    std::size_t replayNext = 0;
    SerialReader* reader = nullptr;
    if (!o.replay.empty()) {
        if (!readLog(o.replay, replay, replayFramed)) { std::fprintf(stderr, "hipiview: can't read %s\n", o.replay.c_str()); return 1; }
    } else {
        reader = new SerialReader(o.device, inbox, log, t0);
        reader->start();
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    LT7683Emu emu;
    attachCache(emu);
    ReplayFeeder feeder(emu, replayFramed);
    int W = emu.width(), H = emu.height();
    SDL_Window* win = SDL_CreateWindow("hipiview", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       static_cast<int>(W * o.scale), static_cast<int>(H * o.scale),
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_RenderSetLogicalSize(ren, W, H);
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    std::vector<std::uint32_t> px(static_cast<std::size_t>(W) * H);

    Recorder rec;
    Clock::time_point recStart;
    auto startRecording = [&](const std::string& path) {
        if (rec.start(path, W, H, o.fps, o.crf)) recStart = Clock::now();
    };
    if (!o.record.empty()) startRecording(o.record);

    // The mouse and keys as HIPI's touch screen and buttons (input.hpp);
    // only live, and only if HIPI's Mirror to PC is set to Control
    auto secsNow = [&] { return std::chrono::duration<double>(Clock::now() - t0).count(); };
    TouchMarks marks;
    auto lastTitle = Clock::time_point();
    RemoteInput input([&](std::vector<std::uint8_t> m) {
        if (!reader) return;
        if (m[0] == hipi::mirror::kMsgTouch) {
            // Kept in the log too, for the rings in a replay or video
            Chunk c{ static_cast<std::uint32_t>(secsNow() * 1000), { m[0], m[2], m[3], m[4], m[5], m[6] } };
            c.input = true;
            log.write(c);
        }
        reader->sendInput(std::move(m));
    });
    auto inPicture = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H; };
    // HIPI's setting is View: clicks and keys would go nowhere -- say so
    // in the title bar instead (for a while after trying)
    double viewOnlyHintUntil = -1;
    auto controlOff = [&](double now) {
        if (emu.mirrorMode() != 1) return false;          // Control, or not told (older firmware)
        viewOnlyHintUntil = now + 5.0;
        lastTitle = Clock::time_point();                   // show it at once
        return true;
    };
    auto clampX = [&](int x) { return std::max(0, std::min(W - 1, x)); };
    auto clampY = [&](int y) { return std::max(0, std::min(H - 1, y)); };

    TestInput testInput;
    if (!o.testInput.empty() && !testInput.load(o.testInput)) {
        std::fprintf(stderr, "hipiview: can't read %s\n", o.testInput.c_str());
        return 1;
    }
    double liveAt = -1;
    int testX = 0, testY = 0;

    bool running = true, fullscreen = false;
    while (running) {
        if (!o.testInput.empty()) {
            if (liveAt < 0 && emu.connected() && !emu.syncing()) liveAt = secsNow();
            if (liveAt >= 0) testInput.play(secsNow() - liveAt, testX, testY);
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            const double tnow = secsNow();
            // The mouse: a finger on the panel (coordinates are the
            // picture's own, SDL_RenderSetLogicalSize() scales them)
            if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT && reader &&
                inPicture(ev.button.x, ev.button.y)) {
                if (controlOff(tnow)) continue;
                SDL_CaptureMouse(SDL_TRUE);          // a swipe may end outside the window
                input.touchDown(ev.button.x, ev.button.y, tnow);
                marks.down(ev.button.x, ev.button.y);
                continue;
            }
            if (ev.type == SDL_MOUSEMOTION && input.touching()) {
                input.touchMove(clampX(ev.motion.x), clampY(ev.motion.y), tnow);
                marks.move(clampX(ev.motion.x), clampY(ev.motion.y));
                continue;
            }
            if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT && input.touching()) {
                SDL_CaptureMouse(SDL_FALSE);
                input.touchUp(tnow);
                marks.up(tnow);
                continue;
            }
            if (ev.type == SDL_MOUSEWHEEL && reader) {
                if (controlOff(tnow)) continue;
                if (ev.wheel.y > 0) input.tap(RemoteInput::kUp);
                else if (ev.wheel.y < 0) input.tap(RemoteInput::kDown);
                continue;
            }
            // HIPI's buttons on the keyboard
            if ((ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) && reader) {
                std::uint8_t code = 0;
                switch (ev.key.keysym.sym) {
                case SDLK_RETURN: case SDLK_KP_ENTER: code = RemoteInput::kOk; break;
                case SDLK_UP:        code = RemoteInput::kUp; break;
                case SDLK_DOWN:      code = RemoteInput::kDown; break;
                case SDLK_BACKSPACE: code = RemoteInput::kX; break;
                case SDLK_TAB:       code = RemoteInput::kShift; break;
                case SDLK_RIGHT:     code = RemoteInput::kNextView; break;
                case SDLK_LEFT:      code = RemoteInput::kPrevView; break;
                default: break;
                }
                if (code != 0) {
                    const bool pressed = ev.type == SDL_KEYDOWN;
                    if (pressed && controlOff(tnow)) continue;
                    if (pressed && ev.key.repeat) continue;   // HIPI repeats ▲/▼ itself
                    // Shift held on the PC: HIPI's Shift first (Shift+Enter = EXIT ...)
                    if (pressed && (ev.key.keysym.mod & KMOD_SHIFT) && code >= RemoteInput::kOk &&
                        code <= RemoteInput::kX)
                        input.tap(RemoteInput::kShift);
                    input.button(code, pressed, tnow);
                    continue;
                }
            }
            if (ev.type != SDL_KEYDOWN) continue;
            switch (ev.key.keysym.sym) {
            case SDLK_q: running = false; break;
            case SDLK_ESCAPE:
                if (fullscreen) { fullscreen = false; SDL_SetWindowFullscreen(win, 0); }
                break;
            case SDLK_t: o.marks = !o.marks; break;
            case SDLK_r:
                if (rec.active()) rec.stop();
                else startRecording(timestampName(".mp4"));
                break;
            case SDLK_s: {
                const std::string f = timestampName(".png");
                if (writePng(f, px.data(), W, H)) std::printf("hipiview: saved %s\n", f.c_str());
                std::fflush(stdout);
                break;
            }
            case SDLK_f:
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                break;
            case SDLK_b: o.backlight = !o.backlight; break;
            default: break;
            }
        }

        // New data
        const auto now = Clock::now();
        const double sec = std::chrono::duration<double>(now - t0).count();
        if (reader) {
            for (Chunk& c : inbox.take()) {
                if (c.restart) emu.powerOff();
                if (c.desync) emu.desync();
                emu.feed(c.data.data(), c.data.size());
            }
        } else {
            while (replayNext < replay.size() && replay[replayNext].ms <= sec * 1000.0) {
                const Chunk& c = replay[replayNext];
                if (c.input) marks.apply(c.data.data(), c.data.size(), c.ms / 1000.0);
                feeder.feed(c);
                ++replayNext;
            }
        }
        if (emu.width() != W || emu.height() != H) {     // another panel size
            W = emu.width(); H = emu.height();
            SDL_DestroyTexture(tex);
            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
            SDL_RenderSetLogicalSize(ren, W, H);
            px.assign(static_cast<std::size_t>(W) * H, 0);
        }

        input.tick(sec);
        emu.render(px.data(), sec, o.backlight);
        if (o.marks) marks.draw(px.data(), W, H, sec);
        SDL_UpdateTexture(tex, nullptr, px.data(), W * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);

        // Video: a constant frame rate in wall-clock time (frames are
        // repeated if the window falls behind)
        if (rec.active()) {
            const double recSec = std::chrono::duration<double>(now - recStart).count();
            while (rec.active() && rec.frames() <= static_cast<long>(recSec * o.fps)) rec.frame(px.data(), px.size());
        }

        if (now - lastTitle > std::chrono::milliseconds(500)) {
            lastTitle = now;
            std::string state;
            if (reader) {
                const double quiet = std::chrono::duration<double>(now - reader->lastData()).count();
                if (!reader->open() && emu.saidBye())
                    state = "HIPI is in Bootsel mode - waiting for it to come back";
                else if (!reader->open() && reader->everOpen())
                    state = "HIPI is gone - waiting for it to come back (the picture as last seen)";
                else if (!reader->open()) state = o.device.empty() ? "looking for HIPI's mirror port..." : "waiting for " + o.device;
                else if (!emu.connected())
                    state = reader->bytes() == 0 && quiet > 3 ? "no answer from HIPI - is Settings > Screen > Mirror to PC on?"
                          : reader->bytes() > 0 ? "data, but no greeting yet (" + std::to_string(reader->bytes()) + " bytes)"
                          : "connecting...";
                else if (emu.syncing()) state = "reading the screen " + std::to_string(emu.syncPercent()) + "%";
                else state = "live";
                if (emu.streamErrors() > 0) state += "  |  stream errors " + std::to_string(emu.streamErrors());
                if (reader->open()) state = reader->device() + " - " + state;
            } else {
                state = "replay " + o.replay + (replayNext >= replay.size() ? " (end)" : "");
            }
            if (reader && reader->resends() > 0) state += "  |  resent: " + std::to_string(reader->resends());
            if (reader && reader->restarts() > 0) state += "  |  restarts: " + std::to_string(reader->restarts());
            if (rec.active()) state += "  |  REC " + mmss(rec.frames() / static_cast<double>(o.fps));
            if (o.backlight) state += "  |  backlight";
            if (!o.marks) state += "  |  no touch rings";
            if (reader && emu.connected() && emu.mirrorMode() == 2) state += "  |  control";
            if (viewOnlyHintUntil > sec)
                state = "VIEW ONLY - set Mirror to PC to Control on HIPI to use the mouse and keys  |  " + state;
            SDL_SetWindowTitle(win, ("hipiview - " + state).c_str());
        }
        // At most ~60 frames a second, also without vsync
        const auto spent = Clock::now() - now;
        if (spent < std::chrono::milliseconds(16))
            std::this_thread::sleep_for(std::chrono::milliseconds(16) - spent);
    }

    SDL_HideWindow(win);                 // gone at once, whatever follows
    rec.stop();
    if (reader) { reader->stop(); if (reader->stopped()) delete reader; }
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
