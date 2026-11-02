/*
 * Dynamic lighting for the hi-res view (see light.h).
 *
 * Light sources are recognised by an FNV-1a hash of their sprite frame's
 * palette indices (the same art is shared by all three episodes). Each light
 * gets a window of the level grid (CELL units per cell, out to its radius)
 * holding intensity x falloff x visibility at three heights; visibility is a
 * 2D test of the segment light -> cell centre against every wall vector near
 * it. Walls lie on multiples of CELL, so a wall's front row of cell centres
 * is exactly CELL/2 in front of it: walls sample that row only, and light
 * never leaks through to the far side. Steady lights are summed into a
 * static map once; flickering ones are added on top every frame.
 */
#include "light.h"
#include "hd_tex.h"
#include "rt/rt.h"
#include "rt/host.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SEL_WORLD 0x1040
#define TRANSPARENT 0x29
#define CELL 4
#define PLAYER_RADIUS 192.0
#define PLAYER_LIGHT 0.22
#define MAX_LIGHTS 256

static int enabled = 1;
double light_ambient = 0.35;
void light_set_enabled(int on) { enabled = on; }
int light_enabled(void) { return enabled; }

static inline int16_t ds16(uint16_t o) { return (int16_t)MEMR16(SEGLIN(RT_SEL_DS, o)); }

/* ---------------------------------------------------------------- sources */
enum { K_CEILING_LAMP, K_CHANDELIER, K_FLOOR_LAMP, K_GLOBE_LAMP, K_CANDLE, K_FIRE, K_EXPLOSION, NKIND };
static const struct { float radius, intensity, flicker; } kinds[NKIND] = {
    [K_CEILING_LAMP] = {288, 1.10f, 0},
    [K_CHANDELIER] = {352, 1.40f, 0},
    [K_FLOOR_LAMP] = {256, 1.00f, 0},
    [K_GLOBE_LAMP] = {256, 1.00f, 0},
    [K_CANDLE] = {224, 1.10f, 0.15f},
    [K_FIRE] = {256, 1.20f, 0.30f},
    [K_EXPLOSION] = {256, 1.00f, 0.10f},
};
static const struct { uint32_t hash; uint8_t kind; } source_frames[] = {
    {0x891ef615, K_CEILING_LAMP}, {0x2852762a, K_CHANDELIER}, {0xecf8bdca, K_FLOOR_LAMP}, {0x5ed94ac7, K_GLOBE_LAMP},
    {0xd0e1d44f, K_CANDLE}, {0xfeda62d5, K_CANDLE}, {0x9ed8ea59, K_CANDLE}, {0xa30bad88, K_CANDLE},
    {0xdca75380, K_CANDLE}, {0xd6f6d75e, K_CANDLE}, {0x8484774e, K_CANDLE}, {0x553b6ed8, K_CANDLE},
    {0xa4fadcde, K_FIRE}, {0xe73315f7, K_FIRE}, {0x890c50aa, K_FIRE}, {0x34a1e6a6, K_FIRE},
    {0x679c1926, K_FIRE}, {0xbefcbbf5, K_FIRE}, {0xf87c9c4b, K_FIRE}, {0xd5b57f46, K_FIRE},
    {0x15d02279, K_FIRE}, {0x96a38987, K_FIRE}, {0x1e10fd25, K_FIRE}, {0x6f5349e4, K_FIRE},
    {0xd521706d, K_FIRE}, {0xd4186419, K_FIRE}, {0x72513800, K_FIRE}, {0xf330fd05, K_FIRE},
    {0x91b8b32b, K_EXPLOSION}, {0x713ece0e, K_EXPLOSION}, {0xf7d6fc1a, K_EXPLOSION},
    {0x3f8db43d, K_EXPLOSION}, {0x1d073ddd, K_EXPLOSION}, {0x33cf5b0f, K_EXPLOSION},
};

/* sprite frames seen (by file offset): kind or -1, and the light's height above the sprite's bottom */
#define FRAME_SLOTS 2048
static struct { uint32_t key; uint8_t used; int8_t kind; float z; } frame_info[FRAME_SLOTS];

void light_flush(void) { memset(frame_info, 0, sizeof frame_info); }

static int frame_kind(uint32_t key, const uint8_t *pix, int w, int h, float *z) {
    uint32_t s = (key * 2654435761u) >> 21;
    for (int probe = 0; probe < FRAME_SLOTS; probe++) {
        int k = (int)((s + probe) & (FRAME_SLOTS - 1));
        if (frame_info[k].used && frame_info[k].key == key) { *z = frame_info[k].z; return frame_info[k].kind; }
        if (frame_info[k].used) continue;
        uint32_t hash = 2166136261u;
        for (int i = 0; i < w * h; i++) hash = (hash ^ pix[i]) * 16777619u;
        int kind = -1;
        for (size_t i = 0; i < sizeof source_frames / sizeof *source_frames; i++)
            if (source_frames[i].hash == hash) kind = source_frames[i].kind;
        /* the light sits at the bright part of the picture (flame, glowing shade) */
        double sum = 0, n = 0;
        if (kind >= 0)
            for (int i = 0; i < w * h; i++) {
                if (pix[i] == TRANSPARENT) continue;
                uint32_t c = hq_palette_color(pix[i]);
                int lum = (int)(((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10;
                if (lum >= 190) { sum += h - (i % h) - 0.5; n++; }
            }
        frame_info[k].key = key;
        frame_info[k].used = 1;
        frame_info[k].kind = (int8_t)kind;
        frame_info[k].z = (float)(n ? sum / n : h * 0.8);
        *z = frame_info[k].z;
        return kind;
    }
    light_flush();
    return frame_kind(key, pix, w, h, z);
}

int light_is_source(uint32_t key, const uint8_t *pix, int w, int h) {
    float z;
    return frame_kind(key, pix, w, h, &z) >= 0;
}

/* ---------------------------------------------------------------- level */
typedef struct { float x1, y1, x2, y2; } seg_t;
static seg_t *segs;
static int nseg, seg_cap;

static int gx0, gy0, gw, gh; /* grid origin (world units) and size (cells) */
static float *smap, *cmap;   /* steady lights; everything: 3 heights per cell */

typedef struct {
    int used, seen, kind, version;
    float x, y, z, seed;
    int cx0, cy0, w, h; /* window (cells) */
    float *val;         /* w*h*3 */
    uint32_t sig;       /* the walls it was baked with */
} light_t;
static light_t lights[MAX_LIGHTS];
static int active[MAX_LIGHTS], nactive;
static int frame_no;
static uint32_t static_built; /* signature of the steady lights in smap */
static int dirty[MAX_LIGHTS][4], ndirty; /* rectangles of cmap holding flickering light */
static double ppx, ppy;
static double lut_ambient = -1; /* light_ambient that light_lut was built with */
static void build_lut(void);
static int have_player;

void light_begin(void) {
    nactive = 0;
    frame_no++;
    have_player = 0;
    int n = ds16(0x7e46);
    if (n < 0) n = 0;
    if (n > seg_cap) { seg_cap = n + 64; segs = realloc(segs, sizeof *segs * (size_t)seg_cap); }
    nseg = 0;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int i = 0; i < n; i++) {
        uint32_t v = SEGLIN(SEL_WORLD, 6 + i * 0x1c);
        if (!(MEMR8(v + 5) & 1)) continue;
        seg_t *s = &segs[nseg++];
        s->x1 = (int16_t)MEMR16(v + 0xc); s->y1 = (int16_t)MEMR16(v + 0xe);
        s->x2 = (int16_t)MEMR16(v + 0x10); s->y2 = (int16_t)MEMR16(v + 0x12);
        int ax = (int)fminf(s->x1, s->x2), bx = (int)fmaxf(s->x1, s->x2), ay = (int)fminf(s->y1, s->y2), by = (int)fmaxf(s->y1, s->y2);
        if (ax < x0) x0 = ax;
        if (bx > x1) x1 = bx;
        if (ay < y0) y0 = ay;
        if (by > y1) y1 = by;
    }
    if (!nseg) return;
    /* grid over the level (+ a margin), origin on a multiple of CELL */
    x0 = (int)floor((x0 - 64) / (double)CELL) * CELL;
    y0 = (int)floor((y0 - 64) / (double)CELL) * CELL;
    int w = (x1 + 64 - x0) / CELL + 1, h = (y1 + 64 - y0) / CELL + 1;
    if (x0 < gx0 || y0 < gy0 || x0 + w * CELL > gx0 + gw * CELL || y0 + h * CELL > gy0 + gh * CELL || !cmap ||
        (size_t)w * h * 4 < (size_t)gw * gh) { /* new level */
        for (int i = 0; i < MAX_LIGHTS; i++) { free(lights[i].val); memset(&lights[i], 0, sizeof lights[i]); }
        free(smap); free(cmap);
        gx0 = x0; gy0 = y0; gw = w; gh = h;
        smap = calloc((size_t)gw * gh * 3, sizeof *smap);
        cmap = calloc((size_t)gw * gh * 3, sizeof *cmap);
        static_built = 0;
        ndirty = 0;
    }
}

int light_sprite(uint32_t key, const uint8_t *pix, int w, int h, double x, double y, double raise) {
    float zo;
    int kind = frame_kind(key, pix, w, h, &zo);
    if (kind < 0) return 0;
    if (!enabled || !cmap || nactive >= MAX_LIGHTS) return 1;
    float z = (float)(raise + zo);
    int free_slot = -1, oldest = -1;
    for (int i = 0; i < MAX_LIGHTS; i++) {
        light_t *l = &lights[i];
        if (!l->used) { if (free_slot < 0) free_slot = i; continue; }
        /* animated frames move the bright part a little: same light */
        if (l->kind == kind && fabsf(l->x - (float)x) < 1 && fabsf(l->y - (float)y) < 1 && fabsf(l->z - z) < 12) {
            if (l->seen != frame_no) { l->seen = frame_no; active[nactive++] = i; }
            return 1;
        }
        if (l->seen != frame_no && (oldest < 0 || l->seen < lights[oldest].seen)) oldest = i;
    }
    int i = free_slot >= 0 ? free_slot : oldest;
    if (i < 0) return 1;
    light_t *l = &lights[i];
    free(l->val);
    memset(l, 0, sizeof *l);
    l->used = 1;
    l->seen = frame_no;
    l->kind = kind;
    l->x = (float)x; l->y = (float)y; l->z = z;
    l->seed = (float)fmod(x * 12.9898 + y * 78.233, 6.2831853);
    float r = kinds[kind].radius;
    l->cx0 = (int)floor((x - r - gx0) / CELL);
    l->cy0 = (int)floor((y - r - gy0) / CELL);
    l->w = (int)ceil(2 * r / CELL) + 2;
    l->h = l->w;
    l->sig = 0;
    active[nactive++] = i;
    return 1;
}

/* walls near a light (the ones that can shadow its window) */
static int near_walls(const light_t *l, int *out, uint32_t *sig) {
    float wx0 = gx0 + l->cx0 * CELL, wy0 = gy0 + l->cy0 * CELL, wx1 = wx0 + l->w * CELL, wy1 = wy0 + l->h * CELL;
    uint32_t h = 2166136261u;
    int n = 0;
    for (int i = 0; i < nseg; i++) {
        const seg_t *s = &segs[i];
        if (fmaxf(s->x1, s->x2) < wx0 || fminf(s->x1, s->x2) > wx1 || fmaxf(s->y1, s->y2) < wy0 || fminf(s->y1, s->y2) > wy1) continue;
        out[n++] = i;
        uint32_t k[4] = {(uint32_t)(int)s->x1, (uint32_t)(int)s->y1, (uint32_t)(int)s->x2, (uint32_t)(int)s->y2};
        for (int j = 0; j < 4; j++) h = (h ^ k[j]) * 16777619u;
    }
    *sig = h ^ (uint32_t)n;
    return n;
}

typedef struct { light_t *l; const int *walls; int nwalls; } bake_job_t;

static void bake_rows(void *ctx, int r0, int r1) {
    const bake_job_t *j = ctx;
    light_t *l = j->l;
    float R = kinds[l->kind].radius, I = kinds[l->kind].intensity;
    for (int r = r0; r < r1; r++)
        for (int c = 0; c < l->w; c++) {
            float *v = l->val + ((size_t)r * l->w + c) * 3;
            float sx = gx0 + (l->cx0 + c) * CELL + CELL * 0.5f, sy = gy0 + (l->cy0 + r) * CELL + CELL * 0.5f;
            float dx = sx - l->x, dy = sy - l->y, dh2 = dx * dx + dy * dy;
            v[0] = v[1] = v[2] = 0;
            if (dh2 >= R * R) continue;
            int lit = 1;
            for (int k = 0; k < j->nwalls && lit; k++) {
                const seg_t *s = &segs[j->walls[k]];
                float ex = s->x2 - s->x1, ey = s->y2 - s->y1;
                float den = dx * ey - dy * ex;
                if (fabsf(den) < 1e-6f) continue;
                float ax = s->x1 - l->x, ay = s->y1 - l->y;
                float t = (ax * ey - ay * ex) / den, u = (ax * dy - ay * dx) / den;
                if (t > 1e-4f && t < 1 - 1e-4f && u >= 0 && u <= 1) lit = 0;
            }
            if (!lit) continue;
            for (int k = 0; k < 3; k++) {
                float dz = k * 32.0f - l->z, d = sqrtf(dh2 + dz * dz);
                if (d < R) { float q = 1 - d / R; v[k] = I * q * q; }
            }
        }
}

static void bake(light_t *l, const int *walls, int nwalls) {
    if (!l->val) l->val = malloc(sizeof *l->val * (size_t)l->w * l->h * 3);
    bake_job_t j = {l, walls, nwalls};
    hq_parallel(bake_rows, &j, l->h);
    l->version++;
}

/* add k * a light's window into map */
static void accumulate(float *map, const light_t *l, float k) {
    for (int r = 0; r < l->h; r++) {
        int gy = l->cy0 + r;
        if (gy < 0 || gy >= gh) continue;
        int c0 = l->cx0 < 0 ? -l->cx0 : 0, c1 = l->cx0 + l->w > gw ? gw - l->cx0 : l->w;
        if (c1 <= c0) continue;
        float *d = map + ((size_t)gy * gw + l->cx0 + c0) * 3;
        const float *s = l->val + ((size_t)r * l->w + c0) * 3;
        for (int i = 0, n = (c1 - c0) * 3; i < n; i++) d[i] += k * s[i];
    }
}

static void copy_rect(int x0, int y0, int w, int h) {
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > gw) w = gw - x0;
    if (y0 + h > gh) h = gh - y0;
    if (w <= 0 || h <= 0) return;
    for (int y = y0; y < y0 + h; y++) memcpy(cmap + ((size_t)y * gw + x0) * 3, smap + ((size_t)y * gw + x0) * 3, sizeof *cmap * (size_t)w * 3);
}

static float flicker(const light_t *l, double t) {
    float f = kinds[l->kind].flicker;
    if (f <= 0) return 1;
    double n = 0.5 * sin(t * 11.0 + l->seed) + 0.3 * sin(t * 17.3 + l->seed * 2.1) + 0.2 * sin(t * 29.1 + l->seed * 3.7);
    return (float)(1 + f * n);
}

void light_end(double px, double py) {
    if (light_ambient != lut_ambient) build_lut();
    ppx = px;
    ppy = py;
    have_player = 1;
    if (!enabled || !cmap) return;
    static int *walls;
    static int walls_cap;
    if (nseg > walls_cap) { walls_cap = nseg + 64; walls = realloc(walls, sizeof *walls * (size_t)walls_cap); }
    uint32_t steady = 2166136261u;
    for (int a = 0; a < nactive; a++) {
        light_t *l = &lights[active[a]];
        uint32_t sig;
        int n = near_walls(l, walls, &sig);
        if (!l->val || sig != l->sig) { bake(l, walls, n); l->sig = sig; }
        if (kinds[l->kind].flicker <= 0) steady = (steady ^ (uint32_t)(active[a] * 7919 + l->version)) * 16777619u;
    }
    steady ^= 1; /* never 0 (= not built) */
    if (steady != static_built) {
        memset(smap, 0, sizeof *smap * (size_t)gw * gh * 3);
        for (int a = 0; a < nactive; a++) {
            const light_t *l = &lights[active[a]];
            if (kinds[l->kind].flicker <= 0) accumulate(smap, l, 1);
        }
        memcpy(cmap, smap, sizeof *cmap * (size_t)gw * gh * 3);
        static_built = steady;
        ndirty = 0;
    }
    for (int i = 0; i < ndirty; i++) copy_rect(dirty[i][0], dirty[i][1], dirty[i][2], dirty[i][3]);
    ndirty = 0;
    double t = host_time_us() / 1e6;
    for (int a = 0; a < nactive; a++) {
        const light_t *l = &lights[active[a]];
        if (kinds[l->kind].flicker <= 0) continue;
        accumulate(cmap, l, flicker(l, t));
        dirty[ndirty][0] = l->cx0; dirty[ndirty][1] = l->cy0; dirty[ndirty][2] = l->w; dirty[ndirty][3] = l->h;
        ndirty++;
    }
    /* forget lights not seen for a while (another part of the level) */
    for (int i = 0; i < MAX_LIGHTS; i++)
        if (lights[i].used && frame_no - lights[i].seen > 2000) { free(lights[i].val); memset(&lights[i], 0, sizeof lights[i]); }
}

/* ---------------------------------------------------------------- sampling */
static inline float player_light(double x, double y, float z) {
    if (!have_player) return 0;
    double dx = x - ppx, dy = y - ppy, dz = z - 32.0, d2 = dx * dx + dy * dy + dz * dz;
    if (d2 >= PLAYER_RADIUS * PLAYER_RADIUS) return 0;
    double q = 1 - d2 / (PLAYER_RADIUS * PLAYER_RADIUS);
    return (float)(PLAYER_LIGHT * q * q);
}

static inline void grid_at(double x, double y, float out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!cmap) return;
    double u = (x - gx0) / CELL - 0.5, v = (y - gy0) / CELL - 0.5;
    int i = (int)floor(u), j = (int)floor(v);
    float fu = (float)(u - i), fv = (float)(v - j);
    if (i < 0) { i = 0; fu = 0; }
    if (j < 0) { j = 0; fv = 0; }
    if (i > gw - 2) { i = gw - 2; fu = 1; }
    if (j > gh - 2) { j = gh - 2; fv = 1; }
    const float *p = cmap + ((size_t)j * gw + i) * 3, *q = p + (size_t)gw * 3;
    for (int k = 0; k < 3; k++)
        out[k] = (p[k] * (1 - fu) + p[k + 3] * fu) * (1 - fv) + (q[k] * (1 - fu) + q[k + 3] * fu) * fv;
}

void light_at3(double x, double y, float out[3]) {
    grid_at(x, y, out);
    for (int k = 0; k < 3; k++) out[k] += player_light(x, y, k * 32.0f);
}

float light_at(double x, double y, int layer) {
    float L = player_light(x, y, layer * 32.0f);
    if (!cmap) return L;
    double u = (x - gx0) / CELL - 0.5, v = (y - gy0) / CELL - 0.5;
    int i = (int)floor(u), j = (int)floor(v);
    float fu = (float)(u - i), fv = (float)(v - j);
    if (i < 0) { i = 0; fu = 0; }
    if (j < 0) { j = 0; fv = 0; }
    if (i > gw - 2) { i = gw - 2; fu = 1; }
    if (j > gh - 2) { j = gh - 2; fv = 1; }
    const float *p = cmap + ((size_t)j * gw + i) * 3 + layer, *q = p + (size_t)gw * 3;
    return L + (p[0] * (1 - fu) + p[3] * fu) * (1 - fv) + (q[0] * (1 - fu) + q[3] * fu) * fv;
}

/* scale for each light level (light_lut): ambient slightly cool, lights a
 * soft warm white; above 1 the scale rolls off towards 2 so bright spots
 * keep their colour instead of burning out */
uint16_t light_lut[LIGHT_LUT_N][6];

static void build_lut(void) {
    static const float amb[3] = {0.94f, 0.97f, 1.08f}, tint[3] = {1.00f, 0.93f, 0.82f};
    for (int i = 0; i < LIGHT_LUT_N; i++) {
        float L = i / LIGHT_LUT_STEPS;
        for (int k = 0; k < 3; k++) {
            float v = (float)light_ambient * amb[k] + L * tint[k];
            if (v > 1) v = 1 + (v - 1) / v;
            light_lut[i][k] = (uint16_t)(v * 256);
            int a = (int)(L * tint[k] * 28);
            light_lut[i][3 + k] = (uint16_t)(a > 72 ? 72 : a);
        }
    }
    lut_ambient = light_ambient;
}

void light_span(double x, double y, double dx, double dy, int n, int layer, float *out) {
    float z = layer * 32.0f - 32.0f, pr2 = (float)(PLAYER_RADIUS * PLAYER_RADIUS);
    float qx = (float)(x - ppx), qy = (float)(y - ppy), qdx = (float)dx, qdy = (float)dy, qz2 = z * z;
    float u = (float)((x - gx0) / CELL - 0.5), v = (float)((y - gy0) / CELL - 0.5), du = (float)(dx / CELL), dv = (float)(dy / CELL);
    for (int k = 0; k < n; k++, u += du, v += dv, qx += qdx, qy += qdy) {
        float L = 0;
        if (have_player) {
            float d2 = qx * qx + qy * qy + qz2;
            if (d2 < pr2) { float q = 1 - d2 / pr2; L = (float)PLAYER_LIGHT * q * q; }
        }
        if (cmap) {
            float cu = u < 0 ? 0 : u > gw - 1.001f ? gw - 1.001f : u, cv = v < 0 ? 0 : v > gh - 1.001f ? gh - 1.001f : v;
            int i = (int)cu, j = (int)cv;
            float fu = cu - i, fv = cv - j;
            const float *p = cmap + ((size_t)j * gw + i) * 3 + layer, *q = p + (size_t)gw * 3;
            L += (p[0] * (1 - fu) + p[3] * fu) * (1 - fv) + (q[0] * (1 - fu) + q[3] * fu) * fv;
        }
        out[k] = L;
    }
}
