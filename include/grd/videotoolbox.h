#ifndef GRD_VIDEOTOOLBOX_H
#define GRD_VIDEOTOOLBOX_H

#include "grd/common.h"
#include "grd/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grd_videotoolbox_decoder grd_videotoolbox_decoder;
typedef struct grd_videotoolbox_encoder grd_videotoolbox_encoder;

grd_videotoolbox_decoder *grd_videotoolbox_decoder_create(grd_error *error);
grd_status grd_videotoolbox_decoder_decode(
    grd_videotoolbox_decoder *decoder,
    const uint8_t *data,
    size_t size,
    grd_frame *frame,
    grd_error *error
);
bool grd_videotoolbox_decoder_needs_parameter_sets(
    grd_videotoolbox_decoder *decoder
);
void grd_videotoolbox_decoder_set_bgra_output(
    grd_videotoolbox_decoder *decoder
);
void grd_videotoolbox_decoder_destroy(grd_videotoolbox_decoder *decoder);

grd_videotoolbox_encoder *grd_videotoolbox_encoder_create(
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint32_t bitrate_kbps,
    grd_error *error
);
grd_status grd_videotoolbox_encoder_encode(
    grd_videotoolbox_encoder *encoder,
    const grd_frame *frame,
    bool force_keyframe,
    uint8_t **data,
    size_t *size,
    bool *keyframe,
    grd_error *error
);
grd_status grd_videotoolbox_encoder_set_bitrate(
    grd_videotoolbox_encoder *encoder,
    uint32_t bitrate_kbps
);
void grd_videotoolbox_encoder_destroy(grd_videotoolbox_encoder *encoder);

#ifdef __cplusplus
}
#endif

#endif
