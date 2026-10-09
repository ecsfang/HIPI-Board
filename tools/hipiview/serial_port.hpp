// serial_port.hpp -- the mirror port, the same way on Linux, macOS and
// Windows (serial_port.cpp has one implementation for POSIX and one for
// Win32). hipiview only needs a few things from it: open the port raw,
// with DTR on (HIPI only starts a session for a port that is really
// open), read with a timeout, write, and notice when the port is gone
// (HIPI unplugged or restarted).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort() { close(); }
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    // name: /dev/ttyACM3, /dev/cu.usbmodem..., COM5
    bool open(const std::string& name);
    // Closes it; anything still waiting to be sent is dropped
    void close();
    bool isOpen() const;

    // Waits at most timeoutMs for data. Returns the number of bytes read,
    // 0 if nothing came, -1 if the port is gone.
    int read(std::uint8_t* buf, std::size_t n, int timeoutMs);
    // false if the port is gone
    bool write(const std::uint8_t* data, std::size_t n);
    // The device still exists (checked when nothing arrives for a while)
    bool stillThere() const;

private:
#ifdef _WIN32
    void* h_ = nullptr;          // HANDLE
    int timeoutMs_ = -1;         // the read timeout last set
#else
    int fd_ = -1;
    std::string name_;
#endif
};

// The mirror port of a connected HIPI, or "" if none is found.
//   Linux:   the serial port whose USB interface is "HIPI Display Mirror"
//   Windows: the COM port of HIPI's USB interface 7 (VID 2E8A, PID 000B),
//            or whose USB interface name is "HIPI Display Mirror"
//   macOS:   not yet -- give the port (/dev/cu.usbmodem...) on the command line
std::string findMirrorPort();

// The USB ids that findMirrorPort() looks for (my_descriptors.c)
constexpr unsigned kHipiVid = 0x2E8A, kHipiPid = 0x000B, kMirrorInterface = 7;
