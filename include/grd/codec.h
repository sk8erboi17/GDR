#ifndef GRD_CODEC_H
#define GRD_CODEC_H

#include "grd/common.h"
#include "grd/gpu.h"
#include "grd/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grd_encoder grd_encoder;
typedef struct grd_decoder grd_decoder;

typedef struct grd_encoded_frame {
    uint8_t *data;
    size_t size;
    bool keyframe;
    uint64_t timestamp_micros;
    /* Average QP of the frame when the encoder exposes it (software
     * x264/x265 and some hardware wrappers); qp_known is false otherwise.
     * Used for quality diagnostics and telemetry. */
    uint8_t avg_qp;
    bool qp_known;
    /* Refcounted ownership of data; released by grd_encoded_frame_release or
     * handed to the transport for zero-copy broadcast. */
    grd_owned_buffer buffer;
} grd_encoded_frame;

typedef struct grd_encoder_timing {
    uint64_t conversion_micros;
    uint64_t send_frame_micros;
    uint64_t receive_packet_micros;
    bool conversion_recorded;
    bool send_frame_recorded;
    bool receive_packet_recorded;
} grd_encoder_timing;

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
);
grd_status grd_encoder_encode(
    grd_encoder *encoder,
    const grd_frame *frame,
    grd_encoded_frame *encoded,
    grd_error *error
);
void grd_encoder_last_timing(
    const grd_encoder *encoder,
    grd_encoder_timing *timing
);
void grd_encoder_request_keyframe(grd_encoder *encoder);
/* Adjusts the encoder bit rate at runtime (used by adaptive bitrate).
 * Returns GRD_NOT_SUPPORTED when the active FFmpeg encoder reads rate-control
 * options only at open time; a running stream must then keep both the encoder
 * and wire budgets stable rather than recreating the encoder for ABR. */
grd_status grd_encoder_set_bitrate(
    grd_encoder *encoder,
    uint32_t bitrate_kbps,
    grd_error *error
);
void grd_encoded_frame_release(grd_encoded_frame *frame);
void grd_encoder_destroy(grd_encoder *encoder);

grd_decoder *grd_decoder_create(
    grd_pipeline_kind requested_pipeline,
    grd_video_codec codec,
    grd_pipeline_kind *active_pipeline,
    grd_error *error
);
const char *grd_codec_name(grd_video_codec codec);
/* Bitmask (GRD_VIDEO_CAPS_*) of codecs the local process can DECODE,
 * advertised to the host at connect so the codec is negotiated before the
 * stream starts. */
uint32_t grd_client_decode_caps(void);
/* Whether an encoder for codec exists on the given pipeline. Used by the
 * host to pick the negotiated codec before the stream starts. */
bool grd_codec_encoder_available(
    grd_pipeline_kind pipeline,
    grd_video_codec codec
);
/* Converts an AV_PKT_DATA_QUALITY_STATS value (lambda scale, QP*118 for
 * x264/x265) to the average QP on the usual 0-51 scale. Exposed for the
 * known-vector regression test. */
uint8_t grd_qp_from_quality(uint32_t quality);
/* Enables GPU-resident decode output on Windows/NVIDIA: decoded frames are
 * written into a D3D11 texture via CUDA interop, avoiding the
 * device→host→device round trip. d3d11_device is the renderer's
 * ID3D11Device. */
grd_status grd_decoder_enable_d3d11_output(
    grd_decoder *decoder,
    void *d3d11_device,
    grd_error *error
);
grd_status grd_decoder_decode(
    grd_decoder *decoder,
    const uint8_t *data,
    size_t size,
    grd_frame *frame,
    grd_error *error
);
/* Zero-copy decode: buffer_base owns a GRD_MEDIA_BUFFER_PADDING zeroed tail
 * and buffer_release frees it when the decoder is done with it. Always
 * consumes buffer_base, even on error. */
grd_status grd_decoder_decode_owned(
    grd_decoder *decoder,
    uint8_t *data,
    size_t size,
    uint8_t *buffer_base,
    size_t buffer_size,
    grd_buffer_release_fn buffer_release,
    grd_frame *frame,
    grd_error *error
);
bool grd_decoder_needs_parameter_sets(grd_decoder *decoder);
void grd_decoder_set_bgra_output(grd_decoder *decoder);
void grd_decoder_destroy(grd_decoder *decoder);

#ifdef __cplusplus
}
#endif

#endif
