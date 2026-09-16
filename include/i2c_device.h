#ifndef __I2C_DEVICE_H__
#define __I2C_DEVICE_H__

#include <cstdint>
#include <cstddef>
#include "hardware/i2c.h"
#include "hardware/gpio.h"

// ─────────────────────────────────────────────────────────────────────────────
//  CI2CBus -- shared-bus bookkeeping
//
//  This board's I2C bus (i2c0, pins 16/17 -- see touch.h's own extern
//  touch_i2c) is already claimed and configured by touchInit(), which runs
//  early in main() before any I2C-based device (like the temperature
//  sensor below) gets constructed. Every CI2CDevice-derived class shares
//  that SAME physical bus rather than owning its own -- markInitialized()
//  lets whichever code already did the real i2c_init()/gpio_set_function()
//  work (touchInit(), in this project) tell every device built on top of
//  CI2CDevice not to repeat it, so pico_main.cpp doesn't need to know or
//  care how many I2C devices exist -- it just marks the bus ready once,
//  right after touchInit(), and every device's own init() below then
//  skips hardware setup automatically.
// ─────────────────────────────────────────────────────────────────────────────
class CI2CBus {
public:
    // Called once (by pico_main.cpp, right after touchInit()) -- marks
    // `bus` as already configured in hardware, so CI2CDevice::init() (see
    // below) knows to skip i2c_init()/gpio_set_function() for it.
    static void markInitialized(i2c_inst_t* bus);

    // True if markInitialized() (or a previous CI2CDevice::init() that
    // itself performed the real hardware setup -- see its own comment)
    // has already been called for this bus.
    static bool isInitialized(i2c_inst_t* bus);

    // Probes every valid 7-bit I2C address (0x08-0x77 -- 0x00-0x07 and
    // 0x78-0x7F are reserved for special bus purposes, e.g. the general
    // call address, and are never scanned, matching every common I2C
    // scanner's own convention, including the Pico SDK's own bus_scan
    // example) and fills `found[addr]` = true for each one that
    // acknowledges. `found` must have at least 128 entries; entries
    // outside the scanned range are left untouched (the caller decides
    // how to render "not applicable" vs "no response" -- see
    // uidialog.hpp's own drawI2CScanGrid()). Takes only a few
    // milliseconds total -- each probe is a single zero-length write,
    // immediately NACKed or ACKed, no actual data transferred.
    static void scan(i2c_inst_t* bus, bool found[128]);

private:
    // Only ever i2c0 or i2c1 on this hardware -- two plain bools rather
    // than a container keyed by pointer, since there's nothing else it
    // could ever need to scale to.
    static bool i2c0Ready_;
    static bool i2c1Ready_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  CI2CDevice -- base class for a single device on a shared I2C bus
//
//  Handles the two things every I2C peripheral needs regardless of what
//  it actually is (a temperature sensor, or anything else built on this
//  later): getting the bus itself into a working state exactly once no
//  matter how many devices share it (see CI2CBus above), and simple
//  register read/write over that bus at this device's own 7-bit address.
//  Anything chip-specific (register map, compensation math, ...) belongs
//  in a subclass -- see bmp280.h for the first one.
// ─────────────────────────────────────────────────────────────────────────────
class CI2CDevice {
public:
    CI2CDevice(i2c_inst_t* bus, std::uint8_t address)
        : bus_(bus), address_(address) {}
    virtual ~CI2CDevice() = default;

    // Ensures the physical bus is ready (see CI2CBus's own comment for
    // why this is normally a no-op here -- touchInit() already did the
    // real work) and lets a subclass confirm its own device is actually
    // present/responding (chip-ID check, etc.) before use. sdaPin/sclPin/
    // baudrateHz are only ever actually used if CI2CBus::isInitialized()
    // is still false for this bus when called -- i.e. only if THIS
    // device happens to be the first thing to ever touch the bus, which
    // isn't how this project is wired up today (touchInit() always runs
    // first) but keeps this class correct standalone too, e.g. for a
    // future board revision or test rig without a touch controller.
    bool initBus(uint gpioSda, uint gpioScl, uint baudrateHz = 400000) {
        if (!CI2CBus::isInitialized(bus_)) {
            i2c_init(bus_, baudrateHz);
            gpio_set_function(gpioSda, GPIO_FUNC_I2C);
            gpio_set_function(gpioScl, GPIO_FUNC_I2C);
            CI2CBus::markInitialized(bus_);
        }
        return true;
    }

protected:
    // Writes `value` to `reg` -- the common single-byte-register-write
    // pattern (register address byte, then the one data byte, in a
    // single I2C transaction) essentially every simple I2C peripheral
    // uses for its own control registers.
    bool writeReg8(std::uint8_t reg, std::uint8_t value) {
        const std::uint8_t buf[2] = { reg, value };
        return i2c_write_blocking(bus_, address_, buf, 2, false) == 2;
    }

    // Reads `len` bytes starting at `reg` into `buf` -- writes the
    // register address first (repeated-start, no stop condition) then
    // reads the response, matching this project's own touch.cpp i2c_read()
    // helpers exactly (see FT5316/GSLX680's own read functions).
    bool readReg(std::uint8_t reg, std::uint8_t* buf, std::size_t len) {
        if (i2c_write_blocking(bus_, address_, &reg, 1, true) != 1) return false;
        return i2c_read_blocking(bus_, address_, buf, static_cast<uint>(len), false)
               == static_cast<int>(len);
    }

    bool readReg8(std::uint8_t reg, std::uint8_t& value) {
        return readReg(reg, &value, 1);
    }

    i2c_inst_t* bus_;
    std::uint8_t address_;
};

#endif//__I2C_DEVICE_H__
