#ifndef GRD_AUDIO_H
#define GRD_AUDIO_H

#include "grd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRD_AUDIO_SAMPLE_RATE 48000U
#define GRD_AUDIO_CHANNELS 2U
#define GRD_AUDIO_FRAME_SAMPLES 480U
#define GRD_AUDIO_BITRATE_KBPS 128U

typedef struct grd_audio_encoder grd_audio_encoder;
typedef struct grd_audio_decoder grd_audio_decoder;

typedef struct grd_encoded_audio {
    uint8_t *data;
    size_t size;
    uint64_t timestamp_micros;
    grd_owned_buffer buffer;
} grd_encoded_audio;

typedef struct grd_decoded_audio {
    float *samples;
    size_t frames;
    uint64_t timestamp_micros;
} grd_decoded_audio;

grd_status grd_platform_audio_start(grd_error *error);
grd_status grd_platform_audio_read(
    float *stereo_samples,
    size_t frame_capacity,
    size_t *frames_read,
    uint64_t *timestamp_micros,
    grd_error *error
);
void grd_platform_audio_stop(void);

grd_audio_encoder *grd_audio_encoder_create(grd_error *error);
grd_status grd_audio_encode(
    grd_audio_encoder *encoder,
    const float *stereo_samples,
    size_t frames,
    uint64_t timestamp_micros,
    grd_encoded_audio *encoded,
    grd_error *error
);
void grd_encoded_audio_release(grd_encoded_audio *audio);
void grd_audio_encoder_destroy(grd_audio_encoder *encoder);

grd_audio_decoder *grd_audio_decoder_create(grd_error *error);
grd_status grd_audio_decode(
    grd_audio_decoder *decoder,
    const uint8_t *data,
    size_t size,
    uint64_t timestamp_micros,
    grd_decoded_audio *decoded,
    grd_error *error
);
void grd_decoded_audio_release(grd_decoded_audio *audio);
void grd_audio_decoder_destroy(grd_audio_decoder *decoder);

#ifdef __cplusplus
}
#endif

#endif
