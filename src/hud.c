/*
 * Gothic full-screen HUD (see hud.h).
 *
 * Game state used (as FUN_1010_a308, the status bar painter, reads it):
 *   DS:4c0d health %          DS:4c06 score (u32)
 *   DS:4c13 weapon (0xff none): ammo 0, 3 -> DS:4c10, 1 -> DS:4c34, 2 -> DS:4c0f
 *   DS:4c18 keys (bits 0..3, pictures DS:378c + i*10)
 *   DS:4c19 items (bits 0..7, pictures DS:37b4 + i*10)
 *   DS:4c32 / DS:4c33 two timed powers (0..100)
 *   DS:37e6 + ((health + 9) / 10) * 10  portrait picture
 * Picture entries are {width, height, file offset, far data}, the data is
 * column-major with 0x29 transparent.
 */
#include "hud.h"
#include "hd_tex.h"
#include "rt/rt.h"
#include "rt/host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint8_t ds8(uint16_t o) { return MEMR8(SEGLIN(RT_SEL_DS, o)); }
static inline uint16_t dsu16(uint16_t o) { return MEMR16(SEGLIN(RT_SEL_DS, o)); }
static inline uint32_t dsu32(uint16_t o) { return MEMR32(SEGLIN(RT_SEL_DS, o)); }

/* ---------------------------------------------------------------- art */
typedef struct {
    char name[25];
    int w, h, aux[4];
    uint32_t *px; /* premultiplied ARGB, row-major */
} img_t;

static img_t *imgs;
static int nimgs, enabled = 1;
static const img_t *glyph[128];

enum { PORTRAIT, PORTRAIT_MASK, HEALTH_FRAME, HEALTH_FILL, ORN_L, ORN_R,
       AMMO_BULLET, AMMO_SILVER, AMMO_SILVERS, SPELL_BOOK,
       KEY_GOLD, KEY_SILVER, KEY_RED, KEY_GREEN, CARD_RED, CARD_YELLOW, NSPRITE };
static const char *sprite_names[NSPRITE] = {
    "portrait-frame", "portrait-mask", "health-frame", "health-fill", "ammo-ornament-left", "ammo-ornament-right",
    "ammo-bullet", "ammo-silver-bullet", "ammo-silver-bullets", "spell-book",
    "key-gold", "key-silver", "key-red", "key-green", "keycard-red", "keycard-yellow"};
static const img_t *spr[NSPRITE];

int hud_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[8];
    uint32_t n = 0;
    int ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "N3DHUD1\n", 8) && fread(&n, 4, 1, f) == 1 && n < 4096;
    if (ok) imgs = calloc(n, sizeof *imgs);
    for (uint32_t i = 0; ok && i < n; i++) {
        uint8_t hdr[36];
        img_t *m = &imgs[i];
        if (fread(hdr, 1, 36, f) != 36) { ok = 0; break; }
        memcpy(m->name, hdr, 24);
        m->w = hdr[24] | hdr[25] << 8;
        m->h = hdr[26] | hdr[27] << 8;
        for (int k = 0; k < 4; k++) m->aux[k] = (int16_t)(hdr[28 + 2 * k] | hdr[29 + 2 * k] << 8);
        size_t np = (size_t)m->w * m->h;
        m->px = malloc(np * 4 + 4);
        if (np && fread(m->px, 4, np, f) != np) { ok = 0; break; }
        nimgs++;
        if (m->name[0] == 'g' && m->aux[3] > 0 && m->aux[3] < 128) glyph[m->aux[3]] = m;
        for (int k = 0; k < NSPRITE; k++) if (!strcmp(m->name, sprite_names[k])) spr[k] = m;
    }
    fclose(f);
    for (int k = 0; k < NSPRITE; k++) ok = ok && spr[k];
    if (!ok || !glyph['0'] || !glyph['0']->h) {
        fprintf(stderr, "hud: %s: invalid, using the classic status bar\n", path);
        nimgs = 0;
        return 0;
    }
    return 1;
}

void hud_set_enabled(int on) { enabled = on; }
int hud_active(void) { return enabled && nimgs > 0; }

/* ---------------------------------------------------------------- scaling */
/* Area-average resample of premultiplied pixels (separable, exact coverage). */
static void resample(const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh) {
    float *tmp = malloc(sizeof(float) * 4 * (size_t)dw * sh);
    double kx = (double)sw / dw, ky = (double)sh / dh;
    for (int x = 0; x < dw; x++) {
        double a = x * kx, b = a + kx;
        for (int y = 0; y < sh; y++) {
            float acc[4] = {0, 0, 0, 0};
            for (int s = (int)a; s < sw && s < b; s++) {
                double w = (s + 1 < b ? s + 1 : b) - (s > a ? s : a);
                uint32_t c = src[(size_t)y * sw + s];
                acc[0] += (float)(w * (c >> 24)); acc[1] += (float)(w * ((c >> 16) & 255));
                acc[2] += (float)(w * ((c >> 8) & 255)); acc[3] += (float)(w * (c & 255));
            }
            float *t = tmp + ((size_t)y * dw + x) * 4;
            for (int k = 0; k < 4; k++) t[k] = acc[k] / (float)kx;
        }
    }
    for (int y = 0; y < dh; y++) {
        double a = y * ky, b = a + ky;
        for (int x = 0; x < dw; x++) {
            float acc[4] = {0, 0, 0, 0};
            for (int s = (int)a; s < sh && s < b; s++) {
                float w = (float)((s + 1 < b ? s + 1 : b) - (s > a ? s : a));
                const float *t = tmp + ((size_t)s * dw + x) * 4;
                for (int k = 0; k < 4; k++) acc[k] += w * t[k];
            }
            uint32_t c = 0;
            for (int k = 0; k < 4; k++) {
                int v = (int)lroundf(acc[k] / (float)ky);
                c = c << 8 | (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
            dst[(size_t)y * dw + x] = c;
        }
    }
    free(tmp);
}

typedef struct { const img_t *src; int w, h; uint32_t *px; } scaled_t;
static scaled_t cache[192];
static int ncache, cache_out_h;

static const scaled_t *scaled(const img_t *m, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    for (int i = 0; i < ncache; i++)
        if (cache[i].src == m && cache[i].w == w && cache[i].h == h) return &cache[i];
    if (ncache == (int)(sizeof cache / sizeof *cache)) { /* full: start over */
        for (int i = 0; i < ncache; i++) free(cache[i].px);
        ncache = 0;
    }
    scaled_t *s = &cache[ncache++];
    s->src = m; s->w = w; s->h = h;
    s->px = malloc(sizeof(uint32_t) * (size_t)w * h);
    resample(m->px, m->w, m->h, s->px, w, h);
    return s;
}

/* ---------------------------------------------------------------- drawing */
static uint32_t *O;
static int OP, OW, OH;

static int shadow; /* draw_clip: draw the shape in translucent black */

static inline void blend(uint32_t *d, uint32_t c, int alpha /* 0..256 */) {
    if (shadow) { alpha = alpha * 3 / 4; c &= 0xff000000u; }
    uint32_t a = (c >> 24) * (uint32_t)alpha >> 8;
    if (!a) return;
    uint32_t ia = 255 - a, s = *d;
    uint32_t r = (((c >> 16) & 255) * (uint32_t)alpha >> 8) + ((s >> 16) & 255) * ia / 255;
    uint32_t g = (((c >> 8) & 255) * (uint32_t)alpha >> 8) + ((s >> 8) & 255) * ia / 255;
    uint32_t b = ((c & 255) * (uint32_t)alpha >> 8) + (s & 255) * ia / 255;
    *d = 0xff000000u | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

/* m scaled to w x h at (x, y); only its columns [0, clip_w) are drawn */
static void draw_clip(const img_t *m, double x, double y, double w, double h, int clip_w, int alpha) {
    const scaled_t *s = scaled(m, (int)lround(w), (int)lround(h));
    int x0 = (int)lround(x), y0 = (int)lround(y);
    if (clip_w > s->w) clip_w = s->w;
    for (int r = 0; r < s->h; r++) {
        int oy = y0 + r;
        if (oy < 0 || oy >= OH) continue;
        uint32_t *row = O + (size_t)oy * OP;
        const uint32_t *src = s->px + (size_t)r * s->w;
        for (int c = 0; c < clip_w; c++) {
            int ox = x0 + c;
            if (ox >= 0 && ox < OW) blend(row + ox, src[c], alpha);
        }
    }
}
static void draw(const img_t *m, double x, double y, double w, double h) { draw_clip(m, x, y, w, h, 1 << 30, 256); }
/* by height, keeping the aspect ratio; returns the width */
static double draw_h(const img_t *m, double x, double y, double h) {
    double w = h * m->w / m->h;
    draw(m, x, y, w, h);
    return w;
}

/* Glyphs are cropped to their ink: they sit on the baseline, scaled so a
 * digit is `size` pixels high and spaced by their width plus a small gap;
 * descenders hang from the x-height. */
static const img_t *glyph_of(char c) { return (unsigned char)c < 128 && glyph[(unsigned char)c] && glyph[(unsigned char)c]->w ? glyph[(unsigned char)c] : NULL; }

/* punctuation the font lacks, drawn as blocks */
static int drawn_mark(char c) { return c == '.' || c == ':' || c == '-'; }

static double text_width(const char *s, double size) {
    double k = size / glyph['0']->h, w = 0;
    for (; *s; s++) {
        const img_t *g = glyph_of(*s);
        w += g ? g->w * k + size * 0.08 : drawn_mark(*s) ? size * (*s == '-' ? 0.5 : 0.3) : size * 0.35;
    }
    return w;
}

static void fill(double x, double y, double w, double h, uint32_t c);

/* size = digit height in pixels; y = baseline */
static void text(const char *s, double x, double y, double size, int align /* -1 left, 0 centre, 1 right */) {
    double k = size / glyph['0']->h;
    double xh = glyph_of('x') ? glyph_of('x')->h * k : size * 0.7;
    if (align > 0) x -= text_width(s, size);
    if (align == 0) x -= text_width(s, size) * 0.5;
    for (; *s; s++) {
        const img_t *g = glyph_of(*s);
        if (!g && drawn_mark(*s)) {
            double d = size * 0.13, w = *s == '-' ? size * 0.36 : d;
            double ys[2] = {*s == '-' ? y - size * 0.45 : y - d, y - size * 0.55};
            for (int k2 = 0; k2 < (*s == ':' ? 2 : 1); k2++) {
                fill(x + size * 0.04 + size * 0.05, ys[k2] + size * 0.05, w, d, 0xa0000000u);
                fill(x + size * 0.04, ys[k2], w, d, 0xffe8dcc0u);
            }
            x += *s == '-' ? size * 0.5 : size * 0.3;
            continue;
        }
        if (!g) { x += size * 0.35; continue; }
        double gw = g->w * k, gh = g->h * k;
        double gy = strchr("gjpqy", *s) ? y - xh : y - gh;
        /* dark drop shadow keeps the ivory text readable over bright walls */
        shadow = 1;
        draw(g, x + size * 0.04, gy + size * 0.05, gw, gh);
        shadow = 0;
        draw(g, x, gy, gw, gh);
        x += gw + size * 0.08;
    }
}

/* filled rectangle, premultiplied colour */
static void fill(double x, double y, double w, double h, uint32_t c) {
    int x0 = (int)lround(x), y0 = (int)lround(y), x1 = (int)lround(x + w), y1 = (int)lround(y + h);
    for (int r = y0 < 0 ? 0 : y0; r < y1 && r < OH; r++)
        for (int q = x0 < 0 ? 0 : x0; q < x1 && q < OW; q++) blend(O + (size_t)r * OP + q, c, 256);
}

/* One of the game's own pictures (column-major indices) filtered into the
 * rectangle; mask (scaled to the same rectangle offset by mx, my) limits it. */
static void draw_picture(uint16_t entry, double x, double y, double w, double h, const scaled_t *mask, int mx, int my) {
    int pw = ds8(entry), ph = ds8((uint16_t)(entry + 1));
    uint32_t fp = dsu32((uint16_t)(entry + 6));
    if (pw <= 0 || ph <= 0 || !fp) return;
    static uint8_t buf[256 * 256];
    for (int k = 0; k < pw * ph; k++) buf[k] = MEMR8(SEGLIN(fp >> 16, (uint16_t)((fp & 0xffff) + k)));
    const hq_tex *t = hq_get(dsu32((uint16_t)(entry + 2)) ^ 0x48554400u, buf, pw, ph, 1, NULL);
    if (!t) return;
    hq_build_pending();
    int x0 = (int)lround(x), y0 = (int)lround(y), x1 = (int)lround(x + w), y1 = (int)lround(y + h);
    float fu = (float)(pw / w), fv = (float)(ph / h);
    for (int r = y0 < 0 ? 0 : y0; r < y1 && r < OH; r++)
        for (int q = x0 < 0 ? 0 : x0; q < x1 && q < OW; q++) {
            int alpha = 256;
            if (mask) {
                int u = q - mx, v = r - my;
                if (u < 0 || v < 0 || u >= mask->w || v >= mask->h) continue;
                alpha = (int)(mask->px[(size_t)v * mask->w + u] >> 24) + 1;
            }
            uint32_t c = hq_sample(t, (float)((q + 0.5 - x) * fu), (float)((r + 0.5 - y) * fv), fu, fv);
            blend(O + (size_t)r * OP + q, c, alpha);
        }
}

/* ---------------------------------------------------------------- layout */
static double now_s(void) { return host_time_us() / 1e6; }

void hud_draw(uint32_t *out, int pitch, int w, int h) {
    if (!hud_active()) return;
    O = out; OP = pitch; OW = w; OH = h;
    if (cache_out_h != h) { /* sizes change with the output: drop the old scalings */
        for (int i = 0; i < ncache; i++) free(cache[i].px);
        ncache = 0;
        cache_out_h = h;
    }
    double u = h / 1080.0;
    if (w / 1440.0 < u) u = w / 1440.0;
    double m = 28 * u;

    int health = ds8(0x4c0d);
    if (health > 100) health = 100;

    /* portrait: the game's face behind the frame's opening */
    const img_t *pf = spr[PORTRAIT];
    double ph = 250 * u, pw = ph * pf->w / pf->h, ps = ph / pf->h;
    double px = m, py = h - m - ph;
    {
        const scaled_t *mask = scaled(spr[PORTRAIT_MASK], (int)lround(pw), (int)lround(ph));
        int mx = (int)lround(px), my = (int)lround(py);
        double hx0 = px + pf->aux[0] * ps, hy0 = py + pf->aux[1] * ps;
        double hw = (pf->aux[2] - pf->aux[0]) * ps, hh = (pf->aux[3] - pf->aux[1]) * ps;
        for (int r = 0; r < mask->h; r++) /* dark backing */
            for (int q = 0; q < mask->w; q++) {
                int ox = mx + q, oy = my + r;
                if (ox >= 0 && ox < w && oy >= 0 && oy < h) blend(out + (size_t)oy * pitch + ox, 0xff0a0608u, (int)(mask->px[(size_t)r * mask->w + q] >> 24) + 1);
            }
        uint16_t pe = (uint16_t)(0x37e6 + ((health + 9) / 10) * 10);
        int fw = ds8(pe), fh = ds8((uint16_t)(pe + 1));
        if (fw > 0 && fh > 0) {
            /* cover the opening, original 4:3 pixel aspect (1 x 1.2) */
            double s = hw / fw, s2 = hh / (fh * 1.2);
            if (s2 > s) s = s2;
            double dw = fw * s, dh = fh * 1.2 * s;
            draw_picture(pe, hx0 + (hw - dw) * 0.5, hy0 + (hh - dh) * 0.5, dw, dh, mask, mx, my);
        }
        draw(pf, px, py, pw, ph);
    }

    /* health bar right of the portrait */
    const img_t *hf = spr[HEALTH_FRAME], *hl = spr[HEALTH_FILL];
    double bw = 430 * u, bs = bw / hf->w, bh = hf->h * bs;
    double bx = px + pw - 6 * u, by = py + ph * 0.80 - bh * 0.5;
    {
        double fw = hl->w * bs, fh = hl->h * bs;
        double fx = bx + (hf->w - hl->w) * 0.5 * bs, fy = by + (hf->h - hl->h) * 0.5 * bs;
        int alpha = 256;
        if (health <= 25) alpha = 150 + (int)(106 * (0.5 + 0.5 * sin(now_s() * 6.0)));
        draw_clip(hl, fx, fy, fw, fh, (int)lround(fw * health / 100.0), alpha);
        draw(hf, bx, by, bw, bh);
        char s[16];
        snprintf(s, sizeof s, "%d%%", health);
        text(s, fx, by - 6 * u, 44 * u, -1);
        /* timed powers: thin bars under the health bar while they last */
        int p1 = ds8(0x4c33), p2 = ds8(0x4c32);
        double ty = by + bh + 2 * u;
        if (p1 > 0 && p1 <= 100) { fill(fx, ty, fw, 7 * u, 0xb0000000u); fill(fx, ty, fw * p1 / 100.0, 7 * u, 0xffc89b3cu); ty += 11 * u; }
        if (p2 > 0 && p2 <= 100) { fill(fx, ty, fw, 7 * u, 0xb0000000u); fill(fx, ty, fw * p2 / 100.0, 7 * u, 0xff4a8ed8u); }
        /* score, right end of the bar */
        snprintf(s, sizeof s, "%lu", (unsigned long)dsu32(0x4c06));
        text(s, fx + fw, by - 6 * u, 30 * u, 1);
    }

    /* items the player carries: the game's own pictures above the health text */
    {
        int items = ds8(0x4c19);
        double isz = 46 * u, ix = bx + 70 * u, iy = by - 60 * u - isz;
        for (int i = 0; i < 8; i++) {
            if (!(items & 1 << i)) continue;
            uint16_t e = (uint16_t)(0x37b4 + i * 10);
            int iw = ds8(e), ih = ds8((uint16_t)(e + 1));
            if (iw <= 0 || ih <= 0) continue;
            double dw = isz * iw / (ih * 1.2);
            draw_picture(e, ix, iy, dw, isz, NULL, 0, 0);
            ix += dw + 8 * u;
        }
    }

    /* ammo of the current weapon, bottom right: ornament, icon, count, ornament */
    double oh = 170 * u;
    double cy = h - m - oh * 0.5;
    double ammo_top = h - m;
    int weapon = ds8(0x4c13);
    if (weapon != 0xff) {
        int ammo = weapon == 1 ? ds8(0x4c34) : weapon == 2 ? ds8(0x4c0f) : ds8(0x4c10);
        const img_t *icon = weapon == 1 ? spr[SPELL_BOOK] : weapon == 2 ? spr[AMMO_SILVERS] : spr[AMMO_BULLET];
        char s[16];
        snprintf(s, sizeof s, "%d", ammo);
        double tsz = 68 * u;
        const img_t *ol = spr[ORN_L], *orr = spr[ORN_R];
        double orw = oh * orr->w / orr->h, olw = oh * ol->w / ol->h;
        double x = w - m - orw;
        draw(orr, x, cy - oh * 0.5, orw, oh);
        double tw = text_width("000", tsz);
        double tx = x - 4 * u - tw;
        text(s, tx + tw * 0.5, cy + tsz * 0.5, tsz, 0);
        double ih = icon == spr[SPELL_BOOK] ? 88 * u : 100 * u;
        double iw = ih * icon->w / icon->h;
        double ix = tx - 10 * u - iw;
        draw(icon, ix, cy - ih * 0.5, iw, ih);
        draw(ol, ix - 4 * u - olw, cy - oh * 0.5, olw, oh);
        ammo_top = cy - oh * 0.5;
    }

    /* keys above the ammo */
    {
        static const int key_spr[4] = {KEY_GOLD, KEY_SILVER, KEY_RED, KEY_GREEN};
        int keys = ds8(0x4c18);
        double kh = 78 * u, x = w - m;
        for (int i = 3; i >= 0; i--) {
            if (!(keys & 1 << i)) continue;
            const img_t *k = spr[key_spr[i]];
            double kw = kh * k->w / k->h;
            x -= kw;
            draw(k, x, ammo_top - 12 * u - kh, kw, kh);
            x -= 12 * u;
        }
    }
}

void hud_status(uint32_t *out, int pitch, int w, int h, const char *const *lines, int n) {
    if (n <= 0 || !glyph['0'] || !glyph['0']->h) return;
    O = out; OP = pitch; OW = w; OH = h;
    double u = h / 1080.0;
    if (w / 1440.0 < u) u = w / 1440.0;
    double size = 26 * u, lh = size * 1.6, m = 24 * u, pad = size * 0.6;
    if (size < 9) { size = 9; lh = size * 1.6; pad = size * 0.6; }
    double tw = 0;
    for (int i = 0; i < n; i++) { double x = text_width(lines[i], size); if (x > tw) tw = x; }
    fill(m, m, tw + 2 * pad, n * lh + pad, 0x99000000u);
    for (int i = 0; i < n; i++) text(lines[i], m + pad, m + pad * 0.5 + (i + 1) * lh - lh * 0.3, size, -1);
}
