#ifndef NETWORK_H
#define NETWORK_H

/*
 * network.h  –  Nätverksprotokoll för ShadowBound
 *
 * PlayerPacket: klient → server  (din egen spelares state)
 * GameState:    server → klienter (alla spelares state samlat)
 *
 * MAX_PLAYERS och Player-typen definieras i player.h.
 */

#include <SDL2/SDL_net.h>
#include <stdbool.h>
#include "player.h"   /* ger MAX_PLAYERS */

#define SERVER_PORT 2000

/* Skickas från varje klient till servern */
typedef struct {
    int   player_id;       /* 0 = reaper, 1–3 = survivors */
    float x, y;
    bool  is_moving;
    bool  flip;
    bool  is_attacking;
    bool  is_alive;
    bool  chest_states[8];
    bool  is_ready;
    bool  all_connected;
} PlayerPacket;

/* Skickas från servern till alla klienter */
typedef struct {
    PlayerPacket players[MAX_PLAYERS];
    bool         chest_states[8];
} GameState;

#endif /* NETWORK_H */
