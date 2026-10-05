// analyzer.h -- HP-IL bus analyzer. Passively watches every frame that
// passes through HIPI (what arrives on IN, and what HIPI's own devices
// send on towards OUT) and turns it into a readable account of what
// happened on the loop: who talked to whom, what was sent (summarised),
// status and ID answers, missing answers, errors and slow responses.
//
// It is not an HP-IL device: it never gets an address and never changes a
// frame. hipi_loop() only hands it each frame with a timestamp
// (analyzer_capture(), a few microseconds); the analysis and drawing run
// from the main loop (analyzer_poll()). Capture runs all the time, so the
// view can be opened after something went wrong.
//
// Shown in its own view (Display -> Analyzer); the menu button opens its
// own menu there (UiDialog: AnalyzerMenu).
#pragma once
#include "display_config.h"
#include "hpil.h"
#include <cstdint>
#include <string>

// Set to 0 to build without the analyzer's hooks in the HP-IL loop and
// the main loop (the view then just stays empty) -- e.g. to rule it out
// when hunting a problem. Can also be set from the build.
#ifndef HIPI_ANALYZER
#define HIPI_ANALYZER 1
#endif

namespace hipi {

enum class AnalyzerMode { Overview, Detailed };

void analyzer_init(DisplayDriver* display);

// From hipi_loop(): the frame as it arrived, and as it leaves HIPI
// (IL_NO_FRAME if a device absorbed it)
void analyzer_capture(IL_CMD_t in, IL_CMD_t out);

// Main loop: analyses newly captured frames, redraws the view if shown
void analyzer_poll();

// The view (see plotterview_showAnalyzer())
void analyzer_redrawAll();
void analyzer_scroll(int lines);          // > 0 = back in time
void analyzer_scrollToLive();
int  analyzer_pageRows();
void analyzer_clear();                    // screen + captured history

// Settings (Analyzer menu)
void analyzer_setMode(AnalyzerMode m);
AnalyzerMode analyzer_mode();
void analyzer_setRelativeTime(bool relative);   // else time since start-up
bool analyzer_relativeTime();
void analyzer_setShowIdle(bool show);           // IDY/ISR polling frames
bool analyzer_showIdle();
void analyzer_setPaused(bool paused);           // freezes the screen, capture goes on
bool analyzer_paused();

// The rain screen saver: the mnemonic of the next frame seen on the loop
// since the last call (idle polling skipped) -- false if there's none
bool analyzer_nextFrameText(char* buf, std::size_t size);

// What the loop's controller is called in the analysis: "CTRL" (unknown --
// usually the HP-41), or "PC" while PILBox is in CON mode
void analyzer_setController(const char* name);

// Frames captured since `cursor` (start with 0), one per call, idle
// polling skipped: as it arrived (in), as it left (out, IL_NO_FRAME if
// absorbed) and when (µs since start-up). False when there's no more --
// for other passive users of the capture (the loop map).
bool analyzer_nextFrame(std::uint32_t& cursor, IL_CMD_t& in, IL_CMD_t& out, std::uint64_t& tUs);

// Logging to logs/analyzer_<n>.txt, in the same form as the screen.
// message: the file name, or why it couldn't start.
bool analyzer_startLog(std::string& message);
void analyzer_stopLog();
bool analyzer_logging();
// Saves what's in the analyzer now (the lines shown/scrollable) to a new
// logs/analyzer_<n>.txt -- only while not logging.
bool analyzer_saveLog(std::string& message);

}  // namespace hipi
