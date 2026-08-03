#define COBJMACROS

#include "grd/platform.h"
#include "grd/audio.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <initguid.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) || defined(__clang__)
/* The Windows SDK 10.0.26100 import libraries shipped with VS Build Tools do
 * not export these classic COM/D3D GUIDs (the MinGW headers define them
 * inline instead), so define them here like the MinGW headers do. */
DEFINE_GUID(CLSID_MMDeviceEnumerator, 0xbcde0395, 0xe52f, 0x467c, 0x8e,0x3d, 0xc4,0x57,0x92,0x91,0x69,0x2e);
DEFINE_GUID(IID_IMMDeviceEnumerator, 0xa95664d2, 0x9614, 0x4f35, 0xa7,0x46, 0xde,0x8d,0xb6,0x36,0x17,0xe6);
DEFINE_GUID(IID_IAudioClient, 0x1cb9ad4c, 0xdbfa, 0x4c32, 0xb1,0x78, 0xc2,0xf5,0x68,0xa7,0x03,0xb2);
DEFINE_GUID(IID_IAudioCaptureClient, 0xc8adbd64, 0xe71e, 0x48a0, 0xa4,0xde, 0x18,0x5c,0x39,0x5c,0xd3,0x17);
DEFINE_GUID(IID_ID3D11Texture2D, 0x6f15aaf2, 0xd208, 0x4e89, 0x9a,0xb4, 0x48,0x95,0x35,0xd3,0x4f,0x9c);
DEFINE_GUID(IID_IDXGIFactory1, 0x770aae78, 0xf26f, 0x4dba, 0xa8,0x29, 0x25,0x3c,0x83,0xd1,0xb3,0x87);
DEFINE_GUID(IID_IDXGIOutput1, 0x00cddea8, 0x939b, 0x4b83, 0xa3,0x40, 0xa6,0x85,0x22,0x66,0x66,0xcc);
#endif

static HMONITOR g_monitors[GRD_MAX_MONITORS];
static size_t g_monitor_count;

/* Desktop Duplication is kept alive between captures. Creating a D3D device,
 * a duplication object and a staging texture for every frame adds several
 * milliseconds and makes the capture rate depend on GDI. */
static ID3D11Device *g_dup_device;
static ID3D11DeviceContext *g_dup_context;
static IDXGIOutput1 *g_dup_output;
static IDXGIOutputDuplication *g_duplication;
static ID3D11Texture2D *g_dup_staging;
static ID3D11Texture2D *g_dup_gpu_texture;
static uint32_t g_dup_monitor_id = UINT32_MAX;
static uint32_t g_dup_width;
static uint32_t g_dup_height;
static grd_cursor_shape g_dup_cursor_shape;
static bool g_dup_cursor_shape_valid;
static uint8_t *g_capture_pixels;
static size_t g_capture_pixels_capacity;
static grd_capture_timing g_capture_timing;
static IMMDeviceEnumerator *g_audio_enumerator;
static IMMDevice *g_audio_device;
static IAudioClient *g_audio_client;
static IAudioCaptureClient *g_audio_capture;
static bool g_audio_com_initialized;

#define AUDIO_RING_FRAMES (GRD_AUDIO_SAMPLE_RATE * 2U)
static const GUID g_float_audio_subtype = {
    0x00000003,
    0x0000,
    0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
};
static float g_audio_ring[AUDIO_RING_FRAMES * GRD_AUDIO_CHANNELS];
static size_t g_audio_read_index;
static size_t g_audio_write_index;
static size_t g_audio_frame_count;

static void release_d3d11_capture_texture(void *owner)
{
    if (owner != NULL) {
        ID3D11Texture2D_Release((ID3D11Texture2D *)owner);
    }
}

static void set_error(grd_error *error, grd_status code, const char *message)
{
    if (error != NULL) {
        error->code = code;
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
}

static BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC dc, LPRECT rectangle, LPARAM data)
{
    (void)dc;
    (void)rectangle;
    (void)data;
    if (g_monitor_count < GRD_MAX_MONITORS) {
        g_monitors[g_monitor_count++] = monitor;
    }
    return TRUE;
}

static void refresh_monitors(void)
{
    g_monitor_count = 0U;
    (void)EnumDisplayMonitors(NULL, NULL, collect_monitor, 0);
}

static void release_duplication_session(void)
{
    if (g_duplication != NULL) {
        IDXGIOutputDuplication_Release(g_duplication);
        g_duplication = NULL;
    }
    memset(&g_dup_cursor_shape, 0, sizeof(g_dup_cursor_shape));
    g_dup_cursor_shape_valid = false;
}

static void release_duplication(void)
{
    release_duplication_session();
    if (g_dup_output != NULL) {
        IDXGIOutput1_Release(g_dup_output);
        g_dup_output = NULL;
    }
    if (g_dup_staging != NULL) {
        ID3D11Texture2D_Release(g_dup_staging);
        g_dup_staging = NULL;
    }
    if (g_dup_gpu_texture != NULL) {
        ID3D11Texture2D_Release(g_dup_gpu_texture);
        g_dup_gpu_texture = NULL;
    }
    if (g_dup_context != NULL) {
        ID3D11DeviceContext_Release(g_dup_context);
        g_dup_context = NULL;
    }
    if (g_dup_device != NULL) {
        ID3D11Device_Release(g_dup_device);
        g_dup_device = NULL;
    }
    g_dup_monitor_id = UINT32_MAX;
    g_dup_width = 0U;
    g_dup_height = 0U;
    free(g_capture_pixels);
    g_capture_pixels = NULL;
    g_capture_pixels_capacity = 0U;
}

static grd_status ensure_duplication(uint32_t monitor_id, grd_error *error)
{
    if (g_duplication != NULL && g_dup_staging != NULL &&
        g_dup_monitor_id == monitor_id) {
        return GRD_OK;
    }

    /* The first watchdog stage releases only IDXGIOutputDuplication. Reuse
     * the adapter-matched D3D11 device and both persistent textures: this is
     * enough to recover an invalid duplication session without forcing CUDA
     * to observe a new device or paying the full allocation cost during a
     * game/compositor presentation hitch. If the mode changed, fall through
     * to the full topology/device path below. */
    if (g_duplication == NULL && g_dup_output != NULL &&
        g_dup_device != NULL && g_dup_staging != NULL &&
        g_dup_monitor_id == monitor_id) {
        HRESULT recreate_result = IDXGIOutput1_DuplicateOutput(
            g_dup_output, (IUnknown *)g_dup_device, &g_duplication
        );
        if (SUCCEEDED(recreate_result) && g_duplication != NULL) {
            DXGI_OUTDUPL_DESC recreated_desc;
            memset(&recreated_desc, 0, sizeof(recreated_desc));
            IDXGIOutputDuplication_GetDesc(
                g_duplication, &recreated_desc
            );
            if ((recreated_desc.ModeDesc.Width == 0U ||
                 recreated_desc.ModeDesc.Width == g_dup_width) &&
                (recreated_desc.ModeDesc.Height == 0U ||
                 recreated_desc.ModeDesc.Height == g_dup_height)) {
                return GRD_OK;
            }
            release_duplication_session();
        }
        /* A stale output/device or a mode-size change needs the complete
         * recreation. Do it immediately rather than entering the CPU/GDI
         * compatibility fallback for a transient session failure. */
    }
    release_duplication();
    if (monitor_id >= g_monitor_count) {
        return GRD_INVALID_ARGUMENT;
    }

    IDXGIFactory1 *factory = NULL;
    HRESULT result = CreateDXGIFactory1(
        &IID_IDXGIFactory1, (void **)&factory
    );
    if (FAILED(result)) {
        set_error(error, GRD_NOT_SUPPORTED, "DXGI factory is unavailable");
        return GRD_NOT_SUPPORTED;
    }

    IDXGIAdapter1 *selected_adapter = NULL;
    IDXGIOutput1 *selected_output = NULL;
    DXGI_OUTPUT_DESC selected_desc;
    memset(&selected_desc, 0, sizeof(selected_desc));
    for (UINT adapter_index = 0U; selected_adapter == NULL; ++adapter_index) {
        IDXGIAdapter1 *adapter = NULL;
        result = IDXGIFactory1_EnumAdapters1(factory, adapter_index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(result) || adapter == NULL) {
            continue;
        }
        for (UINT output_index = 0U; output_index < 32U; ++output_index) {
            IDXGIOutput *output = NULL;
            result = IDXGIAdapter1_EnumOutputs(adapter, output_index, &output);
            if (result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(result) || output == NULL) {
                continue;
            }
            DXGI_OUTPUT_DESC description;
            memset(&description, 0, sizeof(description));
            result = IDXGIOutput_GetDesc(output, &description);
            if (SUCCEEDED(result) && description.Monitor == g_monitors[monitor_id]) {
                result = IDXGIOutput_QueryInterface(
                    output, &IID_IDXGIOutput1, (void **)&selected_output
                );
                if (SUCCEEDED(result)) {
                    selected_adapter = adapter;
                    selected_desc = description;
                    IDXGIOutput_Release(output);
                    break;
                }
            }
            IDXGIOutput_Release(output);
        }
        if (selected_adapter == NULL) {
            IDXGIAdapter1_Release(adapter);
        }
    }
    IDXGIFactory1_Release(factory);
    if (selected_adapter == NULL || selected_output == NULL) {
        if (selected_output != NULL) IDXGIOutput1_Release(selected_output);
        set_error(error, GRD_NOT_SUPPORTED, "DXGI display was not found");
        return GRD_NOT_SUPPORTED;
    }

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };
    D3D_FEATURE_LEVEL selected_level;
    result = D3D11CreateDevice(
        (IDXGIAdapter *)selected_adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,
        (UINT)(sizeof(levels) / sizeof(levels[0])),
        D3D11_SDK_VERSION,
        &g_dup_device,
        &selected_level,
        &g_dup_context
    );
    IDXGIAdapter1_Release(selected_adapter);
    (void)selected_level;
    if (FAILED(result)) {
        IDXGIOutput1_Release(selected_output);
        set_error(error, GRD_NOT_SUPPORTED, "D3D11 device is unavailable");
        release_duplication();
        return GRD_NOT_SUPPORTED;
    }
    result = IDXGIOutput1_DuplicateOutput(
        selected_output, (IUnknown *)g_dup_device, &g_duplication
    );
    if (FAILED(result)) {
        IDXGIOutput1_Release(selected_output);
        set_error(error, GRD_NOT_SUPPORTED, "Desktop Duplication is unavailable");
        release_duplication();
        return GRD_NOT_SUPPORTED;
    }
    g_dup_output = selected_output;

    DXGI_OUTDUPL_DESC duplication_desc;
    memset(&duplication_desc, 0, sizeof(duplication_desc));
    IDXGIOutputDuplication_GetDesc(g_duplication, &duplication_desc);
    const UINT width = duplication_desc.ModeDesc.Width != 0U
                           ? duplication_desc.ModeDesc.Width
                           : (UINT)(selected_desc.DesktopCoordinates.right -
                                    selected_desc.DesktopCoordinates.left);
    const UINT height = duplication_desc.ModeDesc.Height != 0U
                            ? duplication_desc.ModeDesc.Height
                            : (UINT)(selected_desc.DesktopCoordinates.bottom -
                                     selected_desc.DesktopCoordinates.top);
    if (width == 0U || height == 0U ||
        (size_t)width > SIZE_MAX / ((size_t)height * 4U)) {
        set_error(error, GRD_INVALID_ARGUMENT, "Invalid Desktop Duplication dimensions");
        release_duplication();
        return GRD_INVALID_ARGUMENT;
    }
    D3D11_TEXTURE2D_DESC texture_desc;
    memset(&texture_desc, 0, sizeof(texture_desc));
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1U;
    texture_desc.ArraySize = 1U;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_desc.SampleDesc.Count = 1U;
    texture_desc.Usage = D3D11_USAGE_STAGING;
    texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    result = ID3D11Device_CreateTexture2D(
        g_dup_device, &texture_desc, NULL, &g_dup_staging
    );
    if (FAILED(result)) {
        set_error(error, GRD_OUT_OF_MEMORY, "Unable to allocate Desktop Duplication texture");
        release_duplication();
        return GRD_OUT_OF_MEMORY;
    }
    /* GPU-resident copy used by the CUDA/NVENC host path. Desktop
     * Duplication owns the acquired surface only until ReleaseFrame, so copy
     * it into a persistent CUDA-registerable D3D11 texture. This remains a
     * GPU->GPU operation and avoids both staging Map and the later CUDA
     * host-to-device upload. Failure is non-fatal: capture falls back to the
     * staging/CPU path below. */
    D3D11_TEXTURE2D_DESC gpu_texture_desc = texture_desc;
    gpu_texture_desc.Usage = D3D11_USAGE_DEFAULT;
    gpu_texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    gpu_texture_desc.CPUAccessFlags = 0U;
    (void)ID3D11Device_CreateTexture2D(
        g_dup_device, &gpu_texture_desc, NULL, &g_dup_gpu_texture
    );
    g_dup_monitor_id = monitor_id;
    g_dup_width = width;
    g_dup_height = height;
    return GRD_OK;
}

static void update_duplication_cursor(void)
{
    if (g_duplication == NULL) {
        return;
    }
    UINT required = 0U;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info;
    memset(&shape_info, 0, sizeof(shape_info));
    HRESULT result = IDXGIOutputDuplication_GetFramePointerShape(
        g_duplication, 0U, NULL, &required, &shape_info
    );
    if (result != DXGI_ERROR_MORE_DATA || required == 0U ||
        required > sizeof(g_dup_cursor_shape.pixels) * 2U) {
        return;
    }
    uint8_t *shape = malloc(required);
    if (shape == NULL) {
        return;
    }
    result = IDXGIOutputDuplication_GetFramePointerShape(
        g_duplication, required, shape, &required, &shape_info
    );
    if (SUCCEEDED(result) &&
        shape_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR &&
        shape_info.Width <= GRD_CURSOR_MAX_WIDTH &&
        shape_info.Height <= GRD_CURSOR_MAX_HEIGHT &&
        shape_info.Pitch >= shape_info.Width * 4U &&
        (size_t)shape_info.Height * shape_info.Pitch <= required) {
        memset(&g_dup_cursor_shape, 0, sizeof(g_dup_cursor_shape));
        g_dup_cursor_shape.width = (uint16_t)shape_info.Width;
        g_dup_cursor_shape.height = (uint16_t)shape_info.Height;
        g_dup_cursor_shape.hotspot_x = (int16_t)shape_info.HotSpot.x;
        g_dup_cursor_shape.hotspot_y = (int16_t)shape_info.HotSpot.y;
        for (UINT row = 0U; row < shape_info.Height; ++row) {
            memcpy(
                g_dup_cursor_shape.pixels +
                    (size_t)row * shape_info.Width * 4U,
                shape + (size_t)row * shape_info.Pitch,
                (size_t)shape_info.Width * 4U
            );
        }
        g_dup_cursor_shape_valid = true;
    }
    free(shape);
}

grd_status grd_platform_initialize(grd_error *error)
{
    if (sodium_init() < 0) {
        set_error(error, GRD_ERROR, "Cryptographic initialization failed");
        return GRD_ERROR;
    }
    SetProcessDPIAware();
    release_duplication();
    refresh_monitors();
    return GRD_OK;
}

void grd_platform_shutdown(void)
{
    release_duplication();
}

void grd_platform_capture_reset(bool reset_device)
{
    if (reset_device) {
        release_duplication();
    } else {
        /* WAIT_TIMEOUT alone is not a device failure. Keep the adapter,
         * D3D11 device and CUDA-registerable texture alive and replace only
         * the inexpensive duplication interface on the next capture. */
        release_duplication_session();
    }
}

void grd_platform_capture_last_timing(grd_capture_timing *timing)
{
    if (timing != NULL) {
        *timing = g_capture_timing;
    }
}

grd_os grd_platform_os(void)
{
    return GRD_OS_WINDOWS;
}

grd_status grd_platform_validate_host(grd_error *error)
{
    (void)error;
    return GRD_OK;
}

size_t grd_platform_monitors(grd_monitor *monitors, size_t capacity)
{
    refresh_monitors();
    const size_t count = g_monitor_count < capacity ? g_monitor_count : capacity;
    for (size_t index = 0U; index < count; ++index) {
        MONITORINFOEXA info;
        memset(&info, 0, sizeof(info));
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoA(g_monitors[index], (MONITORINFO *)&info)) {
            continue;
        }
        grd_monitor *output = &monitors[index];
        memset(output, 0, sizeof(*output));
        output->id = (uint32_t)index;
        (void)snprintf(output->name, sizeof(output->name), "%s", info.szDevice);
        output->x = info.rcMonitor.left;
        output->y = info.rcMonitor.top;
        output->width = (uint32_t)(info.rcMonitor.right - info.rcMonitor.left);
        output->height = (uint32_t)(info.rcMonitor.bottom - info.rcMonitor.top);
        output->scale = 1.0F;
        output->primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0U;
    }
    return count;
}

grd_status grd_platform_capture(
    uint32_t monitor_id,
    bool prefer_gpu_resident,
    grd_frame *frame,
    grd_error *error
)
{
    if (frame == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    memset(&g_capture_timing, 0, sizeof(g_capture_timing));
    refresh_monitors();
    if (monitor_id >= g_monitor_count) {
        return GRD_INVALID_ARGUMENT;
    }
    const grd_status duplication_status = ensure_duplication(monitor_id, error);
    if (duplication_status == GRD_OK) {
        DXGI_OUTDUPL_FRAME_INFO frame_info;
        memset(&frame_info, 0, sizeof(frame_info));
        IDXGIResource *resource = NULL;
        const uint64_t acquire_started = grd_now_micros();
        g_capture_timing.acquire_attempted = true;
        HRESULT result = IDXGIOutputDuplication_AcquireNextFrame(
            g_duplication, 1U, &frame_info, &resource
        );
        g_capture_timing.acquire_wait_micros =
            grd_now_micros() - acquire_started;
        g_capture_timing.driver_stalled =
            g_capture_timing.acquire_wait_micros >= 250000ULL;
        if (result == DXGI_ERROR_WAIT_TIMEOUT) {
            g_capture_timing.wait_timeout = true;
            return GRD_WOULD_BLOCK;
        }
        if (result == DXGI_ERROR_ACCESS_LOST) {
            /* Microsoft documents ACCESS_LOST as requiring a new
             * IDXGIOutputDuplication. Preserve the D3D device unless the
             * recreated session reports a changed mode on the next call. */
            g_capture_timing.reset_scope = GRD_CAPTURE_RESET_SESSION;
            release_duplication_session();
            set_error(error, GRD_IO_ERROR, "Desktop Duplication lost the display");
            return GRD_IO_ERROR;
        }
        if (FAILED(result) || resource == NULL) {
            const HRESULT acquire_error = FAILED(result) ? result : E_FAIL;
            if (resource != NULL) {
                IDXGIResource_Release(resource);
            }
            if (SUCCEEDED(result)) {
                (void)IDXGIOutputDuplication_ReleaseFrame(g_duplication);
            }
            /* Some games leave the duplication object permanently invalid
             * without returning DXGI_ERROR_ACCESS_LOST. Keeping that object
             * caused one GRD_IO_ERROR per second and a black stream forever.
             * Recreate the complete session on the next capture attempt. */
            g_capture_timing.reset_scope = GRD_CAPTURE_RESET_DEVICE;
            release_duplication();
            if (error != NULL) {
                error->code = GRD_IO_ERROR;
                (void)snprintf(
                    error->message,
                    sizeof(error->message),
                    "Desktop Duplication acquisition failed (HRESULT 0x%08lX)",
                    (unsigned long)(uint32_t)acquire_error
                );
            }
            return GRD_IO_ERROR;
        }
        g_capture_timing.accumulated_frames = frame_info.AccumulatedFrames;
        ID3D11Texture2D *source = NULL;
        result = IDXGIResource_QueryInterface(
            resource, &IID_ID3D11Texture2D, (void **)&source
        );
        if (SUCCEEDED(result) && source == NULL) {
            result = E_NOINTERFACE;
        }
        bool gpu_copy_ready = false;
        if (SUCCEEDED(result) && source != NULL) {
            if (prefer_gpu_resident && g_dup_gpu_texture != NULL) {
                ID3D11DeviceContext_CopyResource(
                    g_dup_context,
                    (ID3D11Resource *)g_dup_gpu_texture,
                    (ID3D11Resource *)source
                );
                gpu_copy_ready = true;
            } else {
                ID3D11DeviceContext_CopyResource(
                    g_dup_context,
                    (ID3D11Resource *)g_dup_staging,
                    (ID3D11Resource *)source
                );
            }
            ID3D11Texture2D_Release(source);
        }
        IDXGIResource_Release(resource);
        update_duplication_cursor();
        (void)IDXGIOutputDuplication_ReleaseFrame(g_duplication);
        if (FAILED(result)) {
            const HRESULT texture_error = result;
            g_capture_timing.reset_scope = GRD_CAPTURE_RESET_DEVICE;
            release_duplication();
            if (error != NULL) {
                error->code = GRD_IO_ERROR;
                (void)snprintf(
                    error->message,
                    sizeof(error->message),
                    "Invalid Desktop Duplication texture (HRESULT 0x%08lX)",
                    (unsigned long)(uint32_t)texture_error
                );
            }
            return GRD_IO_ERROR;
        }
        if (gpu_copy_ready) {
            memset(frame, 0, sizeof(*frame));
            ID3D11Texture2D_AddRef(g_dup_gpu_texture);
            frame->width = g_dup_width;
            frame->height = g_dup_height;
            frame->format = GRD_PIXEL_D3D11_BGRA;
            frame->timestamp_micros = grd_now_micros();
            frame->owner = g_dup_gpu_texture;
            frame->release_fn = release_d3d11_capture_texture;
            return GRD_OK;
        }
        D3D11_MAPPED_SUBRESOURCE mapped;
        memset(&mapped, 0, sizeof(mapped));
        result = ID3D11DeviceContext_Map(
            g_dup_context, (ID3D11Resource *)g_dup_staging, 0U,
            D3D11_MAP_READ, 0U, &mapped
        );
        if (FAILED(result) || mapped.pData == NULL) {
            const HRESULT map_error = FAILED(result) ? result : E_FAIL;
            if (SUCCEEDED(result)) {
                ID3D11DeviceContext_Unmap(
                    g_dup_context, (ID3D11Resource *)g_dup_staging, 0U
                );
            }
            g_capture_timing.reset_scope = GRD_CAPTURE_RESET_DEVICE;
            release_duplication();
            if (error != NULL) {
                error->code = GRD_IO_ERROR;
                (void)snprintf(
                    error->message,
                    sizeof(error->message),
                    "Failed to read Desktop Duplication texture (HRESULT 0x%08lX)",
                    (unsigned long)(uint32_t)map_error
                );
            }
            return GRD_IO_ERROR;
        }
        const size_t stride = (size_t)g_dup_width * 4U;
        const size_t size = stride * (size_t)g_dup_height;
        if (g_capture_pixels_capacity < size) {
            uint8_t *resized = realloc(g_capture_pixels, size);
            if (resized == NULL) {
                ID3D11DeviceContext_Unmap(
                    g_dup_context, (ID3D11Resource *)g_dup_staging, 0U
                );
                return GRD_OUT_OF_MEMORY;
            }
            g_capture_pixels = resized;
            g_capture_pixels_capacity = size;
        }
        if (g_capture_pixels != NULL) {
            for (uint32_t row = 0U; row < g_dup_height; ++row) {
                memcpy(
                    g_capture_pixels + (size_t)row * stride,
                    (const uint8_t *)mapped.pData +
                        (size_t)row * mapped.RowPitch,
                    stride
                );
            }
        }
        ID3D11DeviceContext_Unmap(
            g_dup_context, (ID3D11Resource *)g_dup_staging, 0U
        );
        if (g_capture_pixels == NULL) {
            return GRD_OUT_OF_MEMORY;
        }
        memset(frame, 0, sizeof(*frame));
        frame->data = g_capture_pixels;
        frame->size = size;
        frame->width = g_dup_width;
        frame->height = g_dup_height;
        frame->stride = (uint32_t)stride;
        frame->format = GRD_PIXEL_BGRA8;
        frame->timestamp_micros = grd_now_micros();
        return GRD_OK;
    }

    /* A few remote-desktop sessions expose no duplicable output (for example
     * RDP mirrors). Keep the old GDI path as an explicit compatibility
     * fallback instead of making the host unusable there. */
    MONITORINFO info;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoA(g_monitors[monitor_id], &info)) {
        return GRD_IO_ERROR;
    }
    const int width = info.rcMonitor.right - info.rcMonitor.left;
    const int height = info.rcMonitor.bottom - info.rcMonitor.top;
    HDC screen = GetDC(NULL);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    const BOOL captured = BitBlt(
        memory, 0, 0, width, height, screen,
        info.rcMonitor.left, info.rcMonitor.top, SRCCOPY | CAPTUREBLT
    );
    if (!captured) {
        SelectObject(memory, previous);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(NULL, screen);
        set_error(error, GRD_IO_ERROR, "BitBlt failed");
        return GRD_IO_ERROR;
    }
    BITMAPINFO bitmap_info;
    memset(&bitmap_info, 0, sizeof(bitmap_info));
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    const size_t size = (size_t)width * (size_t)height * 4U;
    bool buffer_ready = true;
    if (g_capture_pixels_capacity < size) {
        uint8_t *resized = realloc(g_capture_pixels, size);
        if (resized == NULL) {
            buffer_ready = false;
        } else {
            g_capture_pixels = resized;
            g_capture_pixels_capacity = size;
        }
    }
    if (!buffer_ready || g_capture_pixels == NULL ||
        GetDIBits(
            memory, bitmap, 0U, (UINT)height, g_capture_pixels,
            &bitmap_info, DIB_RGB_COLORS
        ) == 0) {
        buffer_ready = false;
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(NULL, screen);
    if (!buffer_ready || g_capture_pixels == NULL) {
        return GRD_IO_ERROR;
    }
    memset(frame, 0, sizeof(*frame));
    frame->data = g_capture_pixels;
    frame->size = size;
    frame->width = (uint32_t)width;
    frame->height = (uint32_t)height;
    frame->stride = (uint32_t)width * 4U;
    frame->format = GRD_PIXEL_BGRA8;
    frame->timestamp_micros = grd_now_micros();
    return GRD_OK;
}

void grd_platform_frame_release(grd_frame *frame)
{
    if (frame != NULL) {
        if (frame->release_fn != NULL) {
            frame->release_fn(frame->owner);
        } else if (frame->data != g_capture_pixels) {
            free(frame->data);
        }
        memset(frame, 0, sizeof(*frame));
    }
}

static WORD windows_scan_code(uint32_t usage, bool *extended)
{
    *extended = false;
    switch (usage) {
    case 4U: return 0x1EU;
    case 5U: return 0x30U;
    case 6U: return 0x2EU;
    case 7U: return 0x20U;
    case 8U: return 0x12U;
    case 9U: return 0x21U;
    case 10U: return 0x22U;
    case 11U: return 0x23U;
    case 12U: return 0x17U;
    case 13U: return 0x24U;
    case 14U: return 0x25U;
    case 15U: return 0x26U;
    case 16U: return 0x32U;
    case 17U: return 0x31U;
    case 18U: return 0x18U;
    case 19U: return 0x19U;
    case 20U: return 0x10U;
    case 21U: return 0x13U;
    case 22U: return 0x1FU;
    case 23U: return 0x14U;
    case 24U: return 0x16U;
    case 25U: return 0x2FU;
    case 26U: return 0x11U;
    case 27U: return 0x2DU;
    case 28U: return 0x15U;
    case 29U: return 0x2CU;
    case 30U: return 0x02U;
    case 31U: return 0x03U;
    case 32U: return 0x04U;
    case 33U: return 0x05U;
    case 34U: return 0x06U;
    case 35U: return 0x07U;
    case 36U: return 0x08U;
    case 37U: return 0x09U;
    case 38U: return 0x0AU;
    case 39U: return 0x0BU;
    case 40U: return 0x1CU;
    case 41U: return 0x01U;
    case 42U: return 0x0EU;
    case 43U: return 0x0FU;
    case 44U: return 0x39U;
    case 45U: return 0x0CU;
    case 46U: return 0x0DU;
    case 47U: return 0x1AU;
    case 48U: return 0x1BU;
    case 49U: return 0x2BU;
    case 51U: return 0x27U;
    case 52U: return 0x28U;
    case 53U: return 0x29U;
    case 54U: return 0x33U;
    case 55U: return 0x34U;
    case 56U: return 0x35U;
    case 57U: return 0x3AU;
    case 58U: return 0x3BU;
    case 59U: return 0x3CU;
    case 60U: return 0x3DU;
    case 61U: return 0x3EU;
    case 62U: return 0x3FU;
    case 63U: return 0x40U;
    case 64U: return 0x41U;
    case 65U: return 0x42U;
    case 66U: return 0x43U;
    case 67U: return 0x44U;
    case 68U: return 0x57U;
    case 69U: return 0x58U;
    case GRD_KEY_LEFT_CTRL: return 0x1DU;
    case GRD_KEY_LEFT_SHIFT: return 0x2AU;
    case GRD_KEY_LEFT_ALT: return 0x38U;
    case GRD_KEY_RIGHT_SHIFT: return 0x36U;
    case 73U: *extended = true; return 0x52U;
    case 74U: *extended = true; return 0x47U;
    case 75U: *extended = true; return 0x49U;
    case 76U: *extended = true; return 0x53U;
    case 77U: *extended = true; return 0x4FU;
    case 78U: *extended = true; return 0x51U;
    case 79U: *extended = true; return 0x4DU;
    case 80U: *extended = true; return 0x4BU;
    case 81U: *extended = true; return 0x50U;
    case 82U: *extended = true; return 0x48U;
    case GRD_KEY_RIGHT_CTRL: *extended = true; return 0x1DU;
    case GRD_KEY_RIGHT_ALT: *extended = true; return 0x38U;
    case GRD_KEY_LEFT_GUI: *extended = true; return 0x5BU;
    case GRD_KEY_RIGHT_GUI: *extended = true; return 0x5CU;
    default: return 0U;
    }
}

grd_status grd_platform_inject(
    const grd_monitor *monitor,
    const grd_input_event *event,
    grd_error *error
)
{
    if (monitor == NULL || event == NULL ||
        grd_protocol_validate_input(event) != GRD_OK) {
        set_error(error, GRD_INVALID_ARGUMENT, "Invalid Windows input event");
        return GRD_INVALID_ARGUMENT;
    }
    INPUT inputs[64];
    memset(inputs, 0, sizeof(inputs));
    UINT count = 1U;
    INPUT *input = &inputs[0];
    if (event->kind == GRD_INPUT_POINTER_MOVE) {
        const int virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        const int x = monitor->x + (int)(event->x * (float)monitor->width);
        const int y = monitor->y + (int)(event->y * (float)monitor->height);
        input->type = INPUT_MOUSE;
        input->mi.dx = (LONG)(((x - virtual_x) * 65535) / (virtual_width - 1));
        input->mi.dy = (LONG)(((y - virtual_y) * 65535) / (virtual_height - 1));
        input->mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                            MOUSEEVENTF_VIRTUALDESK;
    } else if (event->kind == GRD_INPUT_POINTER_RELATIVE) {
        /* Preserve one physical client sample as one Windows sample. The old
         * threshold splitter submitted up to 64 INPUT records in one call:
         * distance stayed correct, but their timing collapsed into a single
         * host tick and camera motion felt heavy/jumpy. */
        input->type = INPUT_MOUSE;
        input->mi.dx = (LONG)event->delta_x;
        input->mi.dy = (LONG)event->delta_y;
        input->mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
    } else if (event->kind == GRD_INPUT_POINTER_BUTTON) {
        input->type = INPUT_MOUSE;
        if (event->code == 1U) {
            input->mi.dwFlags = event->pressed
                                    ? MOUSEEVENTF_RIGHTDOWN
                                    : MOUSEEVENTF_RIGHTUP;
        } else if (event->code == 2U) {
            input->mi.dwFlags = event->pressed
                                    ? MOUSEEVENTF_MIDDLEDOWN
                                    : MOUSEEVENTF_MIDDLEUP;
        } else {
            input->mi.dwFlags = event->pressed
                                    ? MOUSEEVENTF_LEFTDOWN
                                    : MOUSEEVENTF_LEFTUP;
        }
    } else if (event->kind == GRD_INPUT_SCROLL) {
        input->type = INPUT_MOUSE;
        input->mi.mouseData = (DWORD)event->delta_y;
        input->mi.dwFlags = MOUSEEVENTF_WHEEL;
    } else if (event->kind == GRD_INPUT_KEY) {
        bool extended = false;
        const WORD scan_code = windows_scan_code(event->code, &extended);
        if (scan_code == 0U) {
            set_error(
                error,
                GRD_NOT_SUPPORTED,
                "Key is unsupported by the Windows mapping"
            );
            return GRD_NOT_SUPPORTED;
        }
        input->type = INPUT_KEYBOARD;
        input->ki.wScan = scan_code;
        input->ki.dwFlags = KEYEVENTF_SCANCODE |
                            (extended ? KEYEVENTF_EXTENDEDKEY : 0U) |
                            (event->pressed ? 0U : KEYEVENTF_KEYUP);
    } else if (event->kind == GRD_INPUT_TEXT) {
        const int utf16_length = MultiByteToWideChar(
            CP_UTF8, 0, event->text, (int)event->text_length, NULL, 0
        );
        if (utf16_length <= 0 || utf16_length > 32) {
            return GRD_INVALID_ARGUMENT;
        }
        WCHAR utf16[32];
        (void)MultiByteToWideChar(
            CP_UTF8, 0, event->text, (int)event->text_length,
            utf16, utf16_length
        );
        count = (UINT)utf16_length * 2U;
        for (int index = 0; index < utf16_length; ++index) {
            inputs[index * 2].type = INPUT_KEYBOARD;
            inputs[index * 2].ki.wScan = utf16[index];
            inputs[index * 2].ki.dwFlags = KEYEVENTF_UNICODE;
            inputs[index * 2 + 1] = inputs[index * 2];
            inputs[index * 2 + 1].ki.dwFlags |= KEYEVENTF_KEYUP;
        }
    } else {
        set_error(error, GRD_INVALID_ARGUMENT, "Invalid Windows input type");
        return GRD_INVALID_ARGUMENT;
    }
    const UINT sent = SendInput(count, inputs, sizeof(INPUT));
    if (sent == count) {
        return GRD_OK;
    }
    /* SendInput returns zero when UIPI blocks injection (for example when
     * the game is elevated and GRD is not). Do not hide this as network lag. */
    const DWORD last_error = GetLastError();
    if (error != NULL) {
        error->code = GRD_IO_ERROR;
        (void)snprintf(
            error->message,
            sizeof(error->message),
            "SendInput rejected (Win32 %lu): run GRD and the application with the same privileges",
            (unsigned long)last_error
        );
    }
    return GRD_IO_ERROR;
}

grd_status grd_platform_cursor_state(
    const grd_monitor *monitor,
    grd_cursor_state *state,
    grd_cursor_shape *shape,
    grd_error *error
)
{
    (void)error;
    if (monitor == NULL || state == NULL || shape == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    CURSORINFO info = {.cbSize = sizeof(info)};
    POINT point;
    if (!GetCursorInfo(&info) || !GetCursorPos(&point)) {
        return GRD_IO_ERROR;
    }
    state->visible = (info.flags & CURSOR_SHOWING) != 0U &&
                     point.x >= monitor->x &&
                     point.y >= monitor->y &&
                     point.x < monitor->x + (int)monitor->width &&
                     point.y < monitor->y + (int)monitor->height;
    state->x = ((float)point.x - (float)monitor->x) / (float)monitor->width;
    state->y = ((float)point.y - (float)monitor->y) / (float)monitor->height;
    memset(state->reserved, 0, sizeof(state->reserved));
    memset(shape, 0, sizeof(*shape));
    if (g_dup_cursor_shape_valid) {
        *shape = g_dup_cursor_shape;
    }
    return GRD_OK;
}

bool grd_platform_screen_permission(void)
{
    return true;
}

bool grd_platform_input_permission(void)
{
    return true;
}

void grd_platform_request_permissions(void)
{
}

static void audio_release_interfaces(void)
{
    if (g_audio_capture != NULL) {
        IAudioCaptureClient_Release(g_audio_capture);
        g_audio_capture = NULL;
    }
    if (g_audio_client != NULL) {
        IAudioClient_Release(g_audio_client);
        g_audio_client = NULL;
    }
    if (g_audio_device != NULL) {
        IMMDevice_Release(g_audio_device);
        g_audio_device = NULL;
    }
    if (g_audio_enumerator != NULL) {
        IMMDeviceEnumerator_Release(g_audio_enumerator);
        g_audio_enumerator = NULL;
    }
    if (g_audio_com_initialized) {
        CoUninitialize();
        g_audio_com_initialized = false;
    }
}

grd_status grd_platform_audio_start(grd_error *error)
{
    if (g_audio_client != NULL) {
        return GRD_OK;
    }
    HRESULT result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(result)) {
        g_audio_com_initialized = true;
    } else if (result != RPC_E_CHANGED_MODE) {
        set_error(error, GRD_ERROR, "Audio COM initialization failed");
        return GRD_ERROR;
    }
    result = CoCreateInstance(
        &CLSID_MMDeviceEnumerator,
        NULL,
        CLSCTX_ALL,
        &IID_IMMDeviceEnumerator,
        (void **)&g_audio_enumerator
    );
    if (SUCCEEDED(result)) {
        result = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
            g_audio_enumerator, eRender, eConsole, &g_audio_device
        );
    }
    if (SUCCEEDED(result)) {
        result = IMMDevice_Activate(
            g_audio_device,
            &IID_IAudioClient,
            CLSCTX_ALL,
            NULL,
            (void **)&g_audio_client
        );
    }
    WAVEFORMATEXTENSIBLE format;
    memset(&format, 0, sizeof(format));
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = (WORD)GRD_AUDIO_CHANNELS;
    format.Format.nSamplesPerSec = GRD_AUDIO_SAMPLE_RATE;
    format.Format.wBitsPerSample = 32U;
    format.Format.nBlockAlign =
        (WORD)(GRD_AUDIO_CHANNELS * sizeof(float));
    format.Format.nAvgBytesPerSec =
        format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = 22U;
    format.Samples.wValidBitsPerSample = 32U;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = g_float_audio_subtype;
    if (SUCCEEDED(result)) {
        result = IAudioClient_Initialize(
            g_audio_client,
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK |
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            10000000,
            0,
            (WAVEFORMATEX *)&format,
            NULL
        );
    }
    if (SUCCEEDED(result)) {
        result = IAudioClient_GetService(
            g_audio_client,
            &IID_IAudioCaptureClient,
            (void **)&g_audio_capture
        );
    }
    if (SUCCEEDED(result)) {
        result = IAudioClient_Start(g_audio_client);
    }
    if (FAILED(result)) {
        audio_release_interfaces();
        set_error(error, GRD_IO_ERROR, "WASAPI loopback is unavailable");
        return GRD_IO_ERROR;
    }
    g_audio_read_index = 0U;
    g_audio_write_index = 0U;
    g_audio_frame_count = 0U;
    return GRD_OK;
}

grd_status grd_platform_audio_read(
    float *stereo_samples,
    size_t frame_capacity,
    size_t *frames_read,
    uint64_t *timestamp_micros,
    grd_error *error
)
{
    if (stereo_samples == NULL || frames_read == NULL ||
        timestamp_micros == NULL || g_audio_capture == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    while (g_audio_frame_count < frame_capacity) {
        UINT32 packet_frames = 0U;
        HRESULT result = IAudioCaptureClient_GetNextPacketSize(
            g_audio_capture, &packet_frames
        );
        if (FAILED(result)) {
            set_error(error, GRD_IO_ERROR, "WASAPI read failed");
            return GRD_IO_ERROR;
        }
        if (packet_frames == 0U) {
            *frames_read = 0U;
            return GRD_WOULD_BLOCK;
        }
        BYTE *data = NULL;
        DWORD flags = 0U;
        UINT64 device_position = 0U;
        UINT64 performance_position = 0U;
        result = IAudioCaptureClient_GetBuffer(
            g_audio_capture,
            &data,
            &packet_frames,
            &flags,
            &device_position,
            &performance_position
        );
        (void)device_position;
        (void)performance_position;
        if (FAILED(result)) {
            return GRD_IO_ERROR;
        }
        const float *input = (const float *)data;
        for (UINT32 frame = 0U; frame < packet_frames; ++frame) {
            if (g_audio_frame_count == AUDIO_RING_FRAMES) {
                g_audio_read_index =
                    (g_audio_read_index + 1U) % AUDIO_RING_FRAMES;
                --g_audio_frame_count;
            }
            const size_t destination =
                g_audio_write_index * GRD_AUDIO_CHANNELS;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0U) {
                g_audio_ring[destination] = 0.0F;
                g_audio_ring[destination + 1U] = 0.0F;
            } else {
                g_audio_ring[destination] = input[(size_t)frame * 2U];
                g_audio_ring[destination + 1U] =
                    input[(size_t)frame * 2U + 1U];
            }
            g_audio_write_index =
                (g_audio_write_index + 1U) % AUDIO_RING_FRAMES;
            ++g_audio_frame_count;
        }
        (void)IAudioCaptureClient_ReleaseBuffer(
            g_audio_capture, packet_frames
        );
    }
    for (size_t frame = 0U; frame < frame_capacity; ++frame) {
        const size_t source = g_audio_read_index * GRD_AUDIO_CHANNELS;
        stereo_samples[frame * 2U] = g_audio_ring[source];
        stereo_samples[frame * 2U + 1U] = g_audio_ring[source + 1U];
        g_audio_read_index =
            (g_audio_read_index + 1U) % AUDIO_RING_FRAMES;
    }
    g_audio_frame_count -= frame_capacity;
    *frames_read = frame_capacity;
    *timestamp_micros = grd_now_micros();
    return GRD_OK;
}

void grd_platform_audio_stop(void)
{
    if (g_audio_client != NULL) {
        (void)IAudioClient_Stop(g_audio_client);
    }
    audio_release_interfaces();
    g_audio_frame_count = 0U;
}
