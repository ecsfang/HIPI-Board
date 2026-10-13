// printout.h -- the printer's paper saved when it's torn off (Printer
// view: drag the paper up to the tear-off edge and then sideways, or the
// menu's "Tear off paper", then Save):
//
//   printouts/printout_<n>.bmp   the paper as printed: every dot 2 x 2
//                                pixels, black on white (1 bit per pixel),
//                                graphics, HP-41 characters and all
//   printouts/printout_<n>.txt   the text of each line, for copying a
//                                listing: characters outside ASCII become
//                                '.' (as in the trace log), a line of
//                                graphics only "[graphics]"
//
// <n>: the first number with neither file there.
#pragma once
#include "printer.h"
#include <string>
#include <vector>

namespace hipi {

// The kept lines (CPrinter::lines()). True and `message` the file name
// (without the folder and extension), or false and why not.
bool printout_save(const CPrinter& printer, std::string& message);

// The printouts saved, newest (highest number) first
struct PrintoutInfo {
    unsigned number;             // printout_<number>
    int lines;                   // from the picture's height (-1: no picture)
};
std::vector<PrintoutInfo> printout_list();
std::string printout_name(unsigned number);           // "printout_7"

// Back into the printer: the lines read from the picture (the dots) and
// the text file (each line's text, if it's there). Replaces the paper.
bool printout_load(unsigned number, CPrinter& printer, std::string& message);
// Both files gone
bool printout_delete(unsigned number);

}  // namespace hipi
