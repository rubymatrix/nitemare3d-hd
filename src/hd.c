/*
 * Native high-resolution renderer for Nitemare-3D's 3D view (see hd.h).
 *
 * World data used (all owned and updated by the original code):
 *   1040:0006 + i*0x1c   wall vectors, i < DS:7e46
 *       +3 frame  +4 texture set  +5 flags (1 = present, 0x20 = texture
 *       anchored at the far end)  +6 type  +7 facing (0 N, 1 S, 2 E, 3 W)
 *       +c x1 +e y1 +10 x2 +12 y2   (64 units per map cell, y grows south)
 *   DS:519c + set*8      wall texture sets {count, delay, ?, frames}
 *   DS:4734 + set*8      sprite sets       {count, delay, ?, frames}
 *       frame entry (10 bytes): width, height, file offset (IMG.n), far data
 *       pixels are column-major; walls are 64 high, sprite colour 0x29 is
 *       transparent.
 *   DS:838e[DS:838c]     far pointers to objects: +3 frame +4 set +5 flags
 *       (1 = visible, 0x10 = ignore depth) +10 x +12 y +1a raise
 *   DS:4c3a + i*0x2a     8 projectiles, state at +c, object part at +e
 *   DS:4be6/4be8 player x/y, DS:4c36/4c38 cos/sin (1024 = 1), DS:3a5c
 *   focal length, DS:3a64 height scale, DS:53d4..53e0 view window,
 *   DS:7e52/7e53 floor/ceiling colours, DS:7e50 + DS:8084 side shading.
 *
 * Projection (same as FUN_1010_e4cc): with dx = X - px, dy = py - Y
 *   depth a = dx*cos + dy*sin, lateral l = dx*sin - dy*cos,
 *   column = l * f / a + centre,  half wall height = h / (16 a) rows.
 */
#include "hd.h"
#include "hd_tex.h"
#include "hd_pack.h"
#include "hud.h"
#include "light.h"
#include "net.h"
#include "rt/rt.h"
#include "rt/host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEL_WORLD 0x1040
#define TRANSPARENT 0x29
#define NEAR_DEPTH 2048.0 /* 2 world units, in depth units (x1024) */

static inline int16_t ds16(uint16_t o) { return (int16_t)MEMR16(SEGLIN(RT_SEL_DS, o)); }
static inline uint16_t dsu16(uint16_t o) { return MEMR16(SEGLIN(RT_SEL_DS, o)); }
static inline int32_t ds32(uint16_t o) { return (int32_t)MEMR32(SEGLIN(RT_SEL_DS, o)); }
static inline uint8_t ds8(uint16_t o) { return MEMR8(SEGLIN(RT_SEL_DS, o)); }

/* ---------------------------------------------------------------- output layout */
static int frame_valid; /* an hd view was rendered for the current output layout */
static int out_w, out_h;
static double kx, ky, ox, oy; /* 320x200 pixel -> output: x = ox + gx*kx, y = oy + gy*ky */

void hd_set_output(int w, int h) {
    if (w == 320 && h == 200) w = h = 0;
    out_w = w;
    out_h = h;
    frame_valid = 0;
    if (!w) return;
    double s = (double)w / 320.0;
    if ((double)h / 240.0 < s) s = (double)h / 240.0;
    kx = s;
    ky = s * 1.2; /* 320x200 was shown at 4:3 */
    ox = (w - 320.0 * kx) * 0.5;
    oy = (h - 200.0 * ky) * 0.5;
}

int hd_enabled(void) { return out_w > 0; }

void hd_output_to_game(int x, int y, int *gx, int *gy) {
    if (!out_w) { *gx = x; *gy = y; return; }
    int a = (int)floor((x + 0.5 - ox) / kx), b = (int)floor((y + 0.5 - oy) / ky);
    *gx = a < 0 ? 0 : a > 319 ? 319 : a;
    *gy = b < 0 ? 0 : b > 199 ? 199 : b;
}

/* ---------------------------------------------------------------- frame state */
static int vx0, vx1, vy0, vy1;   /* original view window (inclusive) */
static int RX0, RY0, RX1, RY1;   /* hd view rectangle on the output */
static int wide;                 /* view widened to the full output width */
static int full_hud;             /* gothic HUD: the view fills the output, the status bar is replaced */
static double view_shift;        /* full_hud: projection moved down to centre the horizon (output pixels) */
static double weapon_drop;       /* full_hud: the weapon moved down to the bottom edge (output pixels) */
static uint32_t *view_buf;       /* hd view, XRGB, (RX1-RX0+2*view_m) x (RY1-RY0+2*view_m) */
static int VM;                   /* overscan the view is allocated with, so it can be rolled (pixels per side) */
static int view_m;               /* overscan of the view last drawn */
static size_t view_cap;
static float *zbuf;
static int *col_vec;
static float *col_t;
/* per column texturing (filled serially, drawn in parallel) */
static const hq_tex **col_tex;
static float *col_u, *col_fu, *col_umin, *col_umax;
static float *col_L; /* dynamic light at the wall: floor, eye and ceiling height (3 per column) */
static int *col_y0, *col_y1; /* rows the wall covers in each column */
static size_t col_cap;
static float *row_a; /* floor/ceiling depth of each view row (lighting) */
static size_t row_cap;
static int lighting;  /* the frame being drawn is lit (light.c) */
static double light_px, light_py; /* where the player was when it was lit */

static uint8_t ov_col[64000], ov_anchor[64000]; /* overlays drawn over the view (320x200 coords) */
static uint8_t expected[64000];                 /* DIB after the regular overlays */
static uint8_t layer_save[64000];
static int layer_anchor;

static uint8_t *dib(void) {
    uint16_t sel = dsu16(0x5190);
    return sel ? rt_seg[sel] : NULL;
}

/* ---------------------------------------------------------------- texture cache */
typedef struct { uint32_t off, size; uint8_t *data; } tex_t;
#define TEX_SLOTS 4096
static tex_t tex_cache[TEX_SLOTS];
static char tex_file[16];
static FILE *tex_fp;

static void tex_flush(void) {
    for (int i = 0; i < TEX_SLOTS; i++) { free(tex_cache[i].data); tex_cache[i].data = NULL; }
    if (tex_fp) fclose(tex_fp);
    tex_fp = NULL;
    light_flush();
}

/* Pixels of a frame entry (DS offset), read from IMG.n like FUN_1010_46da does. */
static const uint8_t *tex_get(uint16_t entry, uint32_t size) {
    char name[16];
    for (int i = 0; i < 15; i++) { name[i] = (char)ds8((uint16_t)(0x7e64 + i)); if (!name[i]) break; }
    name[15] = 0;
    if (strcmp(name, tex_file)) { tex_flush(); strcpy(tex_file, name); }
    uint32_t off = (uint32_t)ds32((uint16_t)(entry + 2));
    uint32_t h = (off * 2654435761u) >> 20;
    for (int probe = 0; probe < TEX_SLOTS; probe++) {
        tex_t *t = &tex_cache[(h + probe) & (TEX_SLOTS - 1)];
        if (t->data && t->off == off && t->size >= size) return t->data;
        if (!t->data) {
            if (!tex_fp) tex_fp = dos_fopen(tex_file, "rb");
            if (!tex_fp) return NULL;
            uint8_t *d = calloc(1, size ? size : 1);
            if (fseek(tex_fp, (long)off, SEEK_SET) == 0) {
                size_t got = fread(d, 1, size, tex_fp);
                (void)got;
            }
            t->off = off;
            t->size = size;
            t->data = d;
            return d;
        }
    }
    tex_flush(); /* full: start over */
    return tex_get(entry, size);
}

/* ---------------------------------------------------------------- renderer */
typedef struct {
    double px, py, c, s, f, hs; /* player, cos/sin, focal, height scale */
    double cx_hi, hor_hi, wall_mid_hi; /* projection centre on the hd view buffer */
    double eye;                        /* view bob: eye raised by this many world units */
    int bw, bh;
} cam_t;

/* Debug aid: N3D_HDCHECK=1 verifies texture/sprite pixels and the angle tables against the game's own data. */
static int hd_check(void) {
    static int v = -1;
    if (v < 0) v = getenv("N3D_HDCHECK") != NULL;
    return v;
}

static double turn_offset;
void hd_set_turn_offset(double d) { turn_offset = d; }

/* Smooth motion. The game updates the world once per game frame (25 Hz:
 * the player moves 10 units / turns 5 degrees a step, doors slide, enemies
 * and projectiles step). At every game frame the hi-res renderer records a
 * snapshot of everything that moves; the view is then drawn (and redrawn
 * between game frames at the --fps rate) with everything placed between the
 * last two snapshots by the time elapsed since the newest one. Game memory is
 * not used for positions in between: it already holds the next frame.
 * Mouse turning is not interpolated (it is shown immediately, see
 * turn_offset): the camera angle is the interpolated keyboard part plus all
 * whole mouse degrees applied so far. */
#define SNAP_VEC 2048
#define SNAP_OBJ 600
typedef struct {
    uint64_t t;
    int valid;
    double px, py, kang; /* player; kang = view angle minus cumulative mouse turn */
    int nvec;
    int16_t vec[SNAP_VEC][4];
    int nobj;
    uint32_t obj[SNAP_OBJ];
    int16_t oxy[SNAP_OBJ][2];
    uint8_t pstate[8];
    int16_t pxy[8][2];
} snap_t;
static snap_t snaps[3];
static snap_t *sp_old = &snaps[0], *sp_prev = &snaps[1], *sp_cur = &snaps[2]; /* game frames k-2, k-1, k */
static const snap_t *sp_a = &snaps[1], *sp_b = &snaps[2]; /* the pair the frame being drawn lies between */
static double mouse_cum;  /* whole mouse degrees applied to the game angle, ever */
static int exact_camera;  /* debug comparison: draw game memory as is */
void hd_note_mouse_turn(int degrees) { mouse_cum += degrees; }

static double wrap180(double a) {
    while (a > 180) a -= 360;
    while (a <= -180) a += 360;
    return a;
}

/* When the game frame being drawn was due. The game draws a frame when
 * floor(ms * fps / 1000) of its clock advances (FUN_1010_d69c: fps DS:53e4,
 * frame number DS:0234), so frame k belongs at exactly k * 1000 / fps ms.
 * Using that instead of the moment the host loop got around to running the
 * game keeps the interpolation steps exact (vsync waits in the loop make the
 * latter late by up to a display refresh, which made walls jitter). */
static uint64_t game_frame_time_us(void) {
    uint64_t now = host_time_us();
    int fps = ds16(0x53e4);
    uint32_t slot = (uint32_t)ds32(0x0234);
    if (fps <= 0 || fps > 1000) return now;
    uint64_t ms = ((uint64_t)slot * 1000 + (uint64_t)fps - 1) / (uint64_t)fps; /* first ms of the slot */
    uint64_t t = host_clock_base_us() + ms * 1000;
    if (t > now || now - t > 200000) return now; /* clock wrapped or not this frame: fall back */
    return t;
}

static void take_snapshot(void) {
    snap_t *t = sp_old;
    sp_old = sp_prev;
    sp_prev = sp_cur;
    sp_cur = t;
    t->t = game_frame_time_us();
    t->valid = 1;
    if (getenv("N3D_TURNLOG")) fprintf(stderr, "snap %llu\n", (unsigned long long)(t->t / 1000));
    t->px = ds16(0x4be6);
    t->py = ds16(0x4be8);
    t->kang = ds16(0x4bda) - mouse_cum;
    t->nvec = ds16(0x7e46);
    if (t->nvec > SNAP_VEC) t->nvec = SNAP_VEC;
    for (int i = 0; i < t->nvec; i++)
        for (int k = 0; k < 4; k++) t->vec[i][k] = (int16_t)MEMR16(SEGLIN(SEL_WORLD, 6 + i * 0x1c + 0xc + 2 * k));
    t->nobj = ds16(0x838c);
    if (t->nobj > SNAP_OBJ) t->nobj = SNAP_OBJ;
    for (int i = 0; i < t->nobj; i++) {
        uint32_t p = MEMR32(SEGLIN(RT_SEL_DS, (uint16_t)(0x838e + i * 4)));
        uint32_t o = SEGLIN(p >> 16, p & 0xffff);
        t->obj[i] = o;
        t->oxy[i][0] = (int16_t)MEMR16(o + 0x10);
        t->oxy[i][1] = (int16_t)MEMR16(o + 0x12);
    }
    for (int i = 0; i < 8; i++) {
        uint16_t b = (uint16_t)(0x4c3a + i * 0x2a);
        t->pstate[i] = ds8((uint16_t)(b + 0xc));
        t->pxy[i][0] = ds16((uint16_t)(b + 0x1e));
        t->pxy[i][1] = ds16((uint16_t)(b + 0x20));
    }
    /* first frame, new level, teleports (loaded game) and pauses: no blending */
    double dx = sp_cur->px - sp_prev->px, dy = sp_cur->py - sp_prev->py;
    if (!sp_prev->valid || sp_prev->nvec != sp_cur->nvec || dx * dx + dy * dy > 64.0 * 64.0 || sp_cur->t - sp_prev->t > 150000)
        *sp_old = *sp_prev = *sp_cur;
    else if (!sp_old->valid || sp_old->nvec != sp_prev->nvec)
        *sp_old = *sp_prev;
}

/* Choose the snapshot pair for the frame being drawn and the position
 * between them. Drawn a little behind the game clock (INTERP_DELAY_US) and
 * allowed to run slightly past the newest frame: the host loop notices a game
 * frame up to one display refresh after it was due (it waits for vsync), and
 * stopping at the newest frame until then froze the camera for a refresh, a
 * judder every few frames while moving. */
#define INTERP_DELAY_US 5000.0 /* plus one game frame: what is shown lies between two known frames */
#define INTERP_MAX_ALPHA 1.35  /* if the newest frame is late, extrapolate at most a third of a frame */
static double interp_alpha(void) {
    sp_a = sp_prev;
    sp_b = sp_cur;
    if (exact_camera || !sp_cur->valid) return 1;
    double frame_us = sp_cur->t > sp_prev->t ? (double)(sp_cur->t - sp_prev->t) : 40000;
    if (frame_us < 10000) frame_us = 10000;
    double show = (double)host_time_us() - INTERP_DELAY_US - frame_us; /* the moment drawn */
    if (show < (double)sp_prev->t && sp_old->valid && sp_prev->t > sp_old->t) { /* between k-2 and k-1 */
        sp_a = sp_old;
        sp_b = sp_prev;
    }
    double span = sp_b->t > sp_a->t ? (double)(sp_b->t - sp_a->t) : frame_us;
    double a = (show - (double)sp_a->t) / span;
    return a < 0 ? 0 : a > INTERP_MAX_ALPHA ? INTERP_MAX_ALPHA : a;
}
static double frame_alpha; /* alpha for the frame being drawn */

static inline double blend(double a, double b) {
    return fabs(b - a) > 64 ? b : a + frame_alpha * (b - a); /* big jumps (respawn, pickup moved): snap */
}

/* wall vector end points as drawn */
static void vec_coords(int i, uint32_t v, double c[4]) {
    if (exact_camera || i >= sp_b->nvec) {
        for (int k = 0; k < 4; k++) c[k] = (int16_t)MEMR16(v + 0xc + 2 * k);
        return;
    }
    for (int k = 0; k < 4; k++) c[k] = blend(sp_a->vec[i][k], sp_b->vec[i][k]);
}

/* object position as drawn; idx is its index in the object list (or -1) */
static void obj_coords(uint32_t o, int idx, double *x, double *y) {
    *x = (int16_t)MEMR16(o + 0x10);
    *y = (int16_t)MEMR16(o + 0x12);
    if (exact_camera) return;
    int c = -1, p = -1;
    if (idx >= 0 && idx < sp_b->nobj && sp_b->obj[idx] == o) c = idx;
    else for (int i = 0; i < sp_b->nobj; i++) if (sp_b->obj[i] == o) { c = i; break; }
    if (c < 0) return;
    if (c < sp_a->nobj && sp_a->obj[c] == o) p = c;
    else for (int i = 0; i < sp_a->nobj; i++) if (sp_a->obj[i] == o) { p = i; break; }
    if (p < 0) { *x = sp_b->oxy[c][0]; *y = sp_b->oxy[c][1]; return; }
    *x = blend(sp_a->oxy[p][0], sp_b->oxy[c][0]);
    *y = blend(sp_a->oxy[p][1], sp_b->oxy[c][1]);
}

/* the game is drawing the 3D view (a game frame within the last 150 ms) */
int hd_view_live(void) { return frame_valid && out_w && sp_cur->valid && host_time_us() - sp_cur->t < 150000; }

/* Quake-style view bob (cl_bob / cl_bobcycle / cl_bobup): while the player
 * moves, the eye rises and falls with a 0.6 s cycle (half up, half down),
 * by 0.3 + 0.7 sin of the phase times an amount proportional to the speed;
 * the weapon follows it and sways sideways at half that rate. Speed comes
 * from the last two game frames, eased so starting and stopping glide. */
double hd_bob_scale = 1.0;
static double bob_speed, bob_eye, bob_wx, bob_wy; /* units/s; world units; 320x200 pixels */
/* Quake-style roll (cl_rollangle / cl_rollspeed): the view leans into
 * sideways motion, full angle at walking speed and above. */
double hd_roll_angle = 2.0;      /* degrees */
#define ROLL_SPEED 250.0          /* units/s for the full angle (walking) */
static double roll_side, view_roll; /* eased sideways speed (units/s, + right); degrees, + leaning right */
static uint64_t bob_t;
#define BOB_CYCLE 0.6
#define BOB_UP 0.5
#define BOB_PER_SPEED 0.008 /* world units of bob per unit/s: walking (250/s) bobs 2 units */
#define BOB_MAX 3.0
#define BOB_PI 3.14159265358979323846

static void update_bob(void) {
    uint64_t now = host_time_us();
    double dt = bob_t && now > bob_t ? (now - bob_t) / 1e6 : 0;
    bob_t = now;
    if (dt > 0.25) dt = 0.25;
    double target = 0;
    if (!exact_camera && sp_cur->valid && sp_prev->valid && sp_cur->t > sp_prev->t && now - sp_cur->t < 120000) {
        double dx = sp_cur->px - sp_prev->px, dy = sp_cur->py - sp_prev->py;
        target = sqrt(dx * dx + dy * dy) / ((sp_cur->t - sp_prev->t) / 1e6);
        if (target > 2000) target = 0; /* teleport */
    }
    double side = 0;
    if (target > 0) { /* sideways part: lateral axis of to_cam, (cos a, sin a) with y south */
        double a = ds16(0x4bda) * (BOB_PI / 180.0), dx = sp_cur->px - sp_prev->px, dy = sp_cur->py - sp_prev->py;
        side = (dx * cos(a) + dy * sin(a)) / ((sp_cur->t - sp_prev->t) / 1e6);
    }
    int playing = ds8(0x46a4) == 1;
    if (!playing) target = side = 0;
    bob_speed += (target - bob_speed) * (1 - exp(-dt / 0.12));
    roll_side += (side - roll_side) * (1 - exp(-dt / 0.12));
    view_roll = fabs(roll_side) < ROLL_SPEED ? roll_side * hd_roll_angle / ROLL_SPEED : (roll_side < 0 ? -hd_roll_angle : hd_roll_angle);
    double amp = bob_speed * BOB_PER_SPEED;
    if (amp > BOB_MAX) amp = BOB_MAX;
    amp *= hd_bob_scale;
    double t = now / 1e6, cyc = fmod(t, BOB_CYCLE) / BOB_CYCLE;
    double ph = cyc < BOB_UP ? BOB_PI * cyc / BOB_UP : BOB_PI + BOB_PI * (cyc - BOB_UP) / (1 - BOB_UP);
    double b = amp * (0.3 + 0.7 * sin(ph));
    bob_eye = b;
    bob_wy = (amp - b) * 3.0;                               /* lowest when the eye is lowest, never above its rest */
    bob_wx = amp * 2.5 * cos(BOB_PI * (t / BOB_CYCLE - 0.25)); /* out to the sides at the top of each step */
}

static void project_setup(cam_t *cam) {
    cam->px = ds16(0x4be6);
    cam->py = ds16(0x4be8);
    cam->c = ds16(0x4c36);
    cam->s = ds16(0x4c38);
    if (hd_check()) {
        double a = ds16(0x4bda) * (3.14159265358979323846 / 180.0);
        if (fabs(1024.0 * sin(a) - cam->c) > 1.5 || fabs(1024.0 * cos(a) - cam->s) > 1.5)
            fprintf(stderr, "anglecheck %d: tables %g %g vs %g %g\n", ds16(0x4bda), cam->c, cam->s, 1024.0 * sin(a), 1024.0 * cos(a));
    }
    double ang = ds16(0x4bda);
    frame_alpha = interp_alpha();
    if (!exact_camera && sp_b->valid) {
        cam->px = sp_a->px + frame_alpha * (sp_b->px - sp_a->px);
        cam->py = sp_a->py + frame_alpha * (sp_b->py - sp_a->py);
        ang = sp_a->kang + frame_alpha * wrap180(sp_b->kang - sp_a->kang) + mouse_cum + turn_offset;
    }
    if (fabs(wrap180(ang - ds16(0x4bda))) > 1e-9) { /* tables: cos slot = 1024 sin(a), sin slot = 1024 cos(a), clockwise from north */
        double a = ang * (3.14159265358979323846 / 180.0);
        cam->c = 1024.0 * sin(a);
        cam->s = 1024.0 * cos(a);
    }
    cam->f = ds32(0x3a5c);
    cam->hs = ds32(0x3a64);
    int cx = ds16(0x53dc), hor = ds16(0x53de);
    cam->cx_hi = ox + (cx + 0.5) * kx - RX0;
    double shift = exact_camera ? 0 : view_shift;
    cam->hor_hi = oy + hor * ky - RY0 + shift;  /* first floor row */
    cam->wall_mid_hi = oy + (hor + 0.5) * ky - RY0 + shift;
    cam->eye = exact_camera ? 0 : bob_eye;
    /* overscan just wide enough for the current roll (none while level): buffers hold VM */
    int m = exact_camera || fabs(view_roll) <= 0.01 ? 0 : (int)ceil(sin(fabs(view_roll) * (BOB_PI / 180.0)) * ((RX1 - RX0) + (RY1 - RY0)) * 0.5) + 2;
    if (m > VM) m = VM;
    view_m = m;
    cam->cx_hi += m;
    cam->hor_hi += m;
    cam->wall_mid_hi += m;
    cam->bw = RX1 - RX0 + 2 * m;
    cam->bh = RY1 - RY0 + 2 * m;
}

static inline void to_cam(const cam_t *cam, double X, double Y, double *a, double *l) {
    double dx = X - cam->px, dy = cam->py - Y;
    *a = dx * cam->c + dy * cam->s;
    *l = dx * cam->s - dy * cam->c;
}

typedef struct { const cam_t *cam; } wall_job_t;

/* draw wall columns [x0, x1): runs on worker threads */
static void draw_wall_columns(void *ctx, int x0, int x1) {
    const cam_t *cam = ((wall_job_t *)ctx)->cam;
    int bw = cam->bw, bh = cam->bh;
    int split = (int)lround(cam->hor_hi);
    if (split < 0) split = 0;
    if (split > bh) split = bh;
    for (int x = x0; x < x1; x++) {
        const hq_tex *t = col_tex[x];
        int y0 = split, y1 = split;
        if (col_vec[x] >= 0 && t) {
            double a = zbuf[x];
            double half = (cam->hs / (16.0 * a) + 0.5) * ky;
            double mid = cam->wall_mid_hi + cam->eye * cam->hs / (16.0 * a) / 32.0 * ky; /* walls are 64 units high */
            double top = mid - half;
            y0 = (int)ceil(top - 0.5), y1 = (int)ceil(mid + half - 0.5);
            if (y0 < 0) y0 = 0;
            if (y1 > bh) y1 = bh;
            float step = (float)(64.0 / (2.0 * half));
            float vpos = (float)((y0 + 0.5 - top) * step);
            float u = col_u[x], fu = col_fu[x];
            if (y1 > y0) hq_column_span(t, u, fu, col_umin[x], col_umax[x], vpos, step, y1 - y0, view_buf + (size_t)y0 * bw + x, bw);
            if (lighting) { /* height on the wall: 64 at its top, 0 at its foot */
                /* light is linear in height above and below eye level: step its LUT index in 16.16 */
                const float *L = col_L + 3 * x;
                uint32_t *p = view_buf + (size_t)y0 * bw + x;
                double z = 64.0 * (1.0 - (y0 + 0.5 - top) / (2.0 * half)), dz = 64.0 / (2.0 * half);
                int ymid = (int)ceil(top + half - 0.5); /* first row below eye level */
                for (int part = 0; part < 2; part++) {
                    int ya = part ? (ymid > y0 ? ymid : y0) : y0, yb = part ? y1 : (ymid < y1 ? ymid : y1);
                    if (yb <= ya) continue;
                    double zz = z - (ya - y0) * dz;
                    double la = part ? L[0] : L[1], lb = part ? L[1] : L[2], z0 = part ? 0 : 32; /* la at z0, lb at z0 + 32 */
                    double i0 = (la + (lb - la) * (zz - z0) / 32) * LIGHT_LUT_STEPS, di = -(lb - la) * dz / 32 * LIGHT_LUT_STEPS;
                    int32_t idx = (int32_t)lround(i0 * 65536.0), didx = (int32_t)lround(di * 65536.0);
                    uint32_t *q = p + (size_t)(ya - y0) * bw;
                    for (int y = ya; y < yb; y++, q += bw, idx += didx) {
                        int i = (idx + 32768) >> 16;
                        *q = light_apply(*q, light_lut[i < 0 ? 0 : i >= LIGHT_LUT_N ? LIGHT_LUT_N - 1 : i]);
                    }
                }
            }
        }
        col_y0[x] = y0;
        col_y1[x] = y1;
    }
}

/* light the floor and ceiling around the walls, row by row: along a row the
 * world point under each pixel moves in a straight line (worker threads) */
static void shade_flat_rows(void *ctx, int r0, int r1) {
    const cam_t *cam = ((wall_job_t *)ctx)->cam;
    int bw = cam->bw;
    int split = (int)lround(cam->hor_hi);
    double n2 = cam->c * cam->c + cam->s * cam->s, kf = cam->f * kx;
    float L[1024 / 4 + 2];
    for (int y = r0; y < r1; y++) {
        double a = row_a[y];
        if (a <= 0) continue;
        int ceiling = y < split;
        uint32_t *row = view_buf + (size_t)y * bw;
        for (int x0 = 0; x0 < bw; x0 += 1024) {
            int n = bw - x0 < 1024 ? bw - x0 : 1024;
            double tau = (x0 + 0.5 - cam->cx_hi) / kf, dt = 1.0 / kf;
            /* every 4th pixel, linear in between */
            light_span(cam->px + a * (cam->c + cam->s * tau) / n2, cam->py - a * (cam->s - cam->c * tau) / n2,
                       4 * a * cam->s * dt / n2, 4 * a * cam->c * dt / n2, n / 4 + 2, ceiling ? 2 : 0, L);
            const int *cy = ceiling ? col_y0 + x0 : col_y1 + x0;
            uint32_t *px = row + x0;
            for (int g = 0; g < n; g += 4) {
                int32_t idx = (int32_t)(L[g >> 2] * (LIGHT_LUT_STEPS * 65536.0f)) + 32768;
                int32_t didx = ((int32_t)(L[(g >> 2) + 1] * (LIGHT_LUT_STEPS * 65536.0f)) + 32768 - idx) >> 2;
                for (int k = g; k < g + 4 && k < n; k++, idx += didx) {
                    if (ceiling ? y >= cy[k] : y < cy[k]) continue;
                    int i = idx >> 16;
                    px[k] = light_apply_flat(px[k], light_lut[i < 0 ? 0 : i >= LIGHT_LUT_N ? LIGHT_LUT_N - 1 : i]);
                }
            }
        }
    }
}

static uint8_t vec_frame[2048]; /* wall frames as the original draws them (it animates after drawing) */
static void render_walls(const cam_t *cam) {
    int bw = cam->bw, bh = cam->bh;
    uint8_t ceil_c = ds8(0x7e53), floor_c = ds8(0x7e52);
    int split = (int)lround(cam->hor_hi);
    if (split < 0) split = 0;
    if (split > bh) split = bh;
    uint32_t cc = hq_palette_color(ceil_c), fc = hq_palette_color(floor_c);
    for (size_t k = 0, e = (size_t)split * bw; k < e; k++) view_buf[k] = cc;
    for (size_t k = (size_t)split * bw, e = (size_t)bh * bw; k < e; k++) view_buf[k] = fc;
    for (int x = 0; x < bw; x++) { zbuf[x] = 1e30f; col_vec[x] = -1; }
    if (exact_camera) lighting = 0;
    if (lighting) { /* depth of the floor (below the horizon) or ceiling seen on each row */
        if ((size_t)bh > row_cap) { row_cap = (size_t)bh; free(row_a); row_a = malloc(row_cap * sizeof *row_a); }
        for (int y = 0; y < bh; y++) {
            double d = (y + 0.5 - cam->wall_mid_hi) / ky, hh = y < split ? (-d - 0.5) / (1 - cam->eye / 32.0) : (d - 0.5) / (1 + cam->eye / 32.0);
            row_a[y] = hh > 0 ? (float)(cam->hs / (16.0 * hh)) : -1;
        }
    }

    double kf = cam->f * kx; /* lateral/depth -> hd pixels */
    int nvec = ds16(0x7e46);
    for (int i = 0; i < nvec; i++) {
        uint32_t v = SEGLIN(SEL_WORLD, 6 + i * 0x1c);
        uint8_t flags = MEMR8(v + 5), facing = MEMR8(v + 7);
        if (!(flags & 1)) continue;
        double vc[4];
        vec_coords(i, v, vc);
        double x1 = vc[0], y1 = vc[1], x2 = vc[2], y2 = vc[3];
        /* back-face culling, as the per-facing sorted lists of FUN_1018_387e do */
        if (facing == 0 ? !(cam->py < y1) : facing == 1 ? !(cam->py > y1) : facing == 2 ? !(cam->px > x1) : !(cam->px < x1))
            continue;
        double a1, l1, a2, l2;
        to_cam(cam, x1, y1, &a1, &l1);
        to_cam(cam, x2, y2, &a2, &l2);
        if (a1 < NEAR_DEPTH && a2 < NEAR_DEPTH) continue;
        double ca1 = a1, cl1 = l1, ca2 = a2, cl2 = l2;
        if (ca1 < NEAR_DEPTH) { double t = (NEAR_DEPTH - a1) / (a2 - a1); ca1 = NEAR_DEPTH; cl1 = l1 + t * (l2 - l1); }
        if (ca2 < NEAR_DEPTH) { double t = (NEAR_DEPTH - a1) / (a2 - a1); ca2 = NEAR_DEPTH; cl2 = l1 + t * (l2 - l1); }
        double sx1 = cam->cx_hi + cl1 / ca1 * kf, sx2 = cam->cx_hi + cl2 / ca2 * kf;
        if (sx1 > sx2) { double t = sx1; sx1 = sx2; sx2 = t; }
        int c0 = (int)ceil(sx1 - 0.5), c1 = (int)floor(sx2 - 0.5);
        if (c0 < 0) c0 = 0;
        if (c1 > bw - 1) c1 = bw - 1;
        double da = a2 - a1, dl = l2 - l1;
        for (int x = c0; x <= c1; x++) {
            double tau = (x + 0.5 - cam->cx_hi) / kf;
            double den = dl - tau * da;
            if (den == 0) continue;
            double t = (tau * a1 - l1) / den;
            if (t < -0.001 || t > 1.001) continue;
            double a = a1 + t * da;
            if (a < NEAR_DEPTH || a >= zbuf[x]) continue;
            zbuf[x] = (float)a;
            col_vec[x] = i;
            col_t[x] = (float)(t < 0 ? 0 : t > 1 ? 1 : t);
        }
    }

    /* texture the columns (FUN_1010_660c / FUN_1010_35e4) */
    int shade_all = ds8(0x519b) != 0, shading = ds16(0x7e50) > 0;
    int last_vec = -1;
    const uint8_t *last_remap = NULL;
    const hq_tex *last_tex = NULL;
    for (int x = 0; x < bw; x++) {
        int i = col_vec[x];
        col_tex[x] = NULL;
        if (i < 0) continue;
        uint32_t v = SEGLIN(SEL_WORLD, 6 + i * 0x1c);
        uint8_t set = MEMR8(v + 4), facing = MEMR8(v + 7), type = MEMR8(v + 6), flags = MEMR8(v + 5);
        int count = ds16((uint16_t)(0x519c + set * 8));
        int frame = i < 2048 ? vec_frame[i] : MEMR8(v + 3);
        if (count > 0 && frame >= count) frame = count - 1;
        uint16_t entry = (uint16_t)(dsu16((uint16_t)(0x51a2 + set * 8)) + frame * 10);
        int tw = ds8(entry);
        if (tw <= 0) continue;
        const uint8_t *tex = tex_get(entry, (uint32_t)tw * 64);
        if (!tex) { col_vec[x] = -1; continue; }
        if (hd_check() && x % 64 == 0) {
            uint32_t fp = (uint32_t)ds32((uint16_t)(entry + 6));
            if (fp) {
                int bad = 0;
                for (int k = 0; k < tw * 64; k++) bad += MEMR8(SEGLIN(fp >> 16, (fp & 0xffff) + k)) != tex[k];
                fprintf(stderr, "texcheck set %d frame %d: %d differ\n", set, frame, bad);
            }
        }
        double vc[4];
        vec_coords(i, v, vc);
        double x1 = vc[0], y1 = vc[1], x2 = vc[2], y2 = vc[3];
        double t = col_t[x];
        double hx = x1 + t * (x2 - x1), hy = y1 + t * (y2 - y1);
        int far_end = ((flags & 0x20) != 0) ^ (type == 0x40 || type == 0x3f);
        int mirror = type != 2 && (facing == 0 || facing == 3);
        double u, ua, ub; /* ua..ub: texels the whole piece spans */
        if (facing <= 1) {
            if (hx > x2 - 0.01) hx = x2 - 0.01;
            u = far_end ? hx - x2 + 0x80 : hx - x1;
            ua = far_end ? x1 - x2 + 0x80 : 0;
            ub = far_end ? 0x80 : x2 - x1;
        } else {
            if (hy > y2 - 0.01) hy = y2 - 0.01;
            u = far_end ? y2 - hy - 0x80 : y1 - hy;
            ua = far_end ? -0x80 : y1 - y2;
            ub = far_end ? y2 - y1 - 0x80 : 0;
        }
        if (mirror) { u = -u; double t2 = -ua; ua = -ub; ub = t2; }
        if (ub - ua >= tw) { ua = -1e9; ub = 1e9; } /* the piece repeats the texture: let it wrap */
        else { /* same wrap as the sampler: shift the span next to u */
            double k = floor((u - ua) / tw) * tw;
            ua += k; ub += k;
        }
        col_umin[x] = (float)ua;
        col_umax[x] = (float)ub;
        const uint8_t *remap = NULL;
        if (shading && (shade_all || facing >= 2)) remap = LP(SEGLIN(RT_SEL_DS, 0x8084));
        if (i != last_vec || remap != last_remap) {
            last_tex = hq_get((uint32_t)ds32((uint16_t)(entry + 2)), tex, tw, 64, 0, remap);
            last_vec = i;
            last_remap = remap;
        }
        col_tex[x] = last_tex;
        col_u[x] = (float)u;
        if (lighting) /* just in front of the wall: its own row of light cells */
            light_at3(hx + (facing == 2 ? 2 : facing == 3 ? -2 : 0), hy + (facing == 0 ? -2 : facing == 1 ? 2 : 0), col_L + 3 * x);
    }
    /* texels per pixel across the column: from the neighbours on the same wall */
    for (int x = 0; x < bw; x++) {
        if (col_vec[x] < 0) continue;
        int l = x > 0 && col_vec[x - 1] == col_vec[x], r = x + 1 < bw && col_vec[x + 1] == col_vec[x];
        float d = l && r ? 0.5f * fabsf(col_u[x + 1] - col_u[x - 1]) : l ? fabsf(col_u[x] - col_u[x - 1]) : r ? fabsf(col_u[x + 1] - col_u[x]) : 1.0f;
        col_fu[x] = d > 64 ? 1.0f : d;
    }
    hq_build_pending();
    wall_job_t job = {cam};
    hq_parallel(draw_wall_columns, &job, bw);
    if (lighting) hq_parallel(shade_flat_rows, &job, bh);
}

typedef struct { uint32_t obj; double a, l, x, y; } spr_t;

/* objects the original renderer queued (and so animated) this frame */
static uint32_t queued[256];
static uint8_t queued_frame[256];
static int nqueued;
static int no_animate; /* set while rendering the debug comparison */
static int gather_only; /* animate objects, draw nothing */
void hd_view_begin(void) {
    nqueued = 0;
    int n = ds16(0x7e46);
    for (int i = 0; i < n && i < 2048; i++) vec_frame[i] = MEMR8(SEGLIN(SEL_WORLD, 6 + i * 0x1c + 3));
}
void hd_object_queued(uint32_t obj, uint8_t frame) {
    if (nqueued < 256) { queued_frame[nqueued] = frame; queued[nqueued++] = obj; }
}
static int was_queued(uint32_t obj) {
    for (int i = 0; i < nqueued; i++) if (queued[i] == obj) return i;
    return -1;
}

static int spr_cmp(const void *p, const void *q) {
    double a = ((const spr_t *)p)->a, b = ((const spr_t *)q)->a;
    return a < b ? 1 : a > b ? -1 : 0;
}

typedef struct {
    const hq_tex *tex;
    float left, top, us, vs, depth;
    int c0, c1, r0, r1, sw, sh, ignore_depth;
    int lit;          /* shaded by the dynamic light (light sources are not) */
    const uint16_t *lf;
} sprite_draw_t;
static sprite_draw_t spr_draw[600];
static int nspr_draw;
typedef struct { const cam_t *cam; } sprite_job_t;

/* Composite a premultiplied texel over dst. Filtered edges get a steeper
 * alpha ramp so sprites keep their cut-out look without halos. */
static inline void over(uint32_t *dst, uint32_t c) {
    uint32_t a = c >> 24;
    if (!a) return;
    if (a == 255) { *dst = c; return; }
    int s = ((int)a - 128) * 3 + 128;
    if (s <= 0) return;
    uint32_t na = s >= 255 ? 255 : (uint32_t)s;
    uint32_t k = (na << 16) / a; /* rescale premultiplied colour to the new alpha */
    uint32_t r = ((c >> 16) & 255) * k >> 16, g = ((c >> 8) & 255) * k >> 16, b = (c & 255) * k >> 16;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    uint32_t d = *dst, ia = 255 - na;
    r += ((d >> 16) & 255) * ia / 255;
    g += ((d >> 8) & 255) * ia / 255;
    b += (d & 255) * ia / 255;
    *dst = 0xff000000u | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}

/* draw every sprite's part in columns [x0, x1), far to near: worker threads */
static void draw_sprite_columns(void *ctx, int x0, int x1) {
    const cam_t *cam = ((sprite_job_t *)ctx)->cam;
    int bw = cam->bw;
    for (int i = 0; i < nspr_draw; i++) {
        const sprite_draw_t *d = &spr_draw[i];
        int c0 = d->c0 > x0 ? d->c0 : x0, c1 = d->c1 < x1 ? d->c1 : x1;
        for (int x = c0; x < c1; x++) {
            if (!d->ignore_depth && d->depth > zbuf[x]) continue;
            float u = (x + 0.5f - d->left) * d->us;
            if (u < 0) u = 0;
            if (u > d->sw - 0.001f) u = d->sw - 0.001f;
            float v = (d->r0 + 0.5f - d->top) * d->vs;
            uint32_t tmp[4096];
            int nrow = d->r1 - d->r0 < 4096 ? d->r1 - d->r0 : 4096;
            hq_column(d->tex, u, d->us, v, d->vs, nrow, tmp, 1);
            uint32_t *dst = view_buf + (size_t)d->r0 * bw + x;
            if (d->lit)
                for (int k = 0; k < nrow; k++) tmp[k] = light_apply(tmp[k], d->lf);
            for (int k = 0; k < nrow; k++, dst += bw) over(dst, tmp[k]);
        }
    }
}

/* the picture an object shows this frame (and its frame entry) */
static const uint8_t *sprite_pixels(uint32_t o, uint16_t *entry, int *sw, int *sh) {
    uint8_t set = MEMR8(o + 4), frame = MEMR8(o + 3);
    int q = was_queued(o);
    if (q >= 0) frame = queued_frame[q]; /* what the original drew this frame */
    *entry = (uint16_t)(dsu16((uint16_t)(0x473a + set * 8)) + frame * 10);
    *sw = ds8(*entry);
    *sh = ds8((uint16_t)(*entry + 1));
    if (*sw <= 0 || *sh <= 0) return NULL;
    const uint8_t *pix = tex_get(*entry, (uint32_t)*sw * *sh);
    if (pix && hd_check()) {
        uint32_t fp = (uint32_t)ds32((uint16_t)(*entry + 6));
        if (fp) {
            int bad = 0;
            for (int k = 0; k < *sw * *sh; k++) bad += MEMR8(SEGLIN(fp >> 16, (fp & 0xffff) + k)) != pix[k];
            fprintf(stderr, "spritecheck set %d frame %d %dx%d: %d differ\n", set, frame, *sw, *sh, bad);
        }
    }
    return pix;
}

static int light_pass; /* render_sprites only hands the light sources to light.c */

static void render_sprites(const cam_t *cam) {
    static spr_t list[600];
    int n = 0;
    int nobj = ds16(0x838c);
    int demo = ds16(0x46a8);
    int animate = !no_animate && !(demo >= 1 && demo <= 4); /* animation may use the RNG: keep demos exact */
    for (int i = 0; i < nobj && n < 590; i++) {
        uint32_t p = MEMR32(SEGLIN(RT_SEL_DS, (uint16_t)(0x838e + i * 4)));
        list[n].obj = SEGLIN(p >> 16, p & 0xffff);
        obj_coords(list[n].obj, i, &list[n].x, &list[n].y);
        n++;
    }
    for (int i = 0; i < 8; i++) { /* projectiles (FUN_1010_9d72), as of the newest snapshot */
        uint16_t b = (uint16_t)(0x4c3a + i * 0x2a);
        uint8_t state = exact_camera ? ds8((uint16_t)(b + 0xc)) : sp_b->pstate[i];
        double x = exact_camera ? ds16((uint16_t)(b + 0x1e)) : sp_b->pxy[i][0];
        double y = exact_camera ? ds16((uint16_t)(b + 0x20)) : sp_b->pxy[i][1];
        if (!exact_camera && sp_a->pstate[i] == state) { x = blend(sp_a->pxy[i][0], x); y = blend(sp_a->pxy[i][1], y); }
        if (state == 1) {
            if (fabs(x - cam->px) <= 0x14 && fabs(y - cam->py) <= 0x14) continue;
        } else if (state == 2) {
            uint8_t set = ds8((uint16_t)(b + 0x12));
            if (ds8((uint16_t)(b + 0x11)) >= ds16((uint16_t)(0x4734 + set * 8))) continue;
        } else continue;
        list[n].obj = SEGLIN(RT_SEL_DS, (uint16_t)(b + 0xe));
        list[n].x = x;
        list[n].y = y;
        n++;
    }
    { /* the other co-op player */
        uint32_t po;
        double x, y;
        if (net_peer(&po, &x, &y)) {
            list[n].obj = po; list[n].x = x; list[n].y = y; n++;
        }
    }
    if (light_pass) { /* every light in the level, seen or not: it may light what is */
        light_begin();
        for (int i = 0; i < n; i++) {
            uint32_t o = list[i].obj;
            if (!(MEMR8(o + 5) & 1)) continue;
            uint16_t entry;
            int sw, sh;
            const uint8_t *pix = sprite_pixels(o, &entry, &sw, &sh);
            int8_t raise = (int8_t)MEMR8(o + 0x1a);
            if (pix) light_sprite((uint32_t)ds32((uint16_t)(entry + 2)), pix, sw, sh, list[i].x, list[i].y, raise > 0 ? raise : 0);
        }
        light_end(cam->px, cam->py);
        return;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        uint32_t o = list[i].obj;
        if (!(MEMR8(o + 5) & 1)) continue;
        double a, l;
        to_cam(cam, list[i].x, list[i].y, &a, &l);
        if (a < 0x4000) continue;
        if (animate && i < nobj && (MEMR8(o + 5) & 1) && was_queued(o) < 0) {
            /* only inside the widened field of view? */
            double sx = cam->cx_hi + l / a * cam->f * kx;
            double half = cam->hs / (16.0 * a) / 32.0 * 128 * kx;
            if (sx + half >= 0 && sx - half < cam->bw) hd_animate_object(o);
        }
        list[m] = list[i]; list[m].a = a; list[m].l = l; m++;
    }
    if (gather_only) return;
    qsort(list, (size_t)m, sizeof *list, spr_cmp);

    int bw = cam->bw, bh = cam->bh;
    nspr_draw = 0;
    for (int i = 0; i < m; i++) {
        uint32_t o = list[i].obj;
        double a = list[i].a;
        uint8_t flags = MEMR8(o + 5);
        int8_t raise = (int8_t)MEMR8(o + 0x1a);
        uint16_t entry;
        int sw, sh;
        const uint8_t *pix = sprite_pixels(o, &entry, &sw, &sh);
        if (!pix) continue;
        double hh = cam->hs / (16.0 * a);   /* rows from horizon to the floor at this depth */
        double texel = hh / 32.0;           /* rows (and columns) per texel */
        double bottom = cam->hor_hi + (hh + 1.0 + cam->eye * hh / 32.0) * ky;
        if (raise > 0) bottom -= (raise * hh / 32.0 + 1.0) * ky;
        double height = sh * texel * ky, width = sw * texel * kx;
        double top = bottom - height;
        double centre = cam->cx_hi + list[i].l / a * cam->f * kx;
        double left = centre - width * 0.5;
        sprite_draw_t *d = &spr_draw[nspr_draw];
        d->c0 = (int)ceil(left - 0.5); d->c1 = (int)ceil(left + width - 0.5);
        d->r0 = (int)ceil(top - 0.5); d->r1 = (int)ceil(bottom - 0.5);
        if (d->c0 < 0) d->c0 = 0;
        if (d->c1 > bw) d->c1 = bw;
        if (d->r0 < 0) d->r0 = 0;
        if (d->r1 > bh) d->r1 = bh;
        if (d->c0 >= d->c1 || d->r0 >= d->r1) continue;
        d->tex = hq_get((uint32_t)ds32((uint16_t)(entry + 2)), pix, sw, sh, 1, NULL);
        if (!d->tex) continue;
        d->left = (float)left;
        d->top = (float)top;
        d->us = (float)(sw / width);
        d->vs = (float)(sh / height);
        d->sw = sw;
        d->sh = sh;
        d->depth = (float)a;
        d->ignore_depth = (flags & 0x10) != 0;
        d->lit = lighting && !light_is_source((uint32_t)ds32((uint16_t)(entry + 2)), pix, sw, sh);
        if (d->lit) { /* lit as at its middle */
            float L[3], z = (float)((raise > 0 ? raise : 0) + sh * 0.5);
            light_at3(list[i].x, list[i].y, L);
            if (z > 64) z = 64;
            d->lf = light_scale(z < 32 ? L[0] + (L[1] - L[0]) * z / 32 : L[1] + (L[2] - L[1]) * (z - 32) / 32);
        }
        nspr_draw++;
    }
    hq_build_pending();
    sprite_job_t job = {cam};
    hq_parallel(draw_sprite_columns, &job, bw);
}

/* ---------------------------------------------------------------- hooks */
static uint32_t view_pal_hash; /* palette the hd view was drawn with */

static uint32_t palette_hash(const uint8_t pal[256][3]) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 768; i++) h = (h ^ (&pal[0][0])[i]) * 16777619u;
    return h;
}

static void refresh_palette(void) {
    uint8_t pal[256][3];
    for (int i = 0; i < 256; i++) host_get_palette_entry(i, &pal[i][0], &pal[i][1], &pal[i][2]);
    hq_set_palette((const uint8_t(*)[3])pal);
}

/* Debug aid: N3D_HDCMP=dir renders the native view at the original 320x200
 * geometry right after the original renderer and writes both (raw 320x200
 * palette-index dumps + palette) every N3D_HDCMP_EVERY frames. */
static uint8_t rgb_to_index(uint32_t c) {
    for (int i = 0; i < 256; i++)
        if (hq_palette_color(i) == (c | 0xff000000u)) return (uint8_t)i;
    return 0;
}

static void compare_with_original(uint8_t *d) {
    static int n, every = -1;
    const char *dir = getenv("N3D_HDCMP");
    if (!dir) return;
    if (every < 0) every = getenv("N3D_HDCMP_EVERY") ? atoi(getenv("N3D_HDCMP_EVERY")) : 30;
    if (n++ % (every ? every : 1)) return;
    int sw = out_w, sh = out_h, s0 = RX0, s1 = RX1, s2 = RY0, s3 = RY1;
    double skx = kx, sky = ky, sox = ox, soy = oy;
    kx = ky = 1; ox = oy = 0;
    exact_camera = 1;
    hq_settings_t saved_hq = hq;
    hq.filter = HQ_NEAREST;
    hq.upscale = 1;
    refresh_palette();
    RX0 = 0; RX1 = 320; RY0 = vy0; RY1 = vy1 + 1;
    cam_t cam;
    project_setup(&cam);
    render_walls(&cam);
    no_animate = 1;
    render_sprites(&cam);
    no_animate = 0;
    static uint8_t img[2][64000];
    memcpy(img[0], d, 64000);
    memcpy(img[1], d, 64000);
    long diff = 0;
    for (int y = vy0; y <= vy1; y++)
        for (int x = vx0; x <= vx1; x++) {
            img[1][y * 320 + x] = rgb_to_index(view_buf[(y - vy0) * 320 + x]);
            diff += img[0][y * 320 + x] != img[1][y * 320 + x];
        }
    char path[1024];
    snprintf(path, sizeof path, "%s/cmp_%04d.raw", dir, n / (every ? every : 1));
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(img, 1, sizeof img, f);
        for (int i = 0; i < 256; i++) { uint8_t c[3]; host_get_palette_entry(i, &c[0], &c[1], &c[2]); fwrite(c, 1, 3, f); }
        fclose(f);
    }
    fprintf(stderr, "hdcmp %s: %ld of %d view pixels differ\n", path, diff, (vx1 - vx0 + 1) * (vy1 - vy0 + 1));
    exact_camera = 0;
    hq = saved_hq;
    out_w = sw; out_h = sh; RX0 = s0; RX1 = s1; RY0 = s2; RY1 = s3;
    kx = skx; ky = sky; ox = sox; oy = soy;
}

void hd_view_drawn(void) {
    uint8_t *d = dib();
    if (!out_w || !d) return;
    vx0 = ds16(0x53d4); vx1 = ds16(0x53d6); vy0 = ds16(0x53d8); vy1 = ds16(0x53da);
    if (vx0 < 0 || vx1 > 319 || vy0 < 0 || vy1 > 199 || vx0 > vx1 || vy0 > vy1) { frame_valid = 0; return; }
    wide = vx0 <= 8 && vx1 >= 311;
    RX0 = wide ? 0 : (int)lround(ox + vx0 * kx);
    RX1 = wide ? out_w : (int)lround(ox + (vx1 + 1) * kx);
    RY0 = (int)lround(oy + (wide ? 0 : vy0) * ky);
    RY1 = (int)lround(oy + (vy1 + 1) * ky);
    full_hud = wide && hud_active();
    view_shift = weapon_drop = 0;
    if (full_hud) {
        weapon_drop = out_h - RY1;
        view_shift = (out_h - RY1 - RY0) * 0.5;
        RY0 = 0;
        RY1 = out_h;
    }
    /* overscan: the corners a rolled view turns in from outside */
    VM = hd_roll_angle != 0 ? (int)ceil(sin(fabs(hd_roll_angle) * (BOB_PI / 180.0)) * ((RX1 - RX0) + (RY1 - RY0)) * 0.5) + 2 : 0;
    size_t need = (size_t)(RX1 - RX0 + 2 * VM) * (RY1 - RY0 + 2 * VM);
    if (need > view_cap) { free(view_buf); view_buf = malloc(need * sizeof *view_buf); view_cap = need; }
    if ((size_t)(RX1 - RX0 + 2 * VM) > col_cap) {
        col_cap = (size_t)(RX1 - RX0 + 2 * VM);
        free(zbuf); free(col_vec); free(col_t); free(col_tex); free(col_u); free(col_fu); free(col_umin); free(col_umax); free(col_L); free(col_y0); free(col_y1);
        zbuf = malloc(col_cap * sizeof *zbuf);
        col_vec = malloc(col_cap * sizeof *col_vec);
        col_t = malloc(col_cap * sizeof *col_t);
        col_tex = malloc(col_cap * sizeof *col_tex);
        col_u = malloc(col_cap * sizeof *col_u);
        col_fu = malloc(col_cap * sizeof *col_fu);
        col_umin = malloc(col_cap * sizeof *col_umin);
        col_umax = malloc(col_cap * sizeof *col_umax);
        col_L = malloc(col_cap * 3 * sizeof *col_L);
        col_y0 = malloc(col_cap * sizeof *col_y0);
        col_y1 = malloc(col_cap * sizeof *col_y1);
    }
    if (need < 320 * 200) need = 320 * 200;
    if (need > view_cap) { free(view_buf); view_buf = malloc(need * sizeof *view_buf); view_cap = need; }
    if (col_cap < 320) {
        col_cap = 320;
        free(zbuf); free(col_vec); free(col_t); free(col_tex); free(col_u); free(col_fu); free(col_umin); free(col_umax); free(col_L); free(col_y0); free(col_y1);
        zbuf = malloc(col_cap * sizeof *zbuf);
        col_vec = malloc(col_cap * sizeof *col_vec);
        col_t = malloc(col_cap * sizeof *col_t);
        col_tex = malloc(col_cap * sizeof *col_tex);
        col_u = malloc(col_cap * sizeof *col_u);
        col_fu = malloc(col_cap * sizeof *col_fu);
        col_umin = malloc(col_cap * sizeof *col_umin);
        col_umax = malloc(col_cap * sizeof *col_umax);
        col_L = malloc(col_cap * 3 * sizeof *col_L);
        col_y0 = malloc(col_cap * sizeof *col_y0);
        col_y1 = malloc(col_cap * sizeof *col_y1);
    }
    compare_with_original(d);
    take_snapshot();
    uint64_t t0 = host_time_us();
    /* The picture itself is drawn by hd_rerender on the display schedule; at
     * the game frame only the objects the wider view shows get animated. */
    cam_t cam;
    project_setup(&cam);
    gather_only = 1;
    render_sprites(&cam);
    gather_only = 0;
    if (getenv("N3D_HDPROF")) {
        static uint64_t acc;
        static int cnt;
        acc += host_time_us() - t0;
        if (++cnt == 100) { fprintf(stderr, "hd view %dx%d: %.2f ms/frame\n", RX1 - RX0, RY1 - RY0, acc / 100000.0); acc = 0; cnt = 0; }
    }
    memset(ov_anchor, 0, sizeof ov_anchor);
    memcpy(expected, d, sizeof expected);
    layer_anchor = 0;
    frame_valid = 1;
}

void hd_layer_begin(int anchor) {
    uint8_t *d = dib();
    if (!frame_valid || !d) return;
    if (layer_anchor) hd_layer_end();
    layer_anchor = anchor;
    memcpy(layer_save, d, sizeof layer_save);
    for (int y = vy0; y <= vy1; y++) memset(d + y * 320 + vx0, TRANSPARENT, (size_t)(vx1 - vx0 + 1));
}

void hd_layer_end(void) {
    uint8_t *d = dib();
    if (!frame_valid || !d || !layer_anchor) return;
    for (int y = vy0; y <= vy1; y++)
        for (int x = vx0; x <= vx1; x++) {
            int k = y * 320 + x;
            if (d[k] != TRANSPARENT) { ov_col[k] = d[k]; ov_anchor[k] = (uint8_t)layer_anchor; }
            else d[k] = layer_save[k];
        }
    layer_anchor = 0;
}

int hd_rerender(void) {
    if (!frame_valid || !out_w) return 0;
    uint64_t t0 = host_time_us();
    refresh_palette();
    {
        uint8_t pal[256][3];
        for (int i = 0; i < 256; i++) host_get_palette_entry(i, &pal[i][0], &pal[i][1], &pal[i][2]);
        view_pal_hash = palette_hash((const uint8_t(*)[3])pal);
    }
    update_bob();
    cam_t cam;
    project_setup(&cam);
    if (getenv("N3D_TURNLOG")) fprintf(stderr, "cam %llu %.2f %.2f %.2f\n", (unsigned long long)host_time_us(), cam.px, cam.py, atan2(cam.c, cam.s) * 57.2957795);
    lighting = light_enabled() && !exact_camera;
    if (lighting) {
        light_pass = 1;
        render_sprites(&cam);
        light_pass = 0;
        light_px = cam.px;
        light_py = cam.py;
    }
    render_walls(&cam);
    no_animate = 1;
    render_sprites(&cam);
    no_animate = 0;
    if (getenv("N3D_HDPROF")) {
        static uint64_t acc, mx;
        static int cnt;
        uint64_t d = host_time_us() - t0;
        acc += d;
        if (d > mx) mx = d;
        if (++cnt == 30) {
            fprintf(stderr, "rerender: %.2f ms avg, %.2f max, %u texture builds\n", acc / 30000.0, mx / 1000.0, hq_take_build_count());
            acc = mx = 0;
            cnt = 0;
        }
    }
    return 1;
}

int hd_layer_current(void) { return frame_valid ? layer_anchor : 0; }
int hd_full_hud(void) { return frame_valid && full_hud; }

void hd_overlays_done(void) {
    uint8_t *d = dib();
    if (!frame_valid || !d) return;
    hd_layer_end();
    memcpy(expected, d, sizeof expected);
}

/* ---------------------------------------------------------------- compositor */
/* Full-screen pictures (title, menus, the status bar frame around the view)
 * with a replacement in the art pack: the frame is matched against the
 * original picture; pixels that still show it come from the replacement,
 * anything the game drew over it (menu text, HUD numbers) from the frame. */
typedef struct {
    const uint8_t *frame;
    const uint32_t *lut;
    const int *xmap, *ymap;
    uint32_t *out;
    int pitch, use_hd;
    const uint8_t *pic; /* matched UI picture (row-major indices) or NULL */
    const hq_tex *tex;  /* its replacement */
    int tw;             /* replacement width in texels (320, or wider: original centred) */
} compose_job_t;

static const hd_pack_img *match_ui(const uint8_t *frame, int skip_view) {
    const hd_pack_img *best = NULL;
    int best_n = 0, total = 0;
    for (int i = 0; i < hd_pack_ui_count(); i++) {
        const hd_pack_img *m = hd_pack_ui(i);
        int n = 0, t = 0;
        for (int k = 0; k < 64000; k += 7) {
            int x = k % 320, y = k / 320;
            if (skip_view && x >= vx0 && x <= vx1 && y >= vy0 && y <= vy1) continue;
            if (m->idx[k] == m->bg) continue; /* a blank frame would match a mostly blank picture */
            n += frame[k] == m->idx[k];
            t++;
        }
        if (n > best_n) { best_n = n; best = m; total = t; }
    }
    return total && best_n * 10 >= total * 6 ? best : NULL; /* 60% of the picture still visible */
}

static inline uint32_t frame_px(const compose_job_t *j, int x, int y, int gy) {
    int gx = j->xmap[x];
    if (j->pic) {
        if (gy < 0) return 0xff000000u;
        if (gx >= 0 && j->frame[gy * 320 + gx] != j->pic[gy * 320 + gx]) return j->lut[j->frame[gy * 320 + gx]];
        double u = (x + 0.5 - ox) / kx + (j->tw - 320) * 0.5;
        if (u < 0 || u >= j->tw) return 0xff000000u;
        return 0xff000000u | hq_sample(j->tex, (float)u, (float)((y + 0.5 - oy) / ky), (float)(1.0 / kx), (float)(1.0 / ky));
    }
    if (gy < 0 || gx < 0) return 0xff000000u;
    return j->lut[j->frame[gy * 320 + gx]];
}

/* The screen's picture that best fills the output: the widest that fits,
 * else the narrowest (cropped at the sides). */
static const hd_pack_img *pick_wide(const hd_pack_img *ui) {
    double avail = out_w / kx + 2; /* output width in texels (+ rounding slack) */
    const hd_pack_img *fit = NULL, *narrowest = NULL;
    const hd_pack_img *c[5];
    int n = 0;
    if (ui->rgba) c[n++] = ui;
    for (int i = 0; i < ui->nwide; i++) c[n++] = ui->wide[i];
    for (int i = 0; i < n; i++) {
        if (c[i]->w <= avail && (!fit || c[i]->w > fit->w)) fit = c[i];
        if (!narrowest || c[i]->w < narrowest->w) narrowest = c[i];
    }
    return fit ? fit : narrowest;
}

static void compose_rows(void *ctx, int y0, int y1) {
    const compose_job_t *j = ctx;
    int m = view_m, vw = RX1 - RX0 + 2 * m, vh = RY1 - RY0 + 2 * m;
    /* view roll: turn the view about its centre (the world leans the other way) */
    double ra = view_roll * (BOB_PI / 180.0), rc = cos(ra), rs = sin(ra);
    int rolled = fabs(view_roll) > 0.01 && m > 0;
    double ccx = (RX1 - RX0) * 0.5, ccy = (RY1 - RY0) * 0.5;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = j->out + (size_t)y * j->pitch;
        int gy = j->ymap[y];
        int view = j->use_hd && y >= RY0 && y < RY1;
        int xs = view ? RX0 : out_w, xe = view ? RX1 : out_w;
        for (int x = 0; x < xs; x++) row[x] = frame_px(j, x, y, gy);
        if (view && rolled) {
            /* the source point steps by (rc, rs) per pixel: 16.16 fixed point, bilinear on two channel pairs */
            double dy = y - RY0 + 0.5 - ccy, dx = 0.5 - ccx;
            int32_t u = (int32_t)lround((dx * rc - dy * rs + ccx + m - 0.5) * 65536.0);
            int32_t v = (int32_t)lround((dx * rs + dy * rc + ccy + m - 0.5) * 65536.0);
            int32_t du = (int32_t)lround(rc * 65536.0), dv = (int32_t)lround(rs * 65536.0);
            int32_t umax = (vw - 2) << 16, vmax = (vh - 2) << 16;
            for (int x = RX0; x < RX1; x++, u += du, v += dv) {
                int32_t cu = u < 0 ? 0 : u > umax ? umax : u, cv = v < 0 ? 0 : v > vmax ? vmax : v;
                uint32_t fx = (uint32_t)(cu >> 8) & 255, fy = (uint32_t)(cv >> 8) & 255;
                const uint32_t *p = view_buf + (size_t)(cv >> 16) * vw + (cu >> 16);
                uint32_t a = p[0], b = p[1], c = p[vw], d = p[vw + 1];
                uint32_t rb0 = ((a & 0xff00ff) * (256 - fx) + (b & 0xff00ff) * fx) >> 8 & 0xff00ff;
                uint32_t g0 = ((a & 0xff00) * (256 - fx) + (b & 0xff00) * fx) >> 8 & 0xff00;
                uint32_t rb1 = ((c & 0xff00ff) * (256 - fx) + (d & 0xff00ff) * fx) >> 8 & 0xff00ff;
                uint32_t g1 = ((c & 0xff00) * (256 - fx) + (d & 0xff00) * fx) >> 8 & 0xff00;
                row[x] = 0xff000000u | ((rb0 * (256 - fy) + rb1 * fy) >> 8 & 0xff00ff) | ((g0 * (256 - fy) + g1 * fy) >> 8 & 0xff00);
            }
        } else if (view) memcpy(row + RX0, view_buf + (size_t)(y - RY0 + m) * vw + m, sizeof(uint32_t) * (size_t)(RX1 - RX0));
        for (int x = xe; x < out_w; x++) row[x] = frame_px(j, x, y, gy);
    }
}

/* Overlay layers (weapon, frame decorations, messages): captured at 320x200,
 * drawn upscaled and filtered like the textures, anchored per layer. */
typedef struct {
    const hq_tex *tex;
    int bx0, by0;        /* layer box in 320x200 coordinates */
    int bw, bh;
    double X0, Y0;       /* box origin on the output */
    int xa, xb, ya, yb;  /* output pixels covered */
    uint32_t *out;
    int pitch;
    int lit;             /* the weapon: shaded by the light where the player stands */
    const uint16_t *lf;
} layer_job_t;

static void draw_layer_columns(void *ctx, int c0, int c1) {
    const layer_job_t *j = ctx;
    uint32_t tmp[4096];
    int n = j->yb - j->ya;
    if (n <= 0) return;
    if (n > 4096) n = 4096;
    float fu = (float)(1.0 / kx), fv = (float)(1.0 / ky);
    for (int c = c0; c < c1; c++) {
        int x = j->xa + c;
        float u = (float)((x + 0.5 - j->X0) / kx);
        float v = (float)((j->ya + 0.5 - j->Y0) / ky);
        if (u < 0 || u >= j->bw) continue;
        hq_column(j->tex, u, fu, v, fv, n, tmp, 1);
        uint32_t *dst = j->out + (size_t)j->ya * j->pitch + x;
        if (j->lit)
            for (int k = 0; k < n; k++) tmp[k] = light_apply(tmp[k], j->lf);
        for (int k = 0; k < n; k++, dst += j->pitch) over(dst, tmp[k]);
    }
}

/* Hit reaction: when health drops, the edges of the screen flash red, more
 * for a bigger hit, fading out over about half a second. */
int hd_hit_flash = 1;
typedef struct { uint32_t *out; int pitch; float k, band; } flash_job_t;

static void flash_rows(void *ctx, int y0, int y1) {
    const flash_job_t *j = ctx;
    for (int y = y0; y < y1; y++) {
        float dy = (float)(y < out_h - 1 - y ? y : out_h - 1 - y);
        uint32_t *row = j->out + (size_t)y * j->pitch;
        int full = dy < j->band; /* rows near the top or bottom: every pixel, else only the sides */
        for (int x = 0; x < out_w; x++) {
            if (!full && x >= (int)j->band && x < out_w - (int)j->band) { x = out_w - (int)j->band - 1; continue; }
            float dx = (float)(x < out_w - 1 - x ? x : out_w - 1 - x);
            float d = (dx < dy ? dx : dy) / j->band;
            if (d >= 1) continue;
            float e = (1 - d) * (1 - d);
            uint32_t a = (uint32_t)(j->k * e * 220), c = row[x];
            uint32_t r = ((c >> 16) & 255) * (255 - a) / 255 + a * 200 / 255, g = ((c >> 8) & 255) * (255 - a) / 255, b = (c & 255) * (255 - a) / 255;
            row[x] = 0xff000000u | r << 16 | g << 8 | b;
        }
    }
}

static void hit_flash(uint32_t *out, int pitch) {
    static int last_health = -1;
    static float level;
    static uint64_t last_t;
    uint64_t now = host_time_us();
    float dt = last_t ? (float)((now - last_t) / 1e6) : 0;
    last_t = now;
    int playing = ds8(0x46a4) == 1, health = ds8(0x4c0d);
    if (!playing) { last_health = -1; level = 0; return; }
    if (last_health >= 0 && health < last_health) {
        float hit = 0.45f + (last_health - health) * 0.04f;
        level = level + hit > 1 ? 1 : level + hit;
    }
    last_health = health;
    level -= dt * 2.0f;
    if (level <= 0 || !hd_hit_flash) { level = level < 0 ? 0 : level; return; }
    flash_job_t j = {out, pitch, level, (float)(out_w < out_h ? out_w : out_h) * 0.22f};
    hq_parallel(flash_rows, &j, out_h);
}

void hd_compose(const uint8_t *frame, const uint8_t pal[256][3], uint32_t *out, int pitch) {
    uint32_t lut[256];
    for (int i = 0; i < 256; i++) lut[i] = 0xff000000u | ((uint32_t)pal[i][0] << 16) | ((uint32_t)pal[i][1] << 8) | pal[i][2];
    static int *xmap, *ymap, map_w, map_h;
    if (map_w != out_w || map_h != out_h) {
        free(xmap); free(ymap);
        xmap = malloc(sizeof(int) * out_w);
        ymap = malloc(sizeof(int) * out_h);
        for (int x = 0; x < out_w; x++) { int g = (int)floor((x + 0.5 - ox) / kx); xmap[x] = g < 0 || g > 319 ? -1 : g; }
        for (int y = 0; y < out_h; y++) { int g = (int)floor((y + 0.5 - oy) / ky); ymap[y] = g < 0 || g > 199 ? -1 : g; }
        map_w = out_w; map_h = out_h;
    }
    int use_hd = frame_valid;
    if (use_hd) {
        /* Anything drawn over the view after the regular overlays (pause text, ...)
         * is an extra centred overlay; if most of the view changed, the game is
         * showing something else and the plain 320x200 frame is used. */
        int changed = 0, total = (vx1 - vx0 + 1) * (vy1 - vy0 + 1);
        for (int y = vy0; y <= vy1; y++)
            for (int x = vx0; x <= vx1; x++) changed += frame[y * 320 + x] != expected[y * 320 + x];
        if (changed * 4 > total) use_hd = 0;
        else if (changed)
            for (int y = vy0; y <= vy1; y++)
                for (int x = vx0; x <= vx1; x++) {
                    int k = y * 320 + x;
                    if (frame[k] != expected[k]) { ov_col[k] = frame[k]; ov_anchor[k] = HD_ANCHOR_CENTER; expected[k] = frame[k]; }
                }
    }
    /* palette animations (fades, flashes) run without new game frames: the
     * view's colours were resolved with the palette of its last drawing */
    if (use_hd && palette_hash(pal) != view_pal_hash) hd_rerender();
    compose_job_t cj = {frame, lut, xmap, ymap, out, pitch, use_hd, NULL, NULL, 320};
    const hd_pack_img *ui = hd_pack_ui_count() ? match_ui(frame, use_hd) : NULL;
    const hd_pack_img *pic = ui ? pick_wide(ui) : NULL;
    if (pic && use_hd) pic = ui->rgba ? ui : NULL; /* around the 3D view: only the 4:3 frame */
    if (pic) {
        hq_set_palette(pal);
        cj.tex = hq_get_ui(ui, pic);
        hq_build_pending();
        if (cj.tex) { cj.pic = ui->idx; cj.tw = pic->w; }
    }
    hq_parallel(compose_rows, &cj, out_h);
    if (!use_hd) return;
    refresh_palette();
    /* overlays, each anchored to the matching part of the widened view */
    double shift[5] = {0, 0, RX0 - (ox + vx0 * kx), RX1 - (ox + (vx1 + 1) * kx), bob_wx * kx};
    double shift_y[5] = {0, 0, 0, 0, weapon_drop + bob_wy * ky};
    for (int an = 1; an <= 4; an++) {
        int bx0 = 320, by0 = 200, bx1 = -1, by1 = -1;
        for (int y = vy0; y <= vy1; y++)
            for (int x = vx0; x <= vx1; x++)
                if (ov_anchor[y * 320 + x] == an) {
                    if (x < bx0) bx0 = x;
                    if (x > bx1) bx1 = x;
                    if (y < by0) by0 = y;
                    if (y > by1) by1 = y;
                }
        if (bx1 < 0) continue;
        /* pad by one texel so filtering fades out at the edges */
        if (bx0 > 0) bx0--;
        if (by0 > 0) by0--;
        if (bx1 < 319) bx1++;
        if (by1 < 199) by1++;
        int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
        static uint8_t img[64000];
        for (int x = 0; x < bw; x++)
            for (int y = 0; y < bh; y++) {
                int k = (by0 + y) * 320 + bx0 + x;
                img[x * bh + y] = ov_anchor[k] == an ? ov_col[k] : TRANSPARENT;
            }
        uint32_t key = 2166136261u;
        for (int i = 0; i < bw * bh; i++) key = (key ^ img[i]) * 16777619u;
        key = (key ^ (uint32_t)(bx0 << 20 | by0 << 10 | an)) * 16777619u;
        layer_job_t lj;
        lj.tex = hq_get(key, img, bw, bh, 1, NULL);
        if (!lj.tex) continue;
        hq_build_pending();
        lj.bx0 = bx0; lj.by0 = by0; lj.bw = bw; lj.bh = bh;
        lj.X0 = ox + bx0 * kx + shift[an];
        lj.Y0 = oy + by0 * ky + shift_y[an];
        lj.xa = (int)lround(lj.X0); lj.xb = (int)lround(lj.X0 + bw * kx);
        lj.ya = (int)lround(lj.Y0); lj.yb = (int)lround(lj.Y0 + bh * ky);
        if (lj.xa < 0) lj.xa = 0;
        if (lj.xb > out_w) lj.xb = out_w;
        if (lj.ya < 0) lj.ya = 0;
        if (lj.yb > RY1) lj.yb = RY1; /* the bobbing weapon stays inside the view */
        lj.out = out;
        lj.pitch = pitch;
        lj.lit = an == HD_ANCHOR_WEAPON && lighting;
        if (lj.lit) lj.lf = light_scale(light_at(light_px, light_py, 1) + 0.2f); /* a little extra: it stays readable */
        if (lj.xb > lj.xa) hq_parallel(draw_layer_columns, &lj, lj.xb - lj.xa);
    }
    hit_flash(out, pitch);
    if (full_hud) hud_draw(out, pitch, out_w, out_h);
}
