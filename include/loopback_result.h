#pragma once
#include <cstdint>
// loopback_result.h -- just the LoopbackResult struct, split out on its
// own so both hipi.h (which declares hipi_loopbackTest(), needing
// HpIlLoop from hpil_pio.hpp -- a real Pico SDK PIO dependency) and
// uidialog.hpp (which only needs the struct itself, to store a
// std::function<LoopbackResult()> callback -- see its own
// setLoopbackTestCallback()) can each include exactly what they need,
// without uidialog.hpp having to pull in the PIO/hardware headers it
// otherwise has no use for.

// Result of hipi_loopbackTest() -- tested is how many values were
// actually sent before the test finished (either the full range, if
// nothing failed, or however many it got through before stopping at the
// first failure -- see hipi_loopbackTest()'s own comment on why it
// aborts immediately rather than continuing through the whole range).
// errors is 0 (full range passed) or 1 (stopped at a failure); when 1,
// failedCmd holds the value that failed (either timed out or came back
// wrong), otherwise 0.
struct LoopbackResult {
    int tested;
    int errors;
    std::uint32_t failedCmd;
};
