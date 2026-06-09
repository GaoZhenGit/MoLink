#ifndef WIN_UTILS_H
#define WIN_UTILS_H

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdio>
#include <cstring>

// ---- Wide argv (bypasses main() argv encoding loss) ----

struct WideArgv {
    int argc = 0;
    wchar_t** argv = nullptr;
    ~WideArgv() { if (argv) LocalFree(argv); }
};

inline WideArgv getWideArgv() {
    WideArgv result;
    result.argv = CommandLineToArgvW(GetCommandLineW(), &result.argc);
    if (!result.argv) result.argc = 0;
    return result;
}

// ---- Encoding conversions (UTF-16 ↔ UTF-8) ----

inline std::string wideToUtf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], len, nullptr, nullptr);
    return result;
}

inline std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring result(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], len);
    return result;
}

// ---- Console output (UTF-8 → console code page) ----

inline bool g_consoleUtf8 = false;

inline void initConsole() {
    g_consoleUtf8 = (SetConsoleOutputCP(CP_UTF8) != 0);
}

inline std::string utf8ForConsole(const std::string& utf8) {
    if (g_consoleUtf8) return utf8;
    std::wstring wide = utf8ToWide(utf8);
    UINT cp = GetConsoleOutputCP();
    int len = WideCharToMultiByte(cp, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return utf8;
    std::string result(len - 1, '\0');
    WideCharToMultiByte(cp, 0, wide.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}

// ---- File I/O with UTF-8 paths ----

inline FILE* fopenUtf8(const char* utf8Path, const char* mode) {
    std::wstring wpath = utf8ToWide(utf8Path);
    std::wstring wmode(mode, mode + strlen(mode));
    return _wfopen(wpath.c_str(), wmode.c_str());
}

inline std::string pathToUtf8(const std::filesystem::path& p) {
    return p.u8string();
}

// Build argc/argv-compatible vectors from WideArgv
struct Utf8Args {
    std::vector<std::string> storage;
    std::vector<char*> ptrs;

    Utf8Args(int argc, wchar_t** wargv) {
        storage.reserve(argc);
        ptrs.reserve(argc + 1);
        for (int i = 0; i < argc; i++) {
            storage.push_back(wideToUtf8(wargv[i]));
            ptrs.push_back(&storage.back()[0]);
        }
        ptrs.push_back(nullptr);
    }

    int argc() const { return (int)ptrs.size() - 1; }
    char** argv() { return ptrs.data(); }
};

#endif
