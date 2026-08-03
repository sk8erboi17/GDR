#ifndef GRD_PROTOCOL_H
#define GRD_PROTOCOL_H

#include "grd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRD_PACKET_MAGIC 0x47524431U
#define GRD_MAX_PACKET_SIZE (16U * 1024U * 1024U)

typedef enum grd_packet_type {
    GRD_PACKET_HELLO = 1,
    GRD_PACKET_AUTH_CHALLENGE = 2,
    GRD_PACKET_AUTH_RESPONSE = 3,
    GRD_PACKET_AUTH_RESULT = 4,
    GRD_PACKET_VIDEO_CONFIG = 10,
    GRD_PACKET_VIDEO_FRAME = 11,
    GRD_PACKET_AUDIO_CONFIG = 12,
    GRD_PACKET_AUDIO_FRAME = 13,
    GRD_PACKET_INPUT = 20,
    GRD_PACKET_CLIPBOARD = 21,
    GRD_PACKET_CURSOR = 23,
    GRD_PACKET_CURSOR_SHAPE = 24,
    GRD_PACKET_PING = 30,
    GRD_PACKET_ERROR = 32,
    GRD_PACKET_DISPLAY_CAPS = 33,
    GRD_PACKET_REQUEST_KEYFRAME = 34,
    GRD_PACKET_VIDEO_UDP_TOKEN = 35,
    GRD_PACKET_BITRATE_REPORT = 36,
    GRD_PACKET_UDP_NACK = 37,
    GRD_PACKET_VIDEO_FEC = 38,
    GRD_PACKET_VIDEO_CAPS = 39
} grd_packet_type;

typedef struct grd_packet_header {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t payload_length;
    uint64_t sequence;
} grd_packet_header;

typedef struct grd_hello {
    uint8_t requested_role;
    uint8_t operating_system;
    uint16_t reserved;
    char device_id[37];
    char device_name[GRD_MAX_DEVICE_NAME];
} grd_hello;

#define GRD_HELLO_CHANNEL_CONTROL 0U
#define GRD_HELLO_CHANNEL_MEDIA 1U

typedef struct grd_video_config {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t bitrate_kbps;
    char codec[16];
    char pipeline[64];
} grd_video_config;

/* Client -> host codec capability advertisement (protocol v3): the host
 * only starts a non-H.264 stream after the client announced it can decode
 * it, so the decoder never has to "fall back" on a bitstream it cannot
 * parse. */
typedef struct grd_video_caps {
    uint32_t codec_bitmask;
} grd_video_caps;

#define GRD_VIDEO_CAPS_H264 (1U << 0U)
#define GRD_VIDEO_CAPS_HEVC (1U << 1U)
#define GRD_VIDEO_CAPS_AV1  (1U << 2U)

typedef struct grd_audio_config {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t frame_samples;
    uint32_t bitrate_kbps;
    char codec[16];
} grd_audio_config;

typedef enum grd_input_kind {
    GRD_INPUT_POINTER_MOVE = 0,
    GRD_INPUT_POINTER_BUTTON = 1,
    GRD_INPUT_SCROLL = 2,
    GRD_INPUT_KEY = 3,
    GRD_INPUT_TEXT = 4,
    GRD_INPUT_POINTER_RELATIVE = 5
} grd_input_kind;

/*
 * I codici tastiera sul filo usano le USB HID Usage ID, equivalenti agli
 * SDL_Scancode for the Keyboard/Keypad page. This makes them independent
 * of the client's keyboard layout and operating system.
 */
enum {
    GRD_KEY_LEFT_CTRL = 224,
    GRD_KEY_LEFT_SHIFT = 225,
    GRD_KEY_LEFT_ALT = 226,
    GRD_KEY_LEFT_GUI = 227,
    GRD_KEY_RIGHT_CTRL = 228,
    GRD_KEY_RIGHT_SHIFT = 229,
    GRD_KEY_RIGHT_ALT = 230,
    GRD_KEY_RIGHT_GUI = 231
};

typedef struct grd_input_event {
    uint8_t kind;
    uint8_t pressed;
    uint16_t modifiers;
    uint32_t code;
    float x;
    float y;
    int32_t delta_x;
    int32_t delta_y;
    uint32_t text_length;
    char text[32];
} grd_input_event;

typedef struct grd_cursor_state {
    float x;
    float y;
    uint8_t visible;
    uint8_t reserved[3];
} grd_cursor_state;

#define GRD_CURSOR_MAX_WIDTH 64U
#define GRD_CURSOR_MAX_HEIGHT 64U

typedef struct grd_cursor_shape {
    uint16_t width;
    uint16_t height;
    int16_t hotspot_x;
    int16_t hotspot_y;
    uint8_t pixels[GRD_CURSOR_MAX_WIDTH * GRD_CURSOR_MAX_HEIGHT * 4U];
} grd_cursor_shape;

/* Capabilities of the receiving display. A ProMotion client can request a
 * 120 Hz stream; the host still clamps this to a safe range and may fall
 * back to its configured frame rate. */
typedef struct grd_display_caps {
    uint32_t max_fps;
    uint32_t width;
    uint32_t height;
    uint8_t high_refresh;
    uint8_t upscale_mode;
    uint8_t offload_flags;
    uint8_t reserved;
} grd_display_caps;

/* These bits describe work that the receiver intentionally performs. They
 * are informative to older hosts (the bytes used to be reserved) and make
 * diagnostics explicit without changing sizeof(grd_display_caps). */
#define GRD_CLIENT_OFFLOAD_FRAME_PACING      (1U << 0U)
#define GRD_CLIENT_OFFLOAD_CURSOR_PREDICTION (1U << 1U)
#define GRD_CLIENT_OFFLOAD_SHARP_SCALING     (1U << 2U)
#define GRD_CLIENT_OFFLOAD_KNOWN_MASK \
    (GRD_CLIENT_OFFLOAD_FRAME_PACING | \
     GRD_CLIENT_OFFLOAD_CURSOR_PREDICTION | \
     GRD_CLIENT_OFFLOAD_SHARP_SCALING)

#define GRD_VIDEO_UDP_TOKEN_BYTES 16U

/* Sent once on the authenticated media TCP channel. The token is presented
 * in clear in the UDP datagram header only to route it to the right session;
 * the datagram payload is still authenticated and encrypted with the
 * per-session XChaCha20 key. */
typedef struct grd_video_udp_token {
    uint16_t port;
    uint16_t reserved;
    uint8_t token[GRD_VIDEO_UDP_TOKEN_BYTES];
} grd_video_udp_token;

/* Periodic media-quality feedback sent by the client over the reliable
 * channel. bitrate_kbps is informational (the host owns the adaptation
 * policy); loss_percent is the measured UDP datagram loss in the last
 * reporting window. */
typedef struct grd_bitrate_report {
    uint32_t bitrate_kbps;
    uint32_t loss_percent;
    uint32_t rtt_micros;
} grd_bitrate_report;

/* Selective retransmission request: the client reports which fragments of a
 * UDP video frame are missing so the host can re-send only those instead of
 * forcing a full keyframe. bitmap has one bit per fragment (1 = missing). */
#define GRD_UDP_NACK_MAX_FRAGMENTS 2048U

typedef struct grd_udp_nack {
    uint64_t frame_id;
    uint16_t fragment_count;
    uint16_t bitmap_bytes;
    uint8_t bitmap[GRD_UDP_NACK_MAX_FRAGMENTS / 8U];
} grd_udp_nack;

/* XOR forward error correction: one parity fragment per block of data
 * fragments recovers any single loss inside the block. Block size and the
 * maximum number of protected blocks bound the client-side parity buffers. */
#define GRD_UDP_FEC_BLOCK_FRAGMENTS 16U
#define GRD_UDP_FEC_MAX_BLOCKS 64U

void grd_protocol_encode_header(
    const grd_packet_header *header,
    uint8_t output[20]
);
grd_status grd_protocol_decode_header(
    const uint8_t input[20],
    grd_packet_header *header
);
grd_status grd_protocol_validate_hello(const grd_hello *hello);
grd_status grd_protocol_validate_input(const grd_input_event *event);

#ifdef __cplusplus
}
#endif

#endif
