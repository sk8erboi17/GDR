#include "test.h"

#include "grd/transport.h"

#include <libavutil/buffer.h>
#include <SDL3/SDL_timer.h>
#include <stdatomic.h>
#include <string.h>

static atomic_int received_ping;
static atomic_int received_media_clipboard;
static atomic_int received_udp_video;
static atomic_int received_udp_video_fec;
static atomic_int received_udp_video_parts;
static atomic_int received_udp_audio;
static atomic_int received_bitrate_reports;
static atomic_int received_realtime_input;

static void *test_avbuf_clone(const void *opaque)
{
    return av_buffer_ref((const AVBufferRef *)opaque);
}

static void test_avbuf_release(void *opaque)
{
    av_buffer_unref((AVBufferRef **)&opaque);
}

static bool receive_packet(
    grd_packet_type type,
    const void *payload,
    size_t payload_length,
    bool payload_takeable,
    grd_role role,
    void *userdata
)
{
    (void)userdata;
    (void)payload_takeable;
    static const char expected[] = "ping-cifrato";
    if (type == GRD_PACKET_BITRATE_REPORT &&
        payload_length == sizeof(grd_bitrate_report)) {
        atomic_fetch_add(&received_bitrate_reports, 1);
    }
    if (type == GRD_PACKET_PING &&
        role == GRD_ROLE_CONTROLLER &&
        payload_length == sizeof(expected) &&
        memcmp(payload, expected, sizeof(expected)) == 0) {
        atomic_store(&received_ping, 1);
    }
    if (type == GRD_PACKET_INPUT &&
        role == GRD_ROLE_CONTROLLER &&
        payload_length == sizeof(grd_input_event)) {
        const grd_input_event *input = payload;
        if (input->kind == GRD_INPUT_POINTER_RELATIVE &&
            input->delta_x == 7 && input->delta_y == -4) {
            atomic_store(&received_realtime_input, 1);
        }
    }
    static const char clipboard[] = "clipboard-su-media";
    if (type == GRD_PACKET_CLIPBOARD &&
        role == GRD_ROLE_CONTROLLER &&
        payload_length == sizeof(clipboard) &&
        memcmp(payload, clipboard, sizeof(clipboard)) == 0) {
        atomic_store(&received_media_clipboard, 1);
    }
    return false;
}

static bool receive_client_packet(
    grd_packet_type type,
    const void *payload,
    size_t payload_length,
    bool payload_takeable,
    grd_role role,
    void *userdata
)
{
    (void)role;
    (void)userdata;
    (void)payload_takeable;
    static const size_t expected_length = 4096U;
    if (type == GRD_PACKET_VIDEO_FRAME &&
        payload_length == expected_length &&
        ((const uint8_t *)payload)[0] == 0x47U &&
        ((const uint8_t *)payload)[payload_length - 1U] == 0x55U) {
        atomic_store(&received_udp_video, 1);
    }
    static const size_t fec_length = 4096U;
    if (type == GRD_PACKET_VIDEO_FRAME &&
        payload_length == fec_length &&
        ((const uint8_t *)payload)[0] == 0x52U &&
        ((const uint8_t *)payload)[payload_length - 1U] == 0x9EU) {
        atomic_store(&received_udp_video_fec, 1);
    }
    static const size_t parts_length = 25U + 4096U;
    if (type == GRD_PACKET_VIDEO_FRAME &&
        payload_length == parts_length &&
        ((const uint8_t *)payload)[25U] == 0x48U &&
        ((const uint8_t *)payload)[payload_length - 1U] == 0x56U) {
        atomic_store(&received_udp_video_parts, 1);
    }
    static const size_t audio_length = 8U + 64U;
    if (type == GRD_PACKET_AUDIO_FRAME &&
        payload_length == audio_length &&
        ((const uint8_t *)payload)[0] == 0x11U &&
        ((const uint8_t *)payload)[audio_length - 1U] == 0x77U) {
        atomic_store(&received_udp_audio, 1);
    }
    return false;
}

void test_transport(void)
{
    static const char password[] = "transport-test-password";
    grd_config host_config;
    grd_config client_config;
    grd_error error = {0};
    grd_config_defaults(&host_config);
    grd_config_defaults(&client_config);
    GRD_ASSERT(grd_config_set_password(
                   &host_config, password, &error
               ) == GRD_OK);

    grd_host *host = NULL;
    for (uint16_t port = 49152U; port < 49216U && host == NULL; ++port) {
        host_config.port = port;
        host = grd_host_start(
            &host_config, receive_packet, NULL, &error
        );
    }
    GRD_ASSERT(host != NULL);

    grd_connection *connection = grd_connect(
        "::1",
        host_config.port,
        password,
        GRD_ROLE_CONTROLLER,
        &client_config,
        receive_client_packet,
        NULL,
        &error
    );
    GRD_ASSERT(connection != NULL);
    GRD_ASSERT(grd_connection_is_active(connection));
    GRD_ASSERT(grd_connection_role(connection) == GRD_ROLE_CONTROLLER);

    grd_connection *media = grd_connect_media(
        "::1",
        host_config.port,
        password,
        &client_config,
        receive_client_packet,
        NULL,
        &error
    );
    GRD_ASSERT(media != NULL);
    for (unsigned attempt = 0U;
         attempt < 100U &&
         (!grd_connection_video_udp_active(media) ||
          grd_host_udp_video_client_count(host) == 0U);
         ++attempt) {
        SDL_Delay(10U);
    }
    GRD_ASSERT(grd_connection_video_udp_active(media));
    GRD_ASSERT(grd_host_udp_video_client_count(host) == 1U);
    /* Relative mouse motion uses the authenticated UDP media channel but is
     * authorized only because the same device owns the controller channel. */
    {
        const grd_input_event motion = {
            .kind = GRD_INPUT_POINTER_RELATIVE,
            .delta_x = 7,
            .delta_y = -4
        };
        atomic_store(&received_realtime_input, 0);
        GRD_ASSERT(grd_connection_send_realtime_input(
                       media, &motion, &error
                   ) == GRD_OK);
        for (unsigned attempt = 0U;
             attempt < 100U && atomic_load(&received_realtime_input) == 0;
             ++attempt) {
            SDL_Delay(10U);
        }
        GRD_ASSERT(atomic_load(&received_realtime_input) == 1);
    }
    /* The transport must flag the stream thread so the first frame sent to a
     * newly ready media client is an IDR instead of an undecodable P-frame. */
    GRD_ASSERT(grd_host_take_keyframe_pending(host));
    GRD_ASSERT(!grd_host_take_keyframe_pending(host));
    GRD_ASSERT(grd_connection_is_active(media));
    /* The media half must not consume a logical controller slot. */
    GRD_ASSERT(grd_host_client_count(host) == 1U);

    /*
     * Regression: the server handshake timeout used to remain active and
     * close an otherwise healthy observer/controller after five idle seconds.
     */
    SDL_Delay(5500U);
    GRD_ASSERT(grd_connection_is_active(connection));
    GRD_ASSERT(grd_host_client_count(host) == 1U);

    static const char ping[] = "ping-cifrato";
    atomic_store(&received_ping, 0);
    GRD_ASSERT(grd_connection_send(
                   connection,
                   GRD_PACKET_PING,
                   ping,
                   sizeof(ping),
                   &error
               ) == GRD_OK);
    for (unsigned attempt = 0U;
         attempt < 100U && atomic_load(&received_ping) == 0;
         ++attempt) {
        SDL_Delay(10U);
    }
    GRD_ASSERT(atomic_load(&received_ping) == 1);
    static const char clipboard[] = "clipboard-su-media";
    atomic_store(&received_media_clipboard, 0);
    GRD_ASSERT(grd_connection_send(
                   media,
                   GRD_PACKET_CLIPBOARD,
                   clipboard,
                   sizeof(clipboard),
                   &error
               ) == GRD_OK);
    for (unsigned attempt = 0U;
         attempt < 100U && atomic_load(&received_media_clipboard) == 0;
         ++attempt) {
        SDL_Delay(10U);
    }
    GRD_ASSERT(atomic_load(&received_media_clipboard) == 1);
    uint8_t video_payload[4096];
    memset(video_payload, 0xA5, sizeof(video_payload));
    video_payload[0] = 0x47U;
    video_payload[sizeof(video_payload) - 1U] = 0x55U;
    atomic_store(&received_udp_video, 0);
    const grd_status video_status = grd_host_broadcast(
        host,
        GRD_PACKET_VIDEO_FRAME,
        video_payload,
        sizeof(video_payload),
        &error
    );
    GRD_ASSERT(video_status == GRD_OK || video_status == GRD_BUSY);
    for (unsigned attempt = 0U;
         attempt < 100U && atomic_load(&received_udp_video) == 0;
         ++attempt) {
        SDL_Delay(10U);
    }
    GRD_ASSERT(atomic_load(&received_udp_video) == 1);
    /* Zero-copy parts broadcast: the 17-byte wire prefix and the payload are
     * fragmented and reassembled without building a contiguous wire buffer. */
    {
        AVBufferRef *parts_buffer = av_buffer_alloc(sizeof(video_payload));
        GRD_ASSERT(parts_buffer != NULL);
        memcpy(parts_buffer->data, video_payload, sizeof(video_payload));
        parts_buffer->data[0] = 0x48U;
        parts_buffer->data[sizeof(video_payload) - 1U] = 0x56U;
        uint8_t parts_prefix[25] = {0};
        parts_prefix[0] = 0xAA;
        const grd_buf_part parts[2] = {
            {.data = parts_prefix, .length = sizeof(parts_prefix)},
            {.data = parts_buffer->data, .length = sizeof(video_payload)}
        };
        grd_owned_buffer parts_ref = {
            .opaque = parts_buffer,
            .clone = test_avbuf_clone,
            .release = test_avbuf_release
        };
        atomic_store(&received_udp_video_parts, 0);
        const grd_status parts_status = grd_host_broadcast_parts(
            host,
            GRD_PACKET_VIDEO_FRAME,
            parts,
            2U,
            &parts_ref,
            false,
            &error
        );
        GRD_ASSERT(parts_status == GRD_OK || parts_status == GRD_BUSY);
        for (unsigned attempt = 0U;
             attempt < 100U && atomic_load(&received_udp_video_parts) == 0;
             ++attempt) {
            SDL_Delay(10U);
        }
        GRD_ASSERT(atomic_load(&received_udp_video_parts) == 1);
    }
    /* FEC smoke test: with XOR parity enabled the host emits one parity
     * datagram per 16-fragment block and the client receive path must accept
     * it (strict length check + received-bitmap) and still deliver the
     * frame intact. Frame id 3, so the NACK test below still targets the
     * parts frame (id 2). */
    {
        grd_host_set_fec_enabled(host, true);
        uint8_t fec_payload[4096];
        memset(fec_payload, 0xC3, sizeof(fec_payload));
        fec_payload[0] = 0x52U;
        fec_payload[sizeof(fec_payload) - 1U] = 0x9EU;
        atomic_store(&received_udp_video_fec, 0);
        const grd_status fec_status = grd_host_broadcast(
            host,
            GRD_PACKET_VIDEO_FRAME,
            fec_payload,
            sizeof(fec_payload),
            &error
        );
        GRD_ASSERT(fec_status == GRD_OK || fec_status == GRD_BUSY);
        for (unsigned attempt = 0U;
             attempt < 100U && atomic_load(&received_udp_video_fec) == 0;
             ++attempt) {
            SDL_Delay(10U);
        }
        GRD_ASSERT(atomic_load(&received_udp_video_fec) == 1);
        grd_host_set_fec_enabled(host, false);
    }
    /* NACK retransmission round trip: the host keeps the last frame and
     * re-sends the fragments the client reports missing. The parts frame
     * above is the second VIDEO_FRAME broadcast, so its frame id is 2 and it
     * spans 4 fragments (4121 bytes / 1128 per fragment). */
    {
        grd_udp_nack nack;
        memset(&nack, 0, sizeof(nack));
        nack.frame_id = 2U;
        nack.fragment_count = 4U;
        nack.bitmap_bytes = 1U;
        nack.bitmap[0] = 0x01U; /* fragment 0 reported missing */
        GRD_ASSERT(grd_connection_send(
            media,
            GRD_PACKET_UDP_NACK,
            &nack,
            sizeof(nack),
            &error
        ) == GRD_OK);
        SDL_Delay(100U);
        GRD_ASSERT(grd_connection_is_active(media));
        GRD_ASSERT(grd_connection_video_udp_active(media));
    }
    /* Audio rides the UDP video channel as a single datagram (with a TCP
     * fallback that this LAN test does not exercise). */
    {
        uint8_t audio_wire[8U + 64U];
        memset(audio_wire, 0x33, sizeof(audio_wire));
        audio_wire[0] = 0x11U;
        audio_wire[sizeof(audio_wire) - 1U] = 0x77U;
        AVBufferRef *audio_buffer = av_buffer_alloc(64U);
        GRD_ASSERT(audio_buffer != NULL);
        memcpy(audio_buffer->data, audio_wire + 8U, 64U);
        const grd_buf_part audio_parts[2] = {
            {.data = audio_wire, .length = 8U},
            {.data = audio_buffer->data, .length = 64U}
        };
        grd_owned_buffer audio_ref = {
            .opaque = audio_buffer,
            .clone = test_avbuf_clone,
            .release = test_avbuf_release
        };
        atomic_store(&received_udp_audio, 0);
        const grd_status audio_status = grd_host_broadcast_parts(
            host,
            GRD_PACKET_AUDIO_FRAME,
            audio_parts,
            2U,
            &audio_ref,
            false,
            &error
        );
        GRD_ASSERT(audio_status == GRD_OK || audio_status == GRD_BUSY);
        for (unsigned attempt = 0U;
             attempt < 100U && atomic_load(&received_udp_audio) == 0;
             ++attempt) {
            SDL_Delay(10U);
        }
        GRD_ASSERT(atomic_load(&received_udp_audio) == 1);
    }
    /* The client UDP receiver publishes a periodic media-quality report on
     * the reliable channel; the host must surface it to the application. */
    atomic_store(&received_bitrate_reports, 0);
    for (unsigned attempt = 0U;
         attempt < 200U && atomic_load(&received_bitrate_reports) == 0;
         ++attempt) {
        SDL_Delay(50U);
    }
    GRD_ASSERT(atomic_load(&received_bitrate_reports) > 0);
    uint64_t local_drop_generation = 0U;
    for (unsigned attempt = 0U;
         attempt < 200U && local_drop_generation == 0U;
         ++attempt) {
        (void)grd_host_udp_initiating_drop_sample(
            host, &local_drop_generation
        );
        if (local_drop_generation == 0U) {
            SDL_Delay(10U);
        }
    }
    GRD_ASSERT(local_drop_generation != 0U);
    GRD_ASSERT((local_drop_generation & 1U) == 0U);
    /* A capture-source stall must collapse into one clean recovery point:
     * stale queued video is discarded and the stream thread is asked for a
     * fresh IDR. The request is edge-triggered when consumed. */
    grd_host_resynchronize_video(host);
    GRD_ASSERT(grd_host_take_keyframe_pending(host));
    GRD_ASSERT(!grd_host_take_keyframe_pending(host));
    /* The source resync has requested an IDR but none has been encoded and
     * queued yet, so the producer must remain free to create that repair. */
    GRD_ASSERT(!grd_host_video_recovery_queued(host));
    grd_host_resynchronize_video(host);
    GRD_ASSERT(!grd_host_take_keyframe_pending(host));
    grd_connection_close(media);

    /* Regression/crash takeover: reconnecting with the same persistent
     * device id must replace a controller socket that is still technically
     * alive. This models an app crash/relaunch before TCP notices the dead
     * process and also covers stale slot reuse. */
    memset(&error, 0, sizeof(error));
    grd_connection *reconnected = grd_connect(
        "::1",
        host_config.port,
        password,
        GRD_ROLE_CONTROLLER,
        &client_config,
        receive_client_packet,
        NULL,
        &error
    );
    GRD_ASSERT(reconnected != NULL);
    GRD_ASSERT(grd_connection_role(reconnected) == GRD_ROLE_CONTROLLER);
    for (unsigned attempt = 0U;
         attempt < 100U && grd_connection_is_active(connection);
         ++attempt) {
        SDL_Delay(10U);
    }
    GRD_ASSERT(!grd_connection_is_active(connection));
    grd_connection_close(connection);
    grd_connection_close(reconnected);
    grd_host_stop(host);

    /* A discovery entry can become stale between selection and connection.
     * Connecting to a host that has just stopped must fail cleanly instead
     * of creating a partial connection or crashing the client. */
    memset(&error, 0, sizeof(error));
    grd_connection *unavailable = grd_connect(
        "::1",
        host_config.port,
        password,
        GRD_ROLE_CONTROLLER,
        &client_config,
        receive_client_packet,
        NULL,
        &error
    );
    GRD_ASSERT(unavailable == NULL);
    GRD_ASSERT(error.code == GRD_IO_ERROR);
}
