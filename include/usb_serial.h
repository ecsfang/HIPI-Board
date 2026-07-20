#pragma once
// usb_serial.h — simple CDC0 + CDC1 helpers.
//
// Call usb_init() once at startup instead of stdio_init_all().
// Use cdc0_printf() where you previously used printf().
// Use cdc1_write / cdc1_read_timeout for the second port.

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "pico/time.h"

// ── Initialisation ────────────────────────────────────────────────────────────
// Call once at the top of main(), replaces stdio_init_all().

static inline void usb_init(void) {
    tusb_init();
    // Give the host time to enumerate both ports
    sleep_ms(2000);
}

// ── USB task ──────────────────────────────────────────────────────────────────
// Call this regularly in your main loop (or after any USB operation) to keep
// TinyUSB running.  If your loop already does real work between USB calls,
// once per loop iteration is enough.

static inline void usb_task(void) {
    tud_task();
}

// ── CDC0 — debug console (replaces printf) ────────────────────────────────────

// ── Boot log buffer ─────────────────────────────────────────────────────────
// cdc0_write() drops anything written before a terminal has actually opened
// the CDC0 port (tud_cdc_n_connected(0)) -- and that can lag well behind
// tud_mounted()/usb_connected, since enumeration is fast but a human still
// has to go click open a terminal window. Buffer what's written before that
// point (capped, so it can't grow unbounded), and flush it out in one go
// the moment the port does connect, so early boot messages aren't silently
// lost just because you were a bit slow opening your terminal after a reset.
namespace usb_serial_detail {
constexpr size_t kBootLogCapacity = 4096;
inline char   bootLogBuf[kBootLogCapacity];
inline size_t bootLogLen = 0;
inline bool   bootLogFlushed = false;

// True once we've paid the one-time "settle" delay below. Guards against a
// known TinyUSB/host quirk: tud_cdc_n_connected() can report true slightly
// before the host OS has fully finished attaching its read pipe for the
// port, so the very first write attempt right after that transition can be
// silently swallowed on the host side even though the device thinks it's
// connected. Costs a few tens of ms, paid exactly once, at first connection.
inline bool   settled = false;
}  // namespace usb_serial_detail

static inline void cdc0_write(const char* buf, size_t len) {
    using namespace usb_serial_detail;

    if (!tud_cdc_n_connected(0)) {
        // Not connected yet -- stash what fits (silently drops anything
        // past the cap; better than blocking boot forever waiting for a
        // terminal that may never come).
        if (!bootLogFlushed) {
            const size_t room = kBootLogCapacity - bootLogLen;
            const size_t n = len < room ? len : room;
            memcpy(bootLogBuf + bootLogLen, buf, n);
            bootLogLen += n;
        }
        return;
    }

    if (!settled) {
        // First write attempt since connecting -- give the host's CDC-ACM
        // pipe a moment to finish settling before trusting it with actual
        // data (see comment on `settled` above).
        for (int i = 0; i < 5; ++i) { tud_task(); sleep_ms(10); }
        settled = true;
    }

    if (!bootLogFlushed && bootLogLen > 0) {
        // Flush the buffered backlog, in order, before this new message.
        size_t written = 0;
        while (written < bootLogLen) {
            uint32_t n = tud_cdc_n_write(0, bootLogBuf + written, bootLogLen - written);
            written += n;
            if (n == 0) tud_task();
        }
        tud_cdc_n_write_flush(0);
    }
    bootLogFlushed = true;

    size_t written = 0;
    while (written < len) {
        uint32_t n = tud_cdc_n_write(0, buf + written, len - written);
        written += n;
        if (n == 0) tud_task();   // TX buffer full — yield and retry
    }
    tud_cdc_n_write_flush(0);
}

// Call periodically (e.g. once per main-loop iteration) to flush any
// buffered boot-time log output as soon as the CDC port connects, without
// waiting for the next LOGF() call to happen to trigger it.
static inline void usb_serial_flush_boot_log(void) {
    using namespace usb_serial_detail;
    if (bootLogFlushed || bootLogLen == 0) return;
    if (!tud_cdc_n_connected(0)) return;
    cdc0_write("", 0);  // len=0 write -- takes the flush-backlog-first path above
}

// Drop-in replacement for printf — use cdc0_printf() in place of printf().
static inline void cdc0_printf(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0)
        cdc0_write(buf, (size_t)len);
}

// ── Logging gate ──────────────────────────────────────────────────────────────
// Gate on tud_mounted() directly (always current), not the cached
// `usb_connected` variable (set/refreshed elsewhere, e.g. for the USB status
// LED in boardui.cpp) -- gating on a value that can still be stale/false at
// the exact moment a given LOGF() call runs would skip that call forever,
// even though cdc0_write()'s own boot-log buffer could otherwise have
// captured it. tud_mounted() is a cheap flag read, safe to call every time.
//
// `usb_connected` itself is unrelated to LOGF now -- it's just a
// live-refreshed USB-status flag used for the status LED (see boardui.cpp).
extern bool usb_connected;
extern bool bTrace;
extern bool bExtTrace;
#define LOGF(...) do { if (tud_mounted()) cdc0_printf(__VA_ARGS__); } while (0)
#define TRC_LOGF(...) do { if (bTrace) LOGF(__VA_ARGS__); } while (0)
#define DBG_LOGF(...) do { if (bExtTrace) LOGF(__VA_ARGS__); } while (0)

// ── CDC1 — data port ──────────────────────────────────────────────────────────

static inline void cdc1_write(const char* buf, size_t len) {
    if (!tud_cdc_n_connected(1)) return;
    size_t written = 0;
    while (written < len) {
        uint32_t n = tud_cdc_n_write(1, buf + written, len - written);
        written += n;
        if (n == 0) tud_task();
    }
    tud_cdc_n_write_flush(1);
}

static inline void cdc1_write_str(const char* s) {
    cdc1_write(s, strlen(s));
}

// Read with timeout — blocks until data arrives or timeout_ms elapses.
// Returns number of bytes read (0 on timeout).
static inline size_t cdc1_read_timeout(char* buf, size_t max_len,
                                        uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!tud_cdc_n_available(1)) {
        if (time_reached(deadline)) return 0;
        tud_task();
        sleep_ms(1);
    }
    return tud_cdc_n_read(1, buf, max_len);
}

// Non-blocking read — returns 0 immediately if nothing available.
static inline size_t cdc1_read(char* buf, size_t max_len) {
    if (!tud_cdc_n_available(1)) return 0;
    return tud_cdc_n_read(1, buf, max_len);
}