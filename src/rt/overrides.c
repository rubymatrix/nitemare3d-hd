/* Hand-written replacements for recompiled functions listed in overrides.txt.
 *
 * Each override is entered exactly like the generated code: the caller has
 * pushed its arguments and a far return address (CS, IP) on the emulated
 * stack. Being cdecl far functions, overrides leave the arguments alone and
 * only pop the 4-byte return address (the RETF the caller expects). */
#include "rt/rt.h"
#include "gen/rc_funcs.h"
#include "hd.h"
#include "net.h"
#include "rt/host.h"

static uint16_t ds16(uint16_t off) { return MEMR16(SEGLIN(RT_SEL_DS, off)); }
static void retf(void) { rSP = (uint16_t)(rSP + 4); }
static uint16_t arg16(int i) { return MEMR16(SEGLIN(rSS, (uint16_t)(rSP + 4 + 2 * i))); }

/* 1010:d6de draw_view: build the visible wall list, draw floor/ceiling,
 * walls and sprites into the WinG DIB. The original sequence is kept (it
 * also has game-logic side effects: automap reveal, wall animation, enemy
 * targeting), then the native renderer gets a chance to snapshot it. */
void f_1010_d6de(void) {
    RT_ENTER(0x1010d6deu);
    net_view_begin();
    hd_view_begin();
    MEMW32(SEGLIN(RT_SEL_DS, 0x53cc), MEMR32(SEGLIN(RT_SEL_DS, 0x53cc)) + 1);
    rt_callf(0x1018387eu, 0, NULL); /* collect visible vectors per column */
    rt_callf(0x101061c2u, 0, NULL); /* split into wall segments */
    {
        uint16_t a[4] = {ds16(0x53d4), ds16(0x53d8), ds16(0x53d0), ds16(0x53d2)};
        rt_callf(0x10103d14u, 4, a); /* floor + ceiling */
    }
    rt_callf(0x1010660cu, 0, NULL); /* textured walls */
    rt_callf(0x101062a4u, 0, NULL); /* queue visible objects */
    rt_callf(0x10106870u, 0, NULL); /* draw queued sprites */
    hd_view_drawn();
    net_view_end();
    retf();
}

/* 1010:b9a6: everything drawn over the 3D view each frame, through
 * FUN_1010_a308 (HUD element painter): 0x12 frame decorations + current
 * message, 2 the weapon, 0x18 the crosshair/indicator. Each is captured as
 * an overlay layer for the hi-res compositor (the weapon as its own, so it
 * can bob and sit at the bottom edge under the full-screen HUD). */
void f_1010_b9a6(void) {
    RT_ENTER(0x1010b9a6u);
    static const uint16_t parts[3] = {0x12, 2, 0x18};
    for (int i = 0; i < 3; i++) {
        hd_layer_begin(i == 1 ? HD_ANCHOR_WEAPON : HD_ANCHOR_CENTER); /* the weapon bobs on its own */
        rt_callf(0x1010a308u, 1, &parts[i]);
    }
    hd_overlays_done();
    retf();
}

/* 1018:035a draw_view_picture(x, y, picture, flags): the gargoyles in the
 * view corners and the ornament at the top are drawn with it (FUN_1010_a308
 * part 0x12, pictures DS:37c8/37d2/37dc); anchor them to the edges of a
 * widened view, or leave them out under the full-screen gothic HUD. */
void f_1018_035a(void) {
    int prev = hd_layer_current();
    if (prev) {
        int x = (int16_t)MEMR16(SEGLIN(rSS, (uint16_t)(rSP + 4)));
        uint16_t pic = MEMR16(SEGLIN(rSS, (uint16_t)(rSP + 8)));
        if (hd_full_hud() && (pic == 0x37c8 || pic == 0x37d2 || pic == 0x37dc)) { retf(); return; }
        hd_layer_begin(x < 100 ? HD_ANCHOR_LEFT : x > 220 ? HD_ANCHOR_RIGHT : HD_ANCHOR_CENTER);
    }
    f_1018_035a_orig(); /* pops the return address itself */
    if (prev) hd_layer_begin(prev);
}

/* 1010:9758 player_tick: the original reads the arrow/Ctrl/Space/Alt bits in
 * DS:3752/3753 (Alt+Left/Right strafes) and moves through FUN_1010_8a08
 * (angle, sideways, speed: collision, doors and pickups included). Modern
 * controls are added afterwards through the same calls: W/S forward/back,
 * A/D strafe, mouse X turns via FUN_1010_e40a (set view angle). Skipped while
 * a demo is recorded or played (DS:46a8 1..4), which only stores DS:3752. */
static void modern_controls(int remote);

static void move_player(int angle, int sideways, int speed) {
    uint16_t a[3] = {(uint16_t)(((angle % 360) + 360) % 360), (uint16_t)sideways, (uint16_t)speed};
    rt_callf(0x10108a08u, 3, a);
}

void f_1010_9758(void) {
    if (net_is_client()) { /* the host moves us: send it our input instead */
        net_player_ticked();
        retf();
        return;
    }
    int remote = net_in_remote(); /* co-op host running the other player's tick */
    int playing = MEMR8(SEGLIN(RT_SEL_DS, 0x46a4)) == 1;
    f_1010_9758_orig(); /* pops the return address itself */
    int demo = (int16_t)ds16(0x46a8);
    if (playing && MEMR8(SEGLIN(RT_SEL_DS, 0x46a4)) == 1 && !(demo >= 1 && demo <= 4)) modern_controls(remote);
    if (!remote) net_player_ticked();
}

static void modern_controls(int remote) {
    int fwd = 0, side = 0, turn = 0;
    if (remote ? !net_remote_input(&fwd, &side) : !host_modern_input(&fwd, &side, &turn)) return;
    uint8_t keys = MEMR8(SEGLIN(RT_SEL_DS, 0x3752));
    int speed = (int16_t)ds16(0x53e6);
    if (keys & 0x20) speed *= 2; /* right shift: run */
    if (keys & 0x40) speed = 1;  /* left shift: fine control */
    if (turn) {
        uint16_t a[1] = {(uint16_t)(int16_t)((int16_t)ds16(0x4bda) + turn)};
        rt_callf(0x1010e40au, 1, a);
    }
    int angle = (int16_t)ds16(0x4bda);
    if (fwd > 0 && !(keys & 2)) move_player(angle, 0, speed);
    if (fwd < 0 && !(keys & 4)) move_player(angle + 180, 0, speed);
    if (side < 0) move_player(angle + 270, 1, speed);
    if (side > 0) move_player(angle + 90, 1, speed);
}

/* 1010:cbce queue_object_sprite(object far*): projects one object, queues it
 * for drawing and advances its animation. The hi-res renderer animates the
 * objects that only its wider view shows the same way (hd_animate_object). */
void f_1010_cbce(void) {
    uint32_t obj = MEMR32(SEGLIN(rSS, (uint16_t)(rSP + 4)));
    uint32_t lin = SEGLIN(obj >> 16, obj & 0xffff);
    uint8_t frame = MEMR8(lin + 3); /* the queued slot keeps the frame from before the animation step */
    f_1010_cbce_orig();
    if (net_offscreen()) return; /* co-op host drawing the other player's view */
    for (int i = 0; i < 100; i++) { /* sprite slots DS:6260, 0x12 bytes: [0]=0 queued, [2] object */
        uint16_t slot = (uint16_t)(0x6260 + i * 0x12);
        if (MEMR8(SEGLIN(RT_SEL_DS, slot)) == 0 && MEMR32(SEGLIN(RT_SEL_DS, (uint16_t)(slot + 2))) == obj) {
            hd_object_queued(lin, frame);
            break;
        }
    }
}

void hd_animate_object(uint32_t obj) {
    uint8_t type = MEMR8(obj + 6), set = MEMR8(obj + 4);
    if (type == 5) return;
    uint16_t a[3] = {(uint16_t)obj, (uint16_t)(obj >> 16), (uint16_t)(0x4734 + set * 8)};
    if (type == 0x2c || type == 0x2d) rt_callf(0x1010ca78u, 3, a); /* 8-direction figure */
    else if (ds16((uint16_t)(0x4736 + set * 8))) rt_callf(0x1010cb26u, 3, a);
}

/* ---- co-op (src/net.c) ---- */

/* 1010:7ab2 guard AI (guard far*, object far*): with the context of the
 * player this guard chases, so it sees, follows and hurts that player. */
void f_1010_7ab2(void) {
    net_guard_begin(SEGLIN(arg16(3), arg16(2)));
    f_1010_7ab2_orig();
    net_guard_end();
}

/* 1010:ceb2 pick up the item in a map cell: on the co-op client the host
 * does it (its world and our player record arrive with the next snapshot). */
void f_1010_ceb2(void) {
    if (net_is_client()) { retf(); return; }
    f_1010_ceb2_orig();
}

/* 1010:d8c6 logic tick (frame number): guards, powers, floor damage, weapon cooldown. */
void f_1010_d8c6(void) {
    if (net_is_client()) { retf(); return; }
    uint16_t lo = arg16(0), hi = arg16(1);
    f_1010_d8c6_orig();
    net_logic_ticked(lo, hi);
}

/* 1010:d84e dying turn, the first thing of every drawn frame. */
void f_1010_d84e(void) {
    if (net_is_client()) { net_client_frame(); retf(); return; }
    f_1010_d84e_orig();
}

/* 1010:1db4 move doors. */
void f_1010_1db4(void) {
    if (net_is_client()) { retf(); return; }
    f_1010_1db4_orig();
}

/* 1010:9d72 move projectiles and queue their sprites. */
void f_1010_9d72(void) {
    if (net_is_client()) { net_client_projectiles(); retf(); return; }
    f_1010_9d72_orig();
}

/* 1010:1d02 count down open doors and close them when nobody is in the way. */
void f_1010_1d02(void) {
    net_doors_hold();
    f_1010_1d02_orig();
}

/* 1010:e2a4 play sound (id, int, byte). */
void f_1010_e2a4(void) {
    uint16_t a[3] = {arg16(0), arg16(1), arg16(2)};
    f_1010_e2a4_orig();
    net_sound(a);
}

/* 1018:06e6 modal message (text far*, arg): a note the remote player read is theirs. */
void f_1018_06e6(void) {
    if (net_message(SEGLIN(arg16(1), arg16(0)), arg16(2))) { retf(); return; }
    f_1018_06e6_orig();
}

/* 1010:a308 draw HUD element: the host's screen shows the host's values. */
void f_1010_a308(void) {
    if (net_in_remote()) { rAX = 0; retf(); return; }
    f_1010_a308_orig();
}

/* 1010:b0f6 palette effects 3..7, 9 (hurt, powers): the host's colours stay the host's. */
void f_1010_b0f6(void) {
    uint16_t mode = arg16(0);
    if (net_in_remote() && mode != 2 && mode != 8) { rAX = 0; rDX = 0; retf(); return; }
    f_1010_b0f6_orig();
}

/* 1018:08c0 load level (level, episode). */
void f_1018_08c0(void) {
    f_1018_08c0_orig();
    net_level_loaded();
}
