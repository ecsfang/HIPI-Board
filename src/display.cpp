#include <stdio.h>
#include <cctype>
#include "display.h"
#include "backlight.h"

#include "display_config.h"
#include "Screen.hpp"
#include "video80.h"

extern hipi::Screen *screen;

#include "usb_serial.h"
void CDisplay::setModel(DisplayModel m)
{
    if (m == DisplayModel::Hp92198 && video80_ == nullptr) m = DisplayModel::Hp82163;
    model_ = m;
    m_nSai = isVideo80() ? kSai92198 : kSai82163;
    fifo = std::queue<unsigned char>();
    statusLeft_ = 0;
}

const char* CDisplay::deviceId()
{
    return isVideo80() ? "MC00701A\r\n" : CDevice::deviceId();
}

void CDisplay::clear(void)
{
    // Clear device (DCL / SDC): the 92198 also goes back to its defaults
    if (isVideo80()) video80_->hardReset();
    else screen->clear();
    fifo = std::queue<unsigned char>();
    statusLeft_ = 0;
}

void CDisplay::idle(void)
{
    if( !fifo.empty() ) {
        hipi::backlight_activity();     // new output: screen in use (backlight.h)
        if (isVideo80()) video80_->put(fifo.front());
        else screen->pr_char(fifo.front());
        fifo.pop();
    }
}

// 92198 only: SST is $80, then the device dependent byte -- each sent
// when the one before comes back round the loop -- then ETO. SDA: ETO.
// (The 82163 answers neither: the frame passes on.)
void CDisplay::doTalker(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    if (!isVideo80()) return;
    if (cmd == NRD) {
        if (statusLeft_ > 1) statusLeft_ = 1;
        return;
    }
    if (cmd == SST) {
        statusByte_ = video80_->status();
        *rtn = 0x80;
        statusLeft_ = 2;
        return;
    }
    if (cmd == SDA) {
        *rtn = ETO;
        return;
    }
    if (IS_DATA(cmd) && statusLeft_ > 0) {
        *rtn = statusLeft_ == 2 ? static_cast<IL_CMD_t>(statusByte_) : static_cast<IL_CMD_t>(ETO);
        --statusLeft_;
    }
}

// Interface clear
void CDisplay::ifc(void)
{
    status(STAT_IDLE);
}

void CDisplay::doListener(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    *rtn = cmd;
    if( IS_DATA(cmd) ) {
        // Data - save in queue ...
        //LOGF("Push %03X\r\n", cmd);
        fifo.push(cmd & 0xFF);
        ++totalPushed_;
    }
}

void CDisplay::show()
{
    CDevice::show();
    LOGF(" model:%s fifo size:%d", isVideo80() ? "92198" : "82163", fifo.size());
}
