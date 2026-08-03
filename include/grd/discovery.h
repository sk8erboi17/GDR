#ifndef GRD_DISCOVERY_H
#define GRD_DISCOVERY_H

#include "grd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grd_discovered_peer {
    char device_id[37];
    char name[GRD_MAX_DEVICE_NAME];
    char address[GRD_MAX_ADDRESS];
    uint16_t port;
    grd_os operating_system;
    uint32_t capabilities;
    uint16_t ssh_port;
    uint64_t last_seen_micros;
} grd_discovered_peer;

enum {
    GRD_DISCOVERY_CAP_SSH_TERMINAL = 1U << 0U,
    GRD_DISCOVERY_CAP_SFTP = 1U << 1U,
    GRD_DISCOVERY_CAP_POWERSHELL = 1U << 2U
};

typedef struct grd_discovery grd_discovery;

grd_discovery *grd_discovery_start(
    const char *device_id,
    const char *name,
    uint16_t port,
    grd_error *error
);
size_t grd_discovery_peers(
    grd_discovery *discovery,
    grd_discovered_peer *peers,
    size_t capacity
);
/* Discovery always listens for LAN peers, but advertises this device only
 * while its host is actually accepting connections. */
void grd_discovery_set_available(
    grd_discovery *discovery,
    bool available,
    const char *name,
    uint16_t port
);
/* Advertises optional services in a separate companion datagram, preserving
 * the original discovery packet layout for compatibility with mainline GRD.
 * Capabilities are informational: OpenSSH performs its own authentication
 * and host-key verification, independent of the GRD password. */
void grd_discovery_set_remote_access(
    grd_discovery *discovery,
    grd_os operating_system,
    uint32_t capabilities,
    uint16_t ssh_port
);
/* Actively asks LAN hosts to announce themselves. The normal periodic
 * announcements remain enabled; this path makes startup and manual refresh
 * immediate even on networks that delay or suppress multicast traffic. */
void grd_discovery_refresh(grd_discovery *discovery);
/* Age in ms since the discovery thread last ran (thread map). */
uint64_t grd_discovery_thread_age_ms(const grd_discovery *discovery);
void grd_discovery_stop(grd_discovery *discovery);

#ifdef __cplusplus
}
#endif

#endif
