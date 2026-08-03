#include "grd/cuda_d3d11.h"

#include <cuda_runtime.h>
#include <cuda_d3d11_interop.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct grd_cuda_d3d11_uploader {
    ID3D11Device *device;
    ID3D11Texture2D *texture;
    cudaGraphicsResource_t graphics_resource;
    cudaStream_t stream;
    uint32_t width;
    uint32_t height;
};

__global__ static void nv12_to_rgba_kernel(
    cudaSurfaceObject_t surface,
    const unsigned char *y_plane,
    const unsigned char *uv_plane,
    int width,
    int height,
    int y_stride,
    int uv_stride
)
{
    const int x = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= width || y >= height) {
        return;
    }
    const int y_index = y * y_stride + x;
    const int uv_index = (y / 2) * uv_stride + (x & ~1);
    const float yy = (float)y_plane[y_index];
    const float u = (float)uv_plane[uv_index] - 128.0F;
    const float v = (float)uv_plane[uv_index + 1] - 128.0F;
    const float r = yy + 1.402F * v;
    const float g = yy - 0.344136F * u - 0.714136F * v;
    const float b = yy + 1.772F * u;
    uchar4 pixel;
    pixel.x = (unsigned char)(r < 0.0F ? 0.0F : r > 255.0F ? 255.0F : r);
    pixel.y = (unsigned char)(g < 0.0F ? 0.0F : g > 255.0F ? 255.0F : g);
    pixel.z = (unsigned char)(b < 0.0F ? 0.0F : b > 255.0F ? 255.0F : b);
    pixel.w = 255U;
    /* For 2D surfaces the x coordinate of surf2Dwrite is a byte offset:
     * each pixel occupies sizeof(uchar4) bytes on the row. */
    surf2Dwrite(pixel, surface, x * (int)sizeof(uchar4), y);
}

static grd_status d3d11_error(
    grd_error *error,
    grd_status code,
    const char *message
)
{
    if (error != NULL) {
        error->code = code;
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
    return code;
}

extern "C" grd_cuda_d3d11_uploader *grd_cuda_d3d11_uploader_create(
    void *d3d11_device,
    grd_error *error
)
{
    if (d3d11_device == NULL) {
        (void)d3d11_error(error, GRD_INVALID_ARGUMENT, "D3D11 device is missing");
        return NULL;
    }
    /* cudaD3D11GetDevice expects the DXGI adapter, not the D3D11 device:
     * query the device's adapter first, then match it to a CUDA device. */
    ID3D11Device *device = (ID3D11Device *)d3d11_device;
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *adapter = NULL;
    int cuda_device = 0;
    bool associated = false;
    if (device->QueryInterface(
            __uuidof(IDXGIDevice), (void **)&dxgi_device
        ) == S_OK &&
        dxgi_device->GetAdapter(&adapter) == S_OK &&
        cudaD3D11GetDevice(&cuda_device, adapter) == cudaSuccess &&
        cudaSetDevice(cuda_device) == cudaSuccess) {
        associated = true;
    }
    if (dxgi_device != NULL) {
        dxgi_device->Release();
    }
    if (adapter != NULL) {
        adapter->Release();
    }
    if (!associated) {
        (void)d3d11_error(
            error, GRD_NOT_SUPPORTED,
            "CUDA is not associated with the renderer's D3D11 device"
        );
        return NULL;
    }
    grd_cuda_d3d11_uploader *uploader =
        (grd_cuda_d3d11_uploader *)calloc(1U, sizeof(*uploader));
    if (uploader == NULL) {
        (void)d3d11_error(error, GRD_OUT_OF_MEMORY, "Unable to allocate CUDA uploader");
        return NULL;
    }
    uploader->device = device;
    uploader->device->AddRef();
    if (cudaStreamCreateWithFlags(&uploader->stream, cudaStreamNonBlocking) !=
        cudaSuccess) {
        grd_cuda_d3d11_uploader_destroy(uploader);
        (void)d3d11_error(
            error, GRD_OUT_OF_MEMORY, "Unable to allocate CUDA stream"
        );
        return NULL;
    }
    return uploader;
}

extern "C" grd_status grd_cuda_d3d11_uploader_upload(
    grd_cuda_d3d11_uploader *uploader,
    uint32_t width,
    uint32_t height,
    const uint8_t *y_device,
    const uint8_t *uv_device,
    uint32_t y_stride,
    uint32_t uv_stride,
    void **out_texture,
    grd_error *error
)
{
    if (uploader == NULL || out_texture == NULL || y_device == NULL ||
        uv_device == NULL || width == 0U || height == 0U ||
        y_stride == 0U || uv_stride == 0U) {
        return d3d11_error(error, GRD_INVALID_ARGUMENT, "Invalid CUDA upload");
    }
    if (uploader->texture == NULL || uploader->width != width ||
        uploader->height != height) {
        if (uploader->texture != NULL) {
            uploader->texture->Release();
            uploader->texture = NULL;
            if (uploader->graphics_resource != NULL) {
                (void)cudaGraphicsUnregisterResource(uploader->graphics_resource);
                uploader->graphics_resource = NULL;
            }
        }
        D3D11_TEXTURE2D_DESC description;
        memset(&description, 0, sizeof(description));
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1U;
        description.ArraySize = 1U;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1U;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D *texture = NULL;
        if (uploader->device->CreateTexture2D(
                &description, NULL, &texture
            ) != S_OK) {
            return d3d11_error(
                error, GRD_ERROR, "Unable to create CUDA D3D11 texture"
            );
        }
        if (cudaGraphicsD3D11RegisterResource(
                &uploader->graphics_resource,
                texture,
                cudaGraphicsRegisterFlagsNone
            ) != cudaSuccess) {
            texture->Release();
            return d3d11_error(
                error, GRD_NOT_SUPPORTED,
                "Failed to register the D3D11 texture with CUDA"
            );
        }
        uploader->texture = texture;
        uploader->width = width;
        uploader->height = height;
    }
    if (cudaGraphicsMapResources(
            1U, &uploader->graphics_resource, uploader->stream
        ) != cudaSuccess) {
        return d3d11_error(
            error, GRD_ERROR, "Failed to map the D3D11 texture with CUDA"
        );
    }
    cudaArray_t array = NULL;
    cudaError_t result = cudaGraphicsSubResourceGetMappedArray(
        &array, uploader->graphics_resource, 0U, 0U
    );
    cudaSurfaceObject_t surface = 0U;
    if (result == cudaSuccess) {
        cudaResourceDesc resource;
        memset(&resource, 0, sizeof(resource));
        resource.resType = cudaResourceTypeArray;
        resource.res.array.array = array;
        result = cudaCreateSurfaceObject(&surface, &resource);
    }
    if (result == cudaSuccess) {
        const dim3 threads(16U, 16U);
        const dim3 blocks((width + 15U) / 16U, (height + 15U) / 16U);
        nv12_to_rgba_kernel<<<blocks, threads, 0U, uploader->stream>>>(
            surface,
            y_device,
            uv_device,
            (int)width,
            (int)height,
            (int)y_stride,
            (int)uv_stride
        );
        result = cudaGetLastError();
    }
    if (surface != 0U) {
        (void)cudaDestroySurfaceObject(surface);
    }
    const cudaError_t unmap_result = cudaGraphicsUnmapResources(
        1U, &uploader->graphics_resource, uploader->stream
    );
    if (result == cudaSuccess) {
        result = unmap_result;
    }
    if (result == cudaSuccess) {
        result = cudaStreamSynchronize(uploader->stream);
    }
    if (result != cudaSuccess) {
        return d3d11_error(
            error, GRD_ERROR, "CUDA NV12-to-RGBA conversion failed"
        );
    }
    *out_texture = uploader->texture;
    return GRD_OK;
}

extern "C" void grd_cuda_d3d11_uploader_destroy(
    grd_cuda_d3d11_uploader *uploader
)
{
    if (uploader == NULL) {
        return;
    }
    if (uploader->graphics_resource != NULL) {
        (void)cudaGraphicsUnregisterResource(uploader->graphics_resource);
    }
    if (uploader->texture != NULL) {
        uploader->texture->Release();
    }
    if (uploader->stream != NULL) {
        (void)cudaStreamDestroy(uploader->stream);
    }
    if (uploader->device != NULL) {
        uploader->device->Release();
    }
    free(uploader);
}
