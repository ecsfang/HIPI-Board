HIPI SD card
============

Copy everything in this folder (or in HIPI-SD-card.zip) to the root of a
micro-SD card, keeping the folders, and put the card in HIPI.

  resources/   Pictures for the Tape and Printer views (7" panel). Leave
               them as they are.
  lif/         Cassettes: one .dat or .lif file (LIF image) per cassette.
               HDRIVCHUU260708.DAT is an example with HP-41 programs.
               Add your own .dat or .lif files here.
  i2c/         Your own I2C chips, in .txt files (e.g. my.txt). HIPI's own
               list is built in; chips.txt.example is a copy of it, with
               the format explained at the top.
  README.txt   This file. HIPI doesn't need it.

HIPI creates the rest by itself:

  CONFIG.TXT   Settings, written when HIPI starts the first time.
  screenshots/ Screen dumps, when you save the first one.
  text/        The display's text, when you save it the first time.
  logs/        Analyzer logs, when you start the first one.

The card should be FAT32 (exFAT works too). Later you can reach the card
from a PC without taking it out: More -> System -> Connect to PC.

More: documents/USER_MANUAL.md in the HIPI project.
