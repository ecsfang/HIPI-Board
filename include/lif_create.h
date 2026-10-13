// lif_create.h -- creates an empty LIF image (a .dat "cassette" for the
// drives) on the SD card: File picker -> Create media...
//
// Laid out like lifinit from J. Siebold's lifutils (lifinit -z):
//   block 0              volume header: LIF id 0x8000, label, directory
//                        at block 2, its length, version 1, the medium's
//                        tracks/heads/sectors, the creation date (BCD)
//   block 1              zeros
//   blocks 2 .. 2+d-1    the directory, all 0xFF (= empty: an entry of
//                        type 0xFFFF ends the directory)
//   block 2+d            0xFF
//   the rest             zeros -- the file always gets the medium's full
//                        size, so the drive can read every block of it
// Differences from lifinit: the whole 6-byte date is written (lifinit
// copies only 4 of its bytes), and the label is given (lifinit leaves it
// blank).
//
// The medium types are the ones lifinit knows, in the same order as the
// drive's own table (drive.cpp: mediaInfo[]).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ff.h"

namespace hipi {

struct LifMediaType {
    const char* name;                   // as shown: "HP82161A cassette"
    const char* prefix;                 // suggested file names: CASS01, DISK01 ...
    std::uint16_t tracks, heads, sectors;
    std::uint16_t defaultDirEntries;    // the directory size offered first
    std::uint32_t blocks() const { return static_cast<std::uint32_t>(tracks) * heads * sectors; }
    std::uint32_t bytes() const { return blocks() * 256u; }
};
constexpr int kLifMediaTypeCount = 7;
extern const LifMediaType kLifMediaTypes[kLifMediaTypeCount];

// Directory: 32-byte entries, 8 per 256-byte block
int lifDirBlocks(int entries);
// The largest directory lifinit allows: a third of the medium
int lifMaxDirEntries(const LifMediaType& t);
// The directory sizes offered: 8, 16, 32 ... up to lifMaxDirEntries()
std::vector<int> lifDirChoices(const LifMediaType& t);
// Blocks left for files
int lifDataBlocks(const LifMediaType& t, int entries);

// The creation date (from the real-time clock); none: the date stays empty
struct LifDate { int year, month, day, hour, minute, second; };

// Block 0. label: up to 6 characters (padded with spaces); date may be null.
void lifVolumeHeader(std::uint8_t block[256], const LifMediaType& t, int dirEntries,
                     const std::string& label, const LifDate* date);

// ── File names ──────────────────────────────────────────────────────────
// A name is typed without ".DAT": upper-case letters, digits, '-' and '_',
// at most kLifNameMax characters
constexpr int kLifNameMax = 24;
bool lifNameChar(char c);
bool lifNameValid(const std::string& name);
// The volume label: the first 6 characters of the name
std::string lifLabelFromName(const std::string& name);
// lif/<name>.DAT
std::string lifFilePath(const std::string& name);
// Is there a lif/<name>.DAT already (in any case)?
bool lifFileExists(const std::string& name);
// The first free <prefix>01 .. <prefix>99
std::string lifSuggestName(const char* prefix);
// Free space on the SD card, in bytes (0 if it can't be read)
std::uint64_t sdFreeBytes();

// ── Writing the file, a piece at a time (the main loop keeps running) ──
class LifCreator {
public:
    enum class State { Idle, Running, Done, Failed };
    ~LifCreator() { cancel(); }

    // Checks the space, creates lif/<name>.DAT (never overwrites a file)
    // and writes the header. False (error() says why) if it can't start.
    bool begin(const std::string& name, const LifMediaType& t, int dirEntries, const LifDate* date);
    // Writes up to maxBytes more; Done when the file is complete. A
    // failed or cancelled file is deleted again.
    State step(std::uint32_t maxBytes = 32768);
    void cancel();

    State state() const { return state_; }
    int percent() const;
    const std::string& error() const { return error_; }
    const std::string& fileName() const { return fileName_; }    // "NAME.DAT"

private:
    void fail(const char* why);
    State state_ = State::Idle;
    FIL file_{};
    bool open_ = false;
    std::string path_, fileName_, error_;
    std::uint32_t total_ = 0, next_ = 0;    // blocks: all, the next to write
    int dirBlocks_ = 0;
    std::vector<std::uint8_t> buf_;
};

}  // namespace hipi
