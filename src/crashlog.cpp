// crashlog.cpp -- see crashlog.h
#define MODULE "CRASH"
#include "crashlog.h"
#include "usb_serial.h"
#include "hardware/exception.h"
#include "hardware/structs/scb.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/platform.h"
#include "pico/time.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <cxxabi.h>
#include <malloc.h>

extern "C" {
extern std::uint32_t __StackTop;          // the linker's: core 0's stack starts here
extern std::uint32_t __HeapLimit;         // ... and the heap ends here
// The FatFs library's own crash record (lib/no-OS-FatFS..., crash.h)
#include "crash.h"
}

namespace hipi {

namespace {

constexpr std::uint32_t kMagic = 0x48495047;      // "HIPG"
constexpr std::uint32_t kPattern = 0xC5ACCE55;

enum Kind : std::uint32_t { Fault = 1, Panic = 2, Terminate = 3 };

// Kept over a reset (not cleared at start-up)
struct CrashInfo {
    std::uint32_t magic, kind;
    std::uint32_t pc, lr, sp, psr;
    std::uint32_t cfsr, hfsr;
    std::uint32_t stackUsed;
    std::uint32_t heapFree, heapUsed;       // (panic / terminate only)
    std::uint32_t trace[8];                 // code addresses found on the stack, newest first
    char text[100];                         // panic message, exception type ...
};
CrashInfo __uninitialized_ram(crashInfo_);

std::uint32_t* stackFloor() { return &__HeapLimit; }
std::uint32_t* stackTop() { return &__StackTop; }

std::uint32_t stackDepth() {
    const std::uint32_t* p = stackFloor();
    while (p < stackTop() && *p == kPattern) ++p;
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(stackTop()) -
                                      reinterpret_cast<std::uintptr_t>(p));
}

std::uint32_t reg(std::uintptr_t a) { return *reinterpret_cast<volatile std::uint32_t*>(a); }

// A poor man's backtrace: words on the stack that look like return
// addresses into the program (in flash, Thumb bit set). Some are stale,
// but the callers are among them, newest first.
void collectTrace(const std::uint32_t* sp) {
    int n = 0;
    for (const std::uint32_t* p = sp; p < stackTop() && n < 8; ++p) {
        const std::uint32_t w = *p;
        if (w >= 0x10000000u && w < 0x10400000u && (w & 1u)) crashInfo_.trace[n++] = w;
    }
    for (; n < 8; ++n) crashInfo_.trace[n] = 0;
}

[[noreturn]] void restart() {
    watchdog_reboot(0, 0, 10);
    for (;;) tight_loop_contents();
}

// For a panic or terminate: where we are, the heap, then a restart
[[noreturn]] void recordAndRestart(Kind kind) {
    std::uintptr_t sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    crashInfo_.magic = kMagic;
    crashInfo_.kind = kind;
    crashInfo_.pc = crashInfo_.lr = crashInfo_.psr = crashInfo_.cfsr = crashInfo_.hfsr = 0;
    crashInfo_.sp = static_cast<std::uint32_t>(sp);
    crashInfo_.stackUsed = stackDepth();
    const struct mallinfo mi = mallinfo();
    crashInfo_.heapFree = static_cast<std::uint32_t>(mi.fordblks);
    crashInfo_.heapUsed = static_cast<std::uint32_t>(mi.uordblks);
    collectTrace(reinterpret_cast<const std::uint32_t*>(sp));
    restart();
}

[[noreturn]] void onTerminate() {
    std::snprintf(crashInfo_.text, sizeof crashInfo_.text, "C++ exception not caught");
    if (std::exception_ptr e = std::current_exception()) {
        const std::type_info* t = abi::__cxa_current_exception_type();
        const char* what = "";
        try { std::rethrow_exception(e); }
        catch (const std::exception& x) { what = x.what(); }
        catch (...) {}
        std::snprintf(crashInfo_.text, sizeof crashInfo_.text, "C++ exception not caught: %s (%s)",
                      t != nullptr ? t->name() : "?", what);
    }
    recordAndRestart(Terminate);
}

std::uint32_t loggedDepth_ = 0;
absolute_time_t nextCheck_ = nil_time;

}  // namespace

// The stacked frame (r0 r1 r2 r3 r12 lr pc xpsr): saved, then a restart
extern "C" void hipiFaultC(std::uint32_t* frame) {
    crashInfo_.magic = kMagic;
    crashInfo_.kind = Fault;
    crashInfo_.lr = frame[5];
    crashInfo_.pc = frame[6];
    crashInfo_.psr = frame[7];
    crashInfo_.sp = reinterpret_cast<std::uintptr_t>(frame);
    crashInfo_.cfsr = reg(0xE000ED28);
    crashInfo_.hfsr = reg(0xE000ED2C);
    crashInfo_.stackUsed = stackDepth();
    crashInfo_.heapFree = crashInfo_.heapUsed = 0;
    crashInfo_.text[0] = 0;
    collectTrace(frame + 8);
    restart();
}

// Which stack the fault was on (bit 2 of the EXC_RETURN in lr), then the C part
extern "C" __attribute__((naked)) void hipiFault() {
    __asm volatile(
        "tst lr, #4      \n"
        "ite eq          \n"
        "mrseq r0, msp   \n"
        "mrsne r0, psp   \n"
        "b hipiFaultC    \n");
}

// The SDK's panic() (hard_assert, out of memory in the SDK ...) comes here
// (PICO_PANIC_FUNCTION, CMakeLists.txt): its message kept, then a restart
extern "C" [[noreturn]] void hipi_panic(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(crashInfo_.text, sizeof crashInfo_.text, fmt != nullptr ? fmt : "", ap);
    va_end(ap);
    recordAndRestart(Panic);
}

void crashlog_init() {
    // The pattern below the stack as it is now (a margin for this call)
    std::uintptr_t sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    std::uint32_t* end = reinterpret_cast<std::uint32_t*>(sp - 256);
    for (std::uint32_t* p = stackFloor(); p < end; ++p) *p = kPattern;
    // Straight into the (RAM) vector table: the FatFs library
    // (lib/no-OS-FatFS..., crash.c) already puts a HardFault_Handler there,
    // so exception_set_exclusive_handler() would panic -- before anything
    // shows, HIPI wouldn't start. Theirs ends in a breakpoint, which with
    // no debugger attached locks the chip up instead of restarting it.
    reinterpret_cast<exception_handler_t*>(scb_hw->vtor)[HARDFAULT_EXCEPTION] = hipiFault;
    __dmb();
    // An exception nobody catches (std::bad_alloc when the heap is out ...)
    std::set_terminate(onTerminate);
    // The FatFs library's record of its own asserts: read (and cleared) now
    crash_handler_init();
}

void crashlog_report() {
    if (const crash_info_t* ci = crash_handler_get_info()) {
        if (ci->magic == crash_magic_assert)
            LOGF("\r\n * The SD card library stopped HIPI last time: assert %s:%d %s (%s)",
                 ci->assert.file, ci->assert.line, ci->assert.func, ci->assert.pred);
    }
    if (crashInfo_.magic != kMagic) return;
    crashInfo_.magic = 0;
    crashInfo_.text[sizeof crashInfo_.text - 1] = 0;
    const char* what = crashInfo_.kind == Panic ? "panic" : crashInfo_.kind == Terminate ? "C++ terminate" : "HardFault";
    LOGF("\r\n * HIPI CRASHED last time (%s) -- it restarted by itself:", what);
    if (crashInfo_.text[0]) LOGF("\r\n     \"%s\"", crashInfo_.text);
    if (crashInfo_.kind == Fault) {
        LOGF("\r\n     PC 0x%08lX  LR 0x%08lX  SP 0x%08lX  xPSR 0x%08lX  CFSR 0x%08lX  HFSR 0x%08lX",
             static_cast<unsigned long>(crashInfo_.pc), static_cast<unsigned long>(crashInfo_.lr),
             static_cast<unsigned long>(crashInfo_.sp), static_cast<unsigned long>(crashInfo_.psr),
             static_cast<unsigned long>(crashInfo_.cfsr), static_cast<unsigned long>(crashInfo_.hfsr));
        if (crashInfo_.hfsr & 0x80000000u)
            LOGF("\r\n     (HFSR DEBUGEVT: a breakpoint instruction -- a panic, assert or abort somewhere)");
    } else {
        LOGF("\r\n     SP 0x%08lX  heap: %lu bytes in use, %lu free (in pieces)",
             static_cast<unsigned long>(crashInfo_.sp), static_cast<unsigned long>(crashInfo_.heapUsed),
             static_cast<unsigned long>(crashInfo_.heapFree));
    }
    LOGF("\r\n     stack used %lu bytes; on the stack:", static_cast<unsigned long>(crashInfo_.stackUsed));
    for (std::uint32_t a : crashInfo_.trace)
        if (a != 0) LOGF(" 0x%08lX", static_cast<unsigned long>(a & ~1u));
    LOGF("\r\n     Where: arm-none-eabi-addr2line -f -C -e build/hipi_7_pico.elf <the addresses above>");
}

std::uint32_t crashlog_stackUsed() { return stackDepth(); }

void crashlog_poll() {
    if (!time_reached(nextCheck_)) return;
    nextCheck_ = make_timeout_time_ms(2000);
    const std::uint32_t d = stackDepth();
    if (d > 2048 && d >= loggedDepth_ + 256) {
        loggedDepth_ = d;
        LOGF("\r\n * Stack: %lu bytes deep (2048 reserved, %lu before the heap)",
             static_cast<unsigned long>(d),
             static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(stackTop()) -
                                        reinterpret_cast<std::uintptr_t>(stackFloor())));
    }
}

}  // namespace hipi
