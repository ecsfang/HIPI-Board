HIPI SD card
============

Copy everything in this folder (or in HIPI-SD-card.zip) to the root of a
micro-SD card, keeping the folders, and put the card in HIPI.

  resources/   Pictures for the Tape view (7" panel). Leave them as they are.
  lif/         Cassettes: one .dat file (LIF image) per cassette.
               HDRIVCHUU260708.DAT is an example with HP-41 programs.
               Add your own .dat files here.
  README.txt   This file. HIPI doesn't need it.

HIPI creates the rest by itself:

  CONFIG.TXT   Settings, written when HIPI starts the first time.
  screenshots/ Screen dumps, when you save the first one.
  logs/        Analyzer logs, when you start the first one.

The card should be FAT32 (exFAT works too). Later you can reach the card
from a PC without taking it out: More -> System -> Connect to PC.

More: documents/USER_MANUAL.md in the HIPI project.
