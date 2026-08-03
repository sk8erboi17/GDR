#include "grd/platform.h"
#include "grd/audio.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/XShm.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <sodium.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

static Display *g_display;
static Window g_root;
static pa_simple *g_audio_capture;
static grd_monitor g_monitors[GRD_MAX_MONITORS];
static size_t g_monitor_count;
static uint64_t g_monitor_refresh_micros;
static int g_randr_event_base;
static XImage *g_shm_image;
static XShmSegmentInfo g_shm_info;
static uint32_t g_shm_monitor;
static uint32_t g_shm_width;
static uint32_t g_shm_height;
static uint8_t *g_capture_pixels;
static size_t g_capture_pixels_capacity;

static void destroy_shm_image(void);

static void set_error(grd_error *error, grd_status code, const char *message)
{
    if (error != NULL) {
        error->code = code;
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
}

static bool contains_case_insensitive(const char *text, const char *needle)
{
    if (text == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    for (; *text != '\0'; ++text) {
        const char *left = text;
        const char *right = needle;
        while (*left != '\0' && *right != '\0') {
            char a = *left;
            char b = *right;
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
            ++left;
            ++right;
        }
        if (*right == '\0') {
            return true;
        }
    }
    return false;
}

grd_status grd_platform_initialize(grd_error *error)
{
    if (sodium_init() < 0) {
        return GRD_ERROR;
    }
    (void)XInitThreads();
    g_display = XOpenDisplay(NULL);
    if (g_display == NULL) {
        set_error(error, GRD_IO_ERROR, "Unable to open the X11 display");
        return GRD_IO_ERROR;
    }
    g_root = DefaultRootWindow(g_display);
    int randr_error_base = 0;
    if (XRRQueryExtension(g_display, &g_randr_event_base, &randr_error_base)) {
        XRRSelectInput(
            g_display,
            g_root,
            RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                RROutputChangeNotifyMask | RROutputPropertyNotifyMask
        );
    }
    g_shm_info.shmid = -1;
    g_shm_info.shmaddr = (char *)-1;
    return GRD_OK;
}

void grd_platform_shutdown(void)
{
    destroy_shm_image();
    free(g_capture_pixels);
    g_capture_pixels = NULL;
    g_capture_pixels_capacity = 0U;
    if (g_display != NULL) {
        XCloseDisplay(g_display);
        g_display = NULL;
    }
}

grd_os grd_platform_os(void)
{
    return GRD_OS_LINUX_X11;
}

grd_status grd_platform_validate_host(grd_error *error)
{
    const char *session = getenv("XDG_SESSION_TYPE");
    if (session == NULL || strcmp(session, "x11") != 0) {
        set_error(error, GRD_NOT_SUPPORTED, "The GRD host requires XFCE with an X11 session");
        return GRD_NOT_SUPPORTED;
    }
    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    if (desktop == NULL) {
        desktop = getenv("DESKTOP_SESSION");
    }
    if (!contains_case_insensitive(desktop, "xfce")) {
        set_error(
            error,
            GRD_NOT_SUPPORTED,
            "On Ubuntu, the GRD host is supported only on XFCE"
        );
        return GRD_NOT_SUPPORTED;
    }
    if (g_display == NULL) {
        set_error(error, GRD_IO_ERROR, "X11 display is unavailable");
        return GRD_IO_ERROR;
    }
    return GRD_OK;
}

static size_t refresh_monitors(void)
{
    if (g_display == NULL) {
        return 0U;
    }
    int count = 0;
    XRRMonitorInfo *info = XRRGetMonitors(g_display, g_root, True, &count);
    if (info == NULL) {
        return 0U;
    }
    const size_t output_count =
        (size_t)count < GRD_MAX_MONITORS ? (size_t)count : GRD_MAX_MONITORS;
    for (size_t index = 0U; index < output_count; ++index) {
        grd_monitor *monitor = &g_monitors[index];
        memset(monitor, 0, sizeof(*monitor));
        monitor->id = (uint32_t)index;
        char *name = XGetAtomName(g_display, info[index].name);
        (void)snprintf(
            monitor->name,
            sizeof(monitor->name),
            "%s",
            name != NULL ? name : "X11 Display"
        );
        if (name != NULL) {
            XFree(name);
        }
        monitor->x = info[index].x;
        monitor->y = info[index].y;
        monitor->width = (uint32_t)info[index].width;
        monitor->height = (uint32_t)info[index].height;
        monitor->scale = 1.0F;
        monitor->primary = info[index].primary;
    }
    XRRFreeMonitors(info);
    g_monitor_count = output_count;
    g_monitor_refresh_micros = grd_now_micros();
    return output_count;
}

static void consume_randr_events(void)
{
    if (g_display == NULL || g_randr_event_base == 0) {
        return;
    }
    bool changed = false;
    while (XPending(g_display) > 0) {
        XEvent event;
        XNextEvent(g_display, &event);
        if (event.type == g_randr_event_base + RRScreenChangeNotify ||
            event.type == g_randr_event_base + RRNotify) {
            changed = true;
        }
    }
    if (changed) {
        g_monitor_refresh_micros = 0U;
    }
}

size_t grd_platform_monitors(grd_monitor *monitors, size_t capacity)
{
    if (g_display == NULL || monitors == NULL || capacity == 0U) {
        return 0U;
    }
    consume_randr_events();
    const uint64_t now = grd_now_micros();
    if (g_monitor_count == 0U ||
        now - g_monitor_refresh_micros >= 5000000ULL) {
        (void)refresh_monitors();
    }
    const size_t output_count =
        g_monitor_count < capacity ? g_monitor_count : capacity;
    memcpy(monitors, g_monitors, output_count * sizeof(monitors[0]));
    return output_count;
}

static void destroy_shm_image(void)
{
    if (g_display != NULL && g_shm_image != NULL) {
        XShmDetach(g_display, &g_shm_info);
        g_shm_image->data = NULL;
        XDestroyImage(g_shm_image);
        g_shm_image = NULL;
    }
    if (g_shm_info.shmaddr != (char *)-1) {
        shmdt(g_shm_info.shmaddr);
    }
    if (g_shm_info.shmid >= 0) {
        (void)shmctl(g_shm_info.shmid, IPC_RMID, NULL);
    }
    memset(&g_shm_info, 0, sizeof(g_shm_info));
    g_shm_info.shmid = -1;
    g_shm_info.shmaddr = (char *)-1;
    g_shm_width = 0U;
    g_shm_height = 0U;
}

static bool ensure_shm_image(const grd_monitor *monitor)
{
    if (g_shm_image != NULL && g_shm_monitor == monitor->id &&
        g_shm_width == monitor->width && g_shm_height == monitor->height) {
        return true;
    }
    destroy_shm_image();
    memset(&g_shm_info, 0, sizeof(g_shm_info));
    g_shm_info.shmid = -1;
    g_shm_info.shmaddr = (char *)-1;
    g_shm_image = XShmCreateImage(
        g_display,
        DefaultVisual(g_display, DefaultScreen(g_display)),
        (unsigned)DefaultDepth(g_display, DefaultScreen(g_display)),
        ZPixmap,
        NULL,
        &g_shm_info,
        monitor->width,
        monitor->height
    );
    if (g_shm_image == NULL) {
        return false;
    }
    const size_t image_size =
        (size_t)g_shm_image->bytes_per_line * monitor->height;
    g_shm_info.shmid = shmget(IPC_PRIVATE, image_size, IPC_CREAT | 0600);
    if (g_shm_info.shmid < 0) {
        destroy_shm_image();
        return false;
    }
    g_shm_info.shmaddr = shmat(g_shm_info.shmid, NULL, 0);
    if (g_shm_info.shmaddr == (char *)-1) {
        destroy_shm_image();
        return false;
    }
    g_shm_info.readOnly = False;
    g_shm_image->data = g_shm_info.shmaddr;
    if (!XShmAttach(g_display, &g_shm_info)) {
        destroy_shm_image();
        return false;
    }
    XSync(g_display, False);
    (void)shmctl(g_shm_info.shmid, IPC_RMID, NULL);
    g_shm_monitor = monitor->id;
    g_shm_width = monitor->width;
    g_shm_height = monitor->height;
    return true;
}

grd_status grd_platform_capture(
    uint32_t monitor_id,
    bool prefer_gpu_resident,
    grd_frame *frame,
    grd_error *error
)
{
    (void)prefer_gpu_resident;
    grd_monitor monitors[GRD_MAX_MONITORS];
    const size_t count = grd_platform_monitors(monitors, GRD_MAX_MONITORS);
    if (frame == NULL || monitor_id >= count) {
        return GRD_INVALID_ARGUMENT;
    }
    const grd_monitor *monitor = &monitors[monitor_id];
    if (!ensure_shm_image(monitor)) {
        set_error(error, GRD_IO_ERROR, "XShmCreateImage failed");
        return GRD_IO_ERROR;
    }
    if (!XShmGetImage(
            g_display,
            g_root,
            g_shm_image,
            monitor->x,
            monitor->y,
            AllPlanes
        )) {
        set_error(error, GRD_IO_ERROR, "XShmGetImage failed");
        return GRD_IO_ERROR;
    }
    const size_t stride = (size_t)monitor->width * 4U;
    const size_t size = stride * monitor->height;
    if (g_capture_pixels_capacity < size) {
        uint8_t *resized = realloc(g_capture_pixels, size);
        if (resized == NULL) {
            return GRD_OUT_OF_MEMORY;
        }
        g_capture_pixels = resized;
        g_capture_pixels_capacity = size;
    }
    if (g_capture_pixels == NULL) {
        return GRD_OUT_OF_MEMORY;
    }
    const bool packed_bgrx =
        g_shm_image->bits_per_pixel == 32 &&
        g_shm_image->red_mask == 0x00ff0000UL &&
        g_shm_image->green_mask == 0x0000ff00UL &&
        g_shm_image->blue_mask == 0x000000ffUL &&
        g_shm_image->bytes_per_line >= (int)stride;
    for (uint32_t y = 0U; y < monitor->height; ++y) {
        const uint8_t *source =
            (const uint8_t *)g_shm_image->data +
            (size_t)y * (size_t)g_shm_image->bytes_per_line;
        uint8_t *destination = g_capture_pixels + (size_t)y * stride;
        if (packed_bgrx) {
            /* XRGB's unused byte is ignored by the H.264 conversion and by
             * the BGRA texture path. Copy the packed row in one operation
             * instead of touching every pixel. */
            memcpy(destination, source, stride);
            continue;
        }
        for (uint32_t x = 0U; x < monitor->width; ++x) {
            const size_t offset = (size_t)x * 4U;
            const unsigned long pixel = XGetPixel(
                g_shm_image, (int)x, (int)y
            );
            destination[offset] =
                (uint8_t)(pixel & g_shm_image->blue_mask);
            destination[offset + 1U] =
                (uint8_t)((pixel & g_shm_image->green_mask) >> 8U);
            destination[offset + 2U] =
                (uint8_t)((pixel & g_shm_image->red_mask) >> 16U);
            destination[offset + 3U] = 255U;
        }
    }
    memset(frame, 0, sizeof(*frame));
    frame->data = g_capture_pixels;
    frame->size = size;
    frame->width = monitor->width;
    frame->height = monitor->height;
    frame->stride = (uint32_t)stride;
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

static KeySym x11_keysym(uint32_t usage)
{
    if (usage >= 4U && usage <= 29U) {
        return (KeySym)(XK_a + (usage - 4U));
    }
    if (usage >= 30U && usage <= 38U) {
        return (KeySym)(XK_1 + (usage - 30U));
    }
    if (usage == 39U) {
        return XK_0;
    }
    if (usage >= 58U && usage <= 69U) {
        return (KeySym)(XK_F1 + (usage - 58U));
    }
    switch (usage) {
    case 40U: return XK_Return;
    case 41U: return XK_Escape;
    case 42U: return XK_BackSpace;
    case 43U: return XK_Tab;
    case 44U: return XK_space;
    case 45U: return XK_minus;
    case 46U: return XK_equal;
    case 47U: return XK_bracketleft;
    case 48U: return XK_bracketright;
    case 49U: return XK_backslash;
    case 51U: return XK_semicolon;
    case 52U: return XK_apostrophe;
    case 53U: return XK_grave;
    case 54U: return XK_comma;
    case 55U: return XK_period;
    case 56U: return XK_slash;
    case 57U: return XK_Caps_Lock;
    case 73U: return XK_Insert;
    case 74U: return XK_Home;
    case 75U: return XK_Page_Up;
    case 76U: return XK_Delete;
    case 77U: return XK_End;
    case 78U: return XK_Page_Down;
    case 79U: return XK_Right;
    case 80U: return XK_Left;
    case 81U: return XK_Down;
    case 82U: return XK_Up;
    case GRD_KEY_LEFT_CTRL: return XK_Control_L;
    case GRD_KEY_LEFT_SHIFT: return XK_Shift_L;
    case GRD_KEY_LEFT_ALT: return XK_Alt_L;
    case GRD_KEY_LEFT_GUI: return XK_Super_L;
    case GRD_KEY_RIGHT_CTRL: return XK_Control_R;
    case GRD_KEY_RIGHT_SHIFT: return XK_Shift_R;
    case GRD_KEY_RIGHT_ALT: return XK_Alt_R;
    case GRD_KEY_RIGHT_GUI: return XK_Super_R;
    default: return NoSymbol;
    }
}

grd_status grd_platform_inject(
    const grd_monitor *monitor,
    const grd_input_event *event,
    grd_error *error
)
{
    (void)error;
    if (monitor == NULL || event == NULL || g_display == NULL ||
        grd_protocol_validate_input(event) != GRD_OK) {
        return GRD_INVALID_ARGUMENT;
    }
    if (event->kind == GRD_INPUT_POINTER_MOVE) {
        const int x = monitor->x + (int)(event->x * (float)monitor->width);
        const int y = monitor->y + (int)(event->y * (float)monitor->height);
        (void)XTestFakeMotionEvent(g_display, -1, x, y, CurrentTime);
    } else if (event->kind == GRD_INPUT_POINTER_RELATIVE) {
        (void)XTestFakeRelativeMotionEvent(
            g_display, event->delta_x, event->delta_y, CurrentTime
        );
    } else if (event->kind == GRD_INPUT_POINTER_BUTTON) {
        const unsigned button = event->code == 1U ? 3U : event->code == 2U ? 2U : 1U;
        (void)XTestFakeButtonEvent(g_display, button, event->pressed, CurrentTime);
    } else if (event->kind == GRD_INPUT_SCROLL) {
        const unsigned button = event->delta_y > 0 ? 4U : 5U;
        (void)XTestFakeButtonEvent(g_display, button, True, CurrentTime);
        (void)XTestFakeButtonEvent(g_display, button, False, CurrentTime);
    } else if (event->kind == GRD_INPUT_KEY) {
        const KeySym symbol = x11_keysym(event->code);
        const KeyCode code = XKeysymToKeycode(g_display, symbol);
        if (symbol == NoSymbol || code == 0U) {
            return GRD_NOT_SUPPORTED;
        }
        (void)XTestFakeKeyEvent(
            g_display, code, event->pressed, CurrentTime
        );
    } else if (event->kind == GRD_INPUT_TEXT && event->text_length == 1U) {
        char text[2] = {event->text[0], '\0'};
        const KeySym symbol = XStringToKeysym(text);
        const KeyCode code = XKeysymToKeycode(g_display, symbol);
        if (code != 0U) {
            (void)XTestFakeKeyEvent(g_display, code, True, CurrentTime);
            (void)XTestFakeKeyEvent(g_display, code, False, CurrentTime);
        }
    }
    XFlush(g_display);
    return GRD_OK;
}

grd_status grd_platform_cursor_state(
    const grd_monitor *monitor,
    grd_cursor_state *state,
    grd_cursor_shape *shape,
    grd_error *error
)
{
    (void)error;
    if (monitor == NULL || state == NULL || shape == NULL ||
        g_display == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    Window root_return;
    Window child_return;
    int root_x;
    int root_y;
    int window_x;
    int window_y;
    unsigned mask;
    if (!XQueryPointer(
            g_display, g_root, &root_return, &child_return,
            &root_x, &root_y, &window_x, &window_y, &mask
        )) {
        return GRD_IO_ERROR;
    }
    XFixesCursorImage *cursor = XFixesGetCursorImage(g_display);
    bool visible = cursor != NULL;
    if (cursor != NULL) {
        visible = false;
        const unsigned long count =
            (unsigned long)cursor->width * (unsigned long)cursor->height;
        for (unsigned long index = 0U; index < count; ++index) {
            if ((cursor->pixels[index] >> 24U) != 0U) {
                visible = true;
                break;
            }
        }
    }
    state->visible = visible &&
                     root_x >= monitor->x &&
                     root_y >= monitor->y &&
                     root_x < monitor->x + (int)monitor->width &&
                     root_y < monitor->y + (int)monitor->height;
    state->x = ((float)root_x - (float)monitor->x) / (float)monitor->width;
    state->y = ((float)root_y - (float)monitor->y) / (float)monitor->height;
    if (state->x < 0.0F) state->x = 0.0F;
    if (state->x > 1.0F) state->x = 1.0F;
    if (state->y < 0.0F) state->y = 0.0F;
    if (state->y > 1.0F) state->y = 1.0F;
    memset(state->reserved, 0, sizeof(state->reserved));
    memset(shape, 0, sizeof(*shape));
    if (state->visible && visible && cursor != NULL && cursor->width > 0 &&
        cursor->height > 0) {
        const uint32_t width = cursor->width > (int)GRD_CURSOR_MAX_WIDTH
                                   ? GRD_CURSOR_MAX_WIDTH
                                   : (uint32_t)cursor->width;
        const uint32_t height = cursor->height > (int)GRD_CURSOR_MAX_HEIGHT
                                    ? GRD_CURSOR_MAX_HEIGHT
                                    : (uint32_t)cursor->height;
        shape->width = (uint16_t)width;
        shape->height = (uint16_t)height;
        shape->hotspot_x = cursor->xhot < 0
                               ? 0
                               : cursor->xhot > INT16_MAX
                                     ? INT16_MAX
                                     : (int16_t)cursor->xhot;
        shape->hotspot_y = cursor->yhot < 0
                               ? 0
                               : cursor->yhot > INT16_MAX
                                     ? INT16_MAX
                                     : (int16_t)cursor->yhot;
        for (uint32_t y = 0U; y < height; ++y) {
            for (uint32_t x = 0U; x < width; ++x) {
                const uint32_t pixel = (uint32_t)cursor->pixels[
                    (unsigned long)y * (unsigned long)cursor->width + x
                ];
                const size_t destination =
                    ((size_t)y * GRD_CURSOR_MAX_WIDTH + x) * 4U;
                shape->pixels[destination] = (uint8_t)(pixel >> 16U);
                shape->pixels[destination + 1U] = (uint8_t)(pixel >> 8U);
                shape->pixels[destination + 2U] = (uint8_t)pixel;
                shape->pixels[destination + 3U] = (uint8_t)(pixel >> 24U);
            }
        }
    }
    if (cursor != NULL) {
        XFree(cursor);
    }
    return GRD_OK;
}

bool grd_platform_screen_permission(void)
{
    return g_display != NULL;
}

bool grd_platform_input_permission(void)
{
    return g_display != NULL;
}

void grd_platform_request_permissions(void)
{
}

grd_status grd_platform_audio_start(grd_error *error)
{
    if (g_audio_capture != NULL) {
        return GRD_OK;
    }
    const pa_sample_spec specification = {
        .format = PA_SAMPLE_FLOAT32LE,
        .rate = GRD_AUDIO_SAMPLE_RATE,
        .channels = GRD_AUDIO_CHANNELS
    };
    const char *source = getenv("GRD_AUDIO_SOURCE");
    if (source == NULL || source[0] == '\0') {
        source = "@DEFAULT_MONITOR@";
    }
    int pulse_error = 0;
    g_audio_capture = pa_simple_new(
        NULL,
        "GRD",
        PA_STREAM_RECORD,
        source,
        "System audio",
        &specification,
        NULL,
        NULL,
        &pulse_error
    );
    if (g_audio_capture == NULL) {
        set_error(error, GRD_IO_ERROR, pa_strerror(pulse_error));
        return GRD_IO_ERROR;
    }
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
    if (g_audio_capture == NULL || stereo_samples == NULL ||
        frames_read == NULL || timestamp_micros == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    int pulse_error = 0;
    const size_t bytes =
        frame_capacity * GRD_AUDIO_CHANNELS * sizeof(float);
    if (pa_simple_read(
            g_audio_capture, stereo_samples, bytes, &pulse_error
        ) < 0) {
        set_error(error, GRD_IO_ERROR, pa_strerror(pulse_error));
        return GRD_IO_ERROR;
    }
    *frames_read = frame_capacity;
    *timestamp_micros = grd_now_micros();
    return GRD_OK;
}

void grd_platform_audio_stop(void)
{
    if (g_audio_capture != NULL) {
        pa_simple_free(g_audio_capture);
        g_audio_capture = NULL;
    }
}
