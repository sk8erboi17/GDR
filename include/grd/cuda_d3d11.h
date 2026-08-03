#ifndef GRD_CUDA_D3D11_H
#define GRD_CUDA_D3D11_H

#include "grd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grd_cuda_d3d11_uploader grd_cuda_d3d11_uploader;

/* GPU-side NV12→RGBA uploader for the Windows/NVIDIA client. The output
 * ID3D11Texture2D is created on the renderer's D3D11 device and filled by a
 * CUDA kernel reading the NVDEC planes directly, so decoded frames stay on
 * the GPU from the decoder to presentation (no device→host→device round
 * trip). */
grd_cuda_d3d11_uploader *grd_cuda_d3d11_uploader_create(
    void *d3d11_device,
    grd_error *error
);
grd_status grd_cuda_d3d11_uploader_upload(
    grd_cuda_d3d11_uploader *uploader,
    uint32_t width,
    uint32_t height,
    const uint8_t *y_device,
    const uint8_t *uv_device,
    uint32_t y_stride,
    uint32_t uv_stride,
    void **out_texture,
    grd_error *error
);
void grd_cuda_d3d11_uploader_destroy(grd_cuda_d3d11_uploader *uploader);

#ifdef __cplusplus
}
#endif

#endif
