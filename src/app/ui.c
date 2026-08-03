#include "app/app.h"
#include "core/stream_policy.h"
#include "grd/log.h"
#include "grd/remote_access.h"

#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* timestamp(8) + width(4) + height(4) + keyframe(1) + frame_id(8): the
 * frame_id is the same counter the host records in its trace, so a decode
 * dump can be correlated with the encode/pacer phases. */
#define VIDEO_WIRE_PREFIX 25U
#define AUDIO_WIRE_PREFIX 8U
/* Adaptive bitrate hysteresis on the client's 100 ms UDP loss reports. */
#define GRD_ABR_CUT_HARD_PCT 10U
#define GRD_ABR_CUT_STRONG_PCT 3U
#define GRD_ABR_CUT_MILD_PCT 1U
#define GRD_ABR_SELF_CUT_HARD_PCT 10U
#define GRD_ABR_SELF_CUT_STRONG_PCT 3U
#define GRD_ABR_SELF_CUT_MILD_PCT 1U
/* One 100 ms loss window can spike during a fast scene change (pacer
 * burst, encoder overshoot) without the link being congested: require two
 * network reports and three host-local reports before cutting, so a single
 * burst cannot drop the bitrate for many seconds. */
#define GRD_ABR_CUT_REQUIRED_REPORTS 2U
#define GRD_ABR_SELF_CUT_REQUIRED_REPORTS 3U
/* A fast additive ramp was visible as 18 -> 16.2 -> 14.58 -> 18 Mbps
 * oscillation during complex scene changes. A local cut now needs sustained
 * evidence, holds for eight seconds, and only recovers after five seconds in
 * which both the LAN and the host pacer were completely clean. */
#define GRD_ABR_HOLD_US 8000000ULL
#define GRD_ABR_RECOVERY_CLEAN_REPORTS 50U
#define GRD_ABR_INCREASE_INTERVAL_US 2000000ULL
#define GRD_ABR_INCREASE_STEP_KBPS 200U
#define GRD_ABR_STARTUP_HOLD_US 3000000ULL
/* After a downward ABR step, keep the pacer on the old wire rate for this
 * long so NVENC can shrink its frames instead of having them mass-dropped
 * (a 46 KB frame does not fit a wire slot sized for the new budget). */
#define GRD_ABR_PACER_DOWN_GRACE_US 700000ULL
/* Any host-local initiating drop blocks an additive quality probe. Raising
 * bitrate while the pacer is already discarding references was the direct
 * cause of the observed up/down hunting and extra recovery IDRs. */
#define GRD_ABR_SELF_DROP_MARGIN 0U
/* Receiver-loss cuts and quality increases are frozen while an IDR is
 * pending and briefly after it lands. Host-local initiating pressure is
 * filtered separately and remains actionable during recovery. */
#define GRD_ABR_RECOVERY_STABILIZE_US 2000000ULL
/* Consecutive texture-upload failures (at 60 fps this is ~2 s) after which
 * the client asks the main thread to switch from the GPU renderer to the
 * software one: a Metal/CAMetalLayer that refuses drawables stays black
 * forever otherwise (observed: 1984 upload-fail, 4 presented). */
#define GRD_UPLOAD_FALLBACK_STREAK 120U
#define GRD_HOST_INPUT_ERROR_PREFIX "Host input rejected: "
#define GRD_HOST_INPUT_ERROR_REPORT_INTERVAL_US 1000000ULL

/* Rolling average-QP history retained for host quality diagnostics. */
#define GRD_QP_RING_CAPACITY 128U
/* The configured bitrate is a TOTAL NETWORK wire budget: the pacer derives
 * its spacing from 1200-byte wire datagrams, but each datagram carries only
 * GRD_UDP_FRAGMENT_CAPACITY (1128) bytes of video payload. Audio, FEC
 * parity and retransmissions share the same budget, so the encoder target
 * must stay below the wire budget: at 24 Mbps/60 fps a full 24 Mbps frame
 * needs 45 datagrams x 400 us = 18 ms, longer than the 16.67 ms period,
 * and the pacer's admission control would drop it every frame. */
/* Encoder share of the total wire budget. Each 1200-byte datagram carries
 * 1128 bytes of media and gaming FEC adds one parity datagram per 16 data
 * fragments. With NVENC's one-frame VBV and 10% peak headroom, 84% without FEC
 * and 78% with FEC leave room for headers, audio, parity and a short NACK
 * retransmission while using the clean-LAN headroom that the old 75/70
 * split left idle (observed 15.4 Mbps on an 18 Mbps session). */
#define GRD_VIDEO_BUDGET_FEC_OFF_PCT 84U
#define GRD_VIDEO_BUDGET_FEC_ON_PCT 78U

#define GRD_TIMING_WINDOW_CAPACITY 1024U

static uint32_t effective_client_stream_fps(const grd_app *app);

typedef struct grd_timing_window {
    uint32_t samples[GRD_TIMING_WINDOW_CAPACITY];
    size_t sample_count;
    uint64_t count;
    uint64_t sum;
    uint64_t maximum;
} grd_timing_window;

typedef struct grd_timing_summary {
    double average;
    uint64_t p95;
    uint64_t maximum;
} grd_timing_summary;

static int compare_timing_sample(const void *left, const void *right)
{
    const uint32_t left_value = *(const uint32_t *)left;
    const uint32_t right_value = *(const uint32_t *)right;
    return left_value < right_value ? -1 : left_value > right_value ? 1 : 0;
}

static void timing_window_add(grd_timing_window *window, uint64_t micros)
{
    if (window == NULL) {
        return;
    }
    if (window->sample_count < GRD_TIMING_WINDOW_CAPACITY) {
        window->samples[window->sample_count++] =
            micros > UINT32_MAX ? UINT32_MAX : (uint32_t)micros;
    }
    ++window->count;
    window->sum += micros;
    if (micros > window->maximum) {
        window->maximum = micros;
    }
}

static grd_timing_summary timing_window_summary(
    const grd_timing_window *window
)
{
    grd_timing_summary summary = {0};
    if (window == NULL || window->count == 0U) {
        return summary;
    }
    summary.average = (double)window->sum / (double)window->count;
    summary.maximum = window->maximum;
    if (window->sample_count == 0U) {
        return summary;
    }
    uint32_t sorted[GRD_TIMING_WINDOW_CAPACITY];
    memcpy(
        sorted,
        window->samples,
        window->sample_count * sizeof(sorted[0])
    );
    qsort(
        sorted,
        window->sample_count,
        sizeof(sorted[0]),
        compare_timing_sample
    );
    size_t p95_index =
        (window->sample_count * 95U + 99U) / 100U;
    if (p95_index > 0U) {
        --p95_index;
    }
    if (p95_index >= window->sample_count) {
        p95_index = window->sample_count - 1U;
    }
    summary.p95 = sorted[p95_index];
    return summary;
}

static void timing_window_reset(grd_timing_window *window)
{
    if (window != NULL) {
        memset(window, 0, sizeof(*window));
    }
}

static uint32_t video_encoder_share_percent(bool fec_enabled)
{
    return fec_enabled
               ? GRD_VIDEO_BUDGET_FEC_ON_PCT
               : GRD_VIDEO_BUDGET_FEC_OFF_PCT;
}

static uint32_t video_encoder_bitrate_kbps(
    uint32_t network_kbps,
    bool fec_enabled
)
{
    const uint32_t percentage = video_encoder_share_percent(fec_enabled);
    return (uint32_t)((uint64_t)network_kbps * percentage / 100U);
}

static uint32_t clamp_stream_rate(uint32_t fps)
{
    if (fps < 30U) {
        return 30U;
    }
    return fps > 120U ? 120U : fps;
}

static _Atomic uint64_t g_warn_throttle_micros;

/* Rate-limited warning: repeated identical failures (decode errors, lost
 * IDRs) must not flood the log at 60 lines/s. */
static void warn_throttled(const char *format, ...)
{
    const uint64_t now = grd_now_micros();
    uint64_t last = atomic_load_explicit(
        &g_warn_throttle_micros, memory_order_relaxed
    );
    if (now - last < 1000000ULL) {
        return;
    }
    if (!atomic_compare_exchange_strong_explicit(
            &g_warn_throttle_micros,
            &last,
            now,
            memory_order_relaxed,
            memory_order_relaxed
        )) {
        return;
    }
    char buffer[512];
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    GRD_WARN("%s", buffer);
}

static void info_throttled(const char *format, ...)
{
    const uint64_t now = grd_now_micros();
    uint64_t last = atomic_load_explicit(
        &g_warn_throttle_micros, memory_order_relaxed
    );
    if (now - last < 1000000ULL) {
        return;
    }
    if (!atomic_compare_exchange_strong_explicit(
            &g_warn_throttle_micros,
            &last,
            now,
            memory_order_relaxed,
            memory_order_relaxed
        )) {
        return;
    }
    char buffer[512];
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    GRD_INFO("%s", buffer);
}

/* Thread map: one row per tracked thread, classified by age since last
 * heartbeat (off / active / idle / STALLED), with transition warnings so a
 * blocked thread is caught over time instead of only at a single instant. */
typedef enum grd_map_thread {
    GRD_MAP_VIDEO_DECODE = 0,
    GRD_MAP_AUDIO_DECODE,
    GRD_MAP_STREAM,
    GRD_MAP_CURSOR,
    GRD_MAP_UDP_RX,
    GRD_MAP_TCP_RX,
    GRD_MAP_TCP_TX,
    GRD_MAP_DISCOVERY,
    GRD_MAP_COUNT
} grd_map_thread;

static const char *const grd_map_thread_names[GRD_MAP_COUNT] = {
    "video-decode",
    "audio-decode",
    "stream",
    "cursor",
    "udp-rx",
    "tcp-rx",
    "tcp-tx",
    "discovery"
};

static uint64_t health_age_ms(const grd_thread_health *health)
{
    const uint64_t last = atomic_load_explicit(
        &health->last_active_micros, memory_order_relaxed
    );
    return last != 0ULL ? (grd_now_micros() - last) / 1000ULL : 0ULL;
}

static void log_thread_map(grd_app *app)
{
    uint64_t udp_age = 0ULL;
    uint64_t tcp_rx_age = 0ULL;
    uint64_t tcp_tx_age = 0ULL;
    /* udp-rx lives on the MEDIA connection (the UDP video side channel);
     * the TCP control threads live on the control connection. */
    grd_connection_thread_health(
        app->media_connection != NULL ? app->media_connection : app->connection,
        &udp_age,
        &tcp_rx_age,
        &tcp_tx_age
    );
    uint64_t control_rx_age = 0ULL;
    uint64_t control_tx_age = 0ULL;
    grd_connection_thread_health(
        app->connection,
        NULL,
        &control_rx_age,
        &control_tx_age
    );
    const uint64_t ages[GRD_MAP_COUNT] = {
        health_age_ms(&app->health_video_decode),
        health_age_ms(&app->health_audio_decode),
        health_age_ms(&app->health_stream),
        health_age_ms(&app->health_cursor),
        udp_age,
        control_rx_age != 0ULL ? control_rx_age : tcp_rx_age,
        control_tx_age != 0ULL ? control_tx_age : tcp_tx_age,
        grd_discovery_thread_age_ms(app->discovery)
    };
    /* STALLED thresholds: media threads must wake every few frames; the UDP
     * receiver has a 500 ms socket timeout; discovery blocks for seconds by
     * design. TCP control threads block on their queues/sockets whenever
     * there is no control traffic (steady UDP streaming sends no TCP data),
     * so they are reported with their age but never flagged: a long tcp-rx
     * age is normal, not a stall. */
    static const uint64_t thresholds[GRD_MAP_COUNT] = {
        2000ULL, 2000ULL, 2000ULL, 2000ULL,
        5000ULL, 0ULL, 0ULL, 10000ULL
    };
    static bool previous_stalled[GRD_MAP_COUNT] = {false};
    char map[768];
    size_t offset = 0U;
    for (size_t index = 0U; index < GRD_MAP_COUNT; ++index) {
        const bool relevant =
            index != GRD_MAP_STREAM && index != GRD_MAP_CURSOR
                ? true
                : app->host != NULL;
        const char *state = "off";
        bool stalled = false;
        if (relevant && ages[index] != 0ULL) {
            if (ages[index] < 100ULL) {
                state = "active";
            } else if (thresholds[index] == 0ULL ||
                       ages[index] < thresholds[index]) {
                state = "idle";
            } else {
                state = "STALLED";
                stalled = true;
            }
        }
        if (stalled && !previous_stalled[index]) {
            GRD_WARN(
                "thread STALLED: %s (inactive %llu ms)",
                grd_map_thread_names[index],
                (unsigned long long)ages[index]
            );
        } else if (!stalled && previous_stalled[index] &&
                   relevant && ages[index] != 0ULL) {
            GRD_INFO(
                "thread reactivated: %s",
                grd_map_thread_names[index]
            );
        }
        previous_stalled[index] = stalled;
        if (!relevant && ages[index] == 0ULL) {
            continue;
        }
        offset += (size_t)snprintf(
            map + offset,
            sizeof(map) - offset,
            "%s%s=%llu(%s)",
            offset != 0U ? " " : "",
            grd_map_thread_names[index],
            (unsigned long long)ages[index],
            state
        );
    }
    GRD_INFO("thread map: %s", offset != 0U ? map : "no active threads");
}

static const char *pipeline_display_name(grd_pipeline_kind pipeline);
static void send_input(grd_app *app, const grd_input_event *event);

/* Pipeline map: decoder, renderer, codec negotiation and host encoder. */
static void log_fallback_map(grd_app *app)
{
    const char *decoder_state =
        app->decoder_pipeline == GRD_PIPELINE_METAL_VIDEOTOOLBOX
            ? "VideoToolbox"
            : pipeline_display_name(app->decoder_pipeline);
    const char *renderer_state =
        app->metal_renderer
            ? (app->renderer_fallback_used
                   ? "metal (fallback from software)"
                   : "metal")
            : (app->renderer_fallback_used
                   ? "software (fallback)"
                   : "software");
    const char *client_codec =
        grd_codec_name(app->decoder_codec);
    char host_state[128] = "-";
    if (app->host != NULL) {
        const int host_pipeline = SDL_GetAtomicInt(
            &app->active_host_pipeline
        );
        const int host_codec = SDL_GetAtomicInt(&app->active_host_codec);
        (void)snprintf(
            host_state,
            sizeof(host_state),
            "%s/%s",
            pipeline_display_name((grd_pipeline_kind)host_pipeline),
            grd_codec_name((grd_video_codec)host_codec)
        );
    }
    GRD_INFO(
        "fallback map: decoder=%s codec=%s renderer=%s host=%s",
        decoder_state,
        client_codec,
        renderer_state,
        host_state
    );
}

static void destroy_remote_texture(grd_app *app);

static void media_packet_release(grd_media_packet *packet)
{
    if (packet == NULL) {
        return;
    }
    free(packet->payload);
    memset(packet, 0, sizeof(*packet));
}

static bool media_queue_init(grd_media_queue *queue)
{
    memset(queue, 0, sizeof(*queue));
    queue->mutex = SDL_CreateMutex();
    queue->condition = SDL_CreateCondition();
    if (queue->mutex == NULL || queue->condition == NULL) {
        if (queue->condition != NULL) {
            SDL_DestroyCondition(queue->condition);
        }
        if (queue->mutex != NULL) {
            SDL_DestroyMutex(queue->mutex);
        }
        memset(queue, 0, sizeof(*queue));
        return false;
    }
    return true;
}

static void media_queue_reset(grd_media_queue *queue)
{
    SDL_LockMutex(queue->mutex);
    queue->stopping = false;
    SDL_UnlockMutex(queue->mutex);
}

static void media_queue_stop(grd_media_queue *queue)
{
    if (queue == NULL || queue->mutex == NULL) {
        return;
    }
    SDL_LockMutex(queue->mutex);
    queue->stopping = true;
    for (size_t index = 0U; index < queue->count; ++index) {
        media_packet_release(&queue->packets[index]);
    }
    queue->count = 0U;
    SDL_BroadcastCondition(queue->condition);
    SDL_UnlockMutex(queue->mutex);
}

static void media_queue_destroy(grd_media_queue *queue)
{
    if (queue == NULL || queue->mutex == NULL) {
        return;
    }
    media_queue_stop(queue);
    SDL_DestroyCondition(queue->condition);
    SDL_DestroyMutex(queue->mutex);
    memset(queue, 0, sizeof(*queue));
}

/* Assumes the mutex is held. Returns false without taking the payload when
 * the queue is stopping. */
static bool media_queue_enqueue_locked(
    grd_media_queue *queue,
    grd_packet_type type,
    uint8_t *payload,
    size_t payload_length,
    bool padded,
    bool *evicted
)
{
    if (evicted != NULL) {
        *evicted = false;
    }
    if (queue->stopping) {
        return false;
    }
    if (queue->count == GRD_MEDIA_QUEUE_CAPACITY) {
        media_packet_release(&queue->packets[0]);
        memmove(
            &queue->packets[0],
            &queue->packets[1],
            (GRD_MEDIA_QUEUE_CAPACITY - 1U) * sizeof(queue->packets[0])
        );
        --queue->count;
        memset(&queue->packets[queue->count], 0, sizeof(queue->packets[0]));
        ++queue->dropped;
        if (evicted != NULL) {
            *evicted = true;
        }
    }
    queue->packets[queue->count++] = (grd_media_packet){
        .type = type,
        .payload = payload,
        .payload_length = payload_length,
        .padded = padded
    };
    SDL_SignalCondition(queue->condition);
    return true;
}

static bool media_queue_push(
    grd_media_queue *queue,
    grd_packet_type type,
    const void *payload,
    size_t payload_length,
    bool *evicted
)
{
    if (queue == NULL || payload == NULL || payload_length == 0U) {
        return false;
    }
    uint8_t *copy = malloc(payload_length);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, payload, payload_length);
    SDL_LockMutex(queue->mutex);
    const bool accepted = media_queue_enqueue_locked(
        queue, type, copy, payload_length, false, evicted
    );
    SDL_UnlockMutex(queue->mutex);
    if (!accepted) {
        free(copy);
    }
    return accepted;
}

/* Zero-copy variant: the queue takes ownership of payload (freed by
 * media_packet_release) without copying it. On failure ownership stays with
 * the caller, which must free it or hand it back to the transport. */
static bool media_queue_push_owned(
    grd_media_queue *queue,
    grd_packet_type type,
    uint8_t *payload,
    size_t payload_length,
    bool *evicted
)
{
    if (queue == NULL || payload == NULL || payload_length == 0U) {
        return false;
    }
    SDL_LockMutex(queue->mutex);
    const bool accepted = media_queue_enqueue_locked(
        queue, type, payload, payload_length, true, evicted
    );
    SDL_UnlockMutex(queue->mutex);
    return accepted;
}

/* Discard queued media without stopping the consumer. Used when the wire
 * frame id proves that the host omitted an encoded reference: no packet
 * already waiting behind that gap is safe to feed to the old decoder. */
static void media_queue_clear(grd_media_queue *queue)
{
    if (queue == NULL || queue->mutex == NULL) {
        return;
    }
    SDL_LockMutex(queue->mutex);
    for (size_t index = 0U; index < queue->count; ++index) {
        media_packet_release(&queue->packets[index]);
    }
    queue->count = 0U;
    SDL_UnlockMutex(queue->mutex);
}

static bool media_queue_pop(grd_media_queue *queue, grd_media_packet *packet)
{
    SDL_LockMutex(queue->mutex);
    while (queue->count == 0U && !queue->stopping) {
        SDL_WaitCondition(queue->condition, queue->mutex);
    }
    if (queue->count == 0U) {
        SDL_UnlockMutex(queue->mutex);
        return false;
    }
    *packet = queue->packets[0];
    if (queue->count > 1U) {
        memmove(
            &queue->packets[0],
            &queue->packets[1],
            (queue->count - 1U) * sizeof(queue->packets[0])
        );
    }
    --queue->count;
    memset(&queue->packets[queue->count], 0, sizeof(queue->packets[0]));
    SDL_UnlockMutex(queue->mutex);
    return true;
}

static void signal_main_thread(grd_app *app)
{
    if (app == NULL || app->wake_event_type == 0U ||
        !SDL_CompareAndSwapAtomicInt(&app->wake_pending, 0, 1)) {
        return;
    }
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = app->wake_event_type;
    if (!SDL_PushEvent(&event)) {
        (void)SDL_SetAtomicInt(&app->wake_pending, 0);
    }
}

static grd_video_codec codec_from_name(const char *name)
{
    if (name != NULL && strcmp(name, "hevc") == 0) {
        return GRD_CODEC_HEVC;
    }
    if (name != NULL && strcmp(name, "av1") == 0) {
        return GRD_CODEC_AV1;
    }
    return GRD_CODEC_H264;
}

/* Media threads carry the real-time pipeline: on macOS this maps to the
 * time-constraint QoS class so capture/encode/decode are not demoted while
 * the UI or other apps are busy. */
static void set_media_thread_priority(void)
{
#if defined(__APPLE__)
    (void)SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    (void)SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif
}

/* The pipeline enum names the encoder backend; on NVIDIA it does not say
 * where the RGBA→NV12 conversion runs. Without the CUDA runtime module the
 * standard installer converts on the CPU even though NVENC is active, so the
 * label distinguishes the two paths. */
static const char *pipeline_display_name(grd_pipeline_kind pipeline)
{
    const bool cuda_runtime = grd_cuda_available(NULL, 0U);
    if (pipeline == GRD_PIPELINE_CUDA_NVENC) {
        return cuda_runtime ? "CUDA + NVENC/NVDEC (GPU conversion)"
                            : "NVENC + CPU conversion";
    }
    if (pipeline == GRD_PIPELINE_CUDA_SOFTWARE) {
        return cuda_runtime ? "CUDA + software H.264" : "software H.264";
    }
    return grd_pipeline_name(pipeline);
}

static void set_status(grd_app *app, const char *message)
{
    if (app->error_mutex != NULL) {
        SDL_LockMutex(app->error_mutex);
    }
    (void)snprintf(app->status, sizeof(app->status), "%s", message);
    if (app->error_mutex != NULL) {
        SDL_UnlockMutex(app->error_mutex);
    }
}

/* Network/capture/decoder threads may report errors while the UI is painting
 * the status line. Keep the diagnostic slot independent from those threads so
 * a partially written message can never be displayed. */
static void publish_error(grd_app *app, const grd_error *error)
{
    if (app == NULL || error == NULL || app->error_mutex == NULL ||
        error->code == GRD_OK) {
        return;
    }
    /* Errors were previously only stored for the UI: a failing decoder or
     * texture upload left grd.log silent while the screen stayed black. */
    warn_throttled(
        "UI error: %s (code %d)",
        error->message[0] != '\0' ? error->message : "unknown",
        (int)error->code
    );
    SDL_LockMutex(app->error_mutex);
    app->last_error = *error;
    SDL_UnlockMutex(app->error_mutex);
}

/* SDL reports relative motion as floats. Keep the sub-pixel part instead of
 * truncating it: truncation makes low-DPI devices feel sticky and uneven. */
static int32_t consume_relative_delta(double *remainder)
{
    if (*remainder >= (double)INT32_MAX) {
        *remainder -= (double)INT32_MAX;
        return INT32_MAX;
    }
    if (*remainder <= (double)INT32_MIN) {
        *remainder -= (double)INT32_MIN;
        return INT32_MIN;
    }
    /* Round to the nearest pixel instead of truncating: truncation makes
     * slow/light mouse movement feel unresponsive because sub-pixel motion
     * is discarded until it happens to cross an integer boundary. */
    const int32_t delta = (int32_t)(
        *remainder >= 0.0 ? *remainder + 0.5 : *remainder - 0.5
    );
    *remainder -= (double)delta;
    return delta;
}

static void atomic_update_max_u32(_Atomic uint32_t *value, uint32_t candidate)
{
    uint32_t current = atomic_load_explicit(value, memory_order_relaxed);
    while (candidate > current &&
           !atomic_compare_exchange_weak_explicit(
               value,
               &current,
               candidate,
               memory_order_relaxed,
               memory_order_relaxed
           )) {
    }
}

static bool set_remote_fullscreen(grd_app *app, bool enabled)
{
    if (app == NULL || app->window == NULL) {
        return false;
    }
    if (app->remote_fullscreen_active == enabled) {
        return true;
    }
    if (!SDL_SetWindowFullscreen(app->window, enabled)) {
        GRD_WARN(
            "%s full screen failed: %s",
            enabled ? "entering" : "leaving",
            SDL_GetError()
        );
        return false;
    }
    /* Fullscreen transitions are asynchronous on macOS. Synchronize here so
     * the display capabilities and the first video layout use the final
     * drawable size instead of the old windowed size. */
    (void)SDL_SyncWindow(app->window);
    app->remote_fullscreen_active = enabled;
    GRD_INFO("remote session: full screen=%d", enabled ? 1 : 0);
    return true;
}

static void set_remote_keyboard_grab(grd_app *app, bool enabled)
{
    if (app == NULL || app->window == NULL ||
        SDL_GetWindowKeyboardGrab(app->window) == enabled) {
        return;
    }
    if (!SDL_SetWindowKeyboardGrab(app->window, enabled)) {
        GRD_WARN(
            "remote keyboard grab %s failed: %s",
            enabled ? "activation" : "release",
            SDL_GetError()
        );
    }
}

static nk_flags password_edit(
    grd_app *app,
    struct nk_context *context,
    char *buffer,
    int capacity
)
{
    const bool masked = app->password_font != NULL &&
                        nk_style_push_font(context, app->password_font);
    const nk_flags state = nk_edit_string_zero_terminated(
        context,
        NK_EDIT_FIELD | NK_EDIT_SIG_ENTER,
        buffer,
        capacity,
        nk_filter_default
    );
    if (app->text_cursor != NULL && nk_widget_is_hovered(context)) {
        (void)SDL_SetCursor(app->text_cursor);
    }
    if (masked) {
        nk_style_pop_font(context);
    }
    return state;
}

static bool clipboard_matches(grd_app *app, const char *text)
{
    bool matches;
    SDL_LockMutex(app->clipboard_mutex);
    matches = app->clipboard_cache != NULL &&
              strcmp(app->clipboard_cache, text) == 0;
    SDL_UnlockMutex(app->clipboard_mutex);
    return matches;
}

static void clipboard_remember(grd_app *app, const char *text)
{
    const size_t length = strlen(text);
    char *copy = malloc(length + 1U);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, text, length + 1U);
    SDL_LockMutex(app->clipboard_mutex);
    free(app->clipboard_cache);
    app->clipboard_cache = copy;
    SDL_UnlockMutex(app->clipboard_mutex);
}

static void write_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void write_u64(uint8_t *output, uint64_t value)
{
    for (size_t index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> ((7U - index) * 8U));
    }
}

static uint32_t read_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) |
           ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) |
           (uint32_t)input[3];
}

static uint64_t read_u64(const uint8_t *input)
{
    uint64_t value = 0U;
    for (size_t index = 0U; index < 8U; ++index) {
        value = (value << 8U) | input[index];
    }
    return value;
}

static void fit_to_max_dimensions(
    uint32_t source_width,
    uint32_t source_height,
    uint32_t max_width,
    uint32_t max_height,
    uint32_t *output_width,
    uint32_t *output_height
)
{
    double scale = 1.0;
    if (source_width > max_width) {
        scale = (double)max_width / (double)source_width;
    }
    if ((double)source_height * scale > (double)max_height) {
        scale = (double)max_height / (double)source_height;
    }
    uint32_t width = (uint32_t)((double)source_width * scale);
    uint32_t height = (uint32_t)((double)source_height * scale);
    width &= ~1U;
    height &= ~1U;
    *output_width = width >= 2U ? width : 2U;
    *output_height = height >= 2U ? height : 2U;
}

/* Bitrate the encoder would like for a resolution/refresh, independent of
 * the configured ceiling (30000 kbps baseline at 1080p60, scaled linearly).
 * HEVC/AV1 get their own curves (rough calibration, to be refined with
 * VMAF/SSIM): same quality at a lower bitrate, which also reduces the wire
 * traffic instead of only improving quality at the same rate. */
static uint32_t resolution_bitrate_kbps(
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    grd_video_codec codec
)
{
    const uint64_t reference_pixels = 1920ULL * 1080ULL;
    const uint64_t reference_rate = reference_pixels * 60ULL;
    const uint64_t requested_rate = (uint64_t)width * (uint64_t)height * fps;
    uint64_t recommended = (30000ULL * requested_rate + reference_rate - 1U) /
                           reference_rate;
    if (recommended < 14000ULL) {
        recommended = 14000ULL;
    }
    if (recommended > 60000ULL) {
        recommended = 60000ULL;
    }
    if (codec == GRD_CODEC_HEVC) {
        recommended = recommended * 72ULL / 100ULL;
    } else if (codec == GRD_CODEC_AV1) {
        recommended = recommended * 62ULL / 100ULL;
    }
    return (uint32_t)recommended;
}

/* Codec chosen from the intersection of host encoders and the client's
 * advertised decode caps. The configured codec is the preference; anything
 * not decodable by the client falls back to H.264. Unknown caps (old
 * client / not yet received) mean H.264 only. */
static grd_video_codec negotiate_codec(
    grd_app *app,
    grd_pipeline_kind pipeline
)
{
    const grd_video_codec configured = app->config.video_codec;
    const uint32_t caps =
        (uint32_t)SDL_GetAtomicInt(&app->client_codec_caps);
    if (configured == GRD_CODEC_HEVC &&
        (caps & GRD_VIDEO_CAPS_HEVC) != 0U &&
        grd_codec_encoder_available(pipeline, GRD_CODEC_HEVC)) {
        return GRD_CODEC_HEVC;
    }
    if (configured == GRD_CODEC_AV1 &&
        (caps & GRD_VIDEO_CAPS_AV1) != 0U &&
        grd_codec_encoder_available(pipeline, GRD_CODEC_AV1)) {
        return GRD_CODEC_AV1;
    }
    return GRD_CODEC_H264;
}

static void report_host_input_failure(
    grd_app *app,
    const grd_error *input_error
)
{
    if (app == NULL || input_error == NULL || app->host == NULL) {
        return;
    }
    atomic_fetch_add_explicit(
        &app->host_input_injection_failures, 1U, memory_order_relaxed
    );
    const uint64_t now = grd_now_micros();
    uint64_t previous = atomic_load_explicit(
        &app->host_input_error_broadcast_micros, memory_order_relaxed
    );
    for (;;) {
        if (previous != 0U &&
            now - previous < GRD_HOST_INPUT_ERROR_REPORT_INTERVAL_US) {
            return;
        }
        if (atomic_compare_exchange_weak_explicit(
                &app->host_input_error_broadcast_micros,
                &previous,
                now,
                memory_order_relaxed,
                memory_order_relaxed
            )) {
            break;
        }
    }

    char message[256];
    (void)snprintf(
        message,
        sizeof(message),
        "%s%s",
        GRD_HOST_INPUT_ERROR_PREFIX,
        input_error->message[0] != '\0'
            ? input_error->message
            : "Windows refused the event"
    );
    grd_error transport_error = {0};
    if (grd_host_broadcast(
            app->host,
            GRD_PACKET_ERROR,
            message,
            strlen(message) + 1U,
            &transport_error
        ) != GRD_OK) {
        warn_throttled(
            "host: unable to notify client of rejected input: %s",
            transport_error.message[0] != '\0'
                ? transport_error.message
                : "transport error"
        );
    }
}

static const char *mouse_mode_name(grd_mouse_mode mode)
{
    switch (mode) {
    case GRD_MOUSE_ABSOLUTE:
        return "absolute";
    case GRD_MOUSE_RELATIVE:
        return "relative";
    case GRD_MOUSE_AUTOMATIC:
    default:
        return "automatic";
    }
}

static void release_remote_mouse(grd_app *app)
{
    if (app == NULL) {
        return;
    }
    if (app->relative_mouse_mode && app->window != NULL) {
        (void)SDL_SetWindowRelativeMouseMode(app->window, false);
    }
    app->relative_mouse_mode = false;
    app->relative_remainder_x = 0.0;
    app->relative_remainder_y = 0.0;
}

static bool capture_remote_mouse(grd_app *app, const char *reason)
{
    if (app == NULL || app->window == NULL) {
        return false;
    }
    if (app->relative_mouse_mode) {
        return true;
    }
    app->relative_remainder_x = 0.0;
    app->relative_remainder_y = 0.0;
    app->relative_mouse_mode = SDL_SetWindowRelativeMouseMode(
        app->window, true
    );
    if (app->relative_mouse_mode) {
        GRD_INFO(
            "client input: relative mouse captured (%s)",
            reason != NULL ? reason : "session"
        );
        return true;
    }
    GRD_WARN(
        "client input: relative mouse capture failed: %s",
        SDL_GetError()
    );
    set_status(app, "Unable to capture the mouse");
    return false;
}

static bool host_packet(
    grd_packet_type type,
    const void *payload,
    size_t payload_length,
    bool payload_takeable,
    grd_role role,
    void *userdata
)
{
    (void)payload_takeable;
    grd_app *app = userdata;
    if (type == GRD_PACKET_BITRATE_REPORT &&
        payload_length == sizeof(grd_bitrate_report)) {
        /* Arrives on the media channel (observer role) from every client's
         * UDP thread; the most recent report drives the shared encoder. */
        const grd_bitrate_report *report = payload;
        (void)SDL_SetAtomicInt(
            &app->abr_loss_percent, (int)report->loss_percent
        );
        (void)SDL_SetAtomicInt(
            &app->abr_rtt_micros,
            report->rtt_micros > (uint32_t)INT_MAX
                ? INT_MAX
                : (int)report->rtt_micros
        );
        (void)SDL_SetAtomicInt(&app->abr_report_updated, 1);
        return false;
    }
    if (type == GRD_PACKET_VIDEO_CAPS &&
        payload_length == sizeof(grd_video_caps)) {
        const grd_video_caps *caps = payload;
        (void)SDL_SetAtomicInt(
            &app->client_codec_caps, (int)caps->codec_bitmask
        );
        info_throttled(
            "host: client video caps 0x%x", caps->codec_bitmask
        );
        return false;
    }
    if (type == GRD_PACKET_REQUEST_KEYFRAME && payload_length == 0U) {
        /* Observers lose the reference chain too: a missing IDR affects the
         * shared stream, so honor requests from any role. */
        (void)SDL_SetAtomicInt(&app->keyframe_requested, 1);
        info_throttled("host: keyframe requested by client");
        return false;
    }
    if (role != GRD_ROLE_CONTROLLER) {
        return false;
    }
    if (type == GRD_PACKET_DISPLAY_CAPS &&
        payload_length == sizeof(grd_display_caps)) {
        const grd_display_caps *caps = payload;
        uint32_t fps = caps->max_fps;
        if (fps < 30U) {
            fps = 30U;
        }
        if (fps > 120U) {
            fps = 120U;
        }
        (void)SDL_SetAtomicInt(&app->requested_host_fps, (int)fps);
        uint32_t width = caps->width;
        uint32_t height = caps->height;
        if (width < 640U || height < 360U) {
            width = 1920U;
            height = 1080U;
        }
        if (width > 3840U) {
            width = 3840U;
        }
        if (height > 2160U) {
            height = 2160U;
        }
        atomic_store_explicit(
            &app->requested_host_width, width, memory_order_relaxed
        );
        atomic_store_explicit(
            &app->requested_host_height, height, memory_order_relaxed
        );
        const int upscale_mode =
            caps->upscale_mode <= GRD_CLIENT_UPSCALE_PERFORMANCE
                ? (int)caps->upscale_mode
                : (int)GRD_CLIENT_UPSCALE_NATIVE;
        const int offload_flags =
            (int)(caps->offload_flags & GRD_CLIENT_OFFLOAD_KNOWN_MASK);
        (void)SDL_SetAtomicInt(
            &app->requested_client_upscale_mode, upscale_mode
        );
        (void)SDL_SetAtomicInt(
            &app->requested_client_offload_flags, offload_flags
        );
        GRD_INFO(
        "host display caps: stream %u FPS, max resolution %ux%u, "
            "upscale client=%d, offload=0x%02x",
            fps,
            width,
            height,
            upscale_mode,
            (unsigned)offload_flags
        );
    } else if (type == GRD_PACKET_INPUT &&
               payload_length == sizeof(grd_input_event)) {
        const grd_input_event *event = payload;
        if (app->selected_monitor < app->monitor_count) {
            grd_error error = {0};
            const grd_status status = grd_platform_inject(
                &app->monitors[app->selected_monitor],
                event,
                &error
            );
            if (event->kind == GRD_INPUT_KEY &&
                (event->code == GRD_KEY_LEFT_CTRL ||
                 event->code == GRD_KEY_RIGHT_CTRL)) {
                GRD_INFO(
                    "host input Ctrl: %s status=%d",
                    event->pressed ? "down" : "up",
                    (int)status
                );
            }
            if (status != GRD_OK) {
                if (error.code == GRD_OK) {
                    error.code = status;
                    (void)snprintf(
                        error.message,
                        sizeof(error.message),
                        "Windows input rejected"
                    );
                }
                publish_error(app, &error);
                set_status(app, error.message);
                report_host_input_failure(app, &error);
                signal_main_thread(app);
            }
        }
    } else if (type == GRD_PACKET_CLIPBOARD &&
               payload_length <= GRD_MAX_CLIPBOARD) {
        char *text = malloc(payload_length + 1U);
        if (text != NULL) {
            memcpy(text, payload, payload_length);
            text[payload_length] = '\0';
            clipboard_remember(app, text);
            grd_error error = {0};
            (void)grd_platform_clipboard_write(text, &error);
            publish_error(app, &error);
            free(text);
        }
    }
    return false;
}

void grd_app_configure_display(grd_app *app)
{
    if (app == NULL || app->window == NULL) {
        return;
    }
    const SDL_DisplayID display = SDL_GetDisplayForWindow(app->window);
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display);
    const float current_refresh_rate =
        mode != NULL && mode->refresh_rate > 0.0F
            ? mode->refresh_rate
            : 60.0F;
    /* ProMotion can be in a 60 Hz power-saving state when the window is
     * inspected. Advertise the highest mode supported by the panel so the
     * host can produce 120 fps (8.33 ms cadence) when the compositor allows
     * it; SDL/Metal still clamps the actual present rate to the display. */
    float maximum_refresh_rate = current_refresh_rate;
    int mode_count = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &mode_count);
    if (modes != NULL) {
        for (int index = 0; index < mode_count; ++index) {
            if (modes[index] != NULL &&
                modes[index]->refresh_rate > maximum_refresh_rate) {
                maximum_refresh_rate = modes[index]->refresh_rate;
            }
        }
        SDL_free(modes);
    }
    app->display_refresh_rate = current_refresh_rate;
    app->display_max_refresh_rate = maximum_refresh_rate;
    const uint32_t maximum_hz = clamp_stream_rate(
        (uint32_t)(app->display_max_refresh_rate + 0.5F)
    );
    const uint32_t preferred_hz = app->config.presentation_hz;
    uint32_t target_fps = preferred_hz == 0U
                              ? maximum_hz
                              : clamp_stream_rate(preferred_hz);
    if (target_fps > maximum_hz) {
        target_fps = maximum_hz;
    }
    if (!app->config.client_frame_pacing) {
        const uint32_t stream_fps = effective_client_stream_fps(app);
        if (target_fps > stream_fps) {
            target_fps = stream_fps;
        }
    }
    app->display_target_fps = target_fps;
    GRD_INFO(
        "client display: current mode %.2f Hz, maximum %.2f Hz, "
        "requested presentation %u Hz (%s, local pacing %s)",
        (double)app->display_refresh_rate,
        (double)app->display_max_refresh_rate,
        app->display_target_fps,
        preferred_hz == 0U ? "automatic ProMotion" : "manual",
        app->config.client_frame_pacing ? "enabled" : "disabled"
    );
}

static uint32_t effective_client_stream_fps(const grd_app *app)
{
    uint32_t requested_fps = clamp_stream_rate(
        app != NULL ? app->config.client_target_fps : 60U
    );
    const uint32_t display_max_fps = clamp_stream_rate(
        app != NULL
            ? (uint32_t)(app->display_max_refresh_rate + 0.5F)
            : 60U
    );
    if (requested_fps > display_max_fps) {
        requested_fps = display_max_fps;
    }
    return requested_fps;
}

static uint8_t client_offload_flags(const grd_app *app)
{
    if (app == NULL) {
        return 0U;
    }
    uint8_t flags = 0U;
    if (app->config.client_frame_pacing) {
        flags |= GRD_CLIENT_OFFLOAD_FRAME_PACING;
    }
    if (app->config.client_cursor_prediction) {
        flags |= GRD_CLIENT_OFFLOAD_CURSOR_PREDICTION;
    }
    if (app->config.sharp_video_scaling) {
        flags |= GRD_CLIENT_OFFLOAD_SHARP_SCALING;
    }
    return flags;
}

static void requested_client_stream_dimensions(
    const grd_app *app,
    uint32_t *requested_width,
    uint32_t *requested_height
)
{
    if (requested_width == NULL || requested_height == NULL) {
        return;
    }
    int display_width = 0;
    int display_height = 0;
    if (app != NULL && app->window != NULL) {
        (void)SDL_GetWindowSizeInPixels(
            app->window, &display_width, &display_height
        );
    }
    *requested_width = display_width > 0 ? (uint32_t)display_width : 1920U;
    *requested_height = display_height > 0 ? (uint32_t)display_height : 1080U;
    if (app != NULL && app->config.client_max_height != 0U) {
        *requested_height = app->config.client_max_height;
        *requested_width = *requested_height == 2160U
                               ? 3840U
                               : *requested_height == 1440U ? 2560U : 1920U;
    } else {
        if (*requested_width > 3840U) {
            *requested_width = 3840U;
        }
        if (*requested_height > 2160U) {
            *requested_height = 2160U;
        }
    }
}

static bool send_display_capabilities(grd_app *app)
{
    if (app == NULL || app->connection == NULL ||
        !grd_connection_is_active(app->connection)) {
        return false;
    }
    uint32_t requested_width = 0U;
    uint32_t requested_height = 0U;
    requested_client_stream_dimensions(
        app, &requested_width, &requested_height
    );
    const uint32_t requested_fps = effective_client_stream_fps(app);
    const grd_display_caps caps = {
        .max_fps = requested_fps,
        .width = requested_width,
        .height = requested_height,
        .high_refresh = app->display_target_fps >= 100U ? 1U : 0U,
        .upscale_mode = (uint8_t)app->config.client_upscale_mode,
        .offload_flags = client_offload_flags(app),
        .reserved = 0U
    };
    grd_error error = {0};
    const grd_status status = grd_connection_send(
        app->connection,
        GRD_PACKET_DISPLAY_CAPS,
        &caps,
        sizeof(caps),
        &error
    );
    if (status != GRD_OK) {
        publish_error(app, &error);
        return false;
    }
    GRD_INFO(
        "client display caps: requested stream %u FPS, max %ux%u, "
        "presentation %u Hz, panel max %.0f Hz, upscale=%u, "
        "offload=0x%02x",
        requested_fps,
        requested_width,
        requested_height,
        app->display_target_fps,
        (double)app->display_max_refresh_rate,
        (unsigned)caps.upscale_mode,
        (unsigned)caps.offload_flags
    );
    return true;
}

void grd_app_handle_display_change(grd_app *app)
{
    if (app == NULL) {
        return;
    }
    grd_app_configure_display(app);
    if (app->connection != NULL) {
        (void)send_display_capabilities(app);
    }
}

static void decode_audio_packet(
    grd_app *app,
    const uint8_t *payload,
    size_t payload_length
)
{
    if (payload_length <= AUDIO_WIRE_PREFIX || app->audio_mutex == NULL) {
        return;
    }
    SDL_LockMutex(app->audio_mutex);
    if (app->audio_decoder == NULL || app->audio_playback == NULL) {
        SDL_UnlockMutex(app->audio_mutex);
        return;
    }
    const uint8_t *wire = payload;
    grd_decoded_audio decoded;
    grd_error error = {0};
    if (grd_audio_decode(
            app->audio_decoder,
            wire + AUDIO_WIRE_PREFIX,
            payload_length - AUDIO_WIRE_PREFIX,
            read_u64(wire),
            &decoded,
            &error
    ) != GRD_OK) {
        publish_error(app, &error);
        SDL_UnlockMutex(app->audio_mutex);
        return;
    }
    const int maximum_queue =
        (int)(GRD_AUDIO_SAMPLE_RATE * GRD_AUDIO_CHANNELS *
              sizeof(float) * 60U / 1000U);
    if (SDL_GetAudioStreamQueued(app->audio_playback) > maximum_queue) {
        (void)SDL_ClearAudioStream(app->audio_playback);
    }
    (void)SDL_PutAudioStreamData(
        app->audio_playback,
        decoded.samples,
        (int)(decoded.frames * GRD_AUDIO_CHANNELS * sizeof(float))
    );
    grd_decoded_audio_release(&decoded);
    SDL_UnlockMutex(app->audio_mutex);
}

static void owned_payload_free(void *payload)
{
    free(payload);
}

/* Asks the host for a fresh IDR over the reliable control channel. The
 * pending flag throttles duplicates; keyframe_requested_micros lets the
 * decode loop re-request when the answer does not arrive. */
static void request_remote_keyframe(grd_app *app)
{
    if (app == NULL || app->connection == NULL ||
        !grd_connection_is_active(app->connection)) {
        return;
    }
    if (SDL_CompareAndSwapAtomicInt(&app->keyframe_request_pending, 0, 1)) {
        atomic_store_explicit(
            &app->keyframe_requested_micros,
            grd_now_micros(),
            memory_order_release
        );
        (void)grd_connection_send(
            app->connection,
            GRD_PACKET_REQUEST_KEYFRAME,
            NULL,
            0U,
            NULL
        );
    }
}

/* Re-send a pending repair request with the same bounded cadence used by the
 * decoder. This also runs in the network-side P-frame gate: if the first IDR
 * itself was lost, no packet would otherwise reach decode_video_packet() to
 * drive its retry timer. */
static void retry_remote_keyframe_if_due(grd_app *app)
{
    if (app == NULL || app->connection == NULL ||
        !grd_connection_is_active(app->connection) ||
        SDL_GetAtomicInt(&app->keyframe_request_pending) == 0) {
        return;
    }
    const uint64_t now = grd_now_micros();
    const uint64_t requested_micros = atomic_load_explicit(
        &app->keyframe_requested_micros, memory_order_acquire
    );
    const uint64_t re_request_interval =
        app->keyframe_request_interval_us != 0U
            ? (uint64_t)app->keyframe_request_interval_us
            : 500000ULL;
    if (requested_micros == 0U || now < requested_micros ||
        now - requested_micros < re_request_interval) {
        return;
    }
    atomic_store_explicit(
        &app->keyframe_requested_micros, now, memory_order_release
    );
    (void)grd_connection_send(
        app->connection,
        GRD_PACKET_REQUEST_KEYFRAME,
        NULL,
        0U,
        NULL
    );
}

/* Returns true when the payload buffer was handed to the decoder (consumed);
 * the caller must not release it afterwards. */
static bool decode_video_packet(
    grd_app *app,
    const uint8_t *payload,
    size_t payload_length,
    bool padded,
    grd_status *out_status
)
{
    if (out_status != NULL) {
        *out_status = GRD_WOULD_BLOCK;
    }
    if (payload_length <= VIDEO_WIRE_PREFIX || app->decoder_mutex == NULL) {
        if (out_status != NULL) {
            *out_status = GRD_INVALID_ARGUMENT;
        }
        return false;
    }
    SDL_LockMutex(app->decoder_mutex);
    if (app->decoder == NULL) {
        if (out_status != NULL) {
            *out_status = GRD_WOULD_BLOCK;
        }
        SDL_UnlockMutex(app->decoder_mutex);
        return false;
    }
    const uint8_t *wire = payload;
    const uint64_t timestamp = read_u64(wire);
    const uint32_t width = read_u32(wire + 8U);
    const uint32_t height = read_u32(wire + 12U);
    const bool keyframe = wire[16U] != 0U;
    const uint64_t frame_id = read_u64(wire + 17U);
    /* Keep re-requesting the IDR (500 ms -> 1 s backoff) until one decodes
     * and clears the flag. Faster requests saturated the host with forced
     * keyframes; P-frames are gated below while the chain is invalid. */
    retry_remote_keyframe_if_due(app);
    /* A transport gap invalidates the inter-frame reference chain. Feeding
     * later P-frames to VideoToolbox only produces kVTVideoDecoderBadDataErr
     * storms and delays the requested IDR behind useless work. Keep the last
     * good texture visible and discard P-frames until a complete keyframe
     * can rebuild the decoder cleanly. */
    if (!keyframe &&
        SDL_GetAtomicInt(&app->keyframe_request_pending) != 0) {
        atomic_fetch_add_explicit(
            &app->pending_dropped_frames, 1U, memory_order_relaxed
        );
        if (out_status != NULL) {
            *out_status = GRD_BUSY;
        }
        SDL_UnlockMutex(app->decoder_mutex);
        return false;
    }
    grd_frame decoded;
    grd_error error = {0};
    /* Native VideoToolbox decodes synchronously from the caller buffer; the
     * FFmpeg decoder instead adopts the padded transport buffer, avoiding
     * the av_new_packet + memcpy input copy. */
    /* Only buffers delivered by the UDP reassembler carry the zeroed
     * padding tail required for adoption; anything else must be copied. */
    const bool can_adopt =
        padded && app->decoder_pipeline != GRD_PIPELINE_METAL_VIDEOTOOLBOX;
    bool consumed = can_adopt;
    grd_status status;
    if (can_adopt) {
        status = grd_decoder_decode_owned(
            app->decoder,
            (uint8_t *)wire + VIDEO_WIRE_PREFIX,
            payload_length - VIDEO_WIRE_PREFIX,
            (uint8_t *)wire,
            payload_length + GRD_MEDIA_BUFFER_PADDING,
            owned_payload_free,
            &decoded,
            &error
        );
    } else {
        status = grd_decoder_decode(
            app->decoder,
            wire + VIDEO_WIRE_PREFIX,
            payload_length - VIDEO_WIRE_PREFIX,
            &decoded,
            &error
        );
    }
    publish_error(app, &error);
    if (status != GRD_OK && status != GRD_WOULD_BLOCK &&
        app->decoder_pipeline == GRD_PIPELINE_METAL_VIDEOTOOLBOX) {
        /* No FFmpeg fallback on the native macOS path: a software H.264
         * decode at 1080p60 costs a full core and adds the very lag the
         * hardware path avoids. Recreate immediately on the first rejected
         * frame, then gate P-frames until the requested IDR. Throttling this
         * reset left a poisoned session alive for two seconds. */
        GRD_WARN(
            "VideoToolbox rejected frame=%llu key=%d bytes=%zu "
            "(%s, code %d): recreating the VideoToolbox session "
            "(no FFmpeg fallback)",
            (unsigned long long)frame_id,
            keyframe ? 1 : 0,
            payload_length - VIDEO_WIRE_PREFIX,
            error.message[0] != '\0' ? error.message : "error",
            (int)error.code
        );
        app->last_vt_recreate_micros = grd_now_micros();
        grd_decoder_destroy(app->decoder);
        memset(&error, 0, sizeof(error));
        app->decoder = grd_decoder_create(
            app->selected_pipeline,
            app->decoder_codec,
            &app->decoder_pipeline,
            &error
        );
        (void)SDL_SetAtomicInt(
            &app->remote_decoder_ready, app->decoder != NULL ? 1 : 0
        );
        publish_error(app, &error);
        if (app->decoder != NULL &&
            app->decoder_pipeline ==
                GRD_PIPELINE_METAL_VIDEOTOOLBOX &&
            !app->metal_renderer) {
            grd_decoder_set_bgra_output(app->decoder);
        }
        app->vt_stall_since_micros = 0ULL;
        app->vt_stall_streak = 0U;
        request_remote_keyframe(app);
    }
    if (out_status != NULL) {
        *out_status = status;
    }
    if (status != GRD_OK) {
        if (status != GRD_WOULD_BLOCK) {
            atomic_fetch_add_explicit(
                &app->decode_failures, 1U, memory_order_relaxed
            );
        }
        /* VideoToolbox stall watchdog: if the native decoder keeps saying
         * WOULD_BLOCK while frames keep arriving (no session, no callback
         * output), recreate the VideoToolbox session after ~3 s instead of
         * falling back to the software decoder (no FFmpeg on macOS). */
        if (status == GRD_WOULD_BLOCK &&
            app->decoder_pipeline == GRD_PIPELINE_METAL_VIDEOTOOLBOX) {
            /* A fresh VideoToolbox decoder has no session until a keyframe
             * carrying SPS/PPS arrives. While it waits, keep requesting
             * IDRs (throttled with backoff) so a dropped or
             * parameter-less first keyframe cannot leave the native
             * decoder stuck for the whole 3 s watchdog. A normal in-flight
             * async decode does NOT request: that would freeze the stream
             * while the decoder is merely catching up. */
            if (grd_decoder_needs_parameter_sets(app->decoder)) {
                request_remote_keyframe(app);
            }
            const uint64_t stall_now = grd_now_micros();
            if (app->vt_stall_since_micros == 0ULL) {
                app->vt_stall_since_micros = stall_now;
            }
            ++app->vt_stall_streak;
            if (stall_now - app->vt_stall_since_micros >= 3000000ULL) {
                app->vt_stall_streak = 0U;
                app->vt_stall_since_micros = 0ULL;
                GRD_WARN(
                    "VideoToolbox produced no output for ~3 s: recreating "
                    "the VideoToolbox session (no FFmpeg fallback)"
                );
                const uint64_t recreate_now = grd_now_micros();
                if (app->last_vt_recreate_micros == 0ULL ||
                    recreate_now - app->last_vt_recreate_micros >=
                        2000000ULL) {
                    app->last_vt_recreate_micros = recreate_now;
                    grd_decoder_destroy(app->decoder);
                    app->decoder = grd_decoder_create(
                        app->selected_pipeline,
                        app->decoder_codec,
                        &app->decoder_pipeline,
                        &error
                    );
                    (void)SDL_SetAtomicInt(
                        &app->remote_decoder_ready,
                        app->decoder != NULL ? 1 : 0
                    );
                    publish_error(app, &error);
                    if (app->decoder != NULL &&
                        app->decoder_pipeline ==
                            GRD_PIPELINE_METAL_VIDEOTOOLBOX &&
                        !app->metal_renderer) {
                        grd_decoder_set_bgra_output(app->decoder);
                    }
                }
                if (app->decoder != NULL) {
                    request_remote_keyframe(app);
                }
            }
        } else {
            app->vt_stall_streak = 0U;
            app->vt_stall_since_micros = 0ULL;
        }
        if (keyframe && status != GRD_WOULD_BLOCK) {
            /* Keyframe backoff: a decoder that cannot consume keyframes must
             * not re-request every 500 ms forever (it saturates the host with
             * forced IDRs: observed key 35-47 per 5 s and 16-51% drops).
             * Double the interval up to 1 s on failure. */
            uint32_t next_interval =
                app->keyframe_request_interval_us != 0U
                    ? app->keyframe_request_interval_us * 2U
                    : 500000U;
            if (next_interval > 1000000U) {
                next_interval = 1000000U;
            }
            app->keyframe_request_interval_us = next_interval;
        }
        if (status != GRD_WOULD_BLOCK) {
            warn_throttled(
                "video decode failed: %s (code %d)",
                error.message[0] != '\0' ? error.message : "unknown",
                (int)error.code
            );
        }
        /* A dropped P-frame can leave a decoder waiting for its reference.
         * The next IDR repairs the chain; do not publish stale video. A
         * slow native VideoToolbox decoder that returns WOULD_BLOCK is just
         * behind on its asynchronous output, not broken: requesting a
         * keyframe there would freeze the stream while it waits. */
        if (!keyframe &&
            (status != GRD_WOULD_BLOCK ||
             app->decoder_pipeline != GRD_PIPELINE_METAL_VIDEOTOOLBOX)) {
            request_remote_keyframe(app);
        }
        SDL_UnlockMutex(app->decoder_mutex);
        return consumed;
    }
    atomic_fetch_add_explicit(
        &app->decoded_frames, 1U, memory_order_relaxed
    );
    app->vt_stall_streak = 0U;
    app->vt_stall_since_micros = 0ULL;
    if (keyframe) {
        (void)SDL_SetAtomicInt(&app->keyframe_request_pending, 0);
        app->keyframe_request_interval_us = 500000U;
    }
    if (decoded.width != width || decoded.height != height) {
        grd_platform_frame_release(&decoded);
        SDL_UnlockMutex(app->decoder_mutex);
        return consumed;
    }
    decoded.timestamp_micros = timestamp;
    SDL_UnlockMutex(app->decoder_mutex);
    SDL_LockMutex(app->frame_mutex);
    grd_platform_frame_release(&app->remote_frame);
    app->remote_frame = decoded;
    ++app->remote_frame_generation;
    SDL_UnlockMutex(app->frame_mutex);
    /* The network thread may have already consumed its wake event before the
     * decoder finished. Wake the renderer again for the actual frame handoff. */
    signal_main_thread(app);
    return consumed;
}

static int video_decode_thread(void *userdata)
{
    grd_app *app = userdata;
    set_media_thread_priority();
    grd_media_packet packet;
    uint64_t stats_window_start = 0U;
    uint64_t stats_packets = 0U;
    /* When the native decoder is busy (frame in flight / waiting for
     * parameter sets), the current packet is held and retried instead of
     * being dropped: dropping it is what turns a brief decoder stall into
     * a visible freeze. Keyframes may be held indefinitely (they must reach
     * the decoder); P-frames get a bounded retry so they cannot starve a
     * keyframe waiting behind them in the queue. */
    grd_media_packet pending_frame;
    memset(&pending_frame, 0, sizeof(pending_frame));
    uint32_t pending_retries = 0U;
    uint64_t input_udp_at = 0U;
    uint64_t input_tcp_at = 0U;
    uint64_t input_failures_at = 0U;
    uint64_t input_rejections_at = 0U;
    app->health_video_decode.name = "video-decode";
    for (;;) {
        atomic_store_explicit(
            &app->health_video_decode.last_active_micros,
            grd_now_micros(),
            memory_order_relaxed
        );
        if (pending_frame.payload != NULL) {
            /* Retry the held frame BEFORE newer frames so ordering is kept. */
            packet = pending_frame;
            memset(&pending_frame, 0, sizeof(pending_frame));
        } else if (!media_queue_pop(&app->video_queue, &packet)) {
            break;
        }
        ++stats_packets;
        grd_status decode_status = GRD_WOULD_BLOCK;
        const bool consumed = decode_video_packet(
            app,
            packet.payload,
            packet.payload_length,
            packet.padded,
            &decode_status
        );
        const bool is_keyframe =
            packet.payload_length >= 25U && packet.payload[16U] != 0U;
        /* Keyframes get a much larger hold budget than P-frames: a fresh
         * VideoToolbox session can take longer than 20 ms to produce the
         * first output, and dropping the IDR while its decode is still in
         * flight breaks the whole reference chain (the following P-frames
         * then fail with kVTVideoDecoderBadDataErr). */
        const uint32_t retry_budget = is_keyframe ? 200U : 40U;
        if (!consumed && decode_status == GRD_WOULD_BLOCK &&
            pending_retries < retry_budget) {
            /* Decoder busy: hold the packet and retry (P-frames up to
             * ~20 ms, keyframes up to ~100 ms). The VideoToolbox slot is
             * self-healing (250 ms stuck recovery), so a bounded hold
             * smooths brief stalls without ever spinning on the same frame
             * forever. */
            pending_frame = packet;
            memset(&packet, 0, sizeof(packet));
            ++pending_retries;
            SDL_DelayPrecise(500000ULL);
        } else {
            pending_retries = 0U;
            if (!consumed) {
                media_packet_release(&packet);
            }
        }
        const uint64_t now = grd_now_micros();
        if (stats_window_start == 0U) {
            stats_window_start = now;
        }
        if (now - stats_window_start >= 5000000ULL) {
            const double seconds =
                (double)(now - stats_window_start) / 1000000.0;
            SDL_LockMutex(app->video_queue.mutex);
            const uint64_t dropped = app->video_queue.dropped;
            SDL_UnlockMutex(app->video_queue.mutex);
            GRD_INFO(
                "client dec: %.1f packets/s, media queue drops %llu, "
                "decoded %llu, failed %llu, pending-drop %llu, "
                "presented %llu, upload-fail %llu, thr dec=%llu ms "
                "aud=%llu ms%s, src-skip=%llu arrival-gaps=%llu "
                "source-recover=%llu max-arrival=%.1f ms "
                "max-source=%.1f ms",
                (double)stats_packets / seconds,
                dropped,
                (unsigned long long)atomic_load_explicit(
                    &app->decoded_frames, memory_order_relaxed
                ),
                (unsigned long long)atomic_load_explicit(
                    &app->decode_failures, memory_order_relaxed
                ),
                (unsigned long long)atomic_load_explicit(
                    &app->pending_dropped_frames, memory_order_relaxed
                ),
                (unsigned long long)atomic_load_explicit(
                    &app->presented_frames, memory_order_relaxed
                ),
                (unsigned long long)atomic_load_explicit(
                    &app->upload_failures, memory_order_relaxed
                ),
                (unsigned long long)(
                    (grd_now_micros() -
                     atomic_load_explicit(
                         &app->health_video_decode.last_active_micros,
                         memory_order_relaxed
                     )) /
                    1000ULL
                ),
                (unsigned long long)(
                    (grd_now_micros() -
                     atomic_load_explicit(
                         &app->health_audio_decode.last_active_micros,
                         memory_order_relaxed
                     )) /
                    1000ULL
                ),
                atomic_load_explicit(
                    &app->health_audio_decode.last_active_micros,
                    memory_order_relaxed
                ) != 0ULL &&
                        (grd_now_micros() -
                         atomic_load_explicit(
                             &app->health_audio_decode.last_active_micros,
                             memory_order_relaxed
                         )) >
                            2000000ULL
                    ? " STALLED"
                    : "",
                (unsigned long long)atomic_load_explicit(
                    &app->remote_source_skipped_frames,
                    memory_order_relaxed
                ),
                (unsigned long long)atomic_load_explicit(
                    &app->remote_arrival_gap_count,
                    memory_order_relaxed
                ),
                (unsigned long long)atomic_load_explicit(
                    &app->remote_source_gap_recoveries,
                    memory_order_relaxed
                ),
                (double)atomic_load_explicit(
                    &app->remote_arrival_max_gap_us,
                    memory_order_relaxed
                ) / 1000.0,
                (double)atomic_load_explicit(
                    &app->remote_source_max_gap_us,
                    memory_order_relaxed
                ) / 1000.0
            );
            const uint64_t input_udp = atomic_load_explicit(
                &app->relative_input_udp_sent, memory_order_relaxed
            );
            const uint64_t input_tcp = atomic_load_explicit(
                &app->relative_input_tcp_sent, memory_order_relaxed
            );
            const uint64_t input_failures = atomic_load_explicit(
                &app->relative_input_send_failures, memory_order_relaxed
            );
            const uint64_t input_rejections = atomic_load_explicit(
                &app->remote_input_rejection_reports, memory_order_relaxed
            );
            GRD_INFO(
                "client input: mouse %.1f samples/s, udp %llu, "
                "tcp-fallback %llu, send-failed %llu, host-rejected %llu, "
                "mode %s, sensitivity %.2fx",
                (double)((input_udp - input_udp_at) +
                         (input_tcp - input_tcp_at)) /
                    seconds,
                (unsigned long long)(input_udp - input_udp_at),
                (unsigned long long)(input_tcp - input_tcp_at),
                (unsigned long long)(input_failures - input_failures_at),
                (unsigned long long)(input_rejections - input_rejections_at),
                mouse_mode_name(app->config.mouse_mode),
                (double)app->config.mouse_sensitivity_percent / 100.0
            );
            input_udp_at = input_udp;
            input_tcp_at = input_tcp;
            input_failures_at = input_failures;
            input_rejections_at = input_rejections;
            log_thread_map(app);
            log_fallback_map(app);
            stats_window_start = now;
            stats_packets = 0U;
        }
    }
    return 0;
}

static int audio_decode_thread(void *userdata)
{
    grd_app *app = userdata;
    set_media_thread_priority();
    grd_media_packet packet;
    app->health_audio_decode.name = "audio-decode";
    while (media_queue_pop(&app->audio_queue, &packet)) {
        atomic_store_explicit(
            &app->health_audio_decode.last_active_micros,
            grd_now_micros(),
            memory_order_relaxed
        );
        decode_audio_packet(app, packet.payload, packet.payload_length);
        media_packet_release(&packet);
    }
    return 0;
}

static bool client_packet(
    grd_packet_type type,
    const void *payload,
    size_t payload_length,
    bool payload_takeable,
    grd_role role,
    void *userdata
)
{
    (void)role;
    grd_app *app = userdata;
    if (type == GRD_PACKET_ERROR && payload_length > 0U) {
        const size_t message_length = payload_length -
                                      (((const char *)payload)[payload_length - 1U] == '\0'
                                           ? 1U
                                           : 0U);
        grd_error error = {.code = GRD_IO_ERROR};
        (void)snprintf(
            error.message,
            sizeof(error.message),
            "%.*s",
            (int)(message_length < sizeof(error.message) - 1U
                      ? message_length
                      : sizeof(error.message) - 1U),
            (const char *)payload
        );
        const size_t input_prefix_length =
            sizeof(GRD_HOST_INPUT_ERROR_PREFIX) - 1U;
        if (message_length >= input_prefix_length &&
            memcmp(
                payload,
                GRD_HOST_INPUT_ERROR_PREFIX,
                input_prefix_length
            ) == 0) {
            atomic_fetch_add_explicit(
                &app->remote_input_rejection_reports,
                1U,
                memory_order_relaxed
            );
        }
        publish_error(app, &error);
        set_status(app, error.message);
        signal_main_thread(app);
        return false;
    }
    if (type == GRD_PACKET_REQUEST_KEYFRAME && payload_length == 0U) {
        /* UDP loss recovery is signalled by the media channel. Gate P-frames
         * and forward the request through reliable TCP. A valid IDR resets
         * the H.264 reference chain by definition; destroying VideoToolbox
         * here added a visible stall to every otherwise recoverable gap. */
        request_remote_keyframe(app);
        return false;
    }
    if (type == GRD_PACKET_AUDIO_CONFIG &&
        payload_length == sizeof(grd_audio_config)) {
        SDL_LockMutex(app->audio_mutex);
        const grd_audio_config *configuration = payload;
        if (configuration->sample_rate != GRD_AUDIO_SAMPLE_RATE ||
            configuration->channels != GRD_AUDIO_CHANNELS ||
            strcmp(configuration->codec, "opus") != 0) {
            SDL_UnlockMutex(app->audio_mutex);
            return false;
        }
        if (app->audio_decoder == NULL) {
            grd_error error = {0};
            app->audio_decoder = grd_audio_decoder_create(&error);
            publish_error(app, &error);
        }
        if (app->audio_playback == NULL && app->audio_decoder != NULL) {
            const SDL_AudioSpec specification = {
                .format = SDL_AUDIO_F32,
                .channels = GRD_AUDIO_CHANNELS,
                .freq = GRD_AUDIO_SAMPLE_RATE
            };
            app->audio_playback = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                &specification,
                NULL,
                NULL
            );
            if (app->audio_playback != NULL) {
                (void)SDL_ResumeAudioStreamDevice(app->audio_playback);
            }
        }
        SDL_UnlockMutex(app->audio_mutex);
        return false;
    }
    if (type == GRD_PACKET_AUDIO_FRAME &&
        payload_length > AUDIO_WIRE_PREFIX) {
        const bool queued = payload_takeable
                                ? media_queue_push_owned(
                                      &app->audio_queue,
                                      type,
                                      (uint8_t *)payload,
                                      payload_length,
                                      NULL
                                  )
                                : media_queue_push(
                                      &app->audio_queue,
                                      type,
                                      payload,
                                      payload_length,
                                      NULL
                                  );
        if (queued) {
            signal_main_thread(app);
        }
        return payload_takeable && queued;
    }
    if (type == GRD_PACKET_VIDEO_CONFIG &&
        payload_length == sizeof(grd_video_config)) {
        const grd_video_config *configuration = payload;
        const grd_video_codec video_codec =
            codec_from_name(configuration->codec);
        const uint32_t previous_fps = atomic_load_explicit(
            &app->remote_stream_fps, memory_order_relaxed
        );
        const uint32_t previous_bitrate = atomic_load_explicit(
            &app->remote_stream_bitrate_kbps, memory_order_relaxed
        );
        const uint32_t previous_width = atomic_load_explicit(
            &app->remote_stream_width, memory_order_relaxed
        );
        const uint32_t previous_height = atomic_load_explicit(
            &app->remote_stream_height, memory_order_relaxed
        );
        const grd_video_codec previous_codec = (grd_video_codec)
            SDL_GetAtomicInt(&app->remote_stream_codec);
        const bool decoder_config_changed =
            previous_width != configuration->width ||
            previous_height != configuration->height ||
            previous_codec != video_codec;
        const bool stream_config_changed =
            decoder_config_changed || previous_fps != configuration->fps ||
            previous_bitrate != configuration->bitrate_kbps;
        if (app->window != NULL && !app->remote_window_raised) {
            /* Safety net: the first config marks the stream start. The raise
             * must happen on the MAIN thread (AppKit requires it), so this
             * receive thread only sets a pending flag consumed by the main
             * loop; calling SDL_RaiseWindow here crashed with
             * 'Must only be used from the main thread'. */
            (void)SDL_SetAtomicInt(&app->remote_raise_pending, 1);
        }
        if (stream_config_changed) {
            GRD_INFO(
                "client video config: codec=%s %ux%u fps=%u bitrate=%u kbps",
                grd_codec_name(video_codec),
                configuration->width,
                configuration->height,
                configuration->fps,
                configuration->bitrate_kbps
            );
        }
        atomic_store_explicit(
            &app->remote_stream_fps,
            configuration->fps,
            memory_order_relaxed
        );
        atomic_store_explicit(
            &app->remote_stream_bitrate_kbps,
            configuration->bitrate_kbps,
            memory_order_relaxed
        );
        atomic_store_explicit(
            &app->remote_stream_width,
            configuration->width,
            memory_order_relaxed
        );
        atomic_store_explicit(
            &app->remote_stream_height,
            configuration->height,
            memory_order_relaxed
        );
        (void)SDL_SetAtomicInt(
            &app->remote_stream_codec, (int)video_codec
        );
        /* VIDEO_CONFIG is also the one-second telemetry heartbeat. Taking
         * decoder_mutex for an unchanged heartbeat made the UDP receive
         * thread wait behind a slow VideoToolbox call; one observed wait was
         * 2.85 s and stopped all datagram reads despite 0% network loss. */
        if (!decoder_config_changed &&
            SDL_GetAtomicInt(&app->remote_decoder_ready) != 0) {
            return false;
        }
        SDL_LockMutex(app->decoder_mutex);
        const bool dimensions_changed =
            app->decoder_width != configuration->width ||
            app->decoder_height != configuration->height;
        const bool codec_changed =
            app->decoder_codec != video_codec;
        app->decoder_codec = video_codec;
        if (app->decoder == NULL || dimensions_changed || codec_changed) {
            /* A fullscreen/mode switch can change the encoded dimensions.
             * Flush the old H.264 reference chain instead of dropping every
             * new frame because its size no longer matches the old decoder. */
            grd_decoder_destroy(app->decoder);
            app->decoder = NULL;
            app->decoder_width = configuration->width;
            app->decoder_height = configuration->height;
            grd_error error = {0};
            app->decoder = grd_decoder_create(
                app->selected_pipeline,
                video_codec,
                &app->decoder_pipeline,
                &error
            );
            publish_error(app, &error);
            GRD_INFO(
                "client decoder: codec=%s pipeline=%s %s",
                grd_codec_name(video_codec),
                pipeline_display_name(app->decoder_pipeline),
                app->decoder != NULL ? "ok" : "FAILED"
            );
            if (app->decoder != NULL &&
                app->decoder_pipeline == GRD_PIPELINE_METAL_VIDEOTOOLBOX &&
                !app->metal_renderer) {
                /* Software renderer cannot wrap IOSurface YUV buffers: ask
                 * VideoToolbox for single-plane BGRA output so the CPU
                 * upload path never interprets NV12/P010 as RGBA (that
                 * produced a green, split image). */
                grd_decoder_set_bgra_output(app->decoder);
            }
#if defined(_WIN32)
            if (app->decoder != NULL && app->d3d11_device != NULL) {
                grd_error d3d11_error = {0};
                (void)grd_decoder_enable_d3d11_output(
                    app->decoder, app->d3d11_device, &d3d11_error
                );
                publish_error(app, &d3d11_error);
            }
#endif
            if (app->decoder != NULL) {
                request_remote_keyframe(app);
            }
        }
        (void)SDL_SetAtomicInt(
            &app->remote_decoder_ready, app->decoder != NULL ? 1 : 0
        );
        SDL_UnlockMutex(app->decoder_mutex);
        return false;
    }
    if (type == GRD_PACKET_CURSOR &&
        payload_length == sizeof(grd_cursor_state)) {
        const grd_cursor_state *state = payload;
        SDL_LockMutex(app->cursor_mutex);
        app->remote_cursor_visible = state->visible != 0U;
        app->remote_cursor_x = state->x;
        app->remote_cursor_y = state->y;
        const float correction_x = app->predicted_cursor_x - state->x;
        const float correction_y = app->predicted_cursor_y - state->y;
        if (!app->predicted_cursor_valid ||
            correction_x * correction_x + correction_y * correction_y >
                0.01F) {
            app->predicted_cursor_x = state->x;
            app->predicted_cursor_y = state->y;
        }
        app->predicted_cursor_valid = true;
        SDL_UnlockMutex(app->cursor_mutex);
        signal_main_thread(app);
        return false;
    }
    if (type == GRD_PACKET_CURSOR_SHAPE &&
        payload_length == sizeof(grd_cursor_shape)) {
        const grd_cursor_shape *shape = payload;
        SDL_LockMutex(app->cursor_mutex);
        app->remote_cursor_shape = *shape;
        ++app->remote_cursor_shape_generation;
        SDL_UnlockMutex(app->cursor_mutex);
        signal_main_thread(app);
        return false;
    }
    if (type == GRD_PACKET_CLIPBOARD &&
        payload_length <= GRD_MAX_CLIPBOARD) {
        char *text = malloc(payload_length + 1U);
        if (text != NULL) {
            memcpy(text, payload, payload_length);
            text[payload_length] = '\0';
            clipboard_remember(app, text);
            grd_error error = {0};
            (void)grd_platform_clipboard_write(text, &error);
            publish_error(app, &error);
            free(text);
        }
        return false;
    }
    if (type != GRD_PACKET_VIDEO_FRAME ||
        payload_length <= VIDEO_WIRE_PREFIX) {
        return false;
    }
    atomic_fetch_add_explicit(
        &app->remote_video_bytes_received,
        payload_length - VIDEO_WIRE_PREFIX,
        memory_order_relaxed
    );
    const uint8_t *video_wire = payload;
    const uint64_t source_timestamp = read_u64(video_wire);
    const bool keyframe = video_wire[16U] != 0U;
    const uint64_t frame_id = read_u64(video_wire + 17U);
    const uint64_t arrival_micros = grd_now_micros();
    bool source_chain_gap = false;
    uint64_t source_skipped = 0ULL;
    uint64_t source_delta = 0ULL;
    if (app->remote_last_wire_frame_id != 0ULL) {
        if (frame_id > app->remote_last_wire_frame_id + 1ULL) {
            source_skipped =
                frame_id - app->remote_last_wire_frame_id - 1ULL;
            source_chain_gap = true;
            atomic_fetch_add_explicit(
                &app->remote_source_skipped_frames,
                source_skipped,
                memory_order_relaxed
            );
        }
        const uint64_t arrival_delta =
            arrival_micros - app->remote_last_wire_arrival_micros;
        if (source_timestamp > app->remote_last_source_timestamp_micros) {
            source_delta =
                source_timestamp - app->remote_last_source_timestamp_micros;
        }
        const uint32_t configured_fps = atomic_load_explicit(
            &app->remote_stream_fps, memory_order_relaxed
        );
        if (grd_stream_arrival_gap_is_late(
                arrival_delta, source_delta, configured_fps
            )) {
            atomic_fetch_add_explicit(
                &app->remote_arrival_gap_count, 1ULL, memory_order_relaxed
            );
        }
        atomic_update_max_u32(
            &app->remote_arrival_max_gap_us,
            arrival_delta > UINT32_MAX ? UINT32_MAX : (uint32_t)arrival_delta
        );
        if (source_delta != 0U) {
            atomic_update_max_u32(
                &app->remote_source_max_gap_us,
                source_delta > UINT32_MAX
                    ? UINT32_MAX
                    : (uint32_t)source_delta
            );
        }
    }
    app->remote_last_wire_frame_id = frame_id;
    app->remote_last_wire_arrival_micros = arrival_micros;
    app->remote_last_source_timestamp_micros = source_timestamp;
    if (source_chain_gap) {
        /* A jump means a frame admitted to the transport was omitted for
         * this client even when UDP packet loss is zero. Drop queued
         * dependants and gate the chain before VideoToolbox sees them. */
        media_queue_clear(&app->video_queue);
        atomic_fetch_add_explicit(
            &app->remote_source_gap_recoveries, 1ULL, memory_order_relaxed
        );
        if (keyframe) {
            /* The arriving IDR is already the repair. Keep the existing
             * hardware session and let the IDR replace its reference chain;
             * recreating VideoToolbox here caused the measured micro-lag. */
            (void)SDL_SetAtomicInt(&app->keyframe_request_pending, 1);
            (void)SDL_SetAtomicInt(
                &app->remote_waiting_for_repair_idr, 0
            );
            atomic_store_explicit(
                &app->keyframe_requested_micros,
                arrival_micros,
                memory_order_release
            );
        } else {
            (void)SDL_SetAtomicInt(
                &app->remote_waiting_for_repair_idr, 1
            );
            request_remote_keyframe(app);
        }
        GRD_WARN(
            "client source gap: frame=%llu skipped=%llu key=%d; "
            "%s",
            (unsigned long long)frame_id,
            (unsigned long long)source_skipped,
            keyframe ? 1 : 0,
            keyframe ? "IDR received without decoder reset"
                     : "waiting for recovery IDR"
        );
    }
    if (keyframe &&
        SDL_GetAtomicInt(&app->remote_waiting_for_repair_idr) != 0) {
        /* The requested repair is now in hand. Permit its following P-chain
         * to queue behind it; keyframe_request_pending remains set until the
         * decoder has successfully consumed this IDR. */
        (void)SDL_SetAtomicInt(&app->remote_waiting_for_repair_idr, 0);
    } else if (!keyframe &&
               SDL_GetAtomicInt(
                   &app->remote_waiting_for_repair_idr
               ) != 0) {
        /* Never let a frame whose missing reference is already known reach
         * VideoToolbox. The old path queued this first damaged P-frame and
         * produced the three observed kVTVideoDecoderBadDataErr failures. */
        atomic_fetch_add_explicit(
            &app->pending_dropped_frames, 1U, memory_order_relaxed
        );
        retry_remote_keyframe_if_due(app);
        return false;
    }
    bool local_gap = false;
    const bool queued = payload_takeable
                            ? media_queue_push_owned(
                                  &app->video_queue,
                                  type,
                                  (uint8_t *)payload,
                                  payload_length,
                                  &local_gap
                              )
                            : media_queue_push(
                                  &app->video_queue,
                                  type,
                                  payload,
                                  payload_length,
                                  &local_gap
                              );
    if (local_gap) {
        /* This drop happens after UDP reassembly, so transport loss remains
         * zero even though the H.264 reference chain is now broken. Gate
         * P-frames until a fresh IDR instead of feeding VideoToolbox a frame
         * that will fail with -12909. */
        if (keyframe) {
            /* The current IDR repairs the eviction. Older queued P-frames
             * are gated by keyframe_request_pending and discarded before the
             * decoder reaches this keyframe. */
            (void)SDL_SetAtomicInt(&app->keyframe_request_pending, 1);
            (void)SDL_SetAtomicInt(
                &app->remote_waiting_for_repair_idr, 0
            );
            atomic_store_explicit(
                &app->keyframe_requested_micros,
                arrival_micros,
                memory_order_release
            );
        } else {
            /* The queue already owns (or copied) the current damaged frame.
             * Clearing it consumes that buffer and prevents a stale P-chain
             * from racing the requested repair IDR. */
            media_queue_clear(&app->video_queue);
            (void)SDL_SetAtomicInt(
                &app->remote_waiting_for_repair_idr, 1
            );
            request_remote_keyframe(app);
        }
    }
    if (queued) {
        signal_main_thread(app);
    }
    return payload_takeable && queued;
}

static int cursor_thread(void *userdata)
{
    grd_app *app = userdata;
    set_media_thread_priority();
    grd_cursor_state last_cursor = {0};
    grd_cursor_shape last_cursor_shape = {0};
    bool initialized = false;
    app->health_cursor.name = "cursor";
    while (SDL_GetAtomicInt(&app->streaming) != 0) {
        atomic_store_explicit(
            &app->health_cursor.last_active_micros,
            grd_now_micros(),
            memory_order_relaxed
        );
        if (app->host == NULL || grd_host_client_count(app->host) == 0U ||
            app->selected_monitor >= app->monitor_count) {
            SDL_DelayPrecise(4000000ULL);
            continue;
        }
        grd_cursor_state cursor;
        grd_cursor_shape shape;
        grd_error error = {0};
        if (grd_platform_cursor_state(
                &app->monitors[app->selected_monitor],
                &cursor,
                &shape,
                &error
            ) == GRD_OK) {
            const bool shape_changed =
                !initialized || memcmp(&shape, &last_cursor_shape, sizeof(shape)) != 0;
            if (shape_changed) {
                (void)grd_host_broadcast(
                    app->host, GRD_PACKET_CURSOR_SHAPE,
                    &shape, sizeof(shape), NULL
                );
                last_cursor_shape = shape;
            }
            const bool cursor_changed =
                !initialized || cursor.visible != last_cursor.visible ||
                (cursor.visible &&
                 (cursor.x != last_cursor.x || cursor.y != last_cursor.y));
            if (cursor_changed) {
                (void)grd_host_broadcast(
                    app->host, GRD_PACKET_CURSOR,
                    &cursor, sizeof(cursor), NULL
                );
                last_cursor = cursor;
                initialized = true;
            }
        } else {
            publish_error(app, &error);
        }
        /* Sample independently of video pacing so the control overlay does
         * not inherit a 60/120 Hz capture cadence. */
        SDL_DelayPrecise(4000000ULL);
    }
    return 0;
}

static void reset_host_fps_pressure(
    grd_app *app,
    grd_stream_fps_pressure_state *state,
    uint32_t target_fps,
    uint64_t now_micros
)
{
    grd_stream_fps_pressure_reset(state, target_fps, now_micros);
    uint64_t current_generation = 0U;
    (void)grd_host_udp_initiating_drop_sample(
        app != NULL ? app->host : NULL, &current_generation
    );
    /* A reset changes the controller's baseline. Do not reinterpret the last
     * already-published one-second window as fresh pressure afterwards. */
    state->last_drop_sample_generation = current_generation;
}

static int stream_thread(void *userdata)
{
    grd_app *app = userdata;
    set_media_thread_priority();
    grd_encoder *encoder = NULL;
    grd_pipeline_kind active_pipeline = GRD_PIPELINE_SOFTWARE;
    grd_video_codec active_codec = GRD_CODEC_H264;
    uint32_t encoder_width = 0U;
    uint32_t encoder_height = 0U;
    uint32_t encoder_fps = 0U;
    uint32_t current_bitrate = 0U;
    uint32_t desired_bitrate = 0U;
    uint32_t encoder_bitrate_kbps = 0U;
    uint64_t last_encoder_rate_change_micros = 0U;
    uint64_t host_frame_counter = 0U;
    /* A clean LAN spends the parity reserve on picture quality. FEC is
     * enabled after sustained real packet loss and retained through a clean
     * recovery window; host-side pacer drops are handled separately and
     * must not permanently tax every Gaming frame. */
    bool fec_current = false;
    uint32_t abr_lossy_reports = 0U;
    uint32_t abr_self_lossy_reports = 0U;
    uint32_t abr_clean_reports = 0U;
    uint32_t abr_self_clean_reports = 0U;
    bool config_pending = false;
    uint64_t last_config = 0U;
    uint64_t stats_window_start = 0U;
    uint64_t stats_captured = 0U;
    uint64_t stats_encoded = 0U;
    uint64_t stats_encoded_bytes = 0U;
    uint64_t stats_blocked = 0U;
    uint64_t stats_encode_errors = 0U;
    uint64_t stats_zero_copy_frames = 0U;
    uint64_t stats_abr_changes = 0U;
    uint64_t stats_keyframes = 0U;
    uint64_t stats_recovery_suppressed = 0U;
    uint64_t abr_hold_until = 0U;
    uint32_t wire_budget_kbps = 0U;
    uint32_t applied_wire_kbps = 0U;
    uint64_t pacer_grace_until = 0U;
    uint64_t abr_last_increase_micros = 0U;
    uint64_t abr_recovery_stabilize_until = 0U;
    uint64_t next_frame_deadline = 0U;
    uint64_t stats_last_micros = 0U;
    uint32_t stats_frame_count = 0U;
    uint32_t client_ladder_level = 0U;
    uint64_t last_forced_idr_micros = 0U;
    uint8_t qp_ring[GRD_QP_RING_CAPACITY] = {0U};
    size_t qp_ring_next = 0U;
    uint8_t latest_qp_avg = 0U;
    uint8_t latest_qp_p95 = 0U;
    bool latest_qp_valid = false;
    uint32_t stream_fps = 0U;
    uint32_t negotiated_target_fps = 0U;
    uint32_t negotiated_max_width = 0U;
    uint32_t negotiated_max_height = 0U;
    grd_stream_fps_pressure_state fps_pressure_state;
    reset_host_fps_pressure(
        app, &fps_pressure_state, 60U, grd_now_micros()
    );
    grd_timing_window capture_acquire_timing = {0};
    grd_timing_window conversion_timing = {0};
    grd_timing_window send_frame_timing = {0};
    grd_timing_window receive_packet_timing = {0};
    grd_timing_window encoder_reopen_timing = {0};
    uint64_t stats_capture_wait_timeouts = 0U;
    uint64_t stats_capture_coalesced_frames = 0U;
#if defined(_WIN32)
    uint64_t capture_last_frame_micros = 0U;
    bool ignore_next_capture_interval = false;
    grd_capture_recovery_state capture_recovery_state;
    grd_capture_recovery_reset(&capture_recovery_state);
    grd_capture_watchdog_state capture_watchdog_state;
    grd_capture_watchdog_reset(&capture_watchdog_state);
    uint64_t capture_raw_gaps_at = 0U;
    uint64_t capture_episodes_at = 0U;
    uint64_t capture_planned_gaps_at = 0U;
    uint64_t stats_capture_session_resets = 0U;
    uint64_t stats_capture_device_resets = 0U;
#endif
    bool capture_recovery_pending = false;
    bool recovery_idr_pending = false;
    bool native_capture_disabled = false;
    /* Keep the display awake while streaming: a display that goes to sleep
     * makes CAMetalLayer refuse drawables, which surfaces as silent
     * SDL_UpdateTexture failures (empty SDL error) on the Metal renderer. */
    (void)SDL_DisableScreenSaver();
    grd_host_set_fec_enabled(app->host, fec_current);
    app->health_stream.name = "stream";
    while (SDL_GetAtomicInt(&app->streaming) != 0) {
        atomic_store_explicit(
            &app->health_stream.last_active_micros,
            grd_now_micros(),
            memory_order_relaxed
        );
        const uint64_t frame_started = grd_now_micros();
        if (app->host == NULL || grd_host_client_count(app->host) == 0U ||
            app->selected_monitor >= app->monitor_count) {
            /* A later client connection starts a fresh watchdog interval.
             * An idle desktop must never trigger a reset by itself. */
#if defined(_WIN32)
            capture_last_frame_micros = 0U;
            ignore_next_capture_interval = false;
            grd_capture_recovery_reset(&capture_recovery_state);
            grd_capture_watchdog_reset(&capture_watchdog_state);
            capture_raw_gaps_at = 0U;
            capture_episodes_at = 0U;
            capture_planned_gaps_at = 0U;
            stats_capture_session_resets = 0U;
            stats_capture_device_resets = 0U;
            stats_capture_coalesced_frames = 0U;
#endif
            capture_recovery_pending = false;
            recovery_idr_pending = false;
            if (negotiated_target_fps != 0U) {
                reset_host_fps_pressure(
                    app,
                    &fps_pressure_state,
                    negotiated_target_fps,
                    grd_now_micros()
                );
                stream_fps = negotiated_target_fps;
            }
            SDL_DelayPrecise(100000000ULL);
            continue;
        }
        uint32_t target_fps = app->config.target_fps < 30U
                                  ? 60U
                                  : clamp_stream_rate(app->config.target_fps);
        const int requested_fps = SDL_GetAtomicInt(&app->requested_host_fps);
        if (requested_fps >= 30 && requested_fps <= 120 &&
            (uint32_t)requested_fps < target_fps) {
            /* The configured target_fps caps the negotiated rate (default
             * 60): a 120 Hz client display must not double the host capture
             * and encode load behind a game. The client display refresh is
             * still respected as the upper bound when it is lower. */
            target_fps = (uint32_t)requested_fps;
        }
        if (target_fps != negotiated_target_fps) {
            GRD_INFO(
                "host frame rate: host limit %u FPS, client %d FPS, "
                "stream %u FPS",
                app->config.target_fps,
                requested_fps,
                target_fps
            );
            negotiated_target_fps = target_fps;
            reset_host_fps_pressure(
                app,
                &fps_pressure_state,
                target_fps,
                grd_now_micros()
            );
            stream_fps = fps_pressure_state.effective_fps;
        }
        uint32_t requested_width = atomic_load_explicit(
            &app->requested_host_width, memory_order_relaxed
        );
        uint32_t requested_height = atomic_load_explicit(
            &app->requested_host_height, memory_order_relaxed
        );
        if (requested_width < 640U || requested_height < 360U) {
            requested_width = 1920U;
            requested_height = 1080U;
        }
        if (requested_width != negotiated_max_width ||
            requested_height != negotiated_max_height) {
            negotiated_max_width = requested_width;
            negotiated_max_height = requested_height;
            GRD_INFO(
                "host resolution request: max %ux%u",
                negotiated_max_width,
                negotiated_max_height
            );
        }
        /* Congestion must not silently recreate NVENC/VideoToolbox. The host
         * resolution now changes only when the user explicitly requests
         * client-side offload; transient load is handled by the continuous
         * FPS controller and live bitrate reconfiguration. */
        const uint32_t requested_client_ladder_level =
            grd_stream_client_offload_level(
                SDL_GetAtomicInt(&app->requested_client_upscale_mode)
            );
        if (requested_client_ladder_level != client_ladder_level) {
            client_ladder_level = requested_client_ladder_level;
            GRD_INFO(
                "host client offload: resolution level %u (%s)",
                client_ladder_level,
                client_ladder_level == 0U
                          ? "native"
                    : client_ladder_level == 1U
                          ? "balanced"
                          : "performance"
            );
        }
        if (grd_host_video_recovery_queued(app->host)) {
            /* Do not feed NVENC a P-frame while the repair IDR is still being
             * transmitted. Encoding and then discarding that P-frame would
             * make it a hidden reference for the next frame and immediately
             * break the freshly repaired chain again. The pacer runs on its
             * own thread, so this short wait does not delay the IDR itself. */
#if defined(_WIN32)
            ignore_next_capture_interval = true;
#endif
            next_frame_deadline = 0U;
            SDL_DelayPrecise(500000ULL);
            continue;
        }
        grd_frame captured;
        grd_error error = {0};
        uint32_t capture_backlog_frames = 0U;
        const bool prefer_gpu_capture =
            !native_capture_disabled && encoder != NULL &&
            active_pipeline == GRD_PIPELINE_CUDA_NVENC;
        const grd_status capture_status = grd_platform_capture(
                app->monitors[app->selected_monitor].id,
                prefer_gpu_capture,
                &captured,
                &error
            );
#if defined(_WIN32)
        grd_capture_timing capture_timing = {0};
        grd_platform_capture_last_timing(&capture_timing);
        if (capture_timing.acquire_attempted) {
            timing_window_add(
                &capture_acquire_timing,
                capture_timing.acquire_wait_micros
            );
        }
        if (capture_timing.wait_timeout) {
            ++stats_capture_wait_timeouts;
        }
        if (capture_timing.accumulated_frames > 1U) {
            stats_capture_coalesced_frames +=
                (uint64_t)capture_timing.accumulated_frames - 1U;
        }
#endif
        if (capture_status != GRD_OK) {
#if defined(_WIN32)
            if (capture_status == GRD_WOULD_BLOCK) {
                if (capture_last_frame_micros == 0U) {
                    capture_last_frame_micros = frame_started;
                }
                const uint64_t stalled_for =
                    frame_started - capture_last_frame_micros;
                const grd_capture_watchdog_action watchdog_action =
                    grd_capture_watchdog_on_timeout(
                        &capture_watchdog_state,
                        capture_last_frame_micros,
                        frame_started,
                        capture_timing.driver_stalled
                    );
                if (watchdog_action != GRD_CAPTURE_WATCHDOG_NONE) {
                    const bool reset_device =
                        watchdog_action ==
                        GRD_CAPTURE_WATCHDOG_RESET_DEVICE;
                    grd_platform_capture_reset(reset_device);
                    capture_recovery_pending = true;
                    (void)grd_capture_recovery_force(
                        &capture_recovery_state
                    );
                    ignore_next_capture_interval = true;
                    if (reset_device) {
                        ++stats_capture_device_resets;
                    } else {
                        ++stats_capture_session_resets;
                    }
                    GRD_WARN(
                        "host: Windows capture active but stalled for %llu ms; "
                        "%s",
                        (unsigned long long)(stalled_for / 1000ULL),
                        reset_device
                            ? "full D3D11 reset"
                            : "recreate Desktop Duplication only"
                    );
                    set_status(
                        app,
                        reset_device
                            ? "Capture stalled: full D3D11 reset"
                            : "Capture stalled: renew Desktop Duplication"
                    );
                    signal_main_thread(app);
                }
            } else if (capture_status == GRD_IO_ERROR) {
                /* The platform has already invalidated exactly the resource
                 * named by reset_scope. Do not turn ACCESS_LOST (which only
                 * requires a new duplication object) into an unnecessary
                 * D3D/CUDA device rebuild here. */
                const bool reset_device =
                    capture_timing.reset_scope !=
                    GRD_CAPTURE_RESET_SESSION;
                if (reset_device) {
                    grd_capture_watchdog_mark_device_reset(
                        &capture_watchdog_state, frame_started
                    );
                    ++stats_capture_device_resets;
                } else {
                    grd_capture_watchdog_mark_session_reset(
                        &capture_watchdog_state, frame_started
                    );
                    ++stats_capture_session_resets;
                }
                capture_recovery_pending = true;
                (void)grd_capture_recovery_force(
                    &capture_recovery_state
                );
                ignore_next_capture_interval = true;
                GRD_WARN(
                    "host: Desktop Duplication error; %s: %s",
                    reset_device
                        ? "full D3D11 reset"
                        : "recreate Desktop Duplication only",
                    error.message[0] != '\0'
                        ? error.message
                        : "unspecified capture error"
                );
            }
#endif
            if (capture_status != GRD_WOULD_BLOCK) {
                if (error.code == GRD_OK) {
                    error.code = capture_status;
                    (void)snprintf(
                        error.message,
                        sizeof(error.message),
                        "Windows capture temporarily unavailable"
                    );
                }
                /* Fullscreen/exclusive mode switches invalidate Desktop
                 * Duplication. Force an IDR after recovery so the UDP client
                 * cannot remain on an undecodable stale reference frame. */
                (void)SDL_SetAtomicInt(&app->keyframe_requested, 1);
                capture_recovery_pending = true;
#if defined(_WIN32)
                (void)grd_capture_recovery_force(
                    &capture_recovery_state
                );
#endif
                publish_error(app, &error);
                set_status(app, error.message);
                signal_main_thread(app);
            }
            SDL_DelayPrecise(
                capture_status == GRD_WOULD_BLOCK ? 1000000ULL : 20000000ULL
            );
            continue;
        }
#if defined(_WIN32)
        grd_capture_watchdog_on_frame(
            &capture_watchdog_state, frame_started
        );
        if (capture_recovery_state.phase ==
            GRD_CAPTURE_PHASE_REQUESTED) {
            /* A reset/gap has produced its first real frame. Start measuring
             * fresh cadence now; do not spend an IDR on this potentially lone
             * frame. */
            grd_capture_recovery_mark_started(&capture_recovery_state);
            capture_backlog_frames = 4U;
        }
        const uint64_t previous_capture_micros = capture_last_frame_micros;
        capture_last_frame_micros = frame_started;
        const uint64_t recovery_previous_capture_micros =
            ignore_next_capture_interval ? 0U : previous_capture_micros;
        const uint64_t observed_capture_gap =
            recovery_previous_capture_micros != 0U &&
            frame_started > recovery_previous_capture_micros
                ? frame_started - recovery_previous_capture_micros
                : 0U;
        const bool capture_discontinuity =
            grd_capture_gap_is_discontinuity(
                recovery_previous_capture_micros,
                frame_started,
                capture_timing.accumulated_frames,
                capture_timing.driver_stalled
            );
        const bool natural_no_change_interval =
            capture_recovery_state.phase == GRD_CAPTURE_PHASE_STABLE &&
            observed_capture_gap >= GRD_CAPTURE_SHORT_GAP_US &&
            !capture_discontinuity;
        const grd_capture_recovery_event capture_event =
            grd_capture_recovery_on_frame(
                &capture_recovery_state,
                recovery_previous_capture_micros,
                frame_started,
                stream_fps != 0U
                    ? 1000000ULL / stream_fps
                    : 16667ULL,
                ignore_next_capture_interval || natural_no_change_interval
            );
        ignore_next_capture_interval = false;
        /* AccumulatedFrames > 1 is normal when the host display refreshes
         * faster than the requested stream (for example 180 Hz -> 120 FPS).
         * It becomes actionable only as part of a confirmed long capture gap
         * below. A measured driver stall is independently trustworthy. */
        if (capture_timing.driver_stalled && capture_backlog_frames < 4U) {
            capture_backlog_frames = 4U;
        }
        if (capture_event == GRD_CAPTURE_EVENT_REQUEST) {
            const uint64_t capture_gap =
                frame_started - previous_capture_micros;
            capture_recovery_pending = true;
            const uint64_t missed =
                stream_fps != 0U
                    ? capture_gap * (uint64_t)stream_fps / 1000000ULL
                    : capture_gap / 16667ULL;
            if (missed > capture_backlog_frames) {
                capture_backlog_frames = missed > UINT32_MAX
                    ? UINT32_MAX
                    : (uint32_t)missed;
            }
            grd_capture_recovery_mark_started(&capture_recovery_state);
            GRD_WARN(
                "host: %.1f ms capture gap; waiting for stable cadence",
                (double)capture_gap / 1000.0
            );
        } else if (capture_event == GRD_CAPTURE_EVENT_REARMED) {
            grd_capture_watchdog_rearm(&capture_watchdog_state);
            GRD_INFO(
                "host: capture cadence stable; watchdog rearmed"
            );
        }
#endif
        const bool capture_recovery_ready =
#if defined(_WIN32)
            capture_recovery_pending &&
            capture_recovery_state.phase == GRD_CAPTURE_PHASE_STABLE;
#else
            capture_recovery_pending;
#endif
        if (capture_recovery_ready) {
            /* Spend one IDR only after several consecutive fresh frames prove
             * capture is stable. This coalesces a long stall and its short
             * aftershocks into one cleanup/repair episode. */
            (void)SDL_SetAtomicInt(&app->keyframe_requested, 1);
            /* The normal forced-IDR limiter may have sent one just before
             * the capture gap. Reset it so this recovery IDR is never
             * suppressed by the 500 ms throttle. */
            last_forced_idr_micros = 0U;
            last_config = 0U;
            config_pending = true;
            capture_recovery_pending = false;
            grd_host_resynchronize_video(app->host);
#if defined(_WIN32)
            grd_capture_recovery_mark_started(&capture_recovery_state);
#endif
            GRD_INFO(
                "host: Windows capture restored; pacer cleared and "
                "recovery keyframe requested"
            );
            set_status(app, "Video capture restored");
            signal_main_thread(app);
        }
        publish_error(app, &error);
        ++stats_captured;
        const bool captured_native =
            captured.format == GRD_PIXEL_D3D11_BGRA;
        if (captured_native) {
            ++stats_zero_copy_frames;
        } else if (prefer_gpu_capture && !native_capture_disabled) {
            native_capture_disabled = true;
            GRD_WARN(
                "host D3D11 zero-copy unavailable; falling back to CPU capture"
            );
        }
        uint32_t output_width;
        uint32_t output_height;
        uint32_t ladder_width;
        uint32_t ladder_height;
        grd_stream_ladder_max_dimensions(
            negotiated_max_width,
            negotiated_max_height,
            client_ladder_level,
            &ladder_width,
            &ladder_height
        );
        fit_to_max_dimensions(
            captured.width,
            captured.height,
            ladder_width,
            ladder_height,
            &output_width,
            &output_height
        );
        /* The configured max is a hard ceiling: a small resolution must not
         * be forced to a huge bitrate, and 24000 must actually cap 1080p60
         * (the old code took max(config, recommendation) which ignored it). */
        const grd_video_codec negotiated_codec = negotiate_codec(
            app, app->selected_pipeline
        );
        const uint32_t raw_recommended = resolution_bitrate_kbps(
            output_width,
            output_height,
            negotiated_target_fps,
            negotiated_codec
        );
        const uint32_t bitrate_ceiling =
            app->config.target_bitrate_kbps < raw_recommended
                ? app->config.target_bitrate_kbps
                : raw_recommended;
        const uint32_t initial_bitrate =
            app->config.initial_bitrate_kbps < bitrate_ceiling
                ? app->config.initial_bitrate_kbps
                : bitrate_ceiling;
        const uint32_t bitrate_floor =
            app->config.min_bitrate_kbps < bitrate_ceiling
                ? app->config.min_bitrate_kbps
                : bitrate_ceiling;
        if (bitrate_ceiling == bitrate_floor) {
            info_throttled(
                "host: ladder disabled (ceiling==floor==%u kbps): "
                "at low bitrate the pacer will drop 1080p60 frames",
                bitrate_ceiling
            );
        }
        const bool encoder_configuration_changed =
            encoder == NULL || grd_stream_encoder_configuration_changed(
                encoder_width,
                encoder_height,
                encoder_fps,
                (int)active_codec,
                output_width,
                output_height,
                negotiated_target_fps,
                (int)negotiated_codec
            );
        if (encoder_configuration_changed) {
            /* Only an explicit codec/resolution/requested-FPS change reaches
             * this path. ABR and pipeline pressure never recreate the active
             * encoder during gameplay. */
            const uint32_t target_bitrate =
                current_bitrate == 0U
                    ? initial_bitrate
                    : (current_bitrate < bitrate_ceiling
                           ? current_bitrate
                           : bitrate_ceiling);
            /* The encoder only sees the video budget; the pacer keeps the
             * full wire budget (see video_encoder_bitrate_kbps). */
            encoder_bitrate_kbps = video_encoder_bitrate_kbps(
                target_bitrate, fec_current
            );
            const bool replacing_encoder = encoder != NULL;
            const uint64_t encoder_reopen_started = grd_now_micros();
            grd_encoder_destroy(encoder);
            /* A codec/resolution/fps change invalidates the QP history:
             * 1080p frames must not influence the 720p decision. */
            qp_ring_next = 0U;
            latest_qp_valid = false;
            encoder_width = output_width;
            encoder_height = output_height;
            encoder_fps = negotiated_target_fps;
            encoder = grd_encoder_create(
                app->selected_pipeline,
                encoder_width,
                encoder_height,
                encoder_fps,
                encoder_bitrate_kbps,
                negotiated_codec,
                app->config.pixel_444,
                &active_pipeline,
                &active_codec,
                &error
            );
            const uint64_t encoder_reopen_finished = grd_now_micros();
            if (replacing_encoder) {
                timing_window_add(
                    &encoder_reopen_timing,
                    encoder_reopen_finished - encoder_reopen_started
                );
#if defined(_WIN32)
                /* The single stream thread intentionally stopped capturing
                 * while the codec was replaced. Exclude that pause from the
                 * no-frame watchdog and exactly one post-reopen gap check,
                 * while retaining it as a planned-gap diagnostic. */
                (void)grd_capture_recovery_on_frame(
                    &capture_recovery_state,
                    encoder_reopen_started,
                    encoder_reopen_finished,
                    negotiated_target_fps != 0U
                        ? 1000000ULL / negotiated_target_fps
                        : 16667ULL,
                    true
                );
                capture_last_frame_micros = encoder_reopen_finished;
                ignore_next_capture_interval = true;
#endif
            }
            (void)SDL_SetAtomicInt(
                &app->active_host_pipeline,
                (int)(encoder != NULL ? active_pipeline : GRD_PIPELINE_SOFTWARE)
            );
            (void)SDL_SetAtomicInt(
                &app->active_host_codec,
                (int)active_codec
            );
            publish_error(app, &error);
            GRD_INFO(
                "host encoder: codec=%s pipeline=%s %ux%u@%u nominal "
                "(%u effective), "
                "wire %u kbps, enc %u kbps (ceiling %u, floor %u), "
                "4:4:4=%d%s",
                grd_codec_name(active_codec),
                pipeline_display_name(active_pipeline),
                encoder_width,
                encoder_height,
                encoder_fps,
                stream_fps,
                target_bitrate,
                encoder_bitrate_kbps,
                bitrate_ceiling,
                bitrate_floor,
                app->config.pixel_444 ? 1 : 0,
                encoder != NULL ? "" : " FAILED"
            );
            last_config = 0U;
            current_bitrate = target_bitrate;
            desired_bitrate = target_bitrate;
            config_pending = false;
            grd_host_set_stream_params(
                app->host,
                current_bitrate * 1000U,
                1000000U / (stream_fps != 0U ? stream_fps : 60U)
            );
            wire_budget_kbps = current_bitrate;
            applied_wire_kbps = current_bitrate;
            pacer_grace_until = 0U;
            const uint64_t abr_started = grd_now_micros();
            last_encoder_rate_change_micros = abr_started;
            const uint64_t startup_hold =
                abr_started + GRD_ABR_STARTUP_HOLD_US;
            if (abr_hold_until < startup_hold) {
                abr_hold_until = startup_hold;
            }
            abr_last_increase_micros = abr_started;
            abr_clean_reports = 0U;
            recovery_idr_pending = true;
        }
        if (encoder == NULL) {
            grd_platform_frame_release(&captured);
            SDL_DelayPrecise(100000000ULL);
            continue;
        }
        if (grd_host_take_keyframe_pending(app->host)) {
            /* A client just became UDP-ready and missed the initial IDR:
             * produce a fresh keyframe immediately and announce the config
             * on the same frame so its decoder is created right away. */
            grd_encoder_request_keyframe(encoder);
            recovery_idr_pending = true;
            last_forced_idr_micros = grd_now_micros();
            last_config = 0U;
        }
        if (SDL_GetAtomicInt(&app->keyframe_requested) != 0) {
            /* Throttle forced IDRs: a client whose decoder cannot start
             * re-requests a keyframe every 60 ms, which previously turned
             * almost every frame into an IDR (observed: 41 keyframes in
             * 5 s, 62 KB average frames, 17% drops). One forced IDR per
             * 500 ms is enough for recovery and stops the request loop from
             * saturating the pacer. */
            recovery_idr_pending = true;
            (void)SDL_SetAtomicInt(&app->keyframe_requested, 0);
        }
        if (recovery_idr_pending) {
            const uint64_t now_idr = grd_now_micros();
            if (now_idr - last_forced_idr_micros >= 500000ULL) {
                grd_encoder_request_keyframe(encoder);
                last_forced_idr_micros = now_idr;
            }
        }
        if (SDL_CompareAndSwapAtomicInt(&app->abr_report_updated, 1, 0) &&
            app->config.abr_enabled) {
            const int reported_loss = SDL_GetAtomicInt(&app->abr_loss_percent);
            if (reported_loss >= 0) {
                const uint32_t loss = (uint32_t)reported_loss;
                const int reported_rtt = SDL_GetAtomicInt(&app->abr_rtt_micros);
                /* These counters describe different layers. `loss` is made
                 * from missing UDP datagrams at the receiver; `self_drop`
                 * includes only locally initiating admission/deadline/
                 * queue/send pressure. Recovery descendants stay telemetry
                 * only. Never subtract one from the other. */
                const uint32_t self_drop =
                    grd_host_udp_initiating_drop_percent(app->host);
                const uint32_t network_loss = loss;
                info_throttled(
                    "host ABR report: loss=%u%% self=%u%% rtt=%u us "
                    "current=%u kbps",
                    loss,
                    self_drop,
                    reported_rtt > 0 ? (unsigned)reported_rtt : 0U,
                    current_bitrate
                );
                /* Real network loss enables parity. Once enabled, retain it
                 * for five clean seconds; this avoids toggling the encoder
                 * on isolated reports while allowing a stable LAN to spend
                 * the reserved bits on image quality. */
                if (network_loss >= 1U) {
                    ++abr_lossy_reports;
                    abr_clean_reports = 0U;
                } else if (abr_clean_reports < 1000U) {
                    ++abr_clean_reports;
                }
                if (self_drop >= GRD_ABR_SELF_CUT_MILD_PCT &&
                    abr_self_lossy_reports < 1000U) {
                    ++abr_self_lossy_reports;
                } else {
                    abr_self_lossy_reports = 0U;
                }
                if (self_drop == 0U) {
                    if (abr_self_clean_reports < 1000U) {
                        ++abr_self_clean_reports;
                    }
                } else {
                    abr_self_clean_reports = 0U;
                }
                if (abr_clean_reports >= 15U) {
                    abr_lossy_reports = 0U;
                }
                const bool fec_wanted =
                    abr_lossy_reports >= 2U ||
                    (fec_current && abr_clean_reports < 50U);
                if (reported_rtt > 0) {
                    grd_host_set_rtt_us(
                        app->host, (uint32_t)reported_rtt
                    );
                }
                const uint64_t now_abr = grd_now_micros();
                bool capture_recovery_unsettled = false;
#if defined(_WIN32)
                capture_recovery_unsettled =
                    grd_capture_recovery_unsettled(
                        &capture_recovery_state
                    );
#endif
                const bool abr_recovery_unsettled =
                    recovery_idr_pending || capture_recovery_unsettled ||
                    now_abr < abr_recovery_stabilize_until;
                const uint32_t previous_rate = current_bitrate;
                uint32_t adjusted = desired_bitrate != 0U
                                        ? desired_bitrate
                                        : current_bitrate;
                bool cut = false;
                const bool network_cut_allowed =
                    grd_stream_abr_cut_allowed(
                        now_abr,
                        abr_hold_until,
                        false,
                        abr_recovery_unsettled
                    );
                const bool self_cut_allowed =
                    grd_stream_abr_cut_allowed(
                        now_abr,
                        abr_hold_until,
                        true,
                        abr_recovery_unsettled
                    );
                if (network_cut_allowed &&
                    abr_lossy_reports >= GRD_ABR_CUT_REQUIRED_REPORTS &&
                    network_loss >= GRD_ABR_CUT_HARD_PCT) {
                    /* Multiplicative decrease, then hold. */
                    adjusted = current_bitrate * 7U / 10U;
                    cut = true;
                } else if (network_cut_allowed &&
                           abr_lossy_reports >=
                               GRD_ABR_CUT_REQUIRED_REPORTS &&
                           network_loss >= GRD_ABR_CUT_STRONG_PCT) {
                    adjusted = current_bitrate * 85U / 100U;
                    cut = true;
                } else if (network_cut_allowed &&
                           abr_lossy_reports >=
                               GRD_ABR_CUT_REQUIRED_REPORTS &&
                           network_loss >= GRD_ABR_CUT_MILD_PCT) {
                    adjusted = current_bitrate * 93U / 100U;
                    cut = true;
                } else if (self_cut_allowed &&
                           abr_self_lossy_reports >=
                               GRD_ABR_SELF_CUT_REQUIRED_REPORTS &&
                           self_drop >= GRD_ABR_SELF_CUT_HARD_PCT) {
                    /* Pacer pressure is local rather than network loss:
                     * lower the encoder budget without enabling FEC. */
                    adjusted = current_bitrate * 7U / 10U;
                    cut = true;
                } else if (self_cut_allowed &&
                           abr_self_lossy_reports >=
                               GRD_ABR_SELF_CUT_REQUIRED_REPORTS &&
                           self_drop >= GRD_ABR_SELF_CUT_STRONG_PCT) {
                    adjusted = current_bitrate * 85U / 100U;
                    cut = true;
                } else if (self_cut_allowed &&
                           abr_self_lossy_reports >=
                               GRD_ABR_SELF_CUT_REQUIRED_REPORTS &&
                           self_drop >= GRD_ABR_SELF_CUT_MILD_PCT) {
                    adjusted = current_bitrate * 93U / 100U;
                    cut = true;
                } else if (!abr_recovery_unsettled &&
                           loss <= GRD_ABR_SELF_DROP_MARGIN &&
                           self_drop <= GRD_ABR_SELF_DROP_MARGIN &&
                           now_abr >= abr_hold_until &&
                           abr_clean_reports >=
                               GRD_ABR_RECOVERY_CLEAN_REPORTS &&
                           abr_self_clean_reports >=
                               GRD_ABR_RECOVERY_CLEAN_REPORTS &&
                           (reported_rtt < 0 ||
                            (uint32_t)reported_rtt <= 60000U) &&
                           now_abr - abr_last_increase_micros >=
                               GRD_ABR_INCREASE_INTERVAL_US) {
                    adjusted = adjusted +
                               GRD_ABR_INCREASE_STEP_KBPS;
                    abr_last_increase_micros = now_abr;
                }
                if (cut) {
                    abr_hold_until = now_abr + GRD_ABR_HOLD_US;
                    abr_last_increase_micros = now_abr;
                }
                const uint32_t ceiling = bitrate_ceiling;
                const uint32_t floor = bitrate_floor;
                if (adjusted < floor) {
                    adjusted = floor;
                }
                if (adjusted > ceiling) {
                    adjusted = ceiling;
                }
                desired_bitrate = adjusted;
                const bool abr_value_changed =
                    adjusted != current_bitrate || fec_wanted != fec_current;
                bool abr_applied = false;
                const bool rate_change_due =
                    abr_value_changed && grd_stream_rate_change_due(
                        current_bitrate,
                        desired_bitrate,
                        now_abr,
                        last_encoder_rate_change_micros,
                        fec_wanted != fec_current,
                        cut && desired_bitrate < current_bitrate
                    );
                if (rate_change_due) {
                    const uint32_t adjusted_encoder =
                        video_encoder_bitrate_kbps(
                            desired_bitrate, fec_wanted
                        );
                    grd_error bitrate_error = {0};
                    const grd_status bitrate_status =
                        grd_encoder_set_bitrate(
                            encoder, adjusted_encoder, &bitrate_error
                        );
                    if (bitrate_status == GRD_OK) {
                        current_bitrate = desired_bitrate;
                        encoder_bitrate_kbps = adjusted_encoder;
                        fec_current = fec_wanted;
                        last_encoder_rate_change_micros = now_abr;
                        grd_host_set_fec_enabled(app->host, fec_current);
                        abr_applied = true;
                    } else if (bitrate_status == GRD_NOT_SUPPORTED) {
                        /* Stable is safer than lying to the pacer. Never lower
                         * the wire budget beneath an encoder that cannot be
                         * reconfigured, and never reopen it during gameplay. */
                        grd_host_set_fec_enabled(app->host, fec_current);
                        info_throttled(
                        "ABR: encoder cannot be reconfigured; keeping "
                        "%u kbps without reopening",
                            encoder_bitrate_kbps
                        );
                        desired_bitrate = current_bitrate;
                    } else {
                        grd_host_set_fec_enabled(app->host, fec_current);
                        publish_error(app, &bitrate_error);
                        desired_bitrate = current_bitrate;
                    }
                    if (abr_applied) {
                        ++stats_abr_changes;
                        if (adjusted < previous_rate) {
                            /* Give the live encoder reconfiguration one short
                             * settling window before shrinking the pacer. */
                            pacer_grace_until =
                                now_abr + GRD_ABR_PACER_DOWN_GRACE_US;
                        } else if (adjusted > previous_rate) {
                            pacer_grace_until = 0U;
                        }
                        config_pending = true;
                    }
                } else if (abr_value_changed) {
                    info_throttled(
                        "ABR: accumulating %u -> %u kbps (next encoder "
                        "reconfiguration is batched)",
                        current_bitrate,
                        desired_bitrate
                    );
                }
                if (abr_applied) {
                    GRD_INFO(
                        "ABR wire %u -> %u kbps (enc %u kbps, "
                        "reconfigure live, fec=%d, "
                        "loss %u%%, self-init %u%%, rtt %u us, frozen=%d)",
                        previous_rate,
                        current_bitrate,
                        encoder_bitrate_kbps,
                        fec_current ? 1 : 0,
                        loss,
                        self_drop,
                        reported_rtt > 0 ? (unsigned)reported_rtt : 0U,
                        abr_recovery_unsettled ? 1 : 0
                    );
                }
                /* Reconcile the pacer only after a successful live encoder
                 * change. Unsupported encoders retain both budgets. */
                const uint32_t requested_wire_budget = current_bitrate;
                if (requested_wire_budget > wire_budget_kbps ||
                    now_abr >= pacer_grace_until) {
                    wire_budget_kbps = requested_wire_budget;
                }
                if (wire_budget_kbps != applied_wire_kbps) {
                    grd_host_set_stream_params(
                        app->host,
                        wire_budget_kbps * 1000U,
                        1000000U / (stream_fps != 0U ? stream_fps : 60U)
                    );
                    applied_wire_kbps = wire_budget_kbps;
                }
            }
        }
        const uint64_t now = grd_now_micros();
        if (config_pending || now - last_config >= 1000000ULL) {
            grd_video_config configuration = {
                .width = encoder_width,
                .height = encoder_height,
                .fps = stream_fps,
                .bitrate_kbps = current_bitrate
            };
            (void)snprintf(
                configuration.codec,
                sizeof(configuration.codec),
                "%s",
                grd_codec_name(active_codec)
            );
            (void)snprintf(
                configuration.pipeline,
                sizeof(configuration.pipeline),
                "%s",
                pipeline_display_name(active_pipeline)
            );
            (void)grd_host_broadcast(
                app->host,
                GRD_PACKET_VIDEO_CONFIG,
                &configuration,
                sizeof(configuration),
                NULL
            );
            last_config = now;
            config_pending = false;
        }
        grd_encoded_frame encoded;
        const grd_status encode_status = grd_encoder_encode(
            encoder, &captured, &encoded, &error
        );
        grd_encoder_timing encode_timing = {0};
        grd_encoder_last_timing(encoder, &encode_timing);
        if (encode_timing.conversion_recorded) {
            timing_window_add(
                &conversion_timing, encode_timing.conversion_micros
            );
        }
        if (encode_timing.send_frame_recorded) {
            timing_window_add(
                &send_frame_timing, encode_timing.send_frame_micros
            );
        }
        if (encode_timing.receive_packet_recorded) {
            timing_window_add(
                &receive_packet_timing,
                encode_timing.receive_packet_micros
            );
        }
        publish_error(app, &error);
        if (encode_status == GRD_OK && encoded.qp_known) {
            qp_ring[qp_ring_next % GRD_QP_RING_CAPACITY] = encoded.avg_qp;
            ++qp_ring_next;
        }
        bool suppress_until_idr = false;
        if (encode_status == GRD_OK && recovery_idr_pending) {
            if (encoded.keyframe) {
                recovery_idr_pending = false;
                abr_recovery_stabilize_until =
                    grd_now_micros() + GRD_ABR_RECOVERY_STABILIZE_US;
            } else {
                /* The pacer or client has discarded a reference frame. The
                 * CUDA/NVENC path returns the previously submitted packet,
                 * so one or more P-frames can emerge before the forced IDR.
                 * Never put those undecodable dependants on the wire. */
                suppress_until_idr = true;
                ++stats_recovery_suppressed;
            }
        }
        if (encode_status == GRD_OK && !suppress_until_idr) {
            stats_encoded_bytes += encoded.size;
            ++stats_frame_count;
            ++stats_encoded;
            if (encoded.keyframe) {
                ++stats_keyframes;
                info_throttled(
                    "host: IDR sent (keyframe, %u kbps, %llu bytes)",
                    current_bitrate,
                    (unsigned long long)encoded.size
                );
            }
        } else if (encode_status == GRD_OK) {
            /* Expected recovery gating, not an encoder error. Most
             * importantly, release this access unit and do not broadcast it:
             * the previous implementation counted it as suppressed but then
             * still sent it below, keeping the per-client discontinuity
             * alive. */
        } else if (encode_status == GRD_WOULD_BLOCK) {
            ++stats_blocked;
        } else {
            ++stats_encode_errors;
            if (captured_native && !native_capture_disabled) {
                native_capture_disabled = true;
                GRD_WARN(
                    "host D3D11 zero-copy disabled; falling back to CPU capture: %s",
                    error.message[0] != '\0'
                        ? error.message
                        : "conversion/encoder unavailable"
                );
            }
        }
        if (encode_status == GRD_OK && !suppress_until_idr) {
            /* Count only access units offered to the transport. Encoder
             * WOULD_BLOCK and recovery-gated P-frames were never sent and
             * must not look like network/source loss to the client. A pacer
             * drop still advances this id, so real per-client gaps remain
             * detectable. */
            ++host_frame_counter;
            uint8_t prefix[VIDEO_WIRE_PREFIX];
            write_u64(prefix, encoded.timestamp_micros);
            write_u32(prefix + 8U, encoder_width);
            write_u32(prefix + 12U, encoder_height);
            prefix[16U] = encoded.keyframe ? 1U : 0U;
            write_u64(prefix + 17U, host_frame_counter);
            const grd_buf_part parts[2] = {
                {.data = prefix, .length = VIDEO_WIRE_PREFIX},
                {.data = encoded.data, .length = encoded.size}
            };
            grd_owned_buffer payload_ref = encoded.buffer;
            grd_error transport_error = {0};
            /* Ownership of the encoded frame transfers to the transport. */
            const grd_status transport_status = grd_host_broadcast_parts(
                app->host,
                GRD_PACKET_VIDEO_FRAME,
                parts,
                2U,
                &payload_ref,
                encoded.keyframe,
                &transport_error
            );
            if (transport_status != GRD_OK) {
                if (transport_error.code == GRD_OK) {
                    transport_error.code = transport_status;
                    (void)snprintf(
                        transport_error.message,
                        sizeof(transport_error.message),
                        "UDP video transport failed"
                    );
                }
                publish_error(app, &transport_error);
                set_status(app, transport_error.message);
                signal_main_thread(app);
            }
        } else if (encode_status == GRD_OK) {
            grd_encoded_frame_release(&encoded);
        }
        grd_platform_frame_release(&captured);
        const uint64_t fps_sampled_at = grd_now_micros();
        const uint64_t pipeline_micros = fps_sampled_at >= frame_started
                                             ? fps_sampled_at - frame_started
                                             : 0U;
        uint64_t initiating_drop_generation = 0U;
        const uint32_t initiating_drop_percent =
            grd_host_udp_initiating_drop_sample(
                app->host, &initiating_drop_generation
            );
        const uint32_t previous_stream_fps = stream_fps;
        stream_fps = grd_stream_fps_pressure_update(
            &fps_pressure_state,
            negotiated_target_fps,
            fps_sampled_at,
            pipeline_micros,
            initiating_drop_percent,
            initiating_drop_generation,
            capture_backlog_frames
        );
        if (stream_fps != previous_stream_fps) {
            const uint64_t average_pipeline_micros =
                (fps_pressure_state.pipeline_ewma_x256 + 255U) >> 8U;
            GRD_INFO(
                "host dynamic FPS: %u -> %u (target %u, pipeline EWMA "
                "%llu us, pressure %u, signals 0x%x)",
                previous_stream_fps,
                stream_fps,
                negotiated_target_fps,
                (unsigned long long)average_pipeline_micros,
                fps_pressure_state.last_change_pressure_score,
                fps_pressure_state.last_change_reasons
            );
            next_frame_deadline = 0U;
            config_pending = true;
            grd_host_set_stream_params(
                app->host,
                applied_wire_kbps * 1000U,
                1000000U / stream_fps
            );
        }
        if (stats_last_micros == 0U) {
            stats_last_micros = frame_started;
        }
        if (frame_started - stats_last_micros >= 1000000ULL) {
            const int stats_loss = SDL_GetAtomicInt(&app->abr_loss_percent);
            const int stats_rtt = SDL_GetAtomicInt(&app->abr_rtt_micros);
            /* Rolling QP percentiles over the last ~2 s of encoded frames. */
            latest_qp_valid = false;
            const size_t qp_count =
                qp_ring_next < GRD_QP_RING_CAPACITY
                    ? qp_ring_next
                    : GRD_QP_RING_CAPACITY;
            if (qp_count != 0U) {
                uint8_t sorted_qp[GRD_QP_RING_CAPACITY];
                memcpy(sorted_qp, qp_ring, qp_count);
                for (size_t a = 1U; a < qp_count; ++a) {
                    const uint8_t value = sorted_qp[a];
                    size_t b = a;
                    while (b > 0U && sorted_qp[b - 1U] > value) {
                        sorted_qp[b] = sorted_qp[b - 1U];
                        --b;
                    }
                    sorted_qp[b] = value;
                }
                uint64_t qp_sum = 0U;
                for (size_t index = 0U; index < qp_count; ++index) {
                    qp_sum += sorted_qp[index];
                }
                latest_qp_avg = (uint8_t)(qp_sum / qp_count);
                const size_t p95_index = (qp_count * 95U) / 100U;
                latest_qp_p95 =
                    sorted_qp[p95_index < qp_count ? p95_index : qp_count - 1U];
                latest_qp_valid = true;
            }
            GRD_INFO(
                "host stats: bitrate=%u kbps loss=%d%% rtt=%u us fps=%u "
                "qp avg=%u p95=%u%s",
                current_bitrate,
                stats_loss >= 0 ? stats_loss : 0,
                stats_rtt > 0 ? (unsigned)stats_rtt : 0U,
                stats_frame_count,
                latest_qp_avg,
                latest_qp_p95,
                latest_qp_valid ? "" : " (no encoder QP)"
            );
            stats_frame_count = 0U;
            stats_last_micros = frame_started;
        }
        const uint64_t stats_now = grd_now_micros();
        if (stats_window_start == 0U) {
            stats_window_start = stats_now;
        }
        if (stats_now - stats_window_start >= 5000000ULL) {
            const double seconds =
                (double)(stats_now - stats_window_start) / 1000000.0;
            const double captured_fps = (double)stats_captured / seconds;
            const double encoded_fps = (double)stats_encoded / seconds;
            const bool source_limited = stream_fps != 0U &&
                captured_fps * 100.0 < (double)stream_fps * 80.0 &&
                (stats_captured == 0U ||
                 encoded_fps * 100.0 >= captured_fps * 90.0) &&
                stats_encode_errors == 0U;
            const grd_timing_summary acquire_summary =
                timing_window_summary(&capture_acquire_timing);
            const grd_timing_summary conversion_summary =
                timing_window_summary(&conversion_timing);
            const grd_timing_summary send_summary =
                timing_window_summary(&send_frame_timing);
            const grd_timing_summary receive_summary =
                timing_window_summary(&receive_packet_timing);
            const grd_timing_summary reopen_summary =
                timing_window_summary(&encoder_reopen_timing);
            uint64_t raw_capture_gaps = 0U;
            uint64_t capture_recovery_episodes = 0U;
            uint64_t planned_capture_gaps = 0U;
#if defined(_WIN32)
            raw_capture_gaps =
                capture_recovery_state.raw_gap_count - capture_raw_gaps_at;
            capture_recovery_episodes =
                capture_recovery_state.episode_count - capture_episodes_at;
            planned_capture_gaps =
                capture_recovery_state.planned_gap_count -
                capture_planned_gaps_at;
#endif
            GRD_INFO(
                "host stream: cap %.1f fps, enc ok %.1f fps, blocked %llu, "
                "err %llu, zero-copy %llu, recover-skip %llu, abr changes %llu, "
                "avg %llu B/frame, "
                "qp avg=%u p95=%u, wire %u kbps, enc %u kbps, pace %u kbps, "
                "fps %u/%u, pressure %u, cadence %s, key %llu, "
                "offload client %u, %s",
                (double)stats_captured / seconds,
                (double)stats_encoded / seconds,
                stats_blocked,
                stats_encode_errors,
                stats_zero_copy_frames,
                stats_recovery_suppressed,
                stats_abr_changes,
                stats_encoded != 0U
                    ? stats_encoded_bytes / stats_encoded
                    : 0U,
                latest_qp_avg,
                latest_qp_p95,
                current_bitrate,
                encoder_bitrate_kbps,
                grd_host_pacing_bits_per_second(app->host) / 1000U,
                stream_fps,
                negotiated_target_fps,
                fps_pressure_state.pressure_score,
                source_limited ? "source-limited" : "target/processing",
                (unsigned long long)stats_keyframes,
                client_ladder_level,
                pipeline_display_name(active_pipeline)
            );
            GRD_INFO(
                "host pipeline 5s: acquire us avg=%.1f p95=%llu max=%llu "
                "timeouts=%llu coalesced=%llu; convert us avg=%.1f "
                "p95=%llu max=%llu; "
                "send us avg=%.1f p95=%llu max=%llu; recv us avg=%.1f "
                "p95=%llu max=%llu; reopen us avg=%.1f p95=%llu max=%llu; "
                "gaps planned=%llu unplanned=%llu episodes=%llu; "
                "capture-reset session/device=%llu/%llu",
                acquire_summary.average,
                (unsigned long long)acquire_summary.p95,
                (unsigned long long)acquire_summary.maximum,
                (unsigned long long)stats_capture_wait_timeouts,
                (unsigned long long)stats_capture_coalesced_frames,
                conversion_summary.average,
                (unsigned long long)conversion_summary.p95,
                (unsigned long long)conversion_summary.maximum,
                send_summary.average,
                (unsigned long long)send_summary.p95,
                (unsigned long long)send_summary.maximum,
                receive_summary.average,
                (unsigned long long)receive_summary.p95,
                (unsigned long long)receive_summary.maximum,
                reopen_summary.average,
                (unsigned long long)reopen_summary.p95,
                (unsigned long long)reopen_summary.maximum,
                (unsigned long long)planned_capture_gaps,
                (unsigned long long)raw_capture_gaps,
                (unsigned long long)capture_recovery_episodes,
#if defined(_WIN32)
                (unsigned long long)stats_capture_session_resets,
                (unsigned long long)stats_capture_device_resets
#else
                0ULL,
                0ULL
#endif
            );
            stats_window_start = stats_now;
            stats_captured = 0U;
            stats_encoded = 0U;
            stats_encoded_bytes = 0U;
            stats_blocked = 0U;
            stats_encode_errors = 0U;
            stats_zero_copy_frames = 0U;
            stats_recovery_suppressed = 0U;
            stats_abr_changes = 0U;
            stats_keyframes = 0U;
            stats_capture_wait_timeouts = 0U;
            timing_window_reset(&capture_acquire_timing);
            timing_window_reset(&conversion_timing);
            timing_window_reset(&send_frame_timing);
            timing_window_reset(&receive_packet_timing);
            timing_window_reset(&encoder_reopen_timing);
#if defined(_WIN32)
            stats_capture_session_resets = 0U;
            stats_capture_device_resets = 0U;
            stats_capture_coalesced_frames = 0U;
            capture_raw_gaps_at = capture_recovery_state.raw_gap_count;
            capture_episodes_at = capture_recovery_state.episode_count;
            capture_planned_gaps_at =
                capture_recovery_state.planned_gap_count;
#endif
        }
        const uint64_t interval = stream_fps == 0U
                                      ? 16667ULL
                                      : 1000000ULL / stream_fps;
        if (next_frame_deadline == 0U ||
            next_frame_deadline < frame_started ||
            frame_started - next_frame_deadline > interval * 2U) {
            next_frame_deadline = frame_started + interval;
        } else {
            next_frame_deadline += interval;
        }
        const uint64_t now_deadline = grd_now_micros();
        if (next_frame_deadline > now_deadline) {
            SDL_DelayPrecise(
                (next_frame_deadline - now_deadline) * 1000ULL
            );
        }
    }
    grd_encoder_destroy(encoder);
    (void)SDL_EnableScreenSaver();
    return 0;
}

static int audio_thread(void *userdata)
{
    grd_app *app = userdata;
    set_media_thread_priority();
    grd_error error = {0};
    grd_audio_encoder *encoder = grd_audio_encoder_create(&error);
    publish_error(app, &error);
    memset(&error, 0, sizeof(error));
    if (encoder == NULL ||
        grd_platform_audio_start(&error) != GRD_OK) {
        publish_error(app, &error);
        grd_audio_encoder_destroy(encoder);
        (void)SDL_SetAtomicInt(&app->audio_active, 0);
        return 0;
    }
    (void)SDL_SetAtomicInt(&app->audio_active, 1);
    float samples[GRD_AUDIO_FRAME_SAMPLES * GRD_AUDIO_CHANNELS];
    uint64_t last_config = 0U;
    while (SDL_GetAtomicInt(&app->streaming) != 0) {
        size_t frames = 0U;
        uint64_t timestamp = 0U;
        memset(&error, 0, sizeof(error));
        const grd_status status = grd_platform_audio_read(
            samples,
            GRD_AUDIO_FRAME_SAMPLES,
            &frames,
            &timestamp,
            &error
        );
        publish_error(app, &error);
        if (status == GRD_WOULD_BLOCK) {
            SDL_DelayPrecise(2000000ULL);
            continue;
        }
        if (status != GRD_OK) {
            SDL_DelayPrecise(20000000ULL);
            continue;
        }
        if (app->host == NULL || grd_host_client_count(app->host) == 0U) {
            continue;
        }
        const uint64_t now = grd_now_micros();
        if (now - last_config >= 1000000ULL) {
            grd_audio_config configuration = {
                .sample_rate = GRD_AUDIO_SAMPLE_RATE,
                .channels = GRD_AUDIO_CHANNELS,
                .frame_samples = GRD_AUDIO_FRAME_SAMPLES,
                .bitrate_kbps = GRD_AUDIO_BITRATE_KBPS
            };
            (void)snprintf(
                configuration.codec,
                sizeof(configuration.codec),
                "opus"
            );
            (void)grd_host_broadcast(
                app->host,
                GRD_PACKET_AUDIO_CONFIG,
                &configuration,
                sizeof(configuration),
                NULL
            );
            last_config = now;
        }
        grd_encoded_audio encoded;
        memset(&error, 0, sizeof(error));
        if (grd_audio_encode(
                encoder,
                samples,
                frames,
                timestamp,
                &encoded,
                &error
        ) == GRD_OK) {
            publish_error(app, &error);
            uint8_t prefix[AUDIO_WIRE_PREFIX];
            write_u64(prefix, encoded.timestamp_micros);
            const grd_buf_part parts[2] = {
                {.data = prefix, .length = AUDIO_WIRE_PREFIX},
                {.data = encoded.data, .length = encoded.size}
            };
            grd_owned_buffer payload_ref = encoded.buffer;
            /* Ownership of the encoded audio transfers to the transport. */
            (void)grd_host_broadcast_parts(
                app->host,
                GRD_PACKET_AUDIO_FRAME,
                parts,
                2U,
                &payload_ref,
                false,
                NULL
            );
        }
    }
    grd_platform_audio_stop();
    grd_audio_encoder_destroy(encoder);
    (void)SDL_SetAtomicInt(&app->audio_active, 0);
    return 0;
}

static void stop_host(grd_app *app)
{
    grd_discovery_set_available(
        app->discovery,
        false,
        app->config.device_name,
        app->config.port
    );
    (void)SDL_SetAtomicInt(&app->streaming, 0);
    if (app->stream_thread != NULL) {
        SDL_WaitThread(app->stream_thread, NULL);
        app->stream_thread = NULL;
    }
    if (app->audio_thread != NULL) {
        SDL_WaitThread(app->audio_thread, NULL);
        app->audio_thread = NULL;
    }
    if (app->cursor_thread != NULL) {
        SDL_WaitThread(app->cursor_thread, NULL);
        app->cursor_thread = NULL;
    }
    grd_host_stop(app->host);
    app->host = NULL;
    app->ssh_remote_access_ready = false;
    grd_discovery_set_remote_access(
        app->discovery, grd_platform_os(), 0U, 0U
    );
    app->config.host_enabled = false;
    (void)grd_config_save(&app->config, &app->last_error);
}

static bool start_media_threads(grd_app *app)
{
    media_queue_reset(&app->video_queue);
    media_queue_reset(&app->audio_queue);
    app->video_decode_thread = SDL_CreateThread(
        video_decode_thread, "grd-video-decode", app
    );
    if (app->video_decode_thread == NULL) {
        media_queue_stop(&app->video_queue);
        return false;
    }
    app->video_decode_thread_started = true;
    app->audio_decode_thread = SDL_CreateThread(
        audio_decode_thread, "grd-audio-decode", app
    );
    if (app->audio_decode_thread == NULL) {
        media_queue_stop(&app->video_queue);
        SDL_WaitThread(app->video_decode_thread, NULL);
        app->video_decode_thread = NULL;
        app->video_decode_thread_started = false;
        return false;
    }
    app->audio_decode_thread_started = true;
    return true;
}

static void stop_media_threads(grd_app *app)
{
    media_queue_stop(&app->video_queue);
    media_queue_stop(&app->audio_queue);
    if (app->video_decode_thread_started) {
        SDL_WaitThread(app->video_decode_thread, NULL);
        app->video_decode_thread = NULL;
        app->video_decode_thread_started = false;
    }
    if (app->audio_decode_thread_started) {
        SDL_WaitThread(app->audio_decode_thread, NULL);
        app->audio_decode_thread = NULL;
        app->audio_decode_thread_started = false;
    }
}

static void start_host(grd_app *app)
{
    if (!app->config.password_configured) {
        set_status(app, "Set a password of at least 12 characters first");
        return;
    }
    grd_error error = {0};
    atomic_store_explicit(
        &app->host_input_injection_failures, 0U, memory_order_relaxed
    );
    atomic_store_explicit(
        &app->host_input_error_broadcast_micros, 0U, memory_order_relaxed
    );
    if (grd_platform_validate_host(&error) != GRD_OK) {
        publish_error(app, &error);
        set_status(app, error.message);
        grd_platform_request_permissions();
        return;
    }
    app->host = grd_host_start(
        &app->config, host_packet, app, &error
    );
    publish_error(app, &error);
    if (app->host == NULL) {
        set_status(app, error.message);
        return;
    }
    app->config.host_enabled = true;
    memset(&error, 0, sizeof(error));
    (void)grd_config_save(&app->config, &error);
    publish_error(app, &error);
    (void)SDL_SetAtomicInt(&app->streaming, 1);
    app->stream_thread = SDL_CreateThread(stream_thread, "grd-stream", app);
    app->audio_thread = SDL_CreateThread(audio_thread, "grd-audio", app);
    app->cursor_thread = SDL_CreateThread(cursor_thread, "grd-cursor", app);
    app->ssh_remote_access_ready = false;
    uint32_t remote_access_capabilities = 0U;
#if !defined(_WIN32)
    if (app->config.ssh_remote_access_enabled) {
        grd_error ssh_error = {0};
        if (grd_remote_access_probe_local_ssh(
                app->config.ssh_remote_access_port, 700U, &ssh_error
            ) == GRD_OK) {
            app->ssh_remote_access_ready = true;
            remote_access_capabilities =
                GRD_DISCOVERY_CAP_SSH_TERMINAL |
                GRD_DISCOVERY_CAP_SFTP;
            GRD_INFO(
                "Local OpenSSH verified on port %u: terminal and SFTP "
                "advertised on the LAN",
                (unsigned)app->config.ssh_remote_access_port
            );
        } else {
            GRD_WARN(
                "OpenSSH not advertised: %s",
                ssh_error.message[0] != '\0'
                    ? ssh_error.message
                    : "service unreachable"
            );
        }
    }
#endif
    grd_discovery_set_remote_access(
        app->discovery,
        grd_platform_os(),
        remote_access_capabilities,
        app->config.ssh_remote_access_port
    );
    grd_discovery_set_available(
        app->discovery,
        true,
        app->config.device_name,
        app->config.port
    );
    if (app->config.ssh_remote_access_enabled &&
        !app->ssh_remote_access_ready) {
        set_status(
            app,
            "Host active; OpenSSH unavailable and not advertised"
        );
    } else if (app->ssh_remote_access_ready) {
        set_status(app, "Host active with SSH terminal and SFTP files");
    } else {
        set_status(app, "Host active on the LAN");
    }
}

static void disconnect(grd_app *app)
{
    if (app->remote_command_tab_active && app->connection != NULL &&
        grd_connection_is_active(app->connection)) {
        grd_input_event release;
        memset(&release, 0, sizeof(release));
        release.kind = GRD_INPUT_KEY;
        release.code = SDL_SCANCODE_TAB;
        send_input(app, &release);
        release.code = GRD_KEY_LEFT_ALT;
        send_input(app, &release);
    }
    set_remote_keyboard_grab(app, false);
    app->remote_command_tab_active = false;
    release_remote_mouse(app);
    app->escape_press_count = 0U;
    app->escape_sequence_started_micros = 0ULL;
    app->remote_heartbeat_sent_micros = 0ULL;
    app->remote_settings_visible = false;
    if (app->remote_fullscreen_active) {
        (void)set_remote_fullscreen(app, false);
    }
    app->remote_window_raised = false;
    atomic_store_explicit(
        &app->remote_stream_fps, 0U, memory_order_relaxed
    );
    atomic_store_explicit(
        &app->remote_receive_fps_tenths, 0U, memory_order_relaxed
    );
    atomic_store_explicit(
        &app->remote_stream_bitrate_kbps, 0U, memory_order_relaxed
    );
    atomic_store_explicit(
        &app->remote_receive_bitrate_kbps, 0U, memory_order_relaxed
    );
    atomic_store_explicit(
        &app->remote_video_bytes_received, 0ULL, memory_order_relaxed
    );
    app->hud_bitrate_sample_micros = 0ULL;
    app->hud_bitrate_sample_bytes = 0ULL;
    app->hud_fps_sample_micros = 0ULL;
    app->hud_health_window_started_micros = 0ULL;
    app->hud_last_skipped_frames = 0ULL;
    app->hud_last_recoveries = 0ULL;
    app->hud_last_decode_failures = 0ULL;
    app->hud_recent_skipped_frames = 0ULL;
    app->hud_recent_recoveries = 0ULL;
    app->hud_recent_decode_failures = 0ULL;
    app->hud_fps_history_count = 0U;
    app->hud_fps_history_next = 0U;
    memset(app->hud_fps_history, 0, sizeof(app->hud_fps_history));
    atomic_store_explicit(
        &app->remote_stream_width, 0U, memory_order_relaxed
    );
    atomic_store_explicit(
        &app->remote_stream_height, 0U, memory_order_relaxed
    );
    (void)SDL_SetAtomicInt(&app->remote_stream_codec, (int)GRD_CODEC_H264);
    (void)SDL_SetAtomicInt(&app->remote_decoder_ready, 0);
    app->remote_last_wire_frame_id = 0ULL;
    app->remote_last_wire_arrival_micros = 0ULL;
    app->remote_last_source_timestamp_micros = 0ULL;
    atomic_store_explicit(
        &app->remote_source_skipped_frames, 0ULL, memory_order_relaxed
    );
    atomic_store_explicit(
        &app->remote_source_gap_recoveries, 0ULL, memory_order_relaxed
    );
    atomic_store_explicit(
        &app->remote_arrival_gap_count, 0ULL, memory_order_relaxed
    );
    atomic_store_explicit(
        &app->remote_arrival_max_gap_us, 0U, memory_order_relaxed
    );
    atomic_store_explicit(
        &app->remote_source_max_gap_us, 0U, memory_order_relaxed
    );
    (void)SDL_SetAtomicInt(&app->remote_raise_pending, 0);
    if (app->config_dirty) {
        (void)grd_config_save(&app->config, &app->last_error);
        app->config_dirty = false;
    }
    stop_media_threads(app);
    grd_connection_close(app->media_connection);
    app->media_connection = NULL;
    grd_connection_close(app->connection);
    app->connection = NULL;
    /* Reset recovery state only after the packet callbacks are stopped: a
     * final in-flight video packet must not re-arm it for the next session. */
    (void)SDL_SetAtomicInt(&app->keyframe_request_pending, 0);
    (void)SDL_SetAtomicInt(&app->remote_waiting_for_repair_idr, 0);
    atomic_store_explicit(
        &app->keyframe_requested_micros, 0ULL, memory_order_relaxed
    );
    app->keyframe_request_interval_us = 500000U;
    /* Software decoder frames come from its reusable mailbox pool. Release
     * the application's last slot before destroying that decoder. */
    SDL_LockMutex(app->frame_mutex);
    grd_platform_frame_release(&app->remote_frame);
    memset(&app->remote_frame, 0, sizeof(app->remote_frame));
    app->remote_frame_generation = 0U;
    app->displayed_generation = 0U;
    SDL_UnlockMutex(app->frame_mutex);
    SDL_LockMutex(app->decoder_mutex);
    grd_decoder_destroy(app->decoder);
    app->decoder = NULL;
    app->decoder_width = 0U;
    app->decoder_height = 0U;
    SDL_UnlockMutex(app->decoder_mutex);
    SDL_LockMutex(app->audio_mutex);
    grd_audio_decoder_destroy(app->audio_decoder);
    app->audio_decoder = NULL;
    if (app->audio_playback != NULL) {
        SDL_DestroyAudioStream(app->audio_playback);
        app->audio_playback = NULL;
    }
    SDL_UnlockMutex(app->audio_mutex);
    destroy_remote_texture(app);
    app->remote_texture_format = 0;
    if (app->remote_cursor_texture != NULL) {
        SDL_DestroyTexture(app->remote_cursor_texture);
        app->remote_cursor_texture = NULL;
    }
    SDL_LockMutex(app->cursor_mutex);
    app->remote_cursor_visible = false;
    app->predicted_cursor_valid = false;
    app->predicted_cursor_x = 0.0F;
    app->predicted_cursor_y = 0.0F;
    app->remote_cursor_shape_generation = 0U;
    app->displayed_cursor_shape_generation = 0U;
    memset(&app->remote_cursor_shape, 0, sizeof(app->remote_cursor_shape));
    SDL_UnlockMutex(app->cursor_mutex);
    app->mode = GRD_APP_HOME;
    set_status(app, "Disconnected");
}

static bool connect_to(grd_app *app, const char *address, uint16_t port)
{
    /* Reconnecting while a session is active leaked the previous
     * connections and started a second UDP handshake (observed as repeated
     * 'UDP video handshake failed'): tear down cleanly first. */
    if (app->connection != NULL || app->media_connection != NULL) {
        disconnect(app);
    }
    if (app->connect_password[0] == '\0') {
        set_status(app, "Enter the host password");
        return false;
    }
    grd_error error = {0};
    app->connection = grd_connect(
        address,
        port,
        app->connect_password,
        app->request_controller ? GRD_ROLE_CONTROLLER : GRD_ROLE_OBSERVER,
        &app->config,
        client_packet,
        app,
        &error
    );
    publish_error(app, &error);
    if (app->connection == NULL) {
        set_status(
            app,
            error.message[0] != '\0' ? error.message : "Not available"
        );
        return false;
    }
    const grd_role assigned_role = grd_connection_role(app->connection);
    GRD_INFO(
        "client role: requested=%s assigned=%s",
        app->request_controller ? "controller" : "observer",
        assigned_role == GRD_ROLE_CONTROLLER ? "controller" : "observer"
    );
    /* A host with another active controller may legitimately downgrade the
     * request to observer. Do not open what looks like a controllable gaming
     * session and then silently discard every mouse and keyboard event. */
    if (app->request_controller && assigned_role != GRD_ROLE_CONTROLLER) {
        grd_connection_close(app->connection);
        app->connection = NULL;
        set_status(
            app,
            "Control unavailable: another client is already connected"
        );
        return false;
    }
    app->remote_settings_visible = false;
    app->remote_heartbeat_sent_micros = 0ULL;
    app->escape_press_count = 0U;
    app->escape_sequence_started_micros = 0ULL;
    if (app->config.remote_fullscreen) {
        (void)set_remote_fullscreen(app, true);
    }
#if defined(_WIN32)
    if (app->window != NULL) {
        SDL_Renderer *renderer = SDL_GetRenderer(app->window);
        if (renderer != NULL) {
            SDL_PropertiesID properties = SDL_GetRendererProperties(renderer);
            app->d3d11_device = SDL_GetPointerProperty(
                properties, SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, NULL
            );
        }
    }
#endif
    grd_app_configure_display(app);
    (void)send_display_capabilities(app);
    /* Protocol v3: advertise the codecs this client can actually decode so
     * the host never starts HEVC/AV1 against a client that would have to
     * "fall back" with an H.264 decoder. */
    const grd_video_caps video_caps = {
        .codec_bitmask = grd_client_decode_caps()
    };
    (void)grd_connection_send(
        app->connection,
        GRD_PACKET_VIDEO_CAPS,
        &video_caps,
        sizeof(video_caps),
        NULL
    );
    memset(&error, 0, sizeof(error));
    app->media_connection = grd_connect_media(
        address,
        port,
        app->connect_password,
        &app->config,
        client_packet,
        app,
        &error
    );
    publish_error(app, &error);
    if (app->media_connection == NULL) {
        grd_connection_close(app->connection);
        app->connection = NULL;
        if (app->remote_fullscreen_active) {
            (void)set_remote_fullscreen(app, false);
        }
        set_status(
            app,
            error.code != GRD_OK && error.message[0] != '\0'
                ? error.message
                : "UDP media channel unavailable"
        );
        return false;
    }
    atomic_store_explicit(
        &app->relative_input_udp_sent, 0U, memory_order_relaxed
    );
    atomic_store_explicit(
        &app->relative_input_tcp_sent, 0U, memory_order_relaxed
    );
    atomic_store_explicit(
        &app->relative_input_send_failures, 0U, memory_order_relaxed
    );
    atomic_store_explicit(
        &app->remote_input_rejection_reports, 0U, memory_order_relaxed
    );
    (void)SDL_SetAtomicInt(&app->keyframe_request_pending, 0);
    (void)SDL_SetAtomicInt(&app->remote_waiting_for_repair_idr, 0);
    atomic_store_explicit(
        &app->keyframe_requested_micros, 0ULL, memory_order_relaxed
    );
    app->keyframe_request_interval_us = 500000U;
    if (!start_media_threads(app)) {
        grd_connection_close(app->media_connection);
        app->media_connection = NULL;
        grd_connection_close(app->connection);
        app->connection = NULL;
        if (app->remote_fullscreen_active) {
            (void)set_remote_fullscreen(app, false);
        }
        SDL_LockMutex(app->decoder_mutex);
        grd_decoder_destroy(app->decoder);
        app->decoder = NULL;
        (void)SDL_SetAtomicInt(&app->remote_decoder_ready, 0);
        SDL_UnlockMutex(app->decoder_mutex);
        SDL_LockMutex(app->audio_mutex);
        grd_audio_decoder_destroy(app->audio_decoder);
        app->audio_decoder = NULL;
        if (app->audio_playback != NULL) {
            SDL_DestroyAudioStream(app->audio_playback);
            app->audio_playback = NULL;
        }
        SDL_UnlockMutex(app->audio_mutex);
        set_status(app, "Unable to start media threads");
        return false;
    }
    /* Bring the remote view to the front: a window left behind other apps
     * is reported OCCLUDED by the compositor, CAMetalLayer stops providing
     * drawables and every texture upload fails silently (observed for whole
     * sessions: 'finestra occlusa, streak >7000'). */
    if (app->window != NULL) {
        (void)SDL_RaiseWindow(app->window);
    }
    app->mode = GRD_APP_REMOTE;
    set_remote_keyboard_grab(
        app,
        grd_connection_role(app->connection) == GRD_ROLE_CONTROLLER
    );
    set_status(
        app,
        grd_connection_role(app->connection) == GRD_ROLE_CONTROLLER
            ? "Connected with control"
            : "Connected in view-only mode"
    );
    return true;
}

bool grd_app_initialize(grd_app *app)
{
    memset(app, 0, sizeof(*app));
    app->request_controller = true;
    app->default_cursor = SDL_GetDefaultCursor();
    app->text_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    if (grd_platform_initialize(&app->last_error) != GRD_OK ||
        grd_config_load(&app->config, &app->last_error) != GRD_OK) {
        return false;
    }
    /* host_enabled is deliberately cleared by stop_host() on an orderly
     * shutdown.  If it is still set here, the previous process disappeared
     * while sharing (for example after a crash or an updater restart).  In
     * that case restore the user's already-authorized LAN host instead of
     * leaving a healthy GUI process running but undiscoverable. */
    const bool restore_host_requested = app->config.host_enabled;
    app->gpu = grd_gpu_detect();
    app->selected_pipeline = grd_gpu_select(
        &app->gpu, app->config.gpu_preference
    );
    (void)SDL_SetAtomicInt(
        &app->active_host_pipeline,
        (int)app->selected_pipeline
    );
    app->monitor_count = grd_platform_monitors(
        app->monitors, GRD_MAX_MONITORS
    );
    for (size_t index = 0U; index < app->monitor_count; ++index) {
        if (app->monitors[index].primary) {
            app->selected_monitor = index;
            break;
        }
    }
    app->frame_mutex = SDL_CreateMutex();
    app->cursor_mutex = SDL_CreateMutex();
    app->clipboard_mutex = SDL_CreateMutex();
    app->decoder_mutex = SDL_CreateMutex();
    app->audio_mutex = SDL_CreateMutex();
    app->error_mutex = SDL_CreateMutex();
    if (app->frame_mutex == NULL || app->cursor_mutex == NULL ||
        app->clipboard_mutex == NULL || app->decoder_mutex == NULL ||
        app->audio_mutex == NULL || app->error_mutex == NULL ||
        !media_queue_init(&app->video_queue) ||
        !media_queue_init(&app->audio_queue)) {
        if (app->frame_mutex != NULL) {
            SDL_DestroyMutex(app->frame_mutex);
            app->frame_mutex = NULL;
        }
        if (app->cursor_mutex != NULL) {
            SDL_DestroyMutex(app->cursor_mutex);
            app->cursor_mutex = NULL;
        }
        if (app->clipboard_mutex != NULL) {
            SDL_DestroyMutex(app->clipboard_mutex);
            app->clipboard_mutex = NULL;
        }
        if (app->decoder_mutex != NULL) {
            SDL_DestroyMutex(app->decoder_mutex);
            app->decoder_mutex = NULL;
        }
        if (app->audio_mutex != NULL) {
            SDL_DestroyMutex(app->audio_mutex);
            app->audio_mutex = NULL;
        }
        if (app->error_mutex != NULL) {
            SDL_DestroyMutex(app->error_mutex);
            app->error_mutex = NULL;
        }
        media_queue_destroy(&app->video_queue);
        media_queue_destroy(&app->audio_queue);
        grd_platform_shutdown();
        return false;
    }
    app->wake_event_type = SDL_RegisterEvents(1U);
    app->discovery = grd_discovery_start(
        app->config.device_id,
        app->config.device_name,
        app->config.port,
        &app->last_error
    );
    if (app->discovery == NULL) {
        GRD_WARN(
            "discovery failed to start: %s",
            app->last_error.message[0] != '\0'
                ? app->last_error.message
                : "unknown error"
        );
    } else {
        grd_discovery_set_remote_access(
            app->discovery, grd_platform_os(), 0U, 0U
        );
    }
    if (!grd_remote_access_username_valid(
            app->config.remote_access_username
        )) {
        (void)grd_remote_access_default_username(
            app->config.remote_access_username,
            sizeof(app->config.remote_access_username)
        );
    }
    if (restore_host_requested) {
        GRD_INFO(
            "sharing was active before shutdown: restoring LAN host"
        );
        start_host(app);
        if (app->host == NULL) {
            GRD_WARN(
                "LAN host restore failed; request retained for the next "
                "launch"
            );
        }
    } else {
        set_status(app, "Ready");
    }
    return true;
}

void grd_app_shutdown(grd_app *app)
{
    if (app == NULL) {
        return;
    }
    if (app->connection != NULL) {
        disconnect(app);
    }
    if (app->host != NULL) {
        stop_host(app);
    }
    if (app->config_dirty) {
        (void)grd_config_save(&app->config, &app->last_error);
        app->config_dirty = false;
    }
    grd_discovery_stop(app->discovery);
    app->discovery = NULL;
    destroy_remote_texture(app);
    if (app->remote_cursor_texture != NULL) {
        SDL_DestroyTexture(app->remote_cursor_texture);
    }
    grd_platform_frame_release(&app->remote_frame);
    free(app->clipboard_cache);
    app->clipboard_cache = NULL;
    media_queue_destroy(&app->video_queue);
    media_queue_destroy(&app->audio_queue);
    if (app->cursor_mutex != NULL) SDL_DestroyMutex(app->cursor_mutex);
    if (app->clipboard_mutex != NULL) SDL_DestroyMutex(app->clipboard_mutex);
    if (app->frame_mutex != NULL) SDL_DestroyMutex(app->frame_mutex);
    if (app->decoder_mutex != NULL) SDL_DestroyMutex(app->decoder_mutex);
    if (app->audio_mutex != NULL) SDL_DestroyMutex(app->audio_mutex);
    if (app->error_mutex != NULL) SDL_DestroyMutex(app->error_mutex);
    if (app->text_cursor != NULL) {
        SDL_DestroyCursor(app->text_cursor);
        app->text_cursor = NULL;
    }
    app->default_cursor = NULL;
    grd_platform_shutdown();
}

#if defined(__APPLE__)
static SDL_Texture *create_metal_pixelbuffer_texture(
    SDL_Renderer *renderer,
    const grd_frame *frame
)
{
    if (renderer == NULL || frame == NULL || frame->owner == NULL ||
        (frame->format != GRD_PIXEL_BGRA8 &&
         frame->format != GRD_PIXEL_NV12 &&
         frame->format != GRD_PIXEL_P010)) {
        return NULL;
    }
    SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0U) {
        return NULL;
    }
    const SDL_PixelFormat sdl_format =
        frame->format == GRD_PIXEL_NV12
            ? SDL_PIXELFORMAT_NV12
            : frame->format == GRD_PIXEL_P010
                  ? SDL_PIXELFORMAT_P010
                  : SDL_PIXELFORMAT_BGRA32;
    (void)SDL_SetNumberProperty(
        properties,
        SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
        (Sint64)sdl_format
    );
    /* YUV textures need an explicit colorspace: without it
     * METAL_CreateTexture fails with 'Unsupported YUV colorspace'.
     * H.264 HD content is BT.709 limited range. */
    if (frame->format == GRD_PIXEL_NV12 ||
        frame->format == GRD_PIXEL_P010) {
        (void)SDL_SetNumberProperty(
            properties,
            SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
            (Sint64)SDL_COLORSPACE_BT709_LIMITED
        );
    }
    (void)SDL_SetNumberProperty(
        properties,
        SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
        (Sint64)SDL_TEXTUREACCESS_STATIC
    );
    (void)SDL_SetNumberProperty(
        properties,
        SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,
        (Sint64)frame->width
    );
    (void)SDL_SetNumberProperty(
        properties,
        SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,
        (Sint64)frame->height
    );
    (void)SDL_SetPointerProperty(
        properties,
        SDL_PROP_TEXTURE_CREATE_METAL_PIXELBUFFER_POINTER,
        frame->owner
    );
    SDL_Texture *texture = SDL_CreateTextureWithProperties(renderer, properties);
    SDL_DestroyProperties(properties);
    return texture;
}
#endif

static void destroy_remote_texture(grd_app *app)
{
    if (app->remote_texture != NULL) {
        SDL_DestroyTexture(app->remote_texture);
        app->remote_texture = NULL;
    }
    app->remote_texture_scale_mode = SDL_SCALEMODE_INVALID;
    if (app->remote_texture_native) {
        grd_platform_frame_release(&app->remote_texture_frame);
        app->remote_texture_native = false;
    }
    memset(&app->remote_texture_frame, 0, sizeof(app->remote_texture_frame));
}

static void apply_remote_texture_scale_mode(grd_app *app)
{
    if (app == NULL || app->remote_texture == NULL) {
        return;
    }
    /* PIXELART is nearest-neighbour sampling and is intended for sprites,
     * not photographic/game video. Linear filtering is the appropriate GPU
     * path for a downscaled H.264 frame; disabling the quality filter keeps
     * a cheaper nearest sample available for very weak clients. */
    const SDL_ScaleMode requested = app->config.sharp_video_scaling
                                        ? SDL_SCALEMODE_LINEAR
                                        : SDL_SCALEMODE_NEAREST;
    if (app->remote_texture_scale_mode == requested) {
        return;
    }
    if (SDL_SetTextureScaleMode(app->remote_texture, requested)) {
        app->remote_texture_scale_mode = requested;
    } else {
        warn_throttled(
            "unable to apply texture scaling: %s", SDL_GetError()
        );
    }
}

void grd_app_refresh_remote_texture(grd_app *app, SDL_Renderer *renderer)
{
    grd_frame frame_to_upload = {0};
    uint64_t frame_generation = 0U;
    /* Consume the raise request set by the receive thread: window
     * operations must run on the main thread (AppKit assertion otherwise). */
    if (app->window != NULL &&
        SDL_CompareAndSwapAtomicInt(&app->remote_raise_pending, 1, 0)) {
        (void)SDL_RaiseWindow(app->window);
        app->remote_window_raised = true;
    }
    grd_cursor_shape cursor_shape;
    bool cursor_shape_changed;
    uint64_t cursor_shape_generation;
    SDL_LockMutex(app->frame_mutex);
    if (app->remote_frame_generation != app->displayed_generation &&
        (app->remote_frame.data != NULL || app->remote_frame.owner != NULL)) {
        frame_to_upload = app->remote_frame;
        memset(&app->remote_frame, 0, sizeof(app->remote_frame));
        frame_generation = app->remote_frame_generation;
    }
    SDL_UnlockMutex(app->frame_mutex);

    /* The decoder only holds frame_mutex while handing off ownership. Texture
     * creation and the CPU->GPU upload happen after unlocking it, so a slow
     * renderer cannot prevent the receiver from publishing a newer frame. */
    if (frame_to_upload.data != NULL) {
#if defined(__APPLE__)
        if (app->metal_renderer && frame_to_upload.owner != NULL &&
            (frame_to_upload.format == GRD_PIXEL_BGRA8 ||
             frame_to_upload.format == GRD_PIXEL_NV12 ||
             frame_to_upload.format == GRD_PIXEL_P010)) {
            /* Cache the native texture: when the decoder hands back the same
             * CVPixelBuffer (an unchanged capture frame), keep the existing
             * GPU texture and skip the per-frame create/destroy churn. */
            const bool same_buffer =
                app->remote_texture_native && app->remote_texture != NULL &&
                app->remote_texture_frame.owner == frame_to_upload.owner;
            if (same_buffer) {
                grd_platform_frame_release(&frame_to_upload);
                memset(&frame_to_upload, 0, sizeof(frame_to_upload));
                app->displayed_generation = frame_generation;
                atomic_fetch_add_explicit(
                    &app->presented_frames, 1U, memory_order_relaxed
                );
            } else {
                /* Create the replacement first, then release the previous
                 * texture: a transient allocation failure keeps the last
                 * frame visible instead of falling back to a CPU upload. */
                SDL_Texture *next_texture = create_metal_pixelbuffer_texture(
                    renderer, &frame_to_upload
                );
                if (next_texture != NULL) {
                    const SDL_PixelFormat remote_format =
                        frame_to_upload.format == GRD_PIXEL_NV12
                            ? SDL_PIXELFORMAT_NV12
                            : frame_to_upload.format == GRD_PIXEL_P010
                                  ? SDL_PIXELFORMAT_P010
                                  : SDL_PIXELFORMAT_BGRA32;
                    destroy_remote_texture(app);
                    app->remote_texture = next_texture;
                    app->remote_texture_native = true;
                    app->remote_texture_frame = frame_to_upload;
                    memset(&frame_to_upload, 0, sizeof(frame_to_upload));
                    app->remote_texture_format = remote_format;
                    app->displayed_generation = frame_generation;
                    atomic_fetch_add_explicit(
                        &app->presented_frames, 1U, memory_order_relaxed
                    );
                }
            }
        }
#endif
        if (app->metal_renderer &&
            (frame_to_upload.format == GRD_PIXEL_NV12 ||
             frame_to_upload.format == GRD_PIXEL_P010) &&
            frame_to_upload.data != NULL) {
            /* YUV frames are rendered exclusively by the Metal zero-copy
             * path (IOSurface wrap + YUV->RGB shader): NO CPU upload
             * fallback. A wrap failure keeps the last good texture and is
             * diagnosed loudly so the cause stays visible in the log. */
            const char *wrap_error = SDL_GetError();
            static uint32_t wrap_log_skip = 0U;
            if (wrap_log_skip == 0U || wrap_log_skip >= 900U) {
                GRD_WARN(
                    "Metal YUV wrapping failed (%ux%u): %s",
                    frame_to_upload.width,
                    frame_to_upload.height,
                    wrap_error != NULL && wrap_error[0] != '\0'
                        ? wrap_error
                        : "error without a message"
                );
                wrap_log_skip = 1U;
            } else {
                ++wrap_log_skip;
            }
            grd_platform_frame_release(&frame_to_upload);
            memset(&frame_to_upload, 0, sizeof(frame_to_upload));
            goto frame_upload_done;
        }
        if (!app->metal_renderer &&
            (frame_to_upload.format == GRD_PIXEL_NV12 ||
             frame_to_upload.format == GRD_PIXEL_P010) &&
            frame_to_upload.data != NULL) {
            /* YUV frames on a non-Metal renderer have no valid CPU path
             * (a raw NV12/P010 upload as RGBA is the green split-screen
             * artifact): keep the last good frame instead. */
            grd_platform_frame_release(&frame_to_upload);
            memset(&frame_to_upload, 0, sizeof(frame_to_upload));
            goto frame_upload_done;
        }
#if defined(_WIN32)
        if (frame_to_upload.format == GRD_PIXEL_D3D11_RGBA &&
            frame_to_upload.owner != NULL) {
            /* GPU-resident decode output: the uploader owns a persistent
             * D3D11 texture that is rewritten every frame, so the SDL
             * wrapper is created once and reused (no per-frame allocation). */
            const bool same_texture =
                app->remote_texture_native && app->remote_texture != NULL &&
                app->remote_texture_frame.owner == frame_to_upload.owner;
            if (same_texture) {
                grd_platform_frame_release(&frame_to_upload);
                memset(&frame_to_upload, 0, sizeof(frame_to_upload));
                app->displayed_generation = frame_generation;
                atomic_fetch_add_explicit(
                    &app->presented_frames, 1U, memory_order_relaxed
                );
            } else {
                SDL_PropertiesID properties = SDL_CreateProperties();
                SDL_Texture *next_texture = NULL;
                if (properties != 0U) {
                    (void)SDL_SetNumberProperty(
                        properties,
                        SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                        (Sint64)SDL_PIXELFORMAT_RGBA32
                    );
                    (void)SDL_SetNumberProperty(
                        properties,
                        SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                        (Sint64)SDL_TEXTUREACCESS_STATIC
                    );
                    (void)SDL_SetNumberProperty(
                        properties,
                        SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,
                        (Sint64)frame_to_upload.width
                    );
                    (void)SDL_SetNumberProperty(
                        properties,
                        SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,
                        (Sint64)frame_to_upload.height
                    );
                    (void)SDL_SetPointerProperty(
                        properties,
                        SDL_PROP_TEXTURE_CREATE_D3D11_TEXTURE_POINTER,
                        frame_to_upload.owner
                    );
                    next_texture = SDL_CreateTextureWithProperties(
                        renderer, properties
                    );
                    SDL_DestroyProperties(properties);
                }
                if (next_texture != NULL) {
                    destroy_remote_texture(app);
                    app->remote_texture = next_texture;
                    app->remote_texture_native = true;
                    app->remote_texture_frame = frame_to_upload;
                    memset(&frame_to_upload, 0, sizeof(frame_to_upload));
                    app->remote_texture_format = SDL_PIXELFORMAT_RGBA32;
                    app->displayed_generation = frame_generation;
                    atomic_fetch_add_explicit(
                        &app->presented_frames, 1U, memory_order_relaxed
                    );
                }
            }
        }
#endif
        if (frame_to_upload.data == NULL) {
            /* Native Metal textures own the frame until destroy_remote_texture. */
            goto frame_upload_done;
        }
        if (app->remote_texture_native) {
            destroy_remote_texture(app);
        }
        const SDL_PixelFormat texture_format =
            frame_to_upload.format == GRD_PIXEL_BGRA8
                ? SDL_PIXELFORMAT_BGRA32
                : SDL_PIXELFORMAT_RGBA32;
        if (app->remote_texture != NULL &&
            app->remote_texture_format != texture_format) {
            destroy_remote_texture(app);
        }
        if (app->remote_texture == NULL) {
            app->remote_texture = SDL_CreateTexture(
                renderer,
                texture_format,
                SDL_TEXTUREACCESS_STREAMING,
                (int)frame_to_upload.width,
                (int)frame_to_upload.height
            );
        } else {
            float width = 0.0F;
            float height = 0.0F;
            (void)SDL_GetTextureSize(app->remote_texture, &width, &height);
            if ((uint32_t)width != frame_to_upload.width ||
                (uint32_t)height != frame_to_upload.height) {
                destroy_remote_texture(app);
                app->remote_texture = SDL_CreateTexture(
                    renderer,
                    texture_format,
                    SDL_TEXTUREACCESS_STREAMING,
                    (int)frame_to_upload.width,
                    (int)frame_to_upload.height
                );
            }
        }
        if (app->remote_texture != NULL) {
            const bool upload_ok = SDL_UpdateTexture(
                app->remote_texture,
                NULL,
                frame_to_upload.data,
                (int)frame_to_upload.stride
            );
            /* SDL3 bool polarity: true is success. The previous SDL2-style
             * check counted every successful upload as a failure, eventually
             * forcing a needless Metal -> software renderer fallback. */
            if (!upload_ok) {
                atomic_fetch_add_explicit(
                    &app->upload_failures, 1U, memory_order_relaxed
                );
                ++app->upload_failure_streak;
                const SDL_WindowFlags window_flags =
                    app->window != NULL
                        ? SDL_GetWindowFlags(app->window)
                        : 0U;
                const bool state_changed =
                    app->last_upload_flags != (uint32_t)window_flags;
                app->last_upload_flags = (uint32_t)window_flags;
                /* Log on the first failure, on window-state changes, and as
                 * a ~30 s heartbeat: a permanently occluded window must not
                 * spam one line per second. */
                if (app->upload_failure_streak == 1U ||
                    state_changed ||
                    app->upload_failure_streak % 900U == 1U) {
                    const char *window_state =
                        (window_flags & SDL_WINDOW_OCCLUDED) != 0U
                            ? "window occluded"
                            : (window_flags & SDL_WINDOW_HIDDEN) != 0U
                                  ? "window hidden"
                                  : (window_flags &
                                     SDL_WINDOW_MINIMIZED) != 0U
                                        ? "window minimized"
                                        : "window visible";
                    const char *sdl_error = SDL_GetError();
                    GRD_WARN(
                        "texture upload failed: %s (%s, streak %u, "
                        "flags 0x%llx)",
                        sdl_error != NULL && sdl_error[0] != '\0'
                            ? sdl_error
                            : "Metal error without a message",
                        window_state,
                        (unsigned)app->upload_failure_streak,
                        (unsigned long long)window_flags
                    );
                }
            } else {
                atomic_fetch_add_explicit(
                    &app->presented_frames, 1U, memory_order_relaxed
                );
                if (app->upload_failure_streak >= 5U) {
                    GRD_INFO(
                        "texture upload restored after %u failures",
                        (unsigned)app->upload_failure_streak
                    );
                }
                app->upload_failure_streak = 0U;
            }
            app->remote_texture_format = texture_format;
            app->displayed_generation = frame_generation;
        }
        grd_platform_frame_release(&frame_to_upload);
frame_upload_done:
        apply_remote_texture_scale_mode(app);
    }

    /* A GPU renderer that cannot obtain drawables fails every upload with an
     * empty SDL error; after a persistent streak ask the main thread for the
     * software renderer instead of leaving the remote view black forever. */
    if (app->metal_renderer &&
        app->upload_failure_streak >= GRD_UPLOAD_FALLBACK_STREAK) {
        (void)SDL_SetAtomicInt(&app->renderer_fallback_requested, 1);
    }

    SDL_LockMutex(app->cursor_mutex);
    cursor_shape_changed =
        app->remote_cursor_shape_generation !=
        app->displayed_cursor_shape_generation;
    cursor_shape = app->remote_cursor_shape;
    cursor_shape_generation = app->remote_cursor_shape_generation;
    SDL_UnlockMutex(app->cursor_mutex);

    if (!cursor_shape_changed) {
        return;
    }
    if (cursor_shape.width == 0U || cursor_shape.height == 0U) {
        if (app->remote_cursor_texture != NULL) {
            SDL_DestroyTexture(app->remote_cursor_texture);
            app->remote_cursor_texture = NULL;
        }
        app->displayed_cursor_shape_generation = cursor_shape_generation;
        return;
    }
    if (app->remote_cursor_texture == NULL) {
        app->remote_cursor_texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING,
            (int)cursor_shape.width,
            (int)cursor_shape.height
        );
    } else {
        float width = 0.0F;
        float height = 0.0F;
        (void)SDL_GetTextureSize(
            app->remote_cursor_texture, &width, &height
        );
        if ((uint32_t)width != cursor_shape.width ||
            (uint32_t)height != cursor_shape.height) {
            SDL_DestroyTexture(app->remote_cursor_texture);
            app->remote_cursor_texture = SDL_CreateTexture(
                renderer,
                SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_STREAMING,
                (int)cursor_shape.width,
                (int)cursor_shape.height
            );
        }
    }
    if (app->remote_cursor_texture != NULL) {
        uint8_t compact_cursor[GRD_CURSOR_MAX_WIDTH *
                               GRD_CURSOR_MAX_HEIGHT * 4U];
        const size_t row_size = (size_t)cursor_shape.width * 4U;
        for (uint16_t row = 0U; row < cursor_shape.height; ++row) {
            memcpy(
                compact_cursor + (size_t)row * row_size,
                cursor_shape.pixels +
                    (size_t)row * GRD_CURSOR_MAX_WIDTH * 4U,
                row_size
            );
        }
        (void)SDL_UpdateTexture(
            app->remote_cursor_texture,
            NULL,
            compact_cursor,
            (int)row_size
        );
        app->displayed_cursor_shape_generation = cursor_shape_generation;
    }
}

void grd_app_reset_remote_texture(grd_app *app)
{
    if (app == NULL) {
        return;
    }
    destroy_remote_texture(app);
    if (app->remote_cursor_texture != NULL) {
        SDL_DestroyTexture(app->remote_cursor_texture);
        app->remote_cursor_texture = NULL;
    }
    app->remote_texture_format = 0;
    app->displayed_generation = 0U;
}

typedef enum grd_ui_button_tone {
    GRD_UI_BUTTON_SECONDARY = 0,
    GRD_UI_BUTTON_PRIMARY,
    GRD_UI_BUTTON_DARK,
    GRD_UI_BUTTON_DANGER
} grd_ui_button_tone;

typedef struct grd_ui_card_style {
    struct nk_style_item background;
    struct nk_color border_color;
    struct nk_vec2 padding;
    float border;
    float rounding;
} grd_ui_card_style;

static void ui_label(
    struct nk_context *context,
    const struct nk_user_font *font,
    const char *text,
    nk_flags alignment,
    struct nk_color color
)
{
    const bool pushed = font != NULL && nk_style_push_font(context, font);
    nk_label_colored(context, text, alignment, color);
    if (pushed) {
        (void)nk_style_pop_font(context);
    }
}

static void ui_label_wrap(
    struct nk_context *context,
    const struct nk_user_font *font,
    const char *text,
    struct nk_color color
)
{
    const bool pushed = font != NULL && nk_style_push_font(context, font);
    nk_label_colored_wrap(context, text, color);
    if (pushed) {
        (void)nk_style_pop_font(context);
    }
}

static bool ui_button(
    struct nk_context *context,
    const char *label,
    grd_ui_button_tone tone
)
{
    if (tone == GRD_UI_BUTTON_SECONDARY) {
        return nk_button_label(context, label) != 0;
    }
    const struct nk_style_button saved = context->style.button;
    struct nk_color normal = nk_rgb(245, 245, 247);
    struct nk_color hover = nk_rgb(232, 232, 237);
    struct nk_color active = nk_rgb(220, 220, 225);
    struct nk_color text = nk_rgb(29, 29, 31);
    struct nk_color border = nk_rgb(245, 245, 247);
    if (tone == GRD_UI_BUTTON_PRIMARY) {
        normal = nk_rgb(0, 113, 227);
        hover = nk_rgb(0, 119, 237);
        active = nk_rgb(0, 102, 204);
        text = nk_rgb(255, 255, 255);
        border = normal;
    } else if (tone == GRD_UI_BUTTON_DARK) {
        normal = nk_rgb(29, 29, 31);
        hover = nk_rgb(66, 66, 69);
        active = nk_rgb(0, 0, 0);
        text = nk_rgb(255, 255, 255);
        border = normal;
    } else if (tone == GRD_UI_BUTTON_DANGER) {
        normal = nk_rgb(255, 255, 255);
        hover = nk_rgb(255, 239, 239);
        active = nk_rgb(255, 224, 224);
        text = nk_rgb(220, 38, 38);
        border = nk_rgb(239, 186, 186);
    }
    context->style.button.normal = nk_style_item_color(normal);
    context->style.button.hover = nk_style_item_color(hover);
    context->style.button.active = nk_style_item_color(active);
    context->style.button.text_normal = text;
    context->style.button.text_hover = text;
    context->style.button.text_active = text;
    context->style.button.border_color = border;
    context->style.button.border = 1.0F;
    const bool pressed = nk_button_label(context, label) != 0;
    context->style.button = saved;
    return pressed;
}

static bool ui_radio_option(
    struct nk_context *context,
    const struct nk_user_font *font,
    const char *label,
    bool selected
)
{
    struct nk_rect bounds;
    const enum nk_widget_layout_states state = nk_widget(&bounds, context);
    if (state == NK_WIDGET_INVALID) {
        return false;
    }
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    const bool interactive = state == NK_WIDGET_VALID;
    const bool hovered = interactive && nk_input_is_mouse_hovering_rect(
                                              &context->input, bounds
                                          );
    if (hovered) {
        nk_fill_rect(
            canvas, bounds, 8.0F, nk_rgb(245, 245, 247)
        );
    }
    const float diameter = 16.0F;
    const struct nk_rect ring = nk_rect(
        bounds.x + 6.0F,
        bounds.y + (bounds.h - diameter) * 0.5F,
        diameter,
        diameter
    );
    nk_fill_circle(canvas, ring, nk_rgb(255, 255, 255));
    nk_stroke_circle(
        canvas,
        ring,
        1.5F,
        selected ? nk_rgb(0, 113, 227) : nk_rgb(174, 174, 178)
    );
    if (selected) {
        nk_fill_circle(
            canvas,
            nk_rect(ring.x + 4.0F, ring.y + 4.0F, 8.0F, 8.0F),
            nk_rgb(0, 113, 227)
        );
    }
    const struct nk_user_font *label_font =
        font != NULL ? font : context->style.font;
    const float font_height = label_font != NULL ? label_font->height : 14.0F;
    nk_draw_text(
        canvas,
        nk_rect(
            ring.x + ring.w + 9.0F,
            bounds.y + (bounds.h - font_height) * 0.5F,
            bounds.w - ring.w - 21.0F,
            font_height
        ),
        label,
        (int)strlen(label),
        label_font,
        nk_rgba(0, 0, 0, 0),
        state == NK_WIDGET_DISABLED
            ? nk_rgb(174, 174, 178)
            : nk_rgb(29, 29, 31)
    );
    return interactive && nk_input_mouse_clicked(
                              &context->input, NK_BUTTON_LEFT, bounds
                          );
}

static bool ui_rate_selector(
    struct nk_context *context,
    const struct nk_user_font *font,
    uint32_t current,
    const uint32_t *rates,
    size_t rate_count,
    const char *unit,
    uint32_t *selected
)
{
    if (rates == NULL || rate_count == 0U || selected == NULL) {
        return false;
    }
    nk_layout_row_dynamic(context, 34.0F, (int)rate_count);
    for (size_t index = 0U; index < rate_count; ++index) {
        char label[32];
        if (rates[index] == 0U) {
            (void)snprintf(label, sizeof(label), "Auto");
        } else if (strcmp(unit, "p") == 0 && rates[index] == 2160U) {
            (void)snprintf(label, sizeof(label), "4K");
        } else if (strcmp(unit, "p") == 0) {
            (void)snprintf(label, sizeof(label), "%up", rates[index]);
        } else {
            (void)snprintf(
                label, sizeof(label), "%u %s", rates[index], unit
            );
        }
        if (ui_radio_option(
                context, font, label, current == rates[index]
            )) {
            *selected = rates[index];
            return current != rates[index];
        }
    }
    return false;
}

static bool ui_number_stepper(
    struct nk_context *context,
    const struct nk_user_font *font,
    const char *label,
    int *value,
    int minimum,
    int maximum
)
{
    struct nk_rect bounds;
    const enum nk_widget_layout_states state = nk_widget(&bounds, context);
    if (state == NK_WIDGET_INVALID || value == NULL) {
        return false;
    }
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    const bool interactive = state == NK_WIDGET_VALID;
    const struct nk_color border = nk_rgb(210, 210, 215);
    const struct nk_color muted = state == NK_WIDGET_DISABLED
                                      ? nk_rgb(174, 174, 178)
                                      : nk_rgb(66, 66, 69);
    nk_fill_rect(canvas, bounds, 8.0F, nk_rgb(255, 255, 255));
    nk_stroke_rect(canvas, bounds, 8.0F, 1.0F, border);

    const float button_width = 28.0F;
    const float value_width = 34.0F;
    const struct nk_rect minus = nk_rect(
        bounds.x + bounds.w - button_width * 2.0F - value_width,
        bounds.y,
        button_width,
        bounds.h
    );
    const struct nk_rect number = nk_rect(
        minus.x + minus.w, bounds.y, value_width, bounds.h
    );
    const struct nk_rect plus = nk_rect(
        number.x + number.w, bounds.y, button_width, bounds.h
    );
    if (interactive && nk_input_is_mouse_hovering_rect(
                           &context->input, minus
                       )) {
        nk_fill_rect(canvas, minus, 7.0F, nk_rgb(245, 245, 247));
    }
    if (interactive && nk_input_is_mouse_hovering_rect(
                           &context->input, plus
                       )) {
        nk_fill_rect(canvas, plus, 7.0F, nk_rgb(245, 245, 247));
    }
    nk_stroke_line(
        canvas,
        minus.x,
        bounds.y + 8.0F,
        minus.x,
        bounds.y + bounds.h - 8.0F,
        1.0F,
        border
    );
    nk_stroke_line(
        canvas,
        plus.x,
        bounds.y + 8.0F,
        plus.x,
        bounds.y + bounds.h - 8.0F,
        1.0F,
        border
    );

    const struct nk_user_font *label_font =
        font != NULL ? font : context->style.font;
    const float font_height = label_font != NULL ? label_font->height : 14.0F;
    nk_draw_text(
        canvas,
        nk_rect(
            bounds.x + 12.0F,
            bounds.y + (bounds.h - font_height) * 0.5F,
            minus.x - bounds.x - 16.0F,
            font_height
        ),
        label,
        (int)strlen(label),
        label_font,
        nk_rgba(0, 0, 0, 0),
        muted
    );

    char number_text[16];
    (void)snprintf(number_text, sizeof(number_text), "%d", *value);
    const char *symbols[2] = {"-", "+"};
    const struct nk_rect text_bounds[3] = {minus, number, plus};
    const char *texts[3] = {symbols[0], number_text, symbols[1]};
    for (size_t index = 0U; index < 3U; ++index) {
        const int length = (int)strlen(texts[index]);
        const float width = label_font != NULL && label_font->width != NULL
                                ? label_font->width(
                                      label_font->userdata,
                                      font_height,
                                      texts[index],
                                      length
                                  )
                                : 8.0F;
        nk_draw_text(
            canvas,
            nk_rect(
                text_bounds[index].x +
                    (text_bounds[index].w - width) * 0.5F,
                bounds.y + (bounds.h - font_height) * 0.5F,
                width + 1.0F,
                font_height
            ),
            texts[index],
            length,
            label_font,
            nk_rgba(0, 0, 0, 0),
            index == 1U ? nk_rgb(29, 29, 31) : muted
        );
    }

    if (!interactive) {
        return false;
    }
    if (nk_input_mouse_clicked(&context->input, NK_BUTTON_LEFT, minus) &&
        *value > minimum) {
        --*value;
        return true;
    }
    if (nk_input_mouse_clicked(&context->input, NK_BUTTON_LEFT, plus) &&
        *value < maximum) {
        ++*value;
        return true;
    }
    return false;
}

static void ui_card_style_begin(
    struct nk_context *context,
    grd_ui_card_style *saved
)
{
    saved->background = context->style.window.fixed_background;
    saved->border_color = context->style.window.border_color;
    saved->padding = context->style.window.group_padding;
    saved->border = context->style.window.border;
    saved->rounding = context->style.window.rounding;
    context->style.window.fixed_background =
        nk_style_item_color(nk_rgb(255, 255, 255));
    context->style.window.border_color = nk_rgb(232, 232, 237);
    context->style.window.group_padding = nk_vec2(24.0F, 20.0F);
    context->style.window.border = 1.0F;
    context->style.window.rounding = 18.0F;
}

static void ui_card_style_end(
    struct nk_context *context,
    const grd_ui_card_style *saved
)
{
    context->style.window.fixed_background = saved->background;
    context->style.window.border_color = saved->border_color;
    context->style.window.group_padding = saved->padding;
    context->style.window.border = saved->border;
    context->style.window.rounding = saved->rounding;
}

static bool save_host_settings(grd_app *app, const char *success_message)
{
    grd_error error = {0};
    if (app->new_host_password[0] != '\0' &&
        grd_config_set_password(
            &app->config, app->new_host_password, &error
        ) != GRD_OK) {
        publish_error(app, &error);
        set_status(app, error.message);
        return false;
    }
    if (grd_config_save(&app->config, &error) != GRD_OK) {
        publish_error(app, &error);
        set_status(app, error.message);
        return false;
    }
    memset(app->new_host_password, 0, sizeof(app->new_host_password));
    if (success_message != NULL) {
        set_status(app, success_message);
    }
    return true;
}

static bool save_config_automatically(
    grd_app *app,
    const char *success_message
)
{
    grd_error error = {0};
    if (grd_config_save(&app->config, &error) != GRD_OK) {
        publish_error(app, &error);
        set_status(app, error.message);
        return false;
    }
    if (success_message != NULL) {
        set_status(app, success_message);
    }
    return true;
}

static void draw_client_rate_controls(
    grd_app *app,
    struct nk_context *context,
    bool update_connected_host,
    bool compact
)
{
    static const uint32_t stream_rates[] = {30U, 60U, 90U, 120U};
    static const uint32_t presentation_rates[] = {0U, 60U, 90U, 120U};
    const struct nk_color text = nk_rgb(29, 29, 31);
    const struct nk_color muted = nk_rgb(110, 110, 115);

    nk_layout_row_dynamic(context, 18.0F, 1);
    ui_label(
        context, app->small_font, "Requested stream FPS", NK_TEXT_LEFT, text
    );
    uint32_t selected_rate = app->config.client_target_fps;
    if (ui_rate_selector(
            context,
            app->small_font,
            app->config.client_target_fps,
            stream_rates,
            sizeof(stream_rates) / sizeof(stream_rates[0]),
            "FPS",
            &selected_rate
        )) {
        app->config.client_target_fps = selected_rate;
        (void)save_config_automatically(app, NULL);
        if (update_connected_host) {
            (void)send_display_capabilities(app);
        }
        set_status(app, "Stream frame rate updated");
    }

    nk_layout_row_dynamic(context, 18.0F, 1);
    ui_label(
        context,
        app->small_font,
        "Client display Hz",
        NK_TEXT_LEFT,
        text
    );
    selected_rate = app->config.presentation_hz;
    if (ui_rate_selector(
            context,
            app->small_font,
            app->config.presentation_hz,
            presentation_rates,
            sizeof(presentation_rates) / sizeof(presentation_rates[0]),
            "Hz",
            &selected_rate
        )) {
        app->config.presentation_hz = selected_rate;
        grd_app_configure_display(app);
        (void)save_config_automatically(app, NULL);
        if (update_connected_host) {
            (void)send_display_capabilities(app);
        }
        set_status(
            app,
            selected_rate == 0U
                ? "Automatic ProMotion enabled"
                : "Presentation rate updated"
        );
    }

    nk_layout_row_dynamic(context, 18.0F, 1);
    ui_label(
        context,
        app->small_font,
        "Maximum stream resolution",
        NK_TEXT_LEFT,
        text
    );
    static const uint32_t resolution_heights[] = {
        0U, 1080U, 1440U, 2160U
    };
    uint32_t selected_height = app->config.client_max_height;
    if (ui_rate_selector(
            context,
            app->small_font,
            app->config.client_max_height,
            resolution_heights,
            sizeof(resolution_heights) / sizeof(resolution_heights[0]),
            "p",
            &selected_height
        )) {
        app->config.client_max_height = selected_height;
        (void)save_config_automatically(app, NULL);
        if (update_connected_host) {
            (void)send_display_capabilities(app);
        }
        set_status(
            app,
            selected_height == 0U
                ? "Stream resolution: automatic"
                : selected_height == 2160U
                      ? "Stream resolution: 4K"
                      : selected_height == 1440U
                            ? "Stream resolution: 1440p"
                            : "Stream resolution: 1080p"
        );
    }

    nk_layout_row_dynamic(context, 20.0F, 1);
    ui_label(
        context,
        app->small_font,
        "Workloads offloaded to the client",
        NK_TEXT_LEFT,
        text
    );
    const grd_client_upscale_mode upscale_modes[] = {
        GRD_CLIENT_UPSCALE_NATIVE,
        GRD_CLIENT_UPSCALE_BALANCED,
        GRD_CLIENT_UPSCALE_PERFORMANCE
    };
    const char *upscale_labels[] = {
        "Native resolution",
        "Balanced upscale",
        "Maximum upscale"
    };
    nk_layout_row_dynamic(context, 38.0F, 3);
    for (size_t index = 0U; index < 3U; ++index) {
        if (ui_button(
                context,
                upscale_labels[index],
                app->config.client_upscale_mode == upscale_modes[index]
                    ? GRD_UI_BUTTON_PRIMARY
                    : GRD_UI_BUTTON_SECONDARY
            ) && app->config.client_upscale_mode != upscale_modes[index]) {
            app->config.client_upscale_mode = upscale_modes[index];
            (void)save_config_automatically(app, NULL);
            if (update_connected_host) {
                (void)send_display_capabilities(app);
            }
            set_status(
                app,
                upscale_modes[index] == GRD_CLIENT_UPSCALE_NATIVE
                    ? "Encoding at the requested resolution"
                    : upscale_modes[index] == GRD_CLIENT_UPSCALE_BALANCED
                          ? "Balanced client upscale enabled"
                          : "Maximum client upscale enabled"
            );
        }
    }

    nk_bool local_pacing =
        app->config.client_frame_pacing ? nk_true : nk_false;
    nk_bool cursor_prediction =
        app->config.client_cursor_prediction ? nk_true : nk_false;
    nk_layout_row_dynamic(context, 34.0F, 2);
    if (nk_checkbox_label(
            context, "Local display pacing", &local_pacing
        )) {
        app->config.client_frame_pacing = local_pacing != nk_false;
        grd_app_configure_display(app);
        (void)save_config_automatically(app, NULL);
        if (update_connected_host) {
            (void)send_display_capabilities(app);
        }
    }
    if (nk_checkbox_label(
            context, "Local cursor prediction", &cursor_prediction
        )) {
        app->config.client_cursor_prediction =
            cursor_prediction != nk_false;
        if (!app->config.client_cursor_prediction) {
            SDL_LockMutex(app->cursor_mutex);
            app->predicted_cursor_valid = false;
            SDL_UnlockMutex(app->cursor_mutex);
        }
        (void)save_config_automatically(app, NULL);
        if (update_connected_host) {
            (void)send_display_capabilities(app);
        }
    }

    nk_bool quality_filter =
        app->config.sharp_video_scaling ? nk_true : nk_false;
    nk_layout_row_dynamic(context, 34.0F, 1);
    if (nk_checkbox_label(
            context, "Client GPU video quality filter", &quality_filter
        )) {
        app->config.sharp_video_scaling = quality_filter != nk_false;
        apply_remote_texture_scale_mode(app);
        (void)save_config_automatically(app, NULL);
        if (update_connected_host) {
            (void)send_display_capabilities(app);
        }
    }
    uint32_t requested_width = 0U;
    uint32_t requested_height = 0U;
    uint32_t encoded_width = 0U;
    uint32_t encoded_height = 0U;
    requested_client_stream_dimensions(
        app, &requested_width, &requested_height
    );
    grd_stream_ladder_max_dimensions(
        requested_width,
        requested_height,
        grd_stream_client_offload_level(app->config.client_upscale_mode),
        &encoded_width,
        &encoded_height
    );
    char offload_summary[224];
    if (app->config.client_upscale_mode == GRD_CLIENT_UPSCALE_NATIVE) {
        (void)snprintf(
            offload_summary,
            sizeof(offload_summary),
            "Host up to %ux%u; no upscale. Pacing and cursor are independent.",
            encoded_width,
            encoded_height
        );
    } else {
        (void)snprintf(
            offload_summary,
            sizeof(offload_summary),
            "Host up to %ux%u; client GPU presents up to %ux%u. "
            "Pacing and cursor are independent.",
            encoded_width,
            encoded_height,
            requested_width,
            requested_height
        );
    }
    nk_layout_row_dynamic(context, compact ? 34.0F : 42.0F, 1);
    ui_label_wrap(
        context,
        app->small_font,
        offload_summary,
        muted
    );

    char cadence[224];
    if (compact) {
        (void)snprintf(
            cadence,
            sizeof(cadence),
            "Client %u FPS | host/ABR %u FPS | present %u Hz | max %.0f Hz | pacing %s",
            effective_client_stream_fps(app),
            atomic_load_explicit(
                &app->remote_stream_fps, memory_order_relaxed
            ),
            app->display_target_fps,
            (double)app->display_max_refresh_rate,
            app->config.client_frame_pacing ? "client" : "stream"
        );
    } else {
        (void)snprintf(
            cadence,
            sizeof(cadence),
            "Client requested %u FPS | effective host/ABR %u FPS | "
            "presentation %u Hz | panel max %.0f Hz | pacing %s",
            effective_client_stream_fps(app),
            atomic_load_explicit(
                &app->remote_stream_fps, memory_order_relaxed
            ),
            app->display_target_fps,
            (double)app->display_max_refresh_rate,
            app->config.client_frame_pacing ? "on client" : "on stream"
        );
    }
    nk_layout_row_dynamic(context, compact ? 22.0F : 32.0F, 1);
    if (compact) {
        ui_label(
            context,
            app->small_font,
            cadence,
            NK_TEXT_LEFT,
            muted
        );
    } else {
        ui_label_wrap(context, app->small_font, cadence, muted);
    }
}

static void apply_stream_profile(
    grd_app *app,
    grd_stream_profile profile
)
{
    app->config.stream_profile = profile;
    app->config.abr_enabled = true;
    if (profile == GRD_STREAM_GAMING) {
        app->config.initial_bitrate_kbps = 20000U;
        app->config.target_bitrate_kbps = 30000U;
        app->config.min_bitrate_kbps = 14000U;
        set_status(app, "Adaptive gaming: 20-30 Mbps, minimum 14 Mbps");
    } else if (profile == GRD_STREAM_DESKTOP) {
        app->config.initial_bitrate_kbps = 10000U;
        app->config.target_bitrate_kbps = 14000U;
        app->config.min_bitrate_kbps = 6000U;
        set_status(app, "Desktop profile: 10-14 Mbps, minimum 6 Mbps");
    } else {
        app->config.initial_bitrate_kbps = 20000U;
        app->config.target_bitrate_kbps = 24000U;
        app->config.min_bitrate_kbps = 10000U;
        set_status(app, "Balanced profile: 20-24 Mbps, minimum 10 Mbps");
    }
}

static void open_remote_access_modal(
    grd_app *app,
    const grd_discovered_peer *peer,
    grd_remote_access_kind kind
)
{
    if (app == NULL || peer == NULL || peer->ssh_port == 0U) {
        return;
    }
    app->remote_access_peer = *peer;
    app->remote_access_kind = kind;
    app->remote_access_error[0] = '\0';
    (void)snprintf(
        app->remote_access_username,
        sizeof(app->remote_access_username),
        "%s",
        app->config.remote_access_username
    );
    if (!grd_remote_access_username_valid(app->remote_access_username)) {
        (void)grd_remote_access_default_username(
            app->remote_access_username,
            sizeof(app->remote_access_username)
        );
    }
    app->lan_join_modal_visible = false;
    app->home_settings_visible = false;
    app->remote_access_modal_visible = true;
}

static void draw_connection_card(grd_app *app, struct nk_context *context)
{
    const struct nk_color text = nk_rgb(29, 29, 31);
    const struct nk_color muted = nk_rgb(110, 110, 115);
    const struct nk_color meta = nk_rgb(134, 134, 139);

    if (app->discovery != NULL) {
        app->peer_count = grd_discovery_peers(
            app->discovery, app->peers, 32U
        );
    }

    nk_layout_row_dynamic(context, 30.0F, 1);
    ui_label(
        context, app->heading_font, "Connect to a computer", NK_TEXT_LEFT, text
    );
    nk_layout_row_dynamic(context, 38.0F, 1);
    ui_label_wrap(
        context,
        app->small_font,
        "Choose a device discovered automatically on the local network.",
        muted
    );

    nk_layout_row_begin(context, NK_DYNAMIC, 40.0F, 3);
    nk_layout_row_push(context, 0.50F);
    ui_label(
        context, app->small_font, "Available devices", NK_TEXT_LEFT, text
    );
    nk_layout_row_push(context, 0.22F);
    char peer_count[48];
    (void)snprintf(
        peer_count,
        sizeof(peer_count),
        "%zu detected",
        app->peer_count
    );
    ui_label(context, app->small_font, peer_count, NK_TEXT_RIGHT, meta);
    nk_layout_row_push(context, 0.28F);
    if (ui_button(context, "Refresh", GRD_UI_BUTTON_SECONDARY)) {
        grd_discovery_refresh(app->discovery);
        set_status(app, "LAN device search refreshed");
    }
    nk_layout_row_end(context);

    nk_layout_row_dynamic(context, 350.0F, 1);
    const struct nk_vec2 saved_group_padding =
        context->style.window.group_padding;
    context->style.window.group_padding = nk_vec2(0.0F, 2.0F);
    if (nk_group_begin(context, "Discovered devices", 0)) {
        if (app->peer_count == 0U) {
            nk_layout_row_dynamic(context, 34.0F, 1);
            ui_label(
                context,
                app->small_font,
                "No computers found",
                NK_TEXT_CENTERED,
                muted
            );
            nk_layout_row_dynamic(context, 42.0F, 1);
            ui_label_wrap(
                context,
                app->small_font,
                "Open GRD on the other device. LAN discovery is automatic.",
                meta
            );
        }
        for (size_t index = 0U; index < app->peer_count; ++index) {
            const grd_discovered_peer *peer = &app->peers[index];
            char label[260];
            (void)snprintf(
                label,
                sizeof(label),
                "%s   %s:%u",
                peer->name,
                peer->address,
                peer->port
            );
            const bool terminal_available =
                (peer->capabilities &
                 (GRD_DISCOVERY_CAP_SSH_TERMINAL |
                  GRD_DISCOVERY_CAP_POWERSHELL)) != 0U &&
                peer->ssh_port != 0U &&
                grd_remote_access_client_available(
                    GRD_REMOTE_ACCESS_TERMINAL
                );
            const bool sftp_available =
                (peer->capabilities & GRD_DISCOVERY_CAP_SFTP) != 0U &&
                peer->ssh_port != 0U &&
                grd_remote_access_client_available(GRD_REMOTE_ACCESS_SFTP);
            const unsigned action_count =
                (terminal_available ? 1U : 0U) +
                (sftp_available ? 1U : 0U);
            if (action_count == 0U) {
                nk_layout_row_dynamic(context, 48.0F, 1);
            } else {
                nk_layout_row_begin(
                    context, NK_DYNAMIC, 48.0F, (int)action_count + 1
                );
                nk_layout_row_push(
                    context, action_count == 2U ? 0.52F : 0.70F
                );
            }
            if (ui_button(context, label, GRD_UI_BUTTON_SECONDARY)) {
                app->lan_join_peer = *peer;
                app->home_settings_visible = false;
                app->remote_access_modal_visible = false;
                app->lan_join_modal_visible = true;
                app->lan_join_error[0] = '\0';
                memset(
                    app->connect_password,
                    0,
                    sizeof(app->connect_password)
                );
            }
            if (terminal_available) {
                nk_layout_row_push(
                    context, action_count == 2U ? 0.22F : 0.30F
                );
                const char *terminal_label =
                    peer->operating_system == GRD_OS_WINDOWS &&
                            (peer->capabilities &
                             GRD_DISCOVERY_CAP_POWERSHELL) != 0U
                        ? "PowerShell"
                        : "Terminal";
                if (ui_button(
                        context,
                        terminal_label,
                        GRD_UI_BUTTON_PRIMARY
                    )) {
                    open_remote_access_modal(
                        app, peer, GRD_REMOTE_ACCESS_TERMINAL
                    );
                }
            }
            if (sftp_available) {
                nk_layout_row_push(
                    context, action_count == 2U ? 0.26F : 0.30F
                );
                if (ui_button(
                        context, "File SFTP", GRD_UI_BUTTON_PRIMARY
                    )) {
                    open_remote_access_modal(
                        app, peer, GRD_REMOTE_ACCESS_SFTP
                    );
                }
            }
            if (action_count != 0U) {
                nk_layout_row_end(context);
            }
        }
        nk_group_end(context);
    }
    context->style.window.group_padding = saved_group_padding;

}

static void draw_host_advanced_controls(
    grd_app *app,
    struct nk_context *context
)
{
    const struct nk_color text = nk_rgb(29, 29, 31);
    const struct nk_color muted = nk_rgb(110, 110, 115);
    bool auto_save_needed = false;
    const bool host_active = app->host != NULL;

    if (host_active) {
        nk_layout_row_dynamic(context, 34.0F, 1);
        ui_label_wrap(
            context,
            app->small_font,
            "Stop sharing to change host settings.",
            muted
        );
        nk_widget_disable_begin(context);
    }

    nk_layout_row_begin(context, NK_DYNAMIC, 18.0F, 2);
    nk_layout_row_push(context, 0.75F);
    ui_label(
        context, app->small_font, "Streaming profile", NK_TEXT_LEFT, text
    );
    nk_layout_row_push(context, 0.25F);
    ui_label(context, app->small_font, "Codec", NK_TEXT_LEFT, text);
    nk_layout_row_end(context);
    nk_layout_row_dynamic(context, 34.0F, 4);
    if (ui_radio_option(
            context,
            app->small_font,
            "Balanced",
            app->config.stream_profile == GRD_STREAM_BALANCED
        )) {
        apply_stream_profile(app, GRD_STREAM_BALANCED);
        auto_save_needed = true;
    }
    if (ui_radio_option(
            context,
            app->small_font,
            "Gaming",
            app->config.stream_profile == GRD_STREAM_GAMING
        )) {
        apply_stream_profile(app, GRD_STREAM_GAMING);
        auto_save_needed = true;
    }
    if (ui_radio_option(
            context,
            app->small_font,
            "Desktop",
            app->config.stream_profile == GRD_STREAM_DESKTOP
        )) {
        apply_stream_profile(app, GRD_STREAM_DESKTOP);
        auto_save_needed = true;
    }
    static const char *codec_names[] = {"H.264", "HEVC", "AV1"};
    unsigned codec_index = app->config.video_codec <= GRD_CODEC_AV1
                               ? (unsigned)app->config.video_codec
                               : 0U;
    if (nk_combo_begin_label(
            context, codec_names[codec_index], nk_vec2(160.0F, 120.0F)
        )) {
        nk_layout_row_dynamic(context, 30.0F, 1);
        for (unsigned index = 0U; index < 3U; ++index) {
            if (nk_combo_item_label(
                    context, codec_names[index], NK_TEXT_LEFT
                )) {
                app->config.video_codec = (grd_video_codec)index;
                auto_save_needed = true;
            }
        }
        nk_combo_end(context);
    }

    nk_layout_row_dynamic(context, 18.0F, 1);
    ui_label(
        context,
        app->small_font,
        "Host FPS limit",
        NK_TEXT_LEFT,
        text
    );
    static const uint32_t host_rates[] = {30U, 60U, 90U, 120U};
    uint32_t selected_host_rate = app->config.target_fps;
    if (ui_rate_selector(
            context,
            app->small_font,
            app->config.target_fps,
            host_rates,
            sizeof(host_rates) / sizeof(host_rates[0]),
            "FPS",
            &selected_host_rate
        )) {
        app->config.target_fps = selected_host_rate;
        auto_save_needed = true;
        set_status(app, "Host FPS limit updated");
    }

    int initial_mbps = (int)(app->config.initial_bitrate_kbps / 1000U);
    int target_mbps = (int)(app->config.target_bitrate_kbps / 1000U);
    int minimum_mbps = (int)(app->config.min_bitrate_kbps / 1000U);
    nk_layout_row_dynamic(context, 18.0F, 1);
    ui_label(
        context,
        app->small_font,
        "Adaptive network budget (Mbps)",
        NK_TEXT_LEFT,
        text
    );
    nk_layout_row_dynamic(context, 50.0F, 3);
    const bool initial_changed = ui_number_stepper(
        context, app->small_font, "Start", &initial_mbps, 4, 100
    );
    const bool target_changed = ui_number_stepper(
        context, app->small_font, "Maximum", &target_mbps, 4, 100
    );
    const bool minimum_changed = ui_number_stepper(
        context, app->small_font, "Minimum", &minimum_mbps, 4, 100
    );
    if (minimum_mbps > target_mbps) {
        minimum_mbps = target_mbps;
    }
    if (initial_mbps < minimum_mbps) {
        initial_mbps = minimum_mbps;
    }
    if (initial_mbps > target_mbps) {
        initial_mbps = target_mbps;
    }
    app->config.initial_bitrate_kbps = (uint32_t)initial_mbps * 1000U;
    app->config.target_bitrate_kbps = (uint32_t)target_mbps * 1000U;
    app->config.min_bitrate_kbps = (uint32_t)minimum_mbps * 1000U;
    auto_save_needed = auto_save_needed || initial_changed ||
                       target_changed || minimum_changed;

    nk_bool abr = app->config.abr_enabled ? nk_true : nk_false;
    nk_layout_row_begin(context, NK_DYNAMIC, 42.0F, 2);
    nk_layout_row_push(context, 0.30F);
    if (nk_checkbox_label(context, "Adaptive bitrate", &abr)) {
        app->config.abr_enabled = abr != nk_false;
        auto_save_needed = true;
    }
    nk_layout_row_push(context, 0.70F);
    ui_label(
        context,
        app->small_font,
        "Stable LAN: quality rises | local pressure: bitrate falls",
        NK_TEXT_RIGHT,
        muted
    );
    nk_layout_row_end(context);

#if !defined(_WIN32)
    nk_layout_row_dynamic(context, 16.0F, 1);
    nk_spacing(context, 1);
    nk_layout_row_dynamic(context, 20.0F, 1);
    ui_label(
        context,
        app->small_font,
        "Terminal and file transfer (OpenSSH)",
        NK_TEXT_LEFT,
        text
    );
    nk_bool ssh_enabled =
        app->config.ssh_remote_access_enabled ? nk_true : nk_false;
    nk_layout_row_begin(context, NK_DYNAMIC, 46.0F, 3);
    nk_layout_row_push(context, 0.44F);
    if (nk_checkbox_label(
            context, "Advertise SSH + SFTP on the LAN", &ssh_enabled
        )) {
        app->config.ssh_remote_access_enabled = ssh_enabled != nk_false;
        if (!app->config.ssh_remote_access_enabled) {
            app->ssh_remote_access_ready = false;
        }
        auto_save_needed = true;
    }
    int ssh_port = (int)app->config.ssh_remote_access_port;
    nk_layout_row_push(context, 0.28F);
    if (ui_number_stepper(
            context, app->small_font, "SSH port", &ssh_port, 1, 65535
        )) {
        app->config.ssh_remote_access_port = (uint16_t)ssh_port;
        app->ssh_remote_access_ready = false;
        auto_save_needed = true;
    }
    nk_layout_row_push(context, 0.28F);
    if (ui_button(context, "Verify service", GRD_UI_BUTTON_SECONDARY)) {
        grd_error ssh_error = {0};
        if (grd_remote_access_probe_local_ssh(
                app->config.ssh_remote_access_port, 700U, &ssh_error
            ) == GRD_OK) {
            app->ssh_remote_access_ready = true;
            set_status(app, "OpenSSH ready: terminal and SFTP available");
        } else {
            app->ssh_remote_access_ready = false;
            publish_error(app, &ssh_error);
            set_status(app, ssh_error.message);
        }
    }
    nk_layout_row_end(context);
    nk_layout_row_dynamic(context, 38.0F, 1);
    ui_label_wrap(
        context,
        app->small_font,
#if defined(__APPLE__)
        "Requires Remote Login in System Settings > General > Sharing. "
        "GRD does not modify the service or store SSH passwords.",
#else
        "Requires an active openssh-server/sshd. GRD does not modify the "
        "service or store SSH passwords.",
#endif
        muted
    );
#endif

    if (host_active) {
        nk_widget_disable_end(context);
    }
    if (auto_save_needed) {
        (void)save_config_automatically(app, NULL);
    }
}

static void draw_host_card(grd_app *app, struct nk_context *context)
{
    const struct nk_color text = nk_rgb(29, 29, 31);
    const struct nk_color muted = nk_rgb(110, 110, 115);
    const struct nk_color success = nk_rgb(22, 163, 74);
    const bool host_active = app->host != NULL;
    bool auto_save_needed = false;

    nk_layout_row_begin(context, NK_DYNAMIC, 32.0F, 2);
    nk_layout_row_push(context, 0.70F);
    ui_label(
        context, app->heading_font, "Share this computer", NK_TEXT_LEFT, text
    );
    nk_layout_row_push(context, 0.30F);
    ui_label(
        context,
        app->small_font,
        host_active ? "Sharing active" : "Sharing inactive",
        NK_TEXT_RIGHT,
        host_active ? success : muted
    );
    nk_layout_row_end(context);
    nk_layout_row_dynamic(context, 38.0F, 1);
    ui_label_wrap(
        context,
        app->small_font,
        "Make this desktop available to authorized devices on the local network.",
        muted
    );

    if (host_active) {
        nk_widget_disable_begin(context);
    }
    nk_layout_row_dynamic(context, 18.0F, 2);
    ui_label(
        context, app->small_font, "Device name", NK_TEXT_LEFT, text
    );
    ui_label(
        context, app->small_font, "Display to share", NK_TEXT_LEFT, text
    );
    nk_layout_row_dynamic(context, 44.0F, 2);
    const nk_flags device_name_state = nk_edit_string_zero_terminated(
        context,
        NK_EDIT_FIELD | NK_EDIT_SIG_ENTER,
        app->config.device_name,
        sizeof(app->config.device_name),
        nk_filter_default
    );
    if (app->text_cursor != NULL && nk_widget_is_hovered(context)) {
        (void)SDL_SetCursor(app->text_cursor);
    }
    if ((device_name_state &
         (NK_EDIT_DEACTIVATED | NK_EDIT_COMMITTED)) != 0U) {
        auto_save_needed = true;
    }
    if (app->monitor_count != 0U) {
        const char *selected = app->monitors[app->selected_monitor].name;
        if (nk_combo_begin_label(
                context, selected, nk_vec2(430.0F, 220.0F)
            )) {
            nk_layout_row_dynamic(context, 36.0F, 1);
            for (size_t index = 0U; index < app->monitor_count; ++index) {
                if (nk_combo_item_label(
                        context, app->monitors[index].name, NK_TEXT_LEFT
                    )) {
                    app->selected_monitor = index;
                }
            }
            nk_combo_end(context);
        }
    } else {
        nk_label(context, "No display available", NK_TEXT_LEFT);
    }

    nk_layout_row_dynamic(context, 18.0F, 1);
    ui_label(
        context,
        app->small_font,
        app->config.password_configured
            ? "Host password - leave blank to keep the saved password"
            : "Host password - at least 12 characters",
        NK_TEXT_LEFT,
        text
    );
    nk_layout_row_dynamic(context, 44.0F, 1);
    const nk_flags host_password_state = password_edit(
        app,
        context,
        app->new_host_password,
        (int)sizeof(app->new_host_password)
    );
    if ((host_password_state &
         (NK_EDIT_DEACTIVATED | NK_EDIT_COMMITTED)) != 0U &&
        app->new_host_password[0] != '\0') {
        (void)save_host_settings(app, "Host password updated");
    }

    if (auto_save_needed) {
        (void)save_config_automatically(app, NULL);
    }
    if (host_active) {
        nk_widget_disable_end(context);
    }

    nk_layout_row_dynamic(context, 48.0F, 1);
    if (!host_active) {
        if (ui_button(
                context, "Join the LAN", GRD_UI_BUTTON_DARK
            ) && save_host_settings(app, NULL)) {
            start_host(app);
        }
    } else if (ui_button(
                   context, "Leave the LAN", GRD_UI_BUTTON_DANGER
               )) {
        stop_host(app);
        set_status(app, "Sharing stopped");
    }
}

static void draw_home(grd_app *app, struct nk_context *context)
{
    if (app->default_cursor != NULL) {
        (void)SDL_SetCursor(app->default_cursor);
    }

    nk_layout_row_begin(context, NK_DYNAMIC, 42.0F, 2);
    nk_layout_row_push(context, 0.82F);
    nk_spacing(context, 1);
    nk_layout_row_push(context, 0.18F);
    if (ui_button(context, "Settings", GRD_UI_BUTTON_SECONDARY)) {
        app->lan_join_modal_visible = false;
        app->remote_access_modal_visible = false;
        app->home_settings_visible = true;
    }
    nk_layout_row_end(context);

    grd_ui_card_style card_style;
    nk_layout_row_dynamic(context, 560.0F, 2);
    ui_card_style_begin(context, &card_style);
    if (nk_group_begin(
            context,
            "Connection",
            NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR
        )) {
        draw_connection_card(app, context);
        nk_group_end(context);
    }
    ui_card_style_end(context, &card_style);

    ui_card_style_begin(context, &card_style);
    if (nk_group_begin(
            context,
            "Sharing",
            NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR
        )) {
        draw_host_card(app, context);
        nk_group_end(context);
    }
    ui_card_style_end(context, &card_style);
}

static void close_lan_join_modal(grd_app *app)
{
    app->lan_join_modal_visible = false;
    app->lan_join_error[0] = '\0';
    memset(app->connect_password, 0, sizeof(app->connect_password));
    if (app->default_cursor != NULL) {
        (void)SDL_SetCursor(app->default_cursor);
    }
}

static void close_remote_access_modal(grd_app *app)
{
    app->remote_access_modal_visible = false;
    app->remote_access_error[0] = '\0';
    if (app->default_cursor != NULL) {
        (void)SDL_SetCursor(app->default_cursor);
    }
}

static void draw_remote_access_modal(
    grd_app *app,
    struct nk_context *context,
    int window_width,
    int window_height
)
{
    const float panel_width = window_width < 590
                                  ? (float)window_width - 24.0F
                                  : 550.0F;
    const float panel_height = window_height < 480
                                   ? (float)window_height - 24.0F
                                   : 440.0F;
    const float panel_x = ((float)window_width - panel_width) * 0.5F;
    const float panel_y = ((float)window_height - panel_height) * 0.5F;
    const struct nk_style_item saved_background =
        context->style.window.fixed_background;
    const struct nk_color saved_border = context->style.window.border_color;
    const struct nk_vec2 saved_padding = context->style.window.padding;
    const float saved_rounding = context->style.window.rounding;
    const float saved_window_border = context->style.window.border;
    const struct nk_color text = nk_rgb(29, 29, 31);
    const struct nk_color muted = nk_rgb(110, 110, 115);
    context->style.window.fixed_background =
        nk_style_item_color(nk_rgb(255, 255, 255));
    context->style.window.border_color = nk_rgb(210, 210, 215);
    context->style.window.padding = nk_vec2(26.0F, 24.0F);
    context->style.window.rounding = 18.0F;
    context->style.window.border = 1.0F;

    if (nk_begin(
            context,
            "OpenSSH access",
            nk_rect(panel_x, panel_y, panel_width, panel_height),
            NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR
        )) {
        const bool sftp =
            app->remote_access_kind == GRD_REMOTE_ACCESS_SFTP;
        const bool powershell =
            !sftp &&
            app->remote_access_peer.operating_system == GRD_OS_WINDOWS &&
            (app->remote_access_peer.capabilities &
             GRD_DISCOVERY_CAP_POWERSHELL) != 0U;
        nk_layout_row_dynamic(context, 34.0F, 1);
        ui_label(
            context,
            app->heading_font,
            sftp ? "Files over SFTP"
                 : powershell ? "Remote PowerShell" : "SSH terminal",
            NK_TEXT_LEFT,
            text
        );
        nk_layout_row_dynamic(context, 50.0F, 1);
        ui_label_wrap(
            context,
            app->small_font,
            sftp
                ? "Opens the system SFTP client to transfer files without "
                  "passing credentials through GRD."
                : "Opens a session in the system terminal. GRD does not "
                  "read or retain the remote account credentials.",
            muted
        );
        char target_label[300];
        (void)snprintf(
            target_label,
            sizeof(target_label),
            "%s   %s:%u",
            app->remote_access_peer.name,
            app->remote_access_peer.address,
            (unsigned)app->remote_access_peer.ssh_port
        );
        nk_layout_row_dynamic(context, 34.0F, 1);
        ui_label(
            context,
            app->small_font,
            target_label,
            NK_TEXT_CENTERED,
            nk_rgb(66, 66, 69)
        );
        nk_layout_row_dynamic(context, 20.0F, 1);
        ui_label(
            context,
            app->small_font,
            "Remote system username",
            NK_TEXT_LEFT,
            text
        );
        nk_layout_row_dynamic(context, 44.0F, 1);
        const nk_flags username_state = nk_edit_string_zero_terminated(
            context,
            NK_EDIT_FIELD | NK_EDIT_SIG_ENTER,
            app->remote_access_username,
            sizeof(app->remote_access_username),
            nk_filter_default
        );
        if (app->text_cursor != NULL && nk_widget_is_hovered(context)) {
            (void)SDL_SetCursor(app->text_cursor);
        }
        nk_layout_row_dynamic(context, 42.0F, 1);
        ui_label_wrap(
            context,
            app->small_font,
            "Host-key verification and any password prompt are handled by "
            "the system OpenSSH client.",
            muted
        );
        nk_layout_row_dynamic(context, 22.0F, 1);
        ui_label(
            context,
            app->small_font,
            app->remote_access_error,
            NK_TEXT_LEFT,
            nk_rgb(220, 38, 38)
        );
        bool launch_requested =
            (username_state & NK_EDIT_COMMITTED) != 0U;
        nk_layout_row_dynamic(context, 46.0F, 2);
        if (ui_button(context, "Cancel", GRD_UI_BUTTON_SECONDARY)) {
            close_remote_access_modal(app);
        }
        if (ui_button(
                context,
                sftp ? "Open SFTP" : powershell ? "Open PowerShell"
                                               : "Open terminal",
                GRD_UI_BUTTON_DARK
            )) {
            launch_requested = true;
        }
        if (app->remote_access_modal_visible && launch_requested) {
            grd_error error = {0};
            if (!grd_remote_access_username_valid(
                    app->remote_access_username
                )) {
                (void)snprintf(
                    app->remote_access_error,
                    sizeof(app->remote_access_error),
                    "Invalid username. Use letters, numbers, '.', '_' or '-'."
                );
            } else if (grd_remote_access_launch(
                           app->remote_access_kind,
                           app->remote_access_peer.address,
                           app->remote_access_peer.ssh_port,
                           app->remote_access_username,
                           &error
                       ) != GRD_OK) {
                publish_error(app, &error);
                (void)snprintf(
                    app->remote_access_error,
                    sizeof(app->remote_access_error),
                    "%s",
                    error.message[0] != '\0'
                        ? error.message
                        : "Unable to start OpenSSH"
                );
            } else {
                (void)snprintf(
                    app->config.remote_access_username,
                    sizeof(app->config.remote_access_username),
                    "%s",
                    app->remote_access_username
                );
                (void)save_config_automatically(app, NULL);
                set_status(
                    app,
                    sftp ? "SFTP client started in the system terminal"
                         : "SSH terminal started"
                );
                close_remote_access_modal(app);
            }
        }
    }
    nk_end(context);
    context->style.window.fixed_background = saved_background;
    context->style.window.border_color = saved_border;
    context->style.window.padding = saved_padding;
    context->style.window.rounding = saved_rounding;
    context->style.window.border = saved_window_border;
}

static void draw_lan_join_modal(
    grd_app *app,
    struct nk_context *context,
    int window_width,
    int window_height
)
{
    const float panel_width = window_width < 560
                                  ? (float)window_width - 24.0F
                                  : 520.0F;
    const float panel_height = window_height < 450
                                   ? (float)window_height - 24.0F
                                   : 420.0F;
    const float panel_x = ((float)window_width - panel_width) * 0.5F;
    const float panel_y = ((float)window_height - panel_height) * 0.5F;
    const struct nk_style_item saved_background =
        context->style.window.fixed_background;
    const struct nk_color saved_border = context->style.window.border_color;
    const struct nk_vec2 saved_padding = context->style.window.padding;
    const float saved_rounding = context->style.window.rounding;
    const float saved_window_border = context->style.window.border;
    context->style.window.fixed_background =
        nk_style_item_color(nk_rgb(255, 255, 255));
    context->style.window.border_color = nk_rgb(210, 210, 215);
    context->style.window.padding = nk_vec2(26.0F, 24.0F);
    context->style.window.rounding = 18.0F;
    context->style.window.border = 1.0F;

    if (nk_begin(
            context,
            "LAN connection",
            nk_rect(panel_x, panel_y, panel_width, panel_height),
            NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR
        )) {
        nk_layout_row_dynamic(context, 34.0F, 1);
        ui_label(
            context,
            app->heading_font,
            "Join the LAN",
            NK_TEXT_LEFT,
            nk_rgb(29, 29, 31)
        );
        nk_layout_row_dynamic(context, 26.0F, 1);
        ui_label(
            context,
            app->small_font,
            "Enter the password to connect to the selected device.",
            NK_TEXT_LEFT,
            nk_rgb(110, 110, 115)
        );

        char peer_label[260];
        (void)snprintf(
            peer_label,
            sizeof(peer_label),
            "%s   %s:%u",
            app->lan_join_peer.name,
            app->lan_join_peer.address,
            app->lan_join_peer.port
        );
        nk_layout_row_dynamic(context, 48.0F, 1);
        ui_label(
            context,
            app->small_font,
            peer_label,
            NK_TEXT_CENTERED,
            nk_rgb(66, 66, 69)
        );

        nk_layout_row_dynamic(context, 20.0F, 1);
        ui_label(
            context,
            app->small_font,
            "Remote computer password",
            NK_TEXT_LEFT,
            nk_rgb(29, 29, 31)
        );
        nk_layout_row_dynamic(context, 44.0F, 1);
        const nk_flags password_state = password_edit(
            app,
            context,
            app->connect_password,
            (int)sizeof(app->connect_password)
        );

        nk_layout_row_dynamic(context, 20.0F, 1);
        ui_label(
            context,
            app->small_font,
            "Connection mode",
            NK_TEXT_LEFT,
            nk_rgb(29, 29, 31)
        );
        nk_layout_row_dynamic(context, 38.0F, 2);
        if (ui_radio_option(
                context,
                app->small_font,
                "Control",
                app->request_controller
            )) {
            app->request_controller = true;
        }
        if (ui_radio_option(
                context,
                app->small_font,
                "View only",
                !app->request_controller
            )) {
            app->request_controller = false;
        }

        nk_layout_row_dynamic(context, 24.0F, 1);
        ui_label(
            context,
            app->small_font,
            app->lan_join_error,
            NK_TEXT_LEFT,
            nk_rgb(220, 38, 38)
        );

        bool join_requested =
            (password_state & NK_EDIT_COMMITTED) != 0U;
        nk_layout_row_dynamic(context, 46.0F, 2);
        if (ui_button(context, "Cancel", GRD_UI_BUTTON_SECONDARY)) {
            close_lan_join_modal(app);
        }
        if (ui_button(context, "Connect", GRD_UI_BUTTON_DARK)) {
            join_requested = true;
        }
        if (app->lan_join_modal_visible && join_requested) {
            if (app->connect_password[0] == '\0') {
                (void)snprintf(
                    app->lan_join_error,
                    sizeof(app->lan_join_error),
                    "Enter the remote computer password."
                );
            } else {
                const bool connected = connect_to(
                    app,
                    app->lan_join_peer.address,
                    app->lan_join_peer.port
                );
                if (connected && app->mode == GRD_APP_REMOTE &&
                    app->connection != NULL &&
                    app->media_connection != NULL) {
                    close_lan_join_modal(app);
                } else {
                    char status[sizeof(app->status)];
                    SDL_LockMutex(app->error_mutex);
                    (void)snprintf(
                        status, sizeof(status), "%s", app->status
                    );
                    SDL_UnlockMutex(app->error_mutex);
                    const bool actionable_error =
                        strstr(status, "Authentication") != NULL ||
                        strstr(status, "Control") != NULL;
                    (void)snprintf(
                        app->lan_join_error,
                        sizeof(app->lan_join_error),
                        "%s",
                        actionable_error
                            ? status
                            : "Not available"
                    );
                }
            }
        }
    }
    nk_end(context);
    context->style.window.fixed_background = saved_background;
    context->style.window.border_color = saved_border;
    context->style.window.padding = saved_padding;
    context->style.window.rounding = saved_rounding;
    context->style.window.border = saved_window_border;
}

static void draw_modal_backdrop(
    struct nk_context *context,
    int window_width,
    int window_height
)
{
    const struct nk_style_item saved_background =
        context->style.window.fixed_background;
    const struct nk_color saved_border = context->style.window.border_color;
    const struct nk_vec2 saved_padding = context->style.window.padding;
    const float saved_window_border = context->style.window.border;
    context->style.window.fixed_background =
        nk_style_item_color(nk_rgba(0, 0, 0, 72));
    context->style.window.border_color = nk_rgba(0, 0, 0, 0);
    context->style.window.padding = nk_vec2(0.0F, 0.0F);
    context->style.window.border = 0.0F;
    (void)nk_begin(
        context,
        "Modal backdrop",
        nk_rect(0.0F, 0.0F, (float)window_width, (float)window_height),
        NK_WINDOW_NO_SCROLLBAR
    );
    nk_end(context);
    context->style.window.fixed_background = saved_background;
    context->style.window.border_color = saved_border;
    context->style.window.padding = saved_padding;
    context->style.window.border = saved_window_border;
}

static void draw_home_settings_modal(
    grd_app *app,
    struct nk_context *context,
    int window_width,
    int window_height
)
{
    const float panel_width = window_width < 820
                                  ? (float)window_width - 24.0F
                                  : 760.0F;
    const float panel_height = window_height < 834
                                   ? (float)window_height - 24.0F
                                   : 810.0F;
    const float panel_x = ((float)window_width - panel_width) * 0.5F;
    const float panel_y = ((float)window_height - panel_height) * 0.5F;
    const struct nk_style_item saved_background =
        context->style.window.fixed_background;
    const struct nk_color saved_border = context->style.window.border_color;
    const struct nk_vec2 saved_padding = context->style.window.padding;
    const float saved_rounding = context->style.window.rounding;
    const float saved_window_border = context->style.window.border;
    const struct nk_color text = nk_rgb(29, 29, 31);
    const struct nk_color muted = nk_rgb(110, 110, 115);
    context->style.window.fixed_background =
        nk_style_item_color(nk_rgb(255, 255, 255));
    context->style.window.border_color = nk_rgb(210, 210, 215);
    context->style.window.padding = nk_vec2(26.0F, 24.0F);
    context->style.window.rounding = 18.0F;
    context->style.window.border = 1.0F;

    if (nk_begin(
            context,
            "Advanced settings",
            nk_rect(panel_x, panel_y, panel_width, panel_height),
            NK_WINDOW_BORDER
        )) {
        nk_layout_row_begin(context, NK_DYNAMIC, 38.0F, 2);
        nk_layout_row_push(context, 0.78F);
        ui_label(
            context,
            app->heading_font,
            "Settings",
            NK_TEXT_LEFT,
            text
        );
        nk_layout_row_push(context, 0.22F);
        if (ui_button(context, "Close", GRD_UI_BUTTON_SECONDARY)) {
            app->home_settings_visible = false;
        }
        nk_layout_row_end(context);

        nk_layout_row_dynamic(context, 30.0F, 1);
        ui_label(
            context,
            app->small_font,
            "Configure the client device and shared host separately.",
            NK_TEXT_LEFT,
            muted
        );

        nk_layout_row_dynamic(context, 42.0F, 2);
        if (ui_button(
                context,
                "Client",
                app->home_settings_host_tab
                    ? GRD_UI_BUTTON_SECONDARY
                    : GRD_UI_BUTTON_PRIMARY
            )) {
            app->home_settings_host_tab = false;
        }
        if (ui_button(
                context,
                "Host",
                app->home_settings_host_tab
                    ? GRD_UI_BUTTON_PRIMARY
                    : GRD_UI_BUTTON_SECONDARY
            )) {
            app->home_settings_host_tab = true;
        }

        nk_layout_row_dynamic(context, 16.0F, 1);
        nk_spacing(context, 1);
        if (app->home_settings_host_tab) {
            draw_host_advanced_controls(app, context);
        } else {
            draw_client_rate_controls(app, context, false, false);
        }
    }
    nk_end(context);
    context->style.window.fixed_background = saved_background;
    context->style.window.border_color = saved_border;
    context->style.window.padding = saved_padding;
    context->style.window.rounding = saved_rounding;
    context->style.window.border = saved_window_border;
}

static float snap_to_physical_pixel(float value, float display_scale)
{
    if (display_scale <= 0.0F) {
        return value;
    }
    const float physical = value * display_scale;
    return (float)((uint32_t)(physical + 0.5F)) / display_scale;
}

static void draw_remote(grd_app *app, struct nk_context *context)
{
    struct nk_rect bounds;
    const float available_height = nk_window_get_content_region_size(context).y;
    nk_layout_row_dynamic(
        context,
        available_height > 1.0F ? available_height : 1.0F,
        1
    );
    if (nk_widget(&bounds, context) && app->remote_texture != NULL) {
        /* Keep the source aspect ratio: the remote frame is drawn into the
         * largest centered sub-rectangle instead of being stretched to the
         * widget, and the cursor is mapped inside the same rect. */
        float texture_width = 0.0F;
        float texture_height = 0.0F;
        (void)SDL_GetTextureSize(
            app->remote_texture, &texture_width, &texture_height
        );
        struct nk_rect view = bounds;
        if (texture_width > 0.0F && texture_height > 0.0F &&
            bounds.w > 0.0F && bounds.h > 0.0F) {
            const float source_ratio = texture_width / texture_height;
            const float target_ratio = bounds.w / bounds.h;
            if (target_ratio > source_ratio) {
                /* Window wider than the frame: pillarbox horizontally. */
                const float width = bounds.h * source_ratio;
                view.x = bounds.x + (bounds.w - width) * 0.5F;
                view.w = width;
            } else {
                /* Window taller than the frame: letterbox vertically. */
                const float height = bounds.w / source_ratio;
                view.y = bounds.y + (bounds.h - height) * 0.5F;
                view.h = height;
            }
            /* Nuklear lays out in logical points while Metal samples the
             * final drawable in physical pixels. Half-pixel video edges
             * force bilinear blending over the whole scaled image and make
             * an otherwise native 1080p frame look soft on Retina. Snap
             * both edges to the physical grid before submitting the quad. */
            const float display_scale = app->window != NULL
                                            ? SDL_GetWindowDisplayScale(
                                                  app->window
                                              )
                                            : 1.0F;
            const float right = snap_to_physical_pixel(
                view.x + view.w, display_scale
            );
            const float bottom = snap_to_physical_pixel(
                view.y + view.h, display_scale
            );
            view.x = snap_to_physical_pixel(view.x, display_scale);
            view.y = snap_to_physical_pixel(view.y, display_scale);
            view.w = right > view.x ? right - view.x : 1.0F;
            view.h = bottom > view.y ? bottom - view.y : 1.0F;
        }
        const struct nk_image image = nk_image_ptr(app->remote_texture);
        nk_draw_image(
            nk_window_get_canvas(context),
            view,
            &image,
            nk_rgb(255, 255, 255)
        );
        bounds = view;
        app->view_rect = bounds;
        bool cursor_visible;
        float cursor_x;
        float cursor_y;
        bool predicted_cursor_valid;
        float predicted_cursor_x;
        float predicted_cursor_y;
        uint16_t cursor_width;
        uint16_t cursor_height;
        int16_t cursor_hotspot_x;
        int16_t cursor_hotspot_y;
        SDL_Texture *cursor_texture;
        SDL_LockMutex(app->cursor_mutex);
        cursor_visible = app->remote_cursor_visible;
        cursor_x = app->remote_cursor_x;
        cursor_y = app->remote_cursor_y;
        predicted_cursor_valid = app->predicted_cursor_valid;
        predicted_cursor_x = app->predicted_cursor_x;
        predicted_cursor_y = app->predicted_cursor_y;
        cursor_width = app->remote_cursor_shape.width;
        cursor_height = app->remote_cursor_shape.height;
        cursor_hotspot_x = app->remote_cursor_shape.hotspot_x;
        cursor_hotspot_y = app->remote_cursor_shape.hotspot_y;
        cursor_texture = app->remote_cursor_texture;
        SDL_UnlockMutex(app->cursor_mutex);
        if (app->config.client_cursor_prediction &&
            !app->relative_mouse_mode && predicted_cursor_valid) {
            cursor_x = predicted_cursor_x;
            cursor_y = predicted_cursor_y;
        }
        if (cursor_visible) {
            struct nk_command_buffer *canvas = nk_window_get_canvas(context);
            const float x = bounds.x + cursor_x * bounds.w;
            const float y = bounds.y + cursor_y * bounds.h;
            if (cursor_texture != NULL && cursor_width != 0U &&
                cursor_height != 0U) {
                float frame_width = 0.0F;
                float frame_height = 0.0F;
                (void)SDL_GetTextureSize(
                    app->remote_texture, &frame_width, &frame_height
                );
                const float scale_x = frame_width > 0.0F
                                          ? bounds.w / frame_width
                                          : 1.0F;
                const float scale_y = frame_height > 0.0F
                                          ? bounds.h / frame_height
                                          : 1.0F;
                const struct nk_rect cursor_bounds = nk_rect(
                    x - (float)cursor_hotspot_x * scale_x,
                    y - (float)cursor_hotspot_y * scale_y,
                    (float)cursor_width * scale_x,
                    (float)cursor_height * scale_y
                );
                const struct nk_image cursor_image =
                    nk_image_ptr(cursor_texture);
                nk_draw_image(
                    canvas,
                    cursor_bounds,
                    &cursor_image,
                    nk_rgb(255, 255, 255)
                );
            } else {
                /* A recognizable local fallback for hosts that expose only
                 * cursor position and no cursor bitmap (common in games).
                 * The previous three-line glyph looked like a small "L". */
                const float cursor_points[] = {
                    x, y,
                    x, y + 25.0F,
                    x + 6.5F, y + 19.0F,
                    x + 12.0F, y + 28.0F,
                    x + 17.0F, y + 25.0F,
                    x + 11.5F, y + 16.5F,
                    x + 21.0F, y + 16.5F
                };
                nk_fill_polygon(
                    canvas, cursor_points, 7, nk_rgb(250, 252, 255)
                );
                nk_stroke_polygon(
                    canvas, cursor_points, 7, 2.0F, nk_rgb(7, 10, 16)
                );
            }
        }
    } else {
        nk_label(context, "Waiting for the first frame...", NK_TEXT_CENTERED);
        app->view_rect = bounds;
    }
}

static void draw_remote_settings(
    grd_app *app,
    struct nk_context *context,
    int window_width,
    int window_height
)
{
    const float panel_width = window_width < 560 ? (float)window_width - 24.0F
                                                  : 520.0F;
    const float panel_height = window_height < 934 ? (float)window_height - 24.0F
                                                    : 910.0F;
    const float panel_x = ((float)window_width - panel_width) * 0.5F;
    const float panel_y = ((float)window_height - panel_height) * 0.5F;
    const struct nk_style_item saved_background =
        context->style.window.fixed_background;
    const struct nk_color saved_border = context->style.window.border_color;
    const struct nk_vec2 saved_padding = context->style.window.padding;
    const float saved_rounding = context->style.window.rounding;
    const float saved_window_border = context->style.window.border;
    context->style.window.fixed_background =
        nk_style_item_color(nk_rgb(255, 255, 255));
    context->style.window.border_color = nk_rgb(210, 210, 215);
    context->style.window.padding = nk_vec2(24.0F, 22.0F);
    context->style.window.rounding = 18.0F;
    context->style.window.border = 1.0F;
    if (nk_begin(
            context,
            "Session settings",
            nk_rect(panel_x, panel_y, panel_width, panel_height),
            NK_WINDOW_BORDER
        )) {
        char pipeline[256];
        const uint32_t stream_fps = atomic_load_explicit(
            &app->remote_stream_fps, memory_order_relaxed
        );
        (void)snprintf(
            pipeline,
            sizeof(pipeline),
            "%s | %s\nstream %u FPS | present %.0f/s | display %.0f Hz "
            "(max %.0f)",
            app->metal_renderer ? "Metal renderer" : "Renderer SDL",
            grd_pipeline_name(app->decoder_pipeline),
            stream_fps,
            (double)app->measured_present_rate,
            (double)app->display_refresh_rate,
            (double)app->display_max_refresh_rate
        );
        nk_layout_row_begin(context, NK_DYNAMIC, 36.0F, 3);
        nk_layout_row_push(context, 0.44F);
        ui_label(
            context,
            app->heading_font,
            "Session settings",
            NK_TEXT_LEFT,
            nk_rgb(29, 29, 31)
        );
        nk_layout_row_push(context, 0.25F);
        if (ui_button(context, "Close - F1", GRD_UI_BUTTON_PRIMARY)) {
            app->remote_settings_visible = false;
            set_remote_keyboard_grab(
                app,
                app->connection != NULL &&
                    grd_connection_role(app->connection) ==
                        GRD_ROLE_CONTROLLER
            );
            if (app->config_dirty) {
                (void)grd_config_save(&app->config, &app->last_error);
                app->config_dirty = false;
            }
            if (app->config.mouse_mode == GRD_MOUSE_RELATIVE) {
                (void)capture_remote_mouse(app, "relative mode");
            }
        }
        nk_layout_row_push(context, 0.31F);
        if (ui_button(context, "Disconnect", GRD_UI_BUTTON_DANGER)) {
            disconnect(app);
        }
        nk_layout_row_end(context);
        nk_layout_row_dynamic(context, 22.0F, 1);
        ui_label(
            context,
            app->small_font,
            "Press F1 to open or close this panel.",
            NK_TEXT_LEFT,
            nk_rgb(110, 110, 115)
        );

        nk_layout_row_dynamic(context, 54.0F, 1);
        ui_label_wrap(
            context,
            app->small_font,
            pipeline,
            nk_rgb(66, 66, 69)
        );

        draw_client_rate_controls(app, context, true, true);

        nk_layout_row_dynamic(context, 24.0F, 1);
        ui_label(
            context,
            app->small_font,
            "Local controls",
            NK_TEXT_LEFT,
            nk_rgb(134, 134, 139)
        );

        nk_layout_row_dynamic(context, 22.0F, 1);
        ui_label(
            context,
            app->small_font,
            "Mouse mode",
            NK_TEXT_LEFT,
            nk_rgb(29, 29, 31)
        );
        nk_layout_row_dynamic(context, 38.0F, 3);
        const grd_mouse_mode mouse_modes[] = {
            GRD_MOUSE_AUTOMATIC,
            GRD_MOUSE_ABSOLUTE,
            GRD_MOUSE_RELATIVE
        };
        const char *mouse_labels[] = {
            "Automatic",
            "Absolute cursor",
            "Relative camera"
        };
        for (size_t index = 0U; index < 3U; ++index) {
            if (ui_button(
                    context,
                    mouse_labels[index],
                    app->config.mouse_mode == mouse_modes[index]
                        ? GRD_UI_BUTTON_PRIMARY
                        : GRD_UI_BUTTON_SECONDARY
                ) && app->config.mouse_mode != mouse_modes[index]) {
                app->config.mouse_mode = mouse_modes[index];
                app->config_dirty = true;
                release_remote_mouse(app);
                GRD_INFO(
                    "client input: mouse mode=%s",
                    mouse_mode_name(app->config.mouse_mode)
                );
            }
        }
        nk_layout_row_dynamic(context, 34.0F, 1);
        ui_label_wrap(
            context,
            app->small_font,
            "Automatic captures on click; Absolute keeps the cursor; Relative controls the camera.",
            nk_rgb(110, 110, 115)
        );

        nk_bool fullscreen = app->config.remote_fullscreen ? nk_true : nk_false;
        nk_layout_row_dynamic(context, 38.0F, 2);
        if (nk_checkbox_label(
                context, "Start in full screen", &fullscreen
            )) {
            const bool requested = fullscreen != nk_false;
            if (set_remote_fullscreen(app, requested)) {
                app->config.remote_fullscreen = requested;
                app->config_dirty = true;
            }
        }
        if (app->remote_fullscreen_active) {
            if (ui_button(
                    context,
                    "Exit full screen",
                    GRD_UI_BUTTON_SECONDARY
                ) && set_remote_fullscreen(app, false)) {
                app->config.remote_fullscreen = false;
                app->config_dirty = true;
                (void)save_config_automatically(app, NULL);
            }
        } else {
            if (ui_button(
                    context,
                    "Enter full screen",
                    GRD_UI_BUTTON_SECONDARY
                ) && set_remote_fullscreen(app, true)) {
                app->config.remote_fullscreen = true;
                app->config_dirty = true;
                (void)save_config_automatically(app, NULL);
            }
        }

        nk_bool advanced =
            app->config.show_advanced_stats ? nk_true : nk_false;
        nk_layout_row_dynamic(context, 38.0F, 1);
        if (nk_checkbox_label(
                context,
                "Advanced information HUD",
                &advanced
            )) {
            app->config.show_advanced_stats = advanced != nk_false;
            app->config_dirty = true;
            (void)save_config_automatically(app, NULL);
        }

        float sensitivity =
            (float)app->config.mouse_sensitivity_percent / 100.0F;
        nk_layout_row_dynamic(context, 22.0F, 1);
        ui_label(
            context,
            app->small_font,
            "Relative mouse sensitivity",
            NK_TEXT_LEFT,
            nk_rgb(29, 29, 31)
        );
        nk_layout_row_dynamic(context, 42.0F, 1);
        if (nk_property_float(
                context,
                "Value",
                0.25F,
                &sensitivity,
                3.00F,
                0.05F,
                0.01F
            )) {
            app->config.mouse_sensitivity_percent =
                (uint32_t)(sensitivity * 100.0F + 0.5F);
            app->config_dirty = true;
        }
        nk_layout_row_dynamic(context, 22.0F, 1);
        ui_label(
            context,
            app->small_font,
            "1.00 = 1:1 movement  |  Range 0.25 - 3.00",
            NK_TEXT_LEFT,
            nk_rgb(110, 110, 115)
        );

        nk_layout_row_dynamic(context, 38.0F, 1);
        ui_label(
            context,
            app->small_font,
            "Esc, Alt+Tab, and Cmd+Tab are sent remotely. Press Esc 3 times in 2 seconds to release the mouse.",
            NK_TEXT_LEFT,
            nk_rgb(110, 110, 115)
        );

    }
    nk_end(context);
    context->style.window.fixed_background = saved_background;
    context->style.window.border_color = saved_border;
    context->style.window.padding = saved_padding;
    context->style.window.rounding = saved_rounding;
    context->style.window.border = saved_window_border;
}

static void hud_metric_row(
    struct nk_context *context,
    const struct nk_user_font *label_font,
    const struct nk_user_font *value_font,
    float content_width,
    float row_height,
    const char *label,
    const char *value,
    struct nk_color label_color,
    struct nk_color value_color
)
{
    const float label_width = 94.0F;
    nk_layout_row_begin(context, NK_STATIC, row_height, 2);
    nk_layout_row_push(context, label_width);
    ui_label(context, label_font, label, NK_TEXT_LEFT, label_color);
    nk_layout_row_push(
        context,
        content_width > label_width + context->style.window.spacing.x
            ? content_width - label_width - context->style.window.spacing.x
            : 1.0F
    );
    ui_label(context, value_font, value, NK_TEXT_RIGHT, value_color);
    nk_layout_row_end(context);
}

static void hud_sample_fps(grd_app *app, uint64_t now, float fps)
{
    if (app->hud_fps_sample_micros != 0ULL &&
        now - app->hud_fps_sample_micros < 250000ULL) {
        return;
    }
    app->hud_fps_history[app->hud_fps_history_next] = fps;
    app->hud_fps_history_next =
        (app->hud_fps_history_next + 1U) % GRD_HUD_FPS_HISTORY_CAPACITY;
    if (app->hud_fps_history_count < GRD_HUD_FPS_HISTORY_CAPACITY) {
        ++app->hud_fps_history_count;
    }
    app->hud_fps_sample_micros = now;
}

static float hud_history_value(const grd_app *app, uint32_t offset)
{
    const uint32_t start =
        (app->hud_fps_history_next + GRD_HUD_FPS_HISTORY_CAPACITY -
         app->hud_fps_history_count) %
        GRD_HUD_FPS_HISTORY_CAPACITY;
    return app->hud_fps_history[
        (start + offset) % GRD_HUD_FPS_HISTORY_CAPACITY
    ];
}

static void hud_draw_fps_graph(
    grd_app *app,
    struct nk_context *context,
    float target_fps
)
{
    struct nk_rect bounds;
    if (nk_widget(&bounds, context) == NK_WIDGET_INVALID) {
        return;
    }
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    nk_fill_rect(canvas, bounds, 2.0F, nk_rgba(0, 0, 0, 72));
    nk_stroke_line(
        canvas,
        bounds.x,
        bounds.y + bounds.h - 1.0F,
        bounds.x + bounds.w,
        bounds.y + bounds.h - 1.0F,
        1.0F,
        nk_rgba(255, 255, 255, 38)
    );
    if (app->hud_fps_history_count < 2U || target_fps <= 0.0F) {
        return;
    }
    const float scale = target_fps * 1.04F;
    const float step = bounds.w /
                       (float)(app->hud_fps_history_count - 1U);
    float previous_x = bounds.x;
    float previous_value = hud_history_value(app, 0U);
    if (previous_value > scale) {
        previous_value = scale;
    }
    float previous_y = bounds.y + bounds.h -
                       previous_value / scale * bounds.h;
    for (uint32_t index = 1U; index < app->hud_fps_history_count; ++index) {
        float value = hud_history_value(app, index);
        if (value > scale) {
            value = scale;
        }
        const float x = bounds.x + (float)index * step;
        const float y = bounds.y + bounds.h - value / scale * bounds.h;
        const struct nk_color color =
            value >= target_fps * 0.95F
                ? nk_rgb(112, 235, 132)
                : (value >= target_fps * 0.80F
                       ? nk_rgb(255, 205, 92)
                       : nk_rgb(255, 104, 112));
        nk_stroke_line(
            canvas, previous_x, previous_y, x, y, 1.5F, color
        );
        previous_x = x;
        previous_y = y;
    }
}

static void draw_advanced_stats(
    grd_app *app,
    struct nk_context *context,
    int window_width
)
{
    const uint64_t sample_now = grd_now_micros();
    const uint64_t received_bytes = atomic_load_explicit(
        &app->remote_video_bytes_received, memory_order_relaxed
    );
    if (app->hud_bitrate_sample_micros == 0ULL) {
        app->hud_bitrate_sample_micros = sample_now;
        app->hud_bitrate_sample_bytes = received_bytes;
    } else if (sample_now - app->hud_bitrate_sample_micros >= 500000ULL) {
        const uint64_t elapsed =
            sample_now - app->hud_bitrate_sample_micros;
        const uint64_t delta =
            received_bytes >= app->hud_bitrate_sample_bytes
                ? received_bytes - app->hud_bitrate_sample_bytes
                : 0ULL;
        uint64_t measured_kbps = delta * 8000ULL / elapsed;
        if (measured_kbps > UINT32_MAX) {
            measured_kbps = UINT32_MAX;
        }
        atomic_store_explicit(
            &app->remote_receive_bitrate_kbps,
            (uint32_t)measured_kbps,
            memory_order_relaxed
        );
        app->hud_bitrate_sample_micros = sample_now;
        app->hud_bitrate_sample_bytes = received_bytes;
    }
    const float panel_width = window_width < 340
                                  ? (float)window_width - 24.0F
                                  : 326.0F;
    const float panel_x = (float)window_width - panel_width - 18.0F;
    const struct nk_style_item saved_background =
        context->style.window.fixed_background;
    const struct nk_color saved_border = context->style.window.border_color;
    const struct nk_vec2 saved_padding = context->style.window.padding;
    const float saved_rounding = context->style.window.rounding;
    const float saved_window_border = context->style.window.border;
    context->style.window.fixed_background =
        nk_style_item_color(nk_rgba(4, 7, 10, 202));
    context->style.window.border_color = nk_rgba(126, 174, 255, 74);
    context->style.window.padding = nk_vec2(12.0F, 10.0F);
    context->style.window.rounding = 3.0F;
    context->style.window.border = 1.0F;
    if (nk_begin(
            context,
            "Advanced statistics",
            nk_rect(panel_x, 18.0F, panel_width, 364.0F),
            NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT
        )) {
        const uint32_t configured_fps = atomic_load_explicit(
            &app->remote_stream_fps, memory_order_relaxed
        );
        const uint32_t requested_fps = effective_client_stream_fps(app);
        const uint32_t decoded_tenths = atomic_load_explicit(
            &app->remote_receive_fps_tenths, memory_order_relaxed
        );
        const uint32_t encoder_bitrate_kbps = atomic_load_explicit(
            &app->remote_stream_bitrate_kbps, memory_order_relaxed
        );
        const uint32_t measured_bitrate_kbps = atomic_load_explicit(
            &app->remote_receive_bitrate_kbps, memory_order_relaxed
        );
        const uint32_t stream_width = atomic_load_explicit(
            &app->remote_stream_width, memory_order_relaxed
        );
        const uint32_t stream_height = atomic_load_explicit(
            &app->remote_stream_height, memory_order_relaxed
        );
        const uint64_t skipped_frames = atomic_load_explicit(
            &app->remote_source_skipped_frames, memory_order_relaxed
        );
        const uint64_t recoveries = atomic_load_explicit(
            &app->remote_source_gap_recoveries, memory_order_relaxed
        );
        const uint64_t decode_failures = atomic_load_explicit(
            &app->decode_failures, memory_order_relaxed
        );
        const uint64_t input_rejections = atomic_load_explicit(
            &app->remote_input_rejection_reports, memory_order_relaxed
        );
        if (app->hud_health_window_started_micros == 0ULL) {
            app->hud_health_window_started_micros = sample_now;
            app->hud_last_skipped_frames = skipped_frames;
            app->hud_last_recoveries = recoveries;
            app->hud_last_decode_failures = decode_failures;
        } else {
            if (sample_now - app->hud_health_window_started_micros >=
                5000000ULL) {
                app->hud_health_window_started_micros = sample_now;
                app->hud_recent_skipped_frames = 0ULL;
                app->hud_recent_recoveries = 0ULL;
                app->hud_recent_decode_failures = 0ULL;
            }
            if (skipped_frames >= app->hud_last_skipped_frames) {
                app->hud_recent_skipped_frames +=
                    skipped_frames - app->hud_last_skipped_frames;
            }
            if (recoveries >= app->hud_last_recoveries) {
                app->hud_recent_recoveries +=
                    recoveries - app->hud_last_recoveries;
            }
            if (decode_failures >= app->hud_last_decode_failures) {
                app->hud_recent_decode_failures +=
                    decode_failures - app->hud_last_decode_failures;
            }
            app->hud_last_skipped_frames = skipped_frames;
            app->hud_last_recoveries = recoveries;
            app->hud_last_decode_failures = decode_failures;
        }
        const uint32_t max_arrival_gap = atomic_load_explicit(
            &app->remote_arrival_max_gap_us, memory_order_relaxed
        );
        const uint32_t max_source_gap = atomic_load_explicit(
            &app->remote_source_max_gap_us, memory_order_relaxed
        );
        const int loss_percent = SDL_GetAtomicInt(&app->abr_loss_percent);
        const int rtt_micros = SDL_GetAtomicInt(&app->abr_rtt_micros);
        const float received_fps = (float)decoded_tenths / 10.0F;
        hud_sample_fps(app, sample_now, received_fps);
        float minimum_frame_ms = 0.0F;
        float maximum_frame_ms = 0.0F;
        for (uint32_t index = 0U; index < app->hud_fps_history_count; ++index) {
            const float fps = hud_history_value(app, index);
            if (fps <= 0.01F) {
                continue;
            }
            const float frame_ms = 1000.0F / fps;
            if (minimum_frame_ms == 0.0F || frame_ms < minimum_frame_ms) {
                minimum_frame_ms = frame_ms;
            }
            if (frame_ms > maximum_frame_ms) {
                maximum_frame_ms = frame_ms;
            }
        }
        const bool cadence_limited = configured_fps != 0U &&
                                     configured_fps < requested_fps;
        const bool cadence_low = configured_fps != 0U &&
                                 received_fps < (float)configured_fps * 0.90F;
        const bool health_warning =
            cadence_limited || cadence_low || loss_percent > 0 ||
            app->hud_recent_recoveries > 0ULL ||
            app->hud_recent_decode_failures > 0ULL ||
            input_rejections > 0ULL;
        const char *health_text = health_warning ? "LIVE  •  CHECK" :
                                                   "LIVE  •  STABLE";
        const struct nk_color white = nk_rgb(246, 248, 252);
        const struct nk_color muted = nk_rgb(166, 176, 190);
        const struct nk_color green = nk_rgb(112, 235, 132);
        const struct nk_color cyan = nk_rgb(91, 199, 255);
        const struct nk_color blue = nk_rgb(126, 174, 255);
        const struct nk_color magenta = nk_rgb(224, 126, 255);
        const struct nk_color yellow = nk_rgb(255, 205, 92);
        const struct nk_color red = nk_rgb(255, 104, 112);
        const float content_width = panel_width - 24.0F;
        char line[256];
        hud_metric_row(
            context,
            app->small_font,
            app->small_font,
            content_width,
            19.0F,
            "GRD",
            health_text,
            blue,
            health_warning ? yellow : green
        );
        (void)snprintf(
            line,
            sizeof(line),
            "%.1f  /  %u",
            (double)received_fps,
            configured_fps != 0U ? configured_fps : requested_fps
        );
        hud_metric_row(
            context,
            app->small_font,
            app->heading_font,
            content_width,
            29.0F,
            "SOURCE FPS",
            line,
            green,
            cadence_low ? yellow : white
        );
        (void)snprintf(
            line,
            sizeof(line),
            "%.1f ms   min %.1f   max %.1f",
            received_fps > 0.01F ? 1000.0 / (double)received_fps : 0.0,
            (double)minimum_frame_ms,
            (double)maximum_frame_ms
        );
        hud_metric_row(
            context,
            app->small_font,
            app->small_font,
            content_width,
            19.0F,
            "FRAME TIME",
            line,
            green,
            white
        );
        nk_layout_row_dynamic(context, 42.0F, 1);
        hud_draw_fps_graph(
            app,
            context,
            (float)(configured_fps != 0U ? configured_fps : requested_fps)
        );
        (void)snprintf(
            line,
            sizeof(line),
            "%.1f real  /  %.1f enc Mbps",
            (double)measured_bitrate_kbps / 1000.0,
            (double)encoder_bitrate_kbps / 1000.0
        );
        hud_metric_row(
            context,
            app->small_font,
            app->small_font,
            content_width,
            19.0F,
            "BITRATE",
            line,
            magenta,
            white
        );
        const char *upscale_name =
            app->config.client_upscale_mode == GRD_CLIENT_UPSCALE_BALANCED
                ? "balanced"
                : app->config.client_upscale_mode ==
                          GRD_CLIENT_UPSCALE_PERFORMANCE
                      ? "maximum"
                      : "native";
        (void)snprintf(
            line,
            sizeof(line),
            "%s  /  pace %s  /  cursor %s",
            upscale_name,
            app->config.client_frame_pacing ? "on" : "off",
            app->config.client_cursor_prediction ? "on" : "off"
        );
        hud_metric_row(
            context,
            app->small_font,
            app->small_font,
            content_width,
            19.0F,
            "OFFLOAD",
            line,
            magenta,
            white
        );
        (void)snprintf(
            line,
            sizeof(line),
            "%s  /  host reject %llu",
            mouse_mode_name(app->config.mouse_mode),
            (unsigned long long)input_rejections
        );
        hud_metric_row(
            context,
            app->small_font,
            app->small_font,
            content_width,
            19.0F,
            "INPUT",
            line,
            input_rejections == 0ULL ? green : red,
            input_rejections == 0ULL ? white : yellow
        );
        (void)snprintf(
            line,
            sizeof(line),
            "%d%% loss  /  %.1f ms RTT",
            loss_percent,
            (double)rtt_micros / 1000.0
        );
        hud_metric_row(
            context,
            app->small_font,
            app->small_font,
            content_width,
            19.0F,
            "NETWORK",
            line,
            cyan,
            loss_percent > 0 ? yellow : white
        );
        (void)snprintf(
            line,
            sizeof(line),
            "%u x %u  /  %s",
            stream_width,
            stream_height,
            grd_codec_name(app->decoder_codec)
        );
        hud_metric_row(
            context,
            app->small_font,
            app->small_font,
            content_width,
            19.0F,
            "VIDEO",
            line,
            blue,
            white
        );
        (void)snprintf(
            line,
            sizeof(line),
            "%s",
            pipeline_display_name(app->decoder_pipeline)
        );
        hud_metric_row(
            context,
            app->small_font,
            app->small_font,
            content_width,
            19.0F,
            "PIPELINE",
            line,
            cyan,
            white
        );
        (void)snprintf(
            line,
            sizeof(line),
            "%.0f/s present  /  %.0f Hz",
            (double)app->measured_present_rate,
            (double)app->display_refresh_rate
        );
        hud_metric_row(
            context,
            app->small_font,
            app->small_font,
            content_width,
            19.0F,
            "DISPLAY",
            line,
            magenta,
            white
        );
        (void)snprintf(
            line,
            sizeof(line),
            "5s %llu/%llu  |  total %llu/%llu",
            (unsigned long long)app->hud_recent_skipped_frames,
            (unsigned long long)app->hud_recent_recoveries,
            (unsigned long long)skipped_frames,
            (unsigned long long)recoveries
        );
        hud_metric_row(
            context,
            app->small_font,
            app->small_font,
            content_width,
            19.0F,
            "RECOVERY",
            line,
            app->hud_recent_recoveries == 0ULL ? green : red,
            app->hud_recent_recoveries == 0ULL ? white : yellow
        );
        (void)snprintf(
            line,
            sizeof(line),
            "%.1f ms arrival  /  %.1f ms source",
            (double)max_arrival_gap / 1000.0,
            (double)max_source_gap / 1000.0
        );
        hud_metric_row(
            context,
            app->small_font,
            app->small_font,
            content_width,
            19.0F,
            "GAP MAX",
            line,
            muted,
            white
        );
        (void)snprintf(
            line,
            sizeof(line),
            "5s %llu  /  total %llu  /  %s",
            (unsigned long long)app->hud_recent_decode_failures,
            (unsigned long long)decode_failures,
            app->config.sharp_video_scaling ? "linear GPU" : "nearest"
        );
        hud_metric_row(
            context,
            app->small_font,
            app->small_font,
            content_width,
            19.0F,
            "DECODE",
            line,
            app->hud_recent_decode_failures == 0ULL ? green : red,
            app->hud_recent_decode_failures == 0ULL ? white : yellow
        );
    }
    nk_end(context);
    context->style.window.fixed_background = saved_background;
    context->style.window.border_color = saved_border;
    context->style.window.padding = saved_padding;
    context->style.window.rounding = saved_rounding;
    context->style.window.border = saved_window_border;
}

void grd_app_draw(grd_app *app, struct nk_context *context, SDL_Renderer *renderer)
{
    const uint64_t now = grd_now_micros();
    if (app->mode == GRD_APP_REMOTE &&
        app->connection != NULL &&
        (!grd_connection_is_active(app->connection) ||
         app->media_connection == NULL ||
         !grd_connection_is_active(app->media_connection))) {
        disconnect(app);
        set_status(app, "Remote connection interrupted");
    }
    if (app->mode == GRD_APP_REMOTE && app->connection != NULL &&
        grd_connection_is_active(app->connection) &&
        (app->remote_heartbeat_sent_micros == 0ULL ||
         now - app->remote_heartbeat_sent_micros >= 1000000ULL)) {
        /* Transport-level lease heartbeat. It is intentionally sent even
         * while F1 is open or the mouse is idle, so a live controller keeps
         * its role while a crashed process becomes replaceable. */
        const uint64_t heartbeat = now;
        if (grd_connection_send(
                app->connection,
                GRD_PACKET_PING,
                &heartbeat,
                sizeof(heartbeat),
                NULL
            ) == GRD_OK) {
            app->remote_heartbeat_sent_micros = now;
        }
    }
    if (app->host != NULL &&
        now - app->clipboard_checked_micros >= 500000ULL) {
        char *text = grd_platform_clipboard_read();
        if (text != NULL) {
            const size_t length = strlen(text);
            if (length <= GRD_MAX_CLIPBOARD &&
                !clipboard_matches(app, text)) {
                clipboard_remember(app, text);
                (void)grd_host_broadcast(
                    app->host,
                    GRD_PACKET_CLIPBOARD,
                    text,
                    length,
                    NULL
                );
            }
            free(text);
        }
        app->clipboard_checked_micros = now;
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(SDL_GetRenderWindow(renderer), &width, &height);
    const bool drawing_remote =
        app->mode == GRD_APP_REMOTE && app->connection != NULL;
    const struct nk_vec2 saved_padding = context->style.window.padding;
    const struct nk_vec2 saved_spacing = context->style.window.spacing;
    const struct nk_style_item saved_background =
        context->style.window.fixed_background;
    const struct nk_color saved_text = context->style.text.color;
    if (drawing_remote) {
        /* The video owns every drawable pixel. Controls are an F1 overlay,
         * never a permanent toolbar that shrinks the remote desktop. */
        context->style.window.padding = nk_vec2(0.0F, 0.0F);
        context->style.window.spacing = nk_vec2(0.0F, 0.0F);
        context->style.window.fixed_background =
            nk_style_item_color(nk_rgb(7, 9, 13));
        context->style.text.color = nk_rgb(245, 248, 255);
    }
    if (nk_begin(
            context,
            "GRD",
            nk_rect(0.0F, 0.0F, (float)width, (float)height),
            drawing_remote ? NK_WINDOW_NO_SCROLLBAR : 0
        )) {
        if (drawing_remote) {
            draw_remote(app, context);
        } else {
            draw_home(app, context);
        }
    }
    nk_end(context);
    context->style.window.padding = saved_padding;
    context->style.window.spacing = saved_spacing;
    context->style.window.fixed_background = saved_background;
    context->style.text.color = saved_text;
    if (drawing_remote && app->config.show_advanced_stats &&
        app->connection != NULL) {
        draw_advanced_stats(app, context, width);
        /* Nuklear raises a clicked window above its siblings. The full-size
         * remote-video window therefore used to cover this separate HUD as
         * soon as the user clicked to capture the mouse. Restore the HUD to
         * the front on every frame; it is NO_INPUT, so this affects only
         * drawing order and never steals mouse or keyboard events. */
        nk_window_show(context, "Advanced statistics", NK_SHOWN);
        nk_window_set_focus(context, "Advanced statistics");
    }
    if (drawing_remote && app->remote_settings_visible &&
        app->connection != NULL) {
        draw_remote_settings(app, context, width, height);
        /* The F1 panel remains the top interactive layer while it is open;
         * closing it returns the permanent telemetry HUD to the top. */
        nk_window_set_focus(context, "Session settings");
    } else if (!drawing_remote && app->home_settings_visible) {
        draw_modal_backdrop(context, width, height);
        draw_home_settings_modal(app, context, width, height);
        nk_window_set_focus(context, "Advanced settings");
    } else if (!drawing_remote && app->remote_access_modal_visible) {
        draw_modal_backdrop(context, width, height);
        draw_remote_access_modal(app, context, width, height);
        nk_window_set_focus(context, "OpenSSH access");
    } else if (!drawing_remote && app->lan_join_modal_visible) {
        draw_modal_backdrop(context, width, height);
        draw_lan_join_modal(app, context, width, height);
    }
}

static void send_input(grd_app *app, const grd_input_event *event)
{
    if (app->connection != NULL &&
        grd_connection_role(app->connection) == GRD_ROLE_CONTROLLER) {
        grd_error error = {0};
        grd_status status = GRD_INVALID_ARGUMENT;
        bool realtime_sent = false;
        if (event->kind == GRD_INPUT_POINTER_RELATIVE &&
            app->media_connection != NULL &&
            grd_connection_video_udp_active(app->media_connection)) {
            status = grd_connection_send_realtime_input(
                app->media_connection, event, &error
            );
            realtime_sent = status == GRD_OK;
            if (realtime_sent) {
                atomic_fetch_add_explicit(
                    &app->relative_input_udp_sent, 1U, memory_order_relaxed
                );
            }
        }
        if (!realtime_sent) {
            memset(&error, 0, sizeof(error));
            status = grd_connection_send(
                app->connection,
                GRD_PACKET_INPUT,
                event,
                sizeof(*event),
                &error
            );
            if (status == GRD_OK &&
                event->kind == GRD_INPUT_POINTER_RELATIVE) {
                atomic_fetch_add_explicit(
                    &app->relative_input_tcp_sent, 1U, memory_order_relaxed
                );
            }
        }
        if (status != GRD_OK) {
            if (event->kind == GRD_INPUT_POINTER_RELATIVE) {
                atomic_fetch_add_explicit(
                    &app->relative_input_send_failures,
                    1U,
                    memory_order_relaxed
                );
            }
            if (error.code == GRD_OK) {
                error.code = status;
                (void)snprintf(
                    error.message,
                    sizeof(error.message),
                    "Failed to send input over the control channel"
                );
            }
            publish_error(app, &error);
            set_status(app, error.message);
            signal_main_thread(app);
        }
    }
}

void grd_app_handle_remote_event(grd_app *app, const SDL_Event *event)
{
    if (app->mode != GRD_APP_REMOTE || app->connection == NULL) {
        return;
    }
#if defined(__APPLE__)
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST &&
        app->remote_command_tab_active) {
        /* Defensive release if macOS still steals focus despite SDL's
         * keyboard grab. Never leave Alt or Tab held on the Windows host. */
        grd_input_event release;
        memset(&release, 0, sizeof(release));
        release.kind = GRD_INPUT_KEY;
        release.code = SDL_SCANCODE_TAB;
        send_input(app, &release);
        release.code = GRD_KEY_LEFT_ALT;
        send_input(app, &release);
        app->remote_command_tab_active = false;
    }
    if (event->type == SDL_EVENT_KEY_UP &&
        app->remote_command_tab_active &&
        (event->key.scancode == SDL_SCANCODE_LGUI ||
         event->key.scancode == SDL_SCANCODE_RGUI)) {
        grd_input_event release_alt;
        memset(&release_alt, 0, sizeof(release_alt));
        release_alt.kind = GRD_INPUT_KEY;
        release_alt.code = GRD_KEY_LEFT_ALT;
        send_input(app, &release_alt);
        app->remote_command_tab_active = false;
        return;
    }
#endif
    /* F1 is the sole local session shortcut and is never forwarded. Opening
     * settings releases relative capture so the overlay can be operated. */
    if ((event->type == SDL_EVENT_KEY_DOWN ||
         event->type == SDL_EVENT_KEY_UP) &&
        event->key.scancode == SDL_SCANCODE_F1) {
        if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) {
            app->remote_settings_visible = !app->remote_settings_visible;
            set_remote_keyboard_grab(
                app,
                !app->remote_settings_visible &&
                    grd_connection_role(app->connection) ==
                        GRD_ROLE_CONTROLLER
            );
            if (app->remote_settings_visible) {
                release_remote_mouse(app);
            } else if (app->config.mouse_mode == GRD_MOUSE_RELATIVE) {
                (void)capture_remote_mouse(app, "closing F1 panel");
            }
            app->escape_press_count = 0U;
            app->escape_sequence_started_micros = 0ULL;
            if (!app->remote_settings_visible && app->config_dirty) {
                (void)grd_config_save(&app->config, &app->last_error);
                app->config_dirty = false;
            }
        }
        return;
    }
    /* Overlay interaction is local; without this guard a click on the
     * sensitivity control would also click the remote game underneath it. */
    if (app->remote_settings_visible ||
        grd_connection_role(app->connection) != GRD_ROLE_CONTROLLER) {
        return;
    }
    grd_input_event input;
    memset(&input, 0, sizeof(input));
    if (event->type == SDL_EVENT_MOUSE_MOTION) {
        if (app->relative_mouse_mode) {
            const double sensitivity =
                (double)app->config.mouse_sensitivity_percent / 100.0;
            input.kind = GRD_INPUT_POINTER_RELATIVE;
            app->relative_remainder_x +=
                (double)event->motion.xrel * sensitivity;
            app->relative_remainder_y +=
                (double)event->motion.yrel * sensitivity;
            input.delta_x = consume_relative_delta(&app->relative_remainder_x);
            input.delta_y = consume_relative_delta(&app->relative_remainder_y);
            if (input.delta_x != 0 || input.delta_y != 0) {
                send_input(app, &input);
            }
            return;
        }
        if (event->motion.x < app->view_rect.x ||
            event->motion.y < app->view_rect.y ||
            event->motion.x > app->view_rect.x + app->view_rect.w ||
            event->motion.y > app->view_rect.y + app->view_rect.h) {
            return;
        }
        input.kind = GRD_INPUT_POINTER_MOVE;
        input.x = (event->motion.x - app->view_rect.x) / app->view_rect.w;
        input.y = (event->motion.y - app->view_rect.y) / app->view_rect.h;
        if (app->config.client_cursor_prediction) {
            /* Predict the remote cursor locally; the host state reconciles
             * this value when it arrives on the control channel. */
            SDL_LockMutex(app->cursor_mutex);
            app->predicted_cursor_x = input.x;
            app->predicted_cursor_y = input.y;
            app->predicted_cursor_valid = true;
            SDL_UnlockMutex(app->cursor_mutex);
        }
        send_input(app, &input);
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
               event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (!app->relative_mouse_mode &&
            (event->button.x < app->view_rect.x ||
             event->button.y < app->view_rect.y ||
             event->button.x > app->view_rect.x + app->view_rect.w ||
             event->button.y > app->view_rect.y + app->view_rect.h)) {
            return;
        }
        input.kind = GRD_INPUT_POINTER_BUTTON;
        input.pressed = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        input.code = event->button.button == SDL_BUTTON_RIGHT
                         ? 1U
                         : event->button.button == SDL_BUTTON_MIDDLE ? 2U : 0U;
        send_input(app, &input);
        /* Deliver the initiating click before changing SDL's mouse mode.
         * SDL may synthesize a motion event while enabling relative mode;
         * changing the mode first used to put that motion ahead of the click
         * on the remote side. */
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            !app->relative_mouse_mode && app->window != NULL &&
            app->config.mouse_mode != GRD_MOUSE_ABSOLUTE) {
            (void)capture_remote_mouse(app, "click");
        }
    } else if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        input.kind = GRD_INPUT_SCROLL;
        input.delta_x = (int32_t)(event->wheel.x * 120.0F);
        input.delta_y = (int32_t)(event->wheel.y * 120.0F);
        send_input(app, &input);
    } else if (event->type == SDL_EVENT_KEY_DOWN ||
               event->type == SDL_EVENT_KEY_UP) {
#if defined(__APPLE__)
        const bool command_tab =
            event->key.scancode == SDL_SCANCODE_TAB &&
            (((event->key.mod & SDL_KMOD_GUI) != 0U) ||
             app->remote_command_tab_active);
        if (command_tab) {
            if (event->type == SDL_EVENT_KEY_DOWN &&
                !app->remote_command_tab_active) {
                /* Command was already forwarded and becomes Ctrl on a
                 * Windows host. Release it, hold Alt instead, then forward
                 * Tab. Alt stays down to support repeated Cmd+Tab presses. */
                const uint32_t command_key =
                    (event->key.mod & SDL_KMOD_RGUI) != 0U
                        ? GRD_KEY_RIGHT_GUI
                        : GRD_KEY_LEFT_GUI;
                grd_input_event chord;
                memset(&chord, 0, sizeof(chord));
                chord.kind = GRD_INPUT_KEY;
                chord.code = command_key;
                send_input(app, &chord);
                chord.pressed = 1U;
                chord.code = GRD_KEY_LEFT_ALT;
                send_input(app, &chord);
                app->remote_command_tab_active = true;
                GRD_INFO("client shortcut: Cmd+Tab -> remote Alt+Tab");
            }
            grd_input_event tab;
            memset(&tab, 0, sizeof(tab));
            tab.kind = GRD_INPUT_KEY;
            tab.pressed = event->type == SDL_EVENT_KEY_DOWN;
            tab.code = SDL_SCANCODE_TAB;
            send_input(app, &tab);
            return;
        }
#endif
        bool release_mouse_after_send = false;
        if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
            event->key.scancode == SDL_SCANCODE_ESCAPE &&
            app->relative_mouse_mode && app->window != NULL) {
            const uint64_t now = grd_now_micros();
            if (app->escape_sequence_started_micros == 0ULL ||
                now - app->escape_sequence_started_micros > 2000000ULL) {
                app->escape_sequence_started_micros = now;
                app->escape_press_count = 1U;
            } else {
                ++app->escape_press_count;
            }
            if (app->escape_press_count >= 3U) {
                release_mouse_after_send = true;
                app->escape_press_count = 0U;
                app->escape_sequence_started_micros = 0ULL;
            }
        }
        input.kind = GRD_INPUT_KEY;
        input.pressed = event->type == SDL_EVENT_KEY_DOWN;
        input.code = (uint32_t)event->key.scancode;
        input.modifiers = (uint16_t)event->key.mod;
        send_input(app, &input);
        if (event->key.scancode == SDL_SCANCODE_LCTRL ||
            event->key.scancode == SDL_SCANCODE_RCTRL ||
            event->key.scancode == SDL_SCANCODE_LGUI ||
            event->key.scancode == SDL_SCANCODE_RGUI) {
            GRD_INFO(
                "client input modifier: scancode=%u %s mod=0x%x",
                (unsigned)event->key.scancode,
                input.pressed ? "down" : "up",
                (unsigned)input.modifiers
            );
        }
        if (release_mouse_after_send) {
            release_remote_mouse(app);
        }
    } else if (event->type == SDL_EVENT_TEXT_INPUT) {
        input.kind = GRD_INPUT_TEXT;
        input.text_length = (uint32_t)strlen(event->text.text);
        if (input.text_length > sizeof(input.text)) {
            input.text_length = sizeof(input.text);
        }
        memcpy(input.text, event->text.text, input.text_length);
        send_input(app, &input);
    } else if (event->type == SDL_EVENT_CLIPBOARD_UPDATE) {
        char *text = grd_platform_clipboard_read();
        if (text != NULL) {
            const size_t length = strlen(text);
            if (length <= GRD_MAX_CLIPBOARD &&
                !clipboard_matches(app, text)) {
                clipboard_remember(app, text);
                /* Clipboard uses the authenticated media socket so a large
                 * paste cannot sit in front of mouse/key packets on control. */
                grd_connection *clipboard_connection =
                    app->media_connection != NULL
                        ? app->media_connection
                        : app->connection;
                (void)grd_connection_send(
                    clipboard_connection,
                    GRD_PACKET_CLIPBOARD,
                    text,
                    length,
                    NULL
                );
            }
            free(text);
        }
    }
}
