/* SDL_Renderer video backend (every platform; see src/video.h). */
#include <stdio.h>
#include <stdlib.h>

#include "video.h"

static SDL_Renderer *renderer;
static SDL_Texture *texture;

static int sdl_init(SDL_Window *window, int vsync) {
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, vsync ? "1" : "0");
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | (vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }
    if (getenv("N3D_DEBUG")) {
        SDL_RendererInfo ri;
        SDL_GetRendererInfo(renderer, &ri);
        SDL_DisplayMode dm;
        SDL_GetCurrentDisplayMode(0, &dm);
        fprintf(stderr, "renderer: %s vsync=%d display %dx%d@%dHz\n", ri.name, (ri.flags & SDL_RENDERER_PRESENTVSYNC) != 0, dm.w, dm.h, dm.refresh_rate);
    }
    return 0;
}

static void sdl_shutdown(void) {
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    texture = NULL;
    renderer = NULL;
}

static int sdl_output_size(int *w, int *h) { return SDL_GetRendererOutputSize(renderer, w, h); }

static int sdl_set_source(int w, int h, int logical_w, int logical_h) {
    if (texture) SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
#if SDL_VERSION_ATLEAST(2, 0, 12)
    /* a hi-res frame drawn smaller than the window (--render-scale) is
     * enlarged smoothly; the 320x200 frame keeps its sharp pixels */
    if (texture && logical_w == w && logical_h == h) SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
#endif
    SDL_RenderSetLogicalSize(renderer, logical_w, logical_h);
    return texture ? 0 : -1;
}

static uint32_t *sdl_lock(int *pitch) {
    void *pixels;
    int bytes;
    if (!texture || SDL_LockTexture(texture, NULL, &pixels, &bytes) != 0) return NULL;
    *pitch = bytes / 4;
    return pixels;
}

static void sdl_unlock(void) { SDL_UnlockTexture(texture); }

static void sdl_present(void) {
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

static void sdl_window_to_logical(int wx, int wy, float *lx, float *ly) { SDL_RenderWindowToLogical(renderer, wx, wy, lx, ly); }

const video_backend video_sdl = {
    "sdl", sdl_init, sdl_shutdown, sdl_output_size, sdl_set_source, sdl_lock, sdl_unlock, sdl_present, sdl_window_to_logical,
};
