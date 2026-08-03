#ifndef GRD_REMOTE_ACCESS_H
#define GRD_REMOTE_ACCESS_H

#include "grd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRD_REMOTE_USERNAME_MAX 64U

typedef enum grd_remote_access_kind {
    GRD_REMOTE_ACCESS_TERMINAL = 0,
    GRD_REMOTE_ACCESS_SFTP = 1
} grd_remote_access_kind;

/* These helpers deliberately accept only values that are safe both as
 * OpenSSH argv entries and in the small macOS .command launcher. Discovery
 * currently supplies numeric IPv4 addresses, while host names and IPv6 are
 * accepted for future direct-connect support. */
bool grd_remote_access_username_valid(const char *username);
bool grd_remote_access_address_valid(const char *address);
grd_status grd_remote_access_target(
    const char *username,
    const char *address,
    char *output,
    size_t capacity,
    grd_error *error
);

/* Client-side OpenSSH integration. Authentication and host-key prompts are
 * handled by the system ssh/sftp process in a native terminal. GRD never
 * receives or stores the Unix account password. */
bool grd_remote_access_client_available(grd_remote_access_kind kind);
bool grd_remote_access_default_username(char *output, size_t capacity);
grd_status grd_remote_access_launch(
    grd_remote_access_kind kind,
    const char *address,
    uint16_t port,
    const char *username,
    grd_error *error
);

/* Host-side readiness probe used before advertising the SSH/SFTP discovery
 * capabilities. It verifies an SSH banner on loopback; it does not enable,
 * reconfigure or otherwise mutate the operating-system SSH service. */
grd_status grd_remote_access_probe_local_ssh(
    uint16_t port,
    uint32_t timeout_millis,
    grd_error *error
);

#ifdef __cplusplus
}
#endif

#endif
