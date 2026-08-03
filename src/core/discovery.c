#include "grd/discovery.h"
#include "grd/log.h"
#include "core/net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#define DISCOVERY_PORT 47989U
#define DISCOVERY_GROUP "239.255.71.68"
#define DISCOVERY_MAGIC 0x47524444U
#define DISCOVERY_GOODBYE_MAGIC 0x47524458U
#define DISCOVERY_QUERY_MAGIC 0x47524451U
#define DISCOVERY_CAPABILITIES_MAGIC 0x47524443U

typedef struct discovery_packet {
    uint32_t magic;
    uint16_t version;
    uint16_t port;
    char device_id[37];
    char name[GRD_MAX_DEVICE_NAME];
} discovery_packet;

typedef struct discovery_capabilities_packet {
    uint32_t magic;
    uint32_t capabilities;
    uint16_t version;
    uint16_t grd_port;
    uint16_t ssh_port;
    uint8_t operating_system;
    uint8_t reserved;
    char device_id[37];
    char name[GRD_MAX_DEVICE_NAME];
} discovery_capabilities_packet;

struct grd_discovery {
    grd_socket socket_value;
    grd_thread thread;
    bool thread_started;
    grd_mutex mutex;
    _Atomic bool running;
    _Atomic bool available;
    _Atomic uint64_t thread_last_active;
    discovery_packet local;
    discovery_capabilities_packet local_capabilities;
    grd_discovered_peer peers[32];
    size_t peer_count;
};

static bool join_discovery_group(grd_socket socket_value)
{
    struct ip_mreq membership;
    memset(&membership, 0, sizeof(membership));
    (void)inet_pton(AF_INET, DISCOVERY_GROUP, &membership.imr_multiaddr);
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    return setsockopt(
               socket_value, IPPROTO_IP, IP_ADD_MEMBERSHIP,
               (const char *)&membership, sizeof(membership)
           ) == 0;
}

static void renew_discovery_group(grd_socket socket_value)
{
    struct ip_mreq membership;
    memset(&membership, 0, sizeof(membership));
    (void)inet_pton(AF_INET, DISCOVERY_GROUP, &membership.imr_multiaddr);
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    (void)setsockopt(
        socket_value, IPPROTO_IP, IP_DROP_MEMBERSHIP,
        (const char *)&membership, sizeof(membership)
    );
    if (!join_discovery_group(socket_value)) {
        GRD_WARN(
            "discovery: failed to renew multicast group %s (%s)",
            DISCOVERY_GROUP,
            strerror(errno)
        );
    }
}

static void update_peer(
    grd_discovery *discovery,
    const discovery_packet *packet,
    const struct sockaddr_in *source
)
{
    const bool goodbye = packet->magic == DISCOVERY_GOODBYE_MAGIC;
    if ((packet->magic != DISCOVERY_MAGIC && !goodbye) ||
        memchr(packet->device_id, '\0', sizeof(packet->device_id)) == NULL ||
        memchr(packet->name, '\0', sizeof(packet->name)) == NULL ||
        strcmp(packet->device_id, discovery->local.device_id) == 0) {
        return;
    }
    if (goodbye) {
        /* A separate magic keeps the goodbye backward-compatible: older
         * clients ignore it instead of listing an unavailable host at port
         * zero, while current clients can remove the peer immediately. */
        grd_mutex_lock(&discovery->mutex);
        for (size_t index = 0U; index < discovery->peer_count; ++index) {
            if (strcmp(
                    discovery->peers[index].device_id,
                    packet->device_id
                ) == 0) {
                discovery->peers[index] =
                    discovery->peers[discovery->peer_count - 1U];
                --discovery->peer_count;
                break;
            }
        }
        grd_mutex_unlock(&discovery->mutex);
        return;
    }
    if (packet->port == 0U) {
        return;
    }
    if (packet->version != GRD_PROTOCOL_VERSION) {
        /* The connect handshake is the authoritative compatibility check:
         * a mixed-version LAN must still show the device (with a clear log)
         * instead of silently hiding it, otherwise the user sees "no
         * devices" while the other side runs an older build. */
        GRD_WARN(
            "discovery: '%s' (%s) uses protocol version %u, expected %u: "
            "device is shown but cannot be connected",
            packet->name,
            packet->device_id,
            (unsigned)packet->version,
            (unsigned)GRD_PROTOCOL_VERSION
        );
    }
    char address[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &source->sin_addr, address, sizeof(address)) == NULL) {
        return;
    }
    grd_mutex_lock(&discovery->mutex);
    size_t index;
    for (index = 0U; index < discovery->peer_count; ++index) {
        if (strcmp(discovery->peers[index].device_id, packet->device_id) == 0) {
            break;
        }
    }
    const bool is_new = index == discovery->peer_count;
    if (is_new) {
        if (discovery->peer_count >=
            sizeof(discovery->peers) / sizeof(discovery->peers[0])) {
            grd_mutex_unlock(&discovery->mutex);
            return;
        }
        ++discovery->peer_count;
    }
    grd_discovered_peer *peer = &discovery->peers[index];
    const grd_os previous_operating_system = peer->operating_system;
    const uint32_t previous_capabilities = peer->capabilities;
    const uint16_t previous_ssh_port = peer->ssh_port;
    memset(peer, 0, sizeof(*peer));
    (void)snprintf(peer->device_id, sizeof(peer->device_id), "%s", packet->device_id);
    (void)snprintf(peer->name, sizeof(peer->name), "%s", packet->name);
    (void)snprintf(peer->address, sizeof(peer->address), "%s", address);
    peer->port = packet->port;
    peer->operating_system = previous_operating_system;
    peer->capabilities = previous_capabilities;
    peer->ssh_port = previous_ssh_port;
    peer->last_seen_micros = grd_now_micros();
    grd_mutex_unlock(&discovery->mutex);
    if (is_new) {
        GRD_INFO(
            "discovery: found '%s' at %s:%u (protocol %u)",
            packet->name,
            address,
            (unsigned)packet->port,
            (unsigned)packet->version
        );
    }
}

static void update_peer_capabilities(
    grd_discovery *discovery,
    const discovery_capabilities_packet *packet,
    const struct sockaddr_in *source
)
{
    if (packet->magic != DISCOVERY_CAPABILITIES_MAGIC ||
        packet->grd_port == 0U ||
        packet->operating_system > GRD_OS_LINUX_X11 ||
        memchr(packet->device_id, '\0', sizeof(packet->device_id)) == NULL ||
        memchr(packet->name, '\0', sizeof(packet->name)) == NULL ||
        strcmp(packet->device_id, discovery->local.device_id) == 0) {
        return;
    }
    const uint32_t known_capabilities =
        GRD_DISCOVERY_CAP_SSH_TERMINAL |
        GRD_DISCOVERY_CAP_SFTP |
        GRD_DISCOVERY_CAP_POWERSHELL;
    const uint32_t capabilities = packet->capabilities & known_capabilities;
    if (capabilities != 0U && packet->ssh_port == 0U) {
        return;
    }
    char address[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &source->sin_addr, address, sizeof(address)) == NULL) {
        return;
    }
    grd_mutex_lock(&discovery->mutex);
    size_t index;
    for (index = 0U; index < discovery->peer_count; ++index) {
        if (strcmp(
                discovery->peers[index].device_id,
                packet->device_id
            ) == 0) {
            break;
        }
    }
    const bool is_new = index == discovery->peer_count;
    if (is_new) {
        if (discovery->peer_count >=
            sizeof(discovery->peers) / sizeof(discovery->peers[0])) {
            grd_mutex_unlock(&discovery->mutex);
            return;
        }
        ++discovery->peer_count;
    }
    grd_discovered_peer *peer = &discovery->peers[index];
    const uint32_t old_capabilities = is_new ? 0U : peer->capabilities;
    (void)snprintf(
        peer->device_id, sizeof(peer->device_id), "%s", packet->device_id
    );
    (void)snprintf(peer->name, sizeof(peer->name), "%s", packet->name);
    (void)snprintf(peer->address, sizeof(peer->address), "%s", address);
    peer->port = packet->grd_port;
    peer->operating_system = (grd_os)packet->operating_system;
    peer->capabilities = capabilities;
    peer->ssh_port = capabilities != 0U ? packet->ssh_port : 0U;
    peer->last_seen_micros = grd_now_micros();
    const uint16_t advertised_ssh_port = peer->ssh_port;
    grd_mutex_unlock(&discovery->mutex);
    if (is_new) {
        GRD_INFO(
            "discovery: found '%s' at %s:%u through extended advertisement",
            packet->name,
            address,
            (unsigned)packet->grd_port
        );
    }
    if (old_capabilities != capabilities) {
        GRD_INFO(
            "discovery: remote services for '%s': ssh=%d sftp=%d powershell=%d "
            "port=%u",
            packet->name,
            (capabilities & GRD_DISCOVERY_CAP_SSH_TERMINAL) != 0U,
            (capabilities & GRD_DISCOVERY_CAP_SFTP) != 0U,
            (capabilities & GRD_DISCOVERY_CAP_POWERSHELL) != 0U,
            (unsigned)advertised_ssh_port
        );
    }
}

static void send_discovery_packet_to(
    grd_discovery *discovery,
    const discovery_packet *packet,
    const struct sockaddr_in *destination
)
{
    (void)sendto(
        discovery->socket_value,
        (const char *)packet,
        (int)sizeof(*packet),
        0,
        (const struct sockaddr *)destination,
        sizeof(*destination)
    );
}

static void send_discovery_packet(
    grd_discovery *discovery,
    const discovery_packet *packet
)
{
    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(DISCOVERY_PORT);
    (void)inet_pton(AF_INET, DISCOVERY_GROUP, &destination.sin_addr);
    send_discovery_packet_to(discovery, packet, &destination);

    /* Some Wi-Fi access points suppress multicast between stations. A LAN
     * broadcast gives discovery a second route without scanning addresses. */
    destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    send_discovery_packet_to(discovery, packet, &destination);
}

static void send_capabilities_packet_to(
    grd_discovery *discovery,
    const discovery_capabilities_packet *packet,
    const struct sockaddr_in *destination
)
{
    (void)sendto(
        discovery->socket_value,
        (const char *)packet,
        (int)sizeof(*packet),
        0,
        (const struct sockaddr *)destination,
        sizeof(*destination)
    );
}

static void send_capabilities_packet(
    grd_discovery *discovery,
    const discovery_capabilities_packet *packet
)
{
    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(DISCOVERY_PORT);
    (void)inet_pton(AF_INET, DISCOVERY_GROUP, &destination.sin_addr);
    send_capabilities_packet_to(discovery, packet, &destination);
    destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    send_capabilities_packet_to(discovery, packet, &destination);
}

static void send_local_capabilities(grd_discovery *discovery)
{
    discovery_capabilities_packet packet;
    grd_mutex_lock(&discovery->mutex);
    packet = discovery->local_capabilities;
    grd_mutex_unlock(&discovery->mutex);
    send_capabilities_packet(discovery, &packet);
}

static void send_discovery_query(grd_discovery *discovery)
{
    discovery_packet query;
    grd_mutex_lock(&discovery->mutex);
    query = discovery->local;
    grd_mutex_unlock(&discovery->mutex);
    query.magic = DISCOVERY_QUERY_MAGIC;
    query.port = 0U;
    send_discovery_packet(discovery, &query);
}

#if defined(_WIN32)
static DWORD WINAPI discovery_thread(void *argument)
#else
static void *discovery_thread(void *argument)
#endif
{
    grd_discovery *discovery = argument;
    uint64_t last_announce = 0U;
    uint64_t last_query = 0U;
    while (atomic_load_explicit(&discovery->running, memory_order_acquire)) {
        atomic_store_explicit(
            &discovery->thread_last_active,
            grd_now_micros(),
            memory_order_relaxed
        );
        const uint64_t now = grd_now_micros();
        if (now - last_announce >= 1000000ULL) {
            if (atomic_load_explicit(
                    &discovery->available, memory_order_acquire
                )) {
                discovery_packet announcement;
                grd_mutex_lock(&discovery->mutex);
                announcement = discovery->local;
                grd_mutex_unlock(&discovery->mutex);
                send_discovery_packet(discovery, &announcement);
                send_local_capabilities(discovery);
            }
            last_announce = now;
        }
        if (now - last_query >= 3000000ULL) {
            send_discovery_query(discovery);
            last_query = now;
        }
        union {
            discovery_packet discovery;
            discovery_capabilities_packet capabilities;
        } packet;
        struct sockaddr_in source;
#if defined(_WIN32)
        int source_length = sizeof(source);
#else
        socklen_t source_length = sizeof(source);
#endif
        const int received = (int)recvfrom(
            discovery->socket_value,
            (char *)&packet,
            sizeof(packet),
            0,
            (struct sockaddr *)&source,
            &source_length
        );
        if (received == (int)sizeof(packet.discovery)) {
            if (packet.discovery.magic == DISCOVERY_QUERY_MAGIC) {
                if (atomic_load_explicit(
                        &discovery->available, memory_order_acquire
                    )) {
                    discovery_packet announcement;
                    grd_mutex_lock(&discovery->mutex);
                    announcement = discovery->local;
                    grd_mutex_unlock(&discovery->mutex);
                    /* Reply directly: unlike multicast, unicast delivery is
                     * rarely filtered by consumer routers or Wi-Fi APs. */
                    send_discovery_packet_to(
                        discovery, &announcement, &source
                    );
                    discovery_capabilities_packet capabilities;
                    grd_mutex_lock(&discovery->mutex);
                    capabilities = discovery->local_capabilities;
                    grd_mutex_unlock(&discovery->mutex);
                    send_capabilities_packet_to(
                        discovery, &capabilities, &source
                    );
                }
            } else {
                update_peer(discovery, &packet.discovery, &source);
            }
        } else if (received == (int)sizeof(packet.capabilities)) {
            update_peer_capabilities(
                discovery, &packet.capabilities, &source
            );
        }
    }
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

grd_discovery *grd_discovery_start(
    const char *device_id,
    const char *name,
    uint16_t port,
    grd_error *error
)
{
    if (device_id == NULL || name == NULL || !grd_net_initialize()) {
        return NULL;
    }
    grd_discovery *discovery = calloc(1U, sizeof(*discovery));
    if (discovery == NULL) {
        return NULL;
    }
    discovery->socket_value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (discovery->socket_value == GRD_INVALID_SOCKET) {
        free(discovery);
        return NULL;
    }
    int reuse = 1;
    (void)setsockopt(
        discovery->socket_value, SOL_SOCKET, SO_REUSEADDR,
        (const char *)&reuse, sizeof(reuse)
    );
    int broadcast = 1;
    (void)setsockopt(
        discovery->socket_value, SOL_SOCKET, SO_BROADCAST,
        (const char *)&broadcast, sizeof(broadcast)
    );
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(DISCOVERY_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(
            discovery->socket_value,
            (const struct sockaddr *)&address,
            sizeof(address)
        ) != 0) {
        GRD_WARN(
            "discovery: bind on port %u failed (%s)",
            (unsigned)DISCOVERY_PORT,
            strerror(errno)
        );
        if (error != NULL) {
            error->code = GRD_IO_ERROR;
            (void)snprintf(error->message, sizeof(error->message), "Discovery bind failed");
        }
        grd_socket_close(discovery->socket_value);
        free(discovery);
        return NULL;
    }
    if (!join_discovery_group(discovery->socket_value)) {
        GRD_WARN(
            "discovery: joining multicast group %s failed (%s)",
            DISCOVERY_GROUP,
            strerror(errno)
        );
    }
    (void)grd_socket_set_timeout(discovery->socket_value, 200U);
    discovery->local.magic = DISCOVERY_MAGIC;
    discovery->local.version = GRD_PROTOCOL_VERSION;
    discovery->local.port = port;
    (void)snprintf(
        discovery->local.device_id,
        sizeof(discovery->local.device_id),
        "%s",
        device_id
    );
    (void)snprintf(discovery->local.name, sizeof(discovery->local.name), "%s", name);
    memset(&discovery->local_capabilities, 0, sizeof(discovery->local_capabilities));
    discovery->local_capabilities.magic = DISCOVERY_CAPABILITIES_MAGIC;
    discovery->local_capabilities.version = GRD_PROTOCOL_VERSION;
    discovery->local_capabilities.grd_port = port;
    (void)snprintf(
        discovery->local_capabilities.device_id,
        sizeof(discovery->local_capabilities.device_id),
        "%s",
        device_id
    );
    (void)snprintf(
        discovery->local_capabilities.name,
        sizeof(discovery->local_capabilities.name),
        "%s",
        name
    );
    grd_mutex_init(&discovery->mutex);
    discovery->running = true;
    discovery->available = false;
#if defined(_WIN32)
    discovery->thread = CreateThread(
        NULL, 0U, discovery_thread, discovery, 0U, NULL
    );
    if (discovery->thread == NULL) {
#else
    if (pthread_create(&discovery->thread, NULL, discovery_thread, discovery) != 0) {
#endif
        discovery->running = false;
        grd_discovery_stop(discovery);
        return NULL;
    }
    discovery->thread_started = true;
    GRD_INFO(
        "discovery started: multicast %s:%u, app port %u, version %u, "
        "inactive advertisement",
        DISCOVERY_GROUP,
        (unsigned)DISCOVERY_PORT,
        (unsigned)port,
        (unsigned)GRD_PROTOCOL_VERSION
    );
    grd_discovery_refresh(discovery);
    return discovery;
}

void grd_discovery_set_available(
    grd_discovery *discovery,
    bool available,
    const char *name,
    uint16_t port
)
{
    if (discovery == NULL) {
        return;
    }
    grd_mutex_lock(&discovery->mutex);
    if (name != NULL && name[0] != '\0') {
        (void)snprintf(
            discovery->local.name,
            sizeof(discovery->local.name),
            "%s",
            name
        );
        (void)snprintf(
            discovery->local_capabilities.name,
            sizeof(discovery->local_capabilities.name),
            "%s",
            name
        );
    }
    if (port != 0U) {
        discovery->local.port = port;
        discovery->local_capabilities.grd_port = port;
    }
    discovery_packet announcement = discovery->local;
    grd_mutex_unlock(&discovery->mutex);
    const bool was_available = atomic_exchange_explicit(
        &discovery->available, available, memory_order_acq_rel
    );
    if (available) {
        send_discovery_packet(discovery, &announcement);
        send_local_capabilities(discovery);
    } else if (was_available) {
        announcement.magic = DISCOVERY_GOODBYE_MAGIC;
        send_discovery_packet(discovery, &announcement);
    }
    GRD_INFO(
        "discovery: host %s",
        available ? "available on the LAN" : "unavailable"
    );
}

void grd_discovery_set_remote_access(
    grd_discovery *discovery,
    grd_os operating_system,
    uint32_t capabilities,
    uint16_t ssh_port
)
{
    if (discovery == NULL) {
        return;
    }
    const uint32_t known_capabilities =
        GRD_DISCOVERY_CAP_SSH_TERMINAL |
        GRD_DISCOVERY_CAP_SFTP |
        GRD_DISCOVERY_CAP_POWERSHELL;
    grd_mutex_lock(&discovery->mutex);
    discovery->local_capabilities.operating_system = (uint8_t)operating_system;
    discovery->local_capabilities.capabilities =
        capabilities & known_capabilities;
    discovery->local_capabilities.ssh_port =
        discovery->local_capabilities.capabilities != 0U ? ssh_port : 0U;
    grd_mutex_unlock(&discovery->mutex);
    if (atomic_load_explicit(&discovery->available, memory_order_acquire)) {
        send_local_capabilities(discovery);
    }
}

void grd_discovery_refresh(grd_discovery *discovery)
{
    if (discovery == NULL) {
        return;
    }
    /* Rebind group membership as well as probing. This repairs discovery
     * after Wi-Fi roaming or an interface transition, which otherwise used
     * to require restarting GRD. */
    renew_discovery_group(discovery->socket_value);
    send_discovery_query(discovery);
    GRD_INFO("discovery: LAN scan requested");
}

size_t grd_discovery_peers(
    grd_discovery *discovery,
    grd_discovered_peer *peers,
    size_t capacity
)
{
    if (discovery == NULL || peers == NULL) {
        return 0U;
    }
    grd_mutex_lock(&discovery->mutex);
    const uint64_t cutoff = grd_now_micros() - 5000000ULL;
    for (size_t index = 0U; index < discovery->peer_count;) {
        if (discovery->peers[index].last_seen_micros < cutoff) {
            discovery->peers[index] =
                discovery->peers[discovery->peer_count - 1U];
            --discovery->peer_count;
        } else {
            ++index;
        }
    }
    const size_t count =
        discovery->peer_count < capacity ? discovery->peer_count : capacity;
    memcpy(peers, discovery->peers, count * sizeof(*peers));
    grd_mutex_unlock(&discovery->mutex);
    return count;
}

uint64_t grd_discovery_thread_age_ms(const grd_discovery *discovery)
{
    if (discovery == NULL) {
        return 0ULL;
    }
    const uint64_t last = atomic_load_explicit(
        &discovery->thread_last_active, memory_order_relaxed
    );
    return last != 0ULL ? (grd_now_micros() - last) / 1000ULL : 0ULL;
}

void grd_discovery_stop(grd_discovery *discovery)
{
    if (discovery == NULL) {
        return;
    }
    const bool was_running = atomic_load_explicit(
        &discovery->running, memory_order_acquire
    );
    atomic_store_explicit(&discovery->running, false, memory_order_release);
    if (discovery->socket_value != GRD_INVALID_SOCKET) {
        grd_socket_shutdown(discovery->socket_value);
    }
    if (was_running && discovery->thread_started) {
        grd_thread_join(discovery->thread);
    }
    if (discovery->socket_value != GRD_INVALID_SOCKET) {
        grd_socket_close(discovery->socket_value);
    }
    grd_mutex_destroy(&discovery->mutex);
    grd_net_shutdown();
    free(discovery);
}
