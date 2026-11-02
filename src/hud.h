/*
 * Gothic full-screen HUD for the hi-res view (src/hud.c).
 *
 * With the view at its largest size, the 3D view fills the whole output and
 * the original 320x200 status bar is replaced by art from hud.pak (made from
 * assets/hud by tools/hud_pack.py): the player's portrait in a gothic frame,
 * a health bar, ammo of the current weapon, keys and items. Values are read
 * from the game's own memory every frame; the portrait and item pictures are
 * the game's own, filtered like the other hi-res art.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load the art; returns 0 if the file is missing or invalid (classic HUD). */
int hud_load(const char *path);
void hud_set_enabled(int on);
int hud_active(void); /* loaded and enabled */

/* Draw over out (w*h XRGB pixels, pitch in pixels). */
void hud_draw(uint32_t *out, int pitch, int w, int h);
/* A few lines of status text in the top left corner on a dark panel (co-op);
 * needs the HUD font, not the gothic HUD itself. */
void hud_status(uint32_t *out, int pitch, int w, int h, const char *const *lines, int n);

#ifdef __cplusplus
}
#endif
