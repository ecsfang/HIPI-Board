// A drive without the tape behind it, for the view tests (views.cpp needs the type)
#pragma once
#include "hpil.h"
#include <string>
enum class DrivePower { Off, Standby, On };
class CDrive : public CDevice {
public:
    CDrive() : CDevice("TFDRIVE", 0x10, 2, DRIVE) {}
    void clear() override {}
    const std::string& mediaFile() const { return file_; }
    void setStubFile(const std::string& f) { file_ = f; }
    static std::string lifPath(const std::string& file) { return file.empty() ? file : "lif/" + file; }
    bool ejected() const { return false; }
    bool poweredUp() const { return false; }
    bool busyLight() const { return false; }
    bool isBusy() const { return false; }
    bool isRewinding() const { return false; }
    DrivePower powerMode() const { return DrivePower::Off; }
private:
    std::string file_;
};
