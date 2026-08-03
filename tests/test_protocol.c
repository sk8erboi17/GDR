#include "test.h"

#include "grd/protocol.h"

#include <math.h>
#include <string.h>

void test_protocol(void)
{
    GRD_ASSERT(sizeof(grd_display_caps) == 16U);
    const grd_display_caps display_caps = {
        .max_fps = 120U,
        .width = 1920U,
        .height = 1080U,
        .high_refresh = 1U,
        .upscale_mode = (uint8_t)GRD_CLIENT_UPSCALE_BALANCED,
        .offload_flags = GRD_CLIENT_OFFLOAD_FRAME_PACING |
                         GRD_CLIENT_OFFLOAD_CURSOR_PREDICTION,
        .reserved = 0U
    };
    GRD_ASSERT(display_caps.upscale_mode == GRD_CLIENT_UPSCALE_BALANCED);
    GRD_ASSERT(
        (display_caps.offload_flags & GRD_CLIENT_OFFLOAD_FRAME_PACING) != 0U
    );
    GRD_ASSERT(display_caps.reserved == 0U);

    const grd_packet_header input = {
        .magic = GRD_PACKET_MAGIC,
        .version = GRD_PROTOCOL_VERSION,
        .type = GRD_PACKET_VIDEO_FRAME,
        .payload_length = 4096U,
        .sequence = UINT64_C(0x0102030405060708)
    };
    uint8_t wire[20];
    grd_packet_header output;
    grd_protocol_encode_header(&input, wire);
    GRD_ASSERT(grd_protocol_decode_header(wire, &output) == GRD_OK);
    GRD_ASSERT(output.magic == input.magic);
    GRD_ASSERT(output.version == input.version);
    GRD_ASSERT(output.type == input.type);
    GRD_ASSERT(output.payload_length == input.payload_length);
    GRD_ASSERT(output.sequence == input.sequence);

    wire[0] = 0U;
    GRD_ASSERT(grd_protocol_decode_header(wire, &output) == GRD_PROTOCOL_ERROR);

    grd_input_event event;
    memset(&event, 0, sizeof(event));
    event.kind = GRD_INPUT_POINTER_MOVE;
    event.x = 0.5F;
    event.y = 1.0F;
    GRD_ASSERT(grd_protocol_validate_input(&event) == GRD_OK);
    event.x = NAN;
    GRD_ASSERT(grd_protocol_validate_input(&event) == GRD_PROTOCOL_ERROR);
    memset(&event, 0, sizeof(event));
    event.kind = GRD_INPUT_POINTER_RELATIVE;
    event.delta_x = -12;
    event.delta_y = 8;
    GRD_ASSERT(grd_protocol_validate_input(&event) == GRD_OK);
    grd_cursor_state cursor = {
        .x = 0.25F,
        .y = 0.75F,
        .visible = 1U
    };
    GRD_ASSERT(cursor.x >= 0.0F && cursor.x <= 1.0F);
    GRD_ASSERT(cursor.y >= 0.0F && cursor.y <= 1.0F);
}
