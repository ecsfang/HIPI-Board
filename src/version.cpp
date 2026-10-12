// version.cpp -- see version.h. The only file that includes the generated
// header, so a new version recompiles just this one.
#include "version.h"
#include "hipi_version.h"     // generated: build/hipi_generated/

namespace hipi {
const char* const kVersion = HIPI_VERSION_TEXT;
const char* const kVersionFull = HIPI_VERSION_FULL;
}
