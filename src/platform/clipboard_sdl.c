#include "grd/platform.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *grd_platform_clipboard_read(void)
{
    char *sdl_text = SDL_GetClipboardText();
    if (sdl_text == NULL) {
        return NULL;
    }
    const size_t length = strlen(sdl_text);
    char *copy = malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, sdl_text, length + 1U);
    }
    SDL_free(sdl_text);
    return copy;
}

grd_status grd_platform_clipboard_write(const char *text, grd_error *error)
{
    if (text == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    if (!SDL_SetClipboardText(text)) {
        if (error != NULL) {
            error->code = GRD_IO_ERROR;
            (void)snprintf(
                error->message,
                sizeof(error->message),
                "Clipboard: %s",
                SDL_GetError()
            );
        }
        return GRD_IO_ERROR;
    }
    return GRD_OK;
}

