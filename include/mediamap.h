// mediamap.h -- the Media map view: what's on the drive's cassette or
// disk, drawn as rings, coloured by what is stored where -- programs,
// data, text, status/keys, ROM/LEX, other, deleted (space only PACK gives
// back), free, the directory and the volume label (BY TYPE), or by the
// machine a file is for: HP-41, HP-71, HP-75, any (BY MACHINE). The file
// types are include/lif_types.h's. A tap on a row of the card highlights
// that group in the rings (the rest dimmed); a tap on a tab switches.
//
//   cassette   two rings: its two tracks, outer = track 0, one step per
//              record (exact)
//   disk       eight rings, outside in like the cylinders of a disk; a
//              step is one block or more (a 16 MB disk: 20 blocks) and
//              shows the most important thing in it, so even a one-block
//              file is never lost in the picture
//
// Built from the LIF directory alone (a file's first block, length and
// type -- LIF files are contiguous), so it's quick even for a 16 MB disk.
// The head is followed live (a marker on the rim and across its ring, the
// operation in the middle); after the drive has written something the
// directory is read again. A tap on a ring shows that file.
//
// Shown as the DisplayOutput::MediaMap view (views_showMediaMap()).
// The drive is reached through two functions (mediamap_setSource()), so
// the view can be tried on the PC (tools/hipiview/test/mediamap_test.cpp).
#pragma once
#include "display_config.h"
#include <cstdint>
#include <functional>
#include <string>

namespace hipi {

// What the drive is doing
enum class MediaOp : std::uint8_t { None, Seek, Read, Write };
struct MediaMapLive {
    std::string file;            // the medium's file name ("" = none)
    std::uint32_t head = 0;      // block the last operation was at
    MediaOp op = MediaOp::None;  // ... and what it was
    std::uint32_t writes = 0;    // blocks written so far (any change: read the directory again)
    bool busy = false;           // the drive is busy (its BUSY light)
};
// live(info): false if there's no drive. read(block, buf): one 256-byte block.
void mediamap_setSource(std::function<bool(MediaMapLive&)> live,
                        std::function<bool(std::uint32_t, std::uint8_t*)> read);

// The demo (demo.h) shows a cassette that isn't really in the drive: its
// own source meanwhile, then the drive's again
void mediamap_setDemoSource(std::function<bool(MediaMapLive&)> live,
                            std::function<bool(std::uint32_t, std::uint8_t*)> read);
void mediamap_clearDemoSource();

void mediamap_init(DisplayDriver* display);
// The view opens: reads the medium
void mediamap_enter();
void mediamap_drawAll();
// Main loop: the head, a changed directory, the tapped file's box
void mediamap_poll(std::uint32_t nowMs);
// A tap while the view is shown: Close (the title bar's X), Handled, or None
enum class MediaMapTap { None, Handled, Close };
MediaMapTap mediamap_tap(int x, int y);

// The tapped file's box has a "Save on PC" button while available() says
// so (hipiview connected); a tap on it: save(lifName, why), the file to
// hipiview (display_mirror.h, displayMirror_sendFile()) -- its answer is
// shown instead of the button. Where the button is (tests, the demo):
// mediamap_savePoint(), false if it isn't shown.
void mediamap_setSaveOnPc(std::function<bool()> available, std::function<bool(const std::string&, std::string&)> save);
bool mediamap_savePoint(int& x, int& y);

// Where to touch (the demo, demo.h): a tab of the card, a row of it, the
// ring step holding a block -- screen pixels; false: not there
void mediamap_tabPoint(bool machine, int& x, int& y);
bool mediamap_rowPoint(int row, int& x, int& y);
bool mediamap_blockPoint(std::uint32_t block, int& x, int& y);

// For the tests: blocks of a group (see mediamap.cpp's Group: 0..9 the
// kinds, 10..14 the machines), the card shown, the highlighted group
std::uint32_t mediamap_blocksOf(int group);
int mediamap_rings();
bool mediamap_byMachine();
int mediamap_highlight();

}  // namespace hipi
