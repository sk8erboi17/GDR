#ifndef GRD_CONFIG_H
#define GRD_CONFIG_H

#include "grd/common.h"
#include "grd/remote_access.h"
#include <sodium.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum grd_gpu_preference {
    GRD_GPU_AUTOMATIC = 0,
    GRD_GPU_METAL = 1,
    GRD_GPU_CUDA = 2,
    GRD_GPU_SOFTWARE = 3
} grd_gpu_preference;

typedef enum grd_stream_profile {
    GRD_STREAM_BALANCED = 0,
    GRD_STREAM_GAMING = 1,
    GRD_STREAM_DESKTOP = 2
} grd_stream_profile;

typedef enum grd_video_codec {
    GRD_CODEC_H264 = 0,
    GRD_CODEC_HEVC = 1,
    GRD_CODEC_AV1 = 2
} grd_video_codec;

typedef enum grd_mouse_mode {
    GRD_MOUSE_AUTOMATIC = 0,
    GRD_MOUSE_ABSOLUTE = 1,
    GRD_MOUSE_RELATIVE = 2
} grd_mouse_mode;

typedef struct grd_config {
    char device_id[37];
    char device_name[GRD_MAX_DEVICE_NAME];
    uint16_t port;
    bool host_enabled;
    /* Host-side capture/encode ceiling. */
    uint32_t target_fps;
    /* Bitrate semantics: the resolution recommends a rate, initial_bitrate
     * is where the stream starts, target_bitrate is the hard ceiling and
     * min_bitrate the ABR floor. The encoder is never forced above the
     * ceiling just because a resolution "recommends" more. */
    uint32_t initial_bitrate_kbps;
    uint32_t target_bitrate_kbps;
    uint32_t min_bitrate_kbps;
    bool abr_enabled;
    grd_stream_profile stream_profile;
    grd_video_codec video_codec;
    bool pixel_444;
    grd_gpu_preference gpu_preference;
    /* Client-side session preferences. The requested stream rate is kept
     * separate from the local presentation cadence: a ProMotion client can,
     * for example, request a 60 FPS stream and still present/predict input at
     * 120 Hz. presentation_hz == 0 selects the fastest supported display
     * cadence automatically. */
    uint32_t client_target_fps;
    uint32_t presentation_hz;
    /* Maximum encoded height requested from the host. 0 selects the native
     * client-display limit; the UI also exposes 1080p, 1440p and 2160p. */
    uint32_t client_max_height;
    /* Selective client-side work. Native keeps the requested encoded
     * resolution. Balanced/Performance ask the host for one/two lower
     * resolution ladder rungs and upscale locally. Frame pacing and cursor
     * prediction remain independent so users can trade client GPU work and
     * perceived input smoothness separately. */
    grd_client_upscale_mode client_upscale_mode;
    bool client_frame_pacing;
    bool client_cursor_prediction;
    bool show_advanced_stats;
    bool sharp_video_scaling;
    /* Automatic preserves the click-to-capture behaviour used by games;
     * Absolute always sends normalized cursor coordinates; Relative keeps
     * camera-style raw motion available after the session captures it. */
    grd_mouse_mode mouse_mode;
    /* Stored as an integer to avoid locale-dependent floating-point config. */
    uint32_t mouse_sensitivity_percent;
    bool remote_fullscreen;
    /* Optional system OpenSSH bridge. GRD only advertises it after a local
     * SSH banner probe succeeds; account authentication and host-key trust
     * remain entirely owned by the operating system's ssh/sftp clients. */
    bool ssh_remote_access_enabled;
    uint16_t ssh_remote_access_port;
    char remote_access_username[GRD_REMOTE_USERNAME_MAX];
    uint8_t password_salt[crypto_pwhash_SALTBYTES];
    uint8_t password_verifier[32];
    bool password_configured;
} grd_config;

void grd_config_defaults(grd_config *config);
bool grd_config_directory(char *output, size_t capacity);
grd_status grd_config_load(grd_config *config, grd_error *error);
grd_status grd_config_save(const grd_config *config, grd_error *error);
grd_status grd_config_set_password(
    grd_config *config,
    const char *password,
    grd_error *error
);

#ifdef __cplusplus
}
#endif

#endif
