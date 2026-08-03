#ifndef GRD_APP_INTERNAL_H
#define GRD_APP_INTERNAL_H

#include "grd/audio.h"
#include "grd/codec.h"
#include "grd/config.h"
#include "grd/discovery.h"
#include "grd/gpu.h"
#include "grd/platform.h"
#include "grd/transport.h"

#include <SDL3/SDL.h>
#include <stdatomic.h>
#include "nuklear.h"

typedef enum grd_app_mode {
    GRD_APP_HOME = 0,
    GRD_APP_REMOTE = 1
} grd_app_mode;

/* Four frames match the transport's bounded reassembly window. When a NACK
 * repairs the oldest frame, the ordered transport may release several
 * completed successors together; this mailbox absorbs that one-off drain
 * without dropping another H.264 reference. */
#define GRD_MEDIA_QUEUE_CAPACITY 4U
#define GRD_HUD_FPS_HISTORY_CAPACITY 72U

typedef struct grd_media_packet {
    grd_packet_type type;
    uint8_t *payload;
    size_t payload_length;
    /* True when the transport delivered the buffer with a zeroed
     * GRD_MEDIA_BUFFER_PADDING tail, so the decoder may adopt it zero-copy. */
    bool padded;
} grd_media_packet;

typedef struct grd_media_queue {
    SDL_Mutex *mutex;
    SDL_Condition *condition;
    grd_media_packet packets[GRD_MEDIA_QUEUE_CAPACITY];
    size_t count;
    uint64_t dropped;
    bool stopping;
} grd_media_queue;

/* Heartbeat used to detect a stalled media thread: the thread stores
 * grd_now_micros() on every loop iteration; the 5 s stats report the age so
 * a blocked thread is visible in the log instead of silently stalling. */
typedef struct grd_thread_health {
    _Atomic uint64_t last_active_micros;
    const char *name;
} grd_thread_health;

typedef struct grd_app {
    grd_config config;
    grd_error last_error;
    grd_gpu_capabilities gpu;
    grd_pipeline_kind selected_pipeline;
    SDL_AtomicInt active_host_pipeline;
    SDL_AtomicInt active_host_codec;
    /* Whether renderer startup or recovery selected a fallback path. */
    bool renderer_fallback_used;
    bool metal_renderer;
    bool immediate_present;
    /* Set by the client texture path on a persistent upload-failure streak;
     * the main thread consumes it to switch to the software renderer. */
    SDL_AtomicInt renderer_fallback_requested;
    /* SDL's current desktop mode and the fastest advertised panel mode are
     * kept separate: on ProMotion the latter is a capability, not proof that
     * the compositor is currently presenting at that cadence. */
    float display_refresh_rate;
    float display_max_refresh_rate;
    float measured_present_rate;
    /* Effective local presentation cadence after applying the user's Hz
     * preference and clamping it to the current display capability. */
    uint32_t display_target_fps;
    _Atomic uint32_t remote_stream_fps;
    _Atomic uint32_t remote_receive_fps_tenths;
    /* Encoder target and measured elementary-stream throughput are kept
     * separate so the HUD never presents a configured ceiling as real
     * network usage. */
    _Atomic uint32_t remote_stream_bitrate_kbps;
    _Atomic uint32_t remote_receive_bitrate_kbps;
    _Atomic uint64_t remote_video_bytes_received;
    _Atomic uint32_t remote_stream_width;
    _Atomic uint32_t remote_stream_height;
    SDL_AtomicInt remote_stream_codec;
    SDL_AtomicInt remote_decoder_ready;
    SDL_AtomicInt requested_host_fps;
    _Atomic uint32_t requested_host_width;
    _Atomic uint32_t requested_host_height;
    SDL_AtomicInt requested_client_upscale_mode;
    SDL_AtomicInt requested_client_offload_flags;
    SDL_AtomicInt keyframe_requested;
    SDL_AtomicInt keyframe_request_pending;
    /* Network-side gate for a broken inter-frame chain. This is separate
     * from keyframe_request_pending: once the repair IDR is queued, later
     * P-frames are valid behind it even though the decoder has not consumed
     * that IDR yet. */
    SDL_AtomicInt remote_waiting_for_repair_idr;
    _Atomic uint64_t keyframe_requested_micros;
    uint32_t keyframe_request_interval_us;
    SDL_AtomicInt abr_loss_percent;
    SDL_AtomicInt abr_rtt_micros;
    SDL_AtomicInt abr_report_updated;
    SDL_AtomicInt client_codec_caps;
    SDL_AtomicInt wake_pending;
    Uint32 wake_event_type;

    grd_monitor monitors[GRD_MAX_MONITORS];
    size_t monitor_count;
    size_t selected_monitor;

    grd_discovery *discovery;
    grd_discovered_peer peers[32];
    size_t peer_count;
    grd_host *host;
    grd_connection *connection;
    grd_connection *media_connection;

    SDL_Thread *stream_thread;
    SDL_Thread *audio_thread;
    SDL_Thread *cursor_thread;
    SDL_Thread *video_decode_thread;
    SDL_Thread *audio_decode_thread;
    grd_thread_health health_video_decode;
    grd_thread_health health_audio_decode;
    grd_thread_health health_stream;
    grd_thread_health health_cursor;
    bool video_decode_thread_started;
    bool audio_decode_thread_started;
    grd_media_queue video_queue;
    grd_media_queue audio_queue;
    SDL_Mutex *frame_mutex;
    SDL_Mutex *cursor_mutex;
    SDL_Mutex *clipboard_mutex;
    SDL_Mutex *decoder_mutex;
    SDL_Mutex *audio_mutex;
    SDL_Mutex *error_mutex;
    SDL_AtomicInt streaming;
    grd_decoder *decoder;
    grd_audio_decoder *audio_decoder;
    SDL_AudioStream *audio_playback;
    SDL_AtomicInt audio_active;
    grd_pipeline_kind decoder_pipeline;
    grd_video_codec decoder_codec;
    uint32_t decoder_width;
    uint32_t decoder_height;
    /* VideoToolbox stall watchdog state (decode thread only): the streak
     * counter counts consecutive WOULD_BLOCK results and the timestamp
     * anchors the first one, so the 3 s fallback is based on wall time and
     * cannot trip early on a connect burst (180 packets can arrive in less
     * than a second). */
    uint32_t vt_stall_streak;
    uint64_t vt_stall_since_micros;
    /* Last VideoToolbox session recreation (decode thread only): a fresh
     * session is created after a rejected frame or a 3 s stall, throttled
     * to 2 s so a persistent decoder failure cannot spin the CPU. */
    uint64_t last_vt_recreate_micros;
    /* Consecutive failed texture uploads (main thread only): used to log
     * the recovery transition instead of counting silently. */
    uint32_t upload_failure_streak;
    uint32_t last_upload_flags;
    bool remote_window_raised;
    SDL_AtomicInt remote_raise_pending;
    grd_frame remote_frame;
    uint64_t remote_frame_generation;
    uint64_t displayed_generation;
    /* Client media diagnostics written by the decode thread and summarized
     * in the periodic stats line. */
    _Atomic uint64_t decoded_frames;
    _Atomic uint64_t decode_failures;
    _Atomic uint64_t pending_dropped_frames;
    _Atomic uint64_t upload_failures;
    _Atomic uint64_t presented_frames;
    /* Relative pointer transport diagnostics. Motion prefers authenticated
     * UDP; the reliable counter shows automatic TCP fallbacks. */
    _Atomic uint64_t relative_input_udp_sent;
    _Atomic uint64_t relative_input_tcp_sent;
    _Atomic uint64_t relative_input_send_failures;
    /* Host-side SendInput failures are rate-limited onto the reliable
     * control channel; the client count is shown in its session HUD. */
    _Atomic uint64_t host_input_injection_failures;
    _Atomic uint64_t host_input_error_broadcast_micros;
    /* Reliable input errors received from the host and surfaced in the
     * client diagnostics. */
    _Atomic uint64_t remote_input_rejection_reports;
    /* End-to-end cadence diagnostics. These expose source/arrival gaps that
     * ordinary UDP loss counters cannot see when the host intentionally
     * skips a whole encoded frame before assigning datagram sequences. */
    uint64_t remote_last_wire_frame_id;
    uint64_t remote_last_wire_arrival_micros;
    uint64_t remote_last_source_timestamp_micros;
    _Atomic uint64_t remote_source_skipped_frames;
    /* Number of times a host-side frame-id gap caused the client to reset
     * the H.264 reference chain before VideoToolbox could consume a P-frame
     * whose reference was never transmitted. */
    _Atomic uint64_t remote_source_gap_recoveries;
    _Atomic uint64_t remote_arrival_gap_count;
    _Atomic uint32_t remote_arrival_max_gap_us;
    _Atomic uint32_t remote_source_max_gap_us;
    SDL_Texture *remote_texture;
    SDL_ScaleMode remote_texture_scale_mode;
    grd_frame remote_texture_frame;
    bool remote_texture_native;
    SDL_PixelFormat remote_texture_format;
    SDL_Texture *remote_cursor_texture;
    bool remote_cursor_visible;
    float remote_cursor_x;
    float remote_cursor_y;
    bool predicted_cursor_valid;
    float predicted_cursor_x;
    float predicted_cursor_y;
    grd_cursor_shape remote_cursor_shape;
    uint64_t remote_cursor_shape_generation;
    uint64_t displayed_cursor_shape_generation;

    grd_app_mode mode;
    SDL_Window *window;
    bool relative_mouse_mode;
    bool remote_settings_visible;
    bool remote_fullscreen_active;
    uint64_t remote_heartbeat_sent_micros;
    uint8_t escape_press_count;
    uint64_t escape_sequence_started_micros;
    /* macOS remote-shortcut state. Keyboard grab keeps Command shortcuts in
     * GRD; Command+Tab is translated to the Windows Alt+Tab chord. */
    bool remote_command_tab_active;
    double relative_remainder_x;
    double relative_remainder_y;
#if defined(_WIN32)
    /* Renderer's ID3D11Device, used for CUDA↔D3D11 GPU decode output. */
    void *d3d11_device;
#endif
    bool config_dirty;
    bool request_controller;
    bool ssh_remote_access_ready;
    bool home_settings_visible;
    bool home_settings_host_tab;
    bool lan_join_modal_visible;
    grd_discovered_peer lan_join_peer;
    char lan_join_error[192];
    bool remote_access_modal_visible;
    grd_remote_access_kind remote_access_kind;
    grd_discovered_peer remote_access_peer;
    char remote_access_username[GRD_REMOTE_USERNAME_MAX];
    char remote_access_error[192];
    char connect_password[160];
    char new_host_password[160];
    char status[256];
    struct nk_rect view_rect;
    const struct nk_user_font *password_font;
    const struct nk_user_font *heading_font;
    const struct nk_user_font *small_font;
    SDL_Cursor *default_cursor;
    SDL_Cursor *text_cursor;
    char *clipboard_cache;
    uint64_t clipboard_checked_micros;
    /* Main-thread sampling state for the always-on session HUD. */
    uint64_t hud_bitrate_sample_micros;
    uint64_t hud_bitrate_sample_bytes;
    uint64_t hud_fps_sample_micros;
    uint64_t hud_health_window_started_micros;
    uint64_t hud_last_skipped_frames;
    uint64_t hud_last_recoveries;
    uint64_t hud_last_decode_failures;
    uint64_t hud_recent_skipped_frames;
    uint64_t hud_recent_recoveries;
    uint64_t hud_recent_decode_failures;
    float hud_fps_history[GRD_HUD_FPS_HISTORY_CAPACITY];
    uint32_t hud_fps_history_count;
    uint32_t hud_fps_history_next;
} grd_app;

bool grd_app_initialize(grd_app *app);
void grd_app_shutdown(grd_app *app);
void grd_app_draw(grd_app *app, struct nk_context *context, SDL_Renderer *renderer);
void grd_app_handle_remote_event(grd_app *app, const SDL_Event *event);
void grd_app_refresh_remote_texture(grd_app *app, SDL_Renderer *renderer);
/* Destroys the remote/cursor textures and releases the held frame; used by
 * the main thread before swapping the SDL renderer at runtime. */
void grd_app_reset_remote_texture(grd_app *app);
void grd_app_configure_display(grd_app *app);
void grd_app_handle_display_change(grd_app *app);

#endif
