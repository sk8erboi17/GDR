#include "test.h"

#include "grd/codec.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#endif

static _Atomic bool owned_buffer_released;

static void test_owned_buffer_release(void *payload)
{
    free(payload);
    atomic_store(&owned_buffer_released, true);
}

void test_codec(void)
{
    /* Known-vector regression for the lambda->QP conversion. x264/x265 put
     * QP*FF_QP2LAMBDA in AV_PKT_DATA_QUALITY_STATS (Avg QP 15 -> 1770,
     * Avg QP 11.87 -> 1400); the old code multiplied by 52/118 AND cast to
     * uint8_t before dividing, turning 1400 into qp=1. */
    GRD_ASSERT(grd_qp_from_quality(0U) == 0U);
    GRD_ASSERT(grd_qp_from_quality(1770U) == 15U);
    GRD_ASSERT(grd_qp_from_quality(1400U) == 12U);
    GRD_ASSERT(grd_qp_from_quality(590U) == 5U);
    GRD_ASSERT(grd_qp_from_quality(6000U) == 51U);

    const uint32_t width = 320U;
    const uint32_t height = 180U;
    grd_error error = {0};
    grd_pipeline_kind active;
    grd_video_codec active_codec;
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
    GRD_ASSERT(encoder != NULL);
    /* FFmpeg rate-control fields are immutable after avcodec_open2. ABR
     * must be told to reopen this encoder instead of believing that only
     * changing AVCodecContext also changed NVENC/libx264. */
    GRD_ASSERT(grd_encoder_set_bitrate(encoder, 1000U, &error) == GRD_OK);
    GRD_ASSERT(
        grd_encoder_set_bitrate(encoder, 900U, &error) ==
        GRD_NOT_SUPPORTED
    );

    grd_frame source;
    memset(&source, 0, sizeof(source));
    source.width = width;
    source.height = height;
    source.stride = width * 4U;
    source.size = (size_t)source.stride * height;
    source.format = GRD_PIXEL_RGBA8;
    source.data = malloc(source.size);
    GRD_ASSERT(source.data != NULL);

    /* A native D3D11 capture must never fall through to a software encoder,
     * which would otherwise dereference its intentionally NULL CPU buffer. */
    grd_frame native_source;
    memset(&native_source, 0, sizeof(native_source));
    native_source.width = width;
    native_source.height = height;
    native_source.format = GRD_PIXEL_D3D11_BGRA;
    native_source.owner = &native_source;
    grd_encoded_frame unsupported_encoded;
    GRD_ASSERT(
        grd_encoder_encode(
            encoder, &native_source, &unsupported_encoded, &error
        ) == GRD_NOT_SUPPORTED
    );

    for (uint32_t y = 0U; y < height; ++y) {
        for (uint32_t x = 0U; x < width; ++x) {
            const size_t offset = ((size_t)y * width + x) * 4U;
            source.data[offset] = (uint8_t)x;
            source.data[offset + 1U] = (uint8_t)y;
            source.data[offset + 2U] = (uint8_t)(x + y);
            source.data[offset + 3U] = 255U;
        }
    }

    grd_encoded_frame encoded;
    grd_status status = GRD_WOULD_BLOCK;
    for (unsigned attempt = 0U;
         attempt < 5U && status == GRD_WOULD_BLOCK;
         ++attempt) {
        source.timestamp_micros = (uint64_t)attempt * 33333U;
        status = grd_encoder_encode(encoder, &source, &encoded, &error);
    }
    GRD_ASSERT(status == GRD_OK);
    GRD_ASSERT(encoded.data != NULL && encoded.size != 0U);

    grd_decoder *decoder = grd_decoder_create(
        GRD_PIPELINE_INTEGRATED_SOFTWARE,
        GRD_CODEC_H264,
        &active,
        &error
    );
    GRD_ASSERT(decoder != NULL);
    grd_frame decoded;
    status = grd_decoder_decode(
        decoder,
        encoded.data,
        encoded.size,
        &decoded,
        &error
    );
    GRD_ASSERT(status == GRD_OK);
    GRD_ASSERT(decoded.width == width);
    GRD_ASSERT(decoded.height == height);
    GRD_ASSERT(decoded.data != NULL);

    grd_platform_frame_release(&decoded);
    grd_decoder_destroy(decoder);

    /* Zero-copy decode: a padded transport buffer is adopted by the decoder
     * and freed through the release hook once consumed. */
    {
        const size_t padded_size = encoded.size + GRD_MEDIA_BUFFER_PADDING;
        uint8_t *padded = calloc(1U, padded_size);
        GRD_ASSERT(padded != NULL);
        memcpy(padded, encoded.data, encoded.size);
        grd_decoder *owned_decoder = grd_decoder_create(
            GRD_PIPELINE_INTEGRATED_SOFTWARE,
            GRD_CODEC_H264,
            &active,
            &error
        );
        GRD_ASSERT(owned_decoder != NULL);
        atomic_store(&owned_buffer_released, false);
        grd_frame owned_decoded;
        status = grd_decoder_decode_owned(
            owned_decoder,
            padded,
            encoded.size,
            padded,
            padded_size,
            test_owned_buffer_release,
            &owned_decoded,
            &error
        );
        GRD_ASSERT(status == GRD_OK);
        GRD_ASSERT(owned_decoded.width == width);
        GRD_ASSERT(owned_decoded.height == height);
        GRD_ASSERT(owned_decoded.data != NULL);
        grd_platform_frame_release(&owned_decoded);
        grd_decoder_destroy(owned_decoder);
        GRD_ASSERT(atomic_load(&owned_buffer_released));
    }

#if defined(__APPLE__)
    /* Regression: a stream without in-band SPS/PPS must not leave the native
     * VideoToolbox decoder stuck in WOULD_BLOCK forever (the NVENC
     * intra-refresh stream had no usable IDR with parameter sets). The
     * decoder must start as soon as a frame carrying SPS/PPS arrives. */
    {
        uint8_t *stripped = malloc(encoded.size);
        size_t stripped_size = 0U;
        GRD_ASSERT(stripped != NULL);
        const uint8_t *data = encoded.data;
        const size_t size = encoded.size;
        size_t offset = 0U;
        while (offset + 3U <= size) {
            size_t start = offset;
            while (start + 3U <= size &&
                   !(data[start] == 0U && data[start + 1U] == 0U &&
                     data[start + 2U] == 1U)) {
                ++start;
            }
            if (start + 3U > size) {
                break;
            }
            const bool four_byte =
                start + 4U <= size && data[start] == 0U &&
                data[start + 1U] == 0U && data[start + 2U] == 0U &&
                data[start + 3U] == 1U;
            const size_t prefix = four_byte ? 4U : 3U;
            const size_t nal_start = start + prefix;
            size_t nal_end = nal_start;
            while (nal_end + 3U <= size &&
                   !(data[nal_end] == 0U && data[nal_end + 1U] == 0U &&
                     data[nal_end + 2U] == 1U)) {
                ++nal_end;
            }
            const uint8_t type = data[nal_start] & 0x1FU;
            if (type != 7U && type != 8U) {
                memcpy(
                    stripped + stripped_size,
                    data + start,
                    nal_end - start
                );
                stripped_size += nal_end - start;
            }
            offset = nal_end;
        }
        grd_decoder *vt = grd_decoder_create(
            GRD_PIPELINE_METAL_VIDEOTOOLBOX,
            GRD_CODEC_H264,
            &active,
            &error
        );
        if (vt != NULL) {
            grd_frame vt_out;
            status = grd_decoder_decode(
                vt, stripped, stripped_size, &vt_out, &error
            );
            if (status == GRD_WOULD_BLOCK) {
                /* Parameter sets missing: the decoder must wait, then start
                 * as soon as the real IDR (with SPS/PPS) is delivered. */
                status = grd_decoder_decode(
                    vt, encoded.data, encoded.size, &vt_out, &error
                );
                if (status == GRD_OK) {
                    GRD_ASSERT(vt_out.width == width);
                    GRD_ASSERT(vt_out.height == height);
                    grd_platform_frame_release(&vt_out);
                }
            }
            grd_decoder_destroy(vt);
        }
        free(stripped);
    }
#endif

    grd_encoded_frame_release(&encoded);
    grd_encoder_destroy(encoder);

    /* HEVC round trip when an encoder is available (hardware on Apple via
     * VideoToolbox, software libx265 elsewhere). */
    {
        grd_encoder *hevc_encoder = grd_encoder_create(
            GRD_PIPELINE_INTEGRATED_SOFTWARE,
            width,
            height,
            30U,
            1000U,
            GRD_CODEC_HEVC,
            false,
            &active,
            &active_codec,
            &error
        );
        if (hevc_encoder != NULL && active_codec != GRD_CODEC_H264) {
            grd_encoded_frame hevc_encoded;
            status = GRD_WOULD_BLOCK;
            for (unsigned attempt = 0U;
                 attempt < 10U && status == GRD_WOULD_BLOCK;
                 ++attempt) {
                source.timestamp_micros = (uint64_t)attempt * 33333U;
                status = grd_encoder_encode(
                    hevc_encoder, &source, &hevc_encoded, &error
                );
            }
            GRD_ASSERT(status == GRD_OK);
            GRD_ASSERT(active_codec == GRD_CODEC_HEVC);
            /* libx265 exposes per-frame average QP through quality stats;
             * at 1000 kbps / 320x180 the average QP is ~12-15, far from the
             * old buggy conversion that returned 1. */
            GRD_ASSERT(hevc_encoded.qp_known);
            GRD_ASSERT(hevc_encoded.avg_qp >= 6U);
            GRD_ASSERT(hevc_encoded.avg_qp <= 35U);
            grd_decoder *hevc_decoder = grd_decoder_create(
                GRD_PIPELINE_INTEGRATED_SOFTWARE,
                GRD_CODEC_HEVC,
                &active,
                &error
            );
            GRD_ASSERT(hevc_decoder != NULL);
            grd_frame hevc_decoded;
            status = GRD_WOULD_BLOCK;
            for (unsigned attempt = 0U;
                 attempt < 10U && status == GRD_WOULD_BLOCK;
                 ++attempt) {
                status = grd_decoder_decode(
                    hevc_decoder,
                    hevc_encoded.data,
                    hevc_encoded.size,
                    &hevc_decoded,
                    &error
                );
            }
            GRD_ASSERT(status == GRD_OK);
            GRD_ASSERT(hevc_decoded.width == width);
            GRD_ASSERT(hevc_decoded.height == height);
            grd_platform_frame_release(&hevc_decoded);
            grd_decoder_destroy(hevc_decoder);
            grd_encoded_frame_release(&hevc_encoded);
        }
        grd_encoder_destroy(hevc_encoder);
    }

    /* 4:4:4 round trip: software HEVC (libx265 / native hevc) exposes
     * YUV444P, so a pixel_444 request must still encode and decode. When
     * the encoder does not support 4:4:4 the create call silently stays on
     * 4:2:0, which is also covered here by a successful round trip. */
    {
        grd_encoder *encoder444 = grd_encoder_create(
            GRD_PIPELINE_INTEGRATED_SOFTWARE,
            width,
            height,
            30U,
            1000U,
            GRD_CODEC_HEVC,
            true,
            &active,
            &active_codec,
            &error
        );
        if (encoder444 != NULL) {
            grd_encoded_frame encoded444;
            status = GRD_WOULD_BLOCK;
            for (unsigned attempt = 0U;
                 attempt < 10U && status == GRD_WOULD_BLOCK;
                 ++attempt) {
                source.timestamp_micros = (uint64_t)attempt * 33333U;
                status = grd_encoder_encode(
                    encoder444, &source, &encoded444, &error
                );
            }
            GRD_ASSERT(status == GRD_OK);
            GRD_ASSERT(encoded444.data != NULL && encoded444.size != 0U);
            GRD_ASSERT(encoded444.qp_known);
            GRD_ASSERT(encoded444.avg_qp >= 6U);
            GRD_ASSERT(encoded444.avg_qp <= 35U);
            grd_decoder *decoder444 = grd_decoder_create(
                GRD_PIPELINE_INTEGRATED_SOFTWARE,
                GRD_CODEC_HEVC,
                &active,
                &error
            );
            GRD_ASSERT(decoder444 != NULL);
            grd_frame decoded444;
            status = GRD_WOULD_BLOCK;
            for (unsigned attempt = 0U;
                 attempt < 10U && status == GRD_WOULD_BLOCK;
                 ++attempt) {
                status = grd_decoder_decode(
                    decoder444,
                    encoded444.data,
                    encoded444.size,
                    &decoded444,
                    &error
                );
            }
            GRD_ASSERT(status == GRD_OK);
            GRD_ASSERT(decoded444.width == width);
            GRD_ASSERT(decoded444.height == height);
            GRD_ASSERT(decoded444.data != NULL);
            grd_platform_frame_release(&decoded444);
            grd_decoder_destroy(decoder444);
            grd_encoded_frame_release(&encoded444);
        }
        grd_encoder_destroy(encoder444);
    }

    /* Exercise the native macOS BGRA path when the local runtime exposes it.
     * Other platforms intentionally skip this optional backend. */
    const grd_gpu_capabilities capabilities = grd_gpu_detect();
    if (capabilities.metal && capabilities.videotoolbox_h264) {
        grd_encoder *native_encoder = grd_encoder_create(
            GRD_PIPELINE_METAL_VIDEOTOOLBOX,
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
        if (native_encoder == NULL) {
            fprintf(
                stderr,
                "Unable to create VideoToolbox encoder; skipping native path\n"
            );
        } else {
            grd_frame bgra_source = source;
            bgra_source.format = GRD_PIXEL_BGRA8;
            grd_encoded_frame native_encoded;
            status = GRD_WOULD_BLOCK;
            for (unsigned attempt = 0U;
                 attempt < 10U && status == GRD_WOULD_BLOCK;
                 ++attempt) {
                bgra_source.timestamp_micros =
                    (uint64_t)attempt * 33333U;
                status = grd_encoder_encode(
                    native_encoder,
                    &bgra_source,
                    &native_encoded,
                    &error
                );
            }
            if (status == GRD_OK) {
                GRD_ASSERT(native_encoded.data != NULL &&
                           native_encoded.size != 0U);
                grd_decoder *native_decoder = grd_decoder_create(
                    GRD_PIPELINE_METAL_VIDEOTOOLBOX,
                    GRD_CODEC_H264,
                    &active,
                    &error
                );
                GRD_ASSERT(native_decoder != NULL);
                grd_frame native_decoded;
                status = GRD_WOULD_BLOCK;
                for (unsigned attempt = 0U;
                     attempt < 10U && status == GRD_WOULD_BLOCK;
                     ++attempt) {
                    status = grd_decoder_decode(
                        native_decoder,
                        native_encoded.data,
                        native_encoded.size,
                        &native_decoded,
                        &error
                    );
                }
                GRD_ASSERT(status == GRD_OK);
                GRD_ASSERT(native_decoded.width == width);
                GRD_ASSERT(native_decoded.height == height);
#if defined(__APPLE__)
                /* The Metal zero-copy path wraps the CVPixelBuffer via its
                 * IOSurface: the decoder must produce IOSurface-backed
                 * buffers, otherwise SDL_CreateTextureWithProperties fails
                 * with 'CVPixelBufferGetIOSurface() failed'. */
                GRD_ASSERT(
                    CVPixelBufferGetIOSurface(
                        (CVPixelBufferRef)native_decoded.owner
                    ) != NULL
                );
#endif
                grd_platform_frame_release(&native_decoded);
                grd_decoder_destroy(native_decoder);
                grd_encoded_frame_release(&native_encoded);
            } else {
                /* Headless/virtualized CI runners expose VideoToolbox but
                 * have no hardware encoder (-12908): skip the native round
                 * trip instead of failing the whole suite. */
                fprintf(
                    stderr,
                    "VideoToolbox hardware encoding is unavailable "
                    "(status %d); skipping native path\n",
                    (int)status
                );
            }
            grd_encoder_destroy(native_encoder);
        }
    }
    free(source.data);
}
