#pragma once
#include <cstdint>
typedef int FRESULT;
#define FR_OK 0
struct FILINFO { std::uint32_t fsize; std::uint16_t fdate, ftime; };
inline FRESULT f_stat(const char*, FILINFO*) { return 1; }
