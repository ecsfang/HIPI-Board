// lif_types.h -- the known LIF file types: the type code in a directory
// entry, its name (as lifutils shows it), what it is, which machine it
// belongs to and what sort of file it is. Used by the Media map
// (mediamap.cpp) to colour and name files.
//
// The list is lifutils' (Appendix A, "Known file types"), plus the HP-41's
// ASCII (E0D5) and buffer (E0B0) files. (S) = secure, (P) = private.
#pragma once
#include <cstdint>

namespace hipi {

// Which machine a file type belongs to
enum class LifMachine : std::uint8_t { Hp41, Hp71, Hp75, Any, Unknown };

// What sort of file it is
enum class LifSort : std::uint8_t { Program, Data, Text, State, RomLex, Other };

struct LifType {
    std::uint16_t code;
    const char* name;            // "BAS71 (S)"
    const char* what;            // "HP-71 BASIC file"
    LifMachine machine;
    LifSort sort;
    bool secure;                 // (S): can't be changed
    bool priv;                   // (P): can't be listed
};

inline constexpr LifType kLifTypes[] = {
    { 0x0001, "TEXT",       "text file (any machine)",  LifMachine::Any,  LifSort::Text,    false, false },
    { 0x00FF, "D-LEX",      "HP-71 LEX file, disabled", LifMachine::Hp71, LifSort::RomLex,  false, false },
    { 0xE020, "WAXM41",     "HP-41 write all + X-Mem",  LifMachine::Hp41, LifSort::State,   false, false },
    { 0xE030, "XM41",       "HP-41 X-Memory file",      LifMachine::Hp41, LifSort::State,   false, false },
    { 0xE040, "ALL41",      "HP-41 write all",          LifMachine::Hp41, LifSort::State,   false, false },
    { 0xE050, "KEY41",      "HP-41 key assignments",    LifMachine::Hp41, LifSort::State,   false, false },
    { 0xE052, "TXT75",      "HP-75 text file",          LifMachine::Hp75, LifSort::Text,    false, false },
    { 0xE053, "APP75",      "HP-75 appointments",       LifMachine::Hp75, LifSort::Data,    false, false },
    { 0xE058, "DAT75",      "HP-75 data file",          LifMachine::Hp75, LifSort::Data,    false, false },
    { 0xE060, "STA41",      "HP-41 status",             LifMachine::Hp41, LifSort::State,   false, false },
    { 0xE070, "X-M41",      "HP-41 MLDL-OS ROM file",   LifMachine::Hp41, LifSort::RomLex,  false, false },
    { 0xE080, "PGM41",      "HP-41 program",            LifMachine::Hp41, LifSort::Program, false, false },
    { 0xE088, "BAS75",      "HP-75 BASIC program",      LifMachine::Hp75, LifSort::Program, false, false },
    { 0xE089, "LEX75",      "HP-75 LEX file",           LifMachine::Hp75, LifSort::RomLex,  false, false },
    { 0xE08A, "WKS75",      "HP-75 VisiCalc sheet",     LifMachine::Hp75, LifSort::Data,    false, false },
    { 0xE08B, "ROM75",      "HP-75 ROM file",           LifMachine::Hp75, LifSort::RomLex,  false, false },
    { 0xE0B0, "BUF41",      "HP-41 buffer",             LifMachine::Hp41, LifSort::State,   false, false },
    { 0xE0D0, "SDATA",      "HP-41 data file",          LifMachine::Hp41, LifSort::Data,    false, false },
    { 0xE0D1, "TEXT (S)",   "text file",                LifMachine::Hp71, LifSort::Text,    true,  false },
    { 0xE0D5, "ASCII",      "HP-41 ASCII file",         LifMachine::Hp41, LifSort::Text,    false, false },
    { 0xE0F0, "DAT71",      "HP-71 data file",          LifMachine::Hp71, LifSort::Data,    false, false },
    { 0xE0F1, "DAT71 (S)",  "HP-71 data file",          LifMachine::Hp71, LifSort::Data,    true,  false },
    { 0xE204, "BIN71",      "HP-71 BIN program",        LifMachine::Hp71, LifSort::Program, false, false },
    { 0xE205, "BIN71 (S)",  "HP-71 BIN program",        LifMachine::Hp71, LifSort::Program, true,  false },
    { 0xE206, "BIN71 (P)",  "HP-71 BIN program",        LifMachine::Hp71, LifSort::Program, false, true  },
    { 0xE207, "BIN71 (SP)", "HP-71 BIN program",        LifMachine::Hp71, LifSort::Program, true,  true  },
    { 0xE208, "LEX71",      "HP-71 LEX file",           LifMachine::Hp71, LifSort::RomLex,  false, false },
    { 0xE209, "LEX71 (S)",  "HP-71 LEX file",           LifMachine::Hp71, LifSort::RomLex,  true,  false },
    { 0xE20A, "LEX71 (P)",  "HP-71 LEX file",           LifMachine::Hp71, LifSort::RomLex,  false, true  },
    { 0xE20B, "LEX71 (SP)", "HP-71 LEX file",           LifMachine::Hp71, LifSort::RomLex,  true,  true  },
    { 0xE20C, "KEY71",      "HP-71 key assignments",    LifMachine::Hp71, LifSort::State,   false, false },
    { 0xE20D, "KEY71 (S)",  "HP-71 key assignments",    LifMachine::Hp71, LifSort::State,   true,  false },
    { 0xE214, "BAS71",      "HP-71 BASIC program",      LifMachine::Hp71, LifSort::Program, false, false },
    { 0xE215, "BAS71 (S)",  "HP-71 BASIC program",      LifMachine::Hp71, LifSort::Program, true,  false },
    { 0xE216, "BAS71 (P)",  "HP-71 BASIC program",      LifMachine::Hp71, LifSort::Program, false, true  },
    { 0xE217, "BAS71 (SP)", "HP-71 BASIC program",      LifMachine::Hp71, LifSort::Program, true,  true  },
    { 0xE218, "FTH71",      "HP-71 FORTH RAM file",     LifMachine::Hp71, LifSort::Program, false, false },
    { 0xE219, "FTH71 (S)",  "HP-71 FORTH RAM file",     LifMachine::Hp71, LifSort::Program, true,  false },
    { 0xE21A, "FTH71 (P)",  "HP-71 FORTH RAM file",     LifMachine::Hp71, LifSort::Program, false, true  },
    { 0xE21B, "FTH71 (SP)", "HP-71 FORTH RAM file",     LifMachine::Hp71, LifSort::Program, true,  true  },
    { 0xE21C, "ROM71",      "HP-71 ROM file",           LifMachine::Hp71, LifSort::RomLex,  false, false },
    { 0xE222, "GRA71",      "HP-71 graphics file",      LifMachine::Hp71, LifSort::Other,   false, false },
    { 0xE224, "ADR71",      "HP-71 address file",       LifMachine::Hp71, LifSort::Other,   false, false },
    { 0xE22E, "SYM71",      "HP-71 symbol file",        LifMachine::Hp71, LifSort::Other,   false, false },
};

// The type with this code, nullptr if it isn't known
inline const LifType* lifType(std::uint16_t code) {
    for (const LifType& t : kLifTypes)
        if (t.code == code) return &t;
    return nullptr;
}

}  // namespace hipi
