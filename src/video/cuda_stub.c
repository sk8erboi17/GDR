#include "grd/gpu.h"

#include <stdio.h>

bool grd_cuda_available(char *name, size_t name_capacity)
{
    (void)name;
    (void)name_capacity;
    return false;
}

grd_cuda_converter *grd_cuda_converter_create(void)
{
    return NULL;
}

void grd_cuda_converter_destroy(grd_cuda_converter *converter)
{
    (void)converter;
}

grd_status grd_cuda_converter_rgba_to_nv12(
    grd_cuda_converter *converter,
    const grd_frame *source,
    uint32_t destination_width,
    uint32_t destination_height,
    uint8_t *destination,
    size_t destination_size,
    grd_error *error
)
{
    (void)converter;
    (void)source;
    (void)destination_width;
    (void)destination_height;
    (void)destination;
    (void)destination_size;
    if (error != NULL) {
        error->code = GRD_NOT_SUPPORTED;
        (void)snprintf(error->message, sizeof(error->message), "CUDA is unavailable");
    }
    return GRD_NOT_SUPPORTED;
}

grd_status grd_cuda_converter_rgba_to_nv12_device(
    grd_cuda_converter *converter,
    const grd_frame *source,
    uint32_t destination_width,
    uint32_t destination_height,
    uint8_t *destination_y_device,
    uint8_t *destination_uv_device,
    uint32_t destination_y_stride,
    uint32_t destination_uv_stride,
    grd_error *error
)
{
    (void)converter;
    (void)source;
    (void)destination_width;
    (void)destination_height;
    (void)destination_y_device;
    (void)destination_uv_device;
    (void)destination_y_stride;
    (void)destination_uv_stride;
    if (error != NULL) {
        error->code = GRD_NOT_SUPPORTED;
        (void)snprintf(error->message, sizeof(error->message), "CUDA is unavailable");
    }
    return GRD_NOT_SUPPORTED;
}

grd_status grd_cuda_converter_rgba_to_nv12_device_async(
    grd_cuda_converter *converter,
    const grd_frame *source,
    uint32_t destination_width,
    uint32_t destination_height,
    uint8_t *destination_y_device,
    uint8_t *destination_uv_device,
    uint32_t destination_y_stride,
    uint32_t destination_uv_stride,
    grd_error *error
)
{
    (void)converter;
    (void)source;
    (void)destination_width;
    (void)destination_height;
    (void)destination_y_device;
    (void)destination_uv_device;
    (void)destination_y_stride;
    (void)destination_uv_stride;
    if (error != NULL) {
        error->code = GRD_NOT_SUPPORTED;
        (void)snprintf(error->message, sizeof(error->message), "CUDA is unavailable");
    }
    return GRD_NOT_SUPPORTED;
}

grd_status grd_cuda_converter_sync(
    grd_cuda_converter *converter,
    grd_error *error
)
{
    (void)converter;
    if (error != NULL) {
        error->code = GRD_NOT_SUPPORTED;
        (void)snprintf(error->message, sizeof(error->message), "CUDA is unavailable");
    }
    return GRD_NOT_SUPPORTED;
}

grd_status grd_cuda_rgba_to_nv12(
    const grd_frame *source,
    uint32_t destination_width,
    uint32_t destination_height,
    uint8_t *destination,
    size_t destination_size,
    grd_error *error
)
{
    (void)source;
    (void)destination_width;
    (void)destination_height;
    (void)destination;
    (void)destination_size;
    if (error != NULL) {
        error->code = GRD_NOT_SUPPORTED;
        (void)snprintf(error->message, sizeof(error->message), "CUDA is unavailable");
    }
    return GRD_NOT_SUPPORTED;
}
