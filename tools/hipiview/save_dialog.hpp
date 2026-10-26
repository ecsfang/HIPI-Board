// save_dialog.hpp -- the system's "Save as" dialog, for a cassette from HIPI
// (download.hpp). SDL has none of its own, so:
//   Linux    GTK 3's own (through python3-gi, as on Ubuntu's desktop), else
//            zenity, else kdialog (KDE)
//   macOS    osascript ("choose file name")
//   Windows  GetSaveFileNameW
// It runs in a thread of its own: the window and the link to HIPI keep
// going while the user picks a folder.
#pragma once
#include <atomic>
#include <string>
#include <thread>

class SaveDialog {
public:
    ~SaveDialog() { if (th_.joinable()) th_.detach(); }   // (quitting with it open)

    // Opens it: in `dir`, proposing `name`
    void open(const std::string& title, const std::string& dir, const std::string& name);
    bool running() const { return th_.joinable() && !done_; }
    // Once it's closed: true, with the path chosen ("" = cancelled).
    // available false: there is no dialog here (no zenity/kdialog) -- the
    // caller saves somewhere by itself.
    bool finished(std::string& path, bool& available);
    // Whether the dialog itself asked before overwriting a file (else the
    // caller asks: zenity / kdialog don't)
    bool askedBeforeOverwriting() const { return asked_; }

private:
    std::thread th_;
    std::atomic<bool> done_{ false };
    std::string path_;
    bool available_ = true;
    bool asked_ = true;
};

// The folder files are saved in by default: the last one used (kept in
// hipiview's cache folder), else Downloads, else home. remember(): the
// folder of a file just saved.
std::string saveFolder(const std::string& cacheDir);
void rememberSaveFolder(const std::string& cacheDir, const std::string& file);
// path if nothing is there yet, else "name (2).ext", "name (3).ext" ...
std::string freePath(const std::string& path);
