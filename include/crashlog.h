// crashlog.h -- what HIPI knows after a crash.
//
// A HardFault (a bad pointer, a stack running into something, ...) is
// caught: where it happened (PC, LR, the fault registers) and how deep the
// stack had gone are kept in RAM that survives a reset, and HIPI restarts
// itself. At the next start-up the USB log says what happened, and how to
// look the address up:
//
//   arm-none-eabi-addr2line -f -C -e build/hipi_7_pico.elf 0x1000abcd
//
// (with the .elf of the very same build). A hang is not caught: the screen
// then stays as it was, and HIPI doesn't restart.
//
// The stack: core 0's stack is 2 kB (PICO_STACK_SIZE), with core 1's unused
// stack and scratch memory below it -- 8 kB in all before it would run into
// the heap. crashlog_init() fills what's below with a pattern, so how deep
// the stack has gone can be seen (crashlog_stackUsed()); crashlog_poll()
// logs it when it reaches a new depth past the 2 kB.
#pragma once
#include <cstdint>

namespace hipi {

// Early in main(): the fault handler, the stack pattern
void crashlog_init();
// Once USB logging works (the boot log keeps it anyway): the last crash, if any
void crashlog_report();
// The deepest the stack has been, in bytes
std::uint32_t crashlog_stackUsed();
// Main loop: logs a new stack depth (now and then)
void crashlog_poll();

}  // namespace hipi
