#ifndef GRD_GPU_H
#define GRD_GPU_H

#include "grd/common.h"
#include "grd/config.h"
#include "grd/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum grd_pipeline_kind {
    GRD_PIPELINE_METAL_VIDEOTOOLBOX = 0,
    GRD_PIPELINE_CUDA_NVENC = 1,
    GRD_PIPELINE_CUDA_SOFTWARE = 2,
    GRD_PIPELINE_INTEGRATED_SOFTWARE = 3,
    GRD_PIPELINE_SOFTWARE = 4
} grd_pipeline_kind;

typedef struct grd_gpu_capabilities {
    bool metal;
    bool videotoolbox_h264;
    bool cuda;
    bool nvenc;
    bool nvdec;
    bool integrated_renderer;
    char adapter_name[128];
} grd_gpu_capabilities;

grd_gpu_capabilities grd_gpu_detect(void);
grd_pipeline_kind grd_gpu_select(
    const grd_gpu_capabilities *capabilities,
    grd_gpu_preference preference
);
const char *grd_pipeline_name(grd_pipeline_kind pipeline);

bool grd_metal_available(char *name, size_t name_capacity);
bool grd_cuda_available(char *name, size_t name_capacity);
typedef struct grd_cuda_converter grd_cuda_converter;
grd_cuda_converter *grd_cuda_converter_create(void);
void grd_cuda_converter_destroy(grd_cuda_converter *converter);
grd_status grd_cuda_converter_rgba_to_nv12(
    grd_cuda_converter *converter,
    const grd_frame *source,
    uint32_t destination_width,
    uint32_t destination_height,
    uint8_t *destination,
    size_t destination_size,
    grd_error *error
);
/* Writes NV12 directly into CUDA device pointers (typically AVFrame data from
 * an AVHWFramesContext). Packed CPU RGBA/BGRA and Windows D3D11 BGRA capture
 * frames are accepted; the latter stays GPU-resident end to end. */
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
);
/* Asynchronous variant: submits the conversion without waiting so the caller
 * can overlap GPU conversion with NVENC encoding; grd_cuda_converter_sync
 * waits only streams with pending submissions and must be called before the
 * destination buffers are read or reused. */
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
);
grd_status grd_cuda_converter_sync(grd_cuda_converter *converter, grd_error *error);
grd_status grd_cuda_rgba_to_nv12(
    const grd_frame *source,
    uint32_t destination_width,
    uint32_t destination_height,
    uint8_t *destination,
    size_t destination_size,
    grd_error *error
);

#ifdef __cplusplus
}
#endif

#endif
