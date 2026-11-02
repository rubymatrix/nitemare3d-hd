/* Two-player co-op over TCP: host-authoritative simulation, see net.h.
 *
 * Wire format: every message is [u32 length][u8 type][payload], little
 * endian. The host sends a snapshot after each frame it draws (25 a second):
 * the world as delta records against what it sent before, the client's own
 * player record, where the host player is and the sounds played since. The
 * client sends its keys, W/A/S/D and its view angle once per drawn frame; the
 * view angle is the client's own, so turning has no network delay. */
#include "net.h"
#include "rt/rt.h"
#include "rt/host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
/* The browser has no sockets; multiplayer is a desktop feature. */
int net_host(int port) { (void)port; return 0; }
int net_join(const char *addr) { (void)addr; return 0; }
void net_poll(void) {}
void net_shutdown(void) {}
int net_is_host(void) { return 0; }
int net_is_client(void) { return 0; }
int net_in_remote(void) { return 0; }
int net_offscreen(void) { return 0; }
void net_player_ticked(void) {}
void net_logic_ticked(uint16_t lo, uint16_t hi) { (void)lo; (void)hi; }
void net_client_frame(void) {}
void net_view_begin(void) {}
int net_status(const char **lines, int max) { (void)lines; (void)max; return 0; }
void net_view_end(void) {}
void net_client_projectiles(void) {}
void net_level_loaded(void) {}
void net_guard_begin(uint32_t obj) { (void)obj; }
void net_guard_end(void) {}
void net_doors_hold(void) {}
void net_sound(const uint16_t args[3]) { (void)args; }
int net_message(uint32_t text, uint16_t arg) { (void)text; (void)arg; return 0; }
int net_remote_input(int *forward, int *strafe) { (void)forward; (void)strafe; return 0; }
int net_peer(uint32_t *obj, double *x, double *y) { (void)obj; (void)x; (void)y; return 0; }
const uint8_t *net_peer_pixels(uint32_t obj, int *w, int *h, uint32_t *key) { (void)obj; (void)w; (void)h; (void)key; return NULL; }
#else

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#define WOULD_BLOCK() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
#define WOULD_BLOCK() (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS)
#endif

/* ---------------------------------------------------------------- game memory */
#define SEL_WORLD 0x1040
#define WORLD_LEN 50846       /* segment 1040: walls, objects, guards, doors, map */
#define PL_OFF 0x4bd8         /* player record, as the save game stores it */
#define PL_LEN 0x62           /* ... up to and including the view cos/sin at 4c36/4c38 */
#define OBJ_BASE 0x6d66       /* objects in segment 1040, 0x1c bytes each */
#define OBJ_SIZE 0x1c
#define OBJ_MAX 350
#define GUARD_BASE 0x93ae     /* guards in segment 1040, 0x1a bytes each */
#define DOOR_BASE 0x9dd6      /* doors in segment 1040, 0x16 bytes each, count at 1040:0000 */
#define FULL_HEALTH 100

/* offsets inside the player record */
#define P_ANGLE (0x4bda - PL_OFF)
#define P_X (0x4be6 - PL_OFF)
#define P_Y (0x4be8 - PL_OFF)
#define P_CELL (0x4c00 - PL_OFF)  /* far pointer to the map cell the player stands on */
#define P_HEALTH (0x4c0d - PL_OFF)
#define P_HURT (0x4bfe - PL_OFF)  /* red flash frames left (FUN_1010_bb1c) */
#define P_POWER1 (0x4c1c - PL_OFF)
#define P_POWER2 (0x4c1d - PL_OFF)
#define P_LOC0 (0x4bda - PL_OFF)  /* where the player is and faces: 4bda..4c03 */
#define P_LOC1 (0x4c04 - PL_OFF)

#define F_SET_ANGLE 0x1010e40au   /* set_view_angle(degrees) */
#define F_SET_POS 0x1010897cu     /* place the player at (x, y) */
#define F_PLAYER_TICK 0x10109758u
#define F_DYING_TURN 0x1010d84eu  /* turn to face the killer, then state 3 */
#define F_QUEUE_SPRITE 0x1010cbceu
#define F_PALETTE_FX 0x1010b0f6u
#define F_SOUND 0x1010e2a4u
#define F_MESSAGE 0x101806e6u
#define F_MENU_CMD 0x101826acu

static uint8_t ds8(uint16_t o) { return MEMR8(SEGLIN(RT_SEL_DS, o)); }
static uint16_t ds16(uint16_t o) { return MEMR16(SEGLIN(RT_SEL_DS, o)); }
static uint32_t ds32(uint16_t o) { return MEMR32(SEGLIN(RT_SEL_DS, o)); }
static void wds8(uint16_t o, uint8_t v) { MEMW8(SEGLIN(RT_SEL_DS, o), v); }
static void wds16(uint16_t o, uint16_t v) { MEMW16(SEGLIN(RT_SEL_DS, o), v); }
static void wds32(uint16_t o, uint32_t v) { MEMW32(SEGLIN(RT_SEL_DS, o), v); }
static uint8_t *ds_ptr(uint16_t o) { return LP(SEGLIN(RT_SEL_DS, o)); }
static uint8_t *world_ptr(uint16_t o) { return LP(SEGLIN(SEL_WORLD, o)); }
static void call0(uint32_t fn) { rt_callf(fn, 0, NULL); }
static void call1(uint32_t fn, uint16_t a) { rt_callf(fn, 1, &a); }
static void call2(uint32_t fn, uint16_t a, uint16_t b) { uint16_t v[2] = {a, b}; rt_callf(fn, 2, v); }

/* What the client mirrors. Region 0 is the world segment; the rest are DS. */
typedef struct { uint16_t sel, off, len; } region_t;
static const region_t regions[] = {
    {SEL_WORLD, 0, WORLD_LEN},
    {RT_SEL_DS, 0x4c3a, 0x150},             /* projectiles, 8 x 0x2a */
    {RT_SEL_DS, 0x5194, 8},                 /* switches */
    {RT_SEL_DS, 0x7e42, 0x12},              /* episode, level, counts, floor/ceiling colours */
    {RT_SEL_DS, 0x8084, 0x100},             /* darkening remap */
    {RT_SEL_DS, 0x838c, 2 + OBJ_MAX * 4},   /* object list */
};
#define NREG ((int)(sizeof regions / sizeof regions[0]))
static uint8_t *shadow[NREG]; /* host: what the client has; client: what the host sent */

static uint8_t *reg_live(int r) { return LP(SEGLIN(regions[r].sel, regions[r].off)); }

/* ---------------------------------------------------------------- per-player context */
typedef struct {
    uint8_t pl[PL_LEN];
    uint8_t keys[2];   /* DS:3752/3753 key bits */
    uint8_t edge[2];   /* DS:012c/012d last use/fire state */
    uint8_t typed;     /* DS:0108 last typed key */
    uint8_t state;     /* DS:46a4: 1 playing, 2 dying, 3 dead */
    uint8_t dying;     /* DS:469c */
    uint8_t cool;      /* DS:01fa weapon cooldown */
} pctx_t;

static pctx_t ctx[2], start_ctx[2];
static int cur;             /* context in the game's globals */
static uint8_t host_input_flags[2]; /* DS:4bd0 mouse, 4bd3 joystick of the host */
static int remote_ready;    /* ctx[1] holds a player in the current level */
static int offscreen_pass;
static uint16_t epoch;      /* bumped when the host moves the remote player (level start, respawn) */

static void ctx_save(pctx_t *c) {
    memcpy(c->pl, ds_ptr(PL_OFF), PL_LEN);
    c->keys[0] = ds8(0x3752); c->keys[1] = ds8(0x3753);
    c->edge[0] = ds8(0x012c); c->edge[1] = ds8(0x012d);
    c->typed = ds8(0x0108);
    c->state = ds8(0x46a4);
    c->dying = ds8(0x469c);
    c->cool = ds8(0x01fa);
}

static void ctx_load(const pctx_t *c) {
    memcpy(ds_ptr(PL_OFF), c->pl, PL_LEN);
    wds8(0x3752, c->keys[0]); wds8(0x3753, c->keys[1]);
    wds8(0x012c, c->edge[0]); wds8(0x012d, c->edge[1]);
    wds8(0x0108, c->typed);
    wds8(0x46a4, c->state);
    wds8(0x469c, c->dying);
    wds8(0x01fa, c->cool);
}

static void swap_to(int i) {
    if (cur == i) return;
    ctx_save(&ctx[cur]);
    if (cur == 0) { host_input_flags[0] = ds8(0x4bd0); host_input_flags[1] = ds8(0x4bd3); }
    ctx_load(&ctx[i]);
    if (i == 0) { wds8(0x4bd0, host_input_flags[0]); wds8(0x4bd3, host_input_flags[1]); }
    else { wds8(0x4bd0, 0); wds8(0x4bd3, 0); } /* the host's mouse and joystick are not theirs */
    cur = i;
}

static int16_t pl16(const pctx_t *c, int off) { int16_t v; memcpy(&v, c->pl + off, 2); return v; }

/* ---------------------------------------------------------------- sockets */
static int role;            /* 0 none, 1 host, 2 client */
static sock_t listen_sock = BAD_SOCK, peer = BAD_SOCK;
static uint8_t *inbuf, *outbuf;
static size_t in_len, in_cap, out_len, out_cap;

enum { M_HELLO = 1, M_WELCOME, M_LEVEL, M_SNAP, M_INPUT, M_MSG, M_BYE, M_PING, M_PONG };
#define NET_MAGIC 0x504d334eu /* "N3MP" */
#define NET_VERSION 2

static void set_nonblocking(sock_t s) {
#ifdef _WIN32
    u_long one = 1;
    ioctlsocket(s, FIONBIO, &one);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    int one1 = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one1, sizeof one1);
}

static void net_init_sockets(void) {
#ifdef _WIN32
    static int done;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
#endif
}

static void drop_peer(const char *why);

static void flush_out(void) {
    while (peer != BAD_SOCK && out_len) {
        int n = (int)send(peer, (const char *)outbuf, (int)(out_len > 1 << 20 ? 1 << 20 : out_len), 0);
        if (n > 0) {
            memmove(outbuf, outbuf + n, out_len - (size_t)n);
            out_len -= (size_t)n;
        } else {
            if (n < 0 && WOULD_BLOCK()) return;
            drop_peer("connection lost");
            return;
        }
    }
}

/* Message being built: begin, put the payload, end. */
static size_t msg_start;
static void out_need(size_t n) {
    if (out_len + n <= out_cap) return;
    while (out_len + n > out_cap) out_cap = out_cap ? out_cap * 2 : 1 << 16;
    outbuf = realloc(outbuf, out_cap);
}
static void put(const void *p, size_t n) { out_need(n); memcpy(outbuf + out_len, p, n); out_len += n; }
static void put8(uint8_t v) { put(&v, 1); }
static void put16(uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; put(b, 2); }
static void put32(uint32_t v) { put16((uint16_t)v); put16((uint16_t)(v >> 16)); }
static void msg_begin(uint8_t type) { msg_start = out_len; put32(0); put8(type); }
static void msg_end(void) {
    uint32_t len = (uint32_t)(out_len - msg_start - 4);
    for (int i = 0; i < 4; i++) outbuf[msg_start + i] = (uint8_t)(len >> (8 * i));
    if (out_len > (64u << 20)) { drop_peer("peer is not reading"); return; }
    flush_out();
}

/* reader over one received message */
typedef struct { const uint8_t *p, *end; } rd_t;
static int rd_ok(rd_t *r, size_t n) { return (size_t)(r->end - r->p) >= n; }
static uint8_t get8(rd_t *r) { return rd_ok(r, 1) ? *r->p++ : 0; }
static uint16_t get16(rd_t *r) { uint16_t lo = get8(r); return (uint16_t)(lo | get8(r) << 8); }
static uint32_t get32(rd_t *r) { uint32_t lo = get16(r); return lo | (uint32_t)get16(r) << 16; }
static void getn(rd_t *r, void *dst, size_t n) {
    if (!rd_ok(r, n)) { memset(dst, 0, n); r->p = r->end; return; }
    memcpy(dst, r->p, n);
    r->p += n;
}

/* ---------------------------------------------------------------- delta records */
/* Records [u16 offset][u16 length][bytes] for every changed run (short equal
 * gaps are bridged), ended by offset 0xffff. Updates the shadow as it goes. */
static void put_delta(const uint8_t *cur_mem, uint8_t *sh, int n) {
    int i = 0;
    while (i < n) {
        if (cur_mem[i] == sh[i]) { i++; continue; }
        int s = i, last = i;
        for (int e = i + 1; e < n && e - s < 0xfff0; e++) {
            if (cur_mem[e] != sh[e]) last = e;
            else if (e - last > 6) break;
        }
        int e = last + 1;
        put16((uint16_t)s);
        put16((uint16_t)(e - s));
        put(cur_mem + s, (size_t)(e - s));
        memcpy(sh + s, cur_mem + s, (size_t)(e - s));
        i = e;
    }
    put16(0xffff);
}

static void get_delta(rd_t *r, uint8_t *sh, int n) {
    for (;;) {
        uint16_t off = get16(r);
        if (off == 0xffff || !rd_ok(r, 2)) return;
        uint16_t len = get16(r);
        if ((int)off + len > n) { r->p = r->end; return; }
        getn(r, sh + off, len);
    }
}

static void reset_shadows(void) {
    for (int r = 0; r < NREG; r++) {
        if (!shadow[r]) shadow[r] = malloc(regions[r].len);
        memset(shadow[r], 0, regions[r].len);
    }
}

/* ---------------------------------------------------------------- shared state */
static int in_level(void) {
    uint8_t s = ds8(0x46a4);
    return ds16(0x46a8) == 0 && s >= 1 && s <= 3;
}

/* host */
static int have_level;      /* a level is loaded (net_level_loaded ran and the game is still on) */
static uint16_t level_ep, level_no, level_diff;
static struct { uint8_t keys0, keys1, typed; int8_t forward, strafe; int16_t angle; int have; } rin;
static uint16_t sounds[64][3];
static int nsounds;
static uint32_t snap_seq;
static uint8_t guard_target[OBJ_MAX];
static int guard_swapped;

/* client */
static int got_snapshot, level_pending;
static uint16_t pend_ep, pend_no, pend_diff;
static pctx_t me;           /* our record as of the last snapshot */
static uint8_t my_state;
static uint16_t my_epoch = 0xffff;
static int32_t clock_delta; /* our clock - host clock */
static uint16_t host_frame_no;
static double local_angle;
static int16_t peer_x, peer_y, peer_angle;
static uint8_t peer_state;
static int peer_known;
static uint8_t prev_powers;
static int snaps_since_frame, snap_hist[4];
/* Snapshots wait here and are applied one per drawn frame: they arrive
 * unevenly (some frames none, some two), which made everything hitch. */
#define SNAPQ 32
static struct { uint8_t *data; size_t len; } snapq[SNAPQ];
static int snapq_head, snapq_n, snap_waited;
static void snapq_clear(void);

static void host_send_level(void);

static int leaving; /* client left on its own: no "disconnected" box */
static int host_port, rtt_ms = -1;
static uint32_t last_ping_ms, last_snap_rx_ms, peer_since_ms;

static void drop_peer(const char *why) {
    if (peer == BAD_SOCK) return;
    sock_close(peer);
    peer = BAD_SOCK;
    in_len = out_len = 0;
    rtt_ms = -1;
    fprintf(stderr, "net: %s\n", why);
    if (role == 1) {
        if (cur != 0) swap_to(0);
        remote_ready = 0;
        host_set_title("Nitemare-3D (hosting, waiting for a player)");
    } else if (role == 2) {
        role = 0;
        host_set_title("Nitemare-3D");
        if (!leaving) host_message_box("Nitemare-3D", "Disconnected from the host.", 0);
        if (in_level()) wds8(0x46a4, 0); /* back to the menu */
    }
}

/* ---------------------------------------------------------------- host side */
int net_host(int port) {
    net_init_sockets();
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == BAD_SOCK) return 0;
    int one = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    if (bind(listen_sock, (struct sockaddr *)&a, sizeof a) != 0 || listen(listen_sock, 1) != 0) {
        fprintf(stderr, "net: can't listen on port %d\n", port);
        sock_close(listen_sock);
        listen_sock = BAD_SOCK;
        return 0;
    }
    set_nonblocking(listen_sock);
    role = 1;
    host_port = port;
    fprintf(stderr, "net: hosting co-op on port %d\n", port);
    host_set_title("Nitemare-3D (hosting, waiting for a player)");
    return 1;
}

int net_is_host(void) { return role == 1 && peer != BAD_SOCK; }
int net_is_client(void) { return role == 2 && peer != BAD_SOCK; }
int net_in_remote(void) { return cur == 1; }
int net_offscreen(void) { return offscreen_pass; }

/* Put player i back at the level start with full health. */
static void respawn(int i) {
    swap_to(i);
    pctx_t s = start_ctx[i];
    uint8_t keys[2] = {ds8(0x4c18), ds8(0x4c19)}; /* keys are shared: keep the ones found since */
    memcpy(ds_ptr(PL_OFF), s.pl, PL_LEN);
    wds8(0x4c18, keys[0]);
    wds8(0x4c19, keys[1]);
    if (ds8(0x4c0d) < FULL_HEALTH) wds8(0x4c0d, FULL_HEALTH);
    wds16(0x4bfe, 0xffff);
    wds8(0x46a4, 1);
    wds8(0x469c, 0);
    wds8(0x01fa, 0xff);
    call2(F_SET_POS, (uint16_t)pl16(&s, P_X), (uint16_t)pl16(&s, P_Y));
    call1(F_SET_ANGLE, (uint16_t)pl16(&s, P_ANGLE));
    swap_to(0);
    if (i == 0) call2(F_PALETTE_FX, 3, 0); /* out of the death colours */
    else epoch++;
    fprintf(stderr, "net: player %d respawned\n", i + 1);
}

/* Keys and keycards (DS:4c18/4c19) belong to both players: what either
 * finds is the other's too, and a key either loses is gone for both. */
static uint8_t shared_keys[2], shared_weapons;

/* Give player i weapon w as picking it up does (FUN_1010_ceb2, item 0x36):
 * own it, its ammo, and take it in hand if they had none. */
static void grant_weapon(int i, int w) {
    swap_to(i);
    wds8(0x4c1a, (uint8_t)(ds8(0x4c1a) | 1 << w));
    int unarmed = ds8(0x4c13) == 0xff;
    call2(0x1010a932u, (uint16_t)w, 1);
    if (unarmed) call1(0x1010aa38u, (uint16_t)w);
    swap_to(0);
}

static void share_keys(void) {
    if (!remote_ready || cur != 0) return;
    /* weapons: one found is found for both */
    uint8_t wa = ds8(0x4c1a), wb = ctx[1].pl[0x4c1a - PL_OFF];
    for (int w = 0; w < 8; w++) {
        uint8_t bit = (uint8_t)(1 << w);
        if (shared_weapons & bit) continue;
        if ((wa & bit) && !(wb & bit)) grant_weapon(1, w);
        else if ((wb & bit) && !(wa & bit)) grant_weapon(0, w);
    }
    shared_weapons = (uint8_t)(ds8(0x4c1a) | ctx[1].pl[0x4c1a - PL_OFF]);
    for (int k = 0; k < 2; k++) {
        uint16_t off = (uint16_t)(0x4c18 + k);
        uint8_t a = ds8(off), b = ctx[1].pl[off - PL_OFF], old = shared_keys[k];
        uint8_t now = (uint8_t)((old & a & b) | ((a | b) & ~old));
        shared_keys[k] = now;
        wds8(off, now);
        ctx[1].pl[off - PL_OFF] = now;
    }
}

/* Remote player at this level's start: their own weapons and items, the
 * host's start position (the first time, a copy of the host). */
static void init_remote(int first) {
    pctx_t *r = &ctx[1];
    if (first) *r = start_ctx[0];
    else memcpy(r->pl + P_LOC0, start_ctx[0].pl + P_LOC0, P_LOC1 - P_LOC0);
    if (!ds8(0x4bd6)) { r->pl[0x4c18 - PL_OFF] = 0; r->pl[0x4c19 - PL_OFF] = 0; } /* keys, as FUN_1018_08c0 */
    if (r->pl[P_HEALTH] == 0) r->pl[P_HEALTH] = FULL_HEALTH;
    r->state = 1;
    r->dying = 0;
    r->keys[0] = r->keys[1] = 0;
    r->edge[0] = r->edge[1] = 0;
    r->typed = 0;
    r->cool = 0xff;
    start_ctx[1] = *r;
    shared_keys[0] = r->pl[0x4c18 - PL_OFF] = ds8(0x4c18);
    shared_keys[1] = r->pl[0x4c19 - PL_OFF] = ds8(0x4c19);
    shared_weapons = (uint8_t)(ds8(0x4c1a) & r->pl[0x4c1a - PL_OFF]); /* the rest gets evened out */
    start_ctx[1] = *r;
    memset(guard_target, 0, sizeof guard_target);
    remote_ready = 1;
    epoch++;
    int wx, wy; /* testing aid: N3D_NETWARP=x,y puts player 2 there */
    if (getenv("N3D_NETWARP") && sscanf(getenv("N3D_NETWARP"), "%d,%d", &wx, &wy) == 2) {
        swap_to(1);
        call2(F_SET_POS, (uint16_t)wx, (uint16_t)wy);
        swap_to(0);
    }
}

static void host_send_level(void) {
    msg_begin(M_LEVEL);
    put16(level_ep);
    put16(level_no);
    put16(level_diff);
    msg_end();
    reset_shadows();
    nsounds = 0;
}

void net_level_loaded(void) {
    if (role != 1) return;
    if (cur != 0) swap_to(0);
    ctx_save(&start_ctx[0]);
    level_ep = ds16(0x7e42);
    level_no = ds16(0x7e44);
    level_diff = ds16(0x4c04);
    int first = !have_level || !remote_ready;
    have_level = 1;
    if (peer != BAD_SOCK) {
        init_remote(first);
        host_send_level();
    }
}

static void accept_peer(void) {
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    sock_t s = accept(listen_sock, (struct sockaddr *)&a, &al);
    if (s == BAD_SOCK) return;
    if (peer != BAD_SOCK) { sock_close(s); return; } /* two players */
    set_nonblocking(s);
    peer = s;
    in_len = out_len = 0;
    rin.have = 0;
    rtt_ms = -1;
    peer_since_ms = host_ticks_ms();
    fprintf(stderr, "net: player 2 connected\n");
}

/* What FUN_1010_a220 (draw the weapon) does to the weapon state besides
 * drawing it: raise the new weapon, finish a switch, end the recoil. The host
 * never draws the remote player's weapon, so it steps it here. */
static void remote_weapon_step(void) {
    if (ds8(0x4c13) == 0xff) return;
    int16_t mode = (int16_t)ds16(0x4c2a), pos = (int16_t)ds16(0x4c2c);
    if (mode == 0) wds8(0x4c20, 0);
    else if (mode == 1) {
        pos++;
        wds16(0x4c2c, (uint16_t)pos);
        if (pos == (int16_t)ds16(0x01f2)) wds16(0x4c2a, 0);
    } else if (mode == 2) {
        pos--;
        wds16(0x4c2c, (uint16_t)pos);
        if (pos == 0) {
            wds16(0x4c2a, 1);
            wds8(0x4c13, ds8(0x4c14));
            wds16(0x4c16, ds8(0x4c14) == 2 ? 1 : 2);
        }
    }
}

/* The remote player's frame: their view (it marks the guards they can shoot),
 * dying turn, and tick, all with their context in the globals. */
static void remote_frame(void) {
    if (ctx[1].state == 3) respawn(1);
    swap_to(1);
    remote_weapon_step(); /* where the host draws its own weapon, before the tick */
    if (ds8(0x46a4) == 1 && rin.have) call1(F_SET_ANGLE, (uint16_t)rin.angle);
    wds8(0x3752, rin.keys0);
    wds8(0x3753, rin.keys1);
    if (rin.typed) { wds8(0x0108, rin.typed); rin.typed = 0; }
    offscreen_pass = 1;
    wds32(0x53cc, ds32(0x53cc) + 1);
    call0(0x1018387eu); /* collect visible vectors */
    call0(0x101061c2u); /* wall segments */
    call0(0x1010660cu); /* walls (wall animation, automap) */
    call0(0x101062a4u); /* queue visible objects: marks the guards in view */
    offscreen_pass = 0;
    call0(F_DYING_TURN);
    call0(F_PLAYER_TICK);
    int16_t hurt = (int16_t)ds16(0x4bfe);
    if (hurt >= 0) wds16(0x4bfe, (uint16_t)(hurt - 1)); /* the red flash runs down like FUN_1010_bb1c */
    swap_to(0);
}

static void send_snapshot(void) {
    msg_begin(M_SNAP);
    put32(++snap_seq);
    put32(ds32(0x0096));
    put16(ds16(0x53cc));
    put16(epoch);
    put(ctx[1].pl, PL_LEN);
    put8(ctx[1].state);
    put8(ctx[1].cool);
    put16((uint16_t)ds16(0x4be6));
    put16((uint16_t)ds16(0x4be8));
    put16((uint16_t)ds16(0x4bda));
    put8(ds8(0x46a4));
    for (int r = 0; r < NREG; r++) put_delta(reg_live(r), shadow[r], regions[r].len);
    put8((uint8_t)nsounds);
    for (int i = 0; i < nsounds; i++) for (int k = 0; k < 3; k++) put16(sounds[i][k]);
    nsounds = 0;
    msg_end();
}

int net_remote_input(int *forward, int *strafe) {
    if (!rin.have) { *forward = *strafe = 0; return 1; }
    *forward = rin.forward;
    *strafe = rin.strafe;
    return 1;
}

void net_logic_ticked(uint16_t lo, uint16_t hi) {
    if (!net_is_host() || !remote_ready || !in_level()) return;
    if (ds8(0x46a4) == 3) respawn(0);
    if (ctx[1].state == 3) respawn(1);
    /* the per-player parts of FUN_1010_d8c6: powers, floor damage, weapon cooldown */
    swap_to(1);
    call2(0x1010ba78u, lo, hi);
    call0(0x1010bdb4u);
    call2(0x1010a9e2u, 1, 0);
    swap_to(0);
    share_keys();
}

void net_guard_begin(uint32_t obj) {
    guard_swapped = 0;
    if (!net_is_host() || !remote_ready || cur != 0) return;
    int idx = ((int)(obj & 0xffff) - OBJ_BASE) / OBJ_SIZE;
    if ((obj >> 16) != SEL_WORLD || idx < 0 || idx >= OBJ_MAX) return;
    int gx = (int16_t)MEMR16(obj + 0x10), gy = (int16_t)MEMR16(obj + 0x12);
    double dx0 = (int16_t)ds16(0x4be6) - gx, dy0 = (int16_t)ds16(0x4be8) - gy;
    double dx1 = pl16(&ctx[1], P_X) - gx, dy1 = pl16(&ctx[1], P_Y) - gy;
    double d[2] = {dx0 * dx0 + dy0 * dy0, dx1 * dx1 + dy1 * dy1};
    int alive[2] = {ds8(0x46a4) == 1, ctx[1].state == 1};
    int t = guard_target[idx];
    if (alive[t] != alive[!t]) t = alive[0] ? 0 : 1;
    else if (d[!t] * 16 < d[t] * 9) t = !t; /* switch to a player clearly nearer */
    guard_target[idx] = (uint8_t)t;
    if (t == 1) { swap_to(1); guard_swapped = 1; }
}

void net_guard_end(void) {
    if (guard_swapped) swap_to(0);
    guard_swapped = 0;
}

void net_doors_hold(void) {
    if (!net_is_host() || !remote_ready) return;
    uint32_t cell;
    memcpy(&cell, ctx[1].pl + P_CELL, 4);
    int n = (int16_t)MEMR16(SEGLIN(SEL_WORLD, 0));
    for (int i = 0; i < n && i < 0x40; i++) {
        uint16_t d = (uint16_t)(DOOR_BASE + i * 0x16);
        if ((int16_t)MEMR16(SEGLIN(SEL_WORLD, d + 0xc)) == 0 && (int16_t)MEMR16(SEGLIN(SEL_WORLD, d + 0xe)) == 1 &&
            MEMR32(SEGLIN(SEL_WORLD, d + 8)) == cell)
            MEMW16(SEGLIN(SEL_WORLD, d + 0xe), 2); /* someone is standing in it: wait */
    }
}

void net_sound(const uint16_t args[3]) {
    if (!net_is_host() || nsounds >= 64) return;
    memcpy(sounds[nsounds++], args, sizeof sounds[0]);
}

int net_message(uint32_t text, uint16_t arg) {
    if (!net_is_host() || cur != 1) return 0;
    msg_begin(M_MSG);
    put16(arg);
    size_t n = rt_strlen_far(text);
    if (n > 1000) n = 1000;
    for (size_t i = 0; i < n; i++) put8(MEMR8(text + (uint32_t)i));
    put8(0);
    msg_end();
    return 1;
}

/* ---------------------------------------------------------------- client side */
int net_join(const char *addr) {
    net_init_sockets();
    char host[256];
    const char *port = "5333";
    snprintf(host, sizeof host, "%s", addr);
    char *colon = strrchr(host, ':');
    if (colon && strchr(host, ':') == colon) { *colon = 0; port = colon + 1; }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        fprintf(stderr, "net: can't resolve %s\n", addr);
        return 0;
    }
    sock_t s = BAD_SOCK;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == BAD_SOCK) continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        sock_close(s);
        s = BAD_SOCK;
    }
    freeaddrinfo(res);
    if (s == BAD_SOCK) {
        fprintf(stderr, "net: can't connect to %s\n", addr);
        return 0;
    }
    set_nonblocking(s);
    peer = s;
    role = 2;
    msg_begin(M_HELLO);
    put32(NET_MAGIC);
    put16(NET_VERSION);
    msg_end();
    fprintf(stderr, "net: connected to %s, waiting for the host to start a level\n", addr);
    host_set_title("Nitemare-3D (co-op, waiting for the host)");
    return 1;
}

/* Start the host's level: the menu's episode and difficulty commands
 * (FUN_1018_26ac 0x15..0x17 load level DS:4696 of episode DS:40f4). */
static void client_start_level(void) {
    level_pending = 0;
    wds16(0x46a8, 0);           /* not a demo */
    wds16(0x40f4, pend_ep);
    wds16(0x4696, pend_no);
    call1(F_MENU_CMD, pend_diff);
    wds16(0x46a6, 1);
    wds8(0x46a4, 1);
    reset_shadows();
    snapq_clear();
    got_snapshot = 0;
    my_epoch = 0xffff;
    peer_known = 0;
    host_set_title("Nitemare-3D (co-op)");
    fprintf(stderr, "net: level %d of episode %d\n", pend_no + 1, pend_ep);
}

static void client_snapshot(rd_t *r) {
    get32(r); /* sequence */
    uint32_t host_clock = get32(r);
    clock_delta = (int32_t)(ds32(0x0096) - host_clock);
    host_frame_no = get16(r);
    uint16_t ep = get16(r);
    getn(r, me.pl, PL_LEN);
    my_state = get8(r);
    me.cool = get8(r);
    peer_x = (int16_t)get16(r);
    peer_y = (int16_t)get16(r);
    peer_angle = (int16_t)get16(r);
    peer_state = get8(r);
    peer_known = 1;
    for (int k = 0; k < NREG; k++) get_delta(r, shadow[k], regions[k].len);
    int ns = get8(r);
    for (int i = 0; i < ns; i++) {
        uint16_t a[3] = {get16(r), get16(r), get16(r)};
        rt_callf(F_SOUND, 3, a);
    }
    if (ep != my_epoch) { /* moved by the host: take its view angle */
        my_epoch = ep;
        local_angle = pl16(&me, P_ANGLE);
    }
    got_snapshot = 1;
    snaps_since_frame++;
}

/* Object animation is the client's own (it is drawn here, in the host's
 * absence too): keep our frame and timing where the host didn't change what
 * the object is; guards and 8/4-direction figures follow the host. */
static void apply_world(void) {
    uint8_t *w = world_ptr(0), *sh = shadow[0];
    static uint8_t keep[OBJ_MAX][6]; /* +2 sequence, +3 frame, +8..+0xb next step time */
    static uint8_t keepok[OBJ_MAX], guard[OBJ_MAX];
    memset(guard, 0, sizeof guard); /* guards' frames are their state (walking, shooting, dying): the host's */
    int ng = (int16_t)(shadow[3][0x7e4e - 0x7e42] | shadow[3][0x7e4f - 0x7e42] << 8);
    for (int g = 0; g < ng && g < 200; g++) {
        int idx = (int16_t)(sh[GUARD_BASE + g * 0x1a + 8] | sh[GUARD_BASE + g * 0x1a + 9] << 8);
        if (idx >= 0 && idx < OBJ_MAX) guard[idx] = 1;
    }
    for (int i = 0; i < OBJ_MAX; i++) {
        const uint8_t *o = w + OBJ_BASE + i * OBJ_SIZE, *h = sh + OBJ_BASE + i * OBJ_SIZE;
        keepok[i] = !guard[i] && o[4] == h[4] && o[6] == h[6] && h[6] != 0x2c && h[6] != 0x2d;
        if (keepok[i]) { memcpy(keep[i], o + 2, 2); memcpy(keep[i] + 2, o + 8, 4); }
    }
    memcpy(w, sh, WORLD_LEN);
    for (int i = 0; i < OBJ_MAX; i++) {
        uint8_t *o = w + OBJ_BASE + i * OBJ_SIZE;
        if (keepok[i]) { memcpy(o + 2, keep[i], 2); memcpy(o + 8, keep[i] + 2, 4); }
        else { uint32_t t; memcpy(&t, o + 8, 4); t += (uint32_t)clock_delta; memcpy(o + 8, &t, 4); }
    }
    for (int r = 1; r < NREG; r++) memcpy(reg_live(r), shadow[r], regions[r].len);
    for (int i = 0; i < 8; i++) { /* projectile animation clocks */
        uint16_t b = (uint16_t)(0x4c3a + i * 0x2a + 0x16);
        wds32(b, ds32(b) + (uint32_t)clock_delta);
    }
}

/* Guards are drawn facing where they go: FUN_1010_6e3c picks the walking/
 * shooting loop for the direction they face seen from "the player" (the
 * one they chase). For everyone else that is the wrong side of them, so each
 * screen picks the loop again for its own viewer, keeping the step in it. */
typedef struct { uint32_t obj; uint16_t g; uint8_t frame, start, count, dir; } guard_view_t;
static guard_view_t gv_saved[200];
static int gv_n;

static int guard_view(int i, guard_view_t *old) {
    uint16_t g = (uint16_t)(GUARD_BASE + i * 0x1a);
    uint32_t gl = SEGLIN(SEL_WORLD, g);
    int idx = (int16_t)MEMR16(gl + 8);
    if (idx < 0 || idx >= OBJ_MAX) return 0;
    uint32_t o = SEGLIN(SEL_WORLD, OBJ_BASE + idx * OBJ_SIZE);
    uint16_t tbl = ds16((uint16_t)(0x4738 + MEMR8(o + 4) * 8));
    uint8_t start = MEMR8(gl), count = MEMR8(gl + 1), dir = MEMR8(gl + 0x12);
    static const uint16_t bases[3] = {4, 0x14, 0x24};
    int base = -1;
    for (int b = 0; b < 3 && base < 0; b++) /* only while in one of the direction loops */
        if (ds16((uint16_t)(tbl + bases[b] + dir * 2)) == (uint16_t)(start | count << 8)) base = bases[b];
    if (base < 0) return 0;
    uint8_t flag = MEMR8(gl + 0xf);
    uint16_t a[3] = {flag, (uint16_t)(OBJ_BASE + idx * OBJ_SIZE), SEL_WORLD};
    uint8_t oct = (uint8_t)rt_callf(0x1010d3a6u, 3, a);
    uint8_t nd = (uint8_t)((4 - (flag == 0) + MEMR8(gl + 0x11) - oct) & 7);
    if (nd == dir) return 0;
    uint16_t e = ds16((uint16_t)(tbl + base + nd * 2));
    uint8_t frame = MEMR8(o + 3);
    if (old) *old = (guard_view_t){o, g, frame, start, count, dir};
    int step = frame - start;
    if (step < 0 || step >= (e >> 8)) step = 0;
    MEMW8(o + 3, (uint8_t)((e & 0xff) + step));
    MEMW16(gl, e);
    MEMW8(gl + 0x12, nd);
    return 1;
}

static void guards_face_viewer(void) {
    int n = (int16_t)ds16(0x7e4e);
    for (int i = 0; i < n && i < 200; i++) guard_view(i, NULL);
}

void net_view_begin(void) {
    gv_n = 0;
    if (!net_is_host() || !remote_ready || cur != 0) return;
    int n = (int16_t)ds16(0x7e4e);
    for (int i = 0; i < n && i < 200; i++)
        if (guard_view(i, &gv_saved[gv_n])) gv_n++;
}

void net_view_end(void) { /* the AI goes on from what it set for its target */
    for (int i = 0; i < gv_n; i++) {
        guard_view_t *v = &gv_saved[i];
        MEMW8(v->obj + 3, v->frame);
        MEMW8(SEGLIN(SEL_WORLD, v->g), v->start);
        MEMW8(SEGLIN(SEL_WORLD, v->g + 1), v->count);
        MEMW8(SEGLIN(SEL_WORLD, v->g + 0x12), v->dir);
    }
    gv_n = 0;
}

static void snapq_apply_one(void) {
    rd_t q = {snapq[snapq_head].data, snapq[snapq_head].data + snapq[snapq_head].len};
    client_snapshot(&q);
    free(snapq[snapq_head].data);
    snapq_head = (snapq_head + 1) % SNAPQ;
    snapq_n--;
}

static void snapq_clear(void) {
    while (snapq_n) { free(snapq[snapq_head].data); snapq_head = (snapq_head + 1) % SNAPQ; snapq_n--; }
    snap_waited = 0;
}

void net_client_frame(void) {
    if (!net_is_client()) return;
    /* one per frame, one kept in reserve against late ones; catch up if they pile up */
    if (snapq_n >= 2 || (snapq_n == 1 && (snap_waited || !got_snapshot))) { snapq_apply_one(); snap_waited = 0; }
    else snap_waited = 1;
    while (snapq_n > 3) snapq_apply_one();
    if (!got_snapshot) return;
    snap_hist[snaps_since_frame > 3 ? 3 : snaps_since_frame]++;
    snaps_since_frame = 0;
    if (getenv("N3D_NETLOG") && (snap_hist[0] + snap_hist[1] + snap_hist[2] + snap_hist[3]) % 100 == 0)
        fprintf(stderr, "netlog snapshots per frame: 0:%d 1:%d 2:%d 3+:%d\n", snap_hist[0], snap_hist[1], snap_hist[2], snap_hist[3]);
    apply_world();
    memcpy(ds_ptr(PL_OFF), me.pl, PL_LEN);
    wds8(0x01fa, me.cool);
    wds32(0x53cc, host_frame_no);
    wds8(0x46a4, 1);
    if (my_state == 1) call1(F_SET_ANGLE, (uint16_t)(int)local_angle);
    else local_angle = pl16(&me, P_ANGLE);
    guards_face_viewer();
    uint8_t powers = me.pl[P_POWER1] | me.pl[P_POWER2];
    if (prev_powers && !powers && (int16_t)ds16(0x4bfe) < 0) call2(F_PALETTE_FX, 3, 0);
    prev_powers = powers;
}

void net_client_projectiles(void) {
    int px = (int16_t)ds16(0x4be6), py = (int16_t)ds16(0x4be8);
    for (int i = 0; i < 8; i++) {
        uint16_t b = (uint16_t)(0x4c3a + i * 0x2a);
        uint8_t state = ds8((uint16_t)(b + 0xc));
        int show = 0;
        if (state == 1) {
            int dx = (int16_t)ds16((uint16_t)(b + 0x1e)) - px, dy = (int16_t)ds16((uint16_t)(b + 0x20)) - py;
            show = abs(dx) > 0x14 || abs(dy) > 0x14;
        } else if (state == 2) {
            uint8_t set = ds8((uint16_t)(b + 0x12));
            show = ds8((uint16_t)(b + 0x11)) < (int16_t)ds16((uint16_t)(0x4734 + set * 8));
        }
        if (show) call2(F_QUEUE_SPRITE, (uint16_t)(b + 0xe), RT_SEL_DS);
    }
}

/* Our input for the host, once per drawn frame (in place of the player tick). */
static void client_input(void) {
    uint8_t typed = ds8(0x0108);
    wds8(0x0108, 0);
    switch (typed) {
    case 0x1b: /* Esc: leave the game */
        msg_begin(M_BYE);
        msg_end();
        flush_out();
        leaving = 1;
        drop_peer("left the game");
        return;
    case 0x09: call0(0x1010c44cu); typed = 0; break; /* status */
    case 0x78: call0(0x1010bbbcu); typed = 0; break; /* automaps */
    case 0x79: call0(0x1010bbe2u); typed = 0; break;
    default:
        if (typed < '1' || typed > '9') typed = 0; /* weapons go to the host */
    }
    int fwd = 0, side = 0, turn = 0;
    host_modern_input(&fwd, &side, &turn);
    uint8_t k0 = ds8(0x3752), k1 = ds8(0x3753);
    if (!(k1 & 1)) { /* arrows turn here (Alt+arrows strafe on the host) */
        int speed = (int16_t)ds16(0x53e8);
        if (k0 & 0x20) speed *= 2;
        if (k0 & 0x40) speed = 1;
        if (k0 & 8) turn -= speed;
        if (k0 & 0x10) turn += speed;
        k0 &= (uint8_t)~0x18;
    }
    if (my_state == 1) {
        local_angle += turn;
        while (local_angle < 0) local_angle += 360;
        while (local_angle >= 360) local_angle -= 360;
        call1(F_SET_ANGLE, (uint16_t)(int)local_angle);
    }
    msg_begin(M_INPUT);
    put8(k0);
    put8(k1);
    put8(typed);
    put8((uint8_t)(int8_t)fwd);
    put8((uint8_t)(int8_t)side);
    put16((uint16_t)(int16_t)local_angle);
    msg_end();
}

static void debug_log(void) {
    static int on = -1;
    static uint32_t last;
    if (on < 0) on = getenv("N3D_NETLOG") != NULL;
    if (!on || host_ticks_ms() - last < 500) return;
    last = host_ticks_ms();
    if (role == 1)
        fprintf(stderr, "netlog host: weapons %02x w%d ammo %d | keys %02x/%02x p2 %02x/%02x | p2 weapons %02x weapon %d ammo %d %d %d | state %d hp %d at %d,%d a%d | p2 state %d hp %d at %d,%d a%d in(%02x %02x f%d s%d a%d) 46a6=%d hostkeys %02x %02x t%02x\n",
                ds8(0x4c1a), (int8_t)ds8(0x4c13), ds8(0x4c10),
                ds8(0x4c18), ds8(0x4c19), ctx[1].pl[0x4c18 - PL_OFF], ctx[1].pl[0x4c19 - PL_OFF], ctx[1].pl[0x4c1a - PL_OFF],
                (int8_t)ctx[1].pl[0x4c13 - PL_OFF], ctx[1].pl[0x4c34 - PL_OFF], ctx[1].pl[0x4c0f - PL_OFF], ctx[1].pl[0x4c10 - PL_OFF],
                ds8(0x46a4), ds8(0x4c0d), (int16_t)ds16(0x4be6), (int16_t)ds16(0x4be8), (int16_t)ds16(0x4bda),
                ctx[1].state, ctx[1].pl[P_HEALTH], pl16(&ctx[1], P_X), pl16(&ctx[1], P_Y), pl16(&ctx[1], P_ANGLE),
                rin.keys0, rin.keys1, rin.forward, rin.strafe, rin.angle, ds16(0x46a6), ds8(0x3752), ds8(0x3753), ds8(0x0108));
}

void net_player_ticked(void) {
    debug_log();
    if (role == 2) {
        if (net_is_client() && got_snapshot) client_input();
        return;
    }
    if (!net_is_host() || cur != 0 || !in_level()) return;
    if (ds8(0x46a4) == 3) respawn(0);
    if (!remote_ready) init_remote(1);
    remote_frame();
    share_keys();
    send_snapshot();
}

/* ---------------------------------------------------------------- the other player's sprite */
static uint16_t peer_sel;

/* The other player is the man in the green lab coat. His 32 walking frames
 * (8 directions x 4) are in IMG.1, back to back from this offset, each after
 * a 10-byte header that starts with its width and height. Episode 3's IMG.3
 * doesn't have him, so he is always read from IMG.1. */
#define GREEN_MAN_IMG "img.1"
#define GREEN_MAN_OFF 0x1d2597L
#define GREEN_MAN_FRAMES 32
static struct { int w, h; uint8_t *pix; } green[GREEN_MAN_FRAMES];
static int green_state; /* 0 not tried, 1 loaded, -1 unavailable */

static int green_load(void) {
    if (green_state) return green_state > 0;
    green_state = -1;
    FILE *f = dos_fopen(GREEN_MAN_IMG, "rb");
    if (!f) return 0;
    long off = GREEN_MAN_OFF;
    int k;
    for (k = 0; k < GREEN_MAN_FRAMES; k++) {
        uint8_t hdr[10];
        if (fseek(f, off - 10, SEEK_SET) || fread(hdr, 1, 10, f) != 10) break;
        int w = hdr[0], h = hdr[1];
        if (k == 0 && (w != 0x20 || h != 0x3a)) break; /* not the file we know */
        if (!w || !h) break;
        green[k].w = w;
        green[k].h = h;
        green[k].pix = malloc((size_t)(w * h));
        if (fread(green[k].pix, 1, (size_t)(w * h), f) != (size_t)(w * h)) break;
        off += w * h + 10;
    }
    fclose(f);
    if (k == GREEN_MAN_FRAMES) green_state = 1;
    return green_state > 0;
}

const uint8_t *net_peer_pixels(uint32_t obj, int *w, int *h, uint32_t *key) {
    if (!peer_sel || (obj >> 16) != peer_sel || green_state <= 0) return NULL;
    int f = MEMR8(obj + 3) % GREEN_MAN_FRAMES;
    *w = green[f].w;
    *h = green[f].h;
    *key = 0x80000000u | (uint32_t)(GREEN_MAN_OFF + f); /* its own cache key, apart from the level's art */
    return green[f].pix;
}

int net_peer(uint32_t *obj, double *x, double *y) {
    int px, py, ang;
    if (net_is_host() && remote_ready && ctx[1].state != 3) {
        px = pl16(&ctx[1], P_X); py = pl16(&ctx[1], P_Y); ang = pl16(&ctx[1], P_ANGLE);
    } else if (net_is_client() && peer_known && peer_state != 3) {
        px = peer_x; py = peer_y; ang = peer_angle;
    } else return 0;
    if (!peer_sel) peer_sel = rt_alloc_selectors(0x40);
    uint32_t o = SEGLIN(peer_sel, 0);
    /* The look of the other player: the green lab-coat man (above); without
     * him, the white lab-coat man (guard type 0x0f; the tallest guard if the
     * level has none, N3D_PEER=set overrides). Standing
     * in the direction they face: frames 0-31 are 8 directions (front, then
     * turning to his right in 45 degree steps) of 4 walking frames each. */
    static int set = -1, lvl = -1, dirs = 8;
    int here = ds16(0x7e42) << 8 | ds16(0x7e44);
    if (lvl != here) { lvl = here; set = -1; }
    if (set < 0 && getenv("N3D_PEER")) set = atoi(getenv("N3D_PEER"));
    for (int pass = 0, best = 0; set < 0 && !green_load() && pass < 2; pass++)
        for (int i = 0, n = (int16_t)ds16(0x7e4e); i < n; i++) {
            int idx = (int16_t)MEMR16(SEGLIN(SEL_WORLD, GUARD_BASE + i * 0x1a + 8));
            if (idx < 0 || idx >= OBJ_MAX) continue;
            uint32_t g = SEGLIN(SEL_WORLD, OBJ_BASE + idx * OBJ_SIZE);
            int gs = MEMR8(g + 4);
            uint16_t e = ds16((uint16_t)(0x473a + gs * 8));
            int h = ds8((uint16_t)(e + 1));
            if (!ds8(e) || !h) continue;
            if (pass == 0 && MEMR8(g + 6) == 0x0f) { set = gs; break; }
            if (pass == 1 && h > best) { best = h; set = gs; }
        }
    if (green_load()) set = 0; /* frames come from net_peer_pixels */
    if (set < 0) return 0;
    if (green_state > 0) dirs = 8;
    else dirs = (int16_t)ds16((uint16_t)(0x4734 + set * 8)) >= 32 ? 8 : 1;
    /* game angles: degrees clockwise from north (-y) */
    double vx = (int16_t)ds16(0x4be6) - px, vy = (int16_t)ds16(0x4be8) - py;
    double toward = atan2(vx, -vy) * 180.0 / 3.14159265358979323846;
    int d = dirs == 1 ? 0 : (((int)lround((ang - toward) / 45.0)) % 8 + 8) % 8;
    /* 4 walking frames per direction: step through them while they move, feet together when not */
    static int last_x, last_y, phase;
    static uint32_t moved_ms, step_ms;
    uint32_t now = host_ticks_ms();
    if (px != last_x || py != last_y) { last_x = px; last_y = py; moved_ms = now; }
    if (now - moved_ms < 150) {
        if (now - step_ms >= 140) { step_ms = now; phase = (phase + 1) & 3; }
    } else phase = 3;
    MEMW8(o + 3, (uint8_t)(dirs == 1 ? 0 : d * 4 + phase));
    MEMW8(o + 4, (uint8_t)set);
    MEMW8(o + 5, 1);
    MEMW8(o + 6, 0);
    MEMW16(o + 0x10, (uint16_t)px);
    MEMW16(o + 0x12, (uint16_t)py);
    MEMW8(o + 0x1a, 0);
    *obj = o;
    *x = px;
    *y = py;
    return 1;
}

/* ---------------------------------------------------------------- loop */
static void handle(uint8_t type, rd_t *r) {
    if (type == M_PING) { uint32_t t = get32(r); msg_begin(M_PONG); put32(t); msg_end(); return; }
    if (type == M_PONG) {
        int rtt = (int)(host_ticks_ms() - get32(r));
        rtt_ms = rtt_ms < 0 ? rtt : (rtt_ms * 3 + rtt) / 4;
        return;
    }
    if (role == 2 && type == M_SNAP) last_snap_rx_ms = host_ticks_ms();
    if (role == 1) {
        switch (type) {
        case M_HELLO:
            if (get32(r) != NET_MAGIC || get16(r) != NET_VERSION) { drop_peer("wrong game or version"); return; }
            msg_begin(M_WELCOME);
            put16(NET_VERSION);
            msg_end();
            host_set_title("Nitemare-3D (co-op host)");
            if (have_level && in_level()) {
                if (cur != 0) swap_to(0);
                init_remote(1);
                host_send_level();
            }
            break;
        case M_INPUT:
            rin.keys0 = get8(r);
            rin.keys1 = get8(r);
            { uint8_t t = get8(r); if (t) rin.typed = t; }
            rin.forward = (int8_t)get8(r);
            rin.strafe = (int8_t)get8(r);
            rin.angle = (int16_t)get16(r);
            rin.have = 1;
            break;
        case M_BYE: drop_peer("player 2 left"); break;
        }
        return;
    }
    switch (type) {
    case M_WELCOME: break;
    case M_LEVEL:
        pend_ep = get16(r);
        pend_no = get16(r);
        pend_diff = get16(r);
        level_pending = 1;
        break;
    case M_SNAP:
        if (level_pending) break;
        if (snapq_n == SNAPQ) { /* far behind: take the oldest now */
            rd_t q = {snapq[snapq_head].data, snapq[snapq_head].data + snapq[snapq_head].len};
            client_snapshot(&q);
            free(snapq[snapq_head].data);
            snapq_head = (snapq_head + 1) % SNAPQ;
            snapq_n--;
        }
        {
            int at = (snapq_head + snapq_n) % SNAPQ;
            snapq[at].len = (size_t)(r->end - r->p);
            snapq[at].data = malloc(snapq[at].len ? snapq[at].len : 1);
            memcpy(snapq[at].data, r->p, snapq[at].len);
            snapq_n++;
        }
        break;
    case M_MSG: {
        uint16_t arg = get16(r);
        size_t n = (size_t)(r->end - r->p);
        uint16_t buf = rt_local_alloc(0x40, (uint16_t)(n + 1));
        if (!buf) break;
        for (size_t i = 0; i < n; i++) wds8((uint16_t)(buf + i), r->p[i]);
        wds8((uint16_t)(buf + n), 0);
        uint16_t a[3] = {buf, RT_SEL_DS, arg};
        rt_callf(F_MESSAGE, 3, a);
        rt_local_free(buf);
        break;
    }
    case M_BYE: drop_peer("the host left"); break;
    }
}

void net_poll(void) {
    if (!role) return;
    if (role == 1) {
        if (listen_sock != BAD_SOCK) accept_peer();
        if (have_level && !in_level()) have_level = 0; /* host went back to the menu */
    }
    if (peer == BAD_SOCK) return;
    if (host_ticks_ms() - last_ping_ms >= 1000) {
        last_ping_ms = host_ticks_ms();
        msg_begin(M_PING);
        put32(last_ping_ms);
        msg_end();
        if (peer == BAD_SOCK) return;
    }
    flush_out();
    for (;;) {
        if (peer == BAD_SOCK) return;
        if (in_cap - in_len < 1 << 16) { in_cap = in_cap ? in_cap * 2 : 1 << 17; inbuf = realloc(inbuf, in_cap); }
        int n = (int)recv(peer, (char *)inbuf + in_len, (int)(in_cap - in_len), 0);
        if (n > 0) { in_len += (size_t)n; continue; }
        if (n < 0 && WOULD_BLOCK()) break;
        drop_peer(role == 1 ? "player 2 disconnected" : "the host closed the connection");
        return;
    }
    size_t at = 0;
    while (in_len - at >= 5) {
        uint32_t len = inbuf[at] | inbuf[at + 1] << 8 | inbuf[at + 2] << 16 | (uint32_t)inbuf[at + 3] << 24;
        if (len == 0 || len > (64u << 20)) { drop_peer("bad message"); return; }
        if (in_len - at - 4 < len) break;
        rd_t r = {inbuf + at + 5, inbuf + at + 4 + len};
        uint8_t type = inbuf[at + 4];
        at += 4 + len;
        handle(type, &r);
        if (peer == BAD_SOCK) return;
        /* a new level is loaded before the snapshots that follow it are read */
        if (role == 2 && level_pending) client_start_level();
    }
    memmove(inbuf, inbuf + at, in_len - at);
    in_len -= at;
}

void net_shutdown(void) {
    if (peer != BAD_SOCK) {
        msg_begin(M_BYE);
        msg_end();
        flush_out();
        sock_close(peer);
        peer = BAD_SOCK;
    }
    if (listen_sock != BAD_SOCK) sock_close(listen_sock);
    listen_sock = BAD_SOCK;
    role = 0;
}

/* ---------------------------------------------------------------- on-screen status */
static const char *lan_address(void) {
    static char addr[64];
    static int done;
    if (done) return addr[0] ? addr : NULL;
    done = 1;
#ifndef _WIN32
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) == 0) {
        for (struct ifaddrs *i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            const struct sockaddr_in *a = (const struct sockaddr_in *)i->ifa_addr;
            if ((ntohl(a->sin_addr.s_addr) >> 24) == 127) continue;
            inet_ntop(AF_INET, &a->sin_addr, addr, sizeof addr);
            break;
        }
        freeifaddrs(list);
    }
#else
    /* No getifaddrs: the address a UDP socket would leave from towards a
     * documentation address. Connecting a UDP socket only picks the route;
     * nothing is sent. */
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s != BAD_SOCK) {
        struct sockaddr_in to = {0}, me = {0};
        int len = sizeof me;
        to.sin_family = AF_INET;
        to.sin_port = htons(9);
        inet_pton(AF_INET, "192.0.2.1", &to.sin_addr);
        if (connect(s, (struct sockaddr *)&to, sizeof to) == 0 && getsockname(s, (struct sockaddr *)&me, &len) == 0 && me.sin_addr.s_addr)
            inet_ntop(AF_INET, &me.sin_addr, addr, sizeof addr);
        sock_close(s);
    }
#endif
    return addr[0] ? addr : NULL;
}

int net_status(const char **lines, int max) {
    static char buf[4][128];
    int n = 0;
    char ping[32] = "";
    if (rtt_ms >= 0) snprintf(ping, sizeof ping, "  -  ping %d ms", rtt_ms);
#define LINE(...) do { if (n < max && n < 4) { snprintf(buf[n], sizeof buf[n], __VA_ARGS__); lines[n] = buf[n]; n++; } } while (0)
    if (role == 1) {
        if (peer == BAD_SOCK) {
            const char *ip = lan_address();
            if (ip) LINE("Co-op host  -  %s port %d", ip, host_port);
            else LINE("Co-op host  -  port %d", host_port);
            LINE("Waiting for player 2 to join");
        } else {
            LINE("Co-op  -  player 2 connected%s", ping);
            if (!in_level()) LINE("Start a game: player 2 joins your level");
            else if (remote_ready && ctx[1].state != 1) LINE("Player 2 is down - respawning");
        }
    } else if (role == 2 && peer != BAD_SOCK) {
        int playing = got_snapshot && in_level();
        LINE("Co-op  -  connected to the host%s", ping);
        if (!playing) LINE("Waiting for the host to start a level");
        else {
            if (host_ticks_ms() - last_snap_rx_ms > 1500) LINE("Host paused (menu or window in the background)");
            if (my_state != 1) LINE("You are down - respawning");
            else if (peer_state != 1) LINE("Player 1 is down");
        }
    }
#undef LINE
    return n;
}
#endif
