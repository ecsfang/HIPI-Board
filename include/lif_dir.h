// lif_dir.h -- a LIF volume's directory: finding a file, and adding one
// (as lifutils' lifput does). Blocks are read and written through
// functions, so it works on a file on the SD card (FatFs), on a drive's own
// open file, and in the PC tests alike.
//
// A LIF volume (see lif_create.h):
//   block 0              header: 0x8000, label, directory start/length,
//                        tracks / surfaces / blocks per track (the medium)
//   the directory        32-byte entries, 8 a block:
//                          0-9   name, padded with spaces
//                          10-11 type (0 = deleted, 0xFFFF = end of directory)
//                          12-15 start block   16-19 length in blocks
//                          20-25 date (BCD)    26-27 0x8001 (last volume)
//                          28-31 depends on the type
//   the files            each in its own run of blocks
// A new file goes into the first deleted entry (else the end mark's place,
// with the end mark one further on), its data into the first gap between
// files that is big enough -- exactly what lifput does.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hipi {

constexpr std::uint32_t kLifEntrySize = 32;
constexpr std::size_t kLifNameLen = 10;
constexpr std::uint32_t kLifCassetteBlocks = 512;   // an HP82161A cassette: 2 tracks x 256 blocks

using LifReadBlock = std::function<bool(std::uint32_t block, std::uint8_t* out)>;    // 256 bytes
using LifWriteBlock = std::function<bool(std::uint32_t block, const std::uint8_t* in)>;

struct LifDirEntry {
    int index = 0;                       // its place in the directory
    std::string name;                    // without the padding
    std::uint16_t type = 0;
    std::uint32_t start = 0, blocks = 0;
    std::uint8_t entry[32] = {};         // the entry as it is
};

struct LifDir {
    std::uint32_t dirStart = 0, dirBlocks = 0;
    std::uint32_t mediumBlocks = 0;      // the whole volume
    std::string label;
    std::vector<LifDirEntry> files;      // in directory order, not the deleted ones
    int firstFree = -1;                  // first deleted entry or the end mark (-1: full)
    bool freeIsEnd = false;              // firstFree is the end mark's place
    int capacity() const { return static_cast<int>(dirBlocks * 8); }
    // A file of that name (an exact match of the padded names), nullptr: none
    const LifDirEntry* find(const std::string& name) const;
    // The first gap of `blocks` blocks between the files (ignoring the file
    // at `ignoreIndex`, which is going to be replaced); false: none
    bool findSpace(std::uint32_t blocks, int ignoreIndex, std::uint32_t& start) const;
    // Free blocks in all, the file at ignoreIndex counted as free
    std::uint32_t freeBlocks(int ignoreIndex = -1) const;
};

// A file's length in bytes, from its directory entry -- as lifutils works
// it out (file_length()): an HP-41 program its byte count + 1 (the
// checksum), HP-41 register files 8 per register + 1, SDATA 8 per
// register, HP-71 files their nibbles / 2, the rest whole blocks. Never more
// than its blocks. What `lifget -r` writes.
std::uint32_t lifFileLength(const std::uint8_t entry[kLifEntrySize]);

// Reads the header and the directory. fileBlocks: the image file's size in
// blocks (the medium's size, if the header doesn't give a sane one)
bool lifReadDir(const LifReadBlock& read, std::uint32_t fileBlocks, LifDir& dir, std::string& why);

// A name as LIF keeps it: 10 characters, padded with spaces
std::string lifPaddedName(const std::string& name);
// A file name for a cassette: 1 to 10 printable characters, no spaces (the
// HP-41 takes names lifutils' check_filename doesn't, like 41CV-AL -- they
// must come back as they were; names made here are plainer still:
// lifNameFromPcName())
bool lifFileNameOk(const std::string& name);

// Adds a file: `entry` its 32-byte directory entry (name, type, length in
// blocks, the type's bytes; the start block is filled in here), `data`
// gives its blocks 0 .. n-1. replace: a file of the same name is deleted
// first (its entry and blocks reused if they fit); without it, a file of
// that name is an error. Nothing is written if there's no room.
bool lifAddFile(const LifReadBlock& read, const LifWriteBlock& write, std::uint32_t fileBlocks,
                const std::uint8_t entry[kLifEntrySize], const LifReadBlock& data, bool replace,
                std::string& why);

}  // namespace hipi
