#include <stdio.h>
#include <string.h>
#include "hpil.h"

typedef struct {
    IL_CMD_t   	opc;    // opcode
    IL_CMD_t   	mask;    // opcode mask
    const char 	*mne;  // mnemonic
} IL_Codes_t;

const static IL_Codes_t ilCodes[] = {
    0x000, 0x700, "DAB",
    0x100, 0x700, "DSR",
    0x200, 0x700, "END",
    0x300, 0x700, "ESR",
    0x400, 0x7FF, "NUL",
    0x401, 0x7FF, "GTL",
    0x404, 0x7FF, "SDC",
    0x405, 0x7FF, "PPD",
    0x408, 0x7FF, "GET",
    0x40F, 0x7FF, "ELN",
    0x410, 0x7FF, "NOP",
    0x411, 0x7FF, "LLO",
    0x414, 0x7FF, "DCL",
    0x415, 0x7FF, "PPU",
    0x418, 0x7FF, "EAR",
    0x43F, 0x7FF, "UNL",
    0x420, 0x7E0, "LAD",
    0x45F, 0x7FF, "UNT",
    0x440, 0x7E0, "TAD",
    0x460, 0x7E0, "SAD",
    0x480, 0x7F0, "PPE",
    0x490, 0x7FF, "IFC",
    0x492, 0x7FF, "REN",
    0x493, 0x7FF, "NRE",
    0x49A, 0x7FF, "AAU",
    0x49B, 0x7FF, "LPD",
    0x4A0, 0x7E0, "DDL",
    0x4C0, 0x7E0, "DDT",
    0x400, 0x700, "CMD",
    0x500, 0x7FF, "RFC",
    0x540, 0x7FF, "ETO",
    0x541, 0x7FF, "ETE",
    0x542, 0x7FF, "NRD",
    0x560, 0x7FF, "SDA",
    0x561, 0x7FF, "SST",
    0x562, 0x7FF, "SDI",
    0x563, 0x7FF, "SAI",
    0x564, 0x7FF, "TCT",
    0x580, 0x7E0, "AAD",
    0x5A0, 0x7E0, "AEP",
    0x5C0, 0x7E0, "AES",
    0x5E0, 0x7E0, "AMP",
    0x500, 0x700, "RDY",
    0x600, 0x700, "IDY",
    0x700, 0x700, "ISR"
};

char *ilMnemonic(IL_CMD_t frame, char *buf)
{
	// go through HP-IL opcode table
	for (int i = 0; i < sizeof(ilCodes) / sizeof(ilCodes[0]); ++i)
	{
		// found opcode in table
		if ((frame & ilCodes[i].mask) == ilCodes[i].opc)
		{
			// get argument from mask
			IL_CMD_t arg = (~ilCodes[i].mask) & 0xFF;

			strcpy(buf,ilCodes[i].mne);		// copy name
			if (arg != 0) {					// opcode has an argument
				sprintf(&buf[3], " %02X", frame & arg);
			}
			break;
		}
	}
    return buf;
}
