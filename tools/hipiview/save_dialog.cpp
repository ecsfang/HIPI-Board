// save_dialog.cpp -- see save_dialog.hpp
#include "save_dialog.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

#if defined(_WIN32)
std::wstring wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(static_cast<std::size_t>(n > 0 ? n - 1 : 0));
    return w;
}
std::string utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.resize(static_cast<std::size_t>(n > 0 ? n - 1 : 0));
    return s;
}

std::string runDialog(const std::string& title, const std::string& dir, const std::string& name, bool& available,
                      bool& asked) {
    available = true;
    asked = true;                           // (the system's dialog asks before overwriting)
    wchar_t file[MAX_PATH * 4] = {};
    const std::wstring wname = wide(name), wdir = wide(dir), wtitle = wide(title);
    wname.copy(file, MAX_PATH * 4 - 1);
    static const wchar_t kFilter[] = L"LIF volumes (*.dat, *.lif)\0*.dat;*.lif\0All files\0*.*\0\0";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrInitialDir = wdir.c_str();
    ofn.lpstrTitle = wtitle.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return "";
    return utf8(file);
}
#else
// A string for the shell, in single quotes
std::string shq(const std::string& s) {
    std::string q = "'";
    for (char c : s) {
        if (c == '\'') q += "'\\''";
        else q += c;
    }
    return q + "'";
}
// The command's output (its first line), and whether it said yes (exit 0)
bool run(const std::string& cmd, std::string& out) {
    out.clear();
    FILE* p = popen(cmd.c_str(), "r");
    if (p == nullptr) return false;
    char buf[1024];
    while (std::fgets(buf, sizeof buf, p) != nullptr) out += buf;
    const int st = pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return st == 0;
}

#if defined(__APPLE__)
// A string for AppleScript, in double quotes
std::string asq(const std::string& s) {
    std::string q = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') q += '\\';
        q += c;
    }
    return q + "\"";
}
std::string runDialog(const std::string& title, const std::string& dir, const std::string& name, bool& available,
                      bool& asked) {
    available = true;
    asked = true;                           // (the system's dialog asks before overwriting)
    const std::string script = "set f to choose file name with prompt " + asq(title) + " default name " + asq(name) +
                               " default location (POSIX file " + asq(dir) + ")\nPOSIX path of f";
    std::string out;
    return run("osascript -e " + shq(script) + " 2>/dev/null", out) ? out : "";
}
#else
bool have(const char* program) {
    return std::system((std::string("command -v ") + program + " >/dev/null 2>&1").c_str()) == 0;
}
// GTK 3's own Save dialog, from Python (python3-gi and GTK 3 come with
// Ubuntu's desktop): it takes a proposed name -- zenity 4 (GTK 4) ignores
// the name in --filename -- and asks before overwriting. Exit 1: cancelled.
const char* const kGtkSave = R"PY(
import sys, gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk
d = Gtk.FileChooserDialog(title=sys.argv[1], action=Gtk.FileChooserAction.SAVE)
d.add_buttons('_Cancel', Gtk.ResponseType.CANCEL, '_Save', Gtk.ResponseType.OK)
d.set_default_response(Gtk.ResponseType.OK)
d.set_do_overwrite_confirmation(True)
d.set_keep_above(True)
d.set_current_folder(sys.argv[2])
d.set_current_name(sys.argv[3])
f = Gtk.FileFilter(); f.set_name('LIF volumes (*.dat, *.lif)')
for p in ('*.dat', '*.DAT', '*.lif', '*.LIF'): f.add_pattern(p)
d.add_filter(f)
a = Gtk.FileFilter(); a.set_name('All files'); a.add_pattern('*'); d.add_filter(a)
ok = d.run() == Gtk.ResponseType.OK
if ok: print(d.get_filename())
sys.exit(0 if ok else 1)
)PY";
bool haveGtk() {
    return std::system("python3 -c \"import gi; gi.require_version('Gtk', '3.0'); from gi.repository import Gtk\" "
                       ">/dev/null 2>&1") == 0;
}
std::string runDialog(const std::string& title, const std::string& dir, const std::string& name, bool& available,
                      bool& asked) {
    const std::string start = (fs::path(dir) / name).string();
    std::string out;
    available = true;
    asked = true;
    if (haveGtk()) {
        const std::string cmd = "python3 -c " + shq(kGtkSave) + " " + shq(title) + " " + shq(dir) + " " + shq(name) +
                                " 2>/dev/null";
        const bool ok = run(cmd, out);
        return ok ? out : "";
    }
    asked = false;                          // (zenity / kdialog don't: the caller asks)
    if (have("zenity")) {
        const bool ok = run("zenity --file-selection --save --title=" + shq(title) + " --filename=" + shq(start) +
                            " --file-filter=" + shq("LIF volumes | *.dat *.DAT *.lif *.LIF") +
                            " --file-filter=" + shq("All files | *") + " 2>/dev/null", out);
        return ok ? out : "";
    }
    if (have("kdialog")) {
        const bool ok = run("kdialog --title " + shq(title) + " --getsavefilename " + shq(start) + " " +
                            shq("*.dat *.DAT *.lif *.LIF|LIF volumes") + " 2>/dev/null", out);
        return ok ? out : "";
    }
    available = false;
    return "";
}
#endif
#endif

}  // namespace

void SaveDialog::open(const std::string& title, const std::string& dir, const std::string& name) {
    if (th_.joinable()) th_.join();
    done_ = false;
    path_.clear();
    available_ = true;
    th_ = std::thread([this, title, dir, name] {
        bool available = true;
        bool asked = true;
        std::string path = runDialog(title, dir, name, available, asked);
        path_ = path;
        available_ = available;
        asked_ = asked;
        done_ = true;
    });
}

bool SaveDialog::finished(std::string& path, bool& available) {
    if (!th_.joinable() || !done_) return false;
    th_.join();
    path = path_;
    available = available_;
    return true;
}

std::string saveFolder(const std::string& cacheDir) {
    std::ifstream f((fs::path(cacheDir) / "save_folder.txt").string());
    std::string dir;
    std::error_code ec;
    if (f && std::getline(f, dir) && !dir.empty() && fs::is_directory(dir, ec)) return dir;
#if defined(_WIN32)
    const fs::path home = env("USERPROFILE").empty() ? fs::path(".") : fs::path(env("USERPROFILE"));
#else
    const fs::path home = env("HOME").empty() ? fs::path(".") : fs::path(env("HOME"));
#endif
    if (fs::is_directory(home / "Downloads", ec)) return (home / "Downloads").string();
    return home.string();
}

void rememberSaveFolder(const std::string& cacheDir, const std::string& file) {
    std::ofstream f((fs::path(cacheDir) / "save_folder.txt").string());
    if (f) f << fs::path(file).parent_path().string() << "\n";
}

std::string freePath(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return path;
    const fs::path p(path);
    for (int i = 2; i < 1000; ++i) {
        const fs::path q = p.parent_path() / (p.stem().string() + " (" + std::to_string(i) + ")" + p.extension().string());
        if (!fs::exists(q, ec)) return q.string();
    }
    return path;
}
