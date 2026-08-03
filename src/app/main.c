#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_COMMAND_USERDATA
#define NK_IMPLEMENTATION
#define NK_SDL3_RENDERER_IMPLEMENTATION

#include "app/app.h"
#include "grd/log.h"
#include "nuklear_sdl3_renderer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Set by the startup renderer self-test when the first renderer failed and
 * the app switched to the other driver (reported in the fallback map). */
static bool g_renderer_fell_back = false;

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#if defined(__linux__)
#include <X11/Xlib.h>
#endif

/* File log sink: GRD is a GUI application, so stderr is invisible. All log
 * lines (including the periodic host transmission stats) are appended to
 * grd.log in the GRD data directory. */
static SDL_Mutex *g_log_mutex;
static FILE *g_log_file;

static void grd_file_log_sink(
    grd_log_level level,
    const char *message,
    void *userdata
)
{
    (void)userdata;
    if (g_log_mutex != NULL) {
        SDL_LockMutex(g_log_mutex);
    }
    if (g_log_file != NULL) {
        static const char *labels[] = {"DEBUG", "INFO", "WARN", "ERROR"};
        const size_t index = (size_t)level < 4U ? (size_t)level : 3U;
        time_t now = time(NULL);
        struct tm tm_value;
#if defined(_WIN32)
        (void)localtime_s(&tm_value, &now);
#else
        (void)localtime_r(&now, &tm_value);
#endif
        (void)fprintf(
            g_log_file,
            "[%04d-%02d-%02d %02d:%02d:%02d][GRD %s] %s\n",
            tm_value.tm_year + 1900,
            tm_value.tm_mon + 1,
            tm_value.tm_mday,
            tm_value.tm_hour,
            tm_value.tm_min,
            tm_value.tm_sec,
            labels[index],
            message
        );
        (void)fflush(g_log_file);
    }
    if (g_log_mutex != NULL) {
        SDL_UnlockMutex(g_log_mutex);
    }
}

static bool grd_open_log_file(void)
{
    char directory[1024];
    char path[1100];
    if (!grd_config_directory(directory, sizeof(directory))) {
        return false;
    }
#if defined(_WIN32)
    if (_mkdir(directory) != 0 && errno != EEXIST) {
        return false;
    }
    (void)snprintf(path, sizeof(path), "%s\\grd.log", directory);
#else
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
        return false;
    }
    (void)snprintf(path, sizeof(path), "%s/grd.log", directory);
#endif
    g_log_file = fopen(path, "a");
    return g_log_file != NULL;
}

typedef struct password_font_wrapper {
    struct nk_user_font font;
    struct nk_user_font source;
} password_font_wrapper;

static float password_font_width(
    nk_handle handle,
    float height,
    const char *text,
    int length
)
{
    password_font_wrapper *wrapper = handle.ptr;
    if (wrapper == NULL || text == NULL || length <= 0) {
        return 0.0F;
    }
    struct nk_user_font_glyph glyph;
    wrapper->source.query(
        wrapper->source.userdata,
        height,
        &glyph,
        (nk_rune)'*',
        (nk_rune)'*'
    );
    return (float)nk_utf_len(text, length) * glyph.xadvance;
}

static void password_font_query(
    nk_handle handle,
    float height,
    struct nk_user_font_glyph *glyph,
    nk_rune codepoint,
    nk_rune next_codepoint
)
{
    password_font_wrapper *wrapper = handle.ptr;
    (void)codepoint;
    (void)next_codepoint;
    wrapper->source.query(
        wrapper->source.userdata,
        height,
        glyph,
        (nk_rune)'*',
        (nk_rune)'*'
    );
}

static void initialize_password_font(
    password_font_wrapper *wrapper,
    const struct nk_font *source
)
{
    wrapper->source = source->handle;
    wrapper->font = source->handle;
    wrapper->font.userdata = nk_handle_ptr(wrapper);
    wrapper->font.width = password_font_width;
    wrapper->font.query = password_font_query;
}

static struct nk_font *add_platform_font(
    struct nk_font_atlas *atlas,
    float size
)
{
    struct nk_font *font = NULL;
#if defined(__APPLE__)
    font = nk_font_atlas_add_from_file(
        atlas, "/System/Library/Fonts/SFNS.ttf", size, NULL
    );
#elif defined(_WIN32)
    font = nk_font_atlas_add_from_file(
        atlas, "C:\\Windows\\Fonts\\segoeui.ttf", size, NULL
    );
#else
    font = nk_font_atlas_add_from_file(
        atlas,
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        size,
        NULL
    );
#endif
    if (font == NULL) {
        font = nk_font_atlas_add_default(atlas, size, NULL);
    }
    return font;
}

static void apply_style(struct nk_context *context)
{
    struct nk_color table[NK_COLOR_COUNT];
    table[NK_COLOR_TEXT] = nk_rgb(29, 29, 31);
    table[NK_COLOR_WINDOW] = nk_rgb(245, 245, 247);
    table[NK_COLOR_HEADER] = nk_rgb(255, 255, 255);
    table[NK_COLOR_BORDER] = nk_rgb(210, 210, 215);
    table[NK_COLOR_BUTTON] = nk_rgb(255, 255, 255);
    table[NK_COLOR_BUTTON_HOVER] = nk_rgb(245, 245, 247);
    table[NK_COLOR_BUTTON_ACTIVE] = nk_rgb(232, 232, 237);
    table[NK_COLOR_TOGGLE] = nk_rgb(210, 210, 215);
    table[NK_COLOR_TOGGLE_HOVER] = nk_rgb(190, 190, 195);
    table[NK_COLOR_TOGGLE_CURSOR] = nk_rgb(0, 113, 227);
    table[NK_COLOR_SELECT] = nk_rgb(245, 245, 247);
    table[NK_COLOR_SELECT_ACTIVE] = nk_rgb(224, 239, 255);
    table[NK_COLOR_SLIDER] = nk_rgb(232, 232, 237);
    table[NK_COLOR_SLIDER_CURSOR] = nk_rgb(0, 113, 227);
    table[NK_COLOR_SLIDER_CURSOR_HOVER] = nk_rgb(0, 119, 237);
    table[NK_COLOR_SLIDER_CURSOR_ACTIVE] = nk_rgb(0, 102, 204);
    table[NK_COLOR_PROPERTY] = nk_rgb(255, 255, 255);
    table[NK_COLOR_EDIT] = nk_rgb(255, 255, 255);
    table[NK_COLOR_EDIT_CURSOR] = nk_rgb(29, 29, 31);
    table[NK_COLOR_COMBO] = nk_rgb(255, 255, 255);
    table[NK_COLOR_CHART] = nk_rgb(255, 255, 255);
    table[NK_COLOR_CHART_COLOR] = nk_rgb(0, 113, 227);
    table[NK_COLOR_CHART_COLOR_HIGHLIGHT] = nk_rgb(0, 119, 237);
    table[NK_COLOR_SCROLLBAR] = nk_rgb(245, 245, 247);
    table[NK_COLOR_SCROLLBAR_CURSOR] = nk_rgb(210, 210, 215);
    table[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = nk_rgb(174, 174, 178);
    table[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgb(134, 134, 139);
    table[NK_COLOR_TAB_HEADER] = nk_rgb(255, 255, 255);
    nk_style_from_table(context, table);
    context->style.window.padding = nk_vec2(28.0F, 20.0F);
    context->style.window.spacing = nk_vec2(10.0F, 10.0F);
    context->style.window.group_padding = nk_vec2(22.0F, 18.0F);
    context->style.window.border = 0.0F;
    context->style.window.rounding = 0.0F;
    context->style.button.padding = nk_vec2(14.0F, 9.0F);
    context->style.button.rounding = 12.0F;
    context->style.button.border = 1.0F;
    context->style.button.border_color = nk_rgb(210, 210, 215);
    context->style.edit.padding = nk_vec2(12.0F, 9.0F);
    context->style.edit.rounding = 8.0F;
    context->style.edit.border = 1.0F;
    context->style.edit.border_color = nk_rgb(210, 210, 215);
    context->style.combo.rounding = 8.0F;
    context->style.combo.button.rounding = 7.0F;
    context->style.combo.button.padding = nk_vec2(14.0F, 14.0F);
    context->style.combo.button.border = 0.0F;
    context->style.property.rounding = 8.0F;
    context->style.property.border = 1.0F;
    context->style.property.border_color = nk_rgb(210, 210, 215);
    context->style.property.padding = nk_vec2(12.0F, 8.0F);
    context->style.property.sym_left = NK_SYMBOL_NONE;
    context->style.property.sym_right = NK_SYMBOL_NONE;
    context->style.property.inc_button.border = 0.0F;
    context->style.property.dec_button.border = 0.0F;
}

/* Runtime fallback to the software renderer: a GPU renderer (Metal on macOS)
 * can refuse drawables mid-session (occluded window, display sleep) and then
 * fail every SDL_UpdateTexture with an empty error, leaving the remote view
 * permanently black. The main thread performs the swap and re-initializes
 * nuklear against the new renderer. */
static void switch_to_software_renderer(
    SDL_Window *window,
    SDL_Renderer **renderer_ptr,
    struct nk_context **context_ptr,
    struct nk_font **font_ptr,
    password_font_wrapper *wrapper,
    grd_app *app
)
{
    const char *old_name = SDL_GetRendererName(*renderer_ptr);
    GRD_WARN(
        "texture upload failed persistently: switching from %s to the "
        "software renderer",
        old_name != NULL ? old_name : "GPU"
    );
    grd_app_reset_remote_texture(app);
    nk_sdl_shutdown(*context_ptr);
    SDL_DestroyRenderer(*renderer_ptr);
    *renderer_ptr = SDL_CreateRenderer(window, "software");
    if (*renderer_ptr == NULL) {
        *renderer_ptr = SDL_CreateRenderer(window, NULL);
    }
    if (*renderer_ptr == NULL) {
        GRD_LOG_ERRORF("unable to create software renderer: %s", SDL_GetError());
        (void)SDL_SetAtomicInt(&app->renderer_fallback_requested, 0);
        return;
    }
    float display_scale = SDL_GetWindowDisplayScale(window);
    if (display_scale < 1.0F) {
        display_scale = 1.0F;
    }
    (void)SDL_SetRenderScale(*renderer_ptr, display_scale, display_scale);
    app->immediate_present = SDL_SetRenderVSync(*renderer_ptr, 0);
    *context_ptr = nk_sdl_init(window, *renderer_ptr, nk_sdl_allocator());
    if (*context_ptr == NULL) {
        GRD_LOG_ERRORF("unable to initialize Nuklear with the software renderer");
        (void)SDL_SetAtomicInt(&app->renderer_fallback_requested, 0);
        return;
    }
    struct nk_font_atlas *atlas = nk_sdl_font_stash_begin(*context_ptr);
    struct nk_font *fallback_font = add_platform_font(atlas, 17.0F);
    struct nk_font *heading_font = add_platform_font(atlas, 22.0F);
    struct nk_font *small_font = add_platform_font(atlas, 14.0F);
    nk_sdl_font_stash_end(*context_ptr);
    if (fallback_font != NULL) {
        nk_style_set_font(*context_ptr, &fallback_font->handle);
    }
    *font_ptr = fallback_font;
    if (fallback_font != NULL) {
        initialize_password_font(wrapper, fallback_font);
    }
    apply_style(*context_ptr);
    app->metal_renderer = false;
    app->renderer_fallback_used = true;
    /* A VideoToolbox session created for the Metal renderer outputs
     * NV12/P010 IOSurfaces, which the software renderer cannot wrap: the
     * CPU upload would misinterpret them as RGBA (green split screen).
     * Recreate the decoder so the next session requests BGRA output. */
    SDL_LockMutex(app->decoder_mutex);
    if (app->decoder != NULL &&
        app->decoder_pipeline == GRD_PIPELINE_METAL_VIDEOTOOLBOX) {
        grd_error decode_error = {0};
        grd_decoder_destroy(app->decoder);
        app->decoder = grd_decoder_create(
            app->selected_pipeline,
            app->decoder_codec,
            &app->decoder_pipeline,
            &decode_error
        );
        if (app->decoder != NULL) {
            grd_decoder_set_bgra_output(app->decoder);
        }
        app->vt_stall_since_micros = 0ULL;
        app->vt_stall_streak = 0U;
    }
    SDL_UnlockMutex(app->decoder_mutex);
    app->password_font =
        fallback_font != NULL ? &wrapper->font : (*context_ptr)->style.font;
    app->heading_font = heading_font != NULL
                            ? &heading_font->handle
                            : (*context_ptr)->style.font;
    app->small_font = small_font != NULL
                          ? &small_font->handle
                          : (*context_ptr)->style.font;
    (void)SDL_SetAtomicInt(&app->renderer_fallback_requested, 0);
    GRD_INFO("software renderer active after runtime fallback");
}

static bool handle_app_event(
    grd_app *app,
    struct nk_context *context,
    SDL_Event *event
)
{
    if (event->type == app->wake_event_type) {
        (void)SDL_SetAtomicInt(&app->wake_pending, 0);
        return true;
    }
    if (event->type == SDL_EVENT_QUIT ||
        event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        const bool remote_session_active =
            app->mode == GRD_APP_REMOTE && app->connection != NULL &&
            grd_connection_is_active(app->connection);
        if (remote_session_active) {
            /* Cocoa can enqueue the close request before SDL exposes the
             * Command+W key-down event, so a timestamp suppression window
             * was inherently racy. While controlling another computer all
             * shortcuts belong to the remote side; F1 -> Disconnect is the
             * explicit local escape hatch. This also makes a close-button
             * click harmless while keys are captured instead of sometimes
             * tearing down the session without releasing remote modifiers. */
            GRD_INFO(
                "client: ignored local close request during the remote session"
            );
            return true;
        }
        return false;
    }
    if (event->type == SDL_EVENT_WINDOW_DISPLAY_CHANGED ||
        event->type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
        /* Moving between a 60 Hz external display and a ProMotion panel
         * updates both local presentation pacing and the live FPS request
         * sent to the host. */
        grd_app_handle_display_change(app);
    }
    (void)nk_sdl_handle_event(context, event);
    grd_app_handle_remote_event(app, event);
    return true;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#if defined(__linux__)
    /* X11 is used by both capture and input threads. This must happen before
     * SDL or GRD opens a display connection. */
    (void)XInitThreads();
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    /* GRD is a remote-desktop frontend: while its keyboard is grabbed, SDL
     * must deliver system switch chords to the remote-event translator
     * instead of minimizing/switching the local application. */
    (void)SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");
    /* macOS normally consumes the first click used to focus an inactive
     * window. That prevented the remote-event path from seeing the click and
     * re-enabling relative mouse capture until the user clicked again. */
    (void)SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    /* Relative mode must expose raw mouse samples. Applying the macOS pointer
     * acceleration here and the numeric GRD sensitivity later would make
     * camera speed depend on how quickly the user moved the mouse. */
    (void)SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0");
    (void)SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE, "1.0");
    g_log_mutex = SDL_CreateMutex();
    if (grd_open_log_file()) {
        grd_log_set_sink(grd_file_log_sink, NULL);
    }
    GRD_INFO("GRD %s started", GRD_VERSION);
    SDL_Window *window = SDL_CreateWindow(
        "GRD — Remote Desktop LAN",
        1120,
        760,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    if (window == NULL) {
        fprintf(stderr, "Window: %s\n", SDL_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }
    (void)SDL_SetWindowMinimumSize(window, 940, 680);
    SDL_Renderer *renderer = NULL;
#if defined(__APPLE__)
    renderer = SDL_CreateRenderer(window, "metal");
#endif
    if (renderer == NULL) {
        renderer = SDL_CreateRenderer(window, NULL);
    }
    if (renderer == NULL) {
        fprintf(stderr, "Renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    GRD_INFO("renderer: %s", SDL_GetRendererName(renderer));
    /* Renderer self-test with automatic fallback: verifies that texture
     * upload actually works in this window/context. SDL3 renderers have been
     * observed failing SDL_UpdateTexture with an EMPTY error when the window
     * cannot obtain drawables; the app then retries with the other built-in
     * driver (metal <-> software) and logs which one passed. */
    {
        bool renderer_fell_back = false;
        static const char *const renderer_candidates[2] = {
            "metal", "software"
        };
        int chosen_index = 0;
        const char *current_name = SDL_GetRendererName(renderer);
        if (current_name != NULL && strcmp(current_name, "software") == 0) {
            chosen_index = 1;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            uint8_t self_test_pixels[16U * 16U * 4U];
            memset(self_test_pixels, 0x80, sizeof(self_test_pixels));
            SDL_Texture *self_test_texture = SDL_CreateTexture(
                renderer,
                SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_STREAMING,
                16,
                16
            );
            SDL_SetError("");
            /* SDL3 returns true on success (SDL2 returned zero). Keeping the
             * old SDL2 polarity here made a healthy Metal renderer look
             * broken and selected the software fallback at every launch. */
            const bool self_test_ok =
                self_test_texture != NULL &&
                SDL_UpdateTexture(
                    self_test_texture,
                    NULL,
                    self_test_pixels,
                    16 * 4
                );
            const char *self_test_err = SDL_GetError();
            GRD_INFO(
                "renderer %s self-test: update=%d err='%s'",
                SDL_GetRendererName(renderer),
                self_test_ok ? 1 : 0,
                self_test_err != NULL && self_test_err[0] != '\0'
                    ? self_test_err
                    : "(empty)"
            );
            if (self_test_texture != NULL) {
                SDL_DestroyTexture(self_test_texture);
            }
            if (self_test_ok || attempt == 1) {
                break;
            }
            renderer_fell_back = true;
            const int fallback_index = 1 - chosen_index;
            GRD_WARN(
                "renderer %s is not working: trying %s",
                SDL_GetRendererName(renderer),
                renderer_candidates[fallback_index]
            );
            SDL_DestroyRenderer(renderer);
            renderer = SDL_CreateRenderer(
                window, renderer_candidates[fallback_index]
            );
            if (renderer == NULL) {
                renderer = SDL_CreateRenderer(window, NULL);
            }
            if (renderer == NULL) {
                fprintf(stderr, "Renderer fallback: %s\n", SDL_GetError());
                SDL_DestroyWindow(window);
                SDL_Quit();
                return EXIT_FAILURE;
            }
            chosen_index = fallback_index;
        }
        /* Assigned to the app after grd_app_initialize. */
        g_renderer_fell_back = renderer_fell_back;
    }
    float display_scale = SDL_GetWindowDisplayScale(window);
    if (display_scale < 1.0F) {
        display_scale = 1.0F;
    }
    (void)SDL_SetRenderScale(renderer, display_scale, display_scale);
    /* Input latency is more important than a tear-free UI while a remote
     * session is active. Keep an explicit fallback for renderers that do not
     * expose an immediate-present mode. */
    const bool immediate_present = SDL_SetRenderVSync(renderer, 0);
    if (!immediate_present) {
        (void)SDL_SetRenderVSync(renderer, 1);
    }
    struct nk_context *context = nk_sdl_init(
        window, renderer, nk_sdl_allocator()
    );
    struct nk_font_atlas *atlas = nk_sdl_font_stash_begin(context);
    struct nk_font *ui_font = add_platform_font(atlas, 17.0F);
    struct nk_font *heading_font = add_platform_font(atlas, 22.0F);
    struct nk_font *small_font = add_platform_font(atlas, 14.0F);
    nk_sdl_font_stash_end(context);
    if (ui_font != NULL) {
        nk_style_set_font(context, &ui_font->handle);
    }
    password_font_wrapper password_font;
    if (ui_font != NULL) {
        initialize_password_font(&password_font, ui_font);
    }
    apply_style(context);

    grd_app app;
    if (!grd_app_initialize(&app)) {
        fprintf(stderr, "GRD: %s\n", app.last_error.message);
        grd_app_shutdown(&app);
        nk_sdl_shutdown(context);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    app.window = window;
    app.immediate_present = immediate_present;
    grd_app_configure_display(&app);
    const char *renderer_name = SDL_GetRendererName(renderer);
    app.metal_renderer = renderer_name != NULL &&
                         strcmp(renderer_name, "metal") == 0;
    app.renderer_fallback_used = g_renderer_fell_back;
    app.password_font =
        ui_font != NULL ? &password_font.font : context->style.font;
    app.heading_font = heading_font != NULL
                           ? &heading_font->handle
                           : context->style.font;
    app.small_font = small_font != NULL
                         ? &small_font->handle
                         : context->style.font;

    bool running = true;
    uint64_t next_present_deadline = 0U;
    uint64_t present_rate_started = grd_now_micros();
    uint32_t present_rate_count = 0U;
    uint64_t decoded_rate_at = atomic_load_explicit(
        &app.decoded_frames, memory_order_relaxed
    );
    bool skip_event_wait = false;
    while (running) {
        if (app.metal_renderer &&
            SDL_GetAtomicInt(&app.renderer_fallback_requested) != 0) {
            switch_to_software_renderer(
                window, &renderer, &context, &ui_font,
                &password_font, &app
            );
        }
        nk_input_begin(context);
        SDL_Event event;
        /* Input is event-driven.  A bounded timeout keeps discovery,
         * clipboard polling and connection liveness visible without spinning
         * the UI when no frame or user event arrived.  Video/cursor decoder
         * threads wake this loop with wake_event_type, so a 120 Hz stream does
         * not need a fixed polling timer. */
        if (skip_event_wait) {
            skip_event_wait = false;
            while (running && SDL_PollEvent(&event)) {
                running = handle_app_event(&app, context, &event);
            }
        } else if (SDL_WaitEventTimeout(&event, 500)) {
            running = handle_app_event(&app, context, &event);
            while (running && SDL_PollEvent(&event)) {
                running = handle_app_event(&app, context, &event);
            }
        }
        nk_input_end(context);

        if (!running) {
            continue;
        }
        grd_app_refresh_remote_texture(&app, renderer);
        grd_app_draw(&app, context, renderer);
        nk_sdl_update_TextInput(context);
        if (app.mode == GRD_APP_REMOTE) {
            (void)SDL_SetRenderDrawColor(renderer, 7U, 9U, 13U, 255U);
        } else {
            (void)SDL_SetRenderDrawColor(renderer, 245U, 245U, 247U, 255U);
        }
        (void)SDL_RenderClear(renderer);
        nk_sdl_render(context, NK_ANTI_ALIASING_ON);
        (void)SDL_RenderPresent(renderer);
        ++present_rate_count;
        const uint64_t present_rate_now = grd_now_micros();
        const uint64_t present_rate_elapsed =
            present_rate_now - present_rate_started;
        if (present_rate_elapsed >= 1000000ULL) {
            app.measured_present_rate =
                (float)((double)present_rate_count * 1000000.0 /
                        (double)present_rate_elapsed);
            const uint64_t decoded_now = atomic_load_explicit(
                &app.decoded_frames, memory_order_relaxed
            );
            const uint64_t decoded_delta = decoded_now - decoded_rate_at;
            uint64_t decoded_tenths =
                decoded_delta * 10000000ULL / present_rate_elapsed;
            if (decoded_tenths > UINT32_MAX) {
                decoded_tenths = UINT32_MAX;
            }
            atomic_store_explicit(
                &app.remote_receive_fps_tenths,
                (uint32_t)decoded_tenths,
                memory_order_relaxed
            );
            decoded_rate_at = decoded_now;
            present_rate_started = present_rate_now;
            present_rate_count = 0U;
        }
        /* Keep the immediate renderer on the same cadence as the local
         * panel. On a 120 Hz ProMotion display this is an 8.33 ms deadline;
         * input remains event-driven and is not sampled on a coarse timer. */
        if (app.immediate_present) {
            const uint32_t present_fps = app.display_target_fps >= 30U
                                             ? app.display_target_fps
                                             : 60U;
            const uint64_t present_interval = 1000000ULL / present_fps;
            const uint64_t present_started = grd_now_micros();
            if (next_present_deadline == 0U ||
                next_present_deadline + present_interval * 2U < present_started) {
                next_present_deadline = present_started + present_interval;
            } else {
                next_present_deadline += present_interval;
            }
            const uint64_t present_now = grd_now_micros();
            if (next_present_deadline > present_now) {
                if (app.mode == GRD_APP_REMOTE &&
                    app.relative_mouse_mode &&
                    !app.remote_settings_visible) {
                    /* Do not suspend the Cocoa event thread for an entire
                     * 120 Hz presentation interval. Wait interruptibly and
                     * forward each captured input sample as it arrives; the
                     * next render is still held to the display deadline. */
                    while (running && app.mode == GRD_APP_REMOTE &&
                           app.relative_mouse_mode &&
                           !app.remote_settings_visible) {
                        const uint64_t input_now = grd_now_micros();
                        if (input_now >= next_present_deadline) {
                            break;
                        }
                        const uint64_t remaining_us =
                            next_present_deadline - input_now;
                        int timeout_ms =
                            (int)((remaining_us + 999ULL) / 1000ULL);
                        if (timeout_ms < 1) {
                            timeout_ms = 1;
                        }
                        nk_input_begin(context);
                        SDL_Event paced_event;
                        if (SDL_WaitEventTimeout(&paced_event, timeout_ms)) {
                            running = handle_app_event(
                                &app, context, &paced_event
                            );
                            while (running && SDL_PollEvent(&paced_event)) {
                                running = handle_app_event(
                                    &app, context, &paced_event
                                );
                            }
                        }
                        nk_input_end(context);
                    }
                    /* Input processing above already waited until the next
                     * presentation (or changed local session state). */
                    skip_event_wait = true;
                } else {
                    SDL_DelayPrecise(
                        (next_present_deadline - present_now) * 1000ULL
                    );
                }
            }
        }
    }

    grd_app_shutdown(&app);
    nk_sdl_shutdown(context);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (g_log_file != NULL) {
        (void)fclose(g_log_file);
        g_log_file = NULL;
    }
    if (g_log_mutex != NULL) {
        SDL_DestroyMutex(g_log_mutex);
        g_log_mutex = NULL;
    }
    return EXIT_SUCCESS;
}
