#ifndef GRD_REMOTE_ACCESS_INTERNAL_H
#define GRD_REMOTE_ACCESS_INTERNAL_H

#include "grd/remote_access.h"

bool grd_platform_remote_access_client_available(grd_remote_access_kind kind);
bool grd_platform_remote_access_default_username(
    char *output,
    size_t capacity
);
grd_status grd_platform_remote_access_launch(
    grd_remote_access_kind kind,
    const char *address,
    uint16_t port,
    const char *username,
    grd_error *error
);
grd_status grd_platform_remote_access_probe_local_ssh(
    uint16_t port,
    uint32_t timeout_millis,
    grd_error *error
);

#endif
