/*
 * Dynamic lighting for the hi-res view (src/light.c).
 *
 * The level's own lamps, chandeliers, candles and fires (and explosions) are
 * point lights: each is recognised by the pixels of its sprite, placed at the
 * bright part of the sprite and baked into a light map on a 4-unit grid with
 * hard shadows from the walls (rebaked when a door near it moves). Flames
 * flicker; lamps are steady. A faint light around the player keeps dark
 * corners playable. Light is sampled at three heights (floor, eye, ceiling)
 * and the renderer scales every pixel by ambient + light.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void light_set_enabled(int on);
int light_enabled(void);
extern double light_ambient; /* 0..1, brightness with no light (default 0.35) */

/* A frame: light_begin, light_sprite for every object that could be drawn,
 * then light_end with the player's position. */
void light_begin(void);
/* Returns 1 if the sprite is a light source (draw it unshaded). key is its
 * file offset; pix its column-major palette indices. */
int light_sprite(uint32_t key, const uint8_t *pix, int w, int h, double x, double y, double raise);
void light_end(double px, double py);
void light_flush(void); /* sprite file changed */
/* The sprite frame is a light source (drawn unshaded). */
int light_is_source(uint32_t key, const uint8_t *pix, int w, int h);

/* Light at a point: layer 0 floor, 1 eye height, 2 ceiling, or all three. */
float light_at(double x, double y, int layer);
void light_at3(double x, double y, float out[3]);
/* n points from (x, y) in steps of (dx, dy), at one height */
void light_span(double x, double y, double dx, double dy, int n, int layer, float *out);

/* Per-channel scale (8.8 fixed point) for light L, and for the untextured
 * floor and ceiling a little light added too, or pools of light on a
 * near-black floor would not show. */
#define LIGHT_LUT_STEPS 64.0f
#define LIGHT_LUT_N 256
extern uint16_t light_lut[LIGHT_LUT_N][6]; /* r, g, b scale; r, g, b added */
static inline const uint16_t *light_scale(float L) {
    int i = (int)(L * LIGHT_LUT_STEPS + 0.5f);
    return light_lut[i < 0 ? 0 : i >= LIGHT_LUT_N ? LIGHT_LUT_N - 1 : i];
}
static inline uint32_t light_apply(uint32_t c, const uint16_t *f) {
    uint32_t r = ((c >> 16) & 255) * f[0] >> 8, g = ((c >> 8) & 255) * f[1] >> 8, b = (c & 255) * f[2] >> 8;
    return (c & 0xff000000u) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}
static inline uint32_t light_apply_flat(uint32_t c, const uint16_t *f) {
    uint32_t r = (((c >> 16) & 255) * f[0] >> 8) + f[3], g = (((c >> 8) & 255) * f[1] >> 8) + f[4], b = ((c & 255) * f[2] >> 8) + f[5];
    return (c & 0xff000000u) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}

#ifdef __cplusplus
}
#endif
