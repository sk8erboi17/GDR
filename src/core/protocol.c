#include "grd/protocol.h"

#include <math.h>
#include <string.h>

static void write_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void write_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void write_u64(uint8_t *output, uint64_t value)
{
    for (size_t index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> ((7U - index) * 8U));
    }
}

static uint16_t read_u16(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t read_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) |
           ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) |
           (uint32_t)input[3];
}

static uint64_t read_u64(const uint8_t *input)
{
    uint64_t value = 0U;
    for (size_t index = 0U; index < 8U; ++index) {
        value = (value << 8U) | input[index];
    }
    return value;
}

void grd_protocol_encode_header(
    const grd_packet_header *header,
    uint8_t output[20]
)
{
    write_u32(output, header->magic);
    write_u16(output + 4U, header->version);
    write_u16(output + 6U, header->type);
    write_u32(output + 8U, header->payload_length);
    write_u64(output + 12U, header->sequence);
}

grd_status grd_protocol_decode_header(
    const uint8_t input[20],
    grd_packet_header *header
)
{
    if (input == NULL || header == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    header->magic = read_u32(input);
    header->version = read_u16(input + 4U);
    header->type = read_u16(input + 6U);
    header->payload_length = read_u32(input + 8U);
    header->sequence = read_u64(input + 12U);
    if (header->magic != GRD_PACKET_MAGIC ||
        header->version != GRD_PROTOCOL_VERSION ||
        header->payload_length > GRD_MAX_PACKET_SIZE) {
        return GRD_PROTOCOL_ERROR;
    }
    return GRD_OK;
}

grd_status grd_protocol_validate_hello(const grd_hello *hello)
{
    if (hello == NULL ||
        (hello->requested_role != GRD_ROLE_OBSERVER &&
         hello->requested_role != GRD_ROLE_CONTROLLER) ||
        (hello->reserved != GRD_HELLO_CHANNEL_CONTROL &&
         hello->reserved != GRD_HELLO_CHANNEL_MEDIA) ||
        hello->operating_system > GRD_OS_LINUX_X11 ||
        memchr(hello->device_id, '\0', sizeof(hello->device_id)) == NULL ||
        memchr(hello->device_name, '\0', sizeof(hello->device_name)) == NULL) {
        return GRD_PROTOCOL_ERROR;
    }
    return GRD_OK;
}

grd_status grd_protocol_validate_input(const grd_input_event *event)
{
    if (event == NULL || event->kind > GRD_INPUT_POINTER_RELATIVE ||
        !isfinite(event->x) || !isfinite(event->y) ||
        event->text_length > sizeof(event->text)) {
        return GRD_PROTOCOL_ERROR;
    }
    if (event->kind == GRD_INPUT_POINTER_MOVE &&
        (event->x < 0.0F || event->x > 1.0F ||
         event->y < 0.0F || event->y > 1.0F)) {
        return GRD_PROTOCOL_ERROR;
    }
    return GRD_OK;
}
