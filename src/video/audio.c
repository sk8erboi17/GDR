#include "grd/audio.h"
#include "grd/log.h"

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *audio_buffer_clone(const void *opaque)
{
    return av_buffer_ref((const AVBufferRef *)opaque);
}

static void audio_buffer_release(void *opaque)
{
    AVBufferRef *ref = opaque;
    av_buffer_unref(&ref);
}

struct grd_audio_encoder {
    AVCodecContext *context;
    AVFrame *frame;
    AVPacket *packet;
    SwrContext *resampler;
    int64_t next_pts;
};

struct grd_audio_decoder {
    AVCodecContext *context;
    AVFrame *frame;
    AVPacket *packet;
    SwrContext *resampler;
};

static void audio_error(
    grd_error *error,
    grd_status status,
    const char *prefix,
    int code
)
{
    if (error == NULL) {
        return;
    }
    char detail[128];
    (void)av_strerror(code, detail, sizeof(detail));
    error->code = status;
    (void)snprintf(
        error->message, sizeof(error->message), "%s: %s", prefix, detail
    );
}

static void set_audio_option(
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
            "Audio option %s=%s was not applied: %s",
            name,
            value,
            detail
        );
    }
}

grd_audio_encoder *grd_audio_encoder_create(grd_error *error)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("libopus");
    if (codec == NULL) {
        codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    }
    if (codec == NULL) {
        if (error != NULL) {
            error->code = GRD_NOT_SUPPORTED;
            (void)snprintf(
                error->message, sizeof(error->message),
                "Opus encoder is unavailable"
            );
        }
        return NULL;
    }
    grd_audio_encoder *encoder = calloc(1U, sizeof(*encoder));
    if (encoder == NULL) {
        return NULL;
    }
    encoder->context = avcodec_alloc_context3(codec);
    encoder->frame = av_frame_alloc();
    encoder->packet = av_packet_alloc();
    if (encoder->context == NULL || encoder->frame == NULL ||
        encoder->packet == NULL) {
        grd_audio_encoder_destroy(encoder);
        return NULL;
    }
    encoder->context->sample_rate = (int)GRD_AUDIO_SAMPLE_RATE;
#if LIBAVCODEC_VERSION_MAJOR >= 61
    const void *supported_formats = NULL;
    int supported_format_count = 0;
    if (avcodec_get_supported_config(
            encoder->context,
            codec,
            AV_CODEC_CONFIG_SAMPLE_FORMAT,
            0U,
            &supported_formats,
            &supported_format_count
        ) == 0 && supported_format_count > 0) {
        encoder->context->sample_fmt =
            ((const enum AVSampleFormat *)supported_formats)[0];
    } else {
        encoder->context->sample_fmt = AV_SAMPLE_FMT_FLT;
    }
#else
    encoder->context->sample_fmt =
        codec->sample_fmts != NULL ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLT;
#endif
    encoder->context->bit_rate = (int64_t)GRD_AUDIO_BITRATE_KBPS * 1000LL;
    encoder->context->time_base =
        (AVRational){1, (int)GRD_AUDIO_SAMPLE_RATE};
    av_channel_layout_default(
        &encoder->context->ch_layout, (int)GRD_AUDIO_CHANNELS
    );
    set_audio_option(encoder->context, "application", "lowdelay");
    /* Ten millisecond access units keep audio/video skew bounded while still
     * being supported by Opus and the platform capture backends. */
    set_audio_option(encoder->context, "frame_duration", "10");
    int result = avcodec_open2(encoder->context, codec, NULL);
    if (result < 0) {
        audio_error(error, GRD_ERROR, "Opening Opus encoder", result);
        grd_audio_encoder_destroy(encoder);
        return NULL;
    }
    encoder->frame->format = encoder->context->sample_fmt;
    encoder->frame->sample_rate = encoder->context->sample_rate;
    encoder->frame->nb_samples = encoder->context->frame_size;
    if (encoder->frame->nb_samples == 0) {
        encoder->frame->nb_samples = (int)GRD_AUDIO_FRAME_SAMPLES;
    }
    (void)av_channel_layout_copy(
        &encoder->frame->ch_layout, &encoder->context->ch_layout
    );
    result = av_frame_get_buffer(encoder->frame, 0);
    if (result < 0) {
        audio_error(error, GRD_OUT_OF_MEMORY, "Buffer Opus", result);
        grd_audio_encoder_destroy(encoder);
        return NULL;
    }
    AVChannelLayout input_layout = AV_CHANNEL_LAYOUT_STEREO;
    result = swr_alloc_set_opts2(
        &encoder->resampler,
        &encoder->context->ch_layout,
        encoder->context->sample_fmt,
        encoder->context->sample_rate,
        &input_layout,
        AV_SAMPLE_FMT_FLT,
        (int)GRD_AUDIO_SAMPLE_RATE,
        0,
        NULL
    );
    if (result >= 0) {
        result = swr_init(encoder->resampler);
    }
    if (result < 0) {
        audio_error(error, GRD_ERROR, "Resampler Opus", result);
        grd_audio_encoder_destroy(encoder);
        return NULL;
    }
    return encoder;
}

grd_status grd_audio_encode(
    grd_audio_encoder *encoder,
    const float *stereo_samples,
    size_t frames,
    uint64_t timestamp_micros,
    grd_encoded_audio *encoded,
    grd_error *error
)
{
    if (encoder == NULL || stereo_samples == NULL || encoded == NULL ||
        frames != (size_t)encoder->frame->nb_samples) {
        return GRD_INVALID_ARGUMENT;
    }
    memset(encoded, 0, sizeof(*encoded));
    int result = av_frame_make_writable(encoder->frame);
    if (result < 0) {
        return GRD_ERROR;
    }
    const uint8_t *input[1] = {(const uint8_t *)stereo_samples};
    result = swr_convert(
        encoder->resampler,
        encoder->frame->data,
        encoder->frame->nb_samples,
        input,
        (int)frames
    );
    if (result != encoder->frame->nb_samples) {
        audio_error(error, GRD_ERROR, "Conversione audio", result);
        return GRD_ERROR;
    }
    encoder->frame->pts = encoder->next_pts;
    encoder->next_pts += encoder->frame->nb_samples;
    result = avcodec_send_frame(encoder->context, encoder->frame);
    if (result < 0) {
        audio_error(error, GRD_ERROR, "Sending audio to Opus encoder", result);
        return GRD_ERROR;
    }
    result = avcodec_receive_packet(encoder->context, encoder->packet);
    if (result == AVERROR(EAGAIN)) {
        return GRD_WOULD_BLOCK;
    }
    if (result < 0) {
        audio_error(error, GRD_ERROR, "Receiving Opus audio", result);
        return GRD_ERROR;
    }
    encoded->buffer.opaque = encoder->packet->buf != NULL
                                 ? av_buffer_ref(encoder->packet->buf)
                                 : NULL;
    if (encoded->buffer.opaque == NULL) {
        av_packet_unref(encoder->packet);
        return GRD_OUT_OF_MEMORY;
    }
    encoded->buffer.clone = audio_buffer_clone;
    encoded->buffer.release = audio_buffer_release;
    encoded->data = encoder->packet->data;
    encoded->size = (size_t)encoder->packet->size;
    encoded->timestamp_micros = timestamp_micros;
    av_packet_unref(encoder->packet);
    return GRD_OK;
}

void grd_encoded_audio_release(grd_encoded_audio *audio)
{
    if (audio != NULL) {
        if (audio->buffer.release != NULL) {
            audio->buffer.release((void *)audio->buffer.opaque);
        }
        memset(audio, 0, sizeof(*audio));
    }
}

void grd_audio_encoder_destroy(grd_audio_encoder *encoder)
{
    if (encoder == NULL) {
        return;
    }
    swr_free(&encoder->resampler);
    av_packet_free(&encoder->packet);
    av_frame_free(&encoder->frame);
    avcodec_free_context(&encoder->context);
    free(encoder);
}

grd_audio_decoder *grd_audio_decoder_create(grd_error *error)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (codec == NULL) {
        return NULL;
    }
    grd_audio_decoder *decoder = calloc(1U, sizeof(*decoder));
    if (decoder == NULL) {
        return NULL;
    }
    decoder->context = avcodec_alloc_context3(codec);
    decoder->frame = av_frame_alloc();
    decoder->packet = av_packet_alloc();
    if (decoder->context == NULL || decoder->frame == NULL ||
        decoder->packet == NULL) {
        grd_audio_decoder_destroy(decoder);
        return NULL;
    }
    decoder->context->sample_rate = (int)GRD_AUDIO_SAMPLE_RATE;
    av_channel_layout_default(
        &decoder->context->ch_layout, (int)GRD_AUDIO_CHANNELS
    );
    const int result = avcodec_open2(decoder->context, codec, NULL);
    if (result < 0) {
        audio_error(error, GRD_ERROR, "Opening Opus decoder", result);
        grd_audio_decoder_destroy(decoder);
        return NULL;
    }
    return decoder;
}

grd_status grd_audio_decode(
    grd_audio_decoder *decoder,
    const uint8_t *data,
    size_t size,
    uint64_t timestamp_micros,
    grd_decoded_audio *decoded,
    grd_error *error
)
{
    if (decoder == NULL || data == NULL || size == 0U || decoded == NULL ||
        size > (size_t)INT_MAX) {
        return GRD_INVALID_ARGUMENT;
    }
    memset(decoded, 0, sizeof(*decoded));
    int result = av_new_packet(decoder->packet, (int)size);
    if (result < 0) {
        return GRD_OUT_OF_MEMORY;
    }
    memcpy(decoder->packet->data, data, size);
    result = avcodec_send_packet(decoder->context, decoder->packet);
    av_packet_unref(decoder->packet);
    if (result < 0) {
        audio_error(error, GRD_PROTOCOL_ERROR, "Opus packet", result);
        return GRD_PROTOCOL_ERROR;
    }
    result = avcodec_receive_frame(decoder->context, decoder->frame);
    if (result == AVERROR(EAGAIN)) {
        return GRD_WOULD_BLOCK;
    }
    if (result < 0) {
        audio_error(error, GRD_PROTOCOL_ERROR, "Decoding Opus", result);
        return GRD_PROTOCOL_ERROR;
    }
    AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
    if (decoder->resampler == NULL) {
        result = swr_alloc_set_opts2(
            &decoder->resampler,
            &output_layout,
            AV_SAMPLE_FMT_FLT,
            (int)GRD_AUDIO_SAMPLE_RATE,
            &decoder->frame->ch_layout,
            (enum AVSampleFormat)decoder->frame->format,
            decoder->frame->sample_rate,
            0,
            NULL
        );
        if (result >= 0) {
            result = swr_init(decoder->resampler);
        }
        if (result < 0) {
            av_frame_unref(decoder->frame);
            return GRD_ERROR;
        }
    }
    const int capacity = swr_get_out_samples(
        decoder->resampler, decoder->frame->nb_samples
    );
    if (capacity <= 0) {
        av_frame_unref(decoder->frame);
        return GRD_ERROR;
    }
    decoded->samples = malloc(
        (size_t)capacity * GRD_AUDIO_CHANNELS * sizeof(float)
    );
    if (decoded->samples == NULL) {
        av_frame_unref(decoder->frame);
        return GRD_OUT_OF_MEMORY;
    }
    uint8_t *output[1] = {(uint8_t *)decoded->samples};
    result = swr_convert(
        decoder->resampler,
        output,
        capacity,
        (const uint8_t **)decoder->frame->extended_data,
        decoder->frame->nb_samples
    );
    av_frame_unref(decoder->frame);
    if (result < 0) {
        free(decoded->samples);
        memset(decoded, 0, sizeof(*decoded));
        return GRD_ERROR;
    }
    decoded->frames = (size_t)result;
    decoded->timestamp_micros = timestamp_micros;
    return GRD_OK;
}

void grd_decoded_audio_release(grd_decoded_audio *audio)
{
    if (audio != NULL) {
        free(audio->samples);
        memset(audio, 0, sizeof(*audio));
    }
}

void grd_audio_decoder_destroy(grd_audio_decoder *decoder)
{
    if (decoder == NULL) {
        return;
    }
    swr_free(&decoder->resampler);
    av_packet_free(&decoder->packet);
    av_frame_free(&decoder->frame);
    avcodec_free_context(&decoder->context);
    free(decoder);
}
