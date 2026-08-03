#include "test.h"

#include "grd/audio.h"

#include <stdlib.h>

void test_audio(void)
{
    grd_error error = {0};
    grd_audio_encoder *encoder = grd_audio_encoder_create(&error);
    GRD_ASSERT(encoder != NULL);
    grd_audio_decoder *decoder = grd_audio_decoder_create(&error);
    GRD_ASSERT(decoder != NULL);

    float samples[GRD_AUDIO_FRAME_SAMPLES * GRD_AUDIO_CHANNELS];
    for (size_t frame = 0U; frame < GRD_AUDIO_FRAME_SAMPLES; ++frame) {
        const float value =
            (float)((int)(frame % 200U) - 100) / 400.0F;
        samples[frame * 2U] = value;
        samples[frame * 2U + 1U] = -value;
    }

    grd_encoded_audio encoded;
    GRD_ASSERT(grd_audio_encode(
                   encoder,
                   samples,
                   GRD_AUDIO_FRAME_SAMPLES,
                   123456U,
                   &encoded,
                   &error
               ) == GRD_OK);
    GRD_ASSERT(encoded.data != NULL && encoded.size != 0U);

    grd_decoded_audio decoded;
    GRD_ASSERT(grd_audio_decode(
                   decoder,
                   encoded.data,
                   encoded.size,
                   encoded.timestamp_micros,
                   &decoded,
                   &error
               ) == GRD_OK);
    GRD_ASSERT(decoded.samples != NULL);
    GRD_ASSERT(decoded.frames == GRD_AUDIO_FRAME_SAMPLES);
    GRD_ASSERT(decoded.timestamp_micros == 123456U);

    grd_decoded_audio_release(&decoded);
    grd_encoded_audio_release(&encoded);
    grd_audio_decoder_destroy(decoder);
    grd_audio_encoder_destroy(encoder);
}
