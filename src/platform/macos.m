#include "grd/platform.h"
#include "grd/audio.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <dispatch/dispatch.h>
#include <math.h>
#include <os/lock.h>

#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(grd_error *error, grd_status code, const char *message)
{
    if (error != NULL) {
        error->code = code;
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
}

static void capture_dimensions(
    uint32_t source_width,
    uint32_t source_height,
    uint32_t *output_width,
    uint32_t *output_height
)
{
    double scale = 1.0;
    if (source_width > 1920U) {
        scale = 1920.0 / (double)source_width;
    }
    if ((double)source_height * scale > 1080.0) {
        scale = 1080.0 / (double)source_height;
    }
    uint32_t width = (uint32_t)((double)source_width * scale) & ~1U;
    uint32_t height = (uint32_t)((double)source_height * scale) & ~1U;
    *output_width = width >= 2U ? width : 2U;
    *output_height = height >= 2U ? height : 2U;
}

static double display_refresh_rate(CGDirectDisplayID display)
{
    double refresh = 60.0;
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(display);
    if (current != NULL) {
        const double value = CGDisplayModeGetRefreshRate(current);
        if (value > 0.0) {
            refresh = value;
        }
        CGDisplayModeRelease(current);
    }
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, NULL);
    if (modes != NULL) {
        const CFIndex count = CFArrayGetCount(modes);
        for (CFIndex index = 0; index < count; ++index) {
            CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(
                modes, index
            );
            const double value = mode != NULL ? CGDisplayModeGetRefreshRate(mode) : 0.0;
            if (value > refresh) {
                refresh = value;
            }
        }
        CFRelease(modes);
    }
    if (refresh < 30.0) {
        refresh = 60.0;
    }
    return refresh > 120.0 ? 120.0 : refresh;
}

static SCContentFilter *capture_filter;
static uint32_t capture_filter_display;
static SCStream *video_stream;
static dispatch_queue_t video_queue;
static NSObject *video_output;
static CVPixelBufferRef video_frame;
static uint64_t video_frame_timestamp;
static uint32_t video_stream_display;
static os_unfair_lock video_lock = OS_UNFAIR_LOCK_INIT;
static void release_capture_pixel_buffer(void *owner)
{
    CVPixelBufferRef pixel_buffer = (CVPixelBufferRef)owner;
    if (pixel_buffer == NULL) {
        return;
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
    CFRelease(pixel_buffer);
}

#define AUDIO_RING_FRAMES (GRD_AUDIO_SAMPLE_RATE * 2U)

static float audio_ring[AUDIO_RING_FRAMES * GRD_AUDIO_CHANNELS];
static size_t audio_read_index;
static size_t audio_write_index;
static size_t audio_frame_count;
static uint64_t audio_timestamp_micros;
static os_unfair_lock audio_lock = OS_UNFAIR_LOCK_INIT;
static SCStream *audio_stream;
static dispatch_queue_t audio_queue;

static void audio_ring_push(const float *samples, size_t frames, uint64_t timestamp)
{
    os_unfair_lock_lock(&audio_lock);
    if (audio_frame_count == 0U) {
        audio_timestamp_micros = timestamp;
    }
    for (size_t frame = 0U; frame < frames; ++frame) {
        if (audio_frame_count == AUDIO_RING_FRAMES) {
            audio_read_index = (audio_read_index + 1U) % AUDIO_RING_FRAMES;
            --audio_frame_count;
        }
        const size_t destination = audio_write_index * GRD_AUDIO_CHANNELS;
        audio_ring[destination] = samples[frame * GRD_AUDIO_CHANNELS];
        audio_ring[destination + 1U] =
            samples[frame * GRD_AUDIO_CHANNELS + 1U];
        audio_write_index = (audio_write_index + 1U) % AUDIO_RING_FRAMES;
        ++audio_frame_count;
    }
    os_unfair_lock_unlock(&audio_lock);
}

@interface GRDAudioOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@end

@interface GRDVideoOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@end

@implementation GRDVideoOutput
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
    ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type != SCStreamOutputTypeScreen ||
        !CMSampleBufferDataIsReady(sampleBuffer)) {
        return;
    }
    CVImageBufferRef image = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (image == NULL) {
        return;
    }
    CVPixelBufferRef retained = (CVPixelBufferRef)CFRetain(image);
    const CMTime presentation =
        CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    const double seconds = CMTimeGetSeconds(presentation);
    const uint64_t timestamp =
        isfinite(seconds) && seconds >= 0.0
            ? (uint64_t)(seconds * 1000000.0)
            : grd_now_micros();
    os_unfair_lock_lock(&video_lock);
    CVPixelBufferRef previous = video_frame;
    video_frame = retained;
    video_frame_timestamp = timestamp;
    os_unfair_lock_unlock(&video_lock);
    if (previous != NULL) {
        CFRelease(previous);
    }
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    (void)stream;
    (void)error;
}
@end

@implementation GRDAudioOutput
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
    ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type != SCStreamOutputTypeAudio ||
        !CMSampleBufferDataIsReady(sampleBuffer)) {
        return;
    }
    CMFormatDescriptionRef description =
        CMSampleBufferGetFormatDescription(sampleBuffer);
    if (description == NULL) {
        return;
    }
    const AudioStreamBasicDescription *format =
        CMAudioFormatDescriptionGetStreamBasicDescription(description);
    const CMItemCount sample_count = CMSampleBufferGetNumSamples(sampleBuffer);
    if (format == NULL || sample_count <= 0 ||
        format->mFormatID != kAudioFormatLinearPCM ||
        (format->mFormatFlags & kAudioFormatFlagIsFloat) == 0U ||
        format->mBitsPerChannel != 32U) {
        return;
    }

    size_t list_size = 0U;
    OSStatus status =
        CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer,
            &list_size,
            NULL,
            0U,
            NULL,
            NULL,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
            NULL
        );
    if (status != noErr && list_size == 0U) {
        return;
    }
    AudioBufferList *list = malloc(list_size);
    CMBlockBufferRef block = NULL;
    if (list == NULL ||
        CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer,
            NULL,
            list,
            list_size,
            NULL,
            NULL,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
            &block
        ) != noErr) {
        free(list);
        return;
    }
    const size_t frames = (size_t)sample_count;
    float *interleaved = malloc(
        frames * GRD_AUDIO_CHANNELS * sizeof(float)
    );
    bool converted = false;
    if (interleaved != NULL) {
        const bool non_interleaved =
            (format->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0U;
        if (non_interleaved && list->mNumberBuffers >= 2U) {
            const float *left = list->mBuffers[0].mData;
            const float *right = list->mBuffers[1].mData;
            for (size_t frame = 0U; frame < frames; ++frame) {
                interleaved[frame * 2U] = left[frame];
                interleaved[frame * 2U + 1U] = right[frame];
            }
            converted = true;
        } else if (list->mNumberBuffers >= 1U) {
            const float *input = list->mBuffers[0].mData;
            const uint32_t channels =
                format->mChannelsPerFrame != 0U
                    ? format->mChannelsPerFrame
                    : 1U;
            for (size_t frame = 0U; frame < frames; ++frame) {
                interleaved[frame * 2U] = input[frame * channels];
                interleaved[frame * 2U + 1U] =
                    channels >= 2U
                        ? input[frame * channels + 1U]
                        : input[frame * channels];
            }
            converted = true;
        }
        if (converted) {
            const CMTime presentation =
                CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
            const double seconds = CMTimeGetSeconds(presentation);
            const uint64_t timestamp =
                isfinite(seconds) && seconds >= 0.0
                    ? (uint64_t)(seconds * 1000000.0)
                    : grd_now_micros();
            audio_ring_push(interleaved, frames, timestamp);
        }
        free(interleaved);
    }
    if (block != NULL) {
        CFRelease(block);
    }
    free(list);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    (void)stream;
    (void)error;
}
@end

static GRDAudioOutput *audio_output;

static SCContentFilter *filter_for_display(
    uint32_t monitor_id,
    grd_error *error
)
{
    if (capture_filter != nil && capture_filter_display == monitor_id) {
        return capture_filter;
    }
    dispatch_semaphore_t completed = dispatch_semaphore_create(0);
    __block SCContentFilter *result = nil;
    __block NSError *failure = nil;
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:NO
        onScreenWindowsOnly:NO
        completionHandler:^(SCShareableContent *content, NSError *content_error) {
            if (content_error != nil) {
                failure = content_error;
            } else {
                for (SCDisplay *display in content.displays) {
                    if (display.displayID == monitor_id) {
                        result = [[SCContentFilter alloc]
                            initWithDisplay:display
                            excludingWindows:@[]];
                        break;
                    }
                }
            }
            dispatch_semaphore_signal(completed);
        }];
    (void)dispatch_semaphore_wait(completed, DISPATCH_TIME_FOREVER);
    if (result == nil) {
        const char *detail = failure.localizedDescription.UTF8String;
        set_error(
            error,
            GRD_IO_ERROR,
            detail != NULL ? detail : "Display is unavailable to ScreenCaptureKit"
        );
        return nil;
    }
    capture_filter = result;
    capture_filter_display = monitor_id;
    return capture_filter;
}

static void stop_video_stream(void)
{
    if (video_stream != nil) {
        dispatch_semaphore_t stopped = dispatch_semaphore_create(0);
        [video_stream stopCaptureWithCompletionHandler:^(NSError *error) {
            (void)error;
            dispatch_semaphore_signal(stopped);
        }];
        (void)dispatch_semaphore_wait(
            stopped,
            dispatch_time(DISPATCH_TIME_NOW, 2LL * NSEC_PER_SEC)
        );
        video_stream = nil;
    }
    video_output = nil;
    video_queue = nil;
    os_unfair_lock_lock(&video_lock);
    CVPixelBufferRef previous = video_frame;
    video_frame = NULL;
    video_frame_timestamp = 0U;
    os_unfair_lock_unlock(&video_lock);
    if (previous != NULL) {
        CFRelease(previous);
    }
}

static bool start_video_stream(uint32_t monitor_id, grd_error *error)
{
    if (video_stream != nil && video_stream_display == monitor_id) {
        return true;
    }
    stop_video_stream();
    SCContentFilter *filter = filter_for_display(monitor_id, error);
    if (filter == nil) {
        return false;
    }
    SCStreamConfiguration *configuration =
        [[SCStreamConfiguration alloc] init];
    uint32_t capture_width = 0U;
    uint32_t capture_height = 0U;
    capture_dimensions(
        (uint32_t)CGDisplayPixelsWide((CGDirectDisplayID)monitor_id),
        (uint32_t)CGDisplayPixelsHigh((CGDirectDisplayID)monitor_id),
        &capture_width,
        &capture_height
    );
    configuration.width = capture_width;
    configuration.height = capture_height;
    configuration.pixelFormat = kCVPixelFormatType_32BGRA;
    configuration.showsCursor = NO;
    configuration.queueDepth = 3;
    const double refresh_rate = display_refresh_rate(
        (CGDirectDisplayID)monitor_id
    );
    configuration.minimumFrameInterval = CMTimeMake(
        1,
        (int32_t)(refresh_rate + 0.5)
    );
    video_output = [[GRDVideoOutput alloc] init];
    video_queue = dispatch_queue_create(
        "dev.grd.video.capture", DISPATCH_QUEUE_SERIAL
    );
    __block NSError *failure = nil;
    video_stream = [[SCStream alloc]
        initWithFilter:filter
        configuration:configuration
        delegate:(id<SCStreamDelegate>)video_output];
    if (video_stream == nil || video_queue == nil ||
        ![video_stream
            addStreamOutput:(id<SCStreamOutput>)video_output
            type:SCStreamOutputTypeScreen
            sampleHandlerQueue:video_queue
            error:&failure]) {
        const char *detail = failure.localizedDescription.UTF8String;
        set_error(
            error,
            GRD_IO_ERROR,
            detail != NULL ? detail : "Failed to start macOS video capture"
        );
        stop_video_stream();
        return false;
    }
    dispatch_semaphore_t started = dispatch_semaphore_create(0);
    [video_stream startCaptureWithCompletionHandler:^(NSError *start_error) {
        failure = start_error;
        dispatch_semaphore_signal(started);
    }];
    (void)dispatch_semaphore_wait(
        started,
        dispatch_time(DISPATCH_TIME_NOW, 2LL * NSEC_PER_SEC)
    );
    if (failure != nil) {
        const char *detail = failure.localizedDescription.UTF8String;
        set_error(
            error,
            GRD_IO_ERROR,
            detail != NULL ? detail : "Failed to start macOS video capture"
        );
        stop_video_stream();
        return false;
    }
    video_stream_display = monitor_id;
    return true;
}

grd_status grd_platform_initialize(grd_error *error)
{
    if (sodium_init() < 0) {
        set_error(error, GRD_ERROR, "Cryptographic initialization failed");
        return GRD_ERROR;
    }
    return GRD_OK;
}

void grd_platform_shutdown(void)
{
    stop_video_stream();
    capture_filter = nil;
}

grd_os grd_platform_os(void)
{
    return GRD_OS_MACOS;
}

grd_status grd_platform_validate_host(grd_error *error)
{
    if (!grd_platform_screen_permission()) {
        set_error(error, GRD_NOT_SUPPORTED, "Screen Recording permission is missing");
        return GRD_NOT_SUPPORTED;
    }
    if (!grd_platform_input_permission()) {
        set_error(error, GRD_NOT_SUPPORTED, "Accessibility permission is missing");
        return GRD_NOT_SUPPORTED;
    }
    return GRD_OK;
}

size_t grd_platform_monitors(grd_monitor *monitors, size_t capacity)
{
    CGDirectDisplayID displays[GRD_MAX_MONITORS];
    uint32_t count = 0U;
    if (CGGetActiveDisplayList(GRD_MAX_MONITORS, displays, &count) != kCGErrorSuccess) {
        return 0U;
    }
    const size_t output_count = count < capacity ? count : capacity;
    for (size_t index = 0U; index < output_count; ++index) {
        const CGDirectDisplayID display = displays[index];
        const CGRect bounds = CGDisplayBounds(display);
        grd_monitor *monitor = &monitors[index];
        memset(monitor, 0, sizeof(*monitor));
        monitor->id = display;
        (void)snprintf(
            monitor->name,
            sizeof(monitor->name),
            "Display %u%s",
            display,
            CGDisplayIsBuiltin(display) ? " built-in" : ""
        );
        monitor->x = (int32_t)bounds.origin.x;
        monitor->y = (int32_t)bounds.origin.y;
        monitor->width = (uint32_t)CGDisplayPixelsWide(display);
        monitor->height = (uint32_t)CGDisplayPixelsHigh(display);
        monitor->scale = bounds.size.width > 0.0
                             ? (float)((double)monitor->width / bounds.size.width)
                             : 1.0F;
        monitor->primary = CGDisplayIsMain(display);
    }
    return output_count;
}

grd_status grd_platform_capture(
    uint32_t monitor_id,
    bool prefer_gpu_resident,
    grd_frame *frame,
    grd_error *error
)
{
    (void)prefer_gpu_resident;
    if (frame == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    memset(frame, 0, sizeof(*frame));
    if (!start_video_stream(monitor_id, error)) {
        return GRD_IO_ERROR;
    }
    CVPixelBufferRef pixel_buffer = NULL;
    uint64_t timestamp = grd_now_micros();
    os_unfair_lock_lock(&video_lock);
    if (video_frame != NULL) {
        pixel_buffer = (CVPixelBufferRef)CFRetain(video_frame);
        timestamp = video_frame_timestamp;
    }
    os_unfair_lock_unlock(&video_lock);
    if (pixel_buffer == NULL) {
        return GRD_WOULD_BLOCK;
    }
    if (CVPixelBufferLockBaseAddress(
            pixel_buffer, kCVPixelBufferLock_ReadOnly
        ) != kCVReturnSuccess) {
        CFRelease(pixel_buffer);
        return GRD_WOULD_BLOCK;
    }
    const size_t width = CVPixelBufferGetWidth(pixel_buffer);
    const size_t height = CVPixelBufferGetHeight(pixel_buffer);
    const size_t stride = CVPixelBufferGetBytesPerRow(pixel_buffer);
    const uint8_t *source = CVPixelBufferGetBaseAddress(pixel_buffer);
    const size_t size = height != 0U && stride <= SIZE_MAX / height
                            ? stride * height
                            : 0U;
    if (source == NULL || size == 0U || width > UINT32_MAX ||
        height > UINT32_MAX || stride > UINT32_MAX) {
        CVPixelBufferUnlockBaseAddress(
            pixel_buffer, kCVPixelBufferLock_ReadOnly
        );
        CFRelease(pixel_buffer);
        return GRD_OUT_OF_MEMORY;
    }
    /* Keep the native ScreenCaptureKit buffer alive until the encoder
     * releases the frame. This removes a full monitor-sized CPU copy from
     * every capture iteration; the release hook unlocks the CVPixelBuffer
     * after the encoder has consumed it. */
    frame->data = (uint8_t *)source;
    frame->size = size;
    frame->width = (uint32_t)width;
    frame->height = (uint32_t)height;
    frame->stride = (uint32_t)stride;
    frame->format = GRD_PIXEL_BGRA8;
    frame->timestamp_micros = timestamp;
    frame->owner = pixel_buffer;
    frame->release_fn = release_capture_pixel_buffer;
    return GRD_OK;
}

void grd_platform_frame_release(grd_frame *frame)
{
    if (frame != NULL) {
        if (frame->release_fn != NULL) {
            frame->release_fn(frame->owner);
        } else {
            free(frame->data);
        }
        memset(frame, 0, sizeof(*frame));
    }
}

static CGMouseButton mouse_button(uint32_t code)
{
    if (code == 1U) {
        return kCGMouseButtonRight;
    }
    if (code == 2U) {
        return kCGMouseButtonCenter;
    }
    return kCGMouseButtonLeft;
}

static CGKeyCode mac_key_code(uint32_t usage)
{
    static const CGKeyCode letters[26] = {
        0U, 11U, 8U, 2U, 14U, 3U, 5U, 4U, 34U, 38U, 40U, 37U, 46U,
        45U, 31U, 35U, 12U, 15U, 1U, 17U, 32U, 9U, 13U, 7U, 16U, 6U
    };
    static const CGKeyCode digits[10] = {
        18U, 19U, 20U, 21U, 23U, 22U, 26U, 28U, 25U, 29U
    };
    if (usage >= 4U && usage <= 29U) {
        return letters[usage - 4U];
    }
    if (usage >= 30U && usage <= 39U) {
        return digits[usage - 30U];
    }
    switch (usage) {
    case 40U: return 36U;
    case 41U: return 53U;
    case 42U: return 51U;
    case 43U: return 48U;
    case 44U: return 49U;
    case 45U: return 27U;
    case 46U: return 24U;
    case 47U: return 33U;
    case 48U: return 30U;
    case 49U: return 42U;
    case 51U: return 41U;
    case 52U: return 39U;
    case 53U: return 50U;
    case 54U: return 43U;
    case 55U: return 47U;
    case 56U: return 44U;
    case 57U: return 57U;
    case 58U: return 122U;
    case 59U: return 120U;
    case 60U: return 99U;
    case 61U: return 118U;
    case 62U: return 96U;
    case 63U: return 97U;
    case 64U: return 98U;
    case 65U: return 100U;
    case 66U: return 101U;
    case 67U: return 109U;
    case 68U: return 103U;
    case 69U: return 111U;
    case 73U: return 114U;
    case 74U: return 115U;
    case 75U: return 116U;
    case 76U: return 117U;
    case 77U: return 119U;
    case 78U: return 121U;
    case 79U: return 124U;
    case 80U: return 123U;
    case 81U: return 125U;
    case 82U: return 126U;
    case GRD_KEY_LEFT_CTRL: return 59U;
    case GRD_KEY_LEFT_SHIFT: return 56U;
    case GRD_KEY_LEFT_ALT: return 58U;
    case GRD_KEY_LEFT_GUI: return 55U;
    case GRD_KEY_RIGHT_CTRL: return 62U;
    case GRD_KEY_RIGHT_SHIFT: return 60U;
    case GRD_KEY_RIGHT_ALT: return 61U;
    case GRD_KEY_RIGHT_GUI: return 54U;
    default: return UINT16_MAX;
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
        return GRD_INVALID_ARGUMENT;
    }
    CGEventRef cg_event = NULL;
    const CGPoint position = CGPointMake(
        (float)monitor->x + event->x * (float)monitor->width,
        (float)monitor->y + event->y * (float)monitor->height
    );
    switch ((grd_input_kind)event->kind) {
    case GRD_INPUT_POINTER_MOVE:
        cg_event = CGEventCreateMouseEvent(
            NULL, kCGEventMouseMoved, position, kCGMouseButtonLeft
        );
        break;
    case GRD_INPUT_POINTER_RELATIVE:
        cg_event = CGEventCreate(NULL);
        if (cg_event != NULL) {
            CGEventSetType(cg_event, kCGEventMouseMoved);
            CGEventSetIntegerValueField(
                cg_event, kCGMouseEventDeltaX, event->delta_x
            );
            CGEventSetIntegerValueField(
                cg_event, kCGMouseEventDeltaY, event->delta_y
            );
        }
        break;
    case GRD_INPUT_POINTER_BUTTON: {
        const CGMouseButton button = mouse_button(event->code);
        CGEventType type;
        if (button == kCGMouseButtonRight) {
            type = event->pressed ? kCGEventRightMouseDown : kCGEventRightMouseUp;
        } else if (button == kCGMouseButtonCenter) {
            type = event->pressed ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
        } else {
            type = event->pressed ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
        }
        cg_event = CGEventCreateMouseEvent(NULL, type, position, button);
        break;
    }
    case GRD_INPUT_SCROLL:
        cg_event = CGEventCreateScrollWheelEvent(
            NULL,
            kCGScrollEventUnitPixel,
            2U,
            event->delta_y,
            event->delta_x
        );
        break;
    case GRD_INPUT_KEY:
        if (mac_key_code(event->code) == UINT16_MAX) {
            return GRD_NOT_SUPPORTED;
        }
        cg_event = CGEventCreateKeyboardEvent(
            NULL,
            mac_key_code(event->code),
            event->pressed
        );
        break;
    case GRD_INPUT_TEXT: {
        @autoreleasepool {
            NSData *data = [NSData dataWithBytes:event->text length:event->text_length];
            NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
            if (text != nil) {
                const NSUInteger length = [text length];
                UniChar characters[32];
                const NSUInteger count = length < 32U ? length : 32U;
                [text getCharacters:characters range:NSMakeRange(0U, count)];
                cg_event = CGEventCreateKeyboardEvent(NULL, 0U, true);
                CGEventKeyboardSetUnicodeString(cg_event, count, characters);
            }
        }
        break;
    }
    default:
        return GRD_INVALID_ARGUMENT;
    }
    if (cg_event == NULL) {
        set_error(error, GRD_ERROR, "Failed to create input event");
        return GRD_ERROR;
    }
    CGEventPost(kCGSessionEventTap, cg_event);
    CFRelease(cg_event);
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
    if (monitor == NULL || state == NULL || shape == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    CGEventRef event = CGEventCreate(NULL);
    if (event == NULL) {
        return GRD_IO_ERROR;
    }
    const CGPoint point = CGEventGetLocation(event);
    CFRelease(event);
    state->visible = point.x >= monitor->x &&
                     point.y >= monitor->y &&
                     point.x < monitor->x + (CGFloat)monitor->width &&
                     point.y < monitor->y + (CGFloat)monitor->height;
    state->x = ((float)point.x - (float)monitor->x) / (float)monitor->width;
    state->y = ((float)point.y - (float)monitor->y) / (float)monitor->height;
    memset(state->reserved, 0, sizeof(state->reserved));
    memset(shape, 0, sizeof(*shape));
    return GRD_OK;
}

bool grd_platform_screen_permission(void)
{
    return CGPreflightScreenCaptureAccess();
}

bool grd_platform_input_permission(void)
{
    return AXIsProcessTrusted();
}

void grd_platform_request_permissions(void)
{
    if (!CGPreflightScreenCaptureAccess()) {
        (void)CGRequestScreenCaptureAccess();
    }
    if (!AXIsProcessTrusted()) {
        const void *keys[] = {kAXTrustedCheckOptionPrompt};
        const void *values[] = {kCFBooleanTrue};
        CFDictionaryRef options = CFDictionaryCreate(
            NULL, keys, values, 1U,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks
        );
        (void)AXIsProcessTrustedWithOptions(options);
        CFRelease(options);
    }
}

grd_status grd_platform_audio_start(grd_error *error)
{
    if (audio_stream != nil) {
        return GRD_OK;
    }
    SCContentFilter *filter = filter_for_display(
        (uint32_t)CGMainDisplayID(), error
    );
    if (filter == nil) {
        return GRD_IO_ERROR;
    }
    SCStreamConfiguration *configuration =
        [[SCStreamConfiguration alloc] init];
    configuration.capturesAudio = YES;
    configuration.excludesCurrentProcessAudio = YES;
    configuration.sampleRate = (NSInteger)GRD_AUDIO_SAMPLE_RATE;
    configuration.channelCount = (NSInteger)GRD_AUDIO_CHANNELS;
    configuration.width = 2U;
    configuration.height = 2U;
    configuration.queueDepth = 3;

    audio_output = [[GRDAudioOutput alloc] init];
    audio_stream = [[SCStream alloc]
        initWithFilter:filter
        configuration:configuration
        delegate:audio_output];
    audio_queue = dispatch_queue_create(
        "dev.grd.audio.capture", DISPATCH_QUEUE_SERIAL
    );
    NSError *add_error = nil;
    if (![audio_stream
            addStreamOutput:audio_output
            type:SCStreamOutputTypeAudio
            sampleHandlerQueue:audio_queue
            error:&add_error]) {
        const char *message = add_error.localizedDescription.UTF8String;
        set_error(
            error,
            GRD_IO_ERROR,
            message != NULL ? message : "Audio output is unavailable"
        );
        audio_stream = nil;
        audio_output = nil;
        audio_queue = nil;
        return GRD_IO_ERROR;
    }
    os_unfair_lock_lock(&audio_lock);
    audio_read_index = 0U;
    audio_write_index = 0U;
    audio_frame_count = 0U;
    os_unfair_lock_unlock(&audio_lock);

    dispatch_semaphore_t completed = dispatch_semaphore_create(0);
    __block NSError *start_error = nil;
    [audio_stream startCaptureWithCompletionHandler:^(NSError *failure) {
        start_error = failure;
        dispatch_semaphore_signal(completed);
    }];
    (void)dispatch_semaphore_wait(completed, DISPATCH_TIME_FOREVER);
    if (start_error != nil) {
        const char *message = start_error.localizedDescription.UTF8String;
        set_error(
            error,
            GRD_IO_ERROR,
            message != NULL ? message : "Failed to start audio capture"
        );
        audio_stream = nil;
        audio_output = nil;
        audio_queue = nil;
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
    (void)error;
    if (stereo_samples == NULL || frames_read == NULL ||
        timestamp_micros == NULL || frame_capacity == 0U) {
        return GRD_INVALID_ARGUMENT;
    }
    *frames_read = 0U;
    os_unfair_lock_lock(&audio_lock);
    if (audio_frame_count < frame_capacity) {
        os_unfair_lock_unlock(&audio_lock);
        return GRD_WOULD_BLOCK;
    }
    *timestamp_micros = audio_timestamp_micros;
    for (size_t frame = 0U; frame < frame_capacity; ++frame) {
        const size_t source = audio_read_index * GRD_AUDIO_CHANNELS;
        stereo_samples[frame * GRD_AUDIO_CHANNELS] = audio_ring[source];
        stereo_samples[frame * GRD_AUDIO_CHANNELS + 1U] =
            audio_ring[source + 1U];
        audio_read_index = (audio_read_index + 1U) % AUDIO_RING_FRAMES;
    }
    audio_frame_count -= frame_capacity;
    audio_timestamp_micros +=
        (uint64_t)frame_capacity * 1000000ULL / GRD_AUDIO_SAMPLE_RATE;
    *frames_read = frame_capacity;
    os_unfair_lock_unlock(&audio_lock);
    return GRD_OK;
}

void grd_platform_audio_stop(void)
{
    if (audio_stream == nil) {
        return;
    }
    dispatch_semaphore_t completed = dispatch_semaphore_create(0);
    [audio_stream stopCaptureWithCompletionHandler:^(NSError *failure) {
        (void)failure;
        dispatch_semaphore_signal(completed);
    }];
    (void)dispatch_semaphore_wait(completed, DISPATCH_TIME_FOREVER);
    audio_stream = nil;
    audio_output = nil;
    audio_queue = nil;
    os_unfair_lock_lock(&audio_lock);
    audio_frame_count = 0U;
    os_unfair_lock_unlock(&audio_lock);
}
