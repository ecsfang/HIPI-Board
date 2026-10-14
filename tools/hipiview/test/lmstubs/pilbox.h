// Host stand-in: no PILBOX on the loop (loopmap_test)
#pragma once
#include "hpil.h"
class CPilBox : public CDevice {
public:
    CPilBox() : CDevice("PILBOX", 0, 0, PILBOX) {}
    void clear() override {}
    IL_CMD_t mode() const { return TDIS; }
};
inline CPilBox* pilbox = nullptr;
