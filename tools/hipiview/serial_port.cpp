// serial_port.cpp -- see serial_port.hpp. POSIX (Linux, macOS) first,
// then Win32.
#include "serial_port.hpp"
#include <cstdio>
#include <cstring>

#ifndef _WIN32
// ════════════════════════════════════════════════════════════════════════
//  POSIX
// ════════════════════════════════════════════════════════════════════════
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

bool SerialPort::open(const std::string& name) {
    close();
    fd_ = ::open(name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) return false;
    termios tio{};
    tcgetattr(fd_, &tio);
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    tcsetattr(fd_, TCSANOW, &tio);
    int bits = TIOCM_DTR | TIOCM_RTS;    // DTR on: HIPI starts a session
    ioctl(fd_, TIOCMBIS, &bits);
    name_ = name;
    return true;
}

void SerialPort::close() {
    if (fd_ < 0) return;
    tcflush(fd_, TCIOFLUSH);             // nothing left to send: close() won't wait
    ::close(fd_);
    fd_ = -1;
}

bool SerialPort::isOpen() const { return fd_ >= 0; }

int SerialPort::read(std::uint8_t* buf, std::size_t n, int timeoutMs) {
    if (fd_ < 0) return -1;
    pollfd pfd{ fd_, POLLIN, 0 };
    const int pr = ::poll(&pfd, 1, timeoutMs);
    if (pr < 0) return errno == EINTR ? 0 : -1;
    if (pr == 0) return 0;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    const ssize_t r = ::read(fd_, buf, n);
    if (r < 0) return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    if (r == 0) return stillThere() ? 0 : -1;
    return static_cast<int>(r);
}

bool SerialPort::write(const std::uint8_t* data, std::size_t n) {
    if (fd_ < 0) return false;
    while (n > 0) {
        const ssize_t w = ::write(fd_, data, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) return true;    // full: dropped (the link sends again)
            return false;
        }
        data += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

bool SerialPort::stillThere() const { return fd_ >= 0 && access(name_.c_str(), F_OK) == 0; }

// Linux: every USB serial port has its interface name in sysfs
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

#else
// ════════════════════════════════════════════════════════════════════════
//  Win32
// ════════════════════════════════════════════════════════════════════════
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>
#include <setupapi.h>
#include <cwchar>

bool SerialPort::open(const std::string& name) {
    close();
    // COM10 and up only open as \\.\COM10 -- the prefix works for all
    const std::string path = name.rfind("\\\\.\\", 0) == 0 ? name : "\\\\.\\" + name;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    SetupComm(h, 1 << 16, 1 << 16);
    DCB dcb{};
    dcb.DCBlength = sizeof dcb;
    GetCommState(h, &dcb);
    dcb.BaudRate = 115200;               // (USB CDC: the speed doesn't matter)
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    // DTR on: HIPI only starts a session for a port that is really open --
    // Windows, unlike Linux and macOS, doesn't set it by itself
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    SetCommState(h, &dcb);
    EscapeCommFunction(h, SETDTR);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    h_ = h;
    timeoutMs_ = -1;
    return true;
}

void SerialPort::close() {
    if (h_ == nullptr) return;
    PurgeComm(static_cast<HANDLE>(h_), PURGE_TXABORT | PURGE_TXCLEAR);   // don't wait for a gone HIPI
    CloseHandle(static_cast<HANDLE>(h_));
    h_ = nullptr;
}

bool SerialPort::isOpen() const { return h_ != nullptr; }

int SerialPort::read(std::uint8_t* buf, std::size_t n, int timeoutMs) {
    if (h_ == nullptr) return -1;
    HANDLE h = static_cast<HANDLE>(h_);
    if (timeoutMs != timeoutMs_) {
        // Return at once with what has arrived; if nothing has, wait for
        // the first byte at most timeoutMs
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = MAXDWORD;
        to.ReadTotalTimeoutMultiplier = MAXDWORD;
        to.ReadTotalTimeoutConstant = static_cast<DWORD>(timeoutMs > 0 ? timeoutMs : 1);
        to.WriteTotalTimeoutConstant = 1000;
        SetCommTimeouts(h, &to);
        timeoutMs_ = timeoutMs;
    }
    DWORD got = 0;
    if (!ReadFile(h, buf, static_cast<DWORD>(n), &got, nullptr)) return -1;     // unplugged
    if (got == 0 && !stillThere()) return -1;
    return static_cast<int>(got);
}

bool SerialPort::write(const std::uint8_t* data, std::size_t n) {
    if (h_ == nullptr) return false;
    DWORD put = 0;
    return WriteFile(static_cast<HANDLE>(h_), data, static_cast<DWORD>(n), &put, nullptr) != 0;
}

bool SerialPort::stillThere() const {
    if (h_ == nullptr) return false;
    DWORD errors = 0;
    COMSTAT st{};
    return ClearCommError(static_cast<HANDLE>(h_), &errors, &st) != 0;
}

// Windows: the COM ports present, by their USB ids (instance id
// "USB\VID_2E8A&PID_000B&MI_07\...") or the interface name the device
// reports ("HIPI Display Mirror")
std::string findMirrorPort() {
    HDEVINFO set = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return "";
    char wantId[64];
    std::snprintf(wantId, sizeof wantId, "VID_%04X&PID_%04X&MI_%02X", kHipiVid, kHipiPid, kMirrorInterface);
    std::string found;
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof dev;
    for (DWORD i = 0; found.empty() && SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        char id[512] = {};
        SetupDiGetDeviceInstanceIdA(set, &dev, id, sizeof id, nullptr);
        for (char* p = id; *p; ++p) if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 'a' + 'A');
        bool match = std::strstr(id, wantId) != nullptr;
        if (!match) {
            // The name HIPI gives its interface, as Windows got it from the device
            DEVPROPTYPE type = 0;
            wchar_t desc[256] = {};
            if (SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_Device_BusReportedDeviceDesc, &type,
                                          reinterpret_cast<PBYTE>(desc), sizeof desc, nullptr, 0))
                match = std::wcsstr(desc, L"HIPI Display Mirror") != nullptr;
        }
        if (!match) continue;
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        char port[64] = {};
        DWORD size = sizeof port, type = 0;
        if (RegQueryValueExA(key, "PortName", nullptr, &type, reinterpret_cast<LPBYTE>(port), &size) == ERROR_SUCCESS)
            found = port;
        RegCloseKey(key);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}
#endif
