#ifndef MOLINK_LOG_H
#define MOLINK_LOG_H

#include <cstdio>
#include <cstdarg>
#include <windows.h>

static inline void logWrite(const char* level, const char* tag, const char* fmt, ...) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(stdout, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%-5s] [%s] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            level, tag);
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fprintf(stdout, "\n");
    fflush(stdout);
}

#ifdef MOLINK_DEBUG_LOG
#define LOG_DEBUG(tag, ...) logWrite("DEBUG", tag, ##__VA_ARGS__)
#else
#define LOG_DEBUG(tag, ...) ((void)0)
#endif

#define LOG_INFO(tag, ...)  logWrite("INFO",  tag, ##__VA_ARGS__)
#define LOG_WARN(tag, ...)  logWrite("WARN",  tag, ##__VA_ARGS__)
#define LOG_ERROR(tag, ...) logWrite("ERROR", tag, ##__VA_ARGS__)

#endif
