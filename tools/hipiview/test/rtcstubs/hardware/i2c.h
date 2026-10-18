// The Pico SDK's I2C calls for rtc_test: served by the made-up chips in
// test/rtc_test.cpp (fakeI2cWrite/fakeI2cRead)
#pragma once
#include <cstddef>
#include <cstdint>
typedef unsigned int uint;
struct i2c_inst_t { int n; };
inline i2c_inst_t i2cStub0{ 0 }, i2cStub1{ 1 };
#define i2c0 (&i2cStub0)
#define i2c1 (&i2cStub1)
#define PICO_ERROR_GENERIC (-1)
int fakeI2cWrite(std::uint8_t addr, const std::uint8_t* src, std::size_t len);
int fakeI2cRead(std::uint8_t addr, std::uint8_t* dst, std::size_t len);
inline uint i2c_init(i2c_inst_t*, uint baud) { return baud; }
inline int i2c_write_blocking(i2c_inst_t*, std::uint8_t addr, const std::uint8_t* src, std::size_t len, bool) {
    return fakeI2cWrite(addr, src, len);
}
inline int i2c_read_blocking(i2c_inst_t*, std::uint8_t addr, std::uint8_t* dst, std::size_t len, bool) {
    return fakeI2cRead(addr, dst, len);
}
