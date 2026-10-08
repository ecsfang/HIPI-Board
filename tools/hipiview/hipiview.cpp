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
#include <zlib.h>

#include <SDL2/SDL.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

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
        "keys: R record, S screenshot, F full screen, B backlight, Q/Esc quit\n");
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

// ── Finding the port ────────────────────────────────────────────────────
// Every USB serial port has its interface name in sysfs; HIPI's mirror
// port is called "HIPI Display Mirror" (my_descriptors.c)
std::string findMirrorPort() {
    DIR* d = opendir("/sys/class/tty");
    if (!d) return "";
    std::string found;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name.rfind("ttyACM", 0) != 0) continue;
        std::ifstream f("/sys/class/tty/" + name + "/device/interface");
        std::string iface;
        std::getline(f, iface);
        if (iface.find("HIPI Display Mirror") != std::string::npos) { found = "/dev/" + name; break; }
    }
    closedir(d);
    return found;
}

// ── Asset cache ─────────────────────────────────────────────────────────
// Layer pictures HIPI sends only once (the Tape view's pictures, ...) are
// kept in ~/.cache/hipiview as <id>.asset; the next session only names
// them. A changed picture on HIPI's SD card gets a new id.
std::string cacheDir() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    std::string base = xdg && *xdg ? xdg : std::string(home ? home : ".") + "/.cache";
    mkdir(base.c_str(), 0755);
    const std::string dir = base + "/hipiview";
    mkdir(dir.c_str(), 0755);
    return dir;
}

std::string assetPath(std::uint32_t id) {
    char name[32];
    std::snprintf(name, sizeof name, "/%08x.asset", id);
    return cacheDir() + name;
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
    if (std::fclose(f) == 0 && ok) std::rename(tmp.c_str(), path.c_str());
    else std::remove(tmp.c_str());
}

// The ids in the cache, newest first (at most 32 -- what a request holds)
std::vector<std::uint32_t> cachedAssets() {
    std::vector<std::pair<long, std::uint32_t>> found;
    const std::string dir = cacheDir();
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            unsigned id = 0;
            char tail[16] = {};
            if (std::sscanf(e->d_name, "%8x.%15s", &id, tail) == 2 && std::strcmp(tail, "asset") == 0) {
                struct stat st{};
                stat((dir + "/" + e->d_name).c_str(), &st);
                found.emplace_back(static_cast<long>(st.st_mtime), id);
            }
        }
        closedir(d);
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
struct Chunk { std::uint32_t ms; std::vector<std::uint8_t> data; bool desync = false; };

class Inbox {
public:
    void push(Chunk c) { std::lock_guard<std::mutex> l(m_); q_.push_back(std::move(c)); }
    std::vector<Chunk> take() { std::lock_guard<std::mutex> l(m_); std::vector<Chunk> out; out.swap(q_); return out; }
private:
    std::mutex m_;
    std::vector<Chunk> q_;
};

// Raw stream log: "HIPIVIEWLOG3\n" (the raw stream, numbered frames;
// "...LOG1" logs are from before frames existed), then records of
// [ms since start (u32 LE)] [length (u32 LE)] [bytes]
class LogWriter {
public:
    bool open(const std::string& path) {
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) return false;
        std::fputs("HIPIVIEWLOG3\n", f_);
        return true;
    }
    ~LogWriter() { if (f_) std::fclose(f_); }
    void write(const Chunk& c) {
        if (!f_) return;
        const std::uint32_t hdr[2] = { c.ms, static_cast<std::uint32_t>(c.data.size()) };
        std::fwrite(hdr, sizeof hdr, 1, f_);
        std::fwrite(c.data.data(), 1, c.data.size(), f_);
    }
private:
    FILE* f_ = nullptr;
};

bool readLog(const std::string& path, std::vector<Chunk>& out, bool& framed) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[13] = {};
    if (std::fread(magic, 1, 13, f) != 13) { std::fclose(f); return false; }
    if (std::memcmp(magic, "HIPIVIEWLOG3\n", 13) == 0) framed = true;
    else if (std::memcmp(magic, "HIPIVIEWLOG1\n", 13) == 0) framed = false;
    else {
        std::fprintf(stderr, "hipiview: %s is from another hipiview version\n", path.c_str());
        std::fclose(f);
        return false;
    }
    std::uint32_t hdr[2];
    while (std::fread(hdr, sizeof hdr, 1, f) == 1) {
        Chunk c{ hdr[0], std::vector<std::uint8_t>(hdr[1]) };
        if (hdr[1] && std::fread(c.data.data(), 1, hdr[1], f) != hdr[1]) break;
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
    unsigned long long resends() const { return resends_; }
    unsigned long long restarts() const { return restarts_; }
    bool stopped() const { return finished_; }
    std::string device() const { std::lock_guard<std::mutex> l(m_); return cur_; }
    Clock::time_point lastData() const { return Clock::time_point(Clock::duration(lastData_.load())); }
    std::uint64_t bytes() const { return bytes_; }

private:
    void run() {
        while (!stop_) {
            std::string dev = dev_.empty() ? findMirrorPort() : dev_;
            int fd = dev.empty() ? -1 : ::open(dev.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
            if (fd < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
            termios tio{};
            tcgetattr(fd, &tio);
            cfmakeraw(&tio);
            tio.c_cflag |= CLOCAL | CREAD;
            tio.c_cc[VMIN] = 0;
            tio.c_cc[VTIME] = 1;                 // 100 ms: lets the thread notice stop_
            tcsetattr(fd, TCSANOW, &tio);
            int bits = TIOCM_DTR | TIOCM_RTS;    // DTR on: HIPI starts a session
            ioctl(fd, TIOCMBIS, &bits);
            { std::lock_guard<std::mutex> l(m_); cur_ = dev; }
            open_ = true;
            std::printf("hipiview: %s open\n", dev.c_str());
            std::fflush(stdout);
            std::vector<std::uint8_t> buf(65536);
            // Ask HIPI for a session -- and again every 3 s until data comes
            // (e.g. HIPI was still starting up)
            auto lastAsk = Clock::now() - std::chrono::seconds(10);
            bool asked = false;
            Link link;
            const Link::Send send = [fd](const std::uint8_t* m, std::size_t n) {
                if (::write(fd, m, n) < 0) { /* the port is gone: noticed below */ }
            };
            auto secs = [this] { return std::chrono::duration<double>(Clock::now() - t0_).count(); };
            auto ms = [this] { return static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0_).count()); };
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
                    if (::write(fd, req.data(), req.size()) < 0) break;
                    std::printf("hipiview: asked HIPI for the screen\n");
                    std::fflush(stdout);
                    lastAsk = Clock::now();
                    if (link.inSession()) inbox_.push(Chunk{ ms(), {}, true });
                    link.reset();
                    asked = true;
                }
                // Wait for data at most 20 ms (acknowledgements go out from
                // here, and stop_ is noticed promptly)
                pollfd pfd{ fd, POLLIN, 0 };
                const int pr = ::poll(&pfd, 1, 20);
                if (pr < 0) { if (errno == EINTR) continue; break; }
                if (pr == 0) {
                    if (access(dev.c_str(), F_OK) != 0) break;    // unplugged / reset
                    link.tick(secs(), send);
                    continue;
                }
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
                const ssize_t n = ::read(fd, buf.data(), buf.size());
                if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
                if (n == 0) {
                    // Timeout -- or the device is gone (unplugged/reset)
                    if (access(dev.c_str(), F_OK) != 0) break;
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
            tcflush(fd, TCIOFLUSH);          // nothing left to send: close() won't wait
            ::close(fd);
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
        p_ = popen(cmd, "w");
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
    for (long k = 0;; ++k) {
        const double t = start + k * 1000.0 / o.fps;
        if (t > end) break;
        while (next < log.size() && log[next].ms <= t) { feeder.feed(log[next]); ++next; }
        emu.render(px.data(), (t - start) / 1000.0, o.backlight);
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
    std::signal(SIGPIPE, SIG_IGN);
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

    bool running = true, fullscreen = false;
    auto lastTitle = Clock::time_point();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type != SDL_KEYDOWN) continue;
            switch (ev.key.keysym.sym) {
            case SDLK_q: case SDLK_ESCAPE: running = false; break;
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
                if (c.desync) emu.desync();
                emu.feed(c.data.data(), c.data.size());
            }
        } else {
            while (replayNext < replay.size() && replay[replayNext].ms <= sec * 1000.0) {
                feeder.feed(replay[replayNext]);
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

        emu.render(px.data(), sec, o.backlight);
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
                if (!reader->open()) state = o.device.empty() ? "looking for HIPI's mirror port..." : "waiting for " + o.device;
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
