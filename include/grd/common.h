#ifndef GRD_COMMON_H
#define GRD_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRD_DEFAULT_PORT 47990U
#define GRD_PROTOCOL_VERSION 3U
#define GRD_MAX_DEVICE_NAME 96U
#define GRD_MAX_ADDRESS 128U
#define GRD_MAX_CLIPBOARD (1024U * 1024U)
#define GRD_MAX_OBSERVERS 3U
#define GRD_MAX_CLIENTS (GRD_MAX_OBSERVERS + 1U)
/* Each logical client may use one control and one media TCP connection. */
#define GRD_MAX_CONNECTIONS (GRD_MAX_CLIENTS * 2U)
/* Media buffers handed to the decoder carry this much zeroed tail space so
 * they can be adopted without copying (FFmpeg requires
 * FF_INPUT_BUFFER_PADDING_SIZE bytes of padding after the encoded data). */
#define GRD_MEDIA_BUFFER_PADDING 64U

typedef void *(*grd_buffer_clone_fn)(const void *opaque);
typedef void (*grd_buffer_release_fn)(void *opaque);

/* Ownership for payloads crossing pipeline stages without copying. clone
 * returns a new independent reference for an additional consumer and must be
 * non-NULL when a buffer may be broadcast to several clients; release frees
 * a reference. */
typedef struct grd_owned_buffer {
    const void *opaque;
    grd_buffer_clone_fn clone;
    grd_buffer_release_fn release;
} grd_owned_buffer;

typedef enum grd_status {
    GRD_OK = 0,
    GRD_ERROR = -1,
    GRD_INVALID_ARGUMENT = -2,
    GRD_OUT_OF_MEMORY = -3,
    GRD_IO_ERROR = -4,
    GRD_NOT_SUPPORTED = -5,
    GRD_AUTH_FAILED = -6,
    GRD_BUSY = -7,
    GRD_PROTOCOL_ERROR = -8,
    GRD_WOULD_BLOCK = -9
} grd_status;

typedef enum grd_role {
    GRD_ROLE_OBSERVER = 0,
    GRD_ROLE_CONTROLLER = 1
} grd_role;

typedef enum grd_os {
    GRD_OS_UNKNOWN = 0,
    GRD_OS_MACOS = 1,
    GRD_OS_WINDOWS = 2,
    GRD_OS_LINUX_X11 = 3
} grd_os;

/* How much of the resolution work the client accepts. The host maps these
 * values to its existing quality ladder and sends a smaller encoded frame;
 * the client GPU then scales that frame to the local display. Keeping this
 * in common.h gives configuration and the wire protocol one canonical ABI. */
typedef enum grd_client_upscale_mode {
    GRD_CLIENT_UPSCALE_NATIVE = 0,
    GRD_CLIENT_UPSCALE_BALANCED = 1,
    GRD_CLIENT_UPSCALE_PERFORMANCE = 2
} grd_client_upscale_mode;

typedef struct grd_error {
    grd_status code;
    char message[256];
} grd_error;

uint64_t grd_now_micros(void);
void grd_secure_zero(void *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif
