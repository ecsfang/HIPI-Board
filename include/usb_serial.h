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

static inline void cdc0_write(const char* buf, size_t len) {
    if (!tud_cdc_n_connected(0)) return;
    size_t written = 0;
    while (written < len) {
        uint32_t n = tud_cdc_n_write(0, buf + written, len - written);
        written += n;
        if (n == 0) tud_task();   // TX buffer full — yield and retry
    }
    tud_cdc_n_write_flush(0);
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