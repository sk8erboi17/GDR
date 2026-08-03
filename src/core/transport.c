#include "grd/transport.h"
#include "grd/log.h"
#include "core/stream_policy.h"
#include "grd/platform.h"
#include "core/net.h"

#include <sodium.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENCRYPTION_OVERHEAD crypto_aead_xchacha20poly1305_ietf_ABYTES
#define GRD_SEND_QUEUE_CAPACITY 256U
/* Audio is real-time data: six 10 ms Opus access units are enough to absorb
 * scheduling jitter without allowing a slow observer to build seconds of
 * playback latency. */
#define GRD_AUDIO_SEND_QUEUE_FRAMES 6U
#define GRD_UDP_HEADER_SIZE 36U
#define GRD_UDP_FRAGMENT_HEADER_SIZE 20U
#define GRD_UDP_AEAD_OVERHEAD crypto_aead_xchacha20poly1305_ietf_ABYTES
#define GRD_UDP_MAX_DATAGRAM 1200U
/* How many frames the client may reassemble in parallel. Two slots evict
 * the oldest frame as soon as a third frame overlaps, which at 1080p60 with
 * ~37 fragments/frame happens during connect bursts and fast scene changes:
 * the evicted frame is frequently the IDR carrying SPS/PPS, so VideoToolbox
 * never starts and the client falls back to the software decoder. Four
 * slots absorb the burst (worst case ~3 frames in flight on a saturated
 * pacer) at the cost of ~600 KB of reassembly buffers. */
#define GRD_UDP_FRAME_SLOTS 4U
#define GRD_UDP_PROBE_TYPE 1U
#define GRD_UDP_ACK_TYPE 2U
#define GRD_UDP_FRAGMENT_CAPACITY \
    (GRD_UDP_MAX_DATAGRAM - GRD_UDP_HEADER_SIZE - GRD_UDP_AEAD_OVERHEAD - \
     GRD_UDP_FRAGMENT_HEADER_SIZE)
/* How long the sender may keep retrying a congested socket before dropping
 * the frame, and how long the client waits for NACK retransmissions before
 * giving up on a frame. Both keep the pipelines bounded on a saturated link. */
#define GRD_UDP_SEND_BUDGET_US 12000U
#define GRD_UDP_NACK_WINDOW_US 25000U
/* Real anti-replay window: out-of-order datagrams inside the window are
 * accepted (and counted as reordered), so they are never misclassified as
 * network loss; only duplicates and packets older than the window are
 * dropped. */
#define GRD_UDP_REORDER_WINDOW_BITS 1024U
/* Pacer: per-client UDP send thread with priorities, sub-millisecond pacing
 * and frame admission control. Sending never happens under host->mutex or in
 * the capture/encode thread. */
#define GRD_PACER_QUEUE_CAPACITY 32U
#define GRD_PACER_RETX_FRAMES 8U
/* Complex game transitions temporarily make both an IDR and the first
 * following P-frames larger. Give that short burst enough room to drain
 * instead of breaking the reference chain to save a few milliseconds. The
 * encoder still runs below the wire budget, so this does not create a
 * persistent queue. */
#define GRD_PFRAME_DEADLINE_PERIODS 3U
#define GRD_KEYFRAME_DEADLINE_PERIODS 4U
#define GRD_PFRAME_MAX_DEADLINE_US 75000ULL
/* The UI sends an authenticated control heartbeat once per second. A dead
 * controller must not keep the exclusive input lease forever after a crash
 * or a Wi-Fi interruption. Same-device reconnects replace the old socket
 * immediately; a different device may take over after this grace period. */
#define GRD_CONTROLLER_LEASE_TIMEOUT_US 5000000ULL

typedef enum grd_pacer_priority {
    GRD_PACER_PRIO_CONTROL = 0,
    GRD_PACER_PRIO_AUDIO = 1,
    GRD_PACER_PRIO_RETX = 2,
    GRD_PACER_PRIO_FEC = 3,
    GRD_PACER_PRIO_KEYFRAME = 4,
    GRD_PACER_PRIO_VIDEO = 5
} grd_pacer_priority;

/* Explicit unit role: what a unit IS, independently of its priority. Only
 * VIDEO_ORIGINAL is cached for NACK, counted as a sent frame and used to
 * generate FEC; retransmissions are never re-cached and never re-protected. */
typedef enum grd_pacer_kind {
    GRD_PACER_KIND_CONTROL = 0,
    GRD_PACER_KIND_AUDIO = 1,
    GRD_PACER_KIND_FEC = 2,
    GRD_PACER_KIND_VIDEO_RETX = 3,
    GRD_PACER_KIND_VIDEO_ORIGINAL = 4
} grd_pacer_kind;

typedef struct grd_pacer_unit {
    grd_pacer_kind kind;
    grd_pacer_priority priority;
    uint64_t frame_id;
    uint16_t fragment_count;
    uint32_t frame_size;
    uint64_t deadline_micros;
    bool keyframe;
    bool audio;
    uint16_t audio_type;
    uint8_t audio_data[GRD_UDP_MAX_DATAGRAM];
    size_t audio_size;
    uint8_t prefix[GRD_BROADCAST_MAX_INLINE_PREFIX];
    size_t prefix_length;
    const uint8_t *payload;
    size_t payload_length;
    grd_owned_buffer payload_ref;
    /* Optional retransmission bitmap (1 = missing fragment). */
    uint8_t *retx_bitmap;
    size_t retx_bitmap_size;
} grd_pacer_unit;

typedef struct grd_udp_reorder_window {
    uint64_t highest;
    uint8_t bitmap[GRD_UDP_REORDER_WINDOW_BITS / 8U];
} grd_udp_reorder_window;

/* Peek: does the sequence pass the anti-replay check WITHOUT mutating the
 * window? The commit is deferred until after AEAD verification, so an
 * invalid datagram with a very high sequence cannot push the window forward
 * and age out legitimate packets. */
static bool reorder_window_peek(
    const grd_udp_reorder_window *window,
    uint64_t sequence,
    bool *reordered
)
{
    if (window == NULL) {
        return false;
    }
    if (window->highest == 0U || sequence > window->highest) {
        if (reordered != NULL) {
            *reordered = false;
        }
        return true;
    }
    const uint64_t distance = window->highest - sequence;
    if (distance >= GRD_UDP_REORDER_WINDOW_BITS) {
        return false;
    }
    const size_t byte = (size_t)(distance / 8U);
    const uint8_t bit = (uint8_t)(1U << (distance % 8U));
    if ((window->bitmap[byte] & bit) != 0U) {
        return false;
    }
    if (reordered != NULL) {
        *reordered = true;
    }
    return true;
}

/* Commit the window update for an already authenticated sequence. */
static void reorder_window_commit(
    grd_udp_reorder_window *window,
    uint64_t sequence
)
{
    if (window == NULL) {
        return;
    }
    if (window->highest == 0U || sequence > window->highest) {
        const uint64_t advance =
            window->highest == 0U ? 0U : sequence - window->highest;
        if (advance >= GRD_UDP_REORDER_WINDOW_BITS) {
            memset(window->bitmap, 0, sizeof(window->bitmap));
        } else if (advance != 0U) {
            /* New index i receives the bit previously at i - advance. */
            for (size_t i = GRD_UDP_REORDER_WINDOW_BITS;
                 i-- > advance;) {
                const size_t source = i - (size_t)advance;
                const uint8_t bit = (uint8_t)(
                    (window->bitmap[source / 8U] >> (source % 8U)) & 1U
                );
                if (bit != 0U) {
                    window->bitmap[i / 8U] |=
                        (uint8_t)(1U << (i % 8U));
                } else {
                    window->bitmap[i / 8U] &=
                        (uint8_t)~(1U << (i % 8U));
                }
            }
            for (size_t i = 0U; i < (size_t)advance; ++i) {
                window->bitmap[i / 8U] &=
                    (uint8_t)~(1U << (i % 8U));
            }
        }
        window->highest = sequence;
        window->bitmap[0] |= 1U;
        return;
    }
    const uint64_t distance = window->highest - sequence;
    if (distance < GRD_UDP_REORDER_WINDOW_BITS) {
        window->bitmap[(size_t)(distance / 8U)] |=
            (uint8_t)(1U << (distance % 8U));
    }
}

typedef struct grd_udp_frame_slot {
    uint64_t frame_id;
    uint32_t frame_size;
    uint16_t fragment_count;
    uint16_t received_fragments;
    uint8_t *data;
    uint8_t *bitmap;
    size_t bitmap_size;
    uint8_t *fec_parity;
    /* One bit per FEC block: a block is only usable for reconstruction once
     * its parity datagram actually arrived. A zeroed calloc buffer must not
     * be mistaken for a received parity. */
    uint8_t *fec_received_bitmap;
    uint16_t fec_block_count;
    bool active;
    uint64_t nack_sent_micros;
} grd_udp_frame_slot;

typedef struct grd_outgoing_packet {
    grd_packet_type type;
    /* Small inline prefix (for example the wire timestamp header) copied by
     * the queue so the packet is self-contained. */
    uint8_t inline_prefix[GRD_BROADCAST_MAX_INLINE_PREFIX];
    size_t inline_prefix_length;
    /* Referenced payload; freed via ref.release or free() when
     * ref.release is NULL (queue-owned copy). */
    const uint8_t *payload;
    size_t payload_length;
    size_t total_length;
    grd_owned_buffer ref;
} grd_outgoing_packet;

typedef struct grd_send_queue {
    grd_mutex mutex;
    grd_cond condition;
    grd_outgoing_packet packets[GRD_SEND_QUEUE_CAPACITY];
    size_t count;
    bool stopping;
} grd_send_queue;

typedef struct grd_crypto {
    uint8_t tx_key[GRD_SESSION_KEY_BYTES];
    uint8_t rx_key[GRD_SESSION_KEY_BYTES];
    uint64_t tx_sequence;
    uint64_t rx_sequence;
    uint8_t *tx_buffer;
    size_t tx_buffer_capacity;
    uint8_t *rx_encrypted;
    size_t rx_encrypted_capacity;
    uint8_t *rx_plain;
    size_t rx_plain_capacity;
} grd_crypto;

typedef struct grd_host_client grd_host_client;

struct grd_host_client {
    grd_socket socket_value;
    grd_thread thread;
    grd_thread send_thread;
    grd_mutex send_mutex;
    grd_send_queue send_queue;
    _Atomic bool running;
    bool occupied;
    bool thread_started;
    bool send_thread_started;
    bool finished;
    bool media_channel;
    /* UDP peer state is produced by the UDP receive thread (which writes
     * udp_address then publishes udp_ready with release) and consumed by
     * the pacer threads (acquire on udp_ready before reading the address). */
    _Atomic bool udp_ready;
    _Atomic bool udp_failed;
    /* True after this client's pacer drops an encoded reference. P-frames
     * are rejected until a recovery IDR is queued, otherwise the receiver
     * sees a syntactically complete but undecodable H.264 chain with zero
     * reported packet loss. */
    _Atomic bool video_discontinuity;
    /* Once the recovery IDR is in the pacer, following P-frames form the new
     * reference chain and must stay queued behind it. Dropping them merely
     * because the IDR has not finished sending creates another broken chain. */
    _Atomic bool recovery_keyframe_queued;
    uint8_t udp_token[GRD_VIDEO_UDP_TOKEN_BYTES];
    struct sockaddr_storage udp_address;
    grd_socklen udp_address_length;
    grd_mutex udp_mutex;
    uint64_t udp_tx_sequence;
    grd_udp_reorder_window udp_rx_window;
    uint64_t udp_tx_frame_id;
    grd_mutex pacer_mutex;
    grd_cond pacer_condition;
    grd_pacer_unit pacer_queue[GRD_PACER_QUEUE_CAPACITY];
    size_t pacer_count;
    bool pacer_stopping;
    grd_thread pacer_thread;
    bool pacer_thread_started;
    grd_pacer_unit retx_ring[GRD_PACER_RETX_FRAMES];
    size_t retx_ring_next;
    uint64_t retx_ring_ids[GRD_PACER_RETX_FRAMES];
    uint64_t pacer_next_send_micros;
    char device_id[37];
    grd_role role;
    grd_os peer_os;
    _Atomic uint64_t last_control_activity_micros;
    grd_crypto crypto;
    struct grd_host *host;
};

struct grd_host {
    grd_config config;
    grd_socket listen_socket;
    grd_thread accept_thread;
    bool accept_thread_started;
    grd_socket udp_socket;
    grd_thread udp_thread;
    bool udp_thread_started;
    grd_mutex mutex;
    _Atomic bool running;
    _Atomic bool keyframe_pending;
    _Atomic uint32_t pacing_bits_per_second;
    _Atomic uint32_t frame_period_us;
    _Atomic bool fec_enabled;
    _Atomic uint32_t rtt_micros;
    /* Shared across the per-client pacer threads and the UDP/NACK thread:
     * counters must be atomic (relaxed increments; the logger thread only
     * reads them for the 5 s stats). */
    _Atomic uint64_t udp_video_frames_sent;
    _Atomic uint64_t udp_video_fragments_sent;
    _Atomic uint64_t udp_video_frames_dropped;
    /* Drop reasons are kept separate because only admission/queue/deadline/
     * send failures mean the wire budget is congested. Frames intentionally
     * discarded after a reference gap (or purged in front of its recovery
     * IDR) must not make ABR cut the bitrate a second time. */
    _Atomic uint64_t udp_video_congestion_dropped;
    _Atomic uint64_t udp_drop_admission;
    _Atomic uint64_t udp_drop_queue;
    _Atomic uint64_t udp_drop_deadline;
    _Atomic uint64_t udp_drop_send;
    _Atomic uint64_t udp_drop_discontinuity;
    _Atomic uint64_t udp_drop_recovery_purge;
    _Atomic uint64_t udp_pacer_queue_peak;
    _Atomic uint64_t udp_keyframes_dropped;
    _Atomic uint64_t udp_normal_keyframes_queued;
    _Atomic uint64_t udp_recovery_keyframes_queued;
    _Atomic uint64_t udp_audio_datagrams_sent;
    _Atomic uint64_t udp_nack_received;
    _Atomic uint64_t udp_retx_fragments_sent;
    _Atomic uint64_t udp_video_fec_fragments_sent;
    _Atomic uint64_t udp_fec_fragments_dropped;
    /* 1 s rolling initiating-pressure percentage exposed to ABR. Recovery
     * purge and discontinuity descendants are deliberately excluded. */
    _Atomic uint32_t udp_recent_initiating_drop_percent;
    _Atomic uint64_t udp_recent_initiating_drop_generation;
    uint64_t udp_stats_log_window_start;
    uint64_t udp_stats_drop_window_start;
    uint64_t udp_stats_drop_frames_at;
    uint64_t udp_stats_drop_admission_at;
    uint64_t udp_stats_drop_queue_at;
    uint64_t udp_stats_drop_deadline_at;
    uint64_t udp_stats_drop_send_at;
    uint64_t udp_stats_log_frames_at;
    uint64_t udp_stats_log_keydrop_at;
    uint64_t udp_stats_log_normal_idr_at;
    uint64_t udp_stats_log_recovery_idr_at;
    uint64_t udp_stats_log_dropped_at;
    uint64_t udp_stats_log_fragments_at;
    uint64_t udp_stats_log_audio_at;
    uint64_t udp_stats_log_nack_at;
    uint64_t udp_stats_log_retx_at;
    uint64_t udp_stats_log_fec_at;
    uint64_t udp_stats_log_fecdrop_at;
    uint64_t udp_stats_log_admission_at;
    uint64_t udp_stats_log_queue_at;
    uint64_t udp_stats_log_deadline_at;
    uint64_t udp_stats_log_send_at;
    uint64_t udp_stats_log_discontinuity_at;
    uint64_t udp_stats_log_purge_at;
    grd_packet_callback callback;
    void *userdata;
    grd_host_client clients[GRD_MAX_CONNECTIONS];
};

struct grd_connection {
    grd_socket socket_value;
    grd_thread receive_thread;
    grd_thread send_thread;
    bool receive_thread_started;
    bool send_thread_started;
    bool media_channel;
    grd_socket udp_socket;
    grd_thread udp_thread;
    bool udp_thread_started;
    _Atomic bool udp_enabled;
    bool udp_token_valid;
    uint8_t udp_token[GRD_VIDEO_UDP_TOKEN_BYTES];
    uint64_t udp_tx_sequence;
    grd_udp_reorder_window udp_rx_window;
    uint64_t udp_retx_fragments_sent;
    /* H.264 access units must reach the decoder in frame-id order. Complete
     * newer slots wait here while an older slot is repaired by FEC/NACK. */
    uint64_t udp_last_frame_delivered;
    grd_udp_frame_slot udp_slots[GRD_UDP_FRAME_SLOTS];
    uint64_t udp_last_keyframe_request_micros;
    uint64_t udp_stats_window_start_micros;
    /* Stall accounting: one lost "slot" per empty receive poll (500 ms of
     * silence) while the stream is down. */
    uint64_t udp_stats_stall_expected;
    uint64_t udp_stats_stall_lost;
    /* Reorder-tolerant window: loss is derived at report time from the
     * highest sequence seen and the number of packets received, so datagrams
     * that arrive slightly out of order are not counted as lost. */
    uint64_t udp_stats_window_first;
    uint64_t udp_stats_window_high;
    uint64_t udp_stats_window_count;
    uint64_t udp_probe_sent_micros;
    bool udp_probe_pending;
    uint64_t udp_rtt_micros;
    uint64_t udp_datagrams_received;
    uint64_t udp_lost_datagrams;
    uint64_t udp_frames_received;
    uint64_t udp_frames_incomplete;
    uint64_t udp_keyframe_requests;
    uint64_t udp_receiver_reordered;
    uint64_t udp_receiver_duplicate;
    uint64_t udp_decrypt_failures;
    uint64_t udp_last_arrival_micros;
    uint64_t udp_last_delta_micros;
    uint64_t udp_jitter_accum;
    uint64_t udp_jitter_samples;
    /* Heartbeats for the thread map (updated by each transport thread). */
    _Atomic uint64_t udp_thread_last_active;
    _Atomic uint64_t tcp_rx_thread_last_active;
    _Atomic uint64_t tcp_tx_thread_last_active;
    uint64_t udp_log_window_start;
    uint64_t udp_log_datagrams_at;
    uint64_t udp_log_lost_at;
    uint64_t udp_log_frames_at;
    uint64_t udp_log_incomplete_at;
    uint64_t udp_log_keyframes_at;
    uint64_t udp_log_reorder_at;
    uint64_t udp_log_dup_at;
    uint64_t udp_log_decrypt_at;
    bool owns_network;
    grd_mutex send_mutex;
    grd_send_queue send_queue;
    /* NACKs/probes and realtime input share one UDP sequence. Serialize
     * encryption so UI and receive threads cannot reuse a nonce. */
    grd_mutex udp_send_mutex;
    _Atomic bool running;
    grd_role role;
    grd_crypto crypto;
    grd_packet_callback callback;
    void *userdata;
};

static bool connection_is_running(grd_connection *connection)
{
    return atomic_load_explicit(&connection->running, memory_order_acquire);
}

static void connection_set_running(grd_connection *connection, bool running)
{
    atomic_store_explicit(&connection->running, running, memory_order_release);
}

static bool packet_is_latest_wins(grd_packet_type type)
{
    return type == GRD_PACKET_VIDEO_FRAME ||
           type == GRD_PACKET_CURSOR ||
           type == GRD_PACKET_CURSOR_SHAPE;
}

static bool packet_is_input(grd_packet_type type)
{
    return type == GRD_PACKET_INPUT;
}

static bool packet_is_audio_frame(grd_packet_type type)
{
    return type == GRD_PACKET_AUDIO_FRAME;
}

static int packet_priority(grd_packet_type type)
{
    if (type == GRD_PACKET_INPUT) {
        return 0;
    }
    if (type == GRD_PACKET_CURSOR || type == GRD_PACKET_CURSOR_SHAPE) {
        return 1;
    }
    if (type == GRD_PACKET_AUDIO_FRAME || type == GRD_PACKET_AUDIO_CONFIG) {
        return 2;
    }
    if (type == GRD_PACKET_CLIPBOARD) {
        return 4;
    }
    return 3;
}

static bool input_payload_is_kind(
    const grd_outgoing_packet *packet,
    uint8_t kind
)
{
    if (packet == NULL || !packet_is_input(packet->type) ||
        packet->payload_length != sizeof(grd_input_event) ||
        packet->payload == NULL) {
        return false;
    }
    const grd_input_event *event = (const grd_input_event *)packet->payload;
    return event->kind == kind;
}

static bool packet_is_droppable(grd_packet_type type)
{
    return packet_is_latest_wins(type) ||
           type == GRD_PACKET_AUDIO_FRAME ||
           type == GRD_PACKET_CLIPBOARD;
}

static bool packet_is_media(grd_packet_type type)
{
    return type == GRD_PACKET_CLIPBOARD ||
           type == GRD_PACKET_VIDEO_CONFIG ||
           type == GRD_PACKET_VIDEO_FRAME ||
           type == GRD_PACKET_AUDIO_CONFIG ||
           type == GRD_PACKET_AUDIO_FRAME;
}

static bool packet_belongs_to_channel(
    const grd_host *host,
    const grd_host_client *client,
    grd_packet_type type
)
{
    if (type == GRD_PACKET_CLIPBOARD) {
        if (client->media_channel) {
            return true;
        }
        /* New clients carry clipboard in the media mailbox as well. Keep it
         * on the control socket only for legacy clients that have no media
         * half, preserving protocol compatibility without delaying cursor
         * packets for current clients. */
        if (host != NULL && client->device_id[0] != '\0') {
            for (size_t index = 0U; index < GRD_MAX_CONNECTIONS; ++index) {
                const grd_host_client *other = &host->clients[index];
                if (other != client && other->occupied &&
                    other->media_channel &&
                    strcmp(other->device_id, client->device_id) == 0) {
                    return false;
                }
            }
        }
        return true;
    }
    if (packet_is_media(type)) {
        /* Video is UDP-only. A control socket without a media/UDP half must
         * never receive a silent TCP video downgrade. */
        if (type == GRD_PACKET_VIDEO_FRAME) {
            return client->media_channel;
        }
        if (client->media_channel) {
            return true;
        }
        /* Compatibility for pre-channel clients: if this control socket has
         * no matching media half, keep delivering media on it. New clients
         * always have a same-device media socket, so they receive each frame
         * exactly once on the dedicated channel. */
        if (host != NULL && client->device_id[0] != '\0') {
            for (size_t index = 0U; index < GRD_MAX_CONNECTIONS; ++index) {
                const grd_host_client *other = &host->clients[index];
                if (other != client && other->occupied &&
                    other->media_channel &&
                    strcmp(other->device_id, client->device_id) == 0) {
                    return false;
                }
            }
        }
        return true;
    }
    /* Cursor and status/control updates stay on the control channel. Unknown
     * future packets are mirrored to both channels until
     * they are explicitly classified. */
    if (type == GRD_PACKET_CURSOR ||
        type == GRD_PACKET_CURSOR_SHAPE ||
        type == GRD_PACKET_ERROR ||
        type == GRD_PACKET_PING) {
        return !client->media_channel;
    }
    return true;
}

static void outgoing_packet_release(grd_outgoing_packet *packet)
{
    if (packet == NULL) {
        return;
    }
    if (packet->ref.release != NULL) {
        packet->ref.release((void *)packet->ref.opaque);
    } else if (packet->payload != NULL) {
        free((void *)packet->payload);
    }
    memset(packet, 0, sizeof(*packet));
}

static void send_queue_init(grd_send_queue *queue)
{
    memset(queue, 0, sizeof(*queue));
    grd_mutex_init(&queue->mutex);
    grd_cond_init(&queue->condition);
}

static void send_queue_stop(grd_send_queue *queue)
{
    if (queue == NULL) {
        return;
    }
    grd_mutex_lock(&queue->mutex);
    queue->stopping = true;
    for (size_t index = 0U; index < queue->count; ++index) {
        outgoing_packet_release(&queue->packets[index]);
    }
    queue->count = 0U;
    grd_cond_broadcast(&queue->condition);
    grd_mutex_unlock(&queue->mutex);
}

static void send_queue_destroy(grd_send_queue *queue)
{
    if (queue == NULL) {
        return;
    }
    send_queue_stop(queue);
    grd_mutex_lock(&queue->mutex);
    for (size_t index = 0U; index < queue->count; ++index) {
        outgoing_packet_release(&queue->packets[index]);
    }
    queue->count = 0U;
    grd_mutex_unlock(&queue->mutex);
    grd_cond_destroy(&queue->condition);
    grd_mutex_destroy(&queue->mutex);
}

static void send_queue_remove(
    grd_send_queue *queue,
    size_t index
)
{
    if (index >= queue->count) {
        return;
    }
    outgoing_packet_release(&queue->packets[index]);
    if (index + 1U < queue->count) {
        memmove(
            &queue->packets[index],
            &queue->packets[index + 1U],
            (queue->count - index - 1U) * sizeof(queue->packets[0])
        );
    }
    --queue->count;
    memset(&queue->packets[queue->count], 0, sizeof(queue->packets[0]));
}

/* Always consumes entry: on success the queue owns it, on failure it is
 * released here. */
static grd_status send_queue_enqueue_locked(
    grd_send_queue *queue,
    grd_outgoing_packet *entry
)
{
    if (queue->stopping) {
        outgoing_packet_release(entry);
        return GRD_IO_ERROR;
    }

    if (packet_is_latest_wins(entry->type)) {
        for (size_t index = 0U; index < queue->count; ++index) {
            if (queue->packets[index].type == entry->type) {
                outgoing_packet_release(&queue->packets[index]);
                queue->packets[index] = *entry;
                grd_cond_signal(&queue->condition);
                return GRD_OK;
            }
        }
    }

    if (packet_is_audio_frame(entry->type)) {
        size_t audio_count = 0U;
        for (size_t index = 0U; index < queue->count; ++index) {
            if (packet_is_audio_frame(queue->packets[index].type)) {
                ++audio_count;
            }
        }
        while (audio_count >= GRD_AUDIO_SEND_QUEUE_FRAMES) {
            size_t oldest_audio = queue->count;
            for (size_t index = 0U; index < queue->count; ++index) {
                if (packet_is_audio_frame(queue->packets[index].type)) {
                    oldest_audio = index;
                    break;
                }
            }
            if (oldest_audio == queue->count) {
                break;
            }
            send_queue_remove(queue, oldest_audio);
            --audio_count;
        }
    }

    if (packet_is_input(entry->type) &&
        entry->payload_length == sizeof(grd_input_event) &&
        entry->ref.release == NULL) {
        const grd_input_event *input =
            (const grd_input_event *)entry->payload;
        /* Relative samples preserve their original cadence. Coalescing them
         * retained the distance but turned an 8 ms event run into one jump,
         * which felt like mouse inertia. Absolute desktop motion remains
         * latest-wins because only its final position matters. */
        if (input->kind == GRD_INPUT_POINTER_MOVE && queue->count != 0U &&
            input_payload_is_kind(
                &queue->packets[queue->count - 1U],
                GRD_INPUT_POINTER_MOVE
            )) {
            memcpy(
                (void *)queue->packets[queue->count - 1U].payload,
                entry->payload,
                sizeof(grd_input_event)
            );
            outgoing_packet_release(entry);
            grd_cond_signal(&queue->condition);
            return GRD_OK;
        }
    }

    if (queue->count == GRD_SEND_QUEUE_CAPACITY) {
        size_t drop_index = GRD_SEND_QUEUE_CAPACITY;
        for (size_t index = 0U; index < queue->count; ++index) {
            if (packet_is_droppable(queue->packets[index].type)) {
                drop_index = index;
                break;
            }
        }
        if (drop_index == GRD_SEND_QUEUE_CAPACITY) {
            outgoing_packet_release(entry);
            return GRD_BUSY;
        }
        send_queue_remove(queue, drop_index);
    }

    queue->packets[queue->count++] = *entry;
    grd_cond_signal(&queue->condition);
    return GRD_OK;
}

static grd_status send_queue_push(
    grd_send_queue *queue,
    grd_packet_type type,
    const void *payload,
    size_t payload_length
)
{
    if (queue == NULL ||
        payload_length > GRD_MAX_PACKET_SIZE - ENCRYPTION_OVERHEAD ||
        (payload_length != 0U && payload == NULL)) {
        return GRD_INVALID_ARGUMENT;
    }
    uint8_t *copy = NULL;
    if (payload_length != 0U) {
        copy = malloc(payload_length);
        if (copy == NULL) {
            return GRD_OUT_OF_MEMORY;
        }
        memcpy(copy, payload, payload_length);
    }
    grd_outgoing_packet entry = {
        .type = type,
        .payload = copy,
        .payload_length = payload_length,
        .total_length = payload_length
    };
    grd_mutex_lock(&queue->mutex);
    const grd_status status = send_queue_enqueue_locked(queue, &entry);
    grd_mutex_unlock(&queue->mutex);
    return status;
}

static grd_status send_queue_push_owned(
    grd_send_queue *queue,
    grd_packet_type type,
    const grd_buf_part *parts,
    size_t part_count,
    grd_owned_buffer *ref
)
{
    if (queue == NULL || ref == NULL || ref->release == NULL ||
        parts == NULL || part_count == 0U ||
        part_count > GRD_BROADCAST_MAX_PARTS) {
        return GRD_INVALID_ARGUMENT;
    }
    size_t prefix_length = 0U;
    for (size_t index = 0U; index + 1U < part_count; ++index) {
        prefix_length += parts[index].length;
    }
    const grd_buf_part *payload_part = &parts[part_count - 1U];
    if (prefix_length > GRD_BROADCAST_MAX_INLINE_PREFIX ||
        payload_part->data == NULL || payload_part->length == 0U ||
        payload_part->length >
            GRD_MAX_PACKET_SIZE - ENCRYPTION_OVERHEAD - prefix_length) {
        return GRD_INVALID_ARGUMENT;
    }
    grd_outgoing_packet entry = {
        .type = type,
        .inline_prefix_length = prefix_length,
        .payload = payload_part->data,
        .payload_length = payload_part->length,
        .total_length = prefix_length + payload_part->length,
        .ref = *ref
    };
    size_t written = 0U;
    for (size_t index = 0U; index + 1U < part_count; ++index) {
        memcpy(
            entry.inline_prefix + written,
            parts[index].data,
            parts[index].length
        );
        written += parts[index].length;
    }
    grd_mutex_lock(&queue->mutex);
    const grd_status status = send_queue_enqueue_locked(queue, &entry);
    grd_mutex_unlock(&queue->mutex);
    return status;
}

static bool send_queue_pop(
    grd_send_queue *queue,
    grd_outgoing_packet *packet
)
{
    if (queue == NULL || packet == NULL) {
        return false;
    }
    grd_mutex_lock(&queue->mutex);
    while (queue->count == 0U && !queue->stopping) {
        grd_cond_wait(&queue->condition, &queue->mutex);
    }
    if (queue->count == 0U) {
        grd_mutex_unlock(&queue->mutex);
        return false;
    }
    size_t selected = 0U;
    int selected_priority = packet_priority(queue->packets[0].type);
    for (size_t index = 1U; index < queue->count; ++index) {
        const int priority = packet_priority(queue->packets[index].type);
        if (priority < selected_priority) {
            selected = index;
            selected_priority = priority;
        }
    }
    *packet = queue->packets[selected];
    if (selected + 1U < queue->count) {
        memmove(
            &queue->packets[selected],
            &queue->packets[selected + 1U],
            (queue->count - selected - 1U) * sizeof(queue->packets[0])
        );
    }
    --queue->count;
    memset(&queue->packets[queue->count], 0, sizeof(queue->packets[0]));
    grd_mutex_unlock(&queue->mutex);
    return true;
}

static bool send_all(grd_socket socket_value, const uint8_t *data, size_t length)
{
    while (length != 0U) {
#if defined(_WIN32)
        const int chunk = length > (size_t)INT_MAX ? INT_MAX : (int)length;
#else
        const size_t chunk =
            length > (size_t)INT_MAX ? (size_t)INT_MAX : length;
#endif
        const int sent = (int)send(
            socket_value,
            (const char *)data,
            chunk,
            0
        );
        if (sent <= 0) {
            return false;
        }
        data += (size_t)sent;
        length -= (size_t)sent;
    }
    return true;
}

static bool receive_all(grd_socket socket_value, uint8_t *data, size_t length)
{
    while (length != 0U) {
#if defined(_WIN32)
        const int chunk = length > (size_t)INT_MAX ? INT_MAX : (int)length;
#else
        const size_t chunk =
            length > (size_t)INT_MAX ? (size_t)INT_MAX : length;
#endif
        const int received = (int)recv(
            socket_value,
            (char *)data,
            chunk,
            0
        );
        if (received <= 0) {
            return false;
        }
        data += (size_t)received;
        length -= (size_t)received;
    }
    return true;
}

static bool send_raw_packet(
    grd_socket socket_value,
    grd_packet_type type,
    uint64_t sequence,
    const void *payload,
    size_t payload_length
)
{
    if (payload_length > GRD_MAX_PACKET_SIZE) {
        return false;
    }
    const grd_packet_header header = {
        .magic = GRD_PACKET_MAGIC,
        .version = GRD_PROTOCOL_VERSION,
        .type = (uint16_t)type,
        .payload_length = (uint32_t)payload_length,
        .sequence = sequence
    };
    uint8_t wire_header[20];
    grd_protocol_encode_header(&header, wire_header);
    return send_all(socket_value, wire_header, sizeof(wire_header)) &&
           (payload_length == 0U ||
            send_all(socket_value, payload, payload_length));
}

static bool receive_raw_packet(
    grd_socket socket_value,
    grd_packet_header *header,
    uint8_t **payload
)
{
    uint8_t wire_header[20];
    *payload = NULL;
    if (!receive_all(socket_value, wire_header, sizeof(wire_header)) ||
        grd_protocol_decode_header(wire_header, header) != GRD_OK) {
        return false;
    }
    if (header->payload_length == 0U) {
        return true;
    }
    *payload = malloc(header->payload_length);
    return *payload != NULL &&
           receive_all(socket_value, *payload, header->payload_length);
}

static void derive_direction_key(
    const uint8_t master[GRD_SESSION_KEY_BYTES],
    const char *label,
    uint8_t output[GRD_SESSION_KEY_BYTES]
)
{
    (void)crypto_generichash(
        output,
        GRD_SESSION_KEY_BYTES,
        (const uint8_t *)label,
        strlen(label),
        master,
        GRD_SESSION_KEY_BYTES
    );
}

static void crypto_initialize(
    grd_crypto *crypto,
    const uint8_t master[GRD_SESSION_KEY_BYTES],
    bool server
)
{
    memset(crypto, 0, sizeof(*crypto));
    derive_direction_key(
        master, server ? "grd-server-tx" : "grd-client-tx", crypto->tx_key
    );
    derive_direction_key(
        master, server ? "grd-client-tx" : "grd-server-tx", crypto->rx_key
    );
    crypto->tx_sequence = 1U;
}

static void crypto_destroy(grd_crypto *crypto)
{
    if (crypto == NULL) {
        return;
    }
    if (crypto->tx_buffer != NULL) {
        grd_secure_zero(crypto->tx_buffer, crypto->tx_buffer_capacity);
        free(crypto->tx_buffer);
    }
    if (crypto->rx_encrypted != NULL) {
        grd_secure_zero(crypto->rx_encrypted, crypto->rx_encrypted_capacity);
        free(crypto->rx_encrypted);
    }
    if (crypto->rx_plain != NULL) {
        grd_secure_zero(crypto->rx_plain, crypto->rx_plain_capacity);
        free(crypto->rx_plain);
    }
    memset(crypto, 0, sizeof(*crypto));
}

static void build_nonce(uint64_t sequence, uint8_t nonce[24])
{
    memset(nonce, 0, 24U);
    for (size_t index = 0U; index < 8U; ++index) {
        nonce[16U + index] = (uint8_t)(sequence >> ((7U - index) * 8U));
    }
}

static void udp_write_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void udp_write_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void udp_write_u64(uint8_t *output, uint64_t value)
{
    for (size_t index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> (56U - index * 8U));
    }
}

static uint16_t udp_read_u16(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t udp_read_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) |
           ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) |
           (uint32_t)input[3];
}

static uint64_t udp_read_u64(const uint8_t *input)
{
    uint64_t value = 0U;
    for (size_t index = 0U; index < 8U; ++index) {
        value = (value << 8U) | input[index];
    }
    return value;
}

static void udp_encode_header(
    uint8_t output[GRD_UDP_HEADER_SIZE],
    uint16_t type,
    uint32_t payload_length,
    uint64_t sequence,
    const uint8_t token[GRD_VIDEO_UDP_TOKEN_BYTES]
)
{
    udp_write_u32(output, 0x47524455U); /* "GRDU" */
    udp_write_u16(output + 4U, GRD_PROTOCOL_VERSION);
    udp_write_u16(output + 6U, type);
    udp_write_u32(output + 8U, payload_length);
    udp_write_u64(output + 12U, sequence);
    memcpy(output + 20U, token, GRD_VIDEO_UDP_TOKEN_BYTES);
}

static bool udp_decode_header(
    const uint8_t *input,
    size_t input_size,
    uint16_t *type,
    uint32_t *payload_length,
    uint64_t *sequence,
    const uint8_t **token
)
{
    if (input == NULL || input_size < GRD_UDP_HEADER_SIZE ||
        udp_read_u32(input) != 0x47524455U ||
        udp_read_u16(input + 4U) != GRD_PROTOCOL_VERSION) {
        return false;
    }
    *type = udp_read_u16(input + 6U);
    *payload_length = udp_read_u32(input + 8U);
    *sequence = udp_read_u64(input + 12U);
    *token = input + 20U;
    return *payload_length >= GRD_UDP_AEAD_OVERHEAD &&
           (size_t)*payload_length + GRD_UDP_HEADER_SIZE == input_size;
}

/* Builds and authenticates one datagram for the given sequence without
 * sending it, so a congested socket can retry the exact same bytes. */
static bool udp_prepare_datagram(
    uint8_t wire[GRD_UDP_MAX_DATAGRAM],
    size_t *wire_size,
    const uint8_t key[GRD_SESSION_KEY_BYTES],
    uint64_t sequence,
    const uint8_t token[GRD_VIDEO_UDP_TOKEN_BYTES],
    uint16_t type,
    const void *payload,
    size_t payload_length
)
{
    if (wire == NULL || wire_size == NULL ||
        (payload_length != 0U && payload == NULL) ||
        payload_length > GRD_UDP_MAX_DATAGRAM - GRD_UDP_HEADER_SIZE -
                              GRD_UDP_AEAD_OVERHEAD) {
        return false;
    }
    const uint32_t encrypted_capacity =
        (uint32_t)(payload_length + GRD_UDP_AEAD_OVERHEAD);
    udp_encode_header(wire, type, encrypted_capacity, sequence, token);
    uint8_t nonce[24];
    build_nonce(sequence, nonce);
    unsigned long long encrypted_length = 0U;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            wire + GRD_UDP_HEADER_SIZE,
            &encrypted_length,
            payload,
            payload_length,
            wire,
            GRD_UDP_HEADER_SIZE,
            NULL,
            nonce,
            key
        ) != 0 || encrypted_length != encrypted_capacity) {
        return false;
    }
    *wire_size = GRD_UDP_HEADER_SIZE + (size_t)encrypted_length;
    return true;
}

static bool udp_send_datagram(
    grd_socket socket_value,
    const struct sockaddr *destination,
    grd_socklen destination_length,
    const uint8_t *wire,
    size_t wire_size
)
{
    const int sent = destination == NULL
                         ? (int)send(socket_value, (const char *)wire, wire_size, 0)
                         : (int)sendto(
                               socket_value,
                               (const char *)wire,
                               wire_size,
                               0,
                               destination,
                               destination_length
                           );
    return sent == (int)wire_size;
}

static bool udp_encrypt_send(
    grd_socket socket_value,
    const struct sockaddr *destination,
    grd_socklen destination_length,
    const uint8_t key[GRD_SESSION_KEY_BYTES],
    uint64_t *sequence,
    const uint8_t token[GRD_VIDEO_UDP_TOKEN_BYTES],
    uint16_t type,
    const void *payload,
    size_t payload_length
)
{
    if (socket_value == GRD_INVALID_SOCKET || sequence == NULL) {
        return false;
    }
    uint8_t wire[GRD_UDP_MAX_DATAGRAM];
    size_t wire_size = 0U;
    const uint64_t packet_sequence = *sequence;
    if (!udp_prepare_datagram(
            wire, &wire_size, key, packet_sequence, token, type,
            payload, payload_length
        ) ||
        !udp_send_datagram(
            socket_value, destination, destination_length, wire, wire_size
        )) {
        return false;
    }
    /* The sequence is committed only when the datagram actually left the
     * machine; a failed sendto must not burn a sequence number. */
    *sequence = packet_sequence + 1U;
    return true;
}

static bool connection_udp_send(
    grd_connection *connection,
    uint16_t type,
    const void *payload,
    size_t payload_length
)
{
    if (connection == NULL || connection->udp_socket == GRD_INVALID_SOCKET ||
        !connection->udp_token_valid) {
        return false;
    }
    grd_mutex_lock(&connection->udp_send_mutex);
    const bool sent = udp_encrypt_send(
        connection->udp_socket,
        NULL,
        0,
        connection->crypto.tx_key,
        &connection->udp_tx_sequence,
        connection->udp_token,
        type,
        payload,
        payload_length
    );
    grd_mutex_unlock(&connection->udp_send_mutex);
    return sent;
}

static bool udp_decrypt(
    const uint8_t *wire,
    size_t wire_size,
    const uint8_t key[GRD_SESSION_KEY_BYTES],
    grd_udp_reorder_window *window,
    const uint8_t expected_token[GRD_VIDEO_UDP_TOKEN_BYTES],
    uint16_t *type,
    uint64_t *out_sequence,
    uint8_t *plain,
    size_t plain_capacity,
    size_t *plain_size,
    bool *reordered,
    bool *duplicate
)
{
    uint32_t encrypted_size = 0U;
    uint64_t sequence = 0U;
    const uint8_t *token = NULL;
    if (!udp_decode_header(
            wire, wire_size, type, &encrypted_size, &sequence, &token
        ) || memcmp(token, expected_token, GRD_VIDEO_UDP_TOKEN_BYTES) != 0 ||
        encrypted_size - GRD_UDP_AEAD_OVERHEAD > plain_capacity) {
        return false;
    }
    bool was_reordered = false;
    if (!reorder_window_peek(window, sequence, &was_reordered)) {
        if (duplicate != NULL) {
            *duplicate = true;
        }
        return false;
    }
    uint8_t nonce[24];
    build_nonce(sequence, nonce);
    unsigned long long decrypted_size = 0U;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            plain,
            &decrypted_size,
            NULL,
            wire + GRD_UDP_HEADER_SIZE,
            encrypted_size,
            wire,
            GRD_UDP_HEADER_SIZE,
            nonce,
            key
        ) != 0) {
        /* Nothing was committed: an invalid datagram cannot advance the
         * window or age out legitimate packets. */
        return false;
    }
    reorder_window_commit(window, sequence);
    if (out_sequence != NULL) {
        *out_sequence = sequence;
    }
    if (reordered != NULL) {
        *reordered = was_reordered;
    }
    *plain_size = (size_t)decrypted_size;
    return true;
}

static bool send_secure_packet(
    grd_socket socket_value,
    grd_mutex *mutex,
    grd_crypto *crypto,
    const grd_outgoing_packet *packet
)
{
    const size_t payload_length = packet->total_length;
    if (payload_length > GRD_MAX_PACKET_SIZE - ENCRYPTION_OVERHEAD) {
        return false;
    }
    grd_mutex_lock(mutex);
    const uint64_t sequence = crypto->tx_sequence++;
    const grd_packet_header header = {
        .magic = GRD_PACKET_MAGIC,
        .version = GRD_PROTOCOL_VERSION,
        .type = (uint16_t)packet->type,
        .payload_length = (uint32_t)(payload_length + ENCRYPTION_OVERHEAD),
        .sequence = sequence
    };
    const size_t wire_header_size = 20U;
    const size_t wire_size = wire_header_size +
                             payload_length + ENCRYPTION_OVERHEAD;
    if (crypto->tx_buffer_capacity < wire_size) {
        uint8_t *resized = realloc(crypto->tx_buffer, wire_size);
        if (resized == NULL) {
            grd_mutex_unlock(mutex);
            return false;
        }
        crypto->tx_buffer = resized;
        crypto->tx_buffer_capacity = wire_size;
    }
    uint8_t *wire = crypto->tx_buffer;
    grd_protocol_encode_header(&header, wire);
    uint8_t *plain = wire + wire_header_size;
    if (packet->inline_prefix_length != 0U) {
        memcpy(
            plain,
            packet->inline_prefix,
            packet->inline_prefix_length
        );
    }
    if (packet->payload_length != 0U) {
        memcpy(
            plain + packet->inline_prefix_length,
            packet->payload,
            packet->payload_length
        );
    }
    uint8_t nonce[24];
    build_nonce(sequence, nonce);
    unsigned long long encrypted_length = 0U;
    const int result = crypto_aead_xchacha20poly1305_ietf_encrypt(
        plain,
        &encrypted_length,
        plain,
        payload_length,
        wire,
        wire_header_size,
        NULL,
        nonce,
        crypto->tx_key
    );
    const bool sent = result == 0 &&
                      send_all(
                          socket_value,
                          wire,
                          wire_header_size + (size_t)encrypted_length
                      );
    grd_mutex_unlock(mutex);
    return sent;
}

#if defined(_WIN32)
static DWORD WINAPI host_send_thread(void *argument)
#else
static void *host_send_thread(void *argument)
#endif
{
    grd_host_client *client = argument;
    grd_outgoing_packet packet;
    while (send_queue_pop(&client->send_queue, &packet)) {
        const bool sent = send_secure_packet(
            client->socket_value,
            &client->send_mutex,
            &client->crypto,
            &packet
        );
        outgoing_packet_release(&packet);
        if (!sent) {
            atomic_store_explicit(&client->running, false, memory_order_release);
            grd_socket_shutdown(client->socket_value);
            break;
        }
    }
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
static DWORD WINAPI connection_send_thread(void *argument)
#else
static void *connection_send_thread(void *argument)
#endif
{
    grd_connection *connection = argument;
    grd_outgoing_packet packet;
    while (send_queue_pop(&connection->send_queue, &packet)) {
        atomic_store_explicit(
            &connection->tcp_tx_thread_last_active,
            grd_now_micros(),
            memory_order_relaxed
        );
        const bool sent = send_secure_packet(
            connection->socket_value,
            &connection->send_mutex,
            &connection->crypto,
            &packet
        );
        outgoing_packet_release(&packet);
        if (!sent) {
            connection_set_running(connection, false);
            grd_socket_shutdown(connection->socket_value);
            break;
        }
    }
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

static bool receive_secure_packet(
    grd_socket socket_value,
    grd_crypto *crypto,
    grd_packet_header *header,
    uint8_t **payload,
    size_t *payload_length
)
{
    uint8_t wire_header[20];
    *payload = NULL;
    *payload_length = 0U;
    if (!receive_all(socket_value, wire_header, sizeof(wire_header)) ||
        grd_protocol_decode_header(wire_header, header) != GRD_OK ||
        header->payload_length < ENCRYPTION_OVERHEAD ||
        header->sequence <= crypto->rx_sequence) {
        return false;
    }
    const size_t plain_capacity =
        (size_t)header->payload_length - ENCRYPTION_OVERHEAD;
    if (crypto->rx_encrypted_capacity < header->payload_length) {
        uint8_t *resized = realloc(
            crypto->rx_encrypted, header->payload_length
        );
        if (resized == NULL) {
            return false;
        }
        crypto->rx_encrypted = resized;
        crypto->rx_encrypted_capacity = header->payload_length;
    }
    if (crypto->rx_plain_capacity < plain_capacity) {
        uint8_t *resized = realloc(crypto->rx_plain, plain_capacity);
        if (resized == NULL) {
            return false;
        }
        crypto->rx_plain = resized;
        crypto->rx_plain_capacity = plain_capacity;
    }
    uint8_t *encrypted = crypto->rx_encrypted;
    uint8_t *plain = crypto->rx_plain;
    if (!receive_all(socket_value, encrypted, header->payload_length)) {
        return false;
    }
    uint8_t nonce[24];
    build_nonce(header->sequence, nonce);
    unsigned long long plain_length = 0U;
    const int result = crypto_aead_xchacha20poly1305_ietf_decrypt(
        plain,
        &plain_length,
        NULL,
        encrypted,
        header->payload_length,
        wire_header,
        sizeof(wire_header),
        nonce,
        crypto->rx_key
    );
    if (result != 0) {
        return false;
    }
    crypto->rx_sequence = header->sequence;
    *payload = plain;
    *payload_length = (size_t)plain_length;
    return true;
}

static void udp_encode_fragment_header(
    uint8_t output[GRD_UDP_FRAGMENT_HEADER_SIZE],
    uint64_t frame_id,
    uint32_t frame_size,
    uint32_t offset,
    uint16_t fragment_index,
    uint16_t fragment_count
)
{
    udp_write_u64(output, frame_id);
    udp_write_u32(output + 8U, frame_size);
    udp_write_u32(output + 12U, offset);
    udp_write_u16(output + 16U, fragment_index);
    udp_write_u16(output + 18U, fragment_count);
}

static bool udp_decode_fragment_header(
    const uint8_t *input,
    size_t input_size,
    uint64_t *frame_id,
    uint32_t *frame_size,
    uint32_t *offset,
    uint16_t *fragment_index,
    uint16_t *fragment_count
)
{
    if (input == NULL || input_size < GRD_UDP_FRAGMENT_HEADER_SIZE) {
        return false;
    }
    *frame_id = udp_read_u64(input);
    *frame_size = udp_read_u32(input + 8U);
    *offset = udp_read_u32(input + 12U);
    *fragment_index = udp_read_u16(input + 16U);
    *fragment_count = udp_read_u16(input + 18U);
    return *frame_id != 0U && *frame_size != 0U && *fragment_count != 0U &&
           *fragment_index < *fragment_count &&
           (size_t)*offset <= *frame_size;
}

static size_t copy_media_segments(
    uint8_t *destination,
    size_t capacity,
    const uint8_t *prefix,
    size_t prefix_length,
    const uint8_t *payload,
    size_t payload_length,
    size_t offset
)
{
    size_t written = 0U;
    if (offset < prefix_length) {
        const size_t available = prefix_length - offset;
        const size_t chunk = available < capacity ? available : capacity;
        memcpy(destination, prefix + offset, chunk);
        written += chunk;
        offset += chunk;
    }
    if (written < capacity && offset >= prefix_length) {
        const size_t payload_offset = offset - prefix_length;
        const size_t available = payload_length - payload_offset;
        const size_t chunk =
            available < capacity - written ? available : capacity - written;
        memcpy(destination + written, payload + payload_offset, chunk);
        written += chunk;
    }
    return written;
}

static void pacer_unit_release(grd_pacer_unit *unit)
{
    if (unit == NULL) {
        return;
    }
    if (unit->payload_ref.release != NULL) {
        unit->payload_ref.release((void *)unit->payload_ref.opaque);
    }
    free(unit->retx_bitmap);
    memset(unit, 0, sizeof(*unit));
}

/* Assumes pacer_mutex is held. Remove every queued dependent access unit;
 * once a reference is lost, transmitting any later P-frame only turns a
 * host-side drop into kVTVideoDecoderBadDataErr on the client. */
static void pacer_purge_dependent_video_locked(
    grd_host *host,
    grd_host_client *client
)
{
    for (size_t index = 0U; index < client->pacer_count;) {
        grd_pacer_unit *queued = &client->pacer_queue[index];
        if (!queued->audio && !queued->keyframe) {
            if (queued->kind == GRD_PACER_KIND_VIDEO_ORIGINAL) {
                ++host->udp_video_frames_dropped;
                ++host->udp_drop_recovery_purge;
            }
            pacer_unit_release(queued);
            for (size_t shift = index;
                 shift + 1U < client->pacer_count; ++shift) {
                client->pacer_queue[shift] =
                    client->pacer_queue[shift + 1U];
            }
            --client->pacer_count;
            memset(
                &client->pacer_queue[client->pacer_count],
                0,
                sizeof(client->pacer_queue[0])
            );
        } else {
            ++index;
        }
    }
}

/* Dropping an encoded reference frame breaks every later P-frame that may
 * refer to it. Ask the stream thread for an IDR immediately instead of
 * waiting for the client decoder to discover the broken chain. */
static bool pacer_request_recovery_idr(
    grd_host *host,
    grd_host_client *client,
    const grd_pacer_unit *unit
)
{
    if (host == NULL || unit == NULL ||
        unit->kind != GRD_PACER_KIND_VIDEO_ORIGINAL) {
        return false;
    }
    bool already_broken = false;
    bool recovery_was_queued = false;
    if (client != NULL) {
        recovery_was_queued = atomic_load_explicit(
            &client->recovery_keyframe_queued, memory_order_acquire
        );
        already_broken = atomic_exchange_explicit(
            &client->video_discontinuity, true, memory_order_acq_rel
        );
    }
    const bool request = grd_stream_drop_requests_recovery(
        unit->keyframe, already_broken, recovery_was_queued
    );
    if (unit->keyframe && recovery_was_queued && client != NULL) {
        /* The repair itself failed. It no longer protects a following chain,
         * so allow exactly one replacement IDR to become the active repair. */
        atomic_store_explicit(
            &client->recovery_keyframe_queued, false, memory_order_release
        );
    }
    if (!request) {
        return false;
    }
    atomic_store_explicit(
        &host->keyframe_pending, true, memory_order_release
    );
    return true;
}

static void pacer_update_queue_peak(grd_host *host, size_t count)
{
    uint64_t current = atomic_load_explicit(
        &host->udp_pacer_queue_peak, memory_order_relaxed
    );
    while ((uint64_t)count > current &&
           !atomic_compare_exchange_weak_explicit(
               &host->udp_pacer_queue_peak,
               &current,
               (uint64_t)count,
               memory_order_relaxed,
               memory_order_relaxed
           )) {
    }
}

static void grd_plain_free(void *opaque)
{
    free(opaque);
}

static uint64_t pacer_pacing_interval_us(grd_host *host)
{
    const uint32_t bits_per_second = atomic_load_explicit(
        &host->pacing_bits_per_second, memory_order_acquire
    );
    if (bits_per_second == 0U) {
        return 400U;
    }
    /* Full 1200-byte datagram on the wire: pacing the fragment stream at the
     * bitrate flattens the per-frame burst to the link capacity. */
    /* pacing_bits_per_second is the WIRE budget (1200-byte datagrams); the
     * encoder target is derived from it in the host stream thread with the
     * payload/wire ratio applied, so the two never collide. */
    const uint64_t bits = (uint64_t)GRD_UDP_MAX_DATAGRAM * 8ULL;
    uint64_t interval_us = bits * 1000000ULL / bits_per_second;
    if (interval_us < 100U) {
        interval_us = 100U;
    }
    if (interval_us > 2000U) {
        interval_us = 2000U;
    }
    return interval_us;
}

static size_t pacer_queued_video_fragments(
    const grd_host_client *client
)
{
    size_t fragments = 0U;
    for (size_t index = 0U; index < client->pacer_count; ++index) {
        const grd_pacer_unit *queued = &client->pacer_queue[index];
        if (queued->audio) {
            /* FEC parity is a paced unit: it occupies one wire slot. */
            if (queued->audio_type == GRD_PACKET_VIDEO_FEC) {
                ++fragments;
            }
        } else {
            fragments += queued->fragment_count;
        }
    }
    return fragments;
}

static bool pacer_enqueue_locked(
    grd_host *host,
    grd_host_client *client,
    grd_pacer_unit *unit
)
{
    if (client->pacer_stopping) {
        return false;
    }
    if (unit->kind == GRD_PACER_KIND_VIDEO_ORIGINAL && !unit->keyframe &&
        atomic_load_explicit(
            &client->video_discontinuity, memory_order_acquire
        )) {
        /* Keep the episode atomic. Even when its repair IDR is queued, a
         * following P-frame may miss admission before that IDR completes and
         * would open a second broken chain. Gate all dependants until the IDR
         * has been sent in full, then resume from a clean reference. */
        ++host->udp_video_frames_dropped;
        ++host->udp_drop_discontinuity;
        return false;
    }
    bool recovery_keyframe = false;
    if (unit->keyframe && !unit->audio) {
        recovery_keyframe = grd_stream_keyframe_preempts_queue(
            true,
            atomic_load_explicit(
                &client->video_discontinuity, memory_order_acquire
            ),
            atomic_load_explicit(
                &client->recovery_keyframe_queued,
                memory_order_acquire
            )
        );
        if (recovery_keyframe) {
            /* Only a repair IDR makes queued dependants unusable. */
            pacer_purge_dependent_video_locked(host, client);
            unit->priority = GRD_PACER_PRIO_KEYFRAME;
        } else {
            /* A periodic/safety IDR is an ordinary frame in the valid
             * reference order. Letting it jump ahead used to purge a burst
             * every ten seconds even on a lossless LAN. */
            unit->priority = GRD_PACER_PRIO_VIDEO;
        }
    }
    if (unit->kind == GRD_PACER_KIND_VIDEO_ORIGINAL ||
        unit->kind == GRD_PACER_KIND_VIDEO_RETX) {
        /* Preventive admission control: a frame that cannot be fully sent
         * inside its window is dropped entirely instead of wasting the link
         * with a partial transmission (a partial IDR is unusable and
         * triggers another recovery). P-frames get one frame period, IDRs
         * three, retransmissions the RTT-aware deadline computed by the
         * NACK handler: each unit carries its own deadline. */
        const uint64_t interval = pacer_pacing_interval_us(host);
        const uint64_t send_time =
            (uint64_t)unit->fragment_count * interval;
        const uint64_t queue_delay =
            pacer_queued_video_fragments(client) * interval;
        const uint64_t now_us = grd_now_micros();
        const uint64_t budget = unit->deadline_micros > now_us
                                    ? unit->deadline_micros - now_us
                                    : 0U;
        if (budget != 0U && queue_delay + send_time > budget) {
            ++host->udp_video_frames_dropped;
            ++host->udp_video_congestion_dropped;
            ++host->udp_drop_admission;
            pacer_request_recovery_idr(host, client, unit);
            if (unit->kind == GRD_PACER_KIND_VIDEO_ORIGINAL &&
                unit->keyframe) {
                ++host->udp_keyframes_dropped;
            }
            return false;
        }
    }
    if (client->pacer_count == GRD_PACER_QUEUE_CAPACITY) {
        /* Drop the oldest non-keyframe video frame to make room. */
        size_t drop_index = GRD_PACER_QUEUE_CAPACITY;
        for (size_t index = 0U; index < client->pacer_count; ++index) {
            grd_pacer_unit *queued = &client->pacer_queue[index];
            if (queued->priority == GRD_PACER_PRIO_VIDEO &&
                !queued->keyframe) {
                drop_index = index;
                break;
            }
        }
        if (drop_index == GRD_PACER_QUEUE_CAPACITY) {
            if (unit->kind == GRD_PACER_KIND_VIDEO_ORIGINAL) {
                ++host->udp_video_frames_dropped;
                ++host->udp_video_congestion_dropped;
                ++host->udp_drop_queue;
                pacer_request_recovery_idr(host, client, unit);
            }
            return false;
        }
        ++host->udp_video_frames_dropped;
        ++host->udp_video_congestion_dropped;
        ++host->udp_drop_queue;
        const bool opened_recovery = pacer_request_recovery_idr(
            host, client, &client->pacer_queue[drop_index]
        );
        pacer_unit_release(&client->pacer_queue[drop_index]);
        for (size_t index = drop_index; index + 1U < client->pacer_count;
             ++index) {
            client->pacer_queue[index] = client->pacer_queue[index + 1U];
        }
        --client->pacer_count;
        memset(
            &client->pacer_queue[client->pacer_count],
            0,
            sizeof(client->pacer_queue[0])
        );
        /* Every P-frame after the evicted reference is unusable. Purge the
         * remainder immediately; otherwise the pacer can transmit it before
         * the newly requested recovery IDR reaches the queue. */
        if (opened_recovery) {
            pacer_purge_dependent_video_locked(host, client);
        }
        if (!unit->keyframe) {
            ++host->udp_video_frames_dropped;
            ++host->udp_drop_discontinuity;
            return false;
        }
    }
    const bool queues_recovery_idr =
        unit->keyframe && !unit->audio &&
        grd_stream_keyframe_preempts_queue(
            true,
            atomic_load_explicit(
                &client->video_discontinuity, memory_order_acquire
            ),
            atomic_load_explicit(
                &client->recovery_keyframe_queued,
                memory_order_acquire
            )
        );
    if (queues_recovery_idr && !recovery_keyframe) {
        /* The queue-full path can create the discontinuity while admitting
         * this keyframe. Promote it to the repair IDR and remove dependants
         * produced after the evicted reference. */
        pacer_purge_dependent_video_locked(host, client);
        unit->priority = GRD_PACER_PRIO_KEYFRAME;
    }
    client->pacer_queue[client->pacer_count++] = *unit;
    if (queues_recovery_idr) {
        ++host->udp_recovery_keyframes_queued;
        atomic_store_explicit(
            &client->recovery_keyframe_queued,
            true,
            memory_order_release
        );
    } else if (unit->keyframe && !unit->audio) {
        ++host->udp_normal_keyframes_queued;
    }
    pacer_update_queue_peak(host, client->pacer_count);
    memset(unit, 0, sizeof(*unit));
    grd_cond_signal(&client->pacer_condition);
    return true;
}

static bool pacer_pop_locked(
    grd_host_client *client,
    grd_pacer_unit *out
)
{
    if (client->pacer_count == 0U) {
        return false;
    }
    size_t selected = 0U;
    int selected_priority = (int)client->pacer_queue[0].priority;
    for (size_t index = 1U; index < client->pacer_count; ++index) {
        const int priority = (int)client->pacer_queue[index].priority;
        if (priority < selected_priority) {
            selected = index;
            selected_priority = priority;
        }
    }
    *out = client->pacer_queue[selected];
    for (size_t index = selected; index + 1U < client->pacer_count;
         ++index) {
        client->pacer_queue[index] = client->pacer_queue[index + 1U];
    }
    --client->pacer_count;
    memset(
        &client->pacer_queue[client->pacer_count],
        0,
        sizeof(client->pacer_queue[0])
    );
    return true;
}

static void pacer_retx_ring_store(
    grd_host_client *client,
    const grd_pacer_unit *frame
)
{
    const size_t slot = client->retx_ring_next % GRD_PACER_RETX_FRAMES;
    pacer_unit_release(&client->retx_ring[slot]);
    client->retx_ring[slot] = *frame;
    memset(&client->retx_ring[slot].retx_bitmap, 0,
           sizeof(client->retx_ring[slot].retx_bitmap));
    client->retx_ring_ids[slot] = frame->frame_id;
    ++client->retx_ring_next;
}

static grd_pacer_unit *pacer_retx_ring_find(
    grd_host_client *client,
    uint64_t frame_id
)
{
    const size_t count = client->retx_ring_next < GRD_PACER_RETX_FRAMES
                             ? client->retx_ring_next
                             : GRD_PACER_RETX_FRAMES;
    for (size_t index = 0U; index < count; ++index) {
        if (client->retx_ring_ids[index] == frame_id &&
            client->retx_ring[index].payload_ref.release != NULL) {
            return &client->retx_ring[index];
        }
    }
    return NULL;
}

/* Copy the UDP peer address under the pacer mutex: the receive thread
 * updates it there, so the pacer never reads a torn sockaddr while the
 * peer roams or reconnects. */
static void client_socket_state_copy(
    grd_host_client *client,
    struct sockaddr_storage *address,
    grd_socklen *address_length
)
{
    grd_mutex_lock(&client->pacer_mutex);
    *address = client->udp_address;
    *address_length = client->udp_address_length;
    grd_mutex_unlock(&client->pacer_mutex);
}

/* Pacer-side video sender: called only by the pacer thread. Fragments the
 * frame with sub-millisecond pacing and a hard deadline. */
static grd_status host_send_udp_video(
    grd_host *host,
    grd_host_client *client,
    grd_pacer_unit *unit
)
{
    if (host == NULL || client == NULL || unit == NULL ||
        host->udp_socket == GRD_INVALID_SOCKET ||
        !atomic_load_explicit(&client->udp_ready, memory_order_acquire)) {
        return GRD_INVALID_ARGUMENT;
    }
    struct sockaddr_storage peer_address;
    grd_socklen peer_address_length = 0U;
    client_socket_state_copy(client, &peer_address, &peer_address_length);
    const uint64_t total_length =
        (uint64_t)unit->prefix_length + unit->payload_length;
    const uint64_t interval = pacer_pacing_interval_us(host);
    const uint64_t now = grd_now_micros();
    uint64_t pacing_at = grd_stream_pacer_schedule_start(
        client->pacer_next_send_micros, now, unit->keyframe
    );
    /* Queue priorities can consume the budget after the enqueue-time check.
     * Re-evaluate immediately before the first datagram and, if necessary,
     * drop the whole access unit. The old per-fragment deadline check could
     * abandon a frame halfway through, producing client-side "incomplete"
     * frames even when the network reported zero packet loss. */
    uint16_t scheduled_fragments = 0U;
    for (uint16_t index = 0U; index < unit->fragment_count; ++index) {
        if (unit->retx_bitmap == NULL ||
            (index / 8U < unit->retx_bitmap_size &&
             (unit->retx_bitmap[index / 8U] &
              (uint8_t)(1U << (index % 8U))) != 0U)) {
            ++scheduled_fragments;
        }
    }
    const uint64_t projected_finish =
        pacing_at + (uint64_t)scheduled_fragments * interval;
    if (unit->deadline_micros != 0U &&
        projected_finish > unit->deadline_micros) {
        ++host->udp_video_frames_dropped;
        ++host->udp_video_congestion_dropped;
        ++host->udp_drop_deadline;
        return GRD_WOULD_BLOCK;
    }
    /* Once transmission starts, complete the frame. This small extra window
     * is only for a transient EWOULDBLOCK; normal pacing still finishes by
     * projected_finish. */
    const uint64_t completion_deadline =
        projected_finish + GRD_UDP_SEND_BUDGET_US;
    uint8_t fragment[GRD_UDP_MAX_DATAGRAM];
    for (uint16_t index = 0U; index < unit->fragment_count; ++index) {
        const size_t offset = (size_t)index * GRD_UDP_FRAGMENT_CAPACITY;
        if (offset >= total_length) {
            break;
        }
        const size_t remaining = total_length - offset;
        const size_t chunk = remaining < GRD_UDP_FRAGMENT_CAPACITY
                                 ? remaining
                                 : GRD_UDP_FRAGMENT_CAPACITY;
        if (unit->retx_bitmap != NULL &&
            index / 8U < unit->retx_bitmap_size &&
            (unit->retx_bitmap[index / 8U] &
             (uint8_t)(1U << (index % 8U))) == 0U) {
            continue;
        }
        while (pacing_at > grd_now_micros()) {
            const uint64_t wait = pacing_at - grd_now_micros();
            if (wait > 1000U) {
                grd_sleep_micros(1000U);
            } else {
                grd_sleep_micros((unsigned)wait);
                break;
            }
        }
        udp_encode_fragment_header(
            fragment,
            unit->frame_id,
            (uint32_t)total_length,
            (uint32_t)offset,
            index,
            unit->fragment_count
        );
        const size_t copied = copy_media_segments(
            fragment + GRD_UDP_FRAGMENT_HEADER_SIZE,
            chunk,
            unit->prefix,
            unit->prefix_length,
            unit->payload,
            unit->payload_length,
            offset
        );
        uint8_t wire[GRD_UDP_MAX_DATAGRAM];
        size_t wire_size = 0U;
        const uint64_t fragment_sequence = client->udp_tx_sequence;
        if (copied != chunk ||
            !udp_prepare_datagram(
                wire,
                &wire_size,
                client->crypto.tx_key,
                fragment_sequence,
                client->udp_token,
                GRD_PACKET_VIDEO_FRAME,
                fragment,
                GRD_UDP_FRAGMENT_HEADER_SIZE + chunk
            )) {
            ++host->udp_video_frames_dropped;
            ++host->udp_video_congestion_dropped;
            ++host->udp_drop_send;
            return GRD_BUSY;
        }
        bool fragment_sent = false;
        for (;;) {
            if (udp_send_datagram(
                    host->udp_socket,
                    (const struct sockaddr *)&peer_address,
                    peer_address_length,
                    wire,
                    wire_size
                )) {
                fragment_sent = true;
                break;
            }
            if (!grd_socket_would_block() ||
                grd_now_micros() >= completion_deadline) {
                break;
            }
            grd_sleep_micros(500U);
        }
        if (!fragment_sent) {
            ++host->udp_video_frames_dropped;
            ++host->udp_video_congestion_dropped;
            ++host->udp_drop_send;
            return GRD_WOULD_BLOCK;
        }
        client->udp_tx_sequence = fragment_sequence + 1U;
        if (unit->kind == GRD_PACER_KIND_VIDEO_RETX) {
            ++host->udp_retx_fragments_sent;
        } else {
            ++host->udp_video_fragments_sent;
        }
        pacing_at += interval;
    }
    /* XOR FEC: one parity fragment per block of 16 data fragments. */
    if (unit->kind == GRD_PACER_KIND_VIDEO_ORIGINAL &&
        atomic_load_explicit(&host->fec_enabled, memory_order_acquire)) {
        const size_t fec_blocks =
            ((size_t)unit->fragment_count + GRD_UDP_FEC_BLOCK_FRAGMENTS - 1U) /
            GRD_UDP_FEC_BLOCK_FRAGMENTS;
        if (fec_blocks != 0U && fec_blocks <= GRD_UDP_FEC_MAX_BLOCKS) {
            uint8_t parity[GRD_UDP_FRAGMENT_CAPACITY];
            for (size_t block = 0U; block < fec_blocks; ++block) {
                memset(parity, 0, sizeof(parity));
                const size_t block_start =
                    block * GRD_UDP_FEC_BLOCK_FRAGMENTS;
                const size_t block_end = block_start +
                                         GRD_UDP_FEC_BLOCK_FRAGMENTS;
                for (size_t index = block_start;
                     index < block_end &&
                     index < (size_t)unit->fragment_count;
                     ++index) {
                    const size_t offset =
                        index * GRD_UDP_FRAGMENT_CAPACITY;
                    size_t length = total_length - offset;
                    if (length > GRD_UDP_FRAGMENT_CAPACITY) {
                        length = GRD_UDP_FRAGMENT_CAPACITY;
                    }
                    uint8_t fragment_data[GRD_UDP_FRAGMENT_CAPACITY];
                    const size_t copied = copy_media_segments(
                        fragment_data,
                        length,
                        unit->prefix,
                        unit->prefix_length,
                        unit->payload,
                        unit->payload_length,
                        offset
                    );
                    if (copied != length) {
                        break;
                    }
                    for (size_t byte_index = 0U;
                         byte_index < GRD_UDP_FRAGMENT_CAPACITY;
                         ++byte_index) {
                        parity[byte_index] ^=
                            byte_index < length
                                ? fragment_data[byte_index]
                                : 0U;
                    }
                }
                uint8_t fec_payload[GRD_UDP_FRAGMENT_HEADER_SIZE +
                                    GRD_UDP_FRAGMENT_CAPACITY];
                udp_encode_fragment_header(
                    fec_payload,
                    unit->frame_id,
                    (uint32_t)total_length,
                    (uint32_t)block,
                    (uint16_t)block,
                    unit->fragment_count
                );
                memcpy(
                    fec_payload + GRD_UDP_FRAGMENT_HEADER_SIZE,
                    parity,
                    GRD_UDP_FRAGMENT_CAPACITY
                );
                /* Parity is a normal pacer unit (FEC priority, paced, with
                 * the frame deadline): it never bursts right after the frame
                 * and is bounded by the same wire budget. */
                grd_pacer_unit fec_unit;
                memset(&fec_unit, 0, sizeof(fec_unit));
                fec_unit.kind = GRD_PACER_KIND_FEC;
                fec_unit.priority = GRD_PACER_PRIO_FEC;
                fec_unit.frame_id = unit->frame_id;
                fec_unit.deadline_micros = unit->deadline_micros;
                fec_unit.audio = true;
                fec_unit.audio_type = GRD_PACKET_VIDEO_FEC;
                fec_unit.audio_size = sizeof(fec_payload);
                memcpy(fec_unit.audio_data, fec_payload, sizeof(fec_payload));
                grd_mutex_lock(&client->pacer_mutex);
                const bool fec_accepted = pacer_enqueue_locked(
                    host, client, &fec_unit
                );
                grd_mutex_unlock(&client->pacer_mutex);
                if (!fec_accepted) {
                    ++host->udp_fec_fragments_dropped;
                }
                pacer_unit_release(&fec_unit);
            }
        }
    }
    if (unit->kind == GRD_PACER_KIND_VIDEO_ORIGINAL) {
        ++host->udp_video_frames_sent;
    }
    client->pacer_next_send_micros = pacing_at;
    return GRD_OK;
}

/* Pacer-side single-datagram sender (audio/control immediate, FEC parity
 * paced like video fragments so it never bursts after a frame). */
static grd_status host_send_udp_audio(
    grd_host *host,
    grd_host_client *client,
    grd_pacer_unit *unit
)
{
    if (host == NULL || client == NULL || unit == NULL ||
        host->udp_socket == GRD_INVALID_SOCKET ||
        !atomic_load_explicit(&client->udp_ready, memory_order_acquire)) {
        return GRD_BUSY;
    }
    struct sockaddr_storage peer_address;
    grd_socklen peer_address_length = 0U;
    client_socket_state_copy(client, &peer_address, &peer_address_length);
    if (unit->audio_type == GRD_PACKET_VIDEO_FEC) {
        /* FEC parity is a normal paced unit: wait for the next wire slot.
         * FEC is activated exactly when the network is lossy, so an
         * un-paced burst would add congestion precisely where it hurts. */
        const uint64_t interval = pacer_pacing_interval_us(host);
        uint64_t pacing_at = client->pacer_next_send_micros;
        const uint64_t send_started = grd_now_micros();
        if (pacing_at == 0U || pacing_at < send_started) {
            pacing_at = send_started;
        }
        while (pacing_at > grd_now_micros()) {
            const uint64_t wait = pacing_at - grd_now_micros();
            if (wait > 1000U) {
                grd_sleep_micros(1000U);
            } else {
                grd_sleep_micros((unsigned)wait);
                break;
            }
        }
        if (grd_now_micros() > unit->deadline_micros) {
            ++host->udp_fec_fragments_dropped;
            return GRD_WOULD_BLOCK;
        }
        uint8_t wire[GRD_UDP_MAX_DATAGRAM];
        size_t wire_size = 0U;
        const uint64_t fec_sequence = client->udp_tx_sequence;
        if (!udp_prepare_datagram(
                wire,
                &wire_size,
                client->crypto.tx_key,
                fec_sequence,
                client->udp_token,
                GRD_PACKET_VIDEO_FEC,
                unit->audio_data,
                unit->audio_size
            ) ||
            !udp_send_datagram(
                host->udp_socket,
                (const struct sockaddr *)&peer_address,
                peer_address_length,
                wire,
                wire_size
            )) {
            return GRD_BUSY;
        }
        client->udp_tx_sequence = fec_sequence + 1U;
        client->pacer_next_send_micros = pacing_at + interval;
        ++host->udp_video_fec_fragments_sent;
        return GRD_OK;
    }
    uint8_t wire[GRD_UDP_MAX_DATAGRAM];
    size_t wire_size = 0U;
    const uint64_t datagram_sequence = client->udp_tx_sequence;
    if (!udp_prepare_datagram(
            wire,
            &wire_size,
            client->crypto.tx_key,
            datagram_sequence,
            client->udp_token,
            unit->audio_type,
            unit->audio_data,
            unit->audio_size
        ) ||
        !udp_send_datagram(
            host->udp_socket,
            (const struct sockaddr *)&peer_address,
            peer_address_length,
            wire,
            wire_size
        )) {
        return GRD_BUSY;
    }
    client->udp_tx_sequence = datagram_sequence + 1U;
    if (unit->audio_type == GRD_PACKET_AUDIO_FRAME) {
        ++host->udp_audio_datagrams_sent;
    }
    return GRD_OK;
}

static grd_status pacer_enqueue_video(
    grd_host *host,
    grd_host_client *client,
    const uint8_t *prefix,
    size_t prefix_length,
    const uint8_t *payload,
    size_t payload_length,
    const grd_owned_buffer *payload_ref,
    bool keyframe
)
{
    if (client == NULL || payload == NULL || payload_length == 0U ||
        prefix_length > GRD_BROADCAST_MAX_INLINE_PREFIX) {
        return GRD_INVALID_ARGUMENT;
    }
    const size_t total_length = prefix_length + payload_length;
    const size_t fragment_count_size =
        (total_length + GRD_UDP_FRAGMENT_CAPACITY - 1U) /
        GRD_UDP_FRAGMENT_CAPACITY;
    if (fragment_count_size == 0U || fragment_count_size > UINT16_MAX) {
        return GRD_BUSY;
    }
    const uint64_t frame_id = ++client->udp_tx_frame_id;
    void *clone = NULL;
    const uint8_t *unit_payload = payload;
    if (payload_ref != NULL && payload_ref->clone != NULL) {
        clone = payload_ref->clone(payload_ref->opaque);
    } else if (payload_ref != NULL && payload_ref->release != NULL) {
        clone = (void *)payload_ref->opaque;
    } else {
        /* No refcounted reference: copy the payload for the pacer. */
        uint8_t *copy = malloc(payload_length);
        if (copy != NULL) {
            memcpy(copy, payload, payload_length);
            unit_payload = copy;
        }
        clone = copy;
    }
    if (clone == NULL) {
        return GRD_OUT_OF_MEMORY;
    }
    grd_pacer_unit unit;
    memset(&unit, 0, sizeof(unit));
    unit.kind = GRD_PACER_KIND_VIDEO_ORIGINAL;
    unit.priority = keyframe ? GRD_PACER_PRIO_KEYFRAME
                             : GRD_PACER_PRIO_VIDEO;
    unit.frame_id = frame_id;
    unit.fragment_count = (uint16_t)fragment_count_size;
    unit.frame_size = (uint32_t)total_length;
    const uint64_t frame_period = atomic_load_explicit(
        &host->frame_period_us, memory_order_relaxed
    );
    /* The pacing itself bounds the long-run rate; these deadlines only let
     * a short scene-change burst drain without dropping a reference frame.
     * An IDR can legitimately exceed four nominal frame budgets. Rejecting
     * that IDR used to enter a recovery loop: every following P-frame was
     * gated, another large IDR was requested, and video vanished while audio
     * remained live. Give a keyframe enough time for its own paced fragments
     * plus one frame of scheduling slack, capped at 250 ms so a persistently
     * overloaded client still cannot build an unbounded queue. */
    const uint64_t deadline_started = grd_now_micros();
    uint64_t deadline_budget =
        frame_period * (keyframe ? GRD_KEYFRAME_DEADLINE_PERIODS
                                 : GRD_PFRAME_DEADLINE_PERIODS);
    const uint64_t paced_send_time =
        (uint64_t)unit.fragment_count * pacer_pacing_interval_us(host);
    if (keyframe) {
        uint64_t keyframe_budget = paced_send_time + frame_period;
        if (keyframe_budget > 250000ULL) {
            keyframe_budget = 250000ULL;
        }
        if (deadline_budget < keyframe_budget) {
            deadline_budget = keyframe_budget;
        }
    } else {
        /* At 120 FPS, one complex foliage/pan frame can be larger than the
         * fixed three-period (25 ms) admission window even though the
         * average stream is well below the wire budget. Dropping that single
         * P-frame invalidates every dependant and starts an expensive IDR
         * recovery loop. Permit a bounded four-frame burst to drain; the
         * queue-delay check still drops persistent overload before latency
         * can exceed 75 ms. */
        uint64_t pframe_budget = paced_send_time + frame_period * 4ULL;
        if (pframe_budget > GRD_PFRAME_MAX_DEADLINE_US) {
            pframe_budget = GRD_PFRAME_MAX_DEADLINE_US;
        }
        if (deadline_budget < pframe_budget) {
            deadline_budget = pframe_budget;
        }
    }
    unit.deadline_micros = deadline_started + deadline_budget;
    unit.keyframe = keyframe;
    memcpy(unit.prefix, prefix, prefix_length);
    unit.prefix_length = prefix_length;
    unit.payload = unit_payload;
    unit.payload_length = payload_length;
    unit.payload_ref.opaque = clone;
    if (payload_ref != NULL) {
        unit.payload_ref.clone = payload_ref->clone;
        unit.payload_ref.release = payload_ref->release;
    } else {
        unit.payload_ref.clone = NULL;
        unit.payload_ref.release = grd_plain_free;
    }
    grd_mutex_lock(&client->pacer_mutex);
    const bool accepted = pacer_enqueue_locked(host, client, &unit);
    grd_mutex_unlock(&client->pacer_mutex);
    if (!accepted) {
        pacer_unit_release(&unit);
        return GRD_BUSY;
    }
    /* video_discontinuity remains set until every repair-IDR fragment is
     * sent. P-frames are gated in the meantime, so one drop episode can cause
     * only one purge and one active recovery chain. */
    return GRD_OK;
}

static bool pacer_enqueue_datagram(
    grd_host *host,
    grd_host_client *client,
    uint16_t type,
    const uint8_t *data,
    size_t data_length
)
{
    const size_t datagram_capacity =
        GRD_UDP_MAX_DATAGRAM - GRD_UDP_HEADER_SIZE - GRD_UDP_AEAD_OVERHEAD;
    if (client == NULL || data_length > datagram_capacity ||
        (data_length != 0U && data == NULL)) {
        return false;
    }
    grd_pacer_unit unit;
    memset(&unit, 0, sizeof(unit));
    unit.kind = type == GRD_PACKET_AUDIO_FRAME
                    ? GRD_PACER_KIND_AUDIO
                    : GRD_PACER_KIND_CONTROL;
    unit.priority = type == GRD_PACKET_AUDIO_FRAME
                        ? GRD_PACER_PRIO_AUDIO
                        : GRD_PACER_PRIO_CONTROL;
    unit.audio = true;
    unit.audio_type = type;
    unit.audio_size = data_length;
    if (data_length != 0U) {
        memcpy(unit.audio_data, data, data_length);
    }
    grd_mutex_lock(&client->pacer_mutex);
    const bool accepted = pacer_enqueue_locked(host, client, &unit);
    grd_mutex_unlock(&client->pacer_mutex);
    if (!accepted) {
        pacer_unit_release(&unit);
    }
    return accepted;
}

#if defined(_WIN32)
static DWORD WINAPI pacer_thread_fn(void *argument)
#else
static void *pacer_thread_fn(void *argument)
#endif
{
    grd_host_client *client = argument;
    grd_host *host = client->host;
    for (;;) {
        grd_pacer_unit unit;
        grd_mutex_lock(&client->pacer_mutex);
        while (client->pacer_count == 0U && !client->pacer_stopping) {
            grd_cond_wait(
                &client->pacer_condition, &client->pacer_mutex
            );
        }
        if (client->pacer_stopping && client->pacer_count == 0U) {
            grd_mutex_unlock(&client->pacer_mutex);
            break;
        }
        const bool got = pacer_pop_locked(client, &unit);
        grd_mutex_unlock(&client->pacer_mutex);
        if (!got) {
            continue;
        }
        grd_status status = unit.kind == GRD_PACER_KIND_VIDEO_ORIGINAL ||
                                    unit.kind == GRD_PACER_KIND_VIDEO_RETX
                                ? host_send_udp_video(host, client, &unit)
                                : host_send_udp_audio(host, client, &unit);
        if (unit.kind == GRD_PACER_KIND_VIDEO_ORIGINAL &&
            status == GRD_OK) {
            if (unit.keyframe) {
                /* Clear the discontinuity only if this is still the active
                 * recovery IDR. A failed repair clears the queued flag and
                 * requests a replacement, which an older completion must not
                 * accidentally cancel. */
                const bool completed_recovery = atomic_exchange_explicit(
                    &client->recovery_keyframe_queued,
                    false,
                    memory_order_acq_rel
                );
                if (completed_recovery) {
                    atomic_store_explicit(
                        &client->video_discontinuity,
                        false,
                        memory_order_release
                    );
                }
            }
            /* Only original frames enter the NACK cache: retransmissions
             * must never re-cache themselves or evict a useful original. */
            grd_mutex_lock(&client->pacer_mutex);
            pacer_retx_ring_store(client, &unit);
            grd_mutex_unlock(&client->pacer_mutex);
            memset(&unit, 0, sizeof(unit));
        } else if (unit.kind == GRD_PACER_KIND_VIDEO_ORIGINAL) {
            const bool opened_recovery =
                pacer_request_recovery_idr(host, client, &unit);
            if (opened_recovery) {
                grd_mutex_lock(&client->pacer_mutex);
                pacer_purge_dependent_video_locked(host, client);
                grd_mutex_unlock(&client->pacer_mutex);
            }
        }
        if (unit.payload_ref.release != NULL) {
            pacer_unit_release(&unit);
        } else {
            free(unit.retx_bitmap);
        }
        if (status == GRD_BUSY || status == GRD_IO_ERROR) {
            atomic_store_explicit(&client->running, false, memory_order_release);
            grd_socket_shutdown(client->socket_value);
            break;
        }
    }
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

/* Stop and drain a client's pacer before its slot can be reused. Previously
 * disconnected clients left this thread waiting on the old condition
 * variable; reconnecting reinitialized the same memory and could leave two
 * pacers racing over one queue, producing short unexplained frame gaps. */
static void client_pacer_stop(grd_host_client *client)
{
    if (client == NULL) {
        return;
    }
    grd_mutex_lock(&client->pacer_mutex);
    for (size_t index = 0U; index < client->pacer_count; ++index) {
        pacer_unit_release(&client->pacer_queue[index]);
    }
    client->pacer_count = 0U;
    client->pacer_stopping = true;
    grd_cond_broadcast(&client->pacer_condition);
    grd_mutex_unlock(&client->pacer_mutex);
    if (client->pacer_thread_started) {
        grd_thread_join(client->pacer_thread);
        client->pacer_thread_started = false;
    }
    for (size_t index = 0U; index < GRD_PACER_RETX_FRAMES; ++index) {
        pacer_unit_release(&client->retx_ring[index]);
        client->retx_ring_ids[index] = 0U;
    }
    client->retx_ring_next = 0U;
    atomic_store_explicit(
        &client->video_discontinuity, false, memory_order_release
    );
    atomic_store_explicit(
        &client->recovery_keyframe_queued, false, memory_order_release
    );
}

void grd_host_set_stream_params(
    grd_host *host,
    uint32_t bits_per_second,
    uint32_t frame_period_us
)
{
    if (host == NULL) {
        return;
    }
    if (bits_per_second != 0U) {
        atomic_store_explicit(
            &host->pacing_bits_per_second,
            bits_per_second,
            memory_order_release
        );
    }
    if (frame_period_us != 0U) {
        atomic_store_explicit(
            &host->frame_period_us, frame_period_us, memory_order_release
        );
    }
}

void grd_host_set_fec_enabled(grd_host *host, bool enabled)
{
    if (host == NULL) {
        return;
    }
    atomic_store_explicit(
        &host->fec_enabled, enabled, memory_order_release
    );
}

void grd_host_set_rtt_us(grd_host *host, uint32_t rtt_micros)
{
    if (host == NULL) {
        return;
    }
    atomic_store_explicit(
        &host->rtt_micros, rtt_micros, memory_order_release
    );
}

uint32_t grd_host_pacing_bits_per_second(const grd_host *host)
{
    if (host == NULL) {
        return 0U;
    }
    return atomic_load_explicit(
        &host->pacing_bits_per_second, memory_order_acquire
    );
}

static void host_log_udp_stats(grd_host *host)
{
    const uint64_t now = grd_now_micros();
    /* 1 s rolling drop rate: short enough for the ABR to distinguish
     * self-inflicted pacer drops from the loss the client reports. */
    if (host->udp_stats_drop_window_start == 0U) {
        host->udp_stats_drop_window_start = now;
        host->udp_stats_drop_frames_at = host->udp_video_frames_sent;
        host->udp_stats_drop_admission_at = host->udp_drop_admission;
        host->udp_stats_drop_queue_at = host->udp_drop_queue;
        host->udp_stats_drop_deadline_at = host->udp_drop_deadline;
        host->udp_stats_drop_send_at = host->udp_drop_send;
    } else if (now - host->udp_stats_drop_window_start >= 1000000ULL) {
        const grd_stream_drop_counts counts = {
            .sent = host->udp_video_frames_sent -
                    host->udp_stats_drop_frames_at,
            .admission = host->udp_drop_admission -
                         host->udp_stats_drop_admission_at,
            .queue = host->udp_drop_queue -
                     host->udp_stats_drop_queue_at,
            .deadline = host->udp_drop_deadline -
                        host->udp_stats_drop_deadline_at,
            .send = host->udp_drop_send - host->udp_stats_drop_send_at,
            .discontinuity = 0U,
            .recovery_purge = 0U
        };
        /* Odd/even sequence lock: readers never pair a new percentage with
         * the preceding sample id (or vice versa). */
        (void)atomic_fetch_add_explicit(
            &host->udp_recent_initiating_drop_generation,
            1U,
            memory_order_acq_rel
        );
        atomic_store_explicit(
            &host->udp_recent_initiating_drop_percent,
            grd_stream_initiating_drop_percent(&counts),
            memory_order_relaxed
        );
        (void)atomic_fetch_add_explicit(
            &host->udp_recent_initiating_drop_generation,
            1U,
            memory_order_release
        );
        host->udp_stats_drop_window_start = now;
        host->udp_stats_drop_frames_at = host->udp_video_frames_sent;
        host->udp_stats_drop_admission_at = host->udp_drop_admission;
        host->udp_stats_drop_queue_at = host->udp_drop_queue;
        host->udp_stats_drop_deadline_at = host->udp_drop_deadline;
        host->udp_stats_drop_send_at = host->udp_drop_send;
    }
    if (host->udp_stats_log_window_start == 0U) {
        host->udp_stats_log_window_start = now;
        host->udp_stats_log_frames_at = host->udp_video_frames_sent;
        host->udp_stats_log_dropped_at = host->udp_video_frames_dropped;
        host->udp_stats_log_keydrop_at = host->udp_keyframes_dropped;
        host->udp_stats_log_normal_idr_at =
            host->udp_normal_keyframes_queued;
        host->udp_stats_log_recovery_idr_at =
            host->udp_recovery_keyframes_queued;
        host->udp_stats_log_fragments_at = host->udp_video_fragments_sent;
        host->udp_stats_log_audio_at = host->udp_audio_datagrams_sent;
        host->udp_stats_log_nack_at = host->udp_nack_received;
        host->udp_stats_log_retx_at = host->udp_retx_fragments_sent;
        host->udp_stats_log_fec_at = host->udp_video_fec_fragments_sent;
        host->udp_stats_log_fecdrop_at = host->udp_fec_fragments_dropped;
        host->udp_stats_log_admission_at = host->udp_drop_admission;
        host->udp_stats_log_queue_at = host->udp_drop_queue;
        host->udp_stats_log_deadline_at = host->udp_drop_deadline;
        host->udp_stats_log_send_at = host->udp_drop_send;
        host->udp_stats_log_discontinuity_at =
            host->udp_drop_discontinuity;
        host->udp_stats_log_purge_at = host->udp_drop_recovery_purge;
        return;
    }
    if (now - host->udp_stats_log_window_start < 5000000ULL) {
        return;
    }
    const double seconds =
        (double)(now - host->udp_stats_log_window_start) / 1000000.0;
    const uint64_t frames =
        host->udp_video_frames_sent - host->udp_stats_log_frames_at;
    const uint64_t dropped =
        host->udp_video_frames_dropped - host->udp_stats_log_dropped_at;
    const uint64_t keydrops =
        host->udp_keyframes_dropped - host->udp_stats_log_keydrop_at;
    const uint64_t normal_idr =
        host->udp_normal_keyframes_queued -
        host->udp_stats_log_normal_idr_at;
    const uint64_t recovery_idr =
        host->udp_recovery_keyframes_queued -
        host->udp_stats_log_recovery_idr_at;
    const uint64_t fragments =
        host->udp_video_fragments_sent - host->udp_stats_log_fragments_at;
    const uint64_t audio =
        host->udp_audio_datagrams_sent - host->udp_stats_log_audio_at;
    const uint64_t nacks =
        host->udp_nack_received - host->udp_stats_log_nack_at;
    const uint64_t retx =
        host->udp_retx_fragments_sent - host->udp_stats_log_retx_at;
    const uint64_t fec =
        host->udp_video_fec_fragments_sent - host->udp_stats_log_fec_at;
    const uint64_t fecdrops =
        host->udp_fec_fragments_dropped - host->udp_stats_log_fecdrop_at;
    const uint64_t admission_drops =
        host->udp_drop_admission - host->udp_stats_log_admission_at;
    const uint64_t queue_drops =
        host->udp_drop_queue - host->udp_stats_log_queue_at;
    const uint64_t deadline_drops =
        host->udp_drop_deadline - host->udp_stats_log_deadline_at;
    const uint64_t send_drops =
        host->udp_drop_send - host->udp_stats_log_send_at;
    const uint64_t discontinuity_drops =
        host->udp_drop_discontinuity -
        host->udp_stats_log_discontinuity_at;
    const uint64_t purge_drops =
        host->udp_drop_recovery_purge - host->udp_stats_log_purge_at;
    const uint64_t total_frames = frames + dropped;
    const uint32_t drop_percent =
        total_frames != 0U
            ? (uint32_t)(dropped * 100ULL / total_frames)
            : 0U;
    GRD_INFO(
        "host tx: %.1f frames/s, %llu fragments, %llu dropped, "
        "drop %u%%, keydrop %llu, idr normal=%llu recovery=%llu, "
        "%.1f audio/s, nacks %llu, retx %llu, "
        "fec %llu, fecdrop %llu, pace %u kbps, %zu udp clients, "
        "why adm=%llu queue=%llu deadline=%llu send=%llu gap=%llu "
        "purge=%llu qpeak=%llu",
        (double)frames / seconds,
        fragments,
        dropped,
        drop_percent,
        keydrops,
        normal_idr,
        recovery_idr,
        (double)audio / seconds,
        nacks,
        retx,
        fec,
        fecdrops,
        grd_host_pacing_bits_per_second(host) / 1000U,
        grd_host_udp_video_client_count(host),
        admission_drops,
        queue_drops,
        deadline_drops,
        send_drops,
        discontinuity_drops,
        purge_drops,
        atomic_exchange_explicit(
            &host->udp_pacer_queue_peak, 0U, memory_order_relaxed
        )
    );
    host->udp_stats_log_window_start = now;
    host->udp_stats_log_frames_at = host->udp_video_frames_sent;
    host->udp_stats_log_dropped_at = host->udp_video_frames_dropped;
    host->udp_stats_log_keydrop_at = host->udp_keyframes_dropped;
    host->udp_stats_log_normal_idr_at =
        host->udp_normal_keyframes_queued;
    host->udp_stats_log_recovery_idr_at =
        host->udp_recovery_keyframes_queued;
    host->udp_stats_log_fragments_at = host->udp_video_fragments_sent;
    host->udp_stats_log_audio_at = host->udp_audio_datagrams_sent;
    host->udp_stats_log_nack_at = host->udp_nack_received;
    host->udp_stats_log_retx_at = host->udp_retx_fragments_sent;
    host->udp_stats_log_fec_at = host->udp_video_fec_fragments_sent;
    host->udp_stats_log_fecdrop_at = host->udp_fec_fragments_dropped;
    host->udp_stats_log_admission_at = host->udp_drop_admission;
    host->udp_stats_log_queue_at = host->udp_drop_queue;
    host->udp_stats_log_deadline_at = host->udp_drop_deadline;
    host->udp_stats_log_send_at = host->udp_drop_send;
    host->udp_stats_log_discontinuity_at = host->udp_drop_discontinuity;
    host->udp_stats_log_purge_at = host->udp_drop_recovery_purge;
}

uint32_t grd_host_udp_initiating_drop_percent(const grd_host *host)
{
    if (host == NULL) {
        return 0U;
    }
    return atomic_load_explicit(
        &host->udp_recent_initiating_drop_percent, memory_order_acquire
    );
}

uint32_t grd_host_udp_initiating_drop_sample(
    const grd_host *host,
    uint64_t *generation
)
{
    if (host == NULL) {
        if (generation != NULL) {
            *generation = 0U;
        }
        return 0U;
    }
    /* The producer makes the sequence odd while publishing and even when the
     * percentage is complete. */
    uint64_t before;
    uint64_t after;
    uint32_t percent;
    for (;;) {
        before = atomic_load_explicit(
            &host->udp_recent_initiating_drop_generation,
            memory_order_acquire
        );
        if ((before & 1U) != 0U) {
            continue;
        }
        percent = atomic_load_explicit(
            &host->udp_recent_initiating_drop_percent,
            memory_order_relaxed
        );
        after = atomic_load_explicit(
            &host->udp_recent_initiating_drop_generation,
            memory_order_acquire
        );
        if (before == after && (after & 1U) == 0U) {
            break;
        }
    }
    if (generation != NULL) {
        *generation = after;
    }
    return percent;
}

uint32_t grd_host_udp_drop_percent(const grd_host *host)
{
    return grd_host_udp_initiating_drop_percent(host);
}

void grd_host_resynchronize_video(grd_host *host)
{
    if (host == NULL) {
        return;
    }
    size_t purged = 0U;
    bool opened_recovery = false;
    grd_mutex_lock(&host->mutex);
    for (size_t client_index = 0U;
         client_index < GRD_MAX_CONNECTIONS;
         ++client_index) {
        grd_host_client *client = &host->clients[client_index];
        if (!client->occupied || !client->media_channel) {
            continue;
        }
        grd_mutex_lock(&client->pacer_mutex);
        if (atomic_load_explicit(
                &client->video_discontinuity, memory_order_acquire
            )) {
            grd_mutex_unlock(&client->pacer_mutex);
            continue;
        }
        opened_recovery = true;
        for (size_t index = 0U; index < client->pacer_count;) {
            grd_pacer_unit *unit = &client->pacer_queue[index];
            const bool video_related =
                unit->kind == GRD_PACER_KIND_VIDEO_ORIGINAL ||
                unit->kind == GRD_PACER_KIND_VIDEO_RETX ||
                unit->kind == GRD_PACER_KIND_FEC;
            if (!video_related) {
                ++index;
                continue;
            }
            if (unit->kind == GRD_PACER_KIND_VIDEO_ORIGINAL) {
                ++host->udp_video_frames_dropped;
                ++host->udp_drop_recovery_purge;
            }
            pacer_unit_release(unit);
            for (size_t shift = index;
                 shift + 1U < client->pacer_count;
                 ++shift) {
                client->pacer_queue[shift] =
                    client->pacer_queue[shift + 1U];
            }
            --client->pacer_count;
            memset(
                &client->pacer_queue[client->pacer_count],
                0,
                sizeof(client->pacer_queue[0])
            );
            ++purged;
        }
        for (size_t index = 0U; index < GRD_PACER_RETX_FRAMES; ++index) {
            pacer_unit_release(&client->retx_ring[index]);
            client->retx_ring_ids[index] = 0U;
        }
        client->retx_ring_next = 0U;
        client->pacer_next_send_micros = grd_now_micros();
        atomic_store_explicit(
            &client->video_discontinuity, true, memory_order_release
        );
        atomic_store_explicit(
            &client->recovery_keyframe_queued, false, memory_order_release
        );
        grd_mutex_unlock(&client->pacer_mutex);
    }
    if (opened_recovery) {
        atomic_store_explicit(
            &host->keyframe_pending, true, memory_order_release
        );
    }
    grd_mutex_unlock(&host->mutex);
    GRD_INFO(
        "host pacer: source resync, removed %zu stale video units%s",
        purged,
        opened_recovery ? "" : " (episode already open)"
    );
}

bool grd_host_video_recovery_queued(grd_host *host)
{
    if (host == NULL) {
        return false;
    }
    bool pending = false;
    grd_mutex_lock(&host->mutex);
    for (size_t index = 0U; index < GRD_MAX_CONNECTIONS; ++index) {
        grd_host_client *client = &host->clients[index];
        if (client->occupied && client->media_channel &&
            atomic_load_explicit(&client->running, memory_order_acquire) &&
            atomic_load_explicit(
                &client->video_discontinuity, memory_order_acquire
            ) &&
            atomic_load_explicit(
                &client->recovery_keyframe_queued, memory_order_acquire
            )) {
            pending = true;
            break;
        }
    }
    grd_mutex_unlock(&host->mutex);
    return pending;
}

static void host_handle_udp_nack(
    grd_host_client *client,
    const uint8_t *payload,
    size_t payload_length
);

static void host_process_udp_datagram(
    grd_host *host,
    const uint8_t *wire,
    size_t wire_size,
    const struct sockaddr_storage *source,
    grd_socklen source_length
)
{
    uint8_t plain[512];
    uint16_t type = 0U;
    size_t plain_size = 0U;
    grd_mutex_lock(&host->mutex);
    for (size_t index = 0U; index < GRD_MAX_CONNECTIONS; ++index) {
        grd_host_client *client = &host->clients[index];
        if (!client->occupied || !client->media_channel) {
            continue;
        }
        /* udp_decrypt validates token, replay sequence and authenticity in
         * one pass. */
        if (!udp_decrypt(
                wire,
                wire_size,
                client->crypto.rx_key,
                &client->udp_rx_window,
                client->udp_token,
                &type,
                NULL,
                plain,
                sizeof(plain),
                &plain_size,
                NULL,
                NULL
            )) {
            continue;
        }
        if (type == GRD_PACKET_UDP_NACK &&
            plain_size == sizeof(grd_udp_nack)) {
            host_handle_udp_nack(client, plain, plain_size);
            continue;
        }
        if (type == GRD_PACKET_INPUT) {
            grd_input_event input;
            bool controller_present = false;
            if (plain_size == sizeof(input)) {
                memcpy(&input, plain, sizeof(input));
                for (size_t control_index = 0U;
                     control_index < GRD_MAX_CONNECTIONS;
                     ++control_index) {
                    const grd_host_client *control =
                        &host->clients[control_index];
                    if (control != client && control->occupied &&
                        !control->media_channel &&
                        atomic_load_explicit(
                            &control->running, memory_order_acquire
                        ) &&
                        control->role == GRD_ROLE_CONTROLLER &&
                        strcmp(control->device_id, client->device_id) == 0) {
                        controller_present = true;
                        break;
                    }
                }
            }
            const bool valid = controller_present &&
                               plain_size == sizeof(input) &&
                               input.kind == GRD_INPUT_POINTER_RELATIVE &&
                               grd_protocol_validate_input(&input) == GRD_OK;
            grd_packet_callback callback = host->callback;
            void *userdata = host->userdata;
            grd_mutex_unlock(&host->mutex);
            if (valid && callback != NULL) {
                (void)callback(
                    GRD_PACKET_INPUT,
                    &input,
                    sizeof(input),
                    false,
                    GRD_ROLE_CONTROLLER,
                    userdata
                );
            }
            return;
        }
        if (type != GRD_UDP_PROBE_TYPE || plain_size != 0U) {
            continue;
        }
        grd_mutex_lock(&client->pacer_mutex);
        client->udp_address = *source;
        client->udp_address_length = source_length;
        grd_mutex_unlock(&client->pacer_mutex);
        const bool became_ready = !atomic_load_explicit(
            &client->udp_ready, memory_order_acquire
        );
        /* Publish the peer address (written above) before udp_ready so the
         * pacer threads observe a fully initialized socket state. */
        atomic_store_explicit(
            &client->udp_ready, true, memory_order_release
        );
        atomic_store_explicit(
            &client->udp_failed, false, memory_order_release
        );
        if (became_ready) {
            /* The first IDR was probably consumed before this client was
             * ready; the stream thread forces a fresh one immediately. */
            atomic_store_explicit(
                &host->keyframe_pending, true, memory_order_release
            );
        }
        (void)pacer_enqueue_datagram(
            host, client, GRD_UDP_ACK_TYPE, NULL, 0U
        );
        break;
    }
    grd_mutex_unlock(&host->mutex);
}

/* Re-sends the fragments the client reported missing from the last frame. */
static void host_handle_udp_nack(
    grd_host_client *client,
    const uint8_t *payload,
    size_t payload_length
)
{
    if (client == NULL || payload == NULL ||
        payload_length != sizeof(grd_udp_nack) ||
        client->host == NULL) {
        return;
    }
    const grd_udp_nack *nack = (const grd_udp_nack *)payload;
    const size_t expected_bitmap =
        ((size_t)nack->fragment_count + 7U) / 8U;
    if (nack->fragment_count == 0U ||
        nack->bitmap_bytes == 0U ||
        nack->bitmap_bytes > sizeof(nack->bitmap) ||
        (size_t)nack->bitmap_bytes > expected_bitmap) {
        return;
    }
    grd_mutex_lock(&client->pacer_mutex);
    grd_pacer_unit *ring = pacer_retx_ring_find(client, nack->frame_id);
    if (ring == NULL || ring->fragment_count != nack->fragment_count) {
        grd_mutex_unlock(&client->pacer_mutex);
        return;
    }
    void *clone = ring->payload_ref.clone(ring->payload_ref.opaque);
    if (clone == NULL) {
        grd_mutex_unlock(&client->pacer_mutex);
        return;
    }
    uint16_t requested_fragments = 0U;
    for (uint16_t fragment = 0U; fragment < nack->fragment_count;
         ++fragment) {
        if ((nack->bitmap[fragment / 8U] &
             (uint8_t)(1U << (fragment % 8U))) != 0U) {
            ++requested_fragments;
        }
    }
    ++client->host->udp_nack_received;
    grd_pacer_unit unit;
    memset(&unit, 0, sizeof(unit));
    unit.kind = GRD_PACER_KIND_VIDEO_RETX;
    unit.priority = GRD_PACER_PRIO_RETX;
    unit.frame_id = ring->frame_id;
    unit.fragment_count = ring->fragment_count;
    unit.frame_size = ring->frame_size;
    /* Retransmission deadline: enough wire time for the requested fragments
     * plus an RTT margin, capped by the original frame deadline when it is
     * still in the future. A fixed 5 ms expires before the retx even starts
     * whenever the pacer already carries temporal debt. */
    {
        const uint64_t retx_now = grd_now_micros();
        const uint64_t interval = pacer_pacing_interval_us(client->host);
        const uint64_t send_time_us =
            (uint64_t)requested_fragments * interval;
        uint64_t rtt_margin = atomic_load_explicit(
            &client->host->rtt_micros, memory_order_acquire
        );
        if (rtt_margin < 2000U) {
            rtt_margin = 2000U;
        }
        if (rtt_margin > 50000U) {
            rtt_margin = 50000U;
        }
        uint64_t deadline = retx_now + send_time_us + rtt_margin;
        if (ring->deadline_micros > retx_now &&
            ring->deadline_micros < deadline) {
            deadline = ring->deadline_micros;
        }
        unit.deadline_micros = deadline;
    }
    memcpy(unit.prefix, ring->prefix, ring->prefix_length);
    unit.prefix_length = ring->prefix_length;
    unit.payload = ring->payload;
    unit.payload_length = ring->payload_length;
    unit.payload_ref.opaque = clone;
    unit.payload_ref.clone = ring->payload_ref.clone;
    unit.payload_ref.release = ring->payload_ref.release;
    unit.retx_bitmap = malloc(nack->bitmap_bytes);
    if (unit.retx_bitmap == NULL) {
        /* Under memory pressure a missing bitmap must not degrade into a
         * full-frame retransmission: drop the retx, the client re-NACKs or
         * requests a keyframe. */
        pacer_unit_release(&unit);
        grd_mutex_unlock(&client->pacer_mutex);
        return;
    }
    memcpy(unit.retx_bitmap, nack->bitmap, nack->bitmap_bytes);
    unit.retx_bitmap_size = nack->bitmap_bytes;
    const bool accepted = pacer_enqueue_locked(
        client->host, client, &unit
    );
    grd_mutex_unlock(&client->pacer_mutex);
    if (!accepted) {
        pacer_unit_release(&unit);
    }
}

#if defined(_WIN32)
static DWORD WINAPI host_udp_thread(void *argument)
#else
static void *host_udp_thread(void *argument)
#endif
{
    grd_host *host = argument;
    uint8_t wire[65536];
    while (atomic_load_explicit(&host->running, memory_order_acquire)) {
        host_log_udp_stats(host);
        struct sockaddr_storage source;
        memset(&source, 0, sizeof(source));
        grd_socklen source_length = (grd_socklen)sizeof(source);
        const int received = (int)recvfrom(
            host->udp_socket,
            (char *)wire,
            sizeof(wire),
            0,
            (struct sockaddr *)&source,
            &source_length
        );
        if (received <= 0) {
            if (grd_socket_would_block()) {
                grd_sleep_millis(1U);
            }
            continue;
        }
        host_process_udp_datagram(
            host, wire, (size_t)received, &source, source_length
        );
    }
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

static grd_role reserve_role(
    grd_host *host,
    const grd_host_client *requester,
    grd_role requested
)
{
    grd_role assigned = GRD_ROLE_OBSERVER;
    if (requested == GRD_ROLE_CONTROLLER) {
        bool controller_present = false;
        const uint64_t now = grd_now_micros();
        for (size_t index = 0U; index < GRD_MAX_CONNECTIONS; ++index) {
            grd_host_client *candidate = &host->clients[index];
            if (candidate != requester && candidate->occupied &&
                !candidate->finished && !candidate->media_channel &&
                candidate->role == GRD_ROLE_CONTROLLER) {
                const uint64_t last_activity = atomic_load_explicit(
                    &candidate->last_control_activity_micros,
                    memory_order_relaxed
                );
                const bool same_device = requester->device_id[0] != '\0' &&
                    strcmp(candidate->device_id, requester->device_id) == 0;
                const bool lease_expired = last_activity != 0ULL &&
                    now - last_activity > GRD_CONTROLLER_LEASE_TIMEOUT_US;
                if (same_device || lease_expired) {
                    GRD_WARN(
                        "host role: releasing controller %s (%s)",
                        candidate->device_id,
                        same_device ? "same-device reconnection"
                                    : "heartbeat expired"
                    );
                    atomic_store_explicit(
                        &candidate->running, false, memory_order_release
                    );
                    if (candidate->socket_value != GRD_INVALID_SOCKET) {
                        grd_socket_shutdown(candidate->socket_value);
                    }
                    continue;
                }
                controller_present = true;
                break;
            }
        }
        if (!controller_present) {
            assigned = GRD_ROLE_CONTROLLER;
        }
    }
    return assigned;
}

static void adapt_shortcut_modifier(
    grd_input_event *event,
    grd_os source,
    grd_os destination
)
{
    if (event->kind != GRD_INPUT_KEY || source == destination) {
        return;
    }
    if (source == GRD_OS_MACOS && destination != GRD_OS_MACOS) {
        if (event->code == GRD_KEY_LEFT_GUI) {
            event->code = GRD_KEY_LEFT_CTRL;
        } else if (event->code == GRD_KEY_RIGHT_GUI) {
            event->code = GRD_KEY_RIGHT_CTRL;
        }
    } else if (source != GRD_OS_MACOS && destination == GRD_OS_MACOS) {
        if (event->code == GRD_KEY_LEFT_CTRL) {
            event->code = GRD_KEY_LEFT_GUI;
        } else if (event->code == GRD_KEY_RIGHT_CTRL) {
            event->code = GRD_KEY_RIGHT_GUI;
        }
    }
}

static bool server_handshake(grd_host_client *client)
{
    grd_packet_header header;
    uint8_t *payload = NULL;
    if (!receive_raw_packet(client->socket_value, &header, &payload) ||
        header.type != GRD_PACKET_HELLO ||
        header.payload_length != sizeof(grd_hello)) {
        free(payload);
        return false;
    }
    grd_hello hello;
    memcpy(&hello, payload, sizeof(hello));
    free(payload);
    payload = NULL;
    if (grd_protocol_validate_hello(&hello) != GRD_OK) {
        return false;
    }
    client->peer_os = (grd_os)hello.operating_system;
    client->media_channel = hello.reserved == GRD_HELLO_CHANNEL_MEDIA;
    (void)snprintf(
        client->device_id, sizeof(client->device_id), "%s", hello.device_id
    );
    atomic_store_explicit(
        &client->last_control_activity_micros,
        grd_now_micros(),
        memory_order_relaxed
    );

    grd_auth_context auth;
    grd_auth_challenge challenge;
    grd_error error;
    if (grd_auth_server_begin(
            &client->host->config, &auth, &challenge, &error
        ) != GRD_OK ||
        !send_raw_packet(
            client->socket_value, GRD_PACKET_AUTH_CHALLENGE, 0U,
            &challenge, sizeof(challenge)
        ) ||
        !receive_raw_packet(client->socket_value, &header, &payload) ||
        header.type != GRD_PACKET_AUTH_RESPONSE ||
        header.payload_length != sizeof(grd_auth_response)) {
        grd_auth_context_clear(&auth);
        free(payload);
        return false;
    }
    grd_auth_response response;
    memcpy(&response, payload, sizeof(response));
    free(payload);
    if (grd_auth_server_finish(
            &client->host->config, &auth, &challenge, &response, &error
        ) != GRD_OK) {
        GRD_WARN("Authentication rejected");
        grd_auth_context_clear(&auth);
        return false;
    }
    crypto_initialize(&client->crypto, auth.session_key, true);
    grd_auth_context_clear(&auth);
    /* Reserve the exclusive input role only after authentication. Otherwise
     * an unauthenticated peer could spoof a known device id and force the
     * current controller out through the crash-recovery takeover path. */
    grd_mutex_lock(&client->host->mutex);
    client->role = client->media_channel
                       ? GRD_ROLE_OBSERVER
                       : reserve_role(
                             client->host,
                             client,
                             (grd_role)hello.requested_role
                         );
    grd_mutex_unlock(&client->host->mutex);
    if (!client->media_channel) {
        GRD_INFO(
            "host role: device=%s requested=%s assigned=%s",
            client->device_id,
            hello.requested_role == GRD_ROLE_CONTROLLER ? "controller"
                                                        : "observer",
            client->role == GRD_ROLE_CONTROLLER ? "controller" : "observer"
        );
    }
    const uint8_t result[2] = {
        (uint8_t)client->role,
        (uint8_t)grd_platform_os()
    };
    const grd_outgoing_packet auth_result_packet = {
        .type = GRD_PACKET_AUTH_RESULT,
        .payload = result,
        .payload_length = sizeof(result),
        .total_length = sizeof(result)
    };
    if (!send_secure_packet(
            client->socket_value,
            &client->send_mutex,
            &client->crypto,
            &auth_result_packet
        )) {
        return false;
    }
    if (client->media_channel) {
        randombytes_buf(client->udp_token, sizeof(client->udp_token));
        grd_video_udp_token token = {
            .port = client->host->config.port,
            .reserved = 0U,
        };
        memcpy(
            token.token,
            client->udp_token,
            sizeof(client->udp_token)
        );
        const grd_outgoing_packet token_packet = {
            .type = GRD_PACKET_VIDEO_UDP_TOKEN,
            .payload = (const uint8_t *)&token,
            .payload_length = sizeof(token),
            .total_length = sizeof(token)
        };
        if (!send_secure_packet(
                client->socket_value,
                &client->send_mutex,
                &client->crypto,
                &token_packet
            )) {
            return false;
        }
    }
    return true;
}

static bool media_clipboard_has_controller(const grd_host_client *client)
{
    if (client == NULL || !client->media_channel || client->host == NULL) {
        return false;
    }
    bool allowed = false;
    grd_host *host = client->host;
    grd_mutex_lock(&host->mutex);
    for (size_t index = 0U; index < GRD_MAX_CONNECTIONS; ++index) {
        const grd_host_client *control = &host->clients[index];
        if (control != client && control->occupied &&
            !control->media_channel &&
            atomic_load_explicit(&control->running, memory_order_acquire) &&
            control->role == GRD_ROLE_CONTROLLER &&
            strcmp(control->device_id, client->device_id) == 0) {
            allowed = true;
            break;
        }
    }
    grd_mutex_unlock(&host->mutex);
    return allowed;
}

#if defined(_WIN32)
static DWORD WINAPI host_client_thread(void *argument)
#else
static void *host_client_thread(void *argument)
#endif
{
    grd_host_client *client = argument;
    if (server_handshake(client)) {
        /*
         * The five-second timeout only protects the unauthenticated
         * handshake. Keeping it afterwards disconnects observers because
         * they legitimately send no input while receiving the stream.
        */
        (void)grd_socket_set_timeout(client->socket_value, 0U);
        atomic_store_explicit(&client->running, true, memory_order_release);
#if defined(_WIN32)
        client->send_thread = CreateThread(
            NULL, 0U, host_send_thread, client, 0U, NULL
        );
        client->send_thread_started = client->send_thread != NULL;
#else
        client->send_thread_started = pthread_create(
            &client->send_thread, NULL, host_send_thread, client
        ) == 0;
#endif
        if (!client->send_thread_started) {
            atomic_store_explicit(&client->running, false, memory_order_release);
        }
#if defined(_WIN32)
        client->pacer_thread = CreateThread(
            NULL, 0U, pacer_thread_fn, client, 0U, NULL
        );
        client->pacer_thread_started = client->pacer_thread != NULL;
#else
        client->pacer_thread_started = pthread_create(
            &client->pacer_thread, NULL, pacer_thread_fn, client
        ) == 0;
#endif
        if (!client->pacer_thread_started) {
            atomic_store_explicit(&client->running, false, memory_order_release);
        }
        while (atomic_load_explicit(&client->host->running, memory_order_acquire) &&
               atomic_load_explicit(&client->running, memory_order_acquire)) {
            grd_packet_header header;
            uint8_t *payload = NULL;
            size_t payload_length = 0U;
            if (!receive_secure_packet(
                    client->socket_value, &client->crypto,
                    &header, &payload, &payload_length
                )) {
                break;
            }
            atomic_store_explicit(
                &client->last_control_activity_micros,
                grd_now_micros(),
                memory_order_relaxed
            );
            if (header.type == GRD_PACKET_UDP_NACK) {
                host_handle_udp_nack(client, payload, payload_length);
                continue;
            }
            const bool media_clipboard =
                header.type == GRD_PACKET_CLIPBOARD &&
                media_clipboard_has_controller(client);
            const bool permitted =
                client->role == GRD_ROLE_CONTROLLER || media_clipboard ||
                (header.type != GRD_PACKET_INPUT &&
                 header.type != GRD_PACKET_CLIPBOARD);
            if (permitted && client->host->callback != NULL) {
                if (header.type == GRD_PACKET_INPUT &&
                    payload_length == sizeof(grd_input_event)) {
                    grd_input_event input;
                    memcpy(&input, payload, sizeof(input));
                    if (grd_protocol_validate_input(&input) == GRD_OK) {
                        adapt_shortcut_modifier(
                            &input, client->peer_os, grd_platform_os()
                        );
                        client->host->callback(
                            GRD_PACKET_INPUT,
                            &input,
                            sizeof(input),
                            false,
                            client->role,
                            client->host->userdata
                        );
                    }
                } else {
                    const bool consumed = client->host->callback(
                        (grd_packet_type)header.type,
                        payload,
                        payload_length,
                        payload != NULL,
                        media_clipboard ? GRD_ROLE_CONTROLLER : client->role,
                        client->host->userdata
                    );
                    if (consumed) {
                        client->crypto.rx_plain = NULL;
                        client->crypto.rx_plain_capacity = 0U;
                    }
                }
            }
        }
    }
    atomic_store_explicit(&client->running, false, memory_order_release);
    send_queue_stop(&client->send_queue);
    grd_socket_shutdown(client->socket_value);
    if (client->send_thread_started) {
        grd_thread_join(client->send_thread);
        client->send_thread_started = false;
    }
    client_pacer_stop(client);
    grd_socket_close(client->socket_value);
    client->socket_value = GRD_INVALID_SOCKET;
    grd_mutex_lock(&client->host->mutex);
    client->finished = true;
    grd_mutex_unlock(&client->host->mutex);
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

static void reap_finished_clients(grd_host *host)
{
    for (size_t index = 0U; index < GRD_MAX_CONNECTIONS; ++index) {
        grd_host_client *client = &host->clients[index];
        grd_mutex_lock(&host->mutex);
        const bool reap = client->occupied && client->thread_started &&
                          client->finished;
        grd_mutex_unlock(&host->mutex);
        if (!reap) {
            continue;
        }
        grd_thread_join(client->thread);
        grd_cond_destroy(&client->pacer_condition);
        grd_mutex_destroy(&client->pacer_mutex);
        send_queue_destroy(&client->send_queue);
        crypto_destroy(&client->crypto);
        grd_mutex_destroy(&client->send_mutex);
        grd_mutex_destroy(&client->udp_mutex);
        grd_mutex_lock(&host->mutex);
        client->occupied = false;
        client->thread_started = false;
        client->finished = false;
        grd_mutex_unlock(&host->mutex);
    }
}

#if defined(_WIN32)
static DWORD WINAPI accept_thread(void *argument)
#else
static void *accept_thread(void *argument)
#endif
{
    grd_host *host = argument;
    while (host->running) {
        reap_finished_clients(host);
        struct sockaddr_storage address;
#if defined(_WIN32)
        int address_length = sizeof(address);
#else
        socklen_t address_length = sizeof(address);
#endif
        const grd_socket socket_value = accept(
            host->listen_socket,
            (struct sockaddr *)&address,
            &address_length
        );
        if (socket_value == GRD_INVALID_SOCKET) {
            continue;
        }
        (void)grd_socket_set_low_latency(socket_value);
        (void)grd_socket_set_timeout(socket_value, 5000U);
        reap_finished_clients(host);
        grd_mutex_lock(&host->mutex);
        grd_host_client *client = NULL;
        for (size_t index = 0U; index < GRD_MAX_CONNECTIONS; ++index) {
            if (!host->clients[index].occupied) {
                client = &host->clients[index];
                break;
            }
        }
        if (client == NULL) {
            grd_mutex_unlock(&host->mutex);
            grd_socket_close(socket_value);
            continue;
        }
        memset(&client->crypto, 0, sizeof(client->crypto));
        client->socket_value = socket_value;
        client->host = host;
        client->occupied = true;
        atomic_store_explicit(&client->running, false, memory_order_release);
        client->thread_started = false;
        client->finished = false;
        client->send_thread_started = false;
        client->media_channel = false;
        client->role = GRD_ROLE_OBSERVER;
        client->peer_os = GRD_OS_UNKNOWN;
        atomic_store_explicit(
            &client->last_control_activity_micros,
            grd_now_micros(),
            memory_order_relaxed
        );
        client->udp_ready = false;
        client->udp_failed = false;
        atomic_store_explicit(
            &client->video_discontinuity, false, memory_order_release
        );
        atomic_store_explicit(
            &client->recovery_keyframe_queued, false, memory_order_release
        );
        memset(client->udp_token, 0, sizeof(client->udp_token));
        memset(&client->udp_address, 0, sizeof(client->udp_address));
        client->udp_address_length = 0;
        client->udp_tx_sequence = 0U;
        memset(&client->udp_rx_window, 0, sizeof(client->udp_rx_window));
        client->udp_tx_frame_id = 0U;
        client->pacer_count = 0U;
        client->pacer_thread_started = false;
        client->retx_ring_next = 0U;
        memset(client->pacer_queue, 0, sizeof(client->pacer_queue));
        memset(client->retx_ring, 0, sizeof(client->retx_ring));
        memset(client->retx_ring_ids, 0, sizeof(client->retx_ring_ids));
        memset(client->device_id, 0, sizeof(client->device_id));
        grd_mutex_init(&client->send_mutex);
        grd_mutex_init(&client->udp_mutex);
        grd_mutex_init(&client->pacer_mutex);
        grd_cond_init(&client->pacer_condition);
        client->pacer_stopping = false;
        client->udp_tx_sequence = 1U;
        send_queue_init(&client->send_queue);
#if defined(_WIN32)
        client->thread = CreateThread(
            NULL, 0U, host_client_thread, client, 0U, NULL
        );
        if (client->thread == NULL) {
#else
        if (pthread_create(&client->thread, NULL, host_client_thread, client) != 0) {
#endif
            client->occupied = false;
            send_queue_destroy(&client->send_queue);
            grd_cond_destroy(&client->pacer_condition);
            grd_mutex_destroy(&client->pacer_mutex);
            grd_mutex_destroy(&client->send_mutex);
            grd_mutex_destroy(&client->udp_mutex);
            grd_socket_close(socket_value);
        } else {
            client->thread_started = true;
        }
        grd_mutex_unlock(&host->mutex);
    }
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

static grd_socket create_host_udp_socket(
    uint16_t port,
    grd_error *error
)
{
    grd_socket socket_value = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_value == GRD_INVALID_SOCKET) {
        if (error != NULL) {
            error->code = GRD_IO_ERROR;
            (void)snprintf(
                error->message, sizeof(error->message),
                "Unable to create the UDP video socket"
            );
        }
        return GRD_INVALID_SOCKET;
    }
    int dual_stack = 0;
    int reuse = 1;
    (void)setsockopt(
        socket_value,
        IPPROTO_IPV6,
        IPV6_V6ONLY,
        (const char *)&dual_stack,
        sizeof(dual_stack)
    );
    (void)setsockopt(
        socket_value,
        SOL_SOCKET,
        SO_REUSEADDR,
        (const char *)&reuse,
        sizeof(reuse)
    );
    int send_buffer = 1024 * 1024;
    (void)setsockopt(
        socket_value,
        SOL_SOCKET,
        SO_SNDBUF,
        (const char *)&send_buffer,
        sizeof(send_buffer)
    );
    /* A full socket buffer must never stall capture/encode: drop the frame
     * and request an IDR instead. Receiver-reported UDP loss controls the
     * wire ABR; local drops are kept separate so they cannot recursively
     * shrink the pacer below an encoder that has not changed rate. */
    if (!grd_socket_set_nonblocking(socket_value)) {
        grd_socket_close(socket_value);
        if (error != NULL) {
            error->code = GRD_IO_ERROR;
            (void)snprintf(
                error->message, sizeof(error->message),
                "Unable to configure the UDP video socket"
            );
        }
        return GRD_INVALID_SOCKET;
    }
    struct sockaddr_in6 address;
    memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    address.sin6_addr = in6addr_any;
    if (bind(
            socket_value,
            (const struct sockaddr *)&address,
            (grd_socklen)sizeof(address)
        ) != 0) {
        grd_socket_close(socket_value);
        if (error != NULL) {
            (void)snprintf(
                error->message,
                sizeof(error->message),
                "UDP video is unavailable on port %u",
                (unsigned)port
            );
            error->code = GRD_IO_ERROR;
        }
        return GRD_INVALID_SOCKET;
    }
    /* Receive timeout makes shutdown deterministic; the socket is
     * non-blocking so a congested peer never stalls the stream thread. */
    if (!grd_socket_set_timeout(socket_value, 200U)) {
        grd_socket_close(socket_value);
        if (error != NULL) {
            error->code = GRD_IO_ERROR;
            (void)snprintf(
                error->message, sizeof(error->message),
                "Unable to configure the UDP video socket"
            );
        }
        return GRD_INVALID_SOCKET;
    }
    return socket_value;
}

grd_host *grd_host_start(
    const grd_config *config,
    grd_packet_callback callback,
    void *userdata,
    grd_error *error
)
{
    if (config == NULL || !config->password_configured || !grd_net_initialize()) {
        return NULL;
    }
    grd_host *host = calloc(1U, sizeof(*host));
    if (host == NULL) {
        grd_net_shutdown();
        return NULL;
    }
    host->listen_socket = GRD_INVALID_SOCKET;
    host->udp_socket = GRD_INVALID_SOCKET;
    host->config = *config;
    host->callback = callback;
    host->userdata = userdata;
    atomic_store_explicit(
        &host->pacing_bits_per_second, 24000000U, memory_order_release
    );
    atomic_store_explicit(
        &host->frame_period_us, 16667U, memory_order_release
    );
    host->listen_socket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (host->listen_socket == GRD_INVALID_SOCKET) {
        free(host);
        grd_net_shutdown();
        return NULL;
    }
    int dual_stack = 0;
    int reuse = 1;
    (void)setsockopt(
        host->listen_socket, IPPROTO_IPV6, IPV6_V6ONLY,
        (const char *)&dual_stack, sizeof(dual_stack)
    );
    (void)setsockopt(
        host->listen_socket, SOL_SOCKET, SO_REUSEADDR,
        (const char *)&reuse, sizeof(reuse)
    );
    struct sockaddr_in6 address;
    memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(config->port);
    address.sin6_addr = in6addr_any;
    if (bind(
            host->listen_socket,
            (const struct sockaddr *)&address,
            sizeof(address)
        ) != 0 ||
        listen(host->listen_socket, (int)GRD_MAX_CONNECTIONS) != 0) {
        if (error != NULL) {
            error->code = GRD_IO_ERROR;
            (void)snprintf(error->message, sizeof(error->message), "Port %u is unavailable", config->port);
        }
        grd_socket_close(host->listen_socket);
        free(host);
        grd_net_shutdown();
        return NULL;
    }
    grd_error udp_error = {0};
    host->udp_socket = create_host_udp_socket(config->port, &udp_error);
    if (host->udp_socket == GRD_INVALID_SOCKET) {
        /* Video transport is intentionally UDP-only. Do not start a host that
         * would silently downgrade the media stream to TCP. */
        if (error != NULL) {
            *error = udp_error;
        }
        grd_socket_close(host->listen_socket);
        free(host);
        grd_net_shutdown();
        return NULL;
    }
    grd_mutex_init(&host->mutex);
    host->running = true;
#if defined(_WIN32)
    host->accept_thread = CreateThread(NULL, 0U, accept_thread, host, 0U, NULL);
    if (host->accept_thread == NULL) {
#else
    if (pthread_create(&host->accept_thread, NULL, accept_thread, host) != 0) {
#endif
        host->running = false;
        grd_host_stop(host);
        return NULL;
    }
    host->accept_thread_started = true;
    if (host->udp_socket != GRD_INVALID_SOCKET) {
#if defined(_WIN32)
        host->udp_thread = CreateThread(
            NULL, 0U, host_udp_thread, host, 0U, NULL
        );
        host->udp_thread_started = host->udp_thread != NULL;
#else
        host->udp_thread_started = pthread_create(
            &host->udp_thread, NULL, host_udp_thread, host
        ) == 0;
#endif
        if (!host->udp_thread_started) {
            if (error != NULL) {
                error->code = GRD_IO_ERROR;
                (void)snprintf(
                    error->message, sizeof(error->message),
                    "Unable to start the UDP video thread"
                );
            }
            grd_host_stop(host);
            return NULL;
        }
    }
    return host;
}

grd_status grd_host_broadcast(
    grd_host *host,
    grd_packet_type type,
    const void *payload,
    size_t payload_length,
    grd_error *error
)
{
    if (host == NULL) {
        if (error != NULL) {
            error->code = GRD_INVALID_ARGUMENT;
            (void)snprintf(
                error->message, sizeof(error->message),
                "Invalid host"
            );
        }
        return GRD_INVALID_ARGUMENT;
    }
    grd_status status = GRD_OK;
    grd_mutex_lock(&host->mutex);
    for (size_t index = 0U; index < GRD_MAX_CONNECTIONS; ++index) {
        grd_host_client *client = &host->clients[index];
        if (client->occupied &&
            atomic_load_explicit(&client->running, memory_order_acquire) &&
            packet_belongs_to_channel(host, client, type)) {
            if (type == GRD_PACKET_VIDEO_FRAME && client->media_channel) {
                if (!client->udp_ready) {
                    if (client->udp_failed) {
                        status = GRD_IO_ERROR;
                        if (error != NULL) {
                            error->code = GRD_IO_ERROR;
                            (void)snprintf(
                                error->message, sizeof(error->message),
                                "UDP video transport is unavailable"
                            );
                        }
                    }
                    /* The UDP probe may still be in flight. Never enqueue the
                     * frame on TCP while waiting for it. */
                    continue;
                }
                const grd_status udp_status = pacer_enqueue_video(
                    host, client, NULL, 0U, payload, payload_length,
                    NULL, false
                );
                if (udp_status == GRD_BUSY) {
                    /* Admission control or a full queue: drop this frame for
                     * this client instead of stalling the capture thread. */
                    continue;
                }
                if (udp_status != GRD_OK) {
                    client->udp_ready = false;
                    client->udp_failed = true;
                    status = GRD_IO_ERROR;
                    if (error != NULL) {
                        error->code = GRD_IO_ERROR;
                        (void)snprintf(
                            error->message, sizeof(error->message),
                            "Failed to send UDP video"
                        );
                    }
                }
                continue;
            }
            if (type == GRD_PACKET_AUDIO_FRAME && client->media_channel &&
                client->udp_ready &&
                pacer_enqueue_datagram(
                    host, client, GRD_PACKET_AUDIO_FRAME,
                    payload, payload_length
                ) == GRD_OK) {
                /* Audio normally rides the video UDP channel as a single
                 * datagram; any failure falls through to the reliable queue
                 * below so a transient UDP problem never silences audio. */
                continue;
            }
            const grd_status push_status = send_queue_push(
                &client->send_queue, type, payload, payload_length
            );
            if (push_status != GRD_OK) {
                /* A saturated media queue means that observer is behind: it
                 * is safer to drop/skip media than to tear down the session
                 * or stall capture for every other client. */
                if (!packet_is_media(type) || push_status != GRD_BUSY) {
                    atomic_store_explicit(&client->running, false, memory_order_release);
                    status = GRD_IO_ERROR;
                } else {
                    status = GRD_BUSY;
                }
            }
        }
    }
    grd_mutex_unlock(&host->mutex);
    return status;
}

grd_status grd_host_broadcast_parts(
    grd_host *host,
    grd_packet_type type,
    const grd_buf_part *parts,
    size_t part_count,
    grd_owned_buffer *payload_ref,
    bool keyframe,
    grd_error *error
)
{
    if (host == NULL || parts == NULL || payload_ref == NULL ||
        payload_ref->clone == NULL || payload_ref->release == NULL ||
        part_count == 0U || part_count > GRD_BROADCAST_MAX_PARTS) {
        if (payload_ref != NULL && payload_ref->release != NULL) {
            payload_ref->release((void *)payload_ref->opaque);
        }
        if (error != NULL) {
            error->code = GRD_INVALID_ARGUMENT;
            (void)snprintf(
                error->message, sizeof(error->message),
                "Invalid broadcast payload"
            );
        }
        return GRD_INVALID_ARGUMENT;
    }
    size_t prefix_length = 0U;
    for (size_t index = 0U; index + 1U < part_count; ++index) {
        prefix_length += parts[index].length;
    }
    const grd_buf_part *payload_part = &parts[part_count - 1U];
    if (prefix_length > GRD_BROADCAST_MAX_INLINE_PREFIX ||
        payload_part->data == NULL || payload_part->length == 0U) {
        payload_ref->release((void *)payload_ref->opaque);
        if (error != NULL) {
            error->code = GRD_INVALID_ARGUMENT;
            (void)snprintf(
                error->message, sizeof(error->message),
                "Invalid broadcast payload"
            );
        }
        return GRD_INVALID_ARGUMENT;
    }
    uint8_t prefix[GRD_BROADCAST_MAX_INLINE_PREFIX];
    size_t written = 0U;
    for (size_t index = 0U; index + 1U < part_count; ++index) {
        memcpy(prefix + written, parts[index].data, parts[index].length);
        written += parts[index].length;
    }
    grd_status status = GRD_OK;
    grd_mutex_lock(&host->mutex);
    for (size_t index = 0U; index < GRD_MAX_CONNECTIONS; ++index) {
        grd_host_client *client = &host->clients[index];
        if (!client->occupied ||
            !atomic_load_explicit(&client->running, memory_order_acquire) ||
            !packet_belongs_to_channel(host, client, type)) {
            continue;
        }
        if (type == GRD_PACKET_VIDEO_FRAME && client->media_channel) {
            if (!client->udp_ready) {
                if (client->udp_failed) {
                    status = GRD_IO_ERROR;
                    if (error != NULL) {
                        error->code = GRD_IO_ERROR;
                        (void)snprintf(
                            error->message, sizeof(error->message),
                            "UDP video transport is unavailable"
                        );
                    }
                }
                /* The UDP probe may still be in flight. Never enqueue the
                 * frame on TCP while waiting for it. */
                continue;
            }
            const grd_status udp_status = pacer_enqueue_video(
                host,
                client,
                prefix,
                prefix_length,
                payload_part->data,
                payload_part->length,
                payload_ref,
                keyframe
            );
            if (udp_status == GRD_BUSY) {
                continue;
            }
            if (udp_status != GRD_OK) {
                client->udp_ready = false;
                client->udp_failed = true;
                status = GRD_IO_ERROR;
                if (error != NULL) {
                    error->code = GRD_IO_ERROR;
                    (void)snprintf(
                        error->message, sizeof(error->message),
                        "Failed to send UDP video"
                    );
                }
            }
            continue;
        }
        if (type == GRD_PACKET_AUDIO_FRAME && client->media_channel &&
            client->udp_ready) {
            uint8_t audio_wire[GRD_UDP_MAX_DATAGRAM];
            size_t audio_wire_length = prefix_length + payload_part->length;
            const size_t audio_capacity =
                GRD_UDP_MAX_DATAGRAM - GRD_UDP_HEADER_SIZE -
                GRD_UDP_AEAD_OVERHEAD;
            if (audio_wire_length <= audio_capacity) {
                memcpy(audio_wire, prefix, prefix_length);
                memcpy(
                    audio_wire + prefix_length,
                    payload_part->data,
                    payload_part->length
                );
                if (pacer_enqueue_datagram(
                        host,
                        client,
                        GRD_PACKET_AUDIO_FRAME,
                        audio_wire,
                        audio_wire_length
                    )) {
                    continue;
                }
            }
            /* Falls through to the reliable queue below. */
        }
        void *owned = payload_ref->clone(payload_ref->opaque);
        if (owned == NULL) {
            status = GRD_BUSY;
            continue;
        }
        grd_owned_buffer entry_ref = {
            .opaque = owned,
            .clone = payload_ref->clone,
            .release = payload_ref->release
        };
        const grd_status push_status = send_queue_push_owned(
            &client->send_queue, type, parts, part_count, &entry_ref
        );
        if (push_status == GRD_INVALID_ARGUMENT) {
            entry_ref.release(owned);
        } else if (push_status != GRD_OK) {
            /* The queue consumed the reference on any other failure. */
            if (!packet_is_media(type) || push_status != GRD_BUSY) {
                atomic_store_explicit(
                    &client->running, false, memory_order_release
                );
                status = GRD_IO_ERROR;
            } else {
                status = GRD_BUSY;
            }
        }
    }
    grd_mutex_unlock(&host->mutex);
    payload_ref->release((void *)payload_ref->opaque);
    return status;
}

size_t grd_host_client_count(const grd_host *host)
{
    if (host == NULL) {
        return 0U;
    }
    size_t count = 0U;
    grd_mutex_lock((grd_mutex *)&host->mutex);
    for (size_t index = 0U; index < GRD_MAX_CONNECTIONS; ++index) {
        if (host->clients[index].occupied && !host->clients[index].media_channel &&
            host->clients[index].running) {
            ++count;
        }
    }
    grd_mutex_unlock((grd_mutex *)&host->mutex);
    return count;
}

size_t grd_host_udp_video_client_count(const grd_host *host)
{
    if (host == NULL) {
        return 0U;
    }
    size_t count = 0U;
    grd_mutex_lock((grd_mutex *)&host->mutex);
    for (size_t index = 0U; index < GRD_MAX_CONNECTIONS; ++index) {
        const grd_host_client *client = &host->clients[index];
        if (client->occupied && client->media_channel && client->udp_ready) {
            ++count;
        }
    }
    grd_mutex_unlock((grd_mutex *)&host->mutex);
    return count;
}

bool grd_host_take_keyframe_pending(grd_host *host)
{
    if (host == NULL) {
        return false;
    }
    return atomic_exchange_explicit(
        &host->keyframe_pending, false, memory_order_acq_rel
    );
}

void grd_host_stop(grd_host *host)
{
    if (host == NULL) {
        return;
    }
    const bool was_running = host->running;
    host->running = false;
    if (host->listen_socket != GRD_INVALID_SOCKET) {
        grd_socket_shutdown(host->listen_socket);
        grd_socket_close(host->listen_socket);
        host->listen_socket = GRD_INVALID_SOCKET;
    }
    if (was_running && host->accept_thread_started) {
        grd_thread_join(host->accept_thread);
    }
    if (host->udp_socket != GRD_INVALID_SOCKET) {
        grd_socket_shutdown(host->udp_socket);
        grd_socket_close(host->udp_socket);
        host->udp_socket = GRD_INVALID_SOCKET;
    }
    if (host->udp_thread_started) {
        grd_thread_join(host->udp_thread);
        host->udp_thread_started = false;
    }
    for (size_t index = 0U; index < GRD_MAX_CONNECTIONS; ++index) {
        grd_host_client *client = &host->clients[index];
        if (client->occupied) {
            atomic_store_explicit(&client->running, false, memory_order_release);
            send_queue_stop(&client->send_queue);
            if (client->socket_value != GRD_INVALID_SOCKET) {
                grd_socket_shutdown(client->socket_value);
            }
            if (client->thread_started) {
                grd_thread_join(client->thread);
            }
            if (client->send_thread_started) {
                grd_thread_join(client->send_thread);
                client->send_thread_started = false;
            }
            client_pacer_stop(client);
            grd_cond_destroy(&client->pacer_condition);
            grd_mutex_destroy(&client->pacer_mutex);
            send_queue_destroy(&client->send_queue);
            crypto_destroy(&client->crypto);
            grd_mutex_destroy(&client->send_mutex);
            grd_mutex_destroy(&client->udp_mutex);
        }
    }
    grd_mutex_destroy(&host->mutex);
    grd_net_shutdown();
    grd_secure_zero(host, sizeof(*host));
    free(host);
}

static bool client_handshake(
    grd_connection *connection,
    const char *password,
    grd_role requested_role,
    const grd_config *local_config,
    bool media_channel
)
{
    const grd_hello hello = {
        .requested_role = (uint8_t)requested_role,
        .operating_system = (uint8_t)grd_platform_os(),
        .reserved = media_channel ? GRD_HELLO_CHANNEL_MEDIA
                                  : GRD_HELLO_CHANNEL_CONTROL
    };
    grd_hello mutable_hello = hello;
    (void)snprintf(
        mutable_hello.device_id,
        sizeof(mutable_hello.device_id),
        "%s",
        local_config->device_id
    );
    (void)snprintf(
        mutable_hello.device_name,
        sizeof(mutable_hello.device_name),
        "%s",
        local_config->device_name
    );
    if (!send_raw_packet(
            connection->socket_value, GRD_PACKET_HELLO, 0U,
            &mutable_hello, sizeof(mutable_hello)
        )) {
        return false;
    }
    grd_packet_header header;
    uint8_t *payload = NULL;
    if (!receive_raw_packet(connection->socket_value, &header, &payload) ||
        header.type != GRD_PACKET_AUTH_CHALLENGE ||
        header.payload_length != sizeof(grd_auth_challenge)) {
        free(payload);
        return false;
    }
    grd_auth_challenge challenge;
    memcpy(&challenge, payload, sizeof(challenge));
    free(payload);
    grd_auth_context auth;
    grd_auth_response response;
    grd_error error;
    if (grd_auth_client_respond(
            password, &challenge, &auth, &response, &error
        ) != GRD_OK ||
        !send_raw_packet(
            connection->socket_value, GRD_PACKET_AUTH_RESPONSE, 0U,
            &response, sizeof(response)
        )) {
        grd_auth_context_clear(&auth);
        return false;
    }
    crypto_initialize(&connection->crypto, auth.session_key, false);
    grd_auth_context_clear(&auth);
    size_t payload_length = 0U;
    if (!receive_secure_packet(
            connection->socket_value, &connection->crypto,
            &header, &payload, &payload_length
        ) ||
        header.type != GRD_PACKET_AUTH_RESULT ||
        payload_length != 2U) {
        return false;
    }
    connection->role = (grd_role)payload[0];
    connection->media_channel = media_channel;
    if (media_channel) {
        if (!receive_secure_packet(
                connection->socket_value,
                &connection->crypto,
                &header,
                &payload,
                &payload_length
            ) ||
            header.type != GRD_PACKET_VIDEO_UDP_TOKEN ||
            payload_length != sizeof(grd_video_udp_token)) {
            return false;
        }
        const grd_video_udp_token *token = (const grd_video_udp_token *)payload;
        memcpy(connection->udp_token, token->token, sizeof(connection->udp_token));
        connection->udp_token_valid = true;
    }
    return true;
}

static void udp_frame_slot_reset(grd_udp_frame_slot *slot)
{
    if (slot == NULL) {
        return;
    }
    free(slot->data);
    free(slot->bitmap);
    free(slot->fec_parity);
    free(slot->fec_received_bitmap);
    memset(slot, 0, sizeof(*slot));
}

static void connection_udp_slots_reset(grd_connection *connection)
{
    if (connection == NULL) {
        return;
    }
    for (size_t index = 0U; index < GRD_UDP_FRAME_SLOTS; ++index) {
        udp_frame_slot_reset(&connection->udp_slots[index]);
    }
}

static grd_udp_frame_slot *udp_slot_for_frame(
    grd_connection *connection,
    uint64_t frame_id
)
{
    for (size_t index = 0U; index < GRD_UDP_FRAME_SLOTS; ++index) {
        grd_udp_frame_slot *slot = &connection->udp_slots[index];
        if (slot->active && slot->frame_id == frame_id) {
            return slot;
        }
    }
    return NULL;
}

static grd_udp_frame_slot *udp_slot_free(grd_connection *connection)
{
    for (size_t index = 0U; index < GRD_UDP_FRAME_SLOTS; ++index) {
        if (!connection->udp_slots[index].active) {
            return &connection->udp_slots[index];
        }
    }
    return NULL;
}

static void udp_slot_start(
    grd_udp_frame_slot *slot,
    uint64_t frame_id,
    uint32_t frame_size,
    uint16_t fragment_count
)
{
    udp_frame_slot_reset(slot);
    slot->frame_id = frame_id;
    slot->frame_size = frame_size;
    slot->fragment_count = fragment_count;
    slot->bitmap_size = ((size_t)fragment_count + 7U) / 8U;
    slot->data = malloc((size_t)frame_size + GRD_MEDIA_BUFFER_PADDING);
    if (slot->data != NULL) {
        /* Zeroed tail space lets the decoder adopt the buffer instead of
         * copying it (FFmpeg requires FF_INPUT_BUFFER_PADDING_SIZE). */
        memset(
            slot->data + frame_size, 0, GRD_MEDIA_BUFFER_PADDING
        );
    }
    slot->bitmap = calloc(slot->bitmap_size, 1U);
    slot->active = slot->data != NULL && slot->bitmap != NULL;
    if (slot->active) {
        const size_t fec_blocks =
            ((size_t)fragment_count + GRD_UDP_FEC_BLOCK_FRAGMENTS - 1U) /
            GRD_UDP_FEC_BLOCK_FRAGMENTS;
        if (fec_blocks != 0U && fec_blocks <= GRD_UDP_FEC_MAX_BLOCKS) {
            slot->fec_parity = calloc(
                fec_blocks, GRD_UDP_FRAGMENT_CAPACITY
            );
            slot->fec_received_bitmap = calloc(
                ((size_t)fec_blocks + 7U) / 8U, 1U
            );
            if (slot->fec_parity != NULL &&
                slot->fec_received_bitmap != NULL) {
                slot->fec_block_count = (uint16_t)fec_blocks;
            } else {
                free(slot->fec_parity);
                free(slot->fec_received_bitmap);
                slot->fec_parity = NULL;
                slot->fec_received_bitmap = NULL;
            }
        }
    }
    if (!slot->active) {
        udp_frame_slot_reset(slot);
    }
}

static void udp_slot_send_nack(
    grd_connection *connection,
    grd_udp_frame_slot *slot
)
{
    if (connection == NULL || slot == NULL || !slot->active) {
        return;
    }
    const uint64_t now = grd_now_micros();
    if (slot->nack_sent_micros != 0U &&
        now - slot->nack_sent_micros < GRD_UDP_NACK_WINDOW_US) {
        return;
    }
    slot->nack_sent_micros = now;
    grd_udp_nack nack;
    memset(&nack, 0, sizeof(nack));
    nack.frame_id = slot->frame_id;
    nack.fragment_count = slot->fragment_count;
    nack.bitmap_bytes = (uint16_t)slot->bitmap_size;
    for (size_t byte = 0U; byte < slot->bitmap_size; ++byte) {
        nack.bitmap[byte] = (uint8_t)~slot->bitmap[byte];
    }
    if (slot->bitmap_size != 0U) {
        const unsigned trailing = (unsigned)slot->fragment_count % 8U;
        if (trailing != 0U) {
            nack.bitmap[slot->bitmap_size - 1U] &= (uint8_t)(
                0xFFU >> (8U - trailing)
            );
        }
    }
    /* Compact authenticated feedback on the UDP channel: no TCP
     * head-of-line blocking while the link is congested. */
    if (!connection_udp_send(
            connection, GRD_PACKET_UDP_NACK, &nack, sizeof(nack)
        )) {
        /* UDP feedback lost: fall back to the reliable channel. */
        (void)send_queue_push(
            &connection->send_queue,
            GRD_PACKET_UDP_NACK,
            &nack,
            sizeof(nack)
        );
    }
}

static bool udp_slot_waiting_expired(
    grd_udp_frame_slot *slot
)
{
    return slot != NULL && slot->active &&
           slot->nack_sent_micros != 0U &&
           grd_now_micros() - slot->nack_sent_micros >=
               GRD_UDP_NACK_WINDOW_US;
}

static void connection_request_keyframe(grd_connection *connection)
{
    if (connection == NULL || connection->callback == NULL) {
        return;
    }
    const uint64_t now = grd_now_micros();
    if (connection->udp_last_keyframe_request_micros != 0U &&
        now - connection->udp_last_keyframe_request_micros < 1000000U) {
        return;
    }
    connection->udp_last_keyframe_request_micros = now;
    ++connection->udp_keyframe_requests;
    connection->callback(
        GRD_PACKET_REQUEST_KEYFRAME,
        NULL,
        0U,
        false,
        connection->role,
        connection->userdata
    );
}

static void connection_report_udp_error(
    grd_connection *connection,
    const char *message
)
{
    if (connection == NULL || connection->callback == NULL || message == NULL) {
        return;
    }
    connection->callback(
        GRD_PACKET_ERROR,
        message,
        strlen(message) + 1U,
        false,
        connection->role,
        connection->userdata
    );
}

/* Drain complete access units strictly in frame order. Previously a newer
 * frame was handed to VideoToolbox while an older one waited for its NACK;
 * the late older frame was then decoded backwards, poisoning the H.264
 * reference chain precisely during short high-motion loss bursts. */
static void udp_slots_deliver_ready(grd_connection *connection)
{
    if (connection == NULL || connection->callback == NULL) {
        return;
    }
    for (;;) {
        grd_udp_frame_slot *oldest = NULL;
        for (size_t index = 0U; index < GRD_UDP_FRAME_SLOTS; ++index) {
            grd_udp_frame_slot *candidate = &connection->udp_slots[index];
            if (candidate->active &&
                (oldest == NULL || candidate->frame_id < oldest->frame_id)) {
                oldest = candidate;
            }
        }
        if (oldest == NULL) {
            return;
        }
        if (oldest->frame_id <= connection->udp_last_frame_delivered) {
            udp_frame_slot_reset(oldest);
            continue;
        }
        if (oldest->received_fragments != oldest->fragment_count) {
            return;
        }
        const uint64_t delivered_frame_id = oldest->frame_id;
        const bool consumed = connection->callback(
            GRD_PACKET_VIDEO_FRAME,
            oldest->data,
            oldest->frame_size,
            true,
            connection->role,
            connection->userdata
        );
        if (consumed) {
            oldest->data = NULL;
        }
        ++connection->udp_frames_received;
        connection->udp_last_frame_delivered = delivered_frame_id;
        udp_frame_slot_reset(oldest);
    }
}

/* XOR reconstruction: a block with exactly one missing fragment is rebuilt
 * from the block parity and all present fragments. Returns true when at
 * least one fragment was recovered (delivering the frame if it completes). */
static bool udp_slot_try_fec_recovery(
    grd_connection *connection,
    grd_udp_frame_slot *slot
)
{
    if (slot == NULL || !slot->active || slot->fec_parity == NULL ||
        slot->fec_received_bitmap == NULL) {
        return false;
    }
    bool recovered_any = false;
    for (uint16_t block = 0U; block < slot->fec_block_count; ++block) {
        /* A zeroed parity buffer is NOT a received parity: reconstruction
         * is only attempted for blocks whose parity datagram arrived,
         * otherwise a missing fragment would be "repaired" with zero bytes
         * and the frame delivered corrupted. */
        const size_t block_bit = (size_t)block / 8U;
        const uint8_t block_mask = (uint8_t)(1U << (block % 8U));
        if ((slot->fec_received_bitmap[block_bit] & block_mask) == 0U) {
            continue;
        }
        const uint16_t block_start =
            (uint16_t)(block * GRD_UDP_FEC_BLOCK_FRAGMENTS);
        const uint16_t block_end =
            block_start + GRD_UDP_FEC_BLOCK_FRAGMENTS;
        const uint16_t block_last =
            block_end < slot->fragment_count ? block_end : slot->fragment_count;
        uint16_t missing_index = block_start;
        uint16_t missing_count = 0U;
        for (uint16_t index = block_start; index < block_last; ++index) {
            const size_t byte = (size_t)index / 8U;
            const uint8_t bit = (uint8_t)(1U << (index % 8U));
            if ((slot->bitmap[byte] & bit) == 0U) {
                missing_index = index;
                ++missing_count;
            }
        }
        if (missing_count != 1U) {
            continue;
        }
        const uint8_t *parity =
            slot->fec_parity + (size_t)block * GRD_UDP_FRAGMENT_CAPACITY;
        uint8_t recovered[GRD_UDP_FRAGMENT_CAPACITY];
        memcpy(recovered, parity, GRD_UDP_FRAGMENT_CAPACITY);
        for (uint16_t index = block_start; index < block_last; ++index) {
            if (index == missing_index) {
                continue;
            }
            const size_t byte = (size_t)index / 8U;
            const uint8_t bit = (uint8_t)(1U << (index % 8U));
            if ((slot->bitmap[byte] & bit) == 0U) {
                continue;
            }
            const size_t offset =
                (size_t)index * GRD_UDP_FRAGMENT_CAPACITY;
            size_t length = slot->frame_size - offset;
            if (length > GRD_UDP_FRAGMENT_CAPACITY) {
                length = GRD_UDP_FRAGMENT_CAPACITY;
            }
            for (size_t byte_index = 0U;
                 byte_index < GRD_UDP_FRAGMENT_CAPACITY; ++byte_index) {
                recovered[byte_index] ^=
                    byte_index < length ? slot->data[offset + byte_index] : 0U;
            }
        }
        const size_t missing_offset =
            (size_t)missing_index * GRD_UDP_FRAGMENT_CAPACITY;
        size_t missing_length = slot->frame_size - missing_offset;
        if (missing_length > GRD_UDP_FRAGMENT_CAPACITY) {
            missing_length = GRD_UDP_FRAGMENT_CAPACITY;
        }
        memcpy(slot->data + missing_offset, recovered, missing_length);
        const size_t missing_byte = (size_t)missing_index / 8U;
        slot->bitmap[missing_byte] |=
            (uint8_t)(1U << (missing_index % 8U));
        ++slot->received_fragments;
        recovered_any = true;
    }
    if (recovered_any) {
        udp_slots_deliver_ready(connection);
    }
    return recovered_any;
}

/* Ensures a slot exists for frame_id, creating it from the frame header
 * carried by data or FEC packets. Returns NULL when the frame is stale
 * (a newer frame is already being assembled) or allocation failed. */
static grd_udp_frame_slot *udp_slot_ensure(
    grd_connection *connection,
    uint64_t frame_id,
    uint32_t frame_size,
    uint16_t fragment_count
)
{
    grd_udp_frame_slot *slot = udp_slot_for_frame(connection, frame_id);
    if (slot != NULL) {
        return slot;
    }
    if (frame_id <= connection->udp_last_frame_delivered) {
        return NULL;
    }
    /* A packet for an unknown (older) frame is stale once a newer frame
     * is already being assembled. */
    for (size_t index = 0U; index < GRD_UDP_FRAME_SLOTS; ++index) {
        if (connection->udp_slots[index].active &&
            connection->udp_slots[index].frame_id > frame_id) {
            return NULL;
        }
    }
    grd_udp_frame_slot *candidate = udp_slot_free(connection);
    if (candidate == NULL) {
        /* Four overlapping frames exhausted the bounded recovery window.
         * The oldest reference is gone, therefore every completed P-frame
         * behind it is unusable too. Discard the whole pending chain and ask
         * for one clean IDR instead of delivering dependants out of order. */
        ++connection->udp_frames_incomplete;
        connection_request_keyframe(connection);
        connection_udp_slots_reset(connection);
        candidate = udp_slot_free(connection);
    } else {
        /* A free slot means a previous frame may be waiting for its
         * missing fragments: request them instead of a full keyframe. */
        for (size_t index = 0U; index < GRD_UDP_FRAME_SLOTS; ++index) {
            grd_udp_frame_slot *other = &connection->udp_slots[index];
            if (other != candidate && other->active &&
                other->frame_id < frame_id &&
                other->received_fragments < other->fragment_count &&
                !udp_slot_waiting_expired(other)) {
                /* Try FEC recovery first: a single lost fragment in a
                 * block is repaired locally. A frame can still have two
                 * losses in another block, so suppress NACK only when FEC
                 * actually completed and delivered the whole frame. */
                (void)udp_slot_try_fec_recovery(connection, other);
                if (other->active &&
                    other->received_fragments < other->fragment_count) {
                    udp_slot_send_nack(connection, other);
                }
            }
        }
    }
    udp_slot_start(candidate, frame_id, frame_size, fragment_count);
    if (!candidate->active) {
        return NULL;
    }
    return candidate;
}

static void connection_udp_receive_fec(
    grd_connection *connection,
    const uint8_t *payload,
    size_t payload_length
)
{
    uint64_t frame_id = 0U;
    uint32_t frame_size = 0U;
    uint32_t offset = 0U;
    uint16_t fragment_index = 0U;
    uint16_t fragment_count = 0U;
    if (!udp_decode_fragment_header(
            payload,
            payload_length,
            &frame_id,
            &frame_size,
            &offset,
            &fragment_index,
            &fragment_count
        )) {
        return;
    }
    /* The sender always emits a full-capacity parity datagram; a shorter
     * packet is truncated/corrupt and must not be copied as parity. */
    if (payload_length !=
        (size_t)GRD_UDP_FRAGMENT_HEADER_SIZE + GRD_UDP_FRAGMENT_CAPACITY) {
        return;
    }
    /* Parity can arrive before the first data fragment: the FEC packet
     * carries the full frame header, so the slot is created from it. */
    grd_udp_frame_slot *slot = udp_slot_ensure(
        connection, frame_id, frame_size, fragment_count
    );
    if (slot == NULL || slot->fec_parity == NULL ||
        slot->fec_received_bitmap == NULL ||
        (size_t)fragment_index >= (size_t)slot->fec_block_count) {
        return;
    }
    const size_t block_bit = (size_t)fragment_index / 8U;
    const uint8_t block_mask =
        (uint8_t)(1U << (fragment_index % 8U));
    slot->fec_received_bitmap[block_bit] |= block_mask;
    uint8_t *parity =
        slot->fec_parity +
        (size_t)fragment_index * GRD_UDP_FRAGMENT_CAPACITY;
    memcpy(
        parity,
        payload + GRD_UDP_FRAGMENT_HEADER_SIZE,
        GRD_UDP_FRAGMENT_CAPACITY
    );
    (void)udp_slot_try_fec_recovery(connection, slot);
}

static void connection_udp_receive_frame(
    grd_connection *connection,
    const uint8_t *payload,
    size_t payload_length
)
{
    uint64_t frame_id = 0U;
    uint32_t frame_size = 0U;
    uint32_t offset = 0U;
    uint16_t fragment_index = 0U;
    uint16_t fragment_count = 0U;
    if (!udp_decode_fragment_header(
            payload,
            payload_length,
            &frame_id,
            &frame_size,
            &offset,
            &fragment_index,
            &fragment_count
        ) || frame_size > GRD_MAX_PACKET_SIZE ||
        (size_t)offset > (size_t)frame_size) {
        return;
    }
    const size_t chunk = payload_length - GRD_UDP_FRAGMENT_HEADER_SIZE;
    if (chunk > (size_t)frame_size - offset) {
        return;
    }
    grd_udp_frame_slot *slot = udp_slot_ensure(
        connection, frame_id, frame_size, fragment_count
    );
    if (slot == NULL) {
        return;
    }
    const size_t bitmap_byte = (size_t)fragment_index / 8U;
    const uint8_t bitmap_bit = (uint8_t)(1U << (fragment_index % 8U));
    if (bitmap_byte >= slot->bitmap_size ||
        (slot->bitmap[bitmap_byte] & bitmap_bit) != 0U) {
        return;
    }
    memcpy(
        slot->data + offset,
        payload + GRD_UDP_FRAGMENT_HEADER_SIZE,
        chunk
    );
    slot->bitmap[bitmap_byte] |= bitmap_bit;
    ++slot->received_fragments;
    udp_slots_deliver_ready(connection);
}

static bool connection_send_udp_probe(grd_connection *connection)
{
    if (connection == NULL || connection->udp_socket == GRD_INVALID_SOCKET ||
        !connection->udp_token_valid) {
        return false;
    }
    connection->udp_probe_sent_micros = grd_now_micros();
    connection->udp_probe_pending = true;
    /* Re-probe the existing authenticated UDP socket instead of tearing down
     * the TCP control session. The host accepts a fresh probe by token and
     * updates the peer address, clearing its transient udp_failed state. */
    return connection_udp_send(
        connection, GRD_UDP_PROBE_TYPE, NULL, 0U
    );
}

static void connection_maybe_report_udp_stats(grd_connection *connection)
{
    const uint64_t now = grd_now_micros();
    if (connection->udp_stats_window_start_micros == 0U) {
        connection->udp_stats_window_start_micros = now;
        return;
    }
    if (now - connection->udp_stats_window_start_micros < 100000ULL) {
        return;
    }
    uint64_t total_expected = connection->udp_stats_stall_expected;
    uint64_t total_lost = connection->udp_stats_stall_lost;
    if (connection->udp_stats_window_count != 0U &&
        connection->udp_stats_window_high >=
            connection->udp_stats_window_first) {
        const uint64_t expected =
            connection->udp_stats_window_high -
            connection->udp_stats_window_first + 1U;
        const uint64_t lost =
            expected > connection->udp_stats_window_count
                ? expected - connection->udp_stats_window_count
                : 0U;
        total_expected += expected;
        total_lost += lost;
    }
    uint32_t loss_percent = 0U;
    if (total_expected != 0U) {
        loss_percent = (uint32_t)(total_lost * 100ULL / total_expected);
    } else if (total_lost != 0U) {
        loss_percent = 100U;
    }
    connection->udp_lost_datagrams += total_lost;
    if (connection->media_channel) {
        const grd_bitrate_report report = {
            .bitrate_kbps = 0U,
            .loss_percent = loss_percent,
            .rtt_micros =
                connection->udp_rtt_micros > (uint64_t)UINT32_MAX
                    ? UINT32_MAX
                    : (uint32_t)connection->udp_rtt_micros
        };
        (void)send_queue_push(
            &connection->send_queue,
            GRD_PACKET_BITRATE_REPORT,
            &report,
            sizeof(report)
        );
    }
    connection->udp_stats_window_start_micros = now;
    connection->udp_stats_stall_expected = 0U;
    connection->udp_stats_stall_lost = 0U;
    connection->udp_stats_window_first = 0U;
    connection->udp_stats_window_high = 0U;
    connection->udp_stats_window_count = 0U;
}

static void connection_log_udp_stats(grd_connection *connection)
{
    const uint64_t now = grd_now_micros();
    if (connection->udp_log_window_start == 0U) {
        connection->udp_log_window_start = now;
        connection->udp_log_datagrams_at = connection->udp_datagrams_received;
        connection->udp_log_lost_at = connection->udp_lost_datagrams;
        connection->udp_log_frames_at = connection->udp_frames_received;
        connection->udp_log_incomplete_at = connection->udp_frames_incomplete;
        connection->udp_log_keyframes_at = connection->udp_keyframe_requests;
        connection->udp_log_reorder_at = connection->udp_receiver_reordered;
        connection->udp_log_dup_at = connection->udp_receiver_duplicate;
        connection->udp_log_decrypt_at = connection->udp_decrypt_failures;
        return;
    }
    if (now - connection->udp_log_window_start < 5000000ULL) {
        return;
    }
    const double seconds =
        (double)(now - connection->udp_log_window_start) / 1000000.0;
    const uint64_t datagrams =
        connection->udp_datagrams_received -
        connection->udp_log_datagrams_at;
    const uint64_t lost =
        connection->udp_lost_datagrams - connection->udp_log_lost_at;
    const uint64_t frames =
        connection->udp_frames_received - connection->udp_log_frames_at;
    const uint64_t incomplete =
        connection->udp_frames_incomplete -
        connection->udp_log_incomplete_at;
    const uint64_t keyframes =
        connection->udp_keyframe_requests -
        connection->udp_log_keyframes_at;
    const uint64_t reordered =
        connection->udp_receiver_reordered -
        connection->udp_log_reorder_at;
    const uint64_t duplicates =
        connection->udp_receiver_duplicate - connection->udp_log_dup_at;
    const uint64_t decrypt_failures =
        connection->udp_decrypt_failures - connection->udp_log_decrypt_at;
    const double jitter =
        connection->udp_jitter_samples != 0U
            ? (double)connection->udp_jitter_accum /
                  (double)connection->udp_jitter_samples
            : 0.0;
    const char *stability = "stable";
    if (lost > 0U || incomplete > 0U || jitter > 3000.0 ||
        frames < 30U) {
        stability = "unstable";
    } else if (jitter > 1500.0 || frames < 50U) {
        stability = "degraded";
    }
    const uint64_t last_arrival_age_ms =
        connection->udp_last_arrival_micros != 0U
            ? (now - connection->udp_last_arrival_micros) / 1000ULL
            : 0ULL;
    GRD_INFO(
        "client rx: %.1f pps, %llu lost, %.1f frames/s, %llu incomplete, "
        "%llu keyframe req, reorder %llu, dup %llu, bad %llu, "
        "jitter %.1f us, state %s, udp_last %llu ms",
        (double)datagrams / seconds,
        lost,
        (double)frames / seconds,
        incomplete,
        keyframes,
        reordered,
        duplicates,
        decrypt_failures,
        jitter,
        stability,
        last_arrival_age_ms
    );
    connection->udp_log_window_start = now;
    connection->udp_log_datagrams_at = connection->udp_datagrams_received;
    connection->udp_log_lost_at = connection->udp_lost_datagrams;
    connection->udp_log_frames_at = connection->udp_frames_received;
    connection->udp_log_incomplete_at = connection->udp_frames_incomplete;
    connection->udp_log_keyframes_at = connection->udp_keyframe_requests;
    connection->udp_log_reorder_at = connection->udp_receiver_reordered;
    connection->udp_log_dup_at = connection->udp_receiver_duplicate;
    connection->udp_log_decrypt_at = connection->udp_decrypt_failures;
    connection->udp_jitter_accum = 0U;
    connection->udp_jitter_samples = 0U;
}

#if defined(_WIN32)
static DWORD WINAPI connection_udp_thread(void *argument)
#else
static void *connection_udp_thread(void *argument)
#endif
{
    grd_connection *connection = argument;
    uint8_t wire[GRD_UDP_MAX_DATAGRAM];
    uint8_t plain[GRD_UDP_MAX_DATAGRAM];
    unsigned timeout_count = 0U;
    bool error_reported = false;
    bool recovery_pending = false;
    uint64_t last_probe_micros = 0U;
    while (connection_is_running(connection)) {
        atomic_store_explicit(
            &connection->udp_thread_last_active,
            grd_now_micros(),
            memory_order_relaxed
        );
        const int received = (int)recv(
            connection->udp_socket,
            (char *)wire,
            sizeof(wire),
            0
        );
        if (received <= 0) {
            const uint64_t now = grd_now_micros();
            if (++timeout_count >= 4U) {
                timeout_count = 0U;
                atomic_store_explicit(
                    &connection->udp_enabled, false, memory_order_release
                );
                connection_udp_slots_reset(connection);
                recovery_pending = true;
                if (!error_reported && connection_is_running(connection)) {
                    connection_report_udp_error(
                        connection, "UDP video reception was interrupted; recovering"
                    );
                    error_reported = true;
                }
                /* The decoder must not keep displaying concealed P-frames
                 * after the gap. The request uses the still-live TCP control
                 * channel and is throttled internally. */
                connection_request_keyframe(connection);
            }
            if (connection_is_running(connection) &&
                !atomic_load_explicit(
                    &connection->udp_enabled, memory_order_acquire
                ) &&
                (last_probe_micros == 0U ||
                 now - last_probe_micros >= 500000ULL)) {
                if (connection_send_udp_probe(connection)) {
                    last_probe_micros = now;
                }
            }
            ++connection->udp_stats_stall_expected;
            ++connection->udp_stats_stall_lost;
            connection_maybe_report_udp_stats(connection);
            connection_log_udp_stats(connection);
            continue;
        }
        timeout_count = 0U;
        uint16_t type = 0U;
        size_t plain_size = 0U;
        uint64_t accepted_sequence = 0U;
        bool datagram_reordered = false;
        bool datagram_duplicate = false;
        if (!udp_decrypt(
                wire,
                (size_t)received,
                connection->crypto.rx_key,
                &connection->udp_rx_window,
                connection->udp_token,
                &type,
                &accepted_sequence,
                plain,
                sizeof(plain),
                &plain_size,
                &datagram_reordered,
                &datagram_duplicate
            )) {
            if (datagram_duplicate) {
                ++connection->udp_receiver_duplicate;
            } else {
                /* Header/token/AEAD rejection: previously completely silent,
                 * so a wrong key or token looked like "no loss" while the
                 * screen stayed black. */
                ++connection->udp_decrypt_failures;
            }
            continue;
        }
        if (datagram_reordered) {
            ++connection->udp_receiver_reordered;
        }
        if (connection->udp_stats_window_first == 0U) {
            connection->udp_stats_window_first = accepted_sequence;
        }
        if (accepted_sequence > connection->udp_stats_window_high) {
            connection->udp_stats_window_high = accepted_sequence;
        }
        ++connection->udp_stats_window_count;
        ++connection->udp_datagrams_received;
        const uint64_t arrival_now = grd_now_micros();
        if (connection->udp_last_arrival_micros != 0U) {
            const uint64_t delta =
                arrival_now - connection->udp_last_arrival_micros;
            if (connection->udp_last_delta_micros != 0U) {
                const uint64_t delta_jitter =
                    delta > connection->udp_last_delta_micros
                        ? delta - connection->udp_last_delta_micros
                        : connection->udp_last_delta_micros - delta;
                connection->udp_jitter_accum += delta_jitter;
                ++connection->udp_jitter_samples;
            }
            connection->udp_last_delta_micros = delta;
        }
        connection->udp_last_arrival_micros = arrival_now;
        if (type == GRD_UDP_ACK_TYPE && plain_size == 0U) {
            if (connection->udp_probe_pending) {
                connection->udp_rtt_micros =
                    grd_now_micros() - connection->udp_probe_sent_micros;
                connection->udp_probe_pending = false;
            }
            atomic_store_explicit(&connection->udp_enabled, true, memory_order_release);
            timeout_count = 0U;
            error_reported = false;
            if (recovery_pending) {
                recovery_pending = false;
                connection_request_keyframe(connection);
            }
        } else if (type == GRD_PACKET_VIDEO_FRAME) {
            error_reported = false;
            atomic_store_explicit(&connection->udp_enabled, true, memory_order_release);
            if (recovery_pending) {
                recovery_pending = false;
                connection_request_keyframe(connection);
            }
            connection_udp_receive_frame(connection, plain, plain_size);
        } else if (type == GRD_PACKET_VIDEO_FEC) {
            error_reported = false;
            atomic_store_explicit(&connection->udp_enabled, true, memory_order_release);
            connection_udp_receive_fec(connection, plain, plain_size);
        } else if (type == GRD_PACKET_AUDIO_FRAME) {
            error_reported = false;
            atomic_store_explicit(&connection->udp_enabled, true, memory_order_release);
            if (recovery_pending) {
                recovery_pending = false;
                connection_request_keyframe(connection);
            }
            if (connection->callback != NULL) {
                /* plain is a stack buffer, so the callback must copy it. */
                (void)connection->callback(
                    GRD_PACKET_AUDIO_FRAME,
                    plain,
                    plain_size,
                    false,
                    connection->role,
                    connection->userdata
                );
            }
        }
        connection_maybe_report_udp_stats(connection);
        connection_log_udp_stats(connection);
    }
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

static bool connection_start_udp(
    grd_connection *connection,
    const char *address,
    uint16_t port,
    grd_error *error
)
{
    if (connection == NULL || !connection->media_channel ||
        !connection->udp_token_valid) {
        if (error != NULL) {
            error->code = GRD_PROTOCOL_ERROR;
            (void)snprintf(
                error->message, sizeof(error->message),
                "UDP video token is missing"
            );
        }
        return false;
    }
    char service[16];
    (void)snprintf(service, sizeof(service), "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *results = NULL;
    if (getaddrinfo(address, service, &hints, &results) != 0) {
        if (error != NULL) {
            error->code = GRD_IO_ERROR;
            (void)snprintf(
                error->message, sizeof(error->message),
                "Failed to resolve UDP video endpoint"
            );
        }
        return false;
    }
    bool started = false;
    for (struct addrinfo *item = results; item != NULL; item = item->ai_next) {
        const grd_socket socket_value = socket(
            item->ai_family, item->ai_socktype, item->ai_protocol
        );
        if (socket_value == GRD_INVALID_SOCKET ||
            connect(socket_value, item->ai_addr, (grd_socklen)item->ai_addrlen) != 0) {
            if (socket_value != GRD_INVALID_SOCKET) {
                grd_socket_close(socket_value);
            }
            continue;
        }
        int receive_buffer = 1024 * 1024;
        (void)setsockopt(
            socket_value,
            SOL_SOCKET,
            SO_RCVBUF,
            (const char *)&receive_buffer,
            sizeof(receive_buffer)
        );
        if (!grd_socket_set_timeout(socket_value, 500U)) {
            grd_socket_close(socket_value);
            continue;
        }
        if (!udp_encrypt_send(
                socket_value,
                NULL,
                0,
                connection->crypto.tx_key,
                &connection->udp_tx_sequence,
                connection->udp_token,
                GRD_UDP_PROBE_TYPE,
                NULL,
                0U
            )) {
            grd_socket_close(socket_value);
            continue;
        }
        uint8_t wire[GRD_UDP_MAX_DATAGRAM];
        uint8_t plain[GRD_UDP_MAX_DATAGRAM];
        const int received = (int)recv(
            socket_value, (char *)wire, sizeof(wire), 0
        );
        uint16_t type = 0U;
        size_t plain_size = 0U;
        grd_udp_reorder_window handshake_window;
        memset(&handshake_window, 0, sizeof(handshake_window));
        uint64_t rx_sequence = 0U;
        if (received <= 0 ||
            !udp_decrypt(
                wire,
                (size_t)received,
                connection->crypto.rx_key,
                &handshake_window,
                connection->udp_token,
                &type,
                &rx_sequence,
                plain,
                sizeof(plain),
                &plain_size,
                NULL,
                NULL
            ) ||
            type != GRD_UDP_ACK_TYPE || plain_size != 0U) {
            grd_socket_close(socket_value);
            continue;
        }
        connection->udp_socket = socket_value;
        connection->udp_rx_window = handshake_window;
#if defined(_WIN32)
        connection->udp_thread = CreateThread(
            NULL, 0U, connection_udp_thread, connection, 0U, NULL
        );
        connection->udp_thread_started = connection->udp_thread != NULL;
#else
        connection->udp_thread_started = pthread_create(
            &connection->udp_thread, NULL, connection_udp_thread, connection
        ) == 0;
#endif
        if (!connection->udp_thread_started) {
            grd_socket_close(socket_value);
            connection->udp_socket = GRD_INVALID_SOCKET;
            atomic_store_explicit(&connection->udp_enabled, false, memory_order_release);
            break;
        }
        atomic_store_explicit(&connection->udp_enabled, true, memory_order_release);
        started = true;
        break;
    }
    freeaddrinfo(results);
    if (!started && error != NULL) {
        error->code = GRD_IO_ERROR;
        (void)snprintf(
            error->message, sizeof(error->message),
            "UDP video handshake failed"
        );
    }
    return started;
}

#if defined(_WIN32)
static DWORD WINAPI connection_receive_thread(void *argument)
#else
static void *connection_receive_thread(void *argument)
#endif
{
    grd_connection *connection = argument;
    while (connection_is_running(connection)) {
        atomic_store_explicit(
            &connection->tcp_rx_thread_last_active,
            grd_now_micros(),
            memory_order_relaxed
        );
        grd_packet_header header;
        uint8_t *payload = NULL;
        size_t payload_length = 0U;
        if (!receive_secure_packet(
                connection->socket_value, &connection->crypto,
                &header, &payload, &payload_length
            )) {
            break;
        }
        if (connection->callback != NULL) {
            const bool consumed = connection->callback(
                (grd_packet_type)header.type,
                payload,
                payload_length,
                payload != NULL,
                connection->role,
                connection->userdata
            );
            if (consumed) {
                /* The callback owns the buffer now; the next receive
                 * allocates a fresh one instead of reusing it. */
                connection->crypto.rx_plain = NULL;
                connection->crypto.rx_plain_capacity = 0U;
            }
        }
    }
    connection_set_running(connection, false);
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

static grd_connection *connect_channel(
    const char *address,
    uint16_t port,
    const char *password,
    grd_role requested_role,
    const grd_config *local_config,
    grd_packet_callback callback,
    void *userdata,
    grd_error *error,
    bool media_channel,
    bool initialize_network
)
{
    if (address == NULL || password == NULL || local_config == NULL ||
        (initialize_network && !grd_net_initialize())) {
        return NULL;
    }
    char service[16];
    (void)snprintf(service, sizeof(service), "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *results = NULL;
    if (getaddrinfo(address, service, &hints, &results) != 0) {
        if (initialize_network) {
            grd_net_shutdown();
        }
        if (error != NULL) {
            error->code = GRD_IO_ERROR;
            (void)snprintf(
                error->message, sizeof(error->message),
                "Failed to resolve address %s", address
            );
        }
        return NULL;
    }
    grd_socket socket_value = GRD_INVALID_SOCKET;
    for (struct addrinfo *item = results; item != NULL; item = item->ai_next) {
        socket_value = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (socket_value != GRD_INVALID_SOCKET &&
            connect(socket_value, item->ai_addr, item->ai_addrlen) == 0) {
            break;
        }
        if (socket_value != GRD_INVALID_SOCKET) {
            grd_socket_close(socket_value);
        }
        socket_value = GRD_INVALID_SOCKET;
    }
    freeaddrinfo(results);
    if (socket_value == GRD_INVALID_SOCKET) {
        if (initialize_network) {
            grd_net_shutdown();
        }
        if (error != NULL) {
            error->code = GRD_IO_ERROR;
            (void)snprintf(error->message, sizeof(error->message), "Connection to %s failed", address);
        }
        return NULL;
    }
    (void)grd_socket_set_low_latency(socket_value);
    (void)grd_socket_set_timeout(socket_value, 5000U);
    grd_connection *connection = calloc(1U, sizeof(*connection));
    if (connection == NULL) {
        grd_socket_close(socket_value);
        if (initialize_network) {
            grd_net_shutdown();
        }
        return NULL;
    }
    connection->socket_value = socket_value;
    connection->udp_socket = GRD_INVALID_SOCKET;
    connection->callback = callback;
    connection->userdata = userdata;
    connection->owns_network = initialize_network;
    grd_mutex_init(&connection->send_mutex);
    grd_mutex_init(&connection->udp_send_mutex);
    send_queue_init(&connection->send_queue);
    if (!client_handshake(
            connection, password, requested_role, local_config, media_channel
        )) {
        grd_connection_close(connection);
        if (error != NULL) {
            error->code = GRD_AUTH_FAILED;
            (void)snprintf(error->message, sizeof(error->message), "Authentication failed");
        }
        return NULL;
    }
    connection_set_running(connection, true);
    (void)grd_socket_set_timeout(socket_value, 0U);
    if (media_channel && !connection_start_udp(connection, address, port, error)) {
        connection_set_running(connection, false);
        grd_connection_close(connection);
        return NULL;
    }
#if defined(_WIN32)
    connection->send_thread = CreateThread(
        NULL, 0U, connection_send_thread, connection, 0U, NULL
    );
    if (connection->send_thread == NULL) {
        connection_set_running(connection, false);
        grd_connection_close(connection);
        return NULL;
    }
    connection->send_thread_started = true;
    connection->receive_thread = CreateThread(
        NULL, 0U, connection_receive_thread, connection, 0U, NULL
    );
    if (connection->receive_thread == NULL) {
#else
    if (pthread_create(
            &connection->send_thread, NULL,
            connection_send_thread, connection
        ) != 0) {
        connection_set_running(connection, false);
        grd_connection_close(connection);
        return NULL;
    }
    connection->send_thread_started = true;
    if (pthread_create(
            &connection->receive_thread, NULL,
            connection_receive_thread, connection
        ) != 0) {
#endif
        connection_set_running(connection, false);
        grd_connection_close(connection);
        return NULL;
    }
    connection->receive_thread_started = true;
    return connection;
}

grd_connection *grd_connect(
    const char *address,
    uint16_t port,
    const char *password,
    grd_role requested_role,
    const grd_config *local_config,
    grd_packet_callback callback,
    void *userdata,
    grd_error *error
)
{
    return connect_channel(
        address, port, password, requested_role, local_config,
        callback, userdata, error, false, true
    );
}

grd_connection *grd_connect_media(
    const char *address,
    uint16_t port,
    const char *password,
    const grd_config *local_config,
    grd_packet_callback callback,
    void *userdata,
    grd_error *error
)
{
    return connect_channel(
        address, port, password, GRD_ROLE_OBSERVER, local_config,
        callback, userdata, error, true, false
    );
}

grd_status grd_connection_send(
    grd_connection *connection,
    grd_packet_type type,
    const void *payload,
    size_t payload_length,
    grd_error *error
)
{
    (void)error;
    if (connection == NULL || !connection_is_running(connection)) {
        return GRD_INVALID_ARGUMENT;
    }
    const grd_status status = send_queue_push(
        &connection->send_queue, type, payload, payload_length
    );
    if (status != GRD_OK && error != NULL) {
        error->code = status;
        (void)snprintf(
            error->message,
            sizeof(error->message),
            "Control-channel queue is full or unavailable"
        );
    }
    return status;
}

grd_status grd_connection_send_realtime_input(
    grd_connection *connection,
    const grd_input_event *event,
    grd_error *error
)
{
    if (connection == NULL || event == NULL ||
        event->kind != GRD_INPUT_POINTER_RELATIVE ||
        grd_protocol_validate_input(event) != GRD_OK ||
        !connection_is_running(connection) ||
        !atomic_load_explicit(
            &connection->udp_enabled, memory_order_acquire
        )) {
        if (error != NULL) {
            error->code = GRD_INVALID_ARGUMENT;
            (void)snprintf(
                error->message,
                sizeof(error->message),
                "Real-time UDP input channel is unavailable"
            );
        }
        return GRD_INVALID_ARGUMENT;
    }
    if (!connection_udp_send(
            connection,
            GRD_PACKET_INPUT,
            event,
            sizeof(*event)
        )) {
        if (error != NULL) {
            error->code = GRD_IO_ERROR;
            (void)snprintf(
                error->message,
                sizeof(error->message),
                "Failed to send UDP mouse movement"
            );
        }
        return GRD_IO_ERROR;
    }
    return GRD_OK;
}

bool grd_connection_is_active(const grd_connection *connection)
{
    return connection != NULL &&
           connection_is_running((grd_connection *)connection);
}

bool grd_connection_video_udp_active(const grd_connection *connection)
{
    return connection != NULL &&
           atomic_load_explicit(&connection->udp_enabled, memory_order_acquire) &&
           connection->udp_thread_started;
}

void grd_connection_thread_health(
    const grd_connection *connection,
    uint64_t *udp_thread_age_ms,
    uint64_t *tcp_rx_thread_age_ms,
    uint64_t *tcp_tx_thread_age_ms
)
{
    if (connection == NULL) {
        if (udp_thread_age_ms != NULL) *udp_thread_age_ms = 0ULL;
        if (tcp_rx_thread_age_ms != NULL) *tcp_rx_thread_age_ms = 0ULL;
        if (tcp_tx_thread_age_ms != NULL) *tcp_tx_thread_age_ms = 0ULL;
        return;
    }
    const uint64_t now = grd_now_micros();
    const uint64_t udp_last = atomic_load_explicit(
        &connection->udp_thread_last_active, memory_order_relaxed
    );
    const uint64_t rx_last = atomic_load_explicit(
        &connection->tcp_rx_thread_last_active, memory_order_relaxed
    );
    const uint64_t tx_last = atomic_load_explicit(
        &connection->tcp_tx_thread_last_active, memory_order_relaxed
    );
    if (udp_thread_age_ms != NULL) {
        *udp_thread_age_ms =
            udp_last != 0ULL ? (now - udp_last) / 1000ULL : 0ULL;
    }
    if (tcp_rx_thread_age_ms != NULL) {
        *tcp_rx_thread_age_ms =
            rx_last != 0ULL ? (now - rx_last) / 1000ULL : 0ULL;
    }
    if (tcp_tx_thread_age_ms != NULL) {
        *tcp_tx_thread_age_ms =
            tx_last != 0ULL ? (now - tx_last) / 1000ULL : 0ULL;
    }
}

grd_role grd_connection_role(const grd_connection *connection)
{
    return connection != NULL ? connection->role : GRD_ROLE_OBSERVER;
}

void grd_connection_close(grd_connection *connection)
{
    if (connection == NULL) {
        return;
    }
    connection_set_running(connection, false);
    if (connection->udp_socket != GRD_INVALID_SOCKET) {
        grd_socket_shutdown(connection->udp_socket);
    }
    if (connection->udp_thread_started) {
        grd_thread_join(connection->udp_thread);
        connection->udp_thread_started = false;
    }
    if (connection->udp_socket != GRD_INVALID_SOCKET) {
        grd_socket_close(connection->udp_socket);
        connection->udp_socket = GRD_INVALID_SOCKET;
    }
    connection_udp_slots_reset(connection);
    send_queue_stop(&connection->send_queue);
    if (connection->socket_value != GRD_INVALID_SOCKET) {
        grd_socket_shutdown(connection->socket_value);
    }
    if (connection->receive_thread_started) {
        grd_thread_join(connection->receive_thread);
        connection->receive_thread_started = false;
    }
    if (connection->send_thread_started) {
        grd_thread_join(connection->send_thread);
        connection->send_thread_started = false;
    }
    if (connection->socket_value != GRD_INVALID_SOCKET) {
        grd_socket_close(connection->socket_value);
    }
    send_queue_destroy(&connection->send_queue);
    grd_mutex_destroy(&connection->send_mutex);
    grd_mutex_destroy(&connection->udp_send_mutex);
    if (connection->owns_network) {
        grd_net_shutdown();
    }
    crypto_destroy(&connection->crypto);
    grd_secure_zero(connection, sizeof(*connection));
    free(connection);
}
