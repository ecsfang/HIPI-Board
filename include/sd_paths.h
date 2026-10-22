// sd_paths.h -- where things live on the SD card. Only CONFIG.TXT stays
// in the root; everything else has its own folder.
#pragma once

// Bitmaps HIPI reads: the Tape view's hp82161a.bmp, tape-in.bmp, open.bmp,
// leds.bmp, reels.bmp (the logo and the buttons are built in)
#define HIPI_DIR_RESOURCES   "resources"
// Screen dumps (screendump_<n>.bmp) -- created when the first is saved
#define HIPI_DIR_SCREENSHOTS "screenshots"
// Cassette images (LIF .dat files) for the drives
#define HIPI_DIR_LIF         "lif"
// Torn-off printer paper (printout_<n>.bmp + .txt) -- created when the first is saved
#define HIPI_DIR_PRINTOUTS   "printouts"
// The display's text, saved (display_<n>.txt) -- created when the first is saved
#define HIPI_DIR_TEXT        "text"
// Analyzer logs (analyzer_<n>.txt) -- created when the first is started
#define HIPI_DIR_LOGS        "logs"
// I2C chip descriptions (*.txt) for System -> Hardware test -- optional
#define HIPI_DIR_I2C         "i2c"

// Path of a file in one of these folders, e.g. HIPI_PATH(HIPI_DIR_RESOURCES, "logo.bmp")
#define HIPI_PATH(dir, file) dir "/" file
