#ifndef __HIPI_VERSION_H__
#define __HIPI_VERSION_H__

// HIPI's version, from git (cmake/hipi_version.cmake writes it at every
// build): "1.2.0" on a release tag, "1.2.0-beta.5" five commits after it.
namespace hipi {
extern const char* const kVersion;        // short: splash, boot screen, About
extern const char* const kVersionFull;    // + commit and build date: the USB log
}

#endif//__HIPI_VERSION_H__
