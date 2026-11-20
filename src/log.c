#include "log.h"

#include <stdarg.h>
#include <stdio.h>

static int g_debug;

void rtsps_set_debug(int on)
{
    g_debug = on ? 1 : 0;
}

void rtsps_log(const char *fmt, ...)
{
    va_list ap;

    if (!g_debug || !fmt)
        return;
    va_start(ap, fmt);
    fputs("rtsps: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
