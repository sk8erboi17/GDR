#ifndef GRD_PLATFORM_H
#define GRD_PLATFORM_H

#include "grd/common.h"
#include "grd/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRD_MAX_MONITORS 16U

typedef enum grd_pixel_format {
    GRD_PIXEL_RGBA8 = 0,
    GRD_PIXEL_BGRA8 = 1,
    /* Windows/NVIDIA only: the frame is an ID3D11Texture2D produced by the
     * CUDA↔D3D11 uploader (owner holds the texture, data is NULL). */
    GRD_PIXEL_D3D11_RGBA = 2,
    /* Bi-planar YUV (VideoToolbox native hardware output): Y plane in
     * data/stride, interleaved U+V plane in data_uv/stride_uv. Rendered
     * by the Metal renderer's NV12/P010 shader (zero-copy IOSurface). */
    GRD_PIXEL_NV12 = 3,
    GRD_PIXEL_P010 = 4,
    /* Windows host capture: owner is an ID3D11Texture2D in BGRA8 format.
     * The CUDA/NVENC encoder consumes it without a GPU->CPU readback. */
    GRD_PIXEL_D3D11_BGRA = 5
} grd_pixel_format;

/* Optional ownership hook for native hardware frames. */
typedef void (*grd_frame_release_fn)(void *owner);

typedef struct grd_monitor {
    uint32_t id;
    char name[128];
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    float scale;
    bool primary;
} grd_monitor;

typedef struct grd_frame {
    uint8_t *data;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    /* Chroma plane for NV12/P010 frames; NULL for packed formats. */
    uint8_t *data_uv;
    uint32_t stride_uv;
    grd_pixel_format format;
    uint64_t timestamp_micros;
    void *owner;
    grd_frame_release_fn release_fn;
} grd_frame;

typedef enum grd_capture_reset_scope {
    GRD_CAPTURE_RESET_NONE = 0,
    GRD_CAPTURE_RESET_SESSION = 1,
    GRD_CAPTURE_RESET_DEVICE = 2
} grd_capture_reset_scope;

typedef struct grd_capture_timing {
    uint64_t acquire_wait_micros;
    uint32_t accumulated_frames;
    grd_capture_reset_scope reset_scope;
    bool acquire_attempted;
    bool wait_timeout;
    /* AcquireNextFrame was asked to wait 1 ms but remained inside the driver
     * for a grossly longer interval. This is positive watchdog evidence;
     * WAIT_TIMEOUT by itself is normal on an unchanged desktop. */
    bool driver_stalled;
} grd_capture_timing;

grd_status grd_platform_initialize(grd_error *error);
void grd_platform_shutdown(void);
grd_os grd_platform_os(void);
grd_status grd_platform_validate_host(grd_error *error);
size_t grd_platform_monitors(grd_monitor *monitors, size_t capacity);
grd_status grd_platform_capture(
    uint32_t monitor_id,
    bool prefer_gpu_resident,
    grd_frame *frame,
    grd_error *error
);
/* Drops the current Desktop Duplication session. A soft reset preserves the
 * D3D11 device/textures; a hard reset rebuilds the complete capture stack.
 * The following capture call creates the missing resources. */
#if defined(_WIN32)
void grd_platform_capture_reset(bool reset_device);
/* Per-call Desktop Duplication timing, consumed immediately by the single
 * Windows stream thread for its five-second pipeline summary. */
void grd_platform_capture_last_timing(grd_capture_timing *timing);
#endif
void grd_platform_frame_release(grd_frame *frame);
grd_status grd_platform_inject(
    const grd_monitor *monitor,
    const grd_input_event *event,
    grd_error *error
);
grd_status grd_platform_cursor_state(
    const grd_monitor *monitor,
    grd_cursor_state *state,
    grd_cursor_shape *shape,
    grd_error *error
);
char *grd_platform_clipboard_read(void);
grd_status grd_platform_clipboard_write(const char *text, grd_error *error);
bool grd_platform_screen_permission(void);
bool grd_platform_input_permission(void);
void grd_platform_request_permissions(void);

#ifdef __cplusplus
}
#endif

#endif
