#include "platform/remote_access_internal.h"

#include <stdio.h>
#include <string.h>

static grd_status unsupported(grd_error *error)
{
    if (error != NULL) {
        error->code = GRD_NOT_SUPPORTED;
        (void)snprintf(
            error->message,
            sizeof(error->message),
            "%s",
            "Integrated SSH/SFTP access is available only on macOS and Linux"
        );
    }
    return GRD_NOT_SUPPORTED;
}

bool grd_platform_remote_access_client_available(grd_remote_access_kind kind)
{
    (void)kind;
    return false;
}

bool grd_platform_remote_access_default_username(
    char *output,
    size_t capacity
)
{
    if (output != NULL && capacity != 0U) {
        output[0] = '\0';
    }
    return false;
}

grd_status grd_platform_remote_access_launch(
    grd_remote_access_kind kind,
    const char *address,
    uint16_t port,
    const char *username,
    grd_error *error
)
{
    (void)kind;
    (void)address;
    (void)port;
    (void)username;
    return unsupported(error);
}

grd_status grd_platform_remote_access_probe_local_ssh(
    uint16_t port,
    uint32_t timeout_millis,
    grd_error *error
)
{
    (void)port;
    (void)timeout_millis;
    return unsupported(error);
}
