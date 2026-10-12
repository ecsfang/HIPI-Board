// Host stand-in for FatFs (printer_view_test): plain files, relative to
// the current directory
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
typedef int FRESULT;
typedef unsigned int UINT;
typedef std::uint32_t FSIZE_t;
#define FR_OK 0
#define FA_READ 1
struct FIL { std::FILE* f = nullptr; };
inline FRESULT f_open(FIL* fp, const char* path, int) { fp->f = std::fopen(path, "rb"); return fp->f ? FR_OK : 4; }
inline FRESULT f_close(FIL* fp) { if (fp->f) std::fclose(fp->f); fp->f = nullptr; return FR_OK; }
inline FRESULT f_read(FIL* fp, void* buf, UINT n, UINT* got) { *got = static_cast<UINT>(std::fread(buf, 1, n, fp->f)); return FR_OK; }
inline FRESULT f_lseek(FIL* fp, FSIZE_t ofs) { return std::fseek(fp->f, static_cast<long>(ofs), SEEK_SET) == 0 ? FR_OK : 1; }
inline FSIZE_t f_tell(FIL* fp) { return static_cast<FSIZE_t>(std::ftell(fp->f)); }
inline FSIZE_t f_size(FIL* fp) {
    const long at = std::ftell(fp->f);
    std::fseek(fp->f, 0, SEEK_END);
    const long n = std::ftell(fp->f);
    std::fseek(fp->f, at, SEEK_SET);
    return static_cast<FSIZE_t>(n);
}
// Directories (std::filesystem: no clash with POSIX's DIR)
#define AM_DIR 0x10
struct FILINFO { char fname[256]; unsigned char fattrib; };
struct DIR { std::vector<std::string> names; std::vector<bool> dirs; std::size_t at = 0; };
inline FRESULT f_opendir(DIR* dp, const char* path) {
    std::error_code ec;
    std::filesystem::directory_iterator it(path, ec);
    if (ec) return 5;
    dp->names.clear(); dp->dirs.clear(); dp->at = 0;
    for (const auto& e : it) { dp->names.push_back(e.path().filename().string()); dp->dirs.push_back(e.is_directory()); }
    return FR_OK;
}
inline FRESULT f_readdir(DIR* dp, FILINFO* fi) {
    if (dp->at >= dp->names.size()) { fi->fname[0] = 0; return FR_OK; }
    std::snprintf(fi->fname, sizeof(fi->fname), "%s", dp->names[dp->at].c_str());
    fi->fattrib = dp->dirs[dp->at] ? AM_DIR : 0;
    ++dp->at;
    return FR_OK;
}
inline FRESULT f_closedir(DIR*) { return FR_OK; }
