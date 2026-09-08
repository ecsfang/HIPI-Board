// SPDX-License-Identifier: MIT
//
// Abstract transport interface for the RA8875 LCD driver.
//
// Implement this for your platform (Linux spidev, ESP-IDF, STM32 HAL,
// Arduino, etc.) and pass an instance to the RA8875 constructor.

#pragma once

#include <cstddef>
#include <cstdint>

namespace hipi {

class RA8875Transport {
public:
    virtual ~RA8875Transport() = default;

    // Chip-select control.
    virtual void csLow()  = 0;
    virtual void csHigh() = 0;

    // Reset-pin control.
    virtual void rstLow()  = 0;
    virtual void rstHigh() = 0;

    // Full-duplex SPI transfer.  `tx` may equal `rx`.  If `tx` is null,
    // zeroes are shifted out and `rx` is filled.  `len` is in bytes.
    virtual void spiTransfer(const std::uint8_t* tx,
                             std::uint8_t*       rx,
                             std::size_t         len) = 0;

    // Millisecond delay.
    virtual void delayMs(std::uint32_t ms) = 0;

    // Microsecond delay -- for fine-grained busy-wait polling (see
    // LT7683::waitStatus()'s own comment for why this matters: polling
    // at 1ms granularity when the actual wait is more like tens of
    // microseconds wastes far more time than necessary, once every
    // character write ends with one of these waits). Default falls back
    // to delayMs() (rounded up to at least 1ms) for any transport that
    // doesn't override this with a real microsecond-granularity sleep --
    // correct either way, just coarser/slower on such a transport.
    virtual void delayUs(std::uint32_t us) {
        delayMs(us == 0 ? 0 : (us + 999) / 1000);
    }
};

}  // namespace hipi
