#include "grd/remote_access.h"
#include "platform/remote_access_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void set_error(grd_error *error, grd_status code, const char *message)
{
    if (error == NULL) {
        return;
    }
    error->code = code;
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

bool grd_remote_access_username_valid(const char *username)
{
    if (username == NULL || username[0] == '\0') {
        return false;
    }
    const size_t length = strlen(username);
    if (length >= GRD_REMOTE_USERNAME_MAX ||
        !(isalnum((unsigned char)username[0]) || username[0] == '_')) {
        return false;
    }
    for (size_t index = 1U; index < length; ++index) {
        const unsigned char value = (unsigned char)username[index];
        if (!(isalnum(value) || value == '_' || value == '-' || value == '.')) {
            return false;
        }
    }
    return true;
}

bool grd_remote_access_address_valid(const char *address)
{
    if (address == NULL || address[0] == '\0' || address[0] == '-') {
        return false;
    }
    const size_t length = strlen(address);
    if (length >= GRD_MAX_ADDRESS) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char value = (unsigned char)address[index];
        if (!(isalnum(value) || value == '.' || value == ':' || value == '-' ||
              value == '_' || value == '%')) {
            return false;
        }
    }
    return true;
}

grd_status grd_remote_access_target(
    const char *username,
    const char *address,
    char *output,
    size_t capacity,
    grd_error *error
)
{
    if (output == NULL || capacity == 0U ||
        !grd_remote_access_username_valid(username) ||
        !grd_remote_access_address_valid(address)) {
        set_error(
            error,
            GRD_INVALID_ARGUMENT,
            "Invalid SSH username or address"
        );
        return GRD_INVALID_ARGUMENT;
    }
    const int written = snprintf(output, capacity, "%s@%s", username, address);
    if (written < 0 || (size_t)written >= capacity) {
        output[0] = '\0';
        set_error(error, GRD_INVALID_ARGUMENT, "SSH destination is too long");
        return GRD_INVALID_ARGUMENT;
    }
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    return GRD_OK;
}

bool grd_remote_access_client_available(grd_remote_access_kind kind)
{
    return grd_platform_remote_access_client_available(kind);
}

bool grd_remote_access_default_username(char *output, size_t capacity)
{
    return grd_platform_remote_access_default_username(output, capacity);
}

grd_status grd_remote_access_launch(
    grd_remote_access_kind kind,
    const char *address,
    uint16_t port,
    const char *username,
    grd_error *error
)
{
    char target[GRD_REMOTE_USERNAME_MAX + GRD_MAX_ADDRESS + 2U];
    const grd_status target_status = grd_remote_access_target(
        username, address, target, sizeof(target), error
    );
    if (target_status != GRD_OK || port == 0U ||
        (kind != GRD_REMOTE_ACCESS_TERMINAL &&
         kind != GRD_REMOTE_ACCESS_SFTP)) {
        if (target_status == GRD_OK) {
            set_error(error, GRD_INVALID_ARGUMENT, "Invalid SSH port or action");
        }
        return GRD_INVALID_ARGUMENT;
    }
    if (!grd_platform_remote_access_client_available(kind)) {
        set_error(
            error,
            GRD_NOT_SUPPORTED,
            kind == GRD_REMOTE_ACCESS_SFTP
                ? "OpenSSH SFTP client or terminal is unavailable"
                : "OpenSSH client or terminal is unavailable"
        );
        return GRD_NOT_SUPPORTED;
    }
    return grd_platform_remote_access_launch(
        kind, address, port, username, error
    );
}

grd_status grd_remote_access_probe_local_ssh(
    uint16_t port,
    uint32_t timeout_millis,
    grd_error *error
)
{
    if (port == 0U || timeout_millis == 0U) {
        set_error(error, GRD_INVALID_ARGUMENT, "Invalid SSH port");
        return GRD_INVALID_ARGUMENT;
    }
    return grd_platform_remote_access_probe_local_ssh(
        port, timeout_millis, error
    );
}
