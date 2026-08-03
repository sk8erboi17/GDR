#ifndef GRD_TRANSPORT_H
#define GRD_TRANSPORT_H

#include "grd/auth.h"
#include "grd/common.h"
#include "grd/config.h"
#include "grd/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grd_host grd_host;
typedef struct grd_connection grd_connection;

/* Returns true when the callback took ownership of payload (only valid when
 * payload_takeable is true); the transport then detaches its buffer and never
 * frees it. */
typedef bool (*grd_packet_callback)(
    grd_packet_type type,
    const void *payload,
    size_t payload_length,
    bool payload_takeable,
    grd_role role,
    void *userdata
);

#define GRD_BROADCAST_MAX_PARTS 2U
#define GRD_BROADCAST_MAX_INLINE_PREFIX 64U

/* Non-contiguous payload view. The last part is the referenced owned buffer;
 * the preceding parts are a small inline prefix copied by the transport. */
typedef struct grd_buf_part {
    const uint8_t *data;
    size_t length;
} grd_buf_part;

grd_host *grd_host_start(
    const grd_config *config,
    grd_packet_callback callback,
    void *userdata,
    grd_error *error
);
grd_status grd_host_broadcast(
    grd_host *host,
    grd_packet_type type,
    const void *payload,
    size_t payload_length,
    grd_error *error
);
/* Broadcasts a frame assembled from parts without copying the payload.
 * Ownership of payload_ref transfers to the transport (it is released after
 * the call, once per cloned consumer); the caller must not touch it again. */
grd_status grd_host_broadcast_parts(
    grd_host *host,
    grd_packet_type type,
    const grd_buf_part *parts,
    size_t part_count,
    grd_owned_buffer *payload_ref,
    bool keyframe,
    grd_error *error
);
size_t grd_host_client_count(const grd_host *host);
size_t grd_host_udp_video_client_count(const grd_host *host);
/* Sets the video stream parameters used by the UDP pacer: pacing rate in
 * bits per second and the frame period in microseconds. */
void grd_host_set_stream_params(
    grd_host *host,
    uint32_t bits_per_second,
    uint32_t frame_period_us
);
/* Enables/disables XOR FEC parity fragments on the video stream. */
void grd_host_set_fec_enabled(grd_host *host, bool enabled);
/* Smoothed RTT reported by the client, used by the NACK handler to size
 * retransmission deadlines (a fixed budget expires while the pacer still
 * carries temporal debt). */
void grd_host_set_rtt_us(grd_host *host, uint32_t rtt_micros);
/* Current UDP pacing rate enforced by the per-client pacer (bits/s), used
 * by the host stats line to show what the network layer is actually doing
 * independently of the encoder configuration. */
uint32_t grd_host_pacing_bits_per_second(const grd_host *host);
/* Recent (1 s rolling) initiating local-pressure percentage. Only admission,
 * queue, deadline and socket-send drops are included; recovery purges and
 * discontinuity gating remain in telemetry but cannot recursively drive ABR. */
uint32_t grd_host_udp_initiating_drop_percent(const grd_host *host);
/* Returns the same one-second local-drop sample together with a monotonically
 * increasing generation. Consumers can therefore process each window once
 * instead of mistaking repeated reads for persistent pressure. */
uint32_t grd_host_udp_initiating_drop_sample(
    const grd_host *host,
    uint64_t *generation
);
/* Compatibility alias for the initiating-pressure metric above. */
uint32_t grd_host_udp_drop_percent(const grd_host *host);
/* Drops stale queued video/FEC work, clears pacing debt and requests one
 * fresh IDR. Used when desktop capture resumes after a visible source gap so
 * expired pre-gap frames cannot start a repeated pacer/IDR recovery loop. */
void grd_host_resynchronize_video(grd_host *host);
/* True while at least one active media client is transmitting a queued
 * repair IDR. The producer briefly pauses before encoding the following
 * P-frame so no hidden reference is created and then discarded by the pacer. */
bool grd_host_video_recovery_queued(grd_host *host);
/* Returns and clears the pending "new UDP client needs a keyframe" flag set
 * by the transport when a media client becomes ready for video. */
bool grd_host_take_keyframe_pending(grd_host *host);
void grd_host_stop(grd_host *host);

grd_connection *grd_connect(
    const char *address,
    uint16_t port,
    const char *password,
    grd_role requested_role,
    const grd_config *local_config,
    grd_packet_callback callback,
    void *userdata,
    grd_error *error
);
/* Opens the media half of an already authenticated logical session. The
 * channel performs its own authenticated handshake and only carries media
 * packets; input/control remains on grd_connect(). */
grd_connection *grd_connect_media(
    const char *address,
    uint16_t port,
    const char *password,
    const grd_config *local_config,
    grd_packet_callback callback,
    void *userdata,
    grd_error *error
);
grd_status grd_connection_send(
    grd_connection *connection,
    grd_packet_type type,
    const void *payload,
    size_t payload_length,
    grd_error *error
);
/* Sends relative pointer motion on the authenticated media UDP channel.
 * Buttons, keys and scroll remain on grd_connection_send() so actions that
 * must never be lost retain reliable ordering. */
grd_status grd_connection_send_realtime_input(
    grd_connection *connection,
    const grd_input_event *event,
    grd_error *error
);
bool grd_connection_is_active(const grd_connection *connection);
/* True when the media connection has an authenticated UDP video side channel. */
bool grd_connection_video_udp_active(const grd_connection *connection);
grd_role grd_connection_role(const grd_connection *connection);
/* Thread-map ages (ms since last activity) for the connection's UDP
 * receive, TCP receive and TCP send threads. A stalled thread's age grows;
 * the UI marks it STALLED in the periodic thread map. */
void grd_connection_thread_health(
    const grd_connection *connection,
    uint64_t *udp_thread_age_ms,
    uint64_t *tcp_rx_thread_age_ms,
    uint64_t *tcp_tx_thread_age_ms
);
void grd_connection_close(grd_connection *connection);

#ifdef __cplusplus
}
#endif

#endif
