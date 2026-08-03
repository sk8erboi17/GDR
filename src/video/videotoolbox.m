#include "grd/videotoolbox.h"
#include "grd/log.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <dispatch/dispatch.h>
#include <limits.h>
#include <os/lock.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct grd_h264_nal {
    const uint8_t *data;
    size_t size;
} grd_h264_nal;

struct grd_videotoolbox_decoder {
    VTDecompressionSessionRef session;
    CMVideoFormatDescriptionRef format;
    uint8_t *sps;
    size_t sps_size;
    uint8_t *pps;
    size_t pps_size;
    grd_frame output;
    OSStatus output_status;
    bool output_ready;
    bool frame_in_flight;
    dispatch_semaphore_t output_semaphore;
    os_unfair_lock output_lock;
    uint8_t *avcc_buffer;
    size_t avcc_capacity;
    uint32_t wouldblock_streak;
    uint32_t pixel_format;
    uint64_t in_flight_since_micros;
    bool needs_parameter_sets;
    bool prefer_bgra;
};

static _Atomic uint64_t g_vt_warn_micros;

/* The native decoder is fed by the real-time decode thread: keep repeated
 * diagnostics rate-limited so they cannot flood the log or add latency. */
static void vt_warn_throttled(const char *format, ...)
{
    const uint64_t now = grd_now_micros();
    uint64_t last = atomic_load_explicit(
        &g_vt_warn_micros, memory_order_relaxed
    );
    if (now - last < 1000000ULL) {
        return;
    }
    if (!atomic_compare_exchange_strong_explicit(
            &g_vt_warn_micros,
            &last,
            now,
            memory_order_relaxed,
            memory_order_relaxed
        )) {
        return;
    }
    char buffer[384];
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    GRD_WARN("VT decoder: %s", buffer);
}

static grd_status vt_would_block(
    grd_videotoolbox_decoder *decoder,
    const char *reason
)
{
    if (decoder->session == NULL) {
        /* No session yet: the decoder is waiting for a keyframe carrying
         * SPS/PPS in-band. The decode loop uses this flag to keep
         * re-requesting IDRs (with backoff) instead of silently waiting:
         * a session can otherwise remain NULL forever when the first
         * keyframe is dropped or arrives without parameter sets. */
        decoder->needs_parameter_sets = true;
    }
    if (++decoder->wouldblock_streak == 60U) {
        vt_warn_throttled(
            "stallo (%s): session=%p frame_in_flight=%d",
            reason,
            (void *)decoder->session,
            decoder->frame_in_flight ? 1 : 0
        );
        decoder->wouldblock_streak = 0U;
    }
    return GRD_WOULD_BLOCK;
}

static void set_error(grd_error *error, grd_status code, const char *message)
{
    if (error != NULL) {
        error->code = code;
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) |
           ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) |
           (uint32_t)data[3];
}

static bool start_code_at(
    const uint8_t *data,
    size_t size,
    size_t offset,
    size_t *prefix_size
)
{
    if (offset + 3U <= size && data[offset] == 0U &&
        data[offset + 1U] == 0U && data[offset + 2U] == 1U) {
        *prefix_size = 3U;
        return true;
    }
    if (offset + 4U <= size && data[offset] == 0U &&
        data[offset + 1U] == 0U && data[offset + 2U] == 0U &&
        data[offset + 3U] == 1U) {
        *prefix_size = 4U;
        return true;
    }
    return false;
}

static size_t collect_annex_b_nals(
    const uint8_t *data,
    size_t size,
    grd_h264_nal *nals,
    size_t capacity
)
{
    size_t count = 0U;
    size_t search = 0U;
    size_t prefix = 0U;
    while (search < size &&
           !start_code_at(data, size, search, &prefix)) {
        ++search;
    }
    while (search < size && count < capacity) {
        const size_t nal_start = search + prefix;
        size_t next = nal_start;
        size_t next_prefix = 0U;
        while (next < size &&
               !start_code_at(data, size, next, &next_prefix)) {
            ++next;
        }
        if (next > nal_start) {
            nals[count].data = data + nal_start;
            nals[count].size = next - nal_start;
            ++count;
        }
        if (next >= size) {
            break;
        }
        search = next;
        prefix = next_prefix;
    }
    return count;
}

static size_t collect_avcc_nals(
    const uint8_t *data,
    size_t size,
    grd_h264_nal *nals,
    size_t capacity
)
{
    size_t count = 0U;
    size_t offset = 0U;
    while (offset + 4U <= size && count < capacity) {
        const uint32_t nal_size = read_be32(data + offset);
        offset += 4U;
        if (nal_size == 0U || (size_t)nal_size > size - offset) {
            return 0U;
        }
        nals[count].data = data + offset;
        nals[count].size = (size_t)nal_size;
        ++count;
        offset += (size_t)nal_size;
    }
    return offset == size ? count : 0U;
}

static size_t collect_nals(
    const uint8_t *data,
    size_t size,
    grd_h264_nal *nals,
    size_t capacity
)
{
    size_t prefix_size = 0U;
    for (size_t index = 0U; index < size; ++index) {
        if (start_code_at(data, size, index, &prefix_size)) {
            return collect_annex_b_nals(data, size, nals, capacity);
        }
    }
    return collect_avcc_nals(data, size, nals, capacity);
}

static void release_pixel_buffer(void *owner)
{
    CVPixelBufferRef pixel_buffer = (CVPixelBufferRef)owner;
    if (pixel_buffer == NULL) {
        return;
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
    CFRelease(pixel_buffer);
}

static void release_block_buffer(
    void *refcon,
    void *doomed_memory_block,
    size_t size
)
{
    (void)refcon;
    /* The AVCC storage belongs to the decoder and is reused only after the
     * synchronous decode call has released its CMSampleBuffer. */
    (void)doomed_memory_block;
    (void)size;
}

static void release_output_locked(grd_videotoolbox_decoder *decoder)
{
    if (decoder->output.release_fn != NULL) {
        decoder->output.release_fn(decoder->output.owner);
    } else {
        free(decoder->output.data);
    }
    memset(&decoder->output, 0, sizeof(decoder->output));
    decoder->output_ready = false;
}

static void release_output(grd_videotoolbox_decoder *decoder)
{
    os_unfair_lock_lock(&decoder->output_lock);
    release_output_locked(decoder);
    os_unfair_lock_unlock(&decoder->output_lock);
}

static void output_callback(
    void *refcon,
    void *source_frame_refcon,
    OSStatus status,
    VTDecodeInfoFlags info_flags,
    CVImageBufferRef image_buffer,
    CMTime presentation_time_stamp,
    CMTime presentation_duration
)
{
    (void)source_frame_refcon;
    (void)info_flags;
    (void)presentation_time_stamp;
    (void)presentation_duration;
    grd_videotoolbox_decoder *decoder = refcon;
    os_unfair_lock_lock(&decoder->output_lock);
    decoder->output_status = status;
    if (status != noErr || image_buffer == NULL) {
        if (status != noErr) {
            vt_warn_throttled(
                "output callback error: %d", (int)status
            );
        }
        os_unfair_lock_unlock(&decoder->output_lock);
        dispatch_semaphore_signal(decoder->output_semaphore);
        return;
    }
    CVPixelBufferRef pixel_buffer = (CVPixelBufferRef)CFRetain(image_buffer);
    if (CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly) !=
        kCVReturnSuccess) {
        decoder->output_status = kCVReturnError;
        CFRelease(pixel_buffer);
        os_unfair_lock_unlock(&decoder->output_lock);
        dispatch_semaphore_signal(decoder->output_semaphore);
        return;
    }
    const size_t width = CVPixelBufferGetWidth(pixel_buffer);
    const size_t height = CVPixelBufferGetHeight(pixel_buffer);
    const size_t plane_count = CVPixelBufferGetPlaneCount(pixel_buffer);
    if (plane_count < 2U && decoder->prefer_bgra) {
        /* Software-renderer path: VideoToolbox produced a single-plane
         * BGRA buffer. The CPU upload treats it like any RGBA frame, so
         * no YUV->RGB conversion or plane juggling is needed. */
        const uint8_t *base = CVPixelBufferGetBaseAddress(pixel_buffer);
        const size_t stride = CVPixelBufferGetBytesPerRow(pixel_buffer);
        if (base == NULL || stride == 0U || width == 0U || height == 0U ||
            width > UINT32_MAX || height > UINT32_MAX ||
            height > SIZE_MAX / stride) {
            decoder->output_status = kCVReturnAllocationFailed;
        } else {
            release_output_locked(decoder);
            decoder->output.data = (uint8_t *)base;
            decoder->output.size = stride * height;
            decoder->output.width = (uint32_t)width;
            decoder->output.height = (uint32_t)height;
            decoder->output.stride = (uint32_t)stride;
            decoder->output.format = GRD_PIXEL_BGRA8;
            decoder->output.timestamp_micros = grd_now_micros();
            decoder->output.owner = pixel_buffer;
            decoder->output.release_fn = release_pixel_buffer;
            decoder->output_ready = true;
            pixel_buffer = NULL;
        }
        if (pixel_buffer != NULL) {
            CVPixelBufferUnlockBaseAddress(
                pixel_buffer, kCVPixelBufferLock_ReadOnly
            );
            CFRelease(pixel_buffer);
        }
        os_unfair_lock_unlock(&decoder->output_lock);
        dispatch_semaphore_signal(decoder->output_semaphore);
        return;
    }
    const uint8_t *y_plane = plane_count >= 2U
                                 ? CVPixelBufferGetBaseAddressOfPlane(
                                       pixel_buffer, 0
                                   )
                                 : NULL;
    const size_t y_stride = plane_count >= 2U
                                ? CVPixelBufferGetBytesPerRowOfPlane(
                                      pixel_buffer, 0
                                  )
                                : 0U;
    const uint8_t *uv_plane = plane_count >= 2U
                                  ? CVPixelBufferGetBaseAddressOfPlane(
                                        pixel_buffer, 1
                                    )
                                  : NULL;
    const size_t uv_stride = plane_count >= 2U
                                 ? CVPixelBufferGetBytesPerRowOfPlane(
                                       pixel_buffer, 1
                                   )
                                 : 0U;
    /* Keep the CVPixelBuffer locked and owned by the frame: the Metal
     * renderer wraps the IOSurface directly (zero copy) and converts
     * NV12/P010 -> RGB in the shader, so no per-frame malloc or CPU copy. */
    if (y_plane == NULL || y_stride == 0U || width == 0U || height == 0U ||
        width > UINT32_MAX || height > UINT32_MAX ||
        height > SIZE_MAX / y_stride) {
        decoder->output_status = kCVReturnAllocationFailed;
    } else {
        release_output_locked(decoder);
        decoder->output.data = (uint8_t *)y_plane;
        decoder->output.size = y_stride * height;
        decoder->output.width = (uint32_t)width;
        decoder->output.height = (uint32_t)height;
        decoder->output.stride = (uint32_t)y_stride;
        decoder->output.data_uv = (uint8_t *)uv_plane;
        decoder->output.stride_uv = (uint32_t)uv_stride;
        decoder->output.format =
            decoder->pixel_format ==
                    kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                ? GRD_PIXEL_P010
                : GRD_PIXEL_NV12;
        decoder->output.timestamp_micros = grd_now_micros();
        decoder->output.owner = pixel_buffer;
        decoder->output.release_fn = release_pixel_buffer;
        decoder->output_ready = true;
        /* Ownership moved into decoder->output. */
        pixel_buffer = NULL;
    }
    if (pixel_buffer != NULL) {
        CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
        CFRelease(pixel_buffer);
    }
    os_unfair_lock_unlock(&decoder->output_lock);
    dispatch_semaphore_signal(decoder->output_semaphore);
}

static bool same_bytes(
    const uint8_t *left,
    size_t left_size,
    const uint8_t *right,
    size_t right_size
)
{
    return left_size == right_size &&
           left_size != 0U &&
           memcmp(left, right, left_size) == 0;
}

/* NVENC rewrites the HRD/VUI tail of the SPS whenever the ABR target moves
 * (bit_rate_value_minus1, cpb_size_value_minus1): the first bytes
 * (profile_idc, constraints, level_idc, dimensions) stay identical while
 * the wire rate ramps or cuts. Recreating the VideoToolbox session for a
 * bitrate-only SPS change makes the next keyframe fail with
 * kVTVideoDecoderBadDataErr (observed -12909 storms during ABR ramps and
 * complex scene changes), so an SPS with the same size and the same first
 * 8 bytes is treated as compatible and the session is kept. A real
 * parameter change (profile/level/dimensions) still recreates it. */
static bool sps_compatible(
    const uint8_t *left,
    size_t left_size,
    const uint8_t *right,
    size_t right_size
)
{
    if (left_size != right_size || left_size == 0U) {
        return false;
    }
    if (left_size < 8U) {
        return memcmp(left, right, left_size) == 0;
    }
    return memcmp(left, right, 8U) == 0;
}

static OSStatus create_session(
    grd_videotoolbox_decoder *decoder,
    const grd_h264_nal *sps,
    const grd_h264_nal *pps,
    uint32_t pixel_format
)
{
    const uint8_t *parameter_sets[2] = {sps->data, pps->data};
    const size_t parameter_sizes[2] = {sps->size, pps->size};
    CMVideoFormatDescriptionRef format = NULL;
    OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
        kCFAllocatorDefault,
        2U,
        parameter_sets,
        parameter_sizes,
        4U,
        &format
    );
    if (status != noErr) {
        return status;
    }
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault,
        2,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    );
    int32_t pixel_format_value_int = (int32_t)pixel_format;
    CFNumberRef pixel_format_value = CFNumberCreate(
        kCFAllocatorDefault,
        kCFNumberSInt32Type,
        &pixel_format_value_int
    );
    if (attributes == NULL || pixel_format_value == NULL) {
        if (pixel_format_value != NULL) CFRelease(pixel_format_value);
        if (attributes != NULL) CFRelease(attributes);
        CFRelease(format);
        return kVTAllocationFailedErr;
    }
    CFDictionarySetValue(
        attributes,
        kCVPixelBufferPixelFormatTypeKey,
        pixel_format_value
    );
    /* Force IOSurface-backed output buffers: the Metal renderer wraps the
     * pixel buffer in an SDL texture via CVPixelBufferGetIOSurface(), which
     * fails for CPU-backed buffers. With this key VideoToolbox allocates
     * IOSurface-backed buffers and the native zero-copy path works. */
    CFMutableDictionaryRef io_surface_properties =
        CFDictionaryCreateMutable(
            kCFAllocatorDefault,
            0,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks
        );
    if (io_surface_properties == NULL) {
        CFRelease(pixel_format_value);
        CFRelease(attributes);
        CFRelease(format);
        return kVTAllocationFailedErr;
    }
    CFDictionarySetValue(
        attributes,
        kCVPixelBufferIOSurfacePropertiesKey,
        io_surface_properties
    );
    CFRelease(io_surface_properties);
    VTDecompressionOutputCallbackRecord callback = {
        .decompressionOutputCallback = output_callback,
        .decompressionOutputRefCon = decoder
    };
    VTDecompressionSessionRef session = NULL;
    status = VTDecompressionSessionCreate(
        kCFAllocatorDefault,
        format,
        NULL,
        attributes,
        &callback,
        &session
    );
    CFRelease(pixel_format_value);
    CFRelease(attributes);
    if (status == noErr) {
        uint8_t *sps_copy = malloc(sps->size);
        uint8_t *pps_copy = malloc(pps->size);
        if (sps_copy == NULL || pps_copy == NULL) {
            free(sps_copy);
            free(pps_copy);
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
            CFRelease(format);
            return kVTAllocationFailedErr;
        }
        memcpy(sps_copy, sps->data, sps->size);
        memcpy(pps_copy, pps->data, pps->size);
        if (decoder->session != NULL) {
            VTDecompressionSessionInvalidate(decoder->session);
            CFRelease(decoder->session);
        }
        if (decoder->format != NULL) {
            CFRelease(decoder->format);
        }
        decoder->session = session;
        decoder->format = format;
        free(decoder->sps);
        free(decoder->pps);
        decoder->sps = sps_copy;
        decoder->pps = pps_copy;
        decoder->sps_size = sps->size;
        decoder->pps_size = pps->size;
        decoder->pixel_format = pixel_format;
        decoder->needs_parameter_sets = false;
    } else {
        CFRelease(format);
    }
    return status;
}

bool grd_videotoolbox_decoder_needs_parameter_sets(
    grd_videotoolbox_decoder *decoder
)
{
    return decoder != NULL && decoder->needs_parameter_sets;
}

void grd_videotoolbox_decoder_set_bgra_output(
    grd_videotoolbox_decoder *decoder
)
{
    if (decoder != NULL) {
        decoder->prefer_bgra = true;
    }
}

grd_videotoolbox_decoder *grd_videotoolbox_decoder_create(grd_error *error)
{
    if (!VTIsHardwareDecodeSupported(kCMVideoCodecType_H264)) {
        set_error(
            error,
            GRD_NOT_SUPPORTED,
            "VideoToolbox hardware H.264 decoder is unavailable"
        );
        return NULL;
    }
    grd_videotoolbox_decoder *decoder = calloc(1U, sizeof(*decoder));
    if (decoder == NULL) {
        set_error(error, GRD_OUT_OF_MEMORY, "Unable to allocate VideoToolbox decoder");
        return NULL;
    }
    decoder->output_status = noErr;
    decoder->output_lock = OS_UNFAIR_LOCK_INIT;
    decoder->output_semaphore = dispatch_semaphore_create(0);
    if (decoder->output_semaphore == NULL) {
        free(decoder);
        set_error(error, GRD_OUT_OF_MEMORY, "Unable to allocate VideoToolbox synchronization");
        return NULL;
    }
    return decoder;
}

grd_status grd_videotoolbox_decoder_decode(
    grd_videotoolbox_decoder *decoder,
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
    memset(frame, 0, sizeof(*frame));
    /* Deliver a frame decoded asynchronously by an earlier call before
     * submitting anything new. Never discard a pending output: a slow decode
     * otherwise starves the reference chain into a visible freeze. */
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        os_unfair_lock_lock(&decoder->output_lock);
        if (decoder->output_ready) {
            *frame = decoder->output;
            memset(&decoder->output, 0, sizeof(decoder->output));
            decoder->output_ready = false;
            decoder->frame_in_flight = false;
            decoder->wouldblock_streak = 0U;
            os_unfair_lock_unlock(&decoder->output_lock);
            return GRD_OK;
        }
        const bool in_flight = decoder->frame_in_flight;
        const OSStatus pending_status = decoder->output_status;
        os_unfair_lock_unlock(&decoder->output_lock);
        if (!in_flight) {
            break;
        }
        if (pending_status != noErr) {
            /* The async output failed: clear the slot and retry with a fresh
             * frame instead of waiting forever. */
            os_unfair_lock_lock(&decoder->output_lock);
            decoder->frame_in_flight = false;
            decoder->output_status = noErr;
            os_unfair_lock_unlock(&decoder->output_lock);
            break;
        }
        if (dispatch_semaphore_wait(
                decoder->output_semaphore, DISPATCH_TIME_NOW
            ) != 0) {
            /* A decode that never produces a callback would leave
             * frame_in_flight stuck forever, turning every later frame into
             * WOULD_BLOCK (observed as a repeated frame-19 retry loop and a
             * 3 s stall watchdog fallback). After 250 ms, drop the stuck
             * slot and submit the new frame instead. */
            if (grd_now_micros() - decoder->in_flight_since_micros >
                250000ULL) {
                os_unfair_lock_lock(&decoder->output_lock);
                decoder->frame_in_flight = false;
                decoder->output_status = noErr;
                os_unfair_lock_unlock(&decoder->output_lock);
                /* A frame that never completes must not be re-submitted
                 * into a busy session (duplicate in-flight submissions of
                 * the same sample were observed to trigger
                 * kVTVideoDecoderBadDataErr). Invalidate the session so the
                 * next keyframe starts completely fresh. */
                if (decoder->session != NULL) {
                    VTDecompressionSessionInvalidate(decoder->session);
                    CFRelease(decoder->session);
                    decoder->session = NULL;
                    if (decoder->format != NULL) {
                        CFRelease(decoder->format);
                        decoder->format = NULL;
                    }
                    decoder->needs_parameter_sets = true;
                    vt_warn_throttled(
                        "VT output stalled: session invalidated, "
                        "waiting for a new keyframe"
                    );
                }
                break;
            }
            return GRD_WOULD_BLOCK;
        }
    }
    grd_h264_nal nals[128];
    const size_t nal_count = collect_nals(data, size, nals, 128U);
    if (nal_count == 0U) {
        set_error(error, GRD_PROTOCOL_ERROR, "Invalid H.264 packet");
        return GRD_PROTOCOL_ERROR;
    }
    const grd_h264_nal *sps = NULL;
    const grd_h264_nal *pps = NULL;
    for (size_t index = 0U; index < nal_count; ++index) {
        if (nals[index].size == 0U) continue;
        const uint8_t type = nals[index].data[0] & 0x1FU;
        if (type == 7U) sps = &nals[index];
        if (type == 8U) pps = &nals[index];
    }
    if (decoder->session == NULL && (sps == NULL || pps == NULL)) {
        return vt_would_block(
            decoder, "waiting for in-band SPS/PPS"
        );
    }
    if (sps != NULL && pps != NULL &&
        (decoder->session == NULL ||
         !sps_compatible(
             decoder->sps, decoder->sps_size, sps->data, sps->size
         ) ||
         !same_bytes(
             decoder->pps, decoder->pps_size, pps->data, pps->size
         ))) {
        /* Keep session recreation observable without dumping frame data. */
        const bool no_session = decoder->session == NULL;
        const bool sps_diff =
            !same_bytes(decoder->sps, decoder->sps_size, sps->data, sps->size);
        const bool pps_diff =
            !same_bytes(decoder->pps, decoder->pps_size, pps->data, pps->size);
        GRD_INFO(
            "VT create_session: no_session=%d sps_diff=%d pps_diff=%d "
            "sps_size=%zu/%zu pps_size=%zu/%zu sps0=%02x%02x%02x%02x%02x",
            no_session ? 1 : 0,
            sps_diff ? 1 : 0,
            pps_diff ? 1 : 0,
            decoder->sps_size,
            sps->size,
            decoder->pps_size,
            pps->size,
            sps->size >= 5U ? sps->data[0] : 0U,
            sps->size >= 5U ? sps->data[1] : 0U,
            sps->size >= 5U ? sps->data[2] : 0U,
            sps->size >= 5U ? sps->data[3] : 0U,
            sps->size >= 5U ? sps->data[4] : 0U
        );
        OSStatus format_status = noErr;
        if (decoder->prefer_bgra) {
            /* Software renderer: no IOSurface wrap, so a single-plane BGRA
             * buffer is uploaded with SDL_UpdateTexture. */
            format_status = create_session(
                decoder,
                sps,
                pps,
                kCVPixelFormatType_32BGRA
            );
        } else {
            /* Metal: prefer 10-bit P010 for banding-free gradients, fall
             * back to 8-bit NV12: both are bi-planar IOSurface formats the
             * shader renders directly (no BGRA conversion, no CPU copy). */
            format_status = create_session(
                decoder,
                sps,
                pps,
                kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
            );
        }
        if (format_status != noErr) {
            format_status = create_session(
                decoder,
                sps,
                pps,
                kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
            );
        }
        if (format_status != noErr) {
            vt_warn_throttled(
                "session creation failed (P010/NV12): %d",
                (int)format_status
            );
            set_error(error, GRD_ERROR, "VideoToolbox session is unavailable");
            return GRD_ERROR;
        } else {
            GRD_INFO(
                "VT decoder: session created (SPS %zu B, PPS %zu B, "
                "formato %u)",
                sps->size,
                pps->size,
                decoder->pixel_format
            );
        }
    }
    size_t avcc_size = 0U;
    for (size_t index = 0U; index < nal_count; ++index) {
        if (nals[index].size == 0U) continue;
        const uint8_t nal_type = nals[index].data[0] & 0x1FU;
        if (nal_type == 7U || nal_type == 8U) {
            /* The CMVideoFormatDescription already carries SPS/PPS: they
             * must NOT appear in the sample (standard AVCC usage). Keeping
             * them in-band while the session was created from a compatible
             * but byte-different SPS is what made VideoToolbox reject the
             * keyframe with kVTVideoDecoderBadDataErr. */
            continue;
        }
        if (nals[index].size > UINT32_MAX - 4U ||
            avcc_size > SIZE_MAX - nals[index].size - 4U) {
            return GRD_INVALID_ARGUMENT;
        }
        avcc_size += nals[index].size + 4U;
    }
    if (decoder->avcc_capacity < avcc_size) {
        uint8_t *resized = realloc(decoder->avcc_buffer, avcc_size);
        if (resized == NULL) {
            return GRD_OUT_OF_MEMORY;
        }
        decoder->avcc_buffer = resized;
        decoder->avcc_capacity = avcc_size;
    }
    uint8_t *avcc = decoder->avcc_buffer;
    if (avcc == NULL) {
        return GRD_OUT_OF_MEMORY;
    }
    size_t avcc_offset = 0U;
    for (size_t index = 0U; index < nal_count; ++index) {
        if (nals[index].size == 0U) continue;
        const uint8_t nal_type = nals[index].data[0] & 0x1FU;
        if (nal_type == 7U || nal_type == 8U) {
            continue;
        }
        const uint32_t nal_size = (uint32_t)nals[index].size;
        avcc[avcc_offset] = (uint8_t)(nal_size >> 24U);
        avcc[avcc_offset + 1U] = (uint8_t)(nal_size >> 16U);
        avcc[avcc_offset + 2U] = (uint8_t)(nal_size >> 8U);
        avcc[avcc_offset + 3U] = (uint8_t)nal_size;
        memcpy(avcc + avcc_offset + 4U, nals[index].data, nals[index].size);
        avcc_offset += nals[index].size + 4U;
    }
    CMBlockBufferRef block = NULL;
    CMBlockBufferCustomBlockSource block_source;
    memset(&block_source, 0, sizeof(block_source));
    block_source.version = 0;
    block_source.FreeBlock = release_block_buffer;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault,
        avcc,
        avcc_size,
        kCFAllocatorNull,
        &block_source,
        0U,
        avcc_size,
        0U,
        &block
    );
    if (status != kCMBlockBufferNoErr) {
        return GRD_ERROR;
    }
    const size_t sample_size = avcc_size;
    CMSampleBufferRef sample = NULL;
    status = CMSampleBufferCreate(
        kCFAllocatorDefault,
        block,
        true,
        NULL,
        NULL,
        decoder->format,
        1U,
        0U,
        NULL,
        1U,
        &sample_size,
        &sample
    );
    if (status == noErr) {
        os_unfair_lock_lock(&decoder->output_lock);
        decoder->output_status = noErr;
        decoder->frame_in_flight = true;
        decoder->in_flight_since_micros = grd_now_micros();
        os_unfair_lock_unlock(&decoder->output_lock);
        VTDecodeInfoFlags flags = 0U;
        status = VTDecompressionSessionDecodeFrame(
            decoder->session,
            sample,
            kVTDecodeFrame_EnableAsynchronousDecompression,
            NULL,
            &flags
        );
        if (status == noErr) {
            const dispatch_time_t deadline = dispatch_time(
                DISPATCH_TIME_NOW, 30LL * NSEC_PER_MSEC
            );
            if (dispatch_semaphore_wait(decoder->output_semaphore, deadline) == 0) {
                os_unfair_lock_lock(&decoder->output_lock);
                status = decoder->output_status;
                if (status == noErr && decoder->output_ready) {
                    *frame = decoder->output;
                    memset(&decoder->output, 0, sizeof(decoder->output));
                    decoder->output_ready = false;
                    decoder->frame_in_flight = false;
                } else if (status != noErr) {
                    decoder->frame_in_flight = false;
                }
                os_unfair_lock_unlock(&decoder->output_lock);
            }
        } else {
            vt_warn_throttled(
                "DecodeFrame failed: %d", (int)status
            );
            os_unfair_lock_lock(&decoder->output_lock);
            decoder->frame_in_flight = false;
            os_unfair_lock_unlock(&decoder->output_lock);
        }
    }
    if (sample != NULL) CFRelease(sample);
    CFRelease(block);
    if (status != noErr) {
        if (status == kVTVideoDecoderBadDataErr) {
            set_error(error, GRD_PROTOCOL_ERROR, "H.264 frame rejected by VideoToolbox");
        } else {
            set_error(error, GRD_ERROR, "VideoToolbox decoding failed");
        }
        return GRD_ERROR;
    }
    if (frame->data == NULL) {
        return vt_would_block(decoder, "no output from callback");
    }
    decoder->wouldblock_streak = 0U;
    return GRD_OK;
}

void grd_videotoolbox_decoder_destroy(grd_videotoolbox_decoder *decoder)
{
    if (decoder == NULL) return;
    release_output(decoder);
    if (decoder->session != NULL) {
        /* Drain once during teardown so the callback cannot outlive the
         * decoder. The hot path never calls this blocking API. */
        (void)VTDecompressionSessionWaitForAsynchronousFrames(
            decoder->session
        );
        VTDecompressionSessionInvalidate(decoder->session);
        CFRelease(decoder->session);
    }
    if (decoder->format != NULL) CFRelease(decoder->format);
    free(decoder->sps);
    free(decoder->pps);
    free(decoder->avcc_buffer);
#if !OS_OBJECT_USE_OBJC
    if (decoder->output_semaphore != NULL) {
        dispatch_release(decoder->output_semaphore);
    }
#endif
    free(decoder);
}

struct grd_videotoolbox_encoder {
    VTCompressionSessionRef session;
    dispatch_semaphore_t output_semaphore;
    os_unfair_lock output_lock;
    uint8_t *output_data;
    size_t output_size;
    bool output_keyframe;
    OSStatus output_status;
    bool frame_in_flight;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    int64_t next_pts;
};

static void encoder_set_error(
    grd_error *error,
    grd_status code,
    const char *message
)
{
    if (error != NULL) {
        error->code = code;
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
}

static void encoder_release_output_locked(grd_videotoolbox_encoder *encoder)
{
    free(encoder->output_data);
    encoder->output_data = NULL;
    encoder->output_size = 0U;
    encoder->output_keyframe = false;
}

static bool sample_is_keyframe(CMSampleBufferRef sample)
{
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(
        sample, false
    );
    if (attachments == NULL || CFArrayGetCount(attachments) == 0) {
        return true;
    }
    CFDictionaryRef dictionary = (CFDictionaryRef)CFArrayGetValueAtIndex(
        attachments, 0
    );
    const void *not_sync = dictionary != NULL
                               ? CFDictionaryGetValue(
                                     dictionary,
                                     kCMSampleAttachmentKey_NotSync
                                 )
                               : NULL;
    return not_sync != kCFBooleanTrue;
}

static bool h264_parameter_set(
    CMFormatDescriptionRef format,
    size_t index,
    const uint8_t **data,
    size_t *size
)
{
    size_t parameter_size = 0U;
    size_t parameter_count = 0U;
    OSStatus status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
        format,
        index,
        data,
        &parameter_size,
        &parameter_count,
        NULL
    );
    if (status != noErr || data == NULL || *data == NULL || parameter_size == 0U) {
        return false;
    }
    *size = parameter_size;
    return true;
}

static void encoder_output_callback(
    void *refcon,
    void *source_frame_refcon,
    OSStatus status,
    VTEncodeInfoFlags info_flags,
    CMSampleBufferRef sample_buffer
)
{
    (void)source_frame_refcon;
    (void)info_flags;
    grd_videotoolbox_encoder *encoder = refcon;
    os_unfair_lock_lock(&encoder->output_lock);
    encoder->output_status = status;
    if (status == noErr && sample_buffer != NULL) {
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample_buffer);
        const size_t block_size = block != NULL
                                      ? CMBlockBufferGetDataLength(block)
                                      : 0U;
        const bool keyframe = sample_is_keyframe(sample_buffer);
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(
            sample_buffer
        );
        const uint8_t *sps = NULL;
        const uint8_t *pps = NULL;
        size_t sps_size = 0U;
        size_t pps_size = 0U;
        if (keyframe && format != NULL) {
            (void)h264_parameter_set(format, 0U, &sps, &sps_size);
            (void)h264_parameter_set(format, 1U, &pps, &pps_size);
        }
        const size_t parameter_size =
            (sps_size <= UINT32_MAX && pps_size <= UINT32_MAX)
                ? sps_size + pps_size + (sps_size != 0U ? 4U : 0U) +
                      (pps_size != 0U ? 4U : 0U)
                : 0U;
        if (block != NULL && block_size <= SIZE_MAX - parameter_size) {
            const size_t output_size = parameter_size + block_size;
            uint8_t *output = malloc(output_size);
            if (output != NULL) {
                size_t offset = 0U;
                if (sps_size != 0U) {
                    output[offset] = (uint8_t)(sps_size >> 24U);
                    output[offset + 1U] = (uint8_t)(sps_size >> 16U);
                    output[offset + 2U] = (uint8_t)(sps_size >> 8U);
                    output[offset + 3U] = (uint8_t)sps_size;
                    offset += 4U;
                    memcpy(output + offset, sps, sps_size);
                    offset += sps_size;
                }
                if (pps_size != 0U) {
                    output[offset] = (uint8_t)(pps_size >> 24U);
                    output[offset + 1U] = (uint8_t)(pps_size >> 16U);
                    output[offset + 2U] = (uint8_t)(pps_size >> 8U);
                    output[offset + 3U] = (uint8_t)pps_size;
                    offset += 4U;
                    memcpy(output + offset, pps, pps_size);
                    offset += pps_size;
                }
                if (CMBlockBufferCopyDataBytes(
                        block, 0U, block_size, output + offset
                    ) == kCMBlockBufferNoErr) {
                    encoder_release_output_locked(encoder);
                    encoder->output_data = output;
                    encoder->output_size = output_size;
                    encoder->output_keyframe = keyframe;
                    output = NULL;
                }
                free(output);
            }
        }
        if (encoder->output_status == noErr && encoder->output_data == NULL) {
            encoder->output_status = kVTAllocationFailedErr;
        }
    }
    os_unfair_lock_unlock(&encoder->output_lock);
    dispatch_semaphore_signal(encoder->output_semaphore);
}

static void set_number(
    CFMutableDictionaryRef dictionary,
    CFStringRef key,
    int32_t value
)
{
    CFNumberRef number = CFNumberCreate(
        kCFAllocatorDefault, kCFNumberSInt32Type, &value
    );
    if (number != NULL) {
        CFDictionarySetValue(dictionary, key, number);
        CFRelease(number);
    }
}

grd_videotoolbox_encoder *grd_videotoolbox_encoder_create(
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint32_t bitrate_kbps,
    grd_error *error
)
{
    if (width == 0U || height == 0U || (width & 1U) != 0U ||
        (height & 1U) != 0U || fps == 0U) {
        encoder_set_error(error, GRD_INVALID_ARGUMENT, "Invalid VideoToolbox dimensions");
        return NULL;
    }
    grd_videotoolbox_encoder *encoder = calloc(1U, sizeof(*encoder));
    if (encoder == NULL) {
        encoder_set_error(error, GRD_OUT_OF_MEMORY, "Unable to allocate VideoToolbox encoder");
        return NULL;
    }
    encoder->width = width;
    encoder->height = height;
    encoder->fps = fps;
    encoder->output_lock = OS_UNFAIR_LOCK_INIT;
    encoder->output_semaphore = dispatch_semaphore_create(0);
    if (encoder->output_semaphore == NULL) {
        free(encoder);
        encoder_set_error(error, GRD_OUT_OF_MEMORY, "Unable to allocate VideoToolbox synchronization");
        return NULL;
    }

    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault,
        3,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    );
    if (attributes == NULL) {
        grd_videotoolbox_encoder_destroy(encoder);
        encoder_set_error(error, GRD_OUT_OF_MEMORY, "Unable to allocate VideoToolbox attributes");
        return NULL;
    }
    int32_t pixel_format = (int32_t)kCVPixelFormatType_32BGRA;
    CFNumberRef pixel_format_number = CFNumberCreate(
        kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format
    );
    if (pixel_format_number != NULL) {
        CFDictionarySetValue(
            attributes, kCVPixelBufferPixelFormatTypeKey, pixel_format_number
        );
        CFRelease(pixel_format_number);
    }
    set_number(attributes, kCVPixelBufferWidthKey, (int32_t)width);
    set_number(attributes, kCVPixelBufferHeightKey, (int32_t)height);
    CFMutableDictionaryRef io_surface = CFDictionaryCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    );
    if (io_surface != NULL) {
        CFDictionarySetValue(
            attributes, kCVPixelBufferIOSurfacePropertiesKey, io_surface
        );
        CFRelease(io_surface);
    }

    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        (int32_t)width,
        (int32_t)height,
        kCMVideoCodecType_H264,
        NULL,
        attributes,
        NULL,
        encoder_output_callback,
        encoder,
        &encoder->session
    );
    CFRelease(attributes);
    if (status != noErr || encoder->session == NULL) {
        grd_videotoolbox_encoder_destroy(encoder);
        encoder_set_error(error, GRD_NOT_SUPPORTED, "VideoToolbox H.264 encoder is unavailable");
        return NULL;
    }

    int32_t expected_fps = (int32_t)fps;
    int32_t average_bitrate = (int32_t)(bitrate_kbps * 1000U);
    int32_t key_interval = (int32_t)(fps * 2U);
    (void)VTSessionSetProperty(
        encoder->session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue
    );
    (void)VTSessionSetProperty(
        encoder->session,
        kVTCompressionPropertyKey_AllowFrameReordering,
        kCFBooleanFalse
    );
    (void)VTSessionSetProperty(
        encoder->session,
        kVTCompressionPropertyKey_ProfileLevel,
        kVTProfileLevel_H264_Main_AutoLevel
    );
    CFNumberRef fps_number = CFNumberCreate(
        kCFAllocatorDefault, kCFNumberSInt32Type, &expected_fps
    );
    CFNumberRef bitrate_number = CFNumberCreate(
        kCFAllocatorDefault, kCFNumberSInt32Type, &average_bitrate
    );
    CFNumberRef interval_number = CFNumberCreate(
        kCFAllocatorDefault, kCFNumberSInt32Type, &key_interval
    );
    if (fps_number != NULL) {
        (void)VTSessionSetProperty(
            encoder->session, kVTCompressionPropertyKey_ExpectedFrameRate,
            fps_number
        );
        CFRelease(fps_number);
    }
    if (bitrate_number != NULL) {
        (void)VTSessionSetProperty(
            encoder->session, kVTCompressionPropertyKey_AverageBitRate,
            bitrate_number
        );
        CFRelease(bitrate_number);
    }
    if (interval_number != NULL) {
        (void)VTSessionSetProperty(
            encoder->session, kVTCompressionPropertyKey_MaxKeyFrameInterval,
            interval_number
        );
        CFRelease(interval_number);
    }
    status = VTCompressionSessionPrepareToEncodeFrames(encoder->session);
    if (status != noErr) {
        grd_videotoolbox_encoder_destroy(encoder);
        encoder_set_error(error, GRD_ERROR, "Failed to prepare VideoToolbox encoder");
        return NULL;
    }
    CFTypeRef hardware_value = NULL;
    status = VTSessionCopyProperty(
        encoder->session,
        kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
        kCFAllocatorDefault,
        &hardware_value
    );
    if (status == noErr && hardware_value != NULL &&
        CFGetTypeID(hardware_value) == CFBooleanGetTypeID() &&
        hardware_value != kCFBooleanTrue) {
        CFRelease(hardware_value);
        grd_videotoolbox_encoder_destroy(encoder);
        encoder_set_error(error, GRD_NOT_SUPPORTED, "VideoToolbox is not using the hardware encoder");
        return NULL;
    }
    if (hardware_value != NULL) {
        CFRelease(hardware_value);
    }
    return encoder;
}

grd_status grd_videotoolbox_encoder_encode(
    grd_videotoolbox_encoder *encoder,
    const grd_frame *frame,
    bool force_keyframe,
    uint8_t **data,
    size_t *size,
    bool *keyframe,
    grd_error *error
)
{
    if (encoder == NULL || frame == NULL || data == NULL || size == NULL ||
        keyframe == NULL || frame->format != GRD_PIXEL_BGRA8 ||
        frame->data == NULL || frame->width != encoder->width ||
        frame->height != encoder->height || frame->stride < frame->width * 4U) {
        encoder_set_error(error, GRD_NOT_SUPPORTED, "Frame is not native to VideoToolbox");
        return GRD_NOT_SUPPORTED;
    }

    /* A previous frame may still be encoding: deliver its output now instead
     * of discarding it. Dropping outputs makes a slow frame look like a lost
     * reference and freezes the client until a keyframe is forced. */
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        os_unfair_lock_lock(&encoder->output_lock);
        if (encoder->output_data != NULL) {
            *data = encoder->output_data;
            *size = encoder->output_size;
            *keyframe = encoder->output_keyframe;
            encoder->output_data = NULL;
            encoder->output_size = 0U;
            encoder->output_keyframe = false;
            encoder->frame_in_flight = false;
            os_unfair_lock_unlock(&encoder->output_lock);
            return GRD_OK;
        }
        const bool in_flight = encoder->frame_in_flight;
        const OSStatus pending_status = encoder->output_status;
        os_unfair_lock_unlock(&encoder->output_lock);
        if (!in_flight) {
            break;
        }
        if (pending_status != noErr) {
            /* The async output failed: clear the slot and retry with a fresh
             * frame instead of waiting forever for an output that will never
             * arrive. */
            os_unfair_lock_lock(&encoder->output_lock);
            encoder->frame_in_flight = false;
            encoder->output_status = noErr;
            os_unfair_lock_unlock(&encoder->output_lock);
            break;
        }
        if (dispatch_semaphore_wait(
                encoder->output_semaphore, DISPATCH_TIME_NOW
            ) != 0) {
            encoder_set_error(error, GRD_WOULD_BLOCK, "VideoToolbox encoder is delayed");
            return GRD_WOULD_BLOCK;
        }
    }

    CVPixelBufferRef pixel_buffer = NULL;
    if (frame->owner != NULL) {
        pixel_buffer = (CVPixelBufferRef)CFRetain((CVPixelBufferRef)frame->owner);
    } else {
        if (CVPixelBufferCreateWithBytes(
                kCFAllocatorDefault,
                frame->width,
                frame->height,
                kCVPixelFormatType_32BGRA,
                frame->data,
                frame->stride,
                NULL,
                NULL,
                NULL,
                &pixel_buffer
            ) != kCVReturnSuccess) {
            encoder_set_error(error, GRD_OUT_OF_MEMORY, "Unable to allocate BGRA CVPixelBuffer");
            return GRD_OUT_OF_MEMORY;
        }
    }
    if (pixel_buffer == NULL) {
        encoder_set_error(error, GRD_ERROR, "BGRA CVPixelBuffer is unavailable");
        return GRD_ERROR;
    }

    CFDictionaryRef frame_properties = NULL;
    if (force_keyframe) {
        const void *keys[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
        const void *values[] = {kCFBooleanTrue};
        frame_properties = CFDictionaryCreate(
            kCFAllocatorDefault,
            keys,
            values,
            1U,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks
        );
    }
    const CMTime timestamp = CMTimeMake(
        encoder->next_pts++, (int32_t)encoder->fps
    );
    os_unfair_lock_lock(&encoder->output_lock);
    encoder->output_status = noErr;
    encoder->frame_in_flight = true;
    os_unfair_lock_unlock(&encoder->output_lock);
    OSStatus status = VTCompressionSessionEncodeFrame(
        encoder->session,
        pixel_buffer,
        timestamp,
        kCMTimeInvalid,
        frame_properties,
        NULL,
        NULL
    );
    if (frame_properties != NULL) {
        CFRelease(frame_properties);
    }
    CFRelease(pixel_buffer);
    if (status != noErr) {
        os_unfair_lock_lock(&encoder->output_lock);
        encoder->frame_in_flight = false;
        os_unfair_lock_unlock(&encoder->output_lock);
        encoder_set_error(error, GRD_ERROR, "VideoToolbox encoding failed");
        return GRD_ERROR;
    }

    const dispatch_time_t deadline = dispatch_time(
        DISPATCH_TIME_NOW, 30LL * NSEC_PER_MSEC
    );
    if (dispatch_semaphore_wait(encoder->output_semaphore, deadline) == 0) {
        os_unfair_lock_lock(&encoder->output_lock);
        status = encoder->output_status;
        if (status == noErr && encoder->output_data != NULL) {
            *data = encoder->output_data;
            *size = encoder->output_size;
            *keyframe = encoder->output_keyframe;
            encoder->output_data = NULL;
            encoder->output_size = 0U;
            encoder->output_keyframe = false;
            encoder->frame_in_flight = false;
        }
        os_unfair_lock_unlock(&encoder->output_lock);
    }
    if (status != noErr) {
        os_unfair_lock_lock(&encoder->output_lock);
        encoder->frame_in_flight = false;
        os_unfair_lock_unlock(&encoder->output_lock);
        encoder_set_error(error, GRD_ERROR, "VideoToolbox output is unavailable");
        return GRD_ERROR;
    }
    if (*data == NULL || *size == 0U) {
        /* The frame is still encoding; the pending output is collected by
         * the next call instead of being discarded. */
        encoder_set_error(error, GRD_WOULD_BLOCK, "VideoToolbox encoder is delayed");
        return GRD_WOULD_BLOCK;
    }
    return GRD_OK;
}

grd_status grd_videotoolbox_encoder_set_bitrate(
    grd_videotoolbox_encoder *encoder,
    uint32_t bitrate_kbps
)
{
    if (encoder == NULL || encoder->session == NULL || bitrate_kbps == 0U) {
        return GRD_INVALID_ARGUMENT;
    }
    const int32_t average_bitrate = (int32_t)(bitrate_kbps * 1000U);
    CFNumberRef bitrate_number = CFNumberCreate(
        kCFAllocatorDefault, kCFNumberSInt32Type, &average_bitrate
    );
    if (bitrate_number == NULL) {
        return GRD_OUT_OF_MEMORY;
    }
    const OSStatus status = VTSessionSetProperty(
        encoder->session,
        kVTCompressionPropertyKey_AverageBitRate,
        bitrate_number
    );
    CFRelease(bitrate_number);
    return status == noErr ? GRD_OK : GRD_ERROR;
}

void grd_videotoolbox_encoder_destroy(grd_videotoolbox_encoder *encoder)
{
    if (encoder == NULL) {
        return;
    }
    if (encoder->session != NULL) {
        (void)VTCompressionSessionCompleteFrames(encoder->session, kCMTimeInvalid);
        VTCompressionSessionInvalidate(encoder->session);
        CFRelease(encoder->session);
    }
    os_unfair_lock_lock(&encoder->output_lock);
    encoder_release_output_locked(encoder);
    os_unfair_lock_unlock(&encoder->output_lock);
#if !OS_OBJECT_USE_OBJC
    if (encoder->output_semaphore != NULL) {
        dispatch_release(encoder->output_semaphore);
    }
#endif
    free(encoder);
}
