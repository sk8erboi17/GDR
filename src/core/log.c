#include "grd/common.h"
#include "grd/log.h"

#include <sodium.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static grd_log_sink g_sink;
static void *g_sink_userdata;

uint64_t grd_now_micros(void)
{
#if defined(_WIN32)
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    if (!QueryPerformanceCounter(&counter) ||
        !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        return 0U;
    }
    const uint64_t ticks = (uint64_t)counter.QuadPart;
    const uint64_t rate = (uint64_t)frequency.QuadPart;
    return (ticks / rate) * 1000000ULL +
           (ticks % rate) * 1000000ULL / rate;
#else
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0U;
    }
    return (uint64_t)value.tv_sec * 1000000ULL +
           (uint64_t)value.tv_nsec / 1000ULL;
#endif
}

void grd_secure_zero(void *data, size_t length)
{
    if (data != NULL && length != 0U) {
        sodium_memzero(data, length);
    }
}

void grd_log_set_sink(grd_log_sink sink, void *userdata)
{
    g_sink = sink;
    g_sink_userdata = userdata;
}

void grd_log_write(grd_log_level level, const char *format, ...)
{
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    static const char *labels[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    const size_t index = (size_t)level < 4U ? (size_t)level : 3U;
    if (g_sink != NULL) {
        g_sink(level, message, g_sink_userdata);
        return;
    }

    fprintf(stderr, "[GRD %s] %s\n", labels[index], message);
}
