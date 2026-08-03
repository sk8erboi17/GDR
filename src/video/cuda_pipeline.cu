#include "grd/gpu.h"

#include <cuda_runtime.h>
#if defined(_WIN32)
#include <cuda_d3d11_interop.h>
#include <d3d11.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRD_CUDA_STREAMS 2U

/* Double-buffered staging: consecutive conversions alternate streams and
 * buffers so the RGBA→NV12 kernel of frame N+1 can run while NVENC is still
 * encoding frame N (the encoder collects the previous packet in between). */
struct grd_cuda_converter {
    unsigned char *device_rgba[GRD_CUDA_STREAMS];
    unsigned char *device_nv12[GRD_CUDA_STREAMS];
    unsigned char *host_rgba[GRD_CUDA_STREAMS];
    size_t rgba_capacity;
    size_t nv12_capacity;
    cudaStream_t streams[GRD_CUDA_STREAMS];
    unsigned buffer_index;
    unsigned pending_stream_mask;
#if defined(_WIN32)
    ID3D11Texture2D *d3d11_texture;
    cudaGraphicsResource_t d3d11_resource;
    cudaTextureObject_t pending_textures[GRD_CUDA_STREAMS];
#endif
};

__device__ static float sample_rgba_channel(
    const unsigned char *rgba,
    int stride,
    int source_width,
    int source_height,
    float x,
    float y,
    int channel,
    int bgra
)
{
    const float x0f = floorf(x);
    const float y0f = floorf(y);
    const float fx = x - x0f;
    const float fy = y - y0f;
    const int x0 = x0f < 0.0F
                       ? 0
                       : (x0f > (float)(source_width - 1)
                              ? source_width - 1
                              : (int)x0f);
    const int y0 = y0f < 0.0F
                       ? 0
                       : (y0f > (float)(source_height - 1)
                              ? source_height - 1
                              : (int)y0f);
    const int x1 = x0 + 1 < source_width ? x0 + 1 : x0;
    const int y1 = y0 + 1 < source_height ? y0 + 1 : y0;
    const int c = bgra != 0 ? (channel == 0 ? 2 : (channel == 2 ? 0 : 1))
                            : channel;
    const int offset_00 = y0 * stride + x0 * 4 + c;
    const int offset_10 = y0 * stride + x1 * 4 + c;
    const int offset_01 = y1 * stride + x0 * 4 + c;
    const int offset_11 = y1 * stride + x1 * 4 + c;
    const float top =
        rgba[offset_00] * (1.0F - fx) + rgba[offset_10] * fx;
    const float bottom =
        rgba[offset_01] * (1.0F - fx) + rgba[offset_11] * fx;
    return top * (1.0F - fy) + bottom * fy;
}

__global__ static void rgba_to_nv12_kernel(
    const unsigned char *rgba,
    unsigned char *y_plane,
    unsigned char *uv_plane,
    int width,
    int height,
    int source_width,
    int source_height,
    int stride,
    int bgra,
    int destination_y_stride,
    int destination_uv_stride
)
{
    const int x = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= width || y >= height) {
        return;
    }
    /* Bilinear sampling in source space: crisp when downscaling and smooth
     * when upscaling, instead of nearest-neighbour aliasing. */
    const float source_x =
        ((float)x + 0.5F) * (float)source_width / (float)width - 0.5F;
    const float source_y =
        ((float)y + 0.5F) * (float)source_height / (float)height - 0.5F;
    const float r = sample_rgba_channel(
        rgba, stride, source_width, source_height,
        source_x, source_y, 0, bgra
    );
    const float g = sample_rgba_channel(
        rgba, stride, source_width, source_height,
        source_x, source_y, 1, bgra
    );
    const float b = sample_rgba_channel(
        rgba, stride, source_width, source_height,
        source_x, source_y, 2, bgra
    );
    y_plane[y * destination_y_stride + x] = (unsigned char)(
        16.0F + 0.183F * r + 0.614F * g + 0.062F * b
    );
    if ((x & 1) == 0 && (y & 1) == 0) {
        /* Chroma is sampled at the centre of the 2x2 luma block. */
        const float source_cx =
            ((float)(x + 1) + 0.5F) * (float)source_width / (float)width - 0.5F;
        const float source_cy =
            ((float)(y + 1) + 0.5F) * (float)source_height / (float)height - 0.5F;
        const float cr = sample_rgba_channel(
            rgba, stride, source_width, source_height,
            source_cx, source_cy, 0, bgra
        );
        const float cg = sample_rgba_channel(
            rgba, stride, source_width, source_height,
            source_cx, source_cy, 1, bgra
        );
        const float cb = sample_rgba_channel(
            rgba, stride, source_width, source_height,
            source_cx, source_cy, 2, bgra
        );
        const int uv = (y / 2) * destination_uv_stride + x;
        uv_plane[uv] = (unsigned char)(
            128.0F - 0.101F * cr - 0.339F * cg + 0.439F * cb
        );
        uv_plane[uv + 1] = (unsigned char)(
            128.0F + 0.439F * cr - 0.399F * cg - 0.040F * cb
        );
    }
}

#if defined(_WIN32)
__device__ static float3 sample_d3d11_bgra(
    cudaTextureObject_t texture,
    float x,
    float y
)
{
    /* The texture unit performs the four-tap bilinear interpolation in one
     * fetch. DXGI BGRA is exposed as normalized B,G,R,A channels. The old
     * surface path issued four reads per colour sample (and eight for every
     * chroma-producing thread), contending heavily with a game on the same
     * GPU during map/foliage loads. */
    const float4 bgra = tex2D<float4>(texture, x, y);
    return make_float3(
        bgra.z * 255.0F,
        bgra.y * 255.0F,
        bgra.x * 255.0F
    );
}

__global__ static void d3d11_bgra_to_nv12_kernel(
    cudaTextureObject_t texture,
    unsigned char *y_plane,
    unsigned char *uv_plane,
    int width,
    int height,
    int source_width,
    int source_height,
    int destination_y_stride,
    int destination_uv_stride
)
{
    const int x = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= width || y >= height) {
        return;
    }
    const float source_x =
        ((float)x + 0.5F) * (float)source_width / (float)width;
    const float source_y =
        ((float)y + 0.5F) * (float)source_height / (float)height;
    const float3 rgb = sample_d3d11_bgra(
        texture, source_x, source_y
    );
    y_plane[y * destination_y_stride + x] = (unsigned char)(
        16.0F + 0.183F * rgb.x + 0.614F * rgb.y + 0.062F * rgb.z
    );
    if ((x & 1) == 0 && (y & 1) == 0) {
        const float source_cx =
            ((float)x + 1.0F) * (float)source_width / (float)width;
        const float source_cy =
            ((float)y + 1.0F) * (float)source_height / (float)height;
        const float3 chroma = sample_d3d11_bgra(
            texture, source_cx, source_cy
        );
        const int uv = (y / 2) * destination_uv_stride + x;
        uv_plane[uv] = (unsigned char)(
            128.0F - 0.101F * chroma.x - 0.339F * chroma.y +
            0.439F * chroma.z
        );
        uv_plane[uv + 1] = (unsigned char)(
            128.0F + 0.439F * chroma.x - 0.399F * chroma.y -
            0.040F * chroma.z
        );
    }
}
#endif

extern "C" bool grd_cuda_available(char *name, size_t name_capacity)
{
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count <= 0) {
        return false;
    }
    cudaDeviceProp properties;
    if (cudaGetDeviceProperties(&properties, 0) != cudaSuccess) {
        return false;
    }
    if (name != NULL && name_capacity != 0U) {
        (void)snprintf(name, name_capacity, "%s", properties.name);
    }
    return true;
}

extern "C" grd_cuda_converter *grd_cuda_converter_create(void)
{
    grd_cuda_converter *converter =
        (grd_cuda_converter *)calloc(1U, sizeof(*converter));
    if (converter == NULL) {
        return NULL;
    }
    for (unsigned index = 0U; index < GRD_CUDA_STREAMS; ++index) {
        if (cudaStreamCreateWithFlags(
                &converter->streams[index], cudaStreamNonBlocking
            ) != cudaSuccess) {
            grd_cuda_converter_destroy(converter);
            return NULL;
        }
    }
    return converter;
}

extern "C" void grd_cuda_converter_destroy(grd_cuda_converter *converter)
{
    if (converter == NULL) {
        return;
    }
    for (unsigned index = 0U; index < GRD_CUDA_STREAMS; ++index) {
        if (converter->streams[index] != NULL) {
            (void)cudaStreamSynchronize(converter->streams[index]);
        }
#if defined(_WIN32)
        if (converter->pending_textures[index] != 0U) {
            (void)cudaDestroyTextureObject(
                converter->pending_textures[index]
            );
            converter->pending_textures[index] = 0U;
        }
#endif
    }
#if defined(_WIN32)
    if (converter->d3d11_resource != NULL) {
        (void)cudaGraphicsUnregisterResource(converter->d3d11_resource);
    }
    if (converter->d3d11_texture != NULL) {
        converter->d3d11_texture->Release();
    }
#endif
    for (unsigned index = 0U; index < GRD_CUDA_STREAMS; ++index) {
        cudaFree(converter->device_rgba[index]);
        cudaFree(converter->device_nv12[index]);
        cudaFreeHost(converter->host_rgba[index]);
        cudaStreamDestroy(converter->streams[index]);
    }
    free(converter);
}

static unsigned next_buffer_index(grd_cuda_converter *converter)
{
    const unsigned index = converter->buffer_index;
    converter->buffer_index =
        (converter->buffer_index + 1U) % GRD_CUDA_STREAMS;
    return index;
}

static grd_status ensure_capacity(
    grd_cuda_converter *converter,
    size_t rgba_size,
    size_t nv12_size,
    grd_error *error
)
{
    if (converter->rgba_capacity < rgba_size) {
        for (unsigned index = 0U; index < GRD_CUDA_STREAMS; ++index) {
            unsigned char *buffer = NULL;
            unsigned char *host_buffer = NULL;
            const cudaError_t result = cudaMalloc((void **)&buffer, rgba_size);
            if (result != cudaSuccess) {
                if (error != NULL) {
                    error->code = GRD_OUT_OF_MEMORY;
                    (void)snprintf(
                        error->message, sizeof(error->message),
                        "CUDA: %s", cudaGetErrorString(result)
                    );
                }
                return GRD_OUT_OF_MEMORY;
            }
            const cudaError_t host_result = cudaHostAlloc(
                (void **)&host_buffer, rgba_size, cudaHostAllocPortable
            );
            if (host_result != cudaSuccess) {
                cudaFree(buffer);
                if (error != NULL) {
                    error->code = GRD_OUT_OF_MEMORY;
                    (void)snprintf(
                        error->message, sizeof(error->message),
                        "CUDA pinned host buffer: %s",
                        cudaGetErrorString(host_result)
                    );
                }
                return GRD_OUT_OF_MEMORY;
            }
            cudaFree(converter->device_rgba[index]);
            cudaFreeHost(converter->host_rgba[index]);
            converter->device_rgba[index] = buffer;
            converter->host_rgba[index] = host_buffer;
        }
        converter->rgba_capacity = rgba_size;
    }
    if (converter->nv12_capacity < nv12_size) {
        for (unsigned index = 0U; index < GRD_CUDA_STREAMS; ++index) {
            unsigned char *buffer = NULL;
            const cudaError_t result = cudaMalloc((void **)&buffer, nv12_size);
            if (result != cudaSuccess) {
                if (error != NULL) {
                    error->code = GRD_OUT_OF_MEMORY;
                    (void)snprintf(
                        error->message, sizeof(error->message),
                        "CUDA: %s", cudaGetErrorString(result)
                    );
                }
                return GRD_OUT_OF_MEMORY;
            }
            cudaFree(converter->device_nv12[index]);
            converter->device_nv12[index] = buffer;
        }
        converter->nv12_capacity = nv12_size;
    }
    return GRD_OK;
}

static grd_status conversion_set_error(
    grd_error *error,
    const char *prefix,
    cudaError_t result
)
{
    if (error != NULL) {
        error->code = GRD_ERROR;
        (void)snprintf(
            error->message,
            sizeof(error->message),
            "%s: %s",
            prefix,
            cudaGetErrorString(result)
        );
    }
    return GRD_ERROR;
}

static grd_status synchronize_all_streams(
    grd_cuda_converter *converter,
    grd_error *error
)
{
    for (unsigned index = 0U; index < GRD_CUDA_STREAMS; ++index) {
        const cudaError_t result =
            cudaStreamSynchronize(converter->streams[index]);
        if (result != cudaSuccess) {
            return conversion_set_error(error, "CUDA sync", result);
        }
#if defined(_WIN32)
        if (converter->pending_textures[index] != 0U) {
            const cudaError_t destroy_result = cudaDestroyTextureObject(
                converter->pending_textures[index]
            );
            converter->pending_textures[index] = 0U;
            if (destroy_result != cudaSuccess) {
                return conversion_set_error(
                    error, "CUDA texture destroy", destroy_result
                );
            }
        }
#endif
    }
    converter->pending_stream_mask = 0U;
    return GRD_OK;
}

#if defined(_WIN32)
static void release_d3d11_interop(grd_cuda_converter *converter)
{
    if (converter->d3d11_resource != NULL) {
        (void)cudaGraphicsUnregisterResource(converter->d3d11_resource);
        converter->d3d11_resource = NULL;
    }
    if (converter->d3d11_texture != NULL) {
        converter->d3d11_texture->Release();
        converter->d3d11_texture = NULL;
    }
}

static grd_status ensure_d3d11_interop(
    grd_cuda_converter *converter,
    const grd_frame *source,
    grd_error *error
)
{
    ID3D11Texture2D *texture = (ID3D11Texture2D *)source->owner;
    if (texture == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    if (converter->d3d11_texture == texture &&
        converter->d3d11_resource != NULL) {
        return GRD_OK;
    }
    if (synchronize_all_streams(converter, error) != GRD_OK) {
        return GRD_ERROR;
    }
    release_d3d11_interop(converter);

    D3D11_TEXTURE2D_DESC description;
    memset(&description, 0, sizeof(description));
    texture->GetDesc(&description);
    if (description.Width != source->width ||
        description.Height != source->height ||
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        description.ArraySize != 1U || description.MipLevels != 1U) {
        if (error != NULL) {
            error->code = GRD_NOT_SUPPORTED;
            (void)snprintf(
                error->message, sizeof(error->message),
                "CUDA D3D11: unsupported BGRA8 texture"
            );
        }
        return GRD_NOT_SUPPORTED;
    }

    ID3D11Device *d3d11_device = NULL;
    texture->GetDevice(&d3d11_device);
    unsigned int device_count = 0U;
    int interop_device = -1;
    cudaError_t result = d3d11_device != NULL
                             ? cudaD3D11GetDevices(
                                   &device_count,
                                   &interop_device,
                                   1U,
                                   d3d11_device,
                                   cudaD3D11DeviceListAll
                               )
                             : cudaErrorInvalidDevice;
    if (d3d11_device != NULL) {
        d3d11_device->Release();
    }
    int current_device = -1;
    if (result == cudaSuccess) {
        result = cudaGetDevice(&current_device);
    }
    if (result != cudaSuccess || device_count == 0U ||
        interop_device != current_device) {
        if (error != NULL) {
            error->code = GRD_NOT_SUPPORTED;
            (void)snprintf(
                error->message, sizeof(error->message),
                "CUDA D3D11: adapter is not associated with the active CUDA device"
            );
        }
        return GRD_NOT_SUPPORTED;
    }

    result = cudaGraphicsD3D11RegisterResource(
        &converter->d3d11_resource,
        texture,
        cudaGraphicsRegisterFlagsNone
    );
    if (result != cudaSuccess) {
        return conversion_set_error(
            error, "CUDA D3D11 texture registration", result
        );
    }
    texture->AddRef();
    converter->d3d11_texture = texture;
    return GRD_OK;
}

static grd_status d3d11_bgra_to_nv12_device_async(
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
    const grd_status interop_status =
        ensure_d3d11_interop(converter, source, error);
    if (interop_status != GRD_OK) {
        return interop_status;
    }
    const unsigned slot = next_buffer_index(converter);
    cudaError_t result = cudaGraphicsMapResources(
        1U, &converter->d3d11_resource, converter->streams[slot]
    );
    if (result != cudaSuccess) {
        return conversion_set_error(error, "CUDA map texture D3D11", result);
    }
    cudaArray_t array = NULL;
    result = cudaGraphicsSubResourceGetMappedArray(
        &array, converter->d3d11_resource, 0U, 0U
    );
    cudaTextureObject_t texture = 0U;
    if (result == cudaSuccess) {
        cudaResourceDesc resource;
        memset(&resource, 0, sizeof(resource));
        resource.resType = cudaResourceTypeArray;
        resource.res.array.array = array;
        cudaTextureDesc texture_description;
        memset(&texture_description, 0, sizeof(texture_description));
        texture_description.addressMode[0] = cudaAddressModeClamp;
        texture_description.addressMode[1] = cudaAddressModeClamp;
        texture_description.filterMode = cudaFilterModeLinear;
        texture_description.readMode = cudaReadModeNormalizedFloat;
        texture_description.normalizedCoords = 0;
        result = cudaCreateTextureObject(
            &texture, &resource, &texture_description, NULL
        );
    }
    if (result == cudaSuccess) {
        const dim3 threads(16U, 16U);
        const dim3 blocks(
            (destination_width + 15U) / 16U,
            (destination_height + 15U) / 16U
        );
        d3d11_bgra_to_nv12_kernel<<<
            blocks, threads, 0U, converter->streams[slot]
        >>>(
            texture,
            destination_y_device,
            destination_uv_device,
            (int)destination_width,
            (int)destination_height,
            (int)source->width,
            (int)source->height,
            (int)destination_y_stride,
            (int)destination_uv_stride
        );
        result = cudaGetLastError();
    }
    const cudaError_t unmap_result = cudaGraphicsUnmapResources(
        1U, &converter->d3d11_resource, converter->streams[slot]
    );
    if (result == cudaSuccess) {
        result = unmap_result;
    }
    if (result != cudaSuccess) {
        /* Keep the texture handle alive until the asynchronous kernel and
         * unmap queued ahead of us finish. This synchronization is confined
         * to the failure path; successful frames release it in the normal
         * converter sync below. */
        (void)cudaStreamSynchronize(converter->streams[slot]);
        if (texture != 0U) {
            (void)cudaDestroyTextureObject(texture);
        }
        return conversion_set_error(
            error, "CUDA D3D11 texture conversion", result
        );
    }
    converter->pending_textures[slot] = texture;
    converter->pending_stream_mask |= 1U << slot;
    return GRD_OK;
}
#endif

extern "C" grd_status grd_cuda_converter_sync(
    grd_cuda_converter *converter,
    grd_error *error
)
{
    if (converter == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    const unsigned pending = converter->pending_stream_mask;
    for (unsigned index = 0U; index < GRD_CUDA_STREAMS; ++index) {
        if ((pending & (1U << index)) == 0U) {
            continue;
        }
        const cudaError_t result =
            cudaStreamSynchronize(converter->streams[index]);
        if (result != cudaSuccess) {
            return conversion_set_error(error, "CUDA sync", result);
        }
#if defined(_WIN32)
        if (converter->pending_textures[index] != 0U) {
            const cudaError_t destroy_result = cudaDestroyTextureObject(
                converter->pending_textures[index]
            );
            converter->pending_textures[index] = 0U;
            if (destroy_result != cudaSuccess) {
                return conversion_set_error(
                    error, "CUDA texture destroy", destroy_result
                );
            }
        }
#endif
        converter->pending_stream_mask &= ~(1U << index);
    }
    return GRD_OK;
}

extern "C" grd_status grd_cuda_converter_rgba_to_nv12_device_async(
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
    if (converter == NULL || source == NULL ||
        destination_y_device == NULL || destination_uv_device == NULL ||
        destination_width == 0U || destination_height == 0U ||
        (destination_width & 1U) != 0U ||
        (destination_height & 1U) != 0U) {
        return GRD_INVALID_ARGUMENT;
    }
#if defined(_WIN32)
    if (source->format == GRD_PIXEL_D3D11_BGRA) {
        if (source->owner == NULL || source->width == 0U ||
            source->height == 0U) {
            return GRD_INVALID_ARGUMENT;
        }
        return d3d11_bgra_to_nv12_device_async(
            converter,
            source,
            destination_width,
            destination_height,
            destination_y_device,
            destination_uv_device,
            destination_y_stride,
            destination_uv_stride,
            error
        );
    }
#endif
    if ((source->format != GRD_PIXEL_RGBA8 &&
         source->format != GRD_PIXEL_BGRA8) ||
        source->data == NULL || source->stride < source->width * 4U) {
        return GRD_INVALID_ARGUMENT;
    }
    const size_t rgba_size = (size_t)source->stride * source->height;
    if (ensure_capacity(converter, rgba_size, 0U, error) != GRD_OK) {
        return GRD_OUT_OF_MEMORY;
    }
    const unsigned slot = next_buffer_index(converter);
    memcpy(
        converter->host_rgba[slot],
        source->data,
        rgba_size
    );
    cudaError_t result = cudaMemcpyAsync(
        converter->device_rgba[slot],
        converter->host_rgba[slot],
        rgba_size,
        cudaMemcpyHostToDevice,
        converter->streams[slot]
    );
    if (result == cudaSuccess) {
        const dim3 threads(16U, 16U);
        const dim3 blocks(
            (destination_width + 15U) / 16U,
            (destination_height + 15U) / 16U
        );
        rgba_to_nv12_kernel<<<blocks, threads, 0U, converter->streams[slot]>>>(
            converter->device_rgba[slot],
            destination_y_device,
            destination_uv_device,
            (int)destination_width,
            (int)destination_height,
            (int)source->width,
            (int)source->height,
            (int)source->stride,
            source->format == GRD_PIXEL_BGRA8 ? 1 : 0,
            (int)destination_y_stride,
            (int)destination_uv_stride
        );
        result = cudaGetLastError();
    }
    if (result != cudaSuccess) {
        return conversion_set_error(error, "CUDA device NV12", result);
    }
    /* No sync here: the caller overlaps this conversion with NVENC encoding
     * and calls grd_cuda_converter_sync before reading the buffers. */
    converter->pending_stream_mask |= 1U << slot;
    return GRD_OK;
}

extern "C" grd_status grd_cuda_converter_rgba_to_nv12_device(
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
    const grd_status status = grd_cuda_converter_rgba_to_nv12_device_async(
        converter,
        source,
        destination_width,
        destination_height,
        destination_y_device,
        destination_uv_device,
        destination_y_stride,
        destination_uv_stride,
        error
    );
    if (status != GRD_OK) {
        return status;
    }
    return grd_cuda_converter_sync(converter, error);
}

extern "C" grd_status grd_cuda_converter_rgba_to_nv12(
    grd_cuda_converter *converter,
    const grd_frame *source,
    uint32_t destination_width,
    uint32_t destination_height,
    uint8_t *destination,
    size_t destination_size,
    grd_error *error
)
{
    if (converter == NULL || source == NULL || destination == NULL ||
        (source->format != GRD_PIXEL_RGBA8 &&
         source->format != GRD_PIXEL_BGRA8) ||
        source->stride < source->width * 4U ||
        destination_width == 0U || destination_height == 0U ||
        (destination_width & 1U) != 0U ||
        (destination_height & 1U) != 0U) {
        return GRD_INVALID_ARGUMENT;
    }
    const size_t rgba_size = (size_t)source->stride * source->height;
    const size_t nv12_size =
        (size_t)destination_width * destination_height * 3U / 2U;
    if (destination_size < nv12_size) {
        return GRD_INVALID_ARGUMENT;
    }
    cudaError_t result =
        ensure_capacity(converter, rgba_size, nv12_size, error) == GRD_OK
            ? cudaSuccess
            : cudaErrorMemoryAllocation;
    const unsigned slot = next_buffer_index(converter);
    if (result == cudaSuccess) {
        memcpy(converter->host_rgba[slot], source->data, rgba_size);
        result = cudaMemcpyAsync(
            converter->device_rgba[slot],
            converter->host_rgba[slot],
            rgba_size,
            cudaMemcpyHostToDevice,
            converter->streams[slot]
        );
    }
    if (result == cudaSuccess) {
        const dim3 threads(16U, 16U);
        const dim3 blocks(
            (destination_width + 15U) / 16U,
            (destination_height + 15U) / 16U
        );
        rgba_to_nv12_kernel<<<blocks, threads, 0U, converter->streams[slot]>>>(
            converter->device_rgba[slot],
            converter->device_nv12[slot],
            converter->device_nv12[slot] +
                (size_t)destination_width * destination_height,
            (int)destination_width,
            (int)destination_height,
            (int)source->width,
            (int)source->height,
            (int)source->stride,
            source->format == GRD_PIXEL_BGRA8 ? 1 : 0,
            (int)destination_width,
            (int)destination_width
        );
        result = cudaGetLastError();
    }
    if (result == cudaSuccess) {
        result = cudaMemcpyAsync(
            destination, converter->device_nv12[slot], nv12_size,
            cudaMemcpyDeviceToHost, converter->streams[slot]
        );
    }
    if (result == cudaSuccess) {
        result = cudaStreamSynchronize(converter->streams[slot]);
    }
    if (result != cudaSuccess) {
        if (error != NULL) {
            error->code = GRD_ERROR;
            (void)snprintf(
                error->message,
                sizeof(error->message),
                "CUDA: %s",
                cudaGetErrorString(result)
            );
        }
        return GRD_ERROR;
    }
    return GRD_OK;
}

extern "C" grd_status grd_cuda_rgba_to_nv12(
    const grd_frame *source,
    uint32_t destination_width,
    uint32_t destination_height,
    uint8_t *destination,
    size_t destination_size,
    grd_error *error
)
{
    grd_cuda_converter *converter = grd_cuda_converter_create();
    if (converter == NULL) {
        return GRD_NOT_SUPPORTED;
    }
    const grd_status status = grd_cuda_converter_rgba_to_nv12(
        converter,
        source,
        destination_width,
        destination_height,
        destination,
        destination_size,
        error
    );
    grd_cuda_converter_destroy(converter);
    return status;
}
