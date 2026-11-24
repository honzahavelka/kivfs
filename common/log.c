#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#else
#  define _POSIX_C_SOURCE 200809L     /* localtime_r, clock_gettime */
#endif

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

void kivfs_log(const char *tag, const char *fmt, ...)
{
    char line[512];
    struct timespec ts;
    struct tm tm;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03ld %-7s ",
                     tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L, tag);

    va_list ap;
    va_start(ap, fmt);
    if (n >= 0 && (size_t)n < sizeof line)
        vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);

    fputs(line, stderr);
    fputc('\n', stderr);
}
