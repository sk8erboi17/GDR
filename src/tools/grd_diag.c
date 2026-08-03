/* grd-diag: headless peer-network diagnostic client.
 *
 * Connects to a GRD host like the real client (control + authenticated UDP
 * media channel), receives the actual video stream, decodes it with the
 * software decoder and reports structured diagnostics that the GUI hides:
 *   - received VIDEO_CONFIG (codec, resolution, fps, bitrate)
 *   - rx rate, assembled frames, keyframes, SPS/PPS in-band presence
 *   - decode results (ok / would-block / failed with message)
 *   - per-5 s window stats and a final verdict with a process exit code
 *
 * Usage: grd-diag <address> <port> <password> [seconds=15]
 *        grd-diag --cuda-selftest
 *        grd-diag --capture-selftest [seconds=5] [fps=120]
 * Exit:  0 video decoded, 1 video not decoded, 2 usage, 3 connect failed.
 */
#include "grd/codec.h"
#include "grd/common.h"
#include "grd/config.h"
#include "grd/gpu.h"
#include "grd/platform.h"
#include "grd/transport.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIAG_WIRE_PREFIX 25U

typedef struct diag_state {
    SDL_Mutex *mutex;
    grd_connection *control;
    grd_decoder *decoder;
    grd_video_codec codec;
    bool have_config;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t bitrate_kbps;
    /* Window counters (reset every 5 s). */
    uint64_t frames_rx;
    uint64_t frames_decoded;
    uint64_t frames_failed;
    uint64_t wouldblock;
    uint64_t keyframes;
    uint64_t sps_frames;
    uint64_t pps_frames;
    uint64_t payload_bytes;
    /* Totals. */
    uint64_t total_rx;
    uint64_t total_decoded;
    uint64_t total_failed;
    uint64_t decode_fail_logged;
} diag_state;

typedef struct diag_timing {
    uint64_t samples;
    uint64_t total_micros;
    uint64_t maximum_micros;
} diag_timing;

static void diag_timing_add(diag_timing *timing, uint64_t micros)
{
    if (timing == NULL) {
        return;
    }
    ++timing->samples;
    timing->total_micros += micros;
    if (micros > timing->maximum_micros) {
        timing->maximum_micros = micros;
    }
}

static double diag_timing_average(const diag_timing *timing)
{
    return timing != NULL && timing->samples != 0U
               ? (double)timing->total_micros / (double)timing->samples
               : 0.0;
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) |
           ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) |
           (uint32_t)data[3];
}

static uint64_t read_be64(const uint8_t *data)
{
    uint64_t value = 0U;
    for (size_t index = 0U; index < 8U; ++index) {
        value = (value << 8U) | data[index];
    }
    return value;
}

static bool start_code_at(
    const uint8_t *data,
    size_t size,
    size_t offset
)
{
    return (offset + 3U <= size && data[offset] == 0U &&
            data[offset + 1U] == 0U && data[offset + 2U] == 1U) ||
           (offset + 4U <= size && data[offset] == 0U &&
            data[offset + 1U] == 0U && data[offset + 2U] == 0U &&
            data[offset + 3U] == 1U);
}

/* Counts whether the access unit contains SPS (type 7) and/or PPS (8),
 * handling both Annex-B (start codes) and AVCC (4-byte lengths). */
static void scan_parameter_sets(
    const uint8_t *data,
    size_t size,
    bool *sps,
    bool *pps
)
{
    *sps = false;
    *pps = false;
    if (data == NULL || size == 0U) {
        return;
    }
    bool annex_b = false;
    for (size_t index = 0U; index < size; ++index) {
        if (start_code_at(data, size, index)) {
            annex_b = true;
            break;
        }
    }
    if (annex_b) {
        size_t offset = 0U;
        while (offset < size) {
            while (offset < size &&
                   !start_code_at(data, size, offset)) {
                ++offset;
            }
            if (offset >= size) {
                break;
            }
            offset += data[offset + 2U] == 1U ? 3U : 4U;
            const size_t nal_start = offset;
            while (offset < size &&
                   !start_code_at(data, size, offset)) {
                ++offset;
            }
            if (offset > nal_start) {
                const uint8_t type = data[nal_start] & 0x1FU;
                if (type == 7U) *sps = true;
                if (type == 8U) *pps = true;
            }
        }
        return;
    }
    size_t offset = 0U;
    while (offset + 4U <= size) {
        const uint32_t nal_size = read_be32(data + offset);
        offset += 4U;
        if (nal_size == 0U || (size_t)nal_size > size - offset) {
            return;
        }
        const uint8_t type = data[offset] & 0x1FU;
        if (type == 7U) *sps = true;
        if (type == 8U) *pps = true;
        offset += (size_t)nal_size;
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

static bool diag_packet(
    grd_packet_type type,
    const void *payload,
    size_t payload_length,
    bool payload_takeable,
    grd_role role,
    void *userdata
)
{
    (void)payload_takeable;
    (void)role;
    diag_state *state = userdata;
    if (type == GRD_PACKET_VIDEO_CONFIG &&
        payload_length == sizeof(grd_video_config)) {
        const grd_video_config *config = payload;
        const grd_video_codec codec = codec_from_name(config->codec);
        printf(
            "[config] codec=%s %ux%u fps=%u bitrate=%u kbps\n",
            grd_codec_name(codec),
            config->width,
            config->height,
            config->fps,
            config->bitrate_kbps
        );
        fflush(stdout);
        SDL_LockMutex(state->mutex);
        const bool recreate =
            state->decoder == NULL || state->codec != codec ||
            state->width != config->width ||
            state->height != config->height;
        if (recreate) {
            grd_decoder_destroy(state->decoder);
            state->decoder = NULL;
            grd_pipeline_kind active = GRD_PIPELINE_SOFTWARE;
            grd_error error = {0};
            state->decoder = grd_decoder_create(
                GRD_PIPELINE_INTEGRATED_SOFTWARE,
                codec,
                &active,
                &error
            );
            printf(
                "[decoder] codec=%s %s\n",
                grd_codec_name(codec),
                state->decoder != NULL ? "ok" : "FAILED"
            );
            if (state->decoder == NULL) {
                printf(
                    "[decoder] error: %s (code %d)\n",
                    error.message[0] != '\0' ? error.message : "unknown",
                    (int)error.code
                );
            }
            fflush(stdout);
        }
        state->codec = codec;
        state->width = config->width;
        state->height = config->height;
        state->fps = config->fps;
        state->bitrate_kbps = config->bitrate_kbps;
        state->have_config = true;
        SDL_UnlockMutex(state->mutex);
        if (state->control != NULL) {
            (void)grd_connection_send(
                state->control,
                GRD_PACKET_REQUEST_KEYFRAME,
                NULL,
                0U,
                NULL
            );
        }
        return false;
    }
    if (type == GRD_PACKET_VIDEO_FRAME &&
        payload_length > DIAG_WIRE_PREFIX) {
        const uint8_t *wire = payload;
        const bool keyframe = wire[16U] != 0U;
        const uint64_t frame_id = read_be64(wire + 17U);
        const uint8_t *video = wire + DIAG_WIRE_PREFIX;
        const size_t video_size = payload_length - DIAG_WIRE_PREFIX;
        bool sps = false;
        bool pps = false;
        scan_parameter_sets(video, video_size, &sps, &pps);
        SDL_LockMutex(state->mutex);
        ++state->frames_rx;
        ++state->total_rx;
        state->payload_bytes += video_size;
        if (keyframe) {
            ++state->keyframes;
        }
        if (sps) {
            ++state->sps_frames;
        }
        if (pps) {
            ++state->pps_frames;
        }
        if (state->decoder != NULL) {
            grd_frame decoded;
            grd_error error = {0};
            const grd_status status = grd_decoder_decode(
                state->decoder,
                video,
                video_size,
                &decoded,
                &error
            );
            if (status == GRD_OK) {
                ++state->frames_decoded;
                ++state->total_decoded;
                if (state->frames_decoded == 1U) {
                    printf(
                        "[decode] first frame ready: id=%llu %ux%u key=%d\n",
                        (unsigned long long)frame_id,
                        decoded.width,
                        decoded.height,
                        keyframe ? 1 : 0
                    );
                    fflush(stdout);
                }
                grd_platform_frame_release(&decoded);
            } else if (status == GRD_WOULD_BLOCK) {
                ++state->wouldblock;
            } else {
                ++state->frames_failed;
                ++state->total_failed;
                if (state->decode_fail_logged < 5U) {
                    ++state->decode_fail_logged;
                    printf(
                        "[decode-fail] id=%llu key=%d: %s (code %d)\n",
                        (unsigned long long)frame_id,
                        keyframe ? 1 : 0,
                        error.message[0] != '\0' ? error.message : "unknown",
                        (int)error.code
                    );
                    fflush(stdout);
                }
            }
        } else {
            ++state->frames_failed;
            ++state->total_failed;
        }
        SDL_UnlockMutex(state->mutex);
        return false;
    }
    return false;
}

static void print_window(const diag_state *state, double seconds)
{
    printf(
        "[stats] %.1fs: rx %.1f fps, decoded %.1f fps, failed %llu, "
        "wb %llu, key %llu, sps %llu, pps %llu, avg %llu B/frame\n",
        seconds,
        (double)state->frames_rx / seconds,
        (double)state->frames_decoded / seconds,
        (unsigned long long)state->frames_failed,
        (unsigned long long)state->wouldblock,
        (unsigned long long)state->keyframes,
        (unsigned long long)state->sps_frames,
        (unsigned long long)state->pps_frames,
        state->frames_rx != 0U
            ? (unsigned long long)(state->payload_bytes / state->frames_rx)
            : 0ULL
    );
    fflush(stdout);
}

static int run_diag(
    const char *address,
    uint16_t port,
    const char *password,
    double total_seconds,
    diag_state *state
)
{
    grd_config config;
    grd_config_defaults(&config);
    grd_error error = {0};

    printf(
        "[grd-diag] connecting to %s:%u (password %s) for %.0f s\n",
        address,
        (unsigned)port,
        password[0] != '\0' ? "***" : "(empty)",
        total_seconds
    );
    fflush(stdout);

    grd_connection *control = grd_connect(
        address,
        port,
        password,
        GRD_ROLE_CONTROLLER,
        &config,
        diag_packet,
        state,
        &error
    );
    if (control == NULL) {
        fprintf(
            stderr,
            "Connection failed: %s (code %d)\n",
            error.message[0] != '\0' ? error.message : "unknown",
            (int)error.code
        );
        return 3;
    }
    state->control = control;
    grd_connection *media = grd_connect_media(
        address,
        port,
        password,
        &config,
        diag_packet,
        state,
        &error
    );
    if (media == NULL) {
        fprintf(
            stderr,
            "Media channel failed: %s (code %d)\n",
            error.message[0] != '\0' ? error.message : "unknown",
            (int)error.code
        );
        grd_connection_close(control);
        return 3;
    }
    for (unsigned attempt = 0U;
         attempt < 100U && !grd_connection_video_udp_active(media);
         ++attempt) {
        SDL_Delay(10U);
    }
    if (!grd_connection_video_udp_active(media)) {
        printf("[media] UDP video channel was not active within 1 s\n");
        fflush(stdout);
    }

    const uint64_t started = grd_now_micros();
    uint64_t last_stats = started;
    for (;;) {
        SDL_Delay(100U);
        const uint64_t now = grd_now_micros();
        if (now - started >= (uint64_t)(total_seconds * 1000000.0)) {
            break;
        }
        if (now - last_stats >= 5000000ULL) {
            const double window_seconds =
                (double)(now - last_stats) / 1000000.0;
            SDL_LockMutex(state->mutex);
            const diag_state window = *state;
            SDL_UnlockMutex(state->mutex);
            print_window(&window, window_seconds);
            last_stats = now;
            SDL_LockMutex(state->mutex);
            state->frames_rx = 0U;
            state->frames_decoded = 0U;
            state->frames_failed = 0U;
            state->wouldblock = 0U;
            state->keyframes = 0U;
            state->sps_frames = 0U;
            state->pps_frames = 0U;
            state->payload_bytes = 0U;
            SDL_UnlockMutex(state->mutex);
        }
    }

    SDL_LockMutex(state->mutex);
    const uint64_t total_rx = state->total_rx;
    const uint64_t total_decoded = state->total_decoded;
    const uint64_t total_failed = state->total_failed;
    const bool had_config = state->have_config;
    SDL_UnlockMutex(state->mutex);
    printf(
        "[verdict] config=%d rx=%llu decoded=%llu failed=%llu\n",
        had_config ? 1 : 0,
        (unsigned long long)total_rx,
        (unsigned long long)total_decoded,
        (unsigned long long)total_failed
    );
    fflush(stdout);

    grd_connection_close(media);
    grd_connection_close(control);
    SDL_LockMutex(state->mutex);
    grd_decoder_destroy(state->decoder);
    state->decoder = NULL;
    SDL_UnlockMutex(state->mutex);
    return total_decoded > 0U ? 0 : 1;
}

static bool selftest_host_callback(
    grd_packet_type type,
    const void *payload,
    size_t payload_length,
    bool payload_takeable,
    grd_role role,
    void *userdata
)
{
    (void)type;
    (void)payload;
    (void)payload_length;
    (void)payload_takeable;
    (void)role;
    (void)userdata;
    return false;
}

typedef struct selftest_broadcaster {
    grd_host *host;
    grd_encoder *encoder;
    grd_frame source;
    SDL_AtomicInt stopping;
} selftest_broadcaster;

static int selftest_broadcast_thread(void *userdata)
{
    selftest_broadcaster *ctx = userdata;
    uint64_t frame_counter = 0U;
    uint64_t last_config = 0U;
    grd_video_config configuration;
    memset(&configuration, 0, sizeof(configuration));
    configuration.width = ctx->source.width;
    configuration.height = ctx->source.height;
    configuration.fps = 30U;
    configuration.bitrate_kbps = 1000U;
    (void)snprintf(
        configuration.codec,
        sizeof(configuration.codec),
        "%s",
        grd_codec_name(GRD_CODEC_H264)
    );
    while (SDL_GetAtomicInt(&ctx->stopping) == 0) {
        if (grd_now_micros() - last_config >= 1000000ULL) {
            grd_error config_error = {0};
            (void)grd_host_broadcast(
                ctx->host,
                GRD_PACKET_VIDEO_CONFIG,
                &configuration,
                sizeof(configuration),
                &config_error
            );
            last_config = grd_now_micros();
        }
        grd_encoded_frame encoded;
        grd_error error = {0};
        grd_status status = GRD_WOULD_BLOCK;
        for (unsigned attempt = 0U;
             attempt < 10U && status == GRD_WOULD_BLOCK;
             ++attempt) {
            ctx->source.timestamp_micros = grd_now_micros();
            status = grd_encoder_encode(
                ctx->encoder, &ctx->source, &encoded, &error
            );
        }
        if (status == GRD_OK) {
            uint8_t prefix[DIAG_WIRE_PREFIX];
            memset(prefix, 0, sizeof(prefix));
            prefix[0] = (uint8_t)(encoded.timestamp_micros >> 56U);
            prefix[1] = (uint8_t)(encoded.timestamp_micros >> 48U);
            prefix[2] = (uint8_t)(encoded.timestamp_micros >> 40U);
            prefix[3] = (uint8_t)(encoded.timestamp_micros >> 32U);
            prefix[4] = (uint8_t)(encoded.timestamp_micros >> 24U);
            prefix[5] = (uint8_t)(encoded.timestamp_micros >> 16U);
            prefix[6] = (uint8_t)(encoded.timestamp_micros >> 8U);
            prefix[7] = (uint8_t)encoded.timestamp_micros;
            prefix[16U] = encoded.keyframe ? 1U : 0U;
            prefix[17U] = (uint8_t)(frame_counter >> 56U);
            prefix[18U] = (uint8_t)(frame_counter >> 48U);
            prefix[19U] = (uint8_t)(frame_counter >> 40U);
            prefix[20U] = (uint8_t)(frame_counter >> 32U);
            prefix[21U] = (uint8_t)(frame_counter >> 24U);
            prefix[22U] = (uint8_t)(frame_counter >> 16U);
            prefix[23U] = (uint8_t)(frame_counter >> 8U);
            prefix[24U] = (uint8_t)frame_counter;
            ++frame_counter;
            uint8_t *wire = malloc(DIAG_WIRE_PREFIX + encoded.size);
            if (wire != NULL) {
                memcpy(wire, prefix, DIAG_WIRE_PREFIX);
                memcpy(wire + DIAG_WIRE_PREFIX, encoded.data, encoded.size);
                (void)grd_host_broadcast(
                    ctx->host,
                    GRD_PACKET_VIDEO_FRAME,
                    wire,
                    DIAG_WIRE_PREFIX + encoded.size,
                    &error
                );
                free(wire);
            }
            grd_encoded_frame_release(&encoded);
        }
        SDL_Delay(33U);
    }
    return 0;
}

static int run_selftest(void)
{
    printf("[selftest] local host + software encoder + real stream\n");
    fflush(stdout);
    grd_config host_config;
    grd_config_defaults(&host_config);
    grd_error error = {0};
    if (grd_config_set_password(
            &host_config, "grd-diag-selftest", &error
        ) != GRD_OK) {
        fprintf(stderr, "Unable to set the self-test password\n");
        return 3;
    }
    grd_host *host = NULL;
    uint16_t host_port = 0U;
    for (uint16_t port = 49300U; port < 49400U && host == NULL; ++port) {
        host_config.port = port;
        host = grd_host_start(
            &host_config, selftest_host_callback, NULL, &error
        );
        if (host != NULL) {
            host_port = port;
        }
    }
    if (host == NULL) {
        fprintf(stderr, "Unable to start the self-test host\n");
        return 3;
    }
    const uint32_t width = 320U;
    const uint32_t height = 180U;
    grd_pipeline_kind active = GRD_PIPELINE_SOFTWARE;
    grd_video_codec active_codec = GRD_CODEC_H264;
    grd_encoder *encoder = grd_encoder_create(
        GRD_PIPELINE_INTEGRATED_SOFTWARE,
        width,
        height,
        30U,
        1000U,
        GRD_CODEC_H264,
        false,
        &active,
        &active_codec,
        &error
    );
    if (encoder == NULL) {
        fprintf(stderr, "Unable to create the self-test encoder\n");
        grd_host_stop(host);
        return 3;
    }
    selftest_broadcaster broadcaster;
    memset(&broadcaster, 0, sizeof(broadcaster));
    broadcaster.host = host;
    broadcaster.encoder = encoder;
    broadcaster.source.width = width;
    broadcaster.source.height = height;
    broadcaster.source.stride = width * 4U;
    broadcaster.source.size = (size_t)width * height * 4U;
    broadcaster.source.format = GRD_PIXEL_RGBA8;
    broadcaster.source.data = malloc(broadcaster.source.size);
    if (broadcaster.source.data == NULL) {
        grd_encoder_destroy(encoder);
        grd_host_stop(host);
        return 3;
    }
    for (uint32_t y = 0U; y < height; ++y) {
        for (uint32_t x = 0U; x < width; ++x) {
            const size_t offset = ((size_t)y * width + x) * 4U;
            broadcaster.source.data[offset] = (uint8_t)x;
            broadcaster.source.data[offset + 1U] = (uint8_t)y;
            broadcaster.source.data[offset + 2U] = (uint8_t)(x + y);
            broadcaster.source.data[offset + 3U] = 255U;
        }
    }
    SDL_SetAtomicInt(&broadcaster.stopping, 0);
    SDL_Thread *thread = SDL_CreateThread(
        selftest_broadcast_thread, "grd-diag-broadcast", &broadcaster
    );
    if (thread == NULL) {
        fprintf(stderr, "Unable to create the self-test broadcast thread\n");
        free(broadcaster.source.data);
        grd_encoder_destroy(encoder);
        grd_host_stop(host);
        return 3;
    }
    diag_state state;
    memset(&state, 0, sizeof(state));
    state.mutex = SDL_CreateMutex();
    if (state.mutex == NULL) {
        SDL_SetAtomicInt(&broadcaster.stopping, 1);
        SDL_WaitThread(thread, NULL);
        free(broadcaster.source.data);
        grd_encoder_destroy(encoder);
        grd_host_stop(host);
        return 3;
    }
    const int result = run_diag(
        "127.0.0.1", host_port, "grd-diag-selftest", 5.0, &state
    );
    SDL_SetAtomicInt(&broadcaster.stopping, 1);
    SDL_WaitThread(thread, NULL);
    SDL_DestroyMutex(state.mutex);
    free(broadcaster.source.data);
    grd_encoder_destroy(encoder);
    grd_host_stop(host);
    printf("[selftest] verdict: %s\n", result == 0 ? "OK" : "FAILED");
    return result;
}

/* Launches a real CUDA conversion kernel instead of merely checking whether
 * the driver can enumerate a device. This catches binaries whose embedded
 * kernel image was compiled for a different GPU architecture. */
static int run_cuda_selftest(void)
{
    enum {
        width = 32,
        height = 18,
        rgba_size = width * height * 4,
        nv12_size = width * height * 3 / 2
    };
    char adapter_name[128] = {0};
    if (!grd_cuda_available(adapter_name, sizeof(adapter_name))) {
        fprintf(stderr, "[cuda-selftest] CUDA device unavailable\n");
        return 3;
    }

    uint8_t rgba[rgba_size];
    uint8_t nv12[nv12_size];
    for (size_t index = 0U; index < sizeof(rgba); index += 4U) {
        rgba[index] = (uint8_t)(index / 4U);
        rgba[index + 1U] = (uint8_t)(index / 8U);
        rgba[index + 2U] = (uint8_t)(255U - index / 4U);
        rgba[index + 3U] = 255U;
    }
    memset(nv12, 0xCD, sizeof(nv12));

    grd_frame source;
    memset(&source, 0, sizeof(source));
    source.data = rgba;
    source.size = sizeof(rgba);
    source.width = width;
    source.height = height;
    source.stride = width * 4U;
    source.format = GRD_PIXEL_RGBA8;
    grd_error error = {0};
    const grd_status status = grd_cuda_rgba_to_nv12(
        &source,
        width,
        height,
        nv12,
        sizeof(nv12),
        &error
    );
    if (status != GRD_OK) {
        fprintf(
            stderr,
            "[cuda-selftest] kernel failed on %s: %s (code %d)\n",
            adapter_name[0] != '\0' ? adapter_name : "unknown adapter",
            error.message[0] != '\0' ? error.message : "unknown CUDA error",
            (int)status
        );
        return 1;
    }

    bool output_changed = false;
    for (size_t index = 0U; index < sizeof(nv12); ++index) {
        if (nv12[index] != 0xCDU) {
            output_changed = true;
            break;
        }
    }
    if (!output_changed) {
        fprintf(
            stderr,
            "[cuda-selftest] kernel returned unchanged output on %s\n",
            adapter_name
        );
        return 1;
    }

    printf("[cuda-selftest] CUDA kernel execution on %s: OK\n", adapter_name);
    return 0;
}

/* Exercises the Windows host path without changing the user's password or
 * opening a second network host. This deliberately requires native D3D11
 * capture plus CUDA hardware frames: a CPU fallback is reported as a failed
 * self-test instead of hiding a broken zero-copy path. */
static int run_capture_selftest(
    double total_seconds,
    uint32_t target_fps
)
{
#if !defined(_WIN32)
    (void)total_seconds;
    (void)target_fps;
    fprintf(stderr, "Capture self-test is available only on Windows\n");
    return 2;
#else
    grd_error error = {0};
    if (grd_platform_initialize(&error) != GRD_OK) {
        fprintf(
            stderr,
            "Platform initialization failed: %s\n",
            error.message[0] != '\0' ? error.message : "unknown error"
        );
        return 3;
    }
    if (grd_platform_validate_host(&error) != GRD_OK) {
        fprintf(
            stderr,
            "Host cannot be captured: %s\n",
            error.message[0] != '\0' ? error.message : "unknown error"
        );
        grd_platform_shutdown();
        return 3;
    }

    grd_monitor monitors[GRD_MAX_MONITORS];
    const size_t monitor_count =
        grd_platform_monitors(monitors, GRD_MAX_MONITORS);
    if (monitor_count == 0U) {
        fprintf(stderr, "No display is available for the capture self-test\n");
        grd_platform_shutdown();
        return 3;
    }
    size_t monitor_index = 0U;
    for (size_t index = 0U; index < monitor_count; ++index) {
        if (monitors[index].primary) {
            monitor_index = index;
            break;
        }
    }
    const grd_monitor *monitor = &monitors[monitor_index];
    const uint32_t encoder_width = monitor->width & ~1U;
    const uint32_t encoder_height = monitor->height & ~1U;
    grd_pipeline_kind active_pipeline = GRD_PIPELINE_SOFTWARE;
    grd_video_codec active_codec = GRD_CODEC_H264;
    grd_encoder *encoder = grd_encoder_create(
        GRD_PIPELINE_CUDA_NVENC,
        encoder_width,
        encoder_height,
        target_fps,
        12000U,
        GRD_CODEC_H264,
        false,
        &active_pipeline,
        &active_codec,
        &error
    );
    if (encoder == NULL || active_pipeline != GRD_PIPELINE_CUDA_NVENC) {
        fprintf(
            stderr,
            "CUDA/NVENC encoder is unavailable: %s\n",
            error.message[0] != '\0' ? error.message : "non-hardware fallback"
        );
        grd_encoder_destroy(encoder);
        grd_platform_shutdown();
        return 3;
    }

    printf(
        "[capture-selftest] monitor %s, %ux%u, codec %s, %u FPS, %.0f s\n",
        monitor->name,
        (unsigned)encoder_width,
        (unsigned)encoder_height,
        grd_codec_name(active_codec),
        target_fps,
        total_seconds
    );
    fflush(stdout);
    uint64_t captured_frames = 0U;
    uint64_t native_frames = 0U;
    uint64_t encoded_frames = 0U;
    uint64_t blocked_frames = 0U;
    uint64_t capture_errors = 0U;
    uint64_t encode_errors = 0U;
    diag_timing pipeline_timing = {0};
    diag_timing capture_timing = {0};
    diag_timing acquire_timing = {0};
    diag_timing conversion_timing = {0};
    diag_timing send_frame_timing = {0};
    diag_timing receive_packet_timing = {0};
    const uint64_t started = grd_now_micros();
    while (grd_now_micros() - started <
           (uint64_t)(total_seconds * 1000000.0)) {
        grd_frame captured;
        memset(&captured, 0, sizeof(captured));
        memset(&error, 0, sizeof(error));
        const uint64_t pipeline_started = grd_now_micros();
        const uint64_t capture_started = pipeline_started;
        const grd_status capture_status = grd_platform_capture(
            monitor->id, true, &captured, &error
        );
        const uint64_t capture_finished = grd_now_micros();
        grd_capture_timing platform_timing = {0};
        grd_platform_capture_last_timing(&platform_timing);
        if (platform_timing.acquire_attempted) {
            diag_timing_add(
                &acquire_timing, platform_timing.acquire_wait_micros
            );
        }
        if (capture_status == GRD_WOULD_BLOCK) {
            SDL_Delay(1U);
            continue;
        }
        if (capture_status != GRD_OK) {
            ++capture_errors;
            fprintf(
                stderr,
                "Capture failed: %s (code %d)\n",
                error.message[0] != '\0'
                    ? error.message
                    : "unknown error",
                (int)capture_status
            );
            break;
        }
        diag_timing_add(
            &capture_timing, capture_finished - capture_started
        );
        ++captured_frames;
        if (captured.format == GRD_PIXEL_D3D11_BGRA &&
            captured.owner != NULL && captured.data == NULL) {
            ++native_frames;
        }
        grd_encoded_frame encoded;
        const grd_status encode_status = grd_encoder_encode(
            encoder, &captured, &encoded, &error
        );
        const uint64_t encode_finished = grd_now_micros();
        diag_timing_add(
            &pipeline_timing, encode_finished - pipeline_started
        );
        grd_encoder_timing encoder_timing = {0};
        grd_encoder_last_timing(encoder, &encoder_timing);
        if (encoder_timing.conversion_recorded) {
            diag_timing_add(
                &conversion_timing, encoder_timing.conversion_micros
            );
        }
        if (encoder_timing.send_frame_recorded) {
            diag_timing_add(
                &send_frame_timing, encoder_timing.send_frame_micros
            );
        }
        if (encoder_timing.receive_packet_recorded) {
            diag_timing_add(
                &receive_packet_timing,
                encoder_timing.receive_packet_micros
            );
        }
        if (encode_status == GRD_OK) {
            ++encoded_frames;
            grd_encoded_frame_release(&encoded);
        } else if (encode_status == GRD_WOULD_BLOCK) {
            ++blocked_frames;
        } else {
            ++encode_errors;
            fprintf(
                stderr,
                "Encoding failed: %s (code %d)\n",
                error.message[0] != '\0'
                    ? error.message
                    : "unknown error",
                (int)encode_status
            );
            grd_platform_frame_release(&captured);
            break;
        }
        grd_platform_frame_release(&captured);
        /* Match the real default host cadence instead of stress-testing the
         * game and encoder at the monitor's full refresh rate. */
        const uint64_t next_frame_deadline =
            started + captured_frames * 1000000ULL / target_fps;
        const uint64_t frame_now = grd_now_micros();
        if (next_frame_deadline > frame_now) {
            SDL_DelayPrecise((next_frame_deadline - frame_now) * 1000ULL);
        }
    }
    const double elapsed =
        (double)(grd_now_micros() - started) / 1000000.0;
    grd_encoder_destroy(encoder);
    grd_platform_shutdown();
    const bool passed = captured_frames != 0U && encoded_frames != 0U &&
                        native_frames == captured_frames &&
                        capture_errors == 0U && encode_errors == 0U;
    printf(
        "[capture-selftest] cap=%llu (%.1f fps), native=%llu, "
        "encoded=%llu, blocked=%llu, caperr=%llu, encerr=%llu\n",
        (unsigned long long)captured_frames,
        elapsed > 0.0 ? (double)captured_frames / elapsed : 0.0,
        (unsigned long long)native_frames,
        (unsigned long long)encoded_frames,
        (unsigned long long)blocked_frames,
        (unsigned long long)capture_errors,
        (unsigned long long)encode_errors
    );
    printf(
        "[capture-selftest] zero-copy D3D11 -> CUDA -> NVENC: %s\n",
        passed ? "OK" : "FAILED"
    );
    printf(
        "[capture-selftest] timing us avg/max: pipeline %.1f/%llu, "
        "capture %.1f/%llu, acquire %.1f/%llu, convert %.1f/%llu, "
        "send %.1f/%llu, receive %.1f/%llu\n",
        diag_timing_average(&pipeline_timing),
        (unsigned long long)pipeline_timing.maximum_micros,
        diag_timing_average(&capture_timing),
        (unsigned long long)capture_timing.maximum_micros,
        diag_timing_average(&acquire_timing),
        (unsigned long long)acquire_timing.maximum_micros,
        diag_timing_average(&conversion_timing),
        (unsigned long long)conversion_timing.maximum_micros,
        diag_timing_average(&send_frame_timing),
        (unsigned long long)send_frame_timing.maximum_micros,
        diag_timing_average(&receive_packet_timing),
        (unsigned long long)receive_packet_timing.maximum_micros
    );
    return passed ? 0 : 1;
#endif
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) {
        return run_selftest();
    }
    if (argc == 2 && strcmp(argv[1], "--cuda-selftest") == 0) {
        return run_cuda_selftest();
    }
    if (argc >= 2 && strcmp(argv[1], "--capture-selftest") == 0) {
        const long seconds_value = argc > 2 ? strtol(argv[2], NULL, 10) : 5L;
        const long fps_value = argc > 3 ? strtol(argv[3], NULL, 10) : 120L;
        if (argc > 4 || seconds_value <= 0L || seconds_value > 60L ||
            fps_value < 30L || fps_value > 240L) {
            fprintf(
                stderr,
                "Invalid capture self-test parameters "
                "(seconds 1-60, FPS 30-240)\n"
            );
            return 2;
        }
        return run_capture_selftest(
            (double)seconds_value, (uint32_t)fps_value
        );
    }
    if (argc < 4) {
        fprintf(
            stderr,
            "Usage: grd-diag <address> <port> <password> [seconds=15]\n"
            "     grd-diag --selftest\n"
            "     grd-diag --cuda-selftest\n"
            "     grd-diag --capture-selftest [seconds=5] [fps=120]\n"
        );
        return 2;
    }
    const char *address = argv[1];
    const long port_value = strtol(argv[2], NULL, 10);
    const char *password = argv[3];
    const long seconds_value =
        argc > 4 ? strtol(argv[4], NULL, 10) : 15L;
    if (port_value <= 0 || port_value > 65535L ||
        seconds_value <= 0L || seconds_value > 600L) {
        fprintf(stderr, "Invalid port or duration\n");
        return 2;
    }
    diag_state state;
    memset(&state, 0, sizeof(state));
    state.mutex = SDL_CreateMutex();
    if (state.mutex == NULL) {
        fprintf(stderr, "Unable to create SDL mutex\n");
        return 3;
    }
    const int result = run_diag(
        address,
        (uint16_t)port_value,
        password,
        (double)seconds_value,
        &state
    );
    SDL_DestroyMutex(state.mutex);
    return result;
}
