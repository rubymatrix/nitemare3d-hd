/*
 * Two-player co-op over TCP (src/net.c).
 *
 * The host runs the only simulation. It keeps a context for each player
 * (the player record DS:4bd8, keys, weapon cooldown, play state) and swaps
 * the remote player's context into the game's globals around that player's
 * view pass and tick, and around the AI of each guard chasing them, so the
 * original code moves, hurts, arms and kills the right player.
 *
 * The client runs the same game as a terminal: it loads the same level,
 * applies the world state the host sends after every frame (segment 1040
 * and a few DS globals) plus its own player record, draws its view and sends
 * its input. It never runs game logic.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_DEFAULT_PORT 5333

int net_host(int port);                /* --host: listen for a player to join */
int net_join(const char *addr);        /* --join HOST[:PORT]; 0 on failure */
void net_poll(void);                   /* once per host loop iteration */
void net_shutdown(void);

int net_is_host(void);                 /* hosting with a player connected */
int net_is_client(void);               /* joined and connected */
int net_in_remote(void);               /* host: the remote player's context is swapped in */
int net_offscreen(void);               /* host: drawing the remote player's view for its side effects */

/* Hooks from src/rt/overrides.c */
void net_player_ticked(void);          /* end of FUN_1010_9758 (one per drawn frame) */
void net_logic_ticked(uint16_t lo, uint16_t hi); /* after FUN_1010_d8c6 (host) */
void net_client_frame(void);           /* client: before the view is drawn */
void net_client_projectiles(void);     /* client: queue projectile sprites like FUN_1010_9d72 */
void net_level_loaded(void);           /* after FUN_1018_08c0 */
void net_guard_begin(uint32_t obj);    /* around FUN_1010_7ab2: chase the nearest player */
void net_guard_end(void);
void net_doors_hold(void);             /* before FUN_1010_1d02: keep doors open on the remote player */
void net_sound(const uint16_t args[3]);
int net_message(uint32_t text, uint16_t arg); /* modal message for the remote player: 1 = sent there */
int net_remote_input(int *forward, int *strafe); /* modern input of the remote player */

/* The other player, for the hi-res renderer: a sprite object to draw at x, y. */
int net_peer(uint32_t *obj, double *x, double *y);

#ifdef __cplusplus
}
#endif
