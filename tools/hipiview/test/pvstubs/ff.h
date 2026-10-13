// Host stand-in for FatFs (printer_view_test): plain files, relative to
// the current directory
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
typedef int FRESULT;
typedef unsigned int UINT;
typedef std::uint32_t FSIZE_t;
typedef std::uint32_t DWORD;
#define FR_OK 0
#define FR_NO_FILE 4
#define FR_EXIST 8
#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_CREATE_NEW 0x04
#define FA_CREATE_ALWAYS 0x08
struct FIL { std::FILE* f = nullptr; };
inline FRESULT f_open(FIL* fp, const char* path, int mode) {
    if (mode & FA_WRITE) {
        if ((mode & FA_CREATE_NEW) && std::filesystem::exists(path)) return FR_EXIST;
        fp->f = std::fopen(path, "wb");
    } else {
        fp->f = std::fopen(path, "rb");
    }
    return fp->f ? FR_OK : FR_NO_FILE;
}
inline FRESULT f_write(FIL* fp, const void* buf, UINT n, UINT* bw) {
    *bw = static_cast<UINT>(std::fwrite(buf, 1, n, fp->f));
    return FR_OK;
}
inline FRESULT f_unlink(const char* path) { return std::remove(path) == 0 ? FR_OK : FR_NO_FILE; }
inline FRESULT f_mkdir(const char* path) {
    std::error_code ec;
    return std::filesystem::create_directory(path, ec) ? FR_OK : FR_EXIST;
}
// Free space: what the test says (stubFreeBytes())
struct FATFS { DWORD csize = 1; };
inline std::uint64_t& stubFreeBytes() { static std::uint64_t b = 8ull << 30; return b; }
inline FRESULT f_getfree(const char*, DWORD* clusters, FATFS** fs) {
    static FATFS f;
    *fs = &f;
    *clusters = static_cast<DWORD>(std::min<std::uint64_t>(stubFreeBytes() / 512, 0xFFFFFFFFu));
    return FR_OK;
}
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
inline FRESULT f_stat(const char* path, FILINFO* fi) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return FR_NO_FILE;
    std::snprintf(fi->fname, sizeof(fi->fname), "%s", std::filesystem::path(path).filename().string().c_str());
    fi->fattrib = std::filesystem::is_directory(path, ec) ? 0x10 : 0;
    return FR_OK;
}
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
