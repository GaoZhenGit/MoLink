#ifndef TIME_UTILS_H
#define TIME_UTILS_H

// 时间戳工具：
//   - DOS date/time ↔ time_t（zip 头部字段）
//   - FILETIME ↔ time_t
//   - 把 time_t 设为文件的 last write / last access time
//
// 依赖 win_utils.h（提供 utf8ToWide），仅 MinGW/Windows 可用。

#include "win_utils.h"
#include <cstdint>
#include <ctime>
#include <string>

namespace timeu {

// time_t（Unix 秒）→ FILETIME（自 1601-01-01 UTC 起的 100ns）
inline FILETIME toFileTime(time_t t) {
    constexpr uint64_t kEpochDiff100ns = 11644473600ULL * 10000000ULL;
    uint64_t v = (uint64_t)(int64_t)t * 10000000ULL + kEpochDiff100ns;
    FILETIME ft;
    ft.dwLowDateTime  = (DWORD)(v & 0xFFFFFFFF);
    ft.dwHighDateTime = (DWORD)(v >> 32);
    return ft;
}

// 把 time_t 设为文件的 last write + last access time（不动 creation time）。
// mtime == 0 时直接返回（无法表达 1970 之前的可移植时间戳，留作 fallback）。
inline bool setMtime(const std::string& utf8Path, time_t mtime) {
    if (mtime == 0) return false;
    std::wstring wpath = utf8ToWide(utf8Path);
    HANDLE h = CreateFileW(wpath.c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME ft = toFileTime(mtime);
    BOOL ok = SetFileTime(h, nullptr, &ft, &ft);  // null creation, set access+write
    CloseHandle(h);
    return ok != FALSE;
}

// ---- DOS 日期/时间格式 ----
// Time:  0-4 秒/2 | 5-10 分 | 11-15 时
// Date:  0-4 日   | 5-8 月   | 9-15 (年-1980)
// 全程按 UTC 编解码（与 UT 扩展口径一致，绕过 TZ 漂移）。
inline void toDos(time_t t, uint16_t& dosTime, uint16_t& dosDate) {
    if (t == 0) { dosTime = 0; dosDate = 0; return; }
    struct tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    // DOS 年字段 7 bits（0-127），对应 1980-2107
    int y = tm.tm_year - 80;
    if (y < 0)   y = 0;
    if (y > 127) y = 127;
    dosTime = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    dosDate = (uint16_t)((y << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

// DOS date/time → time_t（按 local time 解释；UT 扩展会覆盖之）
inline time_t fromDos(uint16_t dosTime, uint16_t dosDate) {
    if (dosTime == 0 && dosDate == 0) return 0;
    struct tm tm{};
    tm.tm_sec  = (dosTime & 0x1F) * 2;
    tm.tm_min  = (dosTime >> 5) & 0x3F;
    tm.tm_hour = (dosTime >> 11) & 0x1F;
    tm.tm_mday = dosDate & 0x1F;
    tm.tm_mon  = ((dosDate >> 5) & 0x0F) - 1;
    tm.tm_year = ((dosDate >> 9) & 0x7F) + 80;  // DOS 年自 1980 起，tm_year 自 1900 起
#ifdef _WIN32
    return _mkgmtime(&tm);                    // 当作 UTC 还原
#else
    return timegm(&tm);
#endif
}

} // namespace timeu

#endif