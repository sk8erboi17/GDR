#include "grd/codec.h"
#include "grd/log.h"

#if defined(__APPLE__)
#include "grd/videotoolbox.h"
#endif
#if defined(_WIN32) && GRD_HAS_CUDA
#include "grd/cuda_d3d11.h"
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/imgutils.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include <libavutil/hwcontext.h>

#include <stdio.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define GRD_DECODER_FRAME_SLOTS 3U

static void *owned_avbuf_clone(const void *opaque)
{
    return av_buffer_ref((const AVBufferRef *)opaque);
}

static void owned_avbuf_release(void *opaque)
{
    AVBufferRef *ref = opaque;
    av_buffer_unref(&ref);
}

#if defined(__APPLE__)
static void plain_buffer_free(void *opaque, uint8_t *data)
{
    (void)opaque;
    free(data);
}
#endif

typedef struct grd_decoder_frame_slot {
    uint8_t *data;
    size_t capacity;
    _Atomic bool in_use;
    _Atomic bool orphaned;
} grd_decoder_frame_slot;

static void release_decoder_frame_slot(void *owner)
{
    grd_decoder_frame_slot *slot = owner;
    if (slot == NULL) {
        return;
    }
    (void)atomic_exchange_explicit(
        &slot->in_use, false, memory_order_release
    );
    if (atomic_load_explicit(&slot->orphaned, memory_order_acquire)) {
        free(slot->data);
        free(slot);
    }
}

static grd_decoder_frame_slot *acquire_decoder_frame_slot(
    grd_decoder_frame_slot *const slots[GRD_DECODER_FRAME_SLOTS]
)
{
    for (size_t index = 0U; index < GRD_DECODER_FRAME_SLOTS; ++index) {
        grd_decoder_frame_slot *slot = slots[index];
        if (slot == NULL) {
            continue;
        }
        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(
                &slot->in_use,
                &expected,
                true,
                memory_order_acquire,
                memory_order_relaxed
            )) {
            return slot;
        }
    }
    return NULL;
}

struct grd_encoder {
    AVCodecContext *context;
    AVFrame *frame;
    AVPacket *packet;
    struct SwsContext *converter;
    uint32_t width;
    uint32_t height;
    int64_t next_pts;
    grd_pipeline_kind pipeline;
    uint8_t *cuda_nv12;
    bool cuda_conversion;
    bool cuda_warning_emitted;
    bool d3d11_zero_copy_logged;
    grd_cuda_converter *cuda_converter;
    bool force_keyframe;
    uint32_t bitrate_kbps;
    grd_video_codec codec;
#if defined(__APPLE__)
    grd_videotoolbox_encoder *videotoolbox;
#endif
#if GRD_HAS_CUDA
    bool cuda_hardware_frames;
    AVBufferRef *cuda_device_context;
    AVBufferRef *cuda_frames_context;
    /* Double-buffered pool frames: the RGBA→NV12 conversion of frame N runs
     * on the GPU while NVENC encodes frame N-1, hiding the conversion behind
     * the encode instead of serializing them. */
    AVFrame *cuda_frames[2];
    unsigned cuda_frame_index;
    bool cuda_packet_pending;
    uint64_t cuda_pending_timestamp;
#endif
    /* SPS/PPS captured from the first IDR (Annex-B, without start codes):
     * NVENC emits them only on the first IDR (repeatSPSPPS=0) and leaves
     * extradata empty without GLOBAL_HEADER, so later keyframes would have
     * nothing for the client to bootstrap from. */
    uint8_t *stored_sps;
    size_t stored_sps_size;
    uint8_t *stored_pps;
    size_t stored_pps_size;
    bool sps_capture_logged;
    bool sps_injection_logged;
    bool sps_present_logged;
    grd_encoder_timing last_timing;
};

static uint32_t read_be32_nal(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) |
           ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) |
           (uint32_t)data[3];
}

static bool packet_is_annexb(const uint8_t *data, size_t size)
{
    for (size_t index = 0U; index + 3U <= size; ++index) {
        if (data[index] == 0U && data[index + 1U] == 0U &&
            (data[index + 2U] == 1U ||
             (index + 4U <= size && data[index + 3U] == 1U))) {
            return true;
        }
    }
    return false;
}

/* True when the packet already contains an SPS (type 7) in either Annex-B
 * or AVCC form. */
static bool packet_has_sps(const uint8_t *data, size_t size)
{
    if (packet_is_annexb(data, size)) {
        size_t offset = 0U;
        while (offset + 3U <= size) {
            if (data[offset] != 0U || data[offset + 1U] != 0U) {
                ++offset;
                continue;
            }
            size_t prefix = 0U;
            if (data[offset + 2U] == 1U) {
                prefix = 3U;
            } else if (offset + 4U <= size && data[offset + 3U] == 1U) {
                prefix = 4U;
            }
            if (prefix != 0U) {
                const size_t nal_start = offset + prefix;
                if (nal_start < size &&
                    (data[nal_start] & 0x1FU) == 7U) {
                    return true;
                }
                offset = nal_start;
                continue;
            }
            ++offset;
        }
        return false;
    }
    size_t position = 0U;
    while (position + 4U <= size) {
        const uint32_t length = read_be32_nal(data + position);
        position += 4U;
        if (length == 0U || (size_t)length > size - position) {
            break;
        }
        if ((data[position] & 0x1FU) == 7U) {
            return true;
        }
        position += (size_t)length;
    }
    return false;
}

static void store_parameter_set(
    uint8_t **storage,
    size_t *storage_size,
    const uint8_t *data,
    size_t length
)
{
    if (length == 0U) {
        return;
    }
    free(*storage);
    *storage = malloc(length);
    if (*storage != NULL) {
        memcpy(*storage, data, length);
        *storage_size = length;
    } else {
        *storage_size = 0U;
    }
}

/* Captures SPS/PPS from the first IDR, handling both Annex-B (start codes)
 * and AVCC (4-byte lengths) packets: some NVENC builds emit AVCC, in which
 * case a start-code-only scan finds nothing and the injection never fires. */
static void capture_parameter_sets(grd_encoder *encoder)
{
    const uint8_t *data = encoder->packet->data;
    const size_t size = (size_t)encoder->packet->size;
    if (packet_is_annexb(data, size)) {
        size_t offset = 0U;
        while (offset + 3U <= size) {
            if (data[offset] != 0U || data[offset + 1U] != 0U) {
                ++offset;
                continue;
            }
            size_t prefix = 0U;
            if (data[offset + 2U] == 1U) {
                prefix = 3U;
            } else if (offset + 4U <= size && data[offset + 3U] == 1U) {
                prefix = 4U;
            }
            if (prefix == 0U) {
                ++offset;
                continue;
            }
            const size_t nal_start = offset + prefix;
            size_t nal_end = nal_start;
            while (nal_end + 3U <= size &&
                   !(data[nal_end] == 0U && data[nal_end + 1U] == 0U &&
                     data[nal_end + 2U] == 1U)) {
                ++nal_end;
            }
            if (nal_end > nal_start) {
                const uint8_t type = data[nal_start] & 0x1FU;
                if (type == 7U) {
                    store_parameter_set(
                        &encoder->stored_sps,
                        &encoder->stored_sps_size,
                        data + nal_start,
                        nal_end - nal_start
                    );
                } else if (type == 8U) {
                    store_parameter_set(
                        &encoder->stored_pps,
                        &encoder->stored_pps_size,
                        data + nal_start,
                        nal_end - nal_start
                    );
                }
            }
            offset = nal_end;
        }
    } else {
        size_t position = 0U;
        while (position + 4U <= size) {
            const uint32_t length = read_be32_nal(data + position);
            position += 4U;
            if (length == 0U || (size_t)length > size - position) {
                break;
            }
            const uint8_t type = data[position] & 0x1FU;
            if (type == 7U) {
                store_parameter_set(
                    &encoder->stored_sps,
                    &encoder->stored_sps_size,
                    data + position,
                    (size_t)length
                );
            } else if (type == 8U) {
                store_parameter_set(
                    &encoder->stored_pps,
                    &encoder->stored_pps_size,
                    data + position,
                    (size_t)length
                );
            }
            position += (size_t)length;
        }
    }
    if (encoder->stored_sps != NULL && encoder->stored_pps != NULL &&
        encoder->stored_sps_size != 0U && encoder->stored_pps_size != 0U) {
        if (!encoder->sps_capture_logged) {
            encoder->sps_capture_logged = true;
            GRD_INFO(
                "host: SPS/PPS captured from the first IDR "
                "(SPS %zu B, PPS %zu B)",
                encoder->stored_sps_size,
                encoder->stored_pps_size
            );
        }
    }
}

/* NVENC does not repeat SPS/PPS in-band by default (repeatSPSPPS=0, and
 * h264_nvenc on some builds ignores repeat_headers): SPS/PPS appear only on
 * the first IDR and the AVCDecoderConfigurationRecord extradata is only
 * populated with GLOBAL_HEADER (which we do not use). A client joining after
 * the first IDR therefore never receives SPS/PPS and every decoder fails
 * with 'Invalid data'. This captures the parameter sets from the first IDR
 * and prepends them to every later keyframe that lacks them, converting AVCC
 * keyframes to Annex-B so the resulting stream is self-consistent. */
static void prepend_parameter_sets(grd_encoder *encoder)
{
    AVCodecContext *context = encoder->context;
    if (context == NULL || (encoder->packet->flags & AV_PKT_FLAG_KEY) == 0) {
        return;
    }
    capture_parameter_sets(encoder);
    const uint8_t *packet_data = encoder->packet->data;
    const size_t packet_size = (size_t)encoder->packet->size;
    if (packet_has_sps(packet_data, packet_size)) {
        /* Some NVENC builds repeat SPS/PPS on every IDR despite
         * repeat_headers being ignored: log it once per encoder session so
         * the server log can tell "already in-band" apart from a missing
         * injection call. */
        if (!encoder->sps_present_logged) {
            encoder->sps_present_logged = true;
            GRD_INFO(
                "host: keyframe already contains in-band SPS/PPS "
                "(no injection required)"
            );
        }
        return;
    }
    uint8_t *stored_sps = encoder->stored_sps;
    size_t stored_sps_size = encoder->stored_sps_size;
    uint8_t *stored_pps = encoder->stored_pps;
    size_t stored_pps_size = encoder->stored_pps_size;
    if (stored_sps == NULL || stored_sps_size == 0U ||
        stored_pps == NULL || stored_pps_size == 0U) {
        static _Atomic uint64_t no_sps_warn_micros;
        const uint64_t now_warn = grd_now_micros();
        uint64_t last_warn = atomic_load_explicit(
            &no_sps_warn_micros, memory_order_relaxed
        );
        if (now_warn - last_warn >= 2000000ULL &&
            atomic_compare_exchange_strong_explicit(
                &no_sps_warn_micros,
                &last_warn,
                now_warn,
                memory_order_relaxed,
                memory_order_relaxed
            )) {
            GRD_WARN(
                "host: keyframe has no SPS/PPS and no parameter set was captured "
                "(packet %d byte)",
                encoder->packet->size
            );
        }
        return;
    }
    const bool annexb = packet_is_annexb(packet_data, packet_size);
    const size_t prefix_size =
        4U + stored_sps_size + 4U + stored_pps_size;
    /* AVCC keyframes are converted to Annex-B so the prepended start codes
     * do not mix with 4-byte lengths. */
    const size_t combined_capacity =
        prefix_size + packet_size + (annexb ? 0U : packet_size / 4U);
    uint8_t *combined = malloc(combined_capacity);
    if (combined == NULL) {
        return;
    }
    static const uint8_t start_code[4] = {0U, 0U, 0U, 1U};
    size_t offset = 0U;
    memcpy(combined + offset, start_code, sizeof(start_code));
    offset += sizeof(start_code);
    memcpy(combined + offset, stored_sps, stored_sps_size);
    offset += stored_sps_size;
    memcpy(combined + offset, start_code, sizeof(start_code));
    offset += sizeof(start_code);
    memcpy(combined + offset, stored_pps, stored_pps_size);
    offset += stored_pps_size;
    if (annexb) {
        memcpy(combined + offset, packet_data, packet_size);
        offset += packet_size;
    } else {
        size_t position = 0U;
        while (position + 4U <= packet_size) {
            const uint32_t length = read_be32_nal(packet_data + position);
            position += 4U;
            if (length == 0U || (size_t)length > packet_size - position) {
                break;
            }
            memcpy(combined + offset, start_code, sizeof(start_code));
            offset += sizeof(start_code);
            memcpy(combined + offset, packet_data + position, (size_t)length);
            offset += (size_t)length;
            position += (size_t)length;
        }
    }
    AVBufferRef *new_buffer = av_buffer_alloc(offset);
    if (new_buffer != NULL) {
        memcpy(new_buffer->data, combined, offset);
        av_buffer_unref(&encoder->packet->buf);
        encoder->packet->buf = new_buffer;
        encoder->packet->data = new_buffer->data;
        encoder->packet->size = (int)offset;
        if (!encoder->sps_injection_logged) {
            encoder->sps_injection_logged = true;
            GRD_INFO(
                "host: SPS/PPS injected into keyframe "
                "(SPS %zu B, PPS %zu B, %s)",
                stored_sps_size,
                stored_pps_size,
                annexb ? "Annex-B" : "AVCC->Annex-B"
            );
        }
    }
    free(combined);
}

struct grd_decoder {
    AVCodecContext *context;
    AVFrame *frame;
    AVPacket *packet;
    struct SwsContext *converter;
    grd_pipeline_kind pipeline;
#if defined(__APPLE__)
    grd_videotoolbox_decoder *videotoolbox;
#endif
#if defined(_WIN32) && GRD_HAS_CUDA
    grd_cuda_d3d11_uploader *d3d11_uploader;
#endif
    AVBufferRef *cuda_device_context;
    AVFrame *cuda_transfer_frame;
    grd_decoder_frame_slot *frame_slots[GRD_DECODER_FRAME_SLOTS];
};

static void set_ffmpeg_error(
    grd_error *error,
    grd_status status,
    const char *prefix,
    int ffmpeg_error
)
{
    if (error == NULL) {
        return;
    }
    char detail[128];
    av_strerror(ffmpeg_error, detail, sizeof(detail));
    error->code = status;
    (void)snprintf(error->message, sizeof(error->message), "%s: %s", prefix, detail);
}

static void set_codec_option(
    AVCodecContext *context,
    const char *name,
    const char *value
)
{
    const int result = av_opt_set(context->priv_data, name, value, 0);
    if (result < 0) {
        char detail[96];
        (void)av_strerror(result, detail, sizeof(detail));
        GRD_WARN(
            "Encoder option %s=%s was not applied: %s",
            name,
            value,
            detail
        );
    }
}

#if GRD_HAS_CUDA
static bool configure_cuda_frames(grd_encoder *encoder)
{
    AVBufferRef *device = NULL;
    int result = av_hwdevice_ctx_create(
        &device,
        AV_HWDEVICE_TYPE_CUDA,
        NULL,
        NULL,
        0
    );
    if (result < 0) {
        return false;
    }
    AVBufferRef *frames = av_hwframe_ctx_alloc(device);
    if (frames == NULL) {
        av_buffer_unref(&device);
        return false;
    }
    AVHWFramesContext *frames_context =
        (AVHWFramesContext *)frames->data;
    frames_context->format = AV_PIX_FMT_CUDA;
    frames_context->sw_format = AV_PIX_FMT_NV12;
    frames_context->width = (int)encoder->width;
    frames_context->height = (int)encoder->height;
    frames_context->initial_pool_size = 8;
    result = av_hwframe_ctx_init(frames);
    if (result < 0) {
        av_buffer_unref(&frames);
        av_buffer_unref(&device);
        return false;
    }
    AVBufferRef *context_ref = av_buffer_ref(frames);
    if (context_ref == NULL) {
        av_buffer_unref(&frames);
        av_buffer_unref(&device);
        return false;
    }
    encoder->context->hw_frames_ctx = context_ref;
    encoder->context->pix_fmt = AV_PIX_FMT_CUDA;
    encoder->cuda_device_context = device;
    encoder->cuda_frames_context = frames;
    encoder->cuda_hardware_frames = true;
    return true;
}
#endif

static const AVCodec *software_encoder(void)
{
    static const char *names[] = {"libopenh264", "libx264", "h264"};
    for (size_t index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        const AVCodec *codec = avcodec_find_encoder_by_name(names[index]);
        if (codec != NULL) {
            return codec;
        }
    }
    return NULL;
}

const char *grd_codec_name(grd_video_codec codec)
{
    switch (codec) {
    case GRD_CODEC_HEVC: return "hevc";
    case GRD_CODEC_AV1: return "av1";
    default: return "h264";
    }
}

uint8_t grd_qp_from_quality(uint32_t quality)
{
    const uint64_t converted =
        ((uint64_t)quality + (uint64_t)(FF_QP2LAMBDA / 2U)) /
        (uint64_t)FF_QP2LAMBDA;
    return (uint8_t)(converted < 51U ? converted : 51U);
}

static const AVCodec *find_codec_encoder(
    grd_pipeline_kind requested,
    grd_video_codec codec
)
{
    if (codec == GRD_CODEC_HEVC) {
        if (requested == GRD_PIPELINE_METAL_VIDEOTOOLBOX) {
            const AVCodec *vt = avcodec_find_encoder_by_name("hevc_videotoolbox");
            if (vt != NULL) {
                return vt;
            }
        }
        if (requested == GRD_PIPELINE_CUDA_NVENC ||
            requested == GRD_PIPELINE_CUDA_SOFTWARE) {
            const AVCodec *nvenc = avcodec_find_encoder_by_name("hevc_nvenc");
            if (nvenc != NULL) {
                return nvenc;
            }
        }
        /* On the hardware-only NVENC pipeline, a missing HEVC encoder must
         * NOT fall back to slow software HEVC: the caller's candidate chain
         * moves to H.264 instead. Software HEVC stays available for the
         * CUDA_SOFTWARE / INTEGRATED_SOFTWARE pipelines. */
        if (requested == GRD_PIPELINE_CUDA_NVENC) {
            return NULL;
        }
        const AVCodec *software = avcodec_find_encoder_by_name("libx265");
        if (software == NULL) {
            software = avcodec_find_encoder_by_name("hevc");
        }
        return software;
    }
    if (codec == GRD_CODEC_AV1) {
        if (requested == GRD_PIPELINE_CUDA_NVENC ||
            requested == GRD_PIPELINE_CUDA_SOFTWARE) {
            const AVCodec *nvenc = avcodec_find_encoder_by_name("av1_nvenc");
            if (nvenc != NULL) {
                return nvenc;
            }
        }
        /* Same rule as HEVC: never software AV1 on the NVENC pipeline. */
        if (requested == GRD_PIPELINE_CUDA_NVENC) {
            return NULL;
        }
        const AVCodec *software = avcodec_find_encoder_by_name("libsvtav1");
        if (software == NULL) {
            software = avcodec_find_encoder_by_name("av1");
        }
        return software;
    }
    if (requested == GRD_PIPELINE_METAL_VIDEOTOOLBOX) {
        const AVCodec *vt = avcodec_find_encoder_by_name("h264_videotoolbox");
        if (vt != NULL) {
            return vt;
        }
    }
    if (requested == GRD_PIPELINE_CUDA_NVENC ||
        requested == GRD_PIPELINE_CUDA_SOFTWARE) {
        const AVCodec *nvenc = avcodec_find_encoder_by_name("h264_nvenc");
        if (nvenc != NULL) {
            return nvenc;
        }
    }
    return software_encoder();
}

static const AVCodec *select_encoder(
    grd_pipeline_kind requested,
    grd_video_codec requested_codec,
    grd_pipeline_kind *active,
    grd_video_codec *active_codec
)
{
    grd_video_codec candidates[3];
    size_t candidate_count = 0U;
    candidates[candidate_count++] = requested_codec;
    if (requested_codec == GRD_CODEC_AV1) {
        candidates[candidate_count++] = GRD_CODEC_HEVC;
    }
    if (requested_codec != GRD_CODEC_H264) {
        candidates[candidate_count++] = GRD_CODEC_H264;
    }
    const AVCodec *codec = NULL;
    for (size_t index = 0U; index < candidate_count && codec == NULL;
         ++index) {
        codec = find_codec_encoder(requested, candidates[index]);
        if (codec != NULL) {
            *active_codec = candidates[index];
        }
    }
    if (codec == NULL) {
        codec = software_encoder();
        *active_codec = GRD_CODEC_H264;
    }
    const char *name = codec != NULL ? codec->name : "";
    if (strstr(name, "nvenc") != NULL) {
        *active = GRD_PIPELINE_CUDA_NVENC;
    } else if (strstr(name, "videotoolbox") != NULL) {
        *active = GRD_PIPELINE_METAL_VIDEOTOOLBOX;
    } else {
        *active = requested == GRD_PIPELINE_CUDA_NVENC ||
                          requested == GRD_PIPELINE_CUDA_SOFTWARE
                      ? GRD_PIPELINE_CUDA_SOFTWARE
                      : GRD_PIPELINE_INTEGRATED_SOFTWARE;
    }
    return codec;
}

uint32_t grd_client_decode_caps(void)
{
    uint32_t caps = GRD_VIDEO_CAPS_H264;
    if (avcodec_find_decoder(AV_CODEC_ID_HEVC) != NULL) {
        caps |= GRD_VIDEO_CAPS_HEVC;
    }
    if (avcodec_find_decoder(AV_CODEC_ID_AV1) != NULL) {
        caps |= GRD_VIDEO_CAPS_AV1;
    }
    return caps;
}

bool grd_codec_encoder_available(
    grd_pipeline_kind pipeline,
    grd_video_codec codec
)
{
    grd_pipeline_kind active = GRD_PIPELINE_SOFTWARE;
    grd_video_codec active_codec = GRD_CODEC_H264;
    return select_encoder(pipeline, codec, &active, &active_codec) != NULL;
}

grd_encoder *grd_encoder_create(
    grd_pipeline_kind requested_pipeline,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint32_t bitrate_kbps,
    grd_video_codec requested_codec,
    bool pixel_444,
    grd_pipeline_kind *active_pipeline,
    grd_video_codec *active_codec,
    grd_error *error
)
{
    if (width == 0U || height == 0U || (width & 1U) != 0U ||
        (height & 1U) != 0U || fps == 0U) {
        if (error != NULL) {
            error->code = GRD_INVALID_ARGUMENT;
            (void)snprintf(error->message, sizeof(error->message), "Invalid video dimensions");
        }
        return NULL;
    }

    grd_encoder *encoder = calloc(1U, sizeof(*encoder));
    if (encoder == NULL) {
        return NULL;
    }
#if defined(__APPLE__)
    grd_error native_error = {0};
    if (requested_pipeline == GRD_PIPELINE_METAL_VIDEOTOOLBOX &&
        requested_codec == GRD_CODEC_H264) {
        encoder->videotoolbox = grd_videotoolbox_encoder_create(
            width, height, fps, bitrate_kbps, &native_error
        );
        if (encoder->videotoolbox != NULL) {
            encoder->pipeline = GRD_PIPELINE_METAL_VIDEOTOOLBOX;
        }
    }
#endif
    const AVCodec *codec = select_encoder(
        requested_pipeline,
        requested_codec,
        &encoder->pipeline,
        &encoder->codec
    );
    if (active_codec != NULL) {
        *active_codec = encoder->codec;
    }
    if (codec == NULL) {
#if defined(__APPLE__)
        if (encoder->videotoolbox != NULL) {
            if (active_pipeline != NULL) {
                *active_pipeline = GRD_PIPELINE_METAL_VIDEOTOOLBOX;
            }
            return encoder;
        }
#endif
        free(encoder);
        if (error != NULL) {
            error->code = GRD_NOT_SUPPORTED;
            (void)snprintf(error->message, sizeof(error->message), "No H.264 encoder is available");
        }
        return NULL;
    }
    encoder->context = avcodec_alloc_context3(codec);
    encoder->frame = av_frame_alloc();
    encoder->packet = av_packet_alloc();
    encoder->width = width;
    encoder->height = height;
    if (encoder->context == NULL || encoder->frame == NULL || encoder->packet == NULL) {
        grd_encoder_destroy(encoder);
        return NULL;
    }

    encoder->context->width = (int)width;
    encoder->context->height = (int)height;
#if defined(__APPLE__)
    if (encoder->videotoolbox != NULL) {
        encoder->pipeline = GRD_PIPELINE_METAL_VIDEOTOOLBOX;
    }
#endif
    /* NVENC accepts CPU YUV frames as well. The optional CUDA preprocessing
     * path is only enabled when the runtime module is actually present; this
     * keeps the hardware encoder usable in distro builds compiled without
     * nvcc (the normal Ubuntu installer configuration). */
    encoder->cuda_conversion =
        (encoder->pipeline == GRD_PIPELINE_CUDA_NVENC ||
         encoder->pipeline == GRD_PIPELINE_CUDA_SOFTWARE) &&
        grd_cuda_available(NULL, 0U);
    if (encoder->cuda_conversion) {
        encoder->cuda_converter = grd_cuda_converter_create();
        if (encoder->cuda_converter == NULL) {
            encoder->cuda_conversion = false;
        }
    }
    encoder->context->pix_fmt =
        encoder->cuda_conversion ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    if (pixel_444) {
        /* 4:4:4 keeps chroma detail (text/UI) at the cost of ~1/3 more
         * data; only used when the selected encoder actually supports it,
         * otherwise the stream silently stays 4:2:0. */
        bool supports_444 = false;
#if LIBAVCODEC_VERSION_MAJOR >= 61
        const void *supported_formats = NULL;
        int supported_format_count = 0;
        if (avcodec_get_supported_config(
                encoder->context,
                codec,
                AV_CODEC_CONFIG_PIX_FORMAT,
                0U,
                &supported_formats,
                &supported_format_count
            ) == 0 && supported_format_count > 0) {
            const enum AVPixelFormat *formats =
                (const enum AVPixelFormat *)supported_formats;
            for (int index = 0; index < supported_format_count; ++index) {
                if (formats[index] == AV_PIX_FMT_YUV444P) {
                    supports_444 = true;
                    break;
                }
            }
        }
#else
        /* FFmpeg < 7 (Ubuntu 24.04 ships libavcodec 60): use the deprecated
         * pix_fmts list, still present in these versions. */
        if (codec->pix_fmts != NULL) {
            for (size_t index = 0U; codec->pix_fmts[index] != AV_PIX_FMT_NONE;
                 ++index) {
                if (codec->pix_fmts[index] == AV_PIX_FMT_YUV444P) {
                    supports_444 = true;
                    break;
                }
            }
        }
#endif
        if (supports_444) {
            encoder->context->pix_fmt = AV_PIX_FMT_YUV444P;
            /* The CUDA converter only produces NV12; force the swscale path. */
            encoder->cuda_conversion = false;
        } else {
            GRD_WARN("Encoder %s does not support 4:4:4; using 4:2:0", codec->name);
        }
    }
#if GRD_HAS_CUDA
    if (encoder->pipeline == GRD_PIPELINE_CUDA_NVENC &&
        encoder->cuda_conversion) {
        (void)configure_cuda_frames(encoder);
    }
#endif
    encoder->context->time_base = (AVRational){1, (int)fps};
    encoder->context->framerate = (AVRational){(int)fps, 1};
    encoder->context->bit_rate = (int64_t)bitrate_kbps * 1000LL;
    encoder->bitrate_kbps = bitrate_kbps;
    /* Give the encoder 50% headroom over the average and a slightly larger
     * VBV: fast panning (gliding over a map, quick turns) needs bursts of
     * bits, and a strict CBR ceiling quantizes those frames into grain. */
    encoder->context->rc_max_rate = encoder->context->bit_rate * 3LL / 2LL;
    encoder->context->rc_min_rate = encoder->context->bit_rate;
    encoder->context->rc_buffer_size =
        (int)(encoder->context->bit_rate * 3LL / (int64_t)fps);
    encoder->context->gop_size = (int)(fps * 2U);
    encoder->context->max_b_frames = 0;
    encoder->context->flags |= AV_CODEC_FLAG_LOW_DELAY;

    if (encoder->pipeline == GRD_PIPELINE_CUDA_NVENC) {
        /* P3/low-latency is the quality-oriented real-time point for game
         * streaming. It retains ample 1080p120 throughput on supported
         * NVENC generations while avoiding the large quality loss of the
         * former P1/ultra-low-latency combination. No B-frames or lookahead
         * are enabled, so this does not add a queued frame of latency. */
        set_codec_option(encoder->context, "preset", "p3");
        set_codec_option(encoder->context, "tune", "ll");
        set_codec_option(encoder->context, "rc", "cbr");
        set_codec_option(encoder->context, "delay", "0");
        set_codec_option(encoder->context, "rc-lookahead", "0");
        /* H.264 High enables CABAC and the more efficient coding tools that
         * Main/Baseline leave unused. It improves detail at the same wire
         * budget without lookahead, extra frames of latency or additional
         * pacer pressure. The 4:4:4 profile remains encoder-selected. */
        if (encoder->codec == GRD_CODEC_H264 && !pixel_444) {
            set_codec_option(encoder->context, "profile", "high");
        }
        /* AV_PICTURE_TYPE_I alone may be encoded as a non-IDR I-frame by
         * NVENC. Recovery needs an IDR that resets every H.264 reference on
         * VideoToolbox, so make forced intra frames true IDRs. */
        set_codec_option(encoder->context, "forced-idr", "1");
        /* AQ redistributes the same frame budget towards perceptually useful
         * regions and stable detailed references. It improves HUD/text and
         * textured game scenes without lookahead or frame reordering. */
        set_codec_option(encoder->context, "temporal-aq", "1");
        set_codec_option(encoder->context, "spatial-aq", "1");
        set_codec_option(encoder->context, "multipass", "disabled");
        /* NVENC intra-refresh is DISABLED on purpose: with intra refresh
         * active, a forced keyframe may not be emitted as an IDR with
         * SPS/PPS in-band, so clients whose decoder needs the parameter sets
         * (VideoToolbox creates its session from SPS/PPS) never start.
         * Recovery therefore remains receiver-driven through NACK and an
         * explicit forced IDR. A 10 s safety GOP replaces the old 2 s GOP,
         * avoiding five large periodic bursts while preserving a bounded
         * fallback if feedback itself is lost. Every IDR carries SPS/PPS,
         * the pacer's keyframe admission bounds the burst, and FEC protects
         * it. */
        /* This FFmpeg NVENC wrapper does not expose repeat_headers. The
         * packet path below caches SPS/PPS from the first IDR and prepends
         * them to later keyframes when NVENC does not include them itself. */
        encoder->context->gop_size = (int)(fps * 10U);
        /* CBR with a single-frame VBV: limit a forced/safety keyframe burst
         * to 2x so it does not visibly jump in sharpness and still fits the
         * pacer's keyframe window. */
        set_codec_option(encoder->context, "ldkfs", "2");
        /* Tight rate control so the pacer can actually carry the stream:
         * a 3-frame VBV lets NVENC burst 3 frames' worth of bits, and every
         * oversize frame is then dropped by the pacer admission control
         * (observed: 50-78% drops at 1080p60/10 Mbps). One-frame VBV with
         * a small headroom keeps each frame inside its wire slot; ldkfs
         * above still lets scene-change keyframes burst. */
        encoder->context->rc_max_rate =
            encoder->context->bit_rate * 11LL / 10LL;
        encoder->context->rc_buffer_size =
            (int)(encoder->context->bit_rate / (int64_t)fps);
        GRD_INFO(
            "NVENC host low-latency quality: preset=p3, tune=ll, "
            "multipass=disabled, spatial-aq=1, temporal-aq=1, gop=10s%s",
            encoder->codec == GRD_CODEC_H264 && !pixel_444
                ? ", h264-profile=high"
                : ""
        );
    } else if (encoder->pipeline == GRD_PIPELINE_METAL_VIDEOTOOLBOX) {
        set_codec_option(encoder->context, "realtime", "1");
        set_codec_option(encoder->context, "allow_sw", "0");
        set_codec_option(encoder->context, "profile", "main");
    } else {
        set_codec_option(encoder->context, "preset", "ultrafast");
        /* zerolatency already configures x264 with sliced threads and zero
         * sync lookahead. Recent FFmpeg wrappers no longer expose the old
         * private sliced-threads/sync-lookahead AVOptions, so setting them
         * separately only emitted misleading "Option not found" warnings. */
        set_codec_option(encoder->context, "tune", "zerolatency");
        set_codec_option(encoder->context, "rc-lookahead", "0");
    }

    int result = avcodec_open2(encoder->context, codec, NULL);
    if (result < 0 && encoder->pipeline != GRD_PIPELINE_INTEGRATED_SOFTWARE) {
        if (encoder->codec != GRD_CODEC_H264) {
            /* Hardware codec failed to open: try the other hardware codec
             * (H.264) on the same pipeline before falling back. Slow
             * software HEVC/AV1 is never used for gaming. */
            GRD_WARN(
                "Encoder %s failed to start; retrying H.264 on the same "
                "pipeline hardware",
                codec->name
            );
            grd_encoder_destroy(encoder);
            return grd_encoder_create(
                requested_pipeline,
                width,
                height,
                fps,
                bitrate_kbps,
                GRD_CODEC_H264,
                pixel_444,
                active_pipeline,
                active_codec,
                error
            );
        }
        GRD_WARN(
            "Hardware H.264 encoder failed to start; falling back to software H.264"
        );
        grd_encoder_destroy(encoder);
        return grd_encoder_create(
            GRD_PIPELINE_INTEGRATED_SOFTWARE,
            width,
            height,
            fps,
            bitrate_kbps,
            GRD_CODEC_H264,
            pixel_444,
            active_pipeline,
            active_codec,
            error
        );
    }
    if (result < 0) {
        set_ffmpeg_error(error, GRD_ERROR, "Opening encoder", result);
        grd_encoder_destroy(encoder);
        return NULL;
    }
    /* Most FFmpeg encoders read rate-control parameters at open. NVENC is
     * handled explicitly by grd_encoder_set_bitrate through FFmpeg's live
     * reconfiguration fields; unsupported encoders report NOT_SUPPORTED so
     * the stream controller keeps both encoder and wire budgets stable.
     * Silently changing only the pacer would create a drop feedback loop. */
    GRD_INFO(
        "encoder %s started: bitrate %u kbps, pix_fmt %s, %ux%u@%u",
        codec->name,
        bitrate_kbps,
        av_get_pix_fmt_name(encoder->context->pix_fmt) != NULL
            ? av_get_pix_fmt_name(encoder->context->pix_fmt)
            : "?",
        (unsigned)width,
        (unsigned)height,
        fps
    );

    encoder->frame->format = encoder->context->pix_fmt;
    encoder->frame->width = (int)width;
    encoder->frame->height = (int)height;
#if GRD_HAS_CUDA
    if (encoder->cuda_hardware_frames) {
        result = 0;
        for (unsigned index = 0U; index < 2U; ++index) {
            encoder->cuda_frames[index] = av_frame_alloc();
            if (encoder->cuda_frames[index] == NULL) {
                result = AVERROR(ENOMEM);
                break;
            }
            result = av_hwframe_get_buffer(
                encoder->cuda_frames_context,
                encoder->cuda_frames[index],
                0
            );
            if (result < 0) {
                break;
            }
            encoder->cuda_frames[index]->format = encoder->context->pix_fmt;
            encoder->cuda_frames[index]->width = (int)width;
            encoder->cuda_frames[index]->height = (int)height;
        }
    } else {
        result = av_frame_get_buffer(encoder->frame, 32);
    }
#else
    result = av_frame_get_buffer(encoder->frame, 32);
#endif
    if (result < 0) {
        set_ffmpeg_error(error, GRD_OUT_OF_MEMORY, "Encoder buffer", result);
        grd_encoder_destroy(encoder);
        return NULL;
    }
    if (encoder->cuda_conversion
#if GRD_HAS_CUDA
        && !encoder->cuda_hardware_frames
#endif
    ) {
        const size_t nv12_size = (size_t)width * height * 3U / 2U;
        encoder->cuda_nv12 = malloc(nv12_size);
        if (encoder->cuda_nv12 == NULL) {
            grd_encoder_destroy(encoder);
            return NULL;
        }
    }
#if GRD_HAS_CUDA
    if (!encoder->cuda_hardware_frames) {
#endif
        encoder->converter = sws_getContext(
            (int)width, (int)height, AV_PIX_FMT_RGBA,
            (int)width, (int)height, encoder->context->pix_fmt,
            SWS_FAST_BILINEAR, NULL, NULL, NULL
        );
        if (encoder->converter == NULL) {
            grd_encoder_destroy(encoder);
            return NULL;
        }
#if GRD_HAS_CUDA
    }
#endif
    if (active_pipeline != NULL) {
        *active_pipeline = encoder->pipeline;
    }
    return encoder;
}

grd_status grd_encoder_encode(
    grd_encoder *encoder,
    const grd_frame *frame,
    grd_encoded_frame *encoded,
    grd_error *error
)
{
    const bool native_d3d11 =
        frame != NULL && frame->format == GRD_PIXEL_D3D11_BGRA;
    bool native_d3d11_supported = false;
#if GRD_HAS_CUDA
    native_d3d11_supported =
        encoder != NULL && encoder->cuda_hardware_frames;
#endif
    if (encoder == NULL || frame == NULL || encoded == NULL ||
        frame->width == 0U || frame->height == 0U ||
        (!native_d3d11 && frame->data == NULL) ||
        (native_d3d11 && frame->owner == NULL)) {
        return GRD_INVALID_ARGUMENT;
    }
    if (native_d3d11 && !native_d3d11_supported) {
        return GRD_NOT_SUPPORTED;
    }
    memset(encoded, 0, sizeof(*encoded));
    memset(&encoder->last_timing, 0, sizeof(encoder->last_timing));
#if defined(__APPLE__)
    if (encoder->videotoolbox != NULL) {
        uint8_t *native_data = NULL;
        size_t native_size = 0U;
        bool native_keyframe = false;
        const grd_status native_status = grd_videotoolbox_encoder_encode(
            encoder->videotoolbox,
            frame,
            encoder->force_keyframe,
            &native_data,
            &native_size,
            &native_keyframe,
            error
        );
        if (native_status == GRD_OK) {
            encoder->force_keyframe = false;
            encoded->data = native_data;
            encoded->size = native_size;
            encoded->keyframe = native_keyframe;
            encoded->timestamp_micros = frame->timestamp_micros;
            encoded->buffer.opaque = av_buffer_create(
                native_data, native_size, plain_buffer_free, NULL, 0
            );
            if (encoded->buffer.opaque == NULL) {
                free(native_data);
                return GRD_OUT_OF_MEMORY;
            }
            encoded->buffer.clone = owned_avbuf_clone;
            encoded->buffer.release = owned_avbuf_release;
            return GRD_OK;
        }
        if (native_status == GRD_NOT_SUPPORTED) {
            /* macOS <14 and non-native fallback frames can still use the
             * existing FFmpeg VideoToolbox path below. */
            grd_videotoolbox_encoder_destroy(encoder->videotoolbox);
            encoder->videotoolbox = NULL;
            if (error != NULL) {
                memset(error, 0, sizeof(*error));
            }
        } else if (encoder->context == NULL) {
            return native_status;
        } else if (error != NULL) {
            /* A transient native encoder miss is handled by the already
             * initialized FFmpeg fallback below. */
            memset(error, 0, sizeof(*error));
        }
    }
#endif
#if GRD_HAS_CUDA
    if (encoder->cuda_hardware_frames) {
        AVFrame *target = encoder->cuda_frames[encoder->cuda_frame_index];
        if (target == NULL || encoder->cuda_converter == NULL) {
            return GRD_ERROR;
        }
        const int write_result = av_frame_make_writable(target);
        if (write_result < 0) {
            set_ffmpeg_error(
                error, GRD_ERROR, "CUDA frame is not writable", write_result
            );
            return GRD_ERROR;
        }
        if (target->data[0] == NULL || target->data[1] == NULL ||
            target->linesize[0] <= 0 || target->linesize[1] <= 0) {
            return GRD_ERROR;
        }
        const uint64_t conversion_started = grd_now_micros();
        const grd_status conversion_status =
            grd_cuda_converter_rgba_to_nv12_device_async(
                encoder->cuda_converter,
                frame,
                encoder->width,
                encoder->height,
                target->data[0],
                target->data[1],
                (uint32_t)target->linesize[0],
                (uint32_t)target->linesize[1],
                error
            );
        if (conversion_status != GRD_OK) {
            return conversion_status;
        }
        if (native_d3d11 && !encoder->d3d11_zero_copy_logged) {
            encoder->d3d11_zero_copy_logged = true;
            GRD_INFO(
                "host zero-copy active: D3D11 BGRA -> CUDA NV12 -> NVENC"
            );
        }
        /* Overlap: while the GPU converts this frame, NVENC finishes the
         * previous one; collect its packet before synchronizing so the
         * conversion is hidden behind the encode. */
        uint64_t packet_timestamp = 0U;
        bool have_packet = false;
        if (encoder->cuda_packet_pending) {
            const uint64_t receive_started = grd_now_micros();
            const int receive = avcodec_receive_packet(
                encoder->context, encoder->packet
            );
            encoder->last_timing.receive_packet_micros +=
                grd_now_micros() - receive_started;
            encoder->last_timing.receive_packet_recorded = true;
            if (receive == AVERROR(EAGAIN)) {
                (void)grd_cuda_converter_sync(encoder->cuda_converter, error);
                encoder->last_timing.conversion_micros =
                    grd_now_micros() - conversion_started;
                encoder->last_timing.conversion_recorded = true;
                return GRD_WOULD_BLOCK;
            }
            if (receive < 0) {
                set_ffmpeg_error(
                    error, GRD_ERROR, "Receiving encoder packet", receive
                );
                (void)grd_cuda_converter_sync(encoder->cuda_converter, error);
                encoder->last_timing.conversion_micros =
                    grd_now_micros() - conversion_started;
                encoder->last_timing.conversion_recorded = true;
                return GRD_ERROR;
            }
            have_packet = true;
            packet_timestamp = encoder->cuda_pending_timestamp;
            encoder->cuda_packet_pending = false;
            /* The CUDA/NVENC double-buffered path receives packets here,
             * NOT in the generic path below: without this call the SPS/PPS
             * injection never ran on the Windows host (no 'SPS/PPS'
             * diagnostics in any server log despite the build having it). */
            prepend_parameter_sets(encoder);
        }
        if (grd_cuda_converter_sync(encoder->cuda_converter, error) != GRD_OK) {
            encoder->last_timing.conversion_micros =
                grd_now_micros() - conversion_started;
            encoder->last_timing.conversion_recorded = true;
            return GRD_ERROR;
        }
        encoder->last_timing.conversion_micros =
            grd_now_micros() - conversion_started;
        encoder->last_timing.conversion_recorded = true;
        target->pts = encoder->next_pts++;
        if (encoder->force_keyframe) {
            target->pict_type = AV_PICTURE_TYPE_I;
            encoder->force_keyframe = false;
        } else {
            target->pict_type = AV_PICTURE_TYPE_NONE;
        }
        const uint64_t send_started = grd_now_micros();
        const int send_result = avcodec_send_frame(encoder->context, target);
        encoder->last_timing.send_frame_micros =
            grd_now_micros() - send_started;
        encoder->last_timing.send_frame_recorded = true;
        if (send_result < 0) {
            set_ffmpeg_error(
                error, GRD_ERROR, "Sending frame to encoder", send_result
            );
            return GRD_ERROR;
        }
        encoder->cuda_frame_index ^= 1U;
        encoder->cuda_packet_pending = true;
        encoder->cuda_pending_timestamp = frame->timestamp_micros;
        if (!have_packet) {
            /* First frame submitted; its packet is returned on the next
             * call so the pipeline stays double-buffered. */
            return GRD_WOULD_BLOCK;
        }
        encoded->buffer.opaque = encoder->packet->buf != NULL
                                     ? av_buffer_ref(encoder->packet->buf)
                                     : NULL;
        if (encoded->buffer.opaque == NULL) {
            av_packet_unref(encoder->packet);
            return GRD_OUT_OF_MEMORY;
        }
        encoded->buffer.clone = owned_avbuf_clone;
        encoded->buffer.release = owned_avbuf_release;
        encoded->data = encoder->packet->data;
        encoded->size = (size_t)encoder->packet->size;
        encoded->keyframe = (encoder->packet->flags & AV_PKT_FLAG_KEY) != 0;
        encoded->timestamp_micros = packet_timestamp;
        av_packet_unref(encoder->packet);
        return GRD_OK;
    }
#endif
    if (encoder->frame == NULL || encoder->context == NULL) {
        return GRD_ERROR;
    }
    int result = av_frame_make_writable(encoder->frame);
    if (result < 0) {
        set_ffmpeg_error(error, GRD_ERROR, "Frame is not writable", result);
        return GRD_ERROR;
    }
    const uint64_t conversion_started = grd_now_micros();
    bool converted = false;
#if GRD_HAS_CUDA
    if (encoder->cuda_hardware_frames) {
        if (encoder->frame->data[0] != NULL &&
            encoder->frame->data[1] != NULL &&
            encoder->frame->linesize[0] > 0 &&
            encoder->frame->linesize[1] > 0 &&
            grd_cuda_converter_rgba_to_nv12_device(
                encoder->cuda_converter,
                frame,
                encoder->width,
                encoder->height,
                encoder->frame->data[0],
                encoder->frame->data[1],
                (uint32_t)encoder->frame->linesize[0],
                (uint32_t)encoder->frame->linesize[1],
                error
            ) == GRD_OK) {
            converted = true;
        } else if (error != NULL && error->code == GRD_OK) {
            error->code = GRD_ERROR;
            (void)snprintf(
                error->message, sizeof(error->message),
                "CUDA NV12 frame is not writable"
            );
        }
    }
#endif
    if (!converted && encoder->cuda_conversion
#if GRD_HAS_CUDA
        && !encoder->cuda_hardware_frames
#endif
    ) {
        const size_t nv12_size =
            (size_t)encoder->width * encoder->height * 3U / 2U;
        if (grd_cuda_converter_rgba_to_nv12(
                encoder->cuda_converter,
                frame,
                encoder->width,
                encoder->height,
                encoder->cuda_nv12,
                nv12_size,
                error
            ) == GRD_OK) {
            for (uint32_t row = 0U; row < encoder->height; ++row) {
                memcpy(
                    encoder->frame->data[0] +
                        (size_t)row * (size_t)encoder->frame->linesize[0],
                    encoder->cuda_nv12 + (size_t)row * encoder->width,
                    encoder->width
                );
            }
            const uint8_t *uv =
                encoder->cuda_nv12 +
                (size_t)encoder->width * encoder->height;
            for (uint32_t row = 0U; row < encoder->height / 2U; ++row) {
                memcpy(
                    encoder->frame->data[1] +
                        (size_t)row * (size_t)encoder->frame->linesize[1],
                    uv + (size_t)row * encoder->width,
                    encoder->width
                );
            }
            converted = true;
        } else if (!encoder->cuda_warning_emitted) {
            GRD_WARN("CUDA conversion failed; using the built-in fallback");
            encoder->cuda_warning_emitted = true;
        }
    }
#if GRD_HAS_CUDA
    if (encoder->cuda_hardware_frames && !converted) {
        return GRD_ERROR;
    }
#endif
    if (!converted) {
        const enum AVPixelFormat input_format =
            frame->format == GRD_PIXEL_BGRA8 ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA;
        /* Area averaging keeps downscaled frames sharp (1440p/4K monitors
         * fitted to 1080p); plain bilinear blurs text and UI. */
        const int scale_flags =
            frame->width > encoder->width || frame->height > encoder->height
                ? SWS_AREA
                : SWS_FAST_BILINEAR;
        encoder->converter = sws_getCachedContext(
                encoder->converter,
                (int)frame->width, (int)frame->height, input_format,
                (int)encoder->width, (int)encoder->height,
                encoder->context->pix_fmt,
                scale_flags, NULL, NULL, NULL
            );
        if (encoder->converter == NULL) {
            return GRD_ERROR;
        }
        const uint8_t *source_data[4] = {frame->data, NULL, NULL, NULL};
        int source_stride[4] = {(int)frame->stride, 0, 0, 0};
        (void)sws_scale(
            encoder->converter,
            source_data,
            source_stride,
            0,
            (int)frame->height,
            encoder->frame->data,
            encoder->frame->linesize
        );
    }
    encoder->last_timing.conversion_micros =
        grd_now_micros() - conversion_started;
    encoder->last_timing.conversion_recorded = true;
    encoder->frame->pts = encoder->next_pts++;
    if (encoder->force_keyframe) {
        encoder->frame->pict_type = AV_PICTURE_TYPE_I;
        encoder->force_keyframe = false;
    } else {
        encoder->frame->pict_type = AV_PICTURE_TYPE_NONE;
    }
    const uint64_t send_started = grd_now_micros();
    result = avcodec_send_frame(encoder->context, encoder->frame);
    encoder->last_timing.send_frame_micros =
        grd_now_micros() - send_started;
    encoder->last_timing.send_frame_recorded = true;
    if (result < 0) {
        set_ffmpeg_error(error, GRD_ERROR, "Sending frame to encoder", result);
        return GRD_ERROR;
    }
    const uint64_t receive_started = grd_now_micros();
    result = avcodec_receive_packet(encoder->context, encoder->packet);
    encoder->last_timing.receive_packet_micros =
        grd_now_micros() - receive_started;
    encoder->last_timing.receive_packet_recorded = true;
    if (result == AVERROR(EAGAIN)) {
        return GRD_WOULD_BLOCK;
    }
    if (result < 0) {
        set_ffmpeg_error(error, GRD_ERROR, "Receiving encoder packet", result);
        return GRD_ERROR;
    }
    prepend_parameter_sets(encoder);
    encoded->buffer.opaque = encoder->packet->buf != NULL
                                 ? av_buffer_ref(encoder->packet->buf)
                                 : NULL;
    if (encoded->buffer.opaque == NULL) {
        av_packet_unref(encoder->packet);
        return GRD_OUT_OF_MEMORY;
    }
    encoded->buffer.clone = owned_avbuf_clone;
    encoded->buffer.release = owned_avbuf_release;
    encoded->data = encoder->packet->data;
    encoded->size = (size_t)encoder->packet->size;
    encoded->keyframe = (encoder->packet->flags & AV_PKT_FLAG_KEY) != 0;
    encoded->timestamp_micros = frame->timestamp_micros;
    /* AV_PKT_DATA_QUALITY_STATS: u32le quality on the lambda scale (1 good
     * .. FF_LAMBDA_MAX bad). Empirically x264/x265 store QP*FF_QP2LAMBDA
     * (Avg QP 15 -> 1770, Avg QP 11.87 -> 1400), so the conversion is
     * QP = quality / FF_QP2LAMBDA. */
    size_t quality_stats_size = 0U;
    uint8_t *quality_stats = av_packet_get_side_data(
        encoder->packet, AV_PKT_DATA_QUALITY_STATS, &quality_stats_size
    );
    if (quality_stats != NULL && quality_stats_size >= (size_t)4U) {
        const uint32_t quality = AV_RL32(quality_stats);
        encoded->avg_qp = grd_qp_from_quality(quality);
        encoded->qp_known = true;
    }
    av_packet_unref(encoder->packet);
    return GRD_OK;
}

void grd_encoder_last_timing(
    const grd_encoder *encoder,
    grd_encoder_timing *timing
)
{
    if (timing == NULL) {
        return;
    }
    if (encoder == NULL) {
        memset(timing, 0, sizeof(*timing));
        return;
    }
    *timing = encoder->last_timing;
}

void grd_encoder_request_keyframe(grd_encoder *encoder)
{
    if (encoder != NULL) {
        encoder->force_keyframe = true;
    }
}

grd_status grd_encoder_set_bitrate(
    grd_encoder *encoder,
    uint32_t bitrate_kbps,
    grd_error *error
)
{
    if (encoder == NULL || bitrate_kbps == 0U) {
        return GRD_INVALID_ARGUMENT;
    }
    if (bitrate_kbps == encoder->bitrate_kbps) {
        return GRD_OK;
    }
#if defined(__APPLE__)
    if (encoder->videotoolbox != NULL) {
        const grd_status status = grd_videotoolbox_encoder_set_bitrate(
            encoder->videotoolbox, bitrate_kbps
        );
        if (status == GRD_OK) {
            encoder->bitrate_kbps = bitrate_kbps;
        }
        if (status != GRD_OK && error != NULL) {
            error->code = status;
            (void)snprintf(
                error->message, sizeof(error->message),
                "VideoToolbox bitrate cannot be updated"
            );
        }
        return status;
    }
#endif
    if (encoder->context == NULL) {
        return GRD_ERROR;
    }
    if (encoder->pipeline == GRD_PIPELINE_CUDA_NVENC) {
        /* FFmpeg's NVENC wrapper compares these live AVCodecContext fields on
         * avcodec_send_frame() and calls nvEncReconfigureEncoder when they
         * change. Keep the same CBR/VBV proportions used at creation; this
         * updates rate control on the next frame without destroying NVENC or
         * forcing a decoder restart. FFmpeg atomically resets rate control and
         * emits an IDR, so the caller batches small changes. */
        const int64_t bits_per_second = (int64_t)bitrate_kbps * 1000LL;
        const int fps = encoder->context->framerate.num > 0
                            ? encoder->context->framerate.num
                            : 60;
        encoder->context->bit_rate = bits_per_second;
        encoder->context->rc_min_rate = bits_per_second;
        encoder->context->rc_max_rate = bits_per_second * 11LL / 10LL;
        encoder->context->rc_buffer_size =
            (int)(bits_per_second / (int64_t)fps);
        encoder->bitrate_kbps = bitrate_kbps;
        GRD_INFO(
            "NVENC: riconfigurazione dinamica accodata a %u kbps",
            bitrate_kbps
        );
        return GRD_OK;
    }
    /* Other FFmpeg encoders are deliberately kept stable. Applying a lower
     * pacer budget while an encoder ignores a live rate change creates local
     * drops; reopening it mid-session is even more disruptive. */
    (void)error;
    return GRD_NOT_SUPPORTED;
}

void grd_encoded_frame_release(grd_encoded_frame *frame)
{
    if (frame != NULL) {
        if (frame->buffer.release != NULL) {
            frame->buffer.release((void *)frame->buffer.opaque);
        }
        memset(frame, 0, sizeof(*frame));
    }
}

void grd_encoder_destroy(grd_encoder *encoder)
{
    if (encoder == NULL) {
        return;
    }
#if defined(__APPLE__)
    grd_videotoolbox_encoder_destroy(encoder->videotoolbox);
#endif
    sws_freeContext(encoder->converter);
    grd_cuda_converter_destroy(encoder->cuda_converter);
#if GRD_HAS_CUDA
    av_buffer_unref(&encoder->cuda_frames_context);
    av_buffer_unref(&encoder->cuda_device_context);
    for (unsigned index = 0U; index < 2U; ++index) {
        av_frame_free(&encoder->cuda_frames[index]);
    }
#endif
    free(encoder->cuda_nv12);
    free(encoder->stored_sps);
    free(encoder->stored_pps);
    av_packet_free(&encoder->packet);
    av_frame_free(&encoder->frame);
    avcodec_free_context(&encoder->context);
    free(encoder);
}

grd_decoder *grd_decoder_create(
    grd_pipeline_kind requested_pipeline,
    grd_video_codec codec,
    grd_pipeline_kind *active_pipeline,
    grd_error *error
)
{
    grd_decoder *decoder = calloc(1U, sizeof(*decoder));
    if (decoder == NULL) {
        return NULL;
    }
#if defined(__APPLE__)
    if (requested_pipeline == GRD_PIPELINE_METAL_VIDEOTOOLBOX &&
        codec == GRD_CODEC_H264) {
        decoder->videotoolbox = grd_videotoolbox_decoder_create(error);
        if (decoder->videotoolbox != NULL) {
            decoder->pipeline = GRD_PIPELINE_METAL_VIDEOTOOLBOX;
            if (active_pipeline != NULL) {
                *active_pipeline = decoder->pipeline;
            }
            return decoder;
        }
    }
#endif
    const AVCodec *codec_av = NULL;
    grd_pipeline_kind active = GRD_PIPELINE_INTEGRATED_SOFTWARE;
    bool cuda_decoder_requested = false;
    if (requested_pipeline == GRD_PIPELINE_CUDA_NVENC) {
        const char *cuvid =
            codec == GRD_CODEC_HEVC ? "hevc_cuvid"
            : codec == GRD_CODEC_AV1 ? "av1_cuvid"
                                     : "h264_cuvid";
        const char *nvdec =
            codec == GRD_CODEC_HEVC ? "hevc_nvdec"
            : codec == GRD_CODEC_AV1 ? "av1_nvdec"
                                     : "h264_nvdec";
        codec_av = avcodec_find_decoder_by_name(cuvid);
        if (codec_av == NULL) {
            codec_av = avcodec_find_decoder_by_name(nvdec);
        }
        if (codec_av != NULL) {
            active = GRD_PIPELINE_CUDA_NVENC;
            cuda_decoder_requested = true;
        }
    }
    if (codec_av == NULL) {
        const enum AVCodecID codec_id =
            codec == GRD_CODEC_HEVC ? AV_CODEC_ID_HEVC
            : codec == GRD_CODEC_AV1 ? AV_CODEC_ID_AV1
                                     : AV_CODEC_ID_H264;
        codec_av = avcodec_find_decoder(codec_id);
    }
    if (codec_av == NULL) {
        if (codec != GRD_CODEC_H264) {
            grd_decoder_destroy(decoder);
            return grd_decoder_create(
                requested_pipeline, GRD_CODEC_H264, active_pipeline, error
            );
        }
        free(decoder);
        if (error != NULL) {
            error->code = GRD_NOT_SUPPORTED;
            (void)snprintf(
                error->message, sizeof(error->message),
                "Video decoder is unavailable"
            );
        }
        return NULL;
    }
    for (size_t index = 0U; index < GRD_DECODER_FRAME_SLOTS; ++index) {
        decoder->frame_slots[index] = calloc(1U, sizeof(*decoder->frame_slots[index]));
        if (decoder->frame_slots[index] == NULL) {
            grd_decoder_destroy(decoder);
            if (error != NULL) {
                error->code = GRD_OUT_OF_MEMORY;
                (void)snprintf(
                    error->message, sizeof(error->message),
                    "Unable to allocate the decoder frame pool"
                );
            }
            return NULL;
        }
    }
    decoder->context = avcodec_alloc_context3(codec_av);
    decoder->frame = av_frame_alloc();
    decoder->packet = av_packet_alloc();
    decoder->pipeline = active;
    if (decoder->context == NULL || decoder->frame == NULL || decoder->packet == NULL) {
        grd_decoder_destroy(decoder);
        return NULL;
    }
    if (cuda_decoder_requested) {
        AVBufferRef *device = NULL;
        if (av_hwdevice_ctx_create(
                &device, AV_HWDEVICE_TYPE_CUDA, NULL, NULL, 0
            ) < 0) {
            grd_decoder_destroy(decoder);
            return grd_decoder_create(
                GRD_PIPELINE_INTEGRATED_SOFTWARE,
                codec,
                active_pipeline,
                error
            );
        }
        decoder->context->hw_device_ctx = av_buffer_ref(device);
        decoder->cuda_device_context = device;
        decoder->cuda_transfer_frame = av_frame_alloc();
        if (decoder->context->hw_device_ctx == NULL ||
            decoder->cuda_transfer_frame == NULL) {
            grd_decoder_destroy(decoder);
            return grd_decoder_create(
                GRD_PIPELINE_INTEGRATED_SOFTWARE,
                codec,
                active_pipeline,
                error
            );
        }
    }
    decoder->context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    const int result = avcodec_open2(decoder->context, codec_av, NULL);
    if (result < 0) {
        if (active == GRD_PIPELINE_CUDA_NVENC) {
            grd_decoder_destroy(decoder);
            return grd_decoder_create(
                GRD_PIPELINE_INTEGRATED_SOFTWARE,
                codec,
                active_pipeline,
                error
            );
        }
        set_ffmpeg_error(error, GRD_ERROR, "Opening decoder", result);
        grd_decoder_destroy(decoder);
        return NULL;
    }
    if (active_pipeline != NULL) {
        *active_pipeline = active;
    }
    return decoder;
}

grd_status grd_decoder_enable_d3d11_output(
    grd_decoder *decoder,
    void *d3d11_device,
    grd_error *error
)
{
    if (decoder == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
#if defined(_WIN32) && GRD_HAS_CUDA
    if (decoder->d3d11_uploader == NULL) {
        decoder->d3d11_uploader = grd_cuda_d3d11_uploader_create(
            d3d11_device, error
        );
        if (decoder->d3d11_uploader == NULL) {
            return error != NULL && error->code != GRD_OK
                       ? error->code
                       : GRD_NOT_SUPPORTED;
        }
    }
    return GRD_OK;
#else
    (void)d3d11_device;
    if (error != NULL) {
        error->code = GRD_NOT_SUPPORTED;
        (void)snprintf(
            error->message, sizeof(error->message),
            "D3D11 output is unavailable"
        );
    }
    return GRD_NOT_SUPPORTED;
#endif
}

static grd_status decoder_render_frame(
    grd_decoder *decoder,
    AVFrame *source_frame,
    grd_frame *frame,
    grd_error *error
)
{
    if (decoder->frame->format == AV_PIX_FMT_CUDA) {
#if defined(_WIN32) && GRD_HAS_CUDA
        if (decoder->d3d11_uploader != NULL &&
            decoder->frame->width > 0 && decoder->frame->height > 0 &&
            decoder->frame->data[0] != NULL &&
            decoder->frame->data[1] != NULL &&
            decoder->frame->linesize[0] > 0 &&
            decoder->frame->linesize[1] > 0) {
            void *texture = NULL;
            const grd_status upload_status = grd_cuda_d3d11_uploader_upload(
                decoder->d3d11_uploader,
                (uint32_t)decoder->frame->width,
                (uint32_t)decoder->frame->height,
                decoder->frame->data[0],
                decoder->frame->data[1],
                (uint32_t)decoder->frame->linesize[0],
                (uint32_t)decoder->frame->linesize[1],
                &texture,
                error
            );
            if (upload_status == GRD_OK && texture != NULL) {
                frame->data = NULL;
                frame->size = 0U;
                frame->width = (uint32_t)decoder->frame->width;
                frame->height = (uint32_t)decoder->frame->height;
                frame->stride = 0U;
                frame->format = GRD_PIXEL_D3D11_RGBA;
                frame->timestamp_micros = grd_now_micros();
                frame->owner = texture;
                frame->release_fn = NULL;
                av_frame_unref(decoder->frame);
                return GRD_OK;
            }
            /* The interop uploader failed: fall back to the CPU transfer
             * path below instead of dropping the frame. */
            if (error != NULL) {
                memset(error, 0, sizeof(*error));
            }
        }
#endif
        av_frame_unref(decoder->cuda_transfer_frame);
        const int transfer = av_hwframe_transfer_data(
            decoder->cuda_transfer_frame, decoder->frame, 0
        );
        if (transfer < 0) {
            av_frame_unref(decoder->frame);
            set_ffmpeg_error(
                error, GRD_ERROR, "Transferring NVDEC frame", transfer
            );
            return GRD_ERROR;
        }
        source_frame = decoder->cuda_transfer_frame;
    }
    const int width = source_frame->width;
    const int height = source_frame->height;
    if (width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / ((size_t)height * 4U)) {
        if (decoder->cuda_transfer_frame != NULL) {
            av_frame_unref(decoder->cuda_transfer_frame);
        }
        av_frame_unref(decoder->frame);
        return GRD_INVALID_ARGUMENT;
    }
    const size_t output_size = (size_t)width * (size_t)height * 4U;
    grd_decoder_frame_slot *slot = acquire_decoder_frame_slot(
        decoder->frame_slots
    );
    if (slot == NULL) {
        if (decoder->cuda_transfer_frame != NULL) {
            av_frame_unref(decoder->cuda_transfer_frame);
        }
        av_frame_unref(decoder->frame);
        return GRD_WOULD_BLOCK;
    }
    if (slot->capacity < output_size) {
        uint8_t *resized = realloc(slot->data, output_size);
        if (resized == NULL) {
            atomic_store_explicit(
                &slot->in_use, false, memory_order_release
            );
            if (decoder->cuda_transfer_frame != NULL) {
                av_frame_unref(decoder->cuda_transfer_frame);
            }
            av_frame_unref(decoder->frame);
            return GRD_OUT_OF_MEMORY;
        }
        slot->data = resized;
        slot->capacity = output_size;
    }
    decoder->converter = sws_getCachedContext(
        decoder->converter,
        width, height, (enum AVPixelFormat)source_frame->format,
        width, height, AV_PIX_FMT_RGBA,
        SWS_FAST_BILINEAR, NULL, NULL, NULL
    );
    if (decoder->converter == NULL) {
        atomic_store_explicit(
            &slot->in_use, false, memory_order_release
        );
        if (decoder->cuda_transfer_frame != NULL) {
            av_frame_unref(decoder->cuda_transfer_frame);
        }
        av_frame_unref(decoder->frame);
        return GRD_ERROR;
    }
    uint8_t *destination[4] = {slot->data, NULL, NULL, NULL};
    int destination_stride[4] = {width * 4, 0, 0, 0};
    const int scaled_lines = sws_scale(
        decoder->converter,
        (const uint8_t *const *)source_frame->data,
        source_frame->linesize,
        0,
        height,
        destination,
        destination_stride
    );
    if (scaled_lines <= 0) {
        atomic_store_explicit(
            &slot->in_use, false, memory_order_release
        );
        if (decoder->cuda_transfer_frame != NULL) {
            av_frame_unref(decoder->cuda_transfer_frame);
        }
        av_frame_unref(decoder->frame);
        return GRD_ERROR;
    }
    frame->data = slot->data;
    frame->size = output_size;
    frame->width = (uint32_t)width;
    frame->height = (uint32_t)height;
    frame->stride = (uint32_t)(width * 4);
    frame->format = GRD_PIXEL_RGBA8;
    frame->timestamp_micros = grd_now_micros();
    frame->owner = slot;
    frame->release_fn = release_decoder_frame_slot;
    if (decoder->cuda_transfer_frame != NULL) {
        av_frame_unref(decoder->cuda_transfer_frame);
    }
    av_frame_unref(decoder->frame);
    return GRD_OK;
}

static grd_status decoder_send_and_render(
    grd_decoder *decoder,
    grd_frame *frame,
    grd_error *error
)
{
    int result = avcodec_send_packet(decoder->context, decoder->packet);
    av_packet_unref(decoder->packet);
    if (result < 0) {
        set_ffmpeg_error(error, GRD_PROTOCOL_ERROR, "Sending packet to decoder", result);
        return GRD_PROTOCOL_ERROR;
    }
    result = avcodec_receive_frame(decoder->context, decoder->frame);
    if (result == AVERROR(EAGAIN)) {
        return GRD_WOULD_BLOCK;
    }
    if (result < 0) {
        set_ffmpeg_error(error, GRD_PROTOCOL_ERROR, "Decoding frame", result);
        return GRD_PROTOCOL_ERROR;
    }
    return decoder_render_frame(decoder, decoder->frame, frame, error);
}

grd_status grd_decoder_decode(
    grd_decoder *decoder,
    const uint8_t *data,
    size_t size,
    grd_frame *frame,
    grd_error *error
)
{
    if (decoder == NULL || data == NULL || size == 0U || frame == NULL ||
        size > (size_t)INT_MAX) {
        return GRD_INVALID_ARGUMENT;
    }
#if defined(__APPLE__)
    if (decoder->videotoolbox != NULL) {
        return grd_videotoolbox_decoder_decode(
            decoder->videotoolbox, data, size, frame, error
        );
    }
#endif
    memset(frame, 0, sizeof(*frame));
    av_packet_unref(decoder->packet);
    const int result = av_new_packet(decoder->packet, (int)size);
    if (result < 0) {
        set_ffmpeg_error(error, GRD_OUT_OF_MEMORY, "Buffer decoder", result);
        return GRD_OUT_OF_MEMORY;
    }
    memcpy(decoder->packet->data, data, size);
    return decoder_send_and_render(decoder, frame, error);
}

typedef struct grd_adopted_buffer {
    grd_buffer_release_fn release;
} grd_adopted_buffer;

static void adopted_buffer_free(void *opaque, uint8_t *data)
{
    (void)data;
    grd_adopted_buffer *context = opaque;
    if (context != NULL && context->release != NULL) {
        context->release(data);
    }
    free(context);
}

grd_status grd_decoder_decode_owned(
    grd_decoder *decoder,
    uint8_t *data,
    size_t size,
    uint8_t *buffer_base,
    size_t buffer_size,
    grd_buffer_release_fn buffer_release,
    grd_frame *frame,
    grd_error *error
)
{
    if (decoder == NULL || data == NULL || size == 0U || frame == NULL ||
        buffer_base == NULL || buffer_release == NULL ||
        size > (size_t)INT_MAX) {
        if (buffer_release != NULL && buffer_base != NULL) {
            buffer_release(buffer_base);
        }
        return GRD_INVALID_ARGUMENT;
    }
    if (data < buffer_base) {
        buffer_release(buffer_base);
        return GRD_INVALID_ARGUMENT;
    }
    const size_t data_offset = (size_t)(data - buffer_base);
    if (data_offset + size + GRD_MEDIA_BUFFER_PADDING > buffer_size) {
        buffer_release(buffer_base);
        return GRD_INVALID_ARGUMENT;
    }
#if defined(__APPLE__)
    if (decoder->videotoolbox != NULL) {
        const grd_status status = grd_decoder_decode(
            decoder, data, size, frame, error
        );
        buffer_release(buffer_base);
        return status;
    }
#endif
    grd_adopted_buffer *context = malloc(sizeof(*context));
    if (context == NULL) {
        buffer_release(buffer_base);
        return GRD_OUT_OF_MEMORY;
    }
    context->release = buffer_release;
    AVBufferRef *owned = av_buffer_create(
        buffer_base, buffer_size, adopted_buffer_free, context, 0
    );
    if (owned == NULL) {
        free(context);
        buffer_release(buffer_base);
        return GRD_OUT_OF_MEMORY;
    }
    memset(frame, 0, sizeof(*frame));
    av_packet_unref(decoder->packet);
    decoder->packet->buf = owned;
    decoder->packet->data = data;
    decoder->packet->size = (int)size;
    return decoder_send_and_render(decoder, frame, error);
}

bool grd_decoder_needs_parameter_sets(grd_decoder *decoder)
{
    if (decoder == NULL) {
        return false;
    }
#if defined(__APPLE__)
    if (decoder->pipeline == GRD_PIPELINE_METAL_VIDEOTOOLBOX &&
        decoder->videotoolbox != NULL) {
        return grd_videotoolbox_decoder_needs_parameter_sets(
            decoder->videotoolbox
        );
    }
#endif
    return false;
}

void grd_decoder_set_bgra_output(grd_decoder *decoder)
{
    if (decoder == NULL) {
        return;
    }
#if defined(__APPLE__)
    if (decoder->videotoolbox != NULL) {
        grd_videotoolbox_decoder_set_bgra_output(decoder->videotoolbox);
    }
#endif
}

void grd_decoder_destroy(grd_decoder *decoder)
{
    if (decoder == NULL) {
        return;
    }
#if defined(__APPLE__)
    if (decoder->videotoolbox != NULL) {
        grd_videotoolbox_decoder_destroy(decoder->videotoolbox);
    }
#endif
#if defined(_WIN32) && GRD_HAS_CUDA
    grd_cuda_d3d11_uploader_destroy(decoder->d3d11_uploader);
#endif
    av_frame_free(&decoder->cuda_transfer_frame);
    av_buffer_unref(&decoder->cuda_device_context);
    sws_freeContext(decoder->converter);
    av_packet_free(&decoder->packet);
    av_frame_free(&decoder->frame);
    avcodec_free_context(&decoder->context);
    for (size_t index = 0U; index < GRD_DECODER_FRAME_SLOTS; ++index) {
        grd_decoder_frame_slot *slot = decoder->frame_slots[index];
        if (slot == NULL) {
            continue;
        }
        if (atomic_load_explicit(&slot->in_use, memory_order_acquire)) {
            /* The application normally releases all mailbox frames before
             * destroying the decoder. If a caller violates that lifetime,
             * let the frame-release hook reclaim this slot instead of
             * leaving a dangling data pointer. */
            atomic_store_explicit(
                &slot->orphaned, true, memory_order_release
            );
        } else {
            free(slot->data);
            free(slot);
        }
        decoder->frame_slots[index] = NULL;
    }
    free(decoder);
}
