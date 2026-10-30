/*
 * Puts the finished XRGB8888 frame on screen (src/video_sdl.c, src/video_d3d12.c).
 *
 * The game and the hi-res renderer draw on the CPU; a backend only owns the
 * window's swap chain: it hands out a frame buffer of the source size, then
 * scales it into the window, letterboxed to the logical size's aspect (like
 * SDL_RenderSetLogicalSize).
 */
#pragma once
#include <stdint.h>
#include <SDL.h>

typedef struct video_backend {
    const char *name;
    int (*init)(SDL_Window *window, int vsync); /* 0 on success */
    void (*shutdown)(void);
    /* Window drawable size in pixels. */
    int (*output_size)(int *w, int *h);
    /* Frame buffer size and the aspect box it is shown in. */
    int (*set_source)(int w, int h, int logical_w, int logical_h);
    /* Frame buffer to draw into; pitch in pixels. NULL on failure. */
    uint32_t *(*lock)(int *pitch);
    void (*unlock)(void);
    void (*present)(void);
    /* Window coordinates (SDL_GetMouseState) to logical coordinates. */
    void (*window_to_logical)(int wx, int wy, float *lx, float *ly);
} video_backend;

extern const video_backend video_sdl;
