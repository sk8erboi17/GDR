#ifndef GRD_LOG_H
#define GRD_LOG_H

#include "grd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum grd_log_level {
    GRD_LOG_DEBUG,
    GRD_LOG_INFO,
    GRD_LOG_WARNING,
    GRD_LOG_ERROR
} grd_log_level;

typedef void (*grd_log_sink)(grd_log_level level, const char *message, void *userdata);

void grd_log_set_sink(grd_log_sink sink, void *userdata);
void grd_log_write(grd_log_level level, const char *format, ...);

#define GRD_DEBUG(...) grd_log_write(GRD_LOG_DEBUG, __VA_ARGS__)
#define GRD_INFO(...) grd_log_write(GRD_LOG_INFO, __VA_ARGS__)
#define GRD_WARN(...) grd_log_write(GRD_LOG_WARNING, __VA_ARGS__)
#define GRD_LOG_ERRORF(...) grd_log_write(GRD_LOG_ERROR, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
