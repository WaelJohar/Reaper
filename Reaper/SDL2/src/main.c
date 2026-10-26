/*
 * main.c  –  Reaper klient
 *
 * Kompilera t.ex.:
 *   gcc main.c -o game -lSDL2 -lSDL2_image -lSDL2_net -lSDL2_ttf -lm
 *
 * Kör:
 *   ./game
 *   Välj roll i lobbyn: P/1,2,3=Survivor, R=Reaper
 *
 * ══════════════════════════════════════════════════════════════════════════
 * NÄTVERKSARKITEKTUR – LÄS DETTA FÖRST
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Grundprincipen (föreläsningens modell):
 *
 *   Klient A ──[PlayerPacket]──▶ Server ──[GameState]──▶ Klient A, B, C, D
 *   Klient B ──[PlayerPacket]──▶ Server ──[GameState]──▶ Klient A, B, C, D
 *   ...
 *
 * PlayerPacket (klient → server):
 *   Din egen spelares position, rörelsestate, attack, liv, kistor.
 *   Du skickar bara DIN egen state.  Aldrig andras.
 *
 * GameState (server → klient):
 *   Alla 4 spelares PlayerPackets samlat + gemensam kist-state.
 *   Du uppdaterar ANDRA spelares state från detta.
 *   Din EGEN state uppdaterar du LOKALT (inte från servern) –
 *   det ger omedelbar respons utan nätverkslatens.
 *
 * Vad är "auktoritativt" (servern bestämmer)?
 *   - Alla spelares positioner utom din egen
 *   - Dödsstate: om servern säger att en spelare är dead, respektera det
 *   - Kiststate: OR-logik (öppen = öppen för alltid)
 *
 * Vad är "klientauktoritativt" (du bestämmer lokalt)?
 *   - Din egna position (du rör dig, skickar till server, andra ser det via gs)
 *   - Din egna attack-animation (du startar den, skickar is_attacking)
 * ══════════════════════════════════════════════════════════════════════════
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_net.h>
#include <SDL2/SDL_mixer.h>
#include "../include/network.h"
#include "../include/player.h"
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ══════════════════════════════════════════════════════════════════════════
 * KONSTANTER OCH TYPDEFINITIONER
 * ══════════════════════════════════════════════════════════════════════════ */

#define VV        -1       /* "tomt" i tilemaps  */
#define TILE_SIZE  32
#define MAP_ROWS   30
#define MAP_COLS   60

/* Synfält: definieras nu i player.h / player.c */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Positioner för Survivors (P1, P2, P3)
SDL_Rect survivorSlots[3] = {
    { 290, 470, 64, 96 }, // P1
    { 470, 470, 64, 96 }, // P2
    { 640, 470, 64, 96 }  // P3
};

// Position för Reaper
SDL_Rect reaperSlot = { 435, 750, 130, 130 };

// Offset för texten under gubbarna
int textOffsetY = 110;

typedef struct {
    float x, y;
    bool is_open;
} Chest;

Chest     chests[8];
int       opened_chests_count = 0;
SDL_Color textColor = {255, 255, 255, 255};

typedef enum {
    GAME_RUNNING = 0,
    GAME_REAPER_WIN,
    GAME_SURVIVORS_WIN
} GameResult;

#define CHESTS_NEEDED_TO_OPEN_EXIT 6

/* Exit-dörren ligger längst upp i mitten, precis ovanför spawnområdet.
   Den är stängd tills 6 av 8 kistor är öppnade. */
static const SDL_Rect EXIT_DOOR_DST     = {928, 64, 96, 96};
static const SDL_Rect EXIT_DOOR_BLOCKER = {928, 72, 96, 88};
static const SDL_Rect EXIT_DOOR_TRIGGER = {948, 70, 56, 96};

/* mainlevbuild.png: stängd port/grind och öppen dörröppning. */
static const SDL_Rect EXIT_DOOR_CLOSED_SRC = {487, 202, 82, 79};
static const SDL_Rect EXIT_DOOR_OPEN_SRC   = {640,   0, 80, 96};

static int countOpenChests(void);

/* ══════════════════════════════════════════════════════════════════════════
 * AUDIO GLOBALS
 * ══════════════════════════════════════════════════════════════════════════ */
Mix_Music *bgmLobby        = NULL;
Mix_Music *bgmGame         = NULL;
Mix_Chunk *sfxAttack       = NULL;
Mix_Chunk *sfxChestSuccess = NULL;
Mix_Chunk *sfxExitDoor     = NULL;
Mix_Chunk *sfxOpening      = NULL;
Mix_Chunk *sfxSteps        = NULL;
Mix_Chunk *sfxGhost        = NULL;
Mix_Chunk *sfxBlood        = NULL;
Mix_Chunk *sfxDing         = NULL;
static bool exitDoorSoundPlayed = false;

/* Dedikerade kanaler för loopade ljud – aldrig -1 (auto) för dessa */
#define SFX_CHANNEL_STEPS   1   /* fotsteg/spökljud – loopas tills rörelse slutar */
#define SFX_CHANNEL_CHEST   2   /* kist-öppningsljud – loopas tills E släpps     */

static bool stepsPlaying = false; /* håller koll på om stegsljud redan loopar */

static bool rectsOverlap(SDL_Rect a, SDL_Rect b)
{
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

static bool isExitDoorOpen(void)
{
    return countOpenChests() >= CHESTS_NEEDED_TO_OPEN_EXIT;
}

static bool blockedByClosedExitDoor(float x, float y, int w, int h)
{
    if (isExitDoorOpen()) return false;
    SDL_Rect pr = {(int)x, (int)y, w, h};
    return rectsOverlap(pr, EXIT_DOOR_BLOCKER);
}

static void drawExitDoor(SDL_Renderer* renderer, SDL_Texture* mainLevTex, const SDL_Rect* camera)
{
    SDL_Rect src = isExitDoorOpen() ? EXIT_DOOR_OPEN_SRC : EXIT_DOOR_CLOSED_SRC;
    SDL_Rect dst = {
        EXIT_DOOR_DST.x - camera->x,
        EXIT_DOOR_DST.y - camera->y,
        EXIT_DOOR_DST.w,
        EXIT_DOOR_DST.h
    };
    SDL_RenderCopy(renderer, mainLevTex, &src, &dst);
}


static int countOpenChests(void)
{
    int count = 0;
    for (int i = 0; i < 8; i++) {
        if (chests[i].is_open) count++;
    }
    return count;
}

static void drawChestCounter(SDL_Renderer* renderer, TTF_Font* font, SDL_Texture* chestTex)
{
    if (!font) return;

    opened_chests_count = countOpenChests();

    SDL_RenderSetScale(renderer, 1.0f, 1.0f);

    SDL_Rect panel = {16, 16, 155, 46};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
    SDL_RenderFillRect(renderer, &panel);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 220);
    SDL_RenderDrawRect(renderer, &panel);

    if (chestTex) {
        SDL_Rect chestSrc = {0, 32 * 2, 48, 32};
        SDL_Rect chestDst = {25, 23, 42, 28};
        SDL_RenderCopy(renderer, chestTex, &chestSrc, &chestDst);
    }

    char counterText[16];
    snprintf(counterText, sizeof(counterText), "%d / 8", opened_chests_count);

    SDL_Color counterColor = {255, 255, 255, 255};
    SDL_Surface* surface = TTF_RenderText_Solid(font, counterText, counterColor);
    if (surface) {
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
        if (texture) {
            SDL_Rect textRect = {78, 25, surface->w, surface->h};
            SDL_RenderCopy(renderer, texture, NULL, &textRect);
            SDL_DestroyTexture(texture);
        }
        SDL_FreeSurface(surface);
    }
}


static int countConnectedPlayers(const bool connected[MAX_PLAYERS])
{
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (connected[i]) count++;
    }
    return count;
}

static void resetToStartScreen(Player players[MAX_PLAYERS],
                               bool lobby_connected[MAX_PLAYERS],
                               int* myId,
                               int* isInMenu,
                               GameResult* gameResult,
                               bool* isAttacking)
{
    for (int i = 0; i < 8; i++) {
        chests[i].is_open = false;
    }
    opened_chests_count = 0;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        player_reset(&players[i], i);
        lobby_connected[i] = false;
    }

    *myId = -1;
    *isInMenu = 1;
    *gameResult = GAME_RUNNING;
    *isAttacking = false;

    /* Stoppa loopade kanalljud så de inte fortsätter in i nästa match */
    Mix_HaltChannel(SFX_CHANNEL_STEPS);
    Mix_HaltChannel(SFX_CHANNEL_CHEST);
    stepsPlaying = false;
}

static void drawCenteredText(SDL_Renderer* renderer, TTF_Font* font,
                             const char* text, int centerX, int y, SDL_Color color)
{
    if (!font || !text) return;
    SDL_Surface* surface = TTF_RenderText_Solid(font, text, color);
    if (!surface) return;

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture) {
        SDL_Rect dst = {centerX - surface->w / 2, y, surface->w, surface->h};
        SDL_RenderCopy(renderer, texture, NULL, &dst);
        SDL_DestroyTexture(texture);
    }
    SDL_FreeSurface(surface);
}

static bool canSpectatePlayer(const Player players[MAX_PLAYERS],
                              const bool connected[MAX_PLAYERS],
                              int id, int myId)
{
    return id >= 1 && id < MAX_PLAYERS && id != myId &&
           connected[id] &&
           players[id].is_alive && !players[id].has_escaped;
}

static int findNextSpectateTarget(const Player players[MAX_PLAYERS],
                                  const bool connected[MAX_PLAYERS],
                                  int myId, int currentTarget)
{
    int start = currentTarget;
    if (start < 1 || start >= MAX_PLAYERS) start = myId;

    for (int step = 1; step < MAX_PLAYERS; step++) {
        int id = ((start - 1 + step) % (MAX_PLAYERS - 1)) + 1;
        if (canSpectatePlayer(players, connected, id, myId))
            return id;
    }

    return -1;
}

static void drawSpectatorBanner(SDL_Renderer* renderer, TTF_Font* font, int targetId,
                                int screenW)
{
    if (!font || targetId < 1) return;

    SDL_RenderSetScale(renderer, 1.0f, 1.0f);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_Rect panel = {screenW / 2 - 230, 16, 460, 58};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 210);
    SDL_RenderDrawRect(renderer, &panel);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Color gray  = {200, 200, 200, 255};

    char title[64];
    snprintf(title, sizeof(title), "SPECTATING PLAYER %d", targetId);
    drawCenteredText(renderer, font, title, screenW / 2, 24, white);
    drawCenteredText(renderer, font, "Press TAB to spectate next player", screenW / 2, 48, gray);
}


static bool anySurvivorEscaped(const Player players[MAX_PLAYERS])
{
    for (int i = 1; i < MAX_PLAYERS; i++) {
        if (players[i].has_escaped) return true;
    }
    return false;
}

static bool allSurvivorsDead(const Player players[MAX_PLAYERS])
{
    /* Game over för survivors ska bara komma när alla tre survivors är döda.
       Vi kollar därför plats 1, 2 och 3 direkt i stället för bara anslutna spelare. */
    for (int i = 1; i < MAX_PLAYERS; i++) {
        /* Om någon redan har escaped har survivors redan vunnit, inte förlorat. */
        if (players[i].has_escaped) return false;

        /* Om minst en survivor fortfarande lever är matchen inte slut. */
        if (players[i].is_alive) return false;
    }

    return true;
}

static GameResult getGameResult(const Player players[MAX_PLAYERS],
                                const bool connected[MAX_PLAYERS])
{
    (void)connected;

    if (anySurvivorEscaped(players))
        return GAME_SURVIVORS_WIN;

    if (allSurvivorsDead(players))
        return GAME_REAPER_WIN;

    return GAME_RUNNING;
}

static void drawEndScreen(SDL_Renderer* renderer, TTF_Font* font,
                          bool localPlayerIsReaper, GameResult result,
                          int screenW, int screenH)
{
    if (result == GAME_RUNNING) return;

    SDL_RenderSetScale(renderer, 1.0f, 1.0f);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 210);
    SDL_Rect overlay = {0, 0, screenW, screenH};
    SDL_RenderFillRect(renderer, &overlay);

    bool reaperWon = (result == GAME_REAPER_WIN);
    bool youWon = localPlayerIsReaper ? reaperWon : !reaperWon;

    SDL_Color titleColor = youWon
        ? (SDL_Color){80, 230, 120, 255}
        : (SDL_Color){230, 60, 80, 255};
    SDL_Color white = {255, 255, 255, 255};

    const char* title = youWon ? "YOU WIN" : "GAME OVER";
    const char* subtitle = reaperWon
        ? "Reaper killed all the survivors."
        : "...";

    drawCenteredText(renderer, font, title, screenW / 2, screenH / 2 - 70, titleColor);
    drawCenteredText(renderer, font, subtitle, screenW / 2, screenH / 2 - 20, white);
    drawCenteredText(renderer, font, "Press SPACE to play again", screenW / 2, screenH / 2 + 30, white);
    drawCenteredText(renderer, font, "Press ESC to quit", screenW / 2, screenH / 2 + 65, white);
}

static void drawLobbyScreen(SDL_Renderer* renderer,
                            SDL_Texture* lobbyTex,
                            SDL_Texture* reaperIdleTex,
                            SDL_Texture* survIdleTex[3],
                            TTF_Font* font,
                            const bool connected[MAX_PLAYERS],
                            int screenW,
                            int screenH)
{
    SDL_RenderSetScale(renderer, 1.0f, 1.0f);

    /* lobby_bild.png ska vara själva lobbyn med grå P1/P2/P3/Reaper inritade.
       Koden ritar bara färgade sprites ovanpå de grå när en plats är connected. */
    if (lobbyTex) {
        SDL_RenderCopy(renderer, lobbyTex, NULL, NULL);
    } else {
        SDL_SetRenderDrawColor(renderer, 15, 0, 5, 255);
        SDL_RenderClear(renderer);
    }

    /* Positionerna är satta efter lobby_bild.png som är 1600x900.
       De skalas automatiskt till spelets 1280x720-fönster. */
// 1. Koordinater och skalning (Använd dessa värden för att det ska pricka skuggorna)
    const float sx = screenW / 1600.0f;
    const float sy = screenH / 900.0f;

    const int survivorCenterX[3] = {349, 633, 938};
    const int survivorY = 270;

    // 2. Rita Survivors (Gubbarna)
    SDL_Rect survivorSrc = {0, 0, 128, 128};
    for (int i = 0; i < 3; i++) {
        if (!connected[i + 1]) continue; 
        if (!survIdleTex[i]) continue;

        SDL_Rect dst = {
            (int)((survivorCenterX[i] - 55) * sx), // Flyttar gubben så den hamnar mitt i skuggan
            (int)(survivorY * sy),
            (int)(406 * sx),                       // Storlek som matchar bakgrundsbilden
            (int)(338 * sy)
        };
        SDL_RenderCopy(renderer, survIdleTex[i], &survivorSrc, &dst);
    }

    // 3. Rita Reaper
    if (connected[0] && reaperIdleTex) {
        SDL_Rect reaperSrc = {0, 0, 48, 48};
        SDL_Rect reaperDst = {
            (int)(672 * sx),
            (int)(687 * sy),
            (int)(200 * sx),
            (int)(200 * sy)
        };
        SDL_RenderCopy(renderer, reaperIdleTex, &reaperSrc, &reaperDst);
    }

    // 4. Texter (Connected-status och instruktioner)
    if (font) {
        SDL_Color green  = {60, 230, 120, 255};
        SDL_Color gray   = {190, 190, 190, 255};
        SDL_Color red    = {230, 60, 80, 255};
        SDL_Color yellow = {255, 255, 0, 255};

        // Status för varje survivor-plats
        for (int i = 0; i < 3; i++) {
            drawCenteredText(renderer, font,
                connected[i + 1] ? "CONNECTED" : "WAITING...",
                (int)(survivorCenterX[i] * sx + 115),
                (int)(610 * sy + 3),
                connected[i + 1] ? green : gray);
        }

        // Status för Reaper-platsen
        drawCenteredText(renderer, font,
            connected[0] ? "CONNECTED" : "WAITING...",
            screenW / 2 - 15,
            (int)(835 * sy + 18),
            connected[0] ? red : gray);

        // Beräkna antal anslutna för instruktionstexten
        int connectedCount = 0;
        for (int i = 0; i < 4; i++) { // 4 = Reaper + 3 Survivors
            if (connected[i]) connectedCount++;
        }
        
        if (connectedCount > 0) {
            drawCenteredText(renderer, font, "Waiting for more players", screenW / 2, (int)(300 * sy), red);
            drawCenteredText(renderer, font, "Press F to Force start", screenW / 2 - 400, (int)(800 * sy), yellow);
        } 
    }
}

static void drawVisionCircle(SDL_Renderer* renderer, const Player* viewer, const SDL_Rect* camera)
{
    int cx = (int)player_center_x(viewer) - camera->x;
    int cy = (int)player_center_y(viewer) - camera->y;
    int r  = (int)player_vision_radius(viewer);

    for (int angle = 0; angle < 360; angle += 3) {
        float rad = angle * (float)M_PI / 180.0f;
        int x = cx + (int)(cosf(rad) * r);
        int y = cy + (int)(sinf(rad) * r);
        SDL_RenderDrawPoint(renderer, x, y);
    }
}

static void sendPlayerState(UDPsocket sd, UDPpacket* sendPacket, IPaddress serverAddr,
                             const Player* player, bool isAttacking, int myId)
{
    if (!sd || !sendPacket || !player) return;

    PlayerPacket pp;
    memset(&pp, 0, sizeof(pp));
    pp.player_id    = player->player_id;
    pp.x            = player->x;
    pp.y            = player->y;
    pp.is_moving    = player->is_moving;
    pp.flip         = player->flip;
    pp.is_attacking = isAttacking;
    pp.is_alive     = player->is_alive;
    pp.is_ready     = (player->player_id == myId && myId != -1); // Set ready when local player has selected role

    for (int c = 0; c < 8; c++)
        pp.chest_states[c] = chests[c].is_open;

    memcpy(sendPacket->data, &pp, sizeof(PlayerPacket));
    sendPacket->len     = sizeof(PlayerPacket);
    sendPacket->address = serverAddr;
    SDLNet_UDP_Send(sd, -1, sendPacket);
}

/* ══════════════════════════════════════════════════════════════════════════
 * TILEMAPS  (oförändrade)
 * ══════════════════════════════════════════════════════════════════════════ */

int tilemap[MAP_ROWS][MAP_COLS] =
{
    {30, 14, 14, 122, 14, 122, 14, 14, 14, 14, 14, 122, 14, 14, 14, 14, 14, 122, 14, 14, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 30, 14, 14, 14, 122, 14, 122, 14, 31},
    {80, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 70, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 80, 90, 90, 90, 90, 90, 90, 90, 70},
    {32, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 32},
    {32, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, 80, 114, 114, 114, 114, 114, 70, VV, VV, VV, VV, 80, 114, 114, 114, 114, 114, 70, VV, VV, VV, VV, VV, VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {32, 114, 114, 114, 114, 114, 70, VV, VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, VV, VV, VV, 80, 114, 114, 114, 114, 114, 114, 70, VV, VV, VV, VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {32, 114, 114, 114, 114, 70, VV, VV, VV, VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {32, 114, 114, 114, 114, 70, VV, VV, VV, VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {80, 114, 114, 114, 114, 70, VV, VV, VV, VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {32, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {32, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, VV, VV, VV, VV, VV, VV, 80, 114, 114, 32},
    {VV, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, VV, VV, VV, VV, VV, VV, 80, 114, 114, 32},
    {80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, VV, VV, VV, VV, VV, VV, 80, 114, 114, 32},
    {32, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, VV, VV, VV, VV, VV, VV, 80, 114, 114, 32},
    {32, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, VV, VV, VV, VV, 80, 114, 114, 114, 114, 32},
    {80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, VV, VV, 80, 114, 114, 114, 114, 114, 114, 32},
    {80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {32, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {32, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70, VV, 80, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {32, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {32, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 32},
    {80, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 70},
    {20, 24, 24, 24, 24, 24, 24, 24, 24, 132, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 132, 24, 24, 24, 24, 24, 24, 24, 132, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 132, 24, 24, 24, 24, 24, 24, 24, 132, 24, 24, 24, 24, 24, 21},
};

int tilemap_layer2[MAP_ROWS][MAP_COLS] =
{
    {VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 31, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV},
    {40, 24, 24, 132, 24, 132, 24, 24, 24, 24, 24, 132, 24, 24, 24, 24, 24, 132, 24, 24, 41, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 40, 24, 24, 24, 132, 24, 132, 24, 41},
    {80, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, 30, 64, 75, 64, 75, 64, 31, VV, VV, VV, VV, 30, 64, 75, 64, 75, 64, 31, VV, VV, VV, VV, VV, VV, VV, 30, 14, 14, 14, 14, 51, VV, VV, VV, VV, VV, VV, VV, 70},
    {80, VV, VV, VV, VV, VV, 30, 14, 14, 14, 31, VV, VV, VV, VV, VV, VV, VV, VV, VV, 34, 61, 22, 85, 22, 85, 22, 41, VV, VV, VV, VV, 40, 22, 85, 22, 85, 22, 60, 31, VV, VV, VV, VV, VV, 30, 61, 24, 24, 24, 24, 21, VV, VV, VV, VV, VV, VV, VV, 70},
    {80, VV, VV, VV, VV, 30, 51, VV, VV, VV, 50, 31, VV, VV, VV, VV, VV, VV, VV, 30, 61, 21, 52, 95, 52, 95, 52, 50, 14, VV, VV, 14, 51, 52, 95, 52, 95, 52, 20, 60, 31, VV, VV, VV, 30, 61, 21, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 70},
    {80, VV, VV, VV, VV, 32, VV, VV, VV, VV, VV, 32, VV, VV, VV, VV, VV, VV, 30, 61, 21, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 20, 60, 31, VV, 30, 61, 21, VV, VV, 30, 14, 14, 31, VV, VV, VV, VV, VV, VV, VV, 70},
    {80, VV, VV, VV, VV, 32, VV, VV, VV, VV, VV, 32, VV, VV, VV, VV, VV, VV, 40, 21, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 20, 41, 30, 61, 21, VV, VV, 30, 61, 24, 24, 60, 31, VV, VV, VV, VV, VV, VV, 70},
    {34, 13, 38, VV, VV, 12, 13, 13, 13, 13, 13, 12, VV, VV, 12, 13, 13, 13, 12, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 50, 61, 21, VV, VV, 30, 61, 21, VV, VV, 20, 41, VV, VV, VV, VV, VV, VV, 70},
    {40, 23, 48, VV, VV, 45, 23, 23, 23, 23, 23, 45, VV, VV, 45, 23, 23, 23, 45, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 20, 21, VV, VV, 30, 61, 21, VV, VV, VV, VV, 50, 14, 14, 31, VV, VV, VV, 70},
    {80, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 30, 61, 21, VV, VV, VV, VV, VV, 20, 24, 24, 54, 31, VV, VV, 70},
    {80, VV, VV, VV, VV, VV, VV, 30, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 36, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 37, 14, 14, 61, 21, VV, VV, VV, VV, VV, VV, VV, VV, VV, 40, 41, VV, VV, 70},
    {50, 31, VV, VV, VV, VV, 30, 61, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 22, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 66, 24, 24, 21, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, 32, VV, VV, 70},
    {20, 60, 31, VV, VV, 30, 61, 21, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 50, 31, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 30, 51, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, 32, VV, VV, 70},
    {VV, 20, 41, VV, VV, 40, 21, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 20, 54, 31, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 30, 61, 21, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 12, 51, VV, VV, 70},
    {VV, VV, 32, VV, VV, 32, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 40, 60, 31, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 30, 61, 21, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 46, 21, VV, VV, 70},
    {VV, VV, 32, VV, VV, 32, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, 20, 60, 31, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 30, 61, 21, VV, VV, VV, VV, VV, 32, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 70},
    {VV, VV, 32, VV, VV, 32, VV, VV, VV, VV, VV, 30, 13, 13, 13, 38, VV, VV, 37, 33, 13, 13, 33, 14, 14, 14, 14, 14, 38, VV, VV, 37, 14, 14, 14, 14, 14, 61, 21, VV, VV, VV, VV, VV, VV, 32, VV, VV, 30, 14, 14, 14, 14, 14, 14, 12, 31, VV, VV, 70},
    {VV, VV, 32, VV, VV, 32, VV, VV, VV, VV, VV, 40, 23, 23, 23, 48, VV, VV, 47, 23, 23, 23, 23, 24, 24, 24, 24, 24, 48, VV, VV, 47, 24, 24, 24, 24, 24, 21, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, 40, 24, 24, 24, 24, 24, 24, 24, 41, VV, VV, 70},
    {30, 14, 51, VV, VV, 50, 14, 14, 14, 14, 14, 12, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, 32, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, 70},
    {40, 24, 21, VV, VV, 20, 24, 24, 24, 24, 24, 45, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, 32, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, 70},
    {80, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 37, 14, 14, 38, VV, VV, 37, 14, 14, 14, 14, 14, 14, 14, 14, 64, 64, 14, 14, 14, 14, 14, 14, 14, 14, 38, VV, VV, 37, 14, 14, 43, VV, VV, 32, VV, VV, VV, VV, VV, 30, 14, 51, VV, VV, 70},
    {80, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 40, 24, 24, 48, VV, VV, 47, 24, 24, 24, 24, 24, 24, 24, 24, 22, 22, 24, 24, 24, 24, 24, 24, 24, 24, 48, VV, VV, 47, 24, 24, 41, VV, VV, 32, VV, VV, VV, 30, 14, 61, 24, 21, VV, VV, 70},
    {34, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 31, VV, VV, 32, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, 32, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, 32, VV, 30, 14, 61, 24, 21, VV, VV, VV, VV, 70},
    {40, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 41, VV, VV, 32, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, 32, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, 32, VV, 40, 24, 21, VV, VV, VV, VV, VV, VV, 70},
    {80, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, 32, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 50, 51, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, 32, VV, 32, VV, VV, VV, VV, VV, VV, VV, VV, 70},
    {80, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, 50, 14, 51, VV, VV, VV, VV, VV, VV, VV, VV, 70},
    {80, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, 20, 24, 21, VV, VV, VV, VV, VV, VV, VV, VV, 70},
    {80, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, 32, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 30, 31, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 32, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, 70},
    {50, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 33, 14, 14, 33, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 33, 33, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 33, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 51},
    {VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, -1, -1, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV},
};

int tilemap_items[MAP_ROWS][MAP_COLS] = {
{ VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV},
    { VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV},
    { VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479, VV},
    { VV,1479,1479,1479,1479,1479, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479, VV},
    { VV,1479,1479,1479,1479, VV, VV, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV},
    { VV,1479,1479,1479,1479, VV, VV, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV,VV, VV, VV, VV,1479,1479, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479, VV},
    { VV,1479,1479,1479,1479, VV, VV, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV, VV, VV,1479,1479, VV, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479, VV},
    { VV, VV, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV,1479,1479, VV, VV, VV,1479,1479, VV, VV,1479,1479,1479,1479,1479,1479, VV},
    { VV, VV, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV,1479,1479, VV, VV, VV,1479,1479,1479,1479, VV, VV, VV, VV,1479,1479,1479, VV},
    { VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV,1479,1479,1479,1479,1479, VV, VV, VV, VV, VV,1479,1479, VV},
    { VV,1479,1479,1479,1479,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV,1479,1479, VV},
    { VV, VV,1479,1479,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV,1479,1479, VV},
    { VV, VV, VV,1479,1479, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV,1479,1479, VV},
    { VV, VV, VV,1479,1479, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV,1479,1479, VV},
    { VV, VV, VV,1479,1479, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV,1479,1479, VV},
    { VV, VV, VV,1479,1479, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV,1479,1479,1479,1479,1479, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV},
    { VV, VV, VV,1479,1479, VV,1479,1479,1479,1479,1479, VV, VV, VV, VV, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV},
    { VV, VV, VV,1479,1479, VV,1479,1479,1479,1479,1479, VV, VV, VV, VV, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV},
    { VV, VV, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV},
    { VV, VV, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV},
    { VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV, VV, VV, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV},
    { VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV, VV, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV, VV, VV, VV,1479,1479, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV},
    { VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV,1479,1479, VV,VV, VV, VV, VV, VV, VV,1479,1479,1479,1479, VV},
    { VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV,1479,1479, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV,1479,1479, VV, VV, VV, VV, VV,1479,1479,1479,1479,1479,1479, VV},
    { VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV,1479,1479, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV,1479,1479, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479, VV},
    { VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV,1479,1479, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479, VV},
    { VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV,1479,1479, VV, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479, VV},
    { VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV,1479,1479, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479,1479, VV},
    { VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV},
    { VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV, VV},
};

/* ══════════════════════════════════════════════════════════════════════════
 * KOLLISIONSDETEKTERING
 *
 * isWall kontrollerar om en given (x,y)-pixel befinner sig i en
 * kolliderbar tile.  Används för rörelsebegränsning.
 *
 * Vi kollar FYRA hörn av spelarens hitbox för att undvika att spelaren
 * fastnar när ett hörn nuddar en vägg.
 * ══════════════════════════════════════════════════════════════════════════ */
bool isWall(float x, float y)
{
    int col = (int)x / TILE_SIZE;
    int row = (int)y / TILE_SIZE;
    if (col < 0 || col >= MAP_COLS || row < 0 || row >= MAP_ROWS) return true;
    int tile1 = tilemap[row][col];
    if (tile1 == 30 || tile1 == 14 || tile1 == 31 || tile1 == 32 ||
        tile1 == 20 || tile1 == 21)
        return true;
    int tile2 = tilemap_layer2[row][col];
    if (tile2 != VV) return true;
    return false;
}

bool isInsideLocalVision(Player* viewer, Player* target, SDL_Rect camera, float zoom, int visionRadius)
{
    int viewerX = (int)((viewer->x + viewer->w / 2 - camera.x) * zoom);
    int viewerY = (int)((viewer->y + viewer->h / 2 - camera.y) * zoom);
    int targetX = (int)((target->x + target->w / 2 - camera.x) * zoom);
    int targetY = (int)((target->y + target->h / 2 - camera.y) * zoom);

    int dx = targetX - viewerX;
    int dy = targetY - viewerY;
    (void)visionRadius;
    int radiusWithBuffer = (int)(player_vision_radius(viewer) * zoom) + 48; /* liten buffer så sprites inte blinkar vid kanten */

    return (dx * dx + dy * dy) <= (radiusWithBuffer * radiusWithBuffer);
}

/* Online-vision: varje klient ritar bara sin EGEN synradie.
 * Andra spelare ritas bara om de är inom den lokala spelarens radie.
 *
 * Prestanda-fix:
 * Den gamla versionen ritade/ändrade väldigt många pixlar varje frame
 * i en render target-texture. Det kan ge mycket lagg.
 * Den här versionen ritar istället mörkret med rektanglar + horisontella
 * linjer runt syncirkeln. Det blir mycket färre draw calls/beräkningar. */
void drawLocalVisionMask(SDL_Renderer* renderer, SDL_Texture* visionTex,
                         Player* viewer, SDL_Rect camera, float zoom,
                         int screenW, int screenH)
{
    (void)visionTex; /* Behålls i funktionssignaturen för att slippa ändra annan kod. */

    const Uint8 darkness = 185;

    /* Viktigt: resten av spelet ritas med SDL_RenderSetScale(renderer, zoom, zoom).
       Masken ska däremot ritas i riktiga skärmpixlar. Annars multipliceras zoom två
       gånger och synfältet hamnar fel/off-center. */
    float oldScaleX = 1.0f;
    float oldScaleY = 1.0f;
    SDL_RenderGetScale(renderer, &oldScaleX, &oldScaleY);
    SDL_RenderSetScale(renderer, 1.0f, 1.0f);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, darkness);

    if (!viewer->is_alive) {
        SDL_Rect fullScreen = {0, 0, screenW, screenH};
        SDL_RenderFillRect(renderer, &fullScreen);
        SDL_RenderSetScale(renderer, oldScaleX, oldScaleY);
        return;
    }

    /* Samma centrum som spelaren har på skärmen:
       world-position minus kamera, därefter multiplicerat med zoom. */
    int cx = (int)roundf((player_center_x(viewer) - camera.x) * zoom);
    int cy = (int)roundf((player_center_y(viewer) - camera.y) * zoom);
    int r  = (int)roundf(player_vision_radius(viewer) * zoom);

    /* 1) Mörka hela området ovanför och under syncirkeln. */
    SDL_Rect top = {0, 0, screenW, cy - r};
    SDL_Rect bottom = {0, cy + r, screenW, screenH - (cy + r)};

    if (top.h > 0) SDL_RenderFillRect(renderer, &top);
    if (bottom.y < screenH && bottom.h > 0) SDL_RenderFillRect(renderer, &bottom);

    /* 2) Mörka området till vänster/höger om cirkeln rad för rad.
       Det här lämnar själva cirkeln transparent så banan syns normalt där. */
    int yStart = cy - r;
    int yEnd   = cy + r;
    if (yStart < 0) yStart = 0;
    if (yEnd >= screenH) yEnd = screenH - 1;

    int rSq = r * r;
    for (int y = yStart; y <= yEnd; y++) {
        int dy = y - cy;
        int inside = rSq - dy * dy;
        if (inside < 0) continue;

        int dx = (int)sqrtf((float)inside);
        int leftEnd = cx - dx;
        int rightStart = cx + dx;

        if (leftEnd > 0) {
            SDL_RenderDrawLine(renderer, 0, y, leftEnd, y);
        }
        if (rightStart < screenW - 1) {
            SDL_RenderDrawLine(renderer, rightStart, y, screenW - 1, y);
        }
    }

    SDL_RenderSetScale(renderer, oldScaleX, oldScaleY);
}

/* ══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char** argv)
{
    const int LEVEL_WIDTH   = MAP_COLS * TILE_SIZE;
    const int LEVEL_HEIGHT  = MAP_ROWS * TILE_SIZE;
    const int SCREEN_WIDTH  = 1280;
    const int SCREEN_HEIGHT = 720;
    float zoom = 2.0f;

    /* ──────────────────────────────────────────────────────────────────────
     * A. SDL-INITIERING
     * ────────────────────────────────────────────────────────────────────── */
    SDL_Init(SDL_INIT_VIDEO);
    if (TTF_Init() == -1) {
        printf("TTF_Init fel: %s\n", TTF_GetError());
        return 1;
    }
    IMG_Init(IMG_INIT_PNG);

    /* ──────────────────────────────────────────────────────────────────────
     * A2. LJUDINITIERING (SDL_mixer)
     *
     * Mix_OpenAudio: frekvens 44100 Hz, stereo, 2048 byte chunks.
     * Alla ljud laddas en gång här och frigörs i cleanup-sektionen.
     * ────────────────────────────────────────────────────────────────────── */
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        printf("Mix_OpenAudio fel: %s\n", Mix_GetError());
    }
    Mix_AllocateChannels(16);

    bgmLobby        = Mix_LoadMUS("assets/Lobby Music.mp3");
    bgmGame         = Mix_LoadMUS("assets/Gameplay Ambience.mp3");
    sfxAttack       = Mix_LoadWAV("assets/Attack.mp3");
    sfxChestSuccess = Mix_LoadWAV("assets/Chest Success.mp3");
    sfxExitDoor     = Mix_LoadWAV("assets/Exit Door.mp3");
    sfxOpening      = Mix_LoadWAV("assets/Opening Chest.mp3");
    sfxSteps        = Mix_LoadWAV("assets/Running footsteps.mp3");
    sfxGhost        = Mix_LoadWAV("assets/Ghost.mp3");
    sfxBlood        = Mix_LoadWAV("assets/Blood.mp3");
    sfxDing         = Mix_LoadWAV("assets/Ding.mp3");

    /* Starta lobbymusiket direkt */
    if (bgmLobby) Mix_PlayMusic(bgmLobby, -1);

    /* ──────────────────────────────────────────────────────────────────────
     * B. NÄTVERKSINITIERING (SDLNet)
     *
     * SDLNet_Init() måste anropas innan något annat nätverks-API används.
     * ────────────────────────────────────────────────────────────────────── */
    if (SDLNet_Init() == -1) {
        printf("SDLNet_Init fel: %s\n", SDLNet_GetError());
        return 1;
    }

    /* ──────────────────────────────────────────────────────────────────────
     * C. ÖPPNA UDP-SOCKET PÅ PORT 0
     *
     * Port 0 innebär att OS väljer en ledig port åt oss.
     * Klienten behöver ingen känd port – det gör bara servern.
     *
     * ⚠️  VARFÖR INTE PORT 12345 HÄR?
     * Om alla klienter på SAMMA dator försökte öppna port 12345
     * skulle bara den första lyckas.  Med port 0 får varje
     * klientprocess en unik port från OS.
     * ────────────────────────────────────────────────────────────────────── */
    UDPsocket sd = SDLNet_UDP_Open(0);
    if (!sd) {
        printf("SDLNet_UDP_Open fel: %s\n", SDLNet_GetError());
        SDLNet_Quit();
        return 1;
    }

    /* ──────────────────────────────────────────────────────────────────────
     * D. ALLOKERA PAKET
     *
     * Samma storlekar som på servern – annars klipps data.
     * ────────────────────────────────────────────────────────────────────── */
    UDPpacket* receivePacket = SDLNet_AllocPacket(sizeof(GameState));
    UDPpacket* sendPacket    = SDLNet_AllocPacket(sizeof(PlayerPacket));

    if (!receivePacket || !sendPacket) {
        printf("SDLNet_AllocPacket fel: %s\n", SDLNet_GetError());
        return 1;
    }

    /* ──────────────────────────────────────────────────────────────────────
     * E. LÖS UPP SERVERADRESSEN
     *
     * SDLNet_ResolveHost() omvandlar ett värdnamn/IP-sträng och ett
     * portnummer till en IPaddress-struct (host + port i nätverksbyteordning).
     *
     * Ändra "127.0.0.1" till serverns faktiska LAN-IP om servern
     * inte kör på samma dator.  I ditt fall "10.22.11.90".
     *
     * ⚠️  VANLIGT MISSTAG: Alla klientdatorer måste ha SERVERNS IP här,
     * inte sin egen.  Serverdatorn kan ha 127.0.0.1 om den både kör
     * servern och spelar som klient, men annars ska den också använda
     * serverns LAN-IP.
     * ────────────────────────────────────────────────────────────────────── */
    IPaddress serverAddr;
    if (SDLNet_ResolveHost(&serverAddr, "172.20.10.12", SERVER_PORT) == -1) {
        printf("SDLNet_ResolveHost fel: %s\n", SDLNet_GetError());
        printf("Kontrollera att serverns IP-adress är korrekt!\n");
        return 1;
    }

    /* ──────────────────────────────────────────────────────────────────────
     * F. LOBBY-VAL
     *
     * Ingen väljer ID i terminalen längre.
     * MY_ID = -1 tills spelaren väljer i lobbyn:
     *   P → första lediga Survivor-plats 1, 2 eller 3
     *   R → Reaper-plats 0
     *
     * lobby_connected[] styr lobbybilden:
     *   false = grå figur
     *   true  = riktig färgad sprite
     * ────────────────────────────────────────────────────────────────────── */

    /* ──────────────────────────────────────────────────────────────────────
     * G. KISTOR
     * ────────────────────────────────────────────────────────────────────── */
    float positions[8][2] = {
        {500, 864}, {1580, 210}, {50, 864},  {1690, 700},
        {540, 100}, {1390, 864}, {190, 480}, {1700, 864}
    };
    for (int i = 0; i < 8; i++) {
        chests[i].x      = positions[i][0];
        chests[i].y      = positions[i][1];
        chests[i].is_open = false;
    }

    /* ──────────────────────────────────────────────────────────────────────
     * H. INITIERA ALLA SPELARE
     *
     * Vi håller en lokal kopia av ALLA 4 spelares state.
     * Din egna (MY_ID) uppdateras från tangentbordet.
     * Andras uppdateras från nätverket (GameState från servern).
     * ────────────────────────────────────────────────────────────────────── */
    /* ──────────────────────────────────────────────────────────────────────
     * H. INITIERA ALLA SPELARE
     *
     * Vi håller en lokal kopia av ALLA 4 spelares state.
     * Din egna (MY_ID) uppdateras från tangentbordet.
     * Andras uppdateras från nätverket (GameState från servern).
     * ────────────────────────────────────────────────────────────────────── */
    Player players[MAX_PLAYERS];
    memset(players, 0, sizeof(players));

    int        MY_ID        = -1;
    int        is_in_menu   = 1;
    GameResult gameResult   = GAME_RUNNING;
    bool       isAttacking  = false;
    bool       lobby_connected[MAX_PLAYERS] = {false, false, false, false};

    resetToStartScreen(players, lobby_connected, &MY_ID,
                       &is_in_menu, &gameResult, &isAttacking);

    /* ──────────────────────────────────────────────────────────────────────
     * I. FÖNSTER OCH RENDERER
     * ────────────────────────────────────────────────────────────────────── */
    SDL_Window*   window   = SDL_CreateWindow("Reaper",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
                                SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);

    SDL_Texture* visionTex = SDL_CreateTexture(renderer,
                                SDL_PIXELFORMAT_RGBA8888,
                                SDL_TEXTUREACCESS_TARGET,
                                SCREEN_WIDTH, SCREEN_HEIGHT);
    SDL_SetTextureBlendMode(visionTex, SDL_BLENDMODE_BLEND);

    /* ──────────────────────────────────────────────────────────────────────
     * J. TEXTURER
     * ────────────────────────────────────────────────────────────────────── */
    SDL_Texture* reaperIdleTex   = IMG_LoadTexture(renderer, "assets/PassiveIdleReaper-Sheet.png");
    SDL_Texture* reaperRunTex    = IMG_LoadTexture(renderer, "assets/PassiveRunningReaper-Sheet.png");
    SDL_Texture* reaperAttackTex = IMG_LoadTexture(renderer, "assets/HostileAttackReaper-Sheet.png");

    SDL_Texture* survIdleTex[3] = {
        IMG_LoadTexture(renderer, "assets/Idle1.png"),
        IMG_LoadTexture(renderer, "assets/Idle2.png"),
        IMG_LoadTexture(renderer, "assets/Idle3.png"),
    };
    SDL_Texture* survRunTex[3] = {
        IMG_LoadTexture(renderer, "assets/Run1.png"),
        IMG_LoadTexture(renderer, "assets/Run2.png"),
        IMG_LoadTexture(renderer, "assets/Run3.png"),
    };
    SDL_Texture* survDeadTex[3] = {
        IMG_LoadTexture(renderer, "assets/Dead1.png"),
        IMG_LoadTexture(renderer, "assets/Dead2.png"),
        IMG_LoadTexture(renderer, "assets/Dead3.png"),
    };
    int survDeadFrames[3] = {4, 4, 5};

    SDL_Texture* menuTex    = IMG_LoadTexture(renderer, "assets/lobby_bild.png");
    SDL_Texture* backgroundTex    = IMG_LoadTexture(renderer, "assets/bakgrund_spel.png");
    SDL_Texture* grassTex   = IMG_LoadTexture(renderer, "assets/DarkDungeon.png");
    SDL_Texture* mainLevTex = IMG_LoadTexture(renderer, "assets/mainlevbuild.png");
    SDL_Texture* chestTex   = IMG_LoadTexture(renderer, "assets/Chests.png");
    TTF_Font*    font       = TTF_OpenFont("assets/alagard.ttf", 24);

    const int REAPER_FW = 48, REAPER_FH = 48;
    const int SURV_FW   = 128, SURV_FH  = 128;
    const int SURV_IDLE_FC = 6, SURV_RUN_FC = 10;

    Uint32 lastFrameTime[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++)
        lastFrameTime[i] = SDL_GetTicks();

    SDL_Rect camera = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
    bool     isRunning = true;
    SDL_Event event;

    int    spectateTargetId = -1;

    Uint32 lastSendTime = 0;
    static Uint32 lastTick = 0;

    /* ══════════════════════════════════════════════════════════════════════
     * HUVUDLOOP
     * ══════════════════════════════════════════════════════════════════════ */
    while (isRunning)
    {
        /* ────────────────────────────────────────────────────────────────
         * DELTA TIME
         *
         * deltaTime = sekunder sedan förra bildrutan.
         * Multiplikation med deltaTime gör rörelsen frame-rate-oberoende:
         *   pixel/frame → pixel/sekund
         * Vi begränsar till 50 ms (20 fps minimum) för att undvika
         * stora hopp vid laggar eller breakpoints.
         * ──────────────────────────────────────────────────────────────── */
        if (lastTick == 0) lastTick = SDL_GetTicks();
        Uint32 currentTick = SDL_GetTicks();
        float deltaTime = (currentTick - lastTick) / 1000.0f;
        if (deltaTime > 0.05f) deltaTime = 0.05f;
        lastTick = currentTick;

        /* ────────────────────────────────────────────────────────────────
         * EVENT-HANTERING
         * ──────────────────────────────────────────────────────────────── */
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT) isRunning = false;

            if (event.type == SDL_KEYDOWN) {
                if ((is_in_menu || gameResult != GAME_RUNNING) &&
                    event.key.keysym.sym == SDLK_ESCAPE) {
                    isRunning = false;
                }

                if (gameResult != GAME_RUNNING &&
                    event.key.keysym.sym == SDLK_SPACE) {
                    resetToStartScreen(players, lobby_connected, &MY_ID,
                                       &is_in_menu, &gameResult, &isAttacking);
                    spectateTargetId = -1;
                    continue;
                }
            }

            if (is_in_menu && event.type == SDL_KEYDOWN) {
                int oldSelectedId = MY_ID;

                // Handle role selection (only if no role selected yet)
                if (MY_ID == -1) {
                    if (event.key.keysym.sym == SDLK_r && !lobby_connected[0]) {
                        MY_ID = 0;
                        lobby_connected[0] = true;
                        spectateTargetId = -1;
                        printf("You're playing as Reaper (ID 0)\n");
                    } else if (event.key.keysym.sym == SDLK_1 && !lobby_connected[1]) {
                        MY_ID = 1;
                        lobby_connected[1] = true;
                        spectateTargetId = -1;
                        printf("You're playing as Survivor 1 (ID 1)\n");
                    } else if (event.key.keysym.sym == SDLK_2 && !lobby_connected[2]) {
                        MY_ID = 2;
                        lobby_connected[2] = true;
                        spectateTargetId = -1;
                        printf("You're playing as Survivor 2 (ID 2)\n");
                    } else if (event.key.keysym.sym == SDLK_3 && !lobby_connected[3]) {
                        MY_ID = 3;
                        lobby_connected[3] = true;
                        spectateTargetId = -1;
                        printf("You're playing as Survivor 3 (ID 3)\n");
                    } else if (event.key.keysym.sym == SDLK_p) {
                        /* P väljer första lediga survivor-plats enligt den lobby-state
                           vi fått från servern. Eftersom valda roller nu skickas redan
                           i lobbyn kommer nästa klient se P1 upptagen och välja P2/P3. */
                        for (int id = 1; id <= 3; id++) {
                            if (!lobby_connected[id]) {
                                MY_ID = id;
                                lobby_connected[id] = true;
                                spectateTargetId = -1;
                                printf("You're playing as (ID %d)\n", MY_ID);
                                break;
                            }
                        }
                    }
                }

                if (MY_ID != -1 && MY_ID != oldSelectedId) {
                    players[MY_ID].player_id = MY_ID;
                    lobby_connected[MY_ID] = true;
                    sendPlayerState(sd, sendPacket, serverAddr, &players[MY_ID], false, MY_ID);
                    lastSendTime = SDL_GetTicks();
                }
                
                // Handle F key for force start (only if a role has been selected)
                if (event.key.keysym.sym == SDLK_f && MY_ID != -1) {
                    // Force start with F when local player has selected a role
                    is_in_menu = 0;
                    printf("Force starting game!\n");
                    Mix_HaltMusic();
                    if (bgmGame) Mix_PlayMusic(bgmGame, -1);
                }
                
                // Auto-start game when all players are connected
                if (MY_ID != -1) {
                    int connectedCount = 0;
                    for (int i = 0; i < MAX_PLAYERS; i++) {
                        if (lobby_connected[i]) connectedCount++;
                    }
                    if (connectedCount == MAX_PLAYERS) {
                        is_in_menu = 0;
                        printf("All players connected - starting game!\n");
                        Mix_HaltMusic();
                        if (bgmGame) Mix_PlayMusic(bgmGame, -1);
                    }
                }
            }

            /* Reaper startar attack med SPACE */
            if (!is_in_menu && MY_ID == 0 && gameResult == GAME_RUNNING) {
                if (event.type  == SDL_KEYDOWN &&
                    event.key.keysym.scancode == SDL_SCANCODE_SPACE &&
                    !isAttacking) {
                    isAttacking = true;
                    players[0].current_frame = 0;
                    if (sfxAttack) Mix_PlayChannel(-1, sfxAttack, 0);
                }
            }

            /* Döda survivors kan spectata levande kamrater med TAB. */
            if (!is_in_menu && MY_ID > 0 && gameResult == GAME_RUNNING &&
                event.type == SDL_KEYDOWN &&
                event.key.keysym.scancode == SDL_SCANCODE_TAB &&
                !players[MY_ID].is_alive) {
                spectateTargetId = findNextSpectateTarget(players, lobby_connected, MY_ID, spectateTargetId);
            }
        }

        /* Ta emot lobby-state även medan vi är kvar i lobbyn.
           Då blir andra klienters figurer färgade när de har valt roll. */
        if (is_in_menu) {
            while (SDLNet_UDP_Recv(sd, receivePacket) == 1) {
                if (receivePacket->len != sizeof(GameState)) continue;

                GameState gs;
                memcpy(&gs, receivePacket->data, sizeof(GameState));

                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (gs.players[i].player_id == i && gs.players[i].is_ready)
                        lobby_connected[i] = true;
                }
            }
        }

        /* Skicka en lobby-heartbeat efter rollval.
           Detta är bugfixen som gör att andra klienter ser platsen som upptagen
           redan i lobbyn, och att P-snabbvalet väljer nästa lediga survivor. */
        if (is_in_menu && MY_ID != -1) {
            Uint32 now = SDL_GetTicks();
            if (now - lastSendTime > 100) {
                lastSendTime = now;
                players[MY_ID].player_id = MY_ID;
                sendPlayerState(sd, sendPacket, serverAddr, &players[MY_ID], false, MY_ID);
            }
        }

        SDL_RenderClear(renderer);

        /* ════════════════════════════════════════════════════════════════
         * MENYLÄGE
         * ════════════════════════════════════════════════════════════════ */
        if (is_in_menu)
        {
            drawLobbyScreen(renderer, menuTex, reaperIdleTex, survIdleTex, font,
                            lobby_connected, SCREEN_WIDTH, SCREEN_HEIGHT);
            
        }
        /* ════════════════════════════════════════════════════════════════
         * SPELLÄGE
         * ════════════════════════════════════════════════════════════════ */
        else
        {
            const Uint8* ks     = SDL_GetKeyboardState(NULL);
            Player*      me     = &players[MY_ID];
            bool         isMoving = false;

            /* ────────────────────────────────────────────────────────────
             * 1. RÖRELSELOGIK (klientauktoritativ)
             *
             * Din position räknas ut lokalt.  Ingen väntan på servern.
             * Resultatet skickas till servern → broadcastas → andra ser dig.
             *
             * Kollisionskontroll: vi testar nextX/nextY separat för X och Y
             * så att spelaren kan glida längs väggar istället för att
             * fastna helt.
             * ──────────────────────────────────────────────────────────── */
            if (gameResult == GAME_RUNNING && me->is_alive && !me->has_escaped && !isAttacking)
            {
                float nextX = me->x;
                float nextY = me->y;

                if (ks[SDL_SCANCODE_W]) { nextY -= me->speed * deltaTime; isMoving = true;}
                if (ks[SDL_SCANCODE_S]) { nextY += me->speed * deltaTime; isMoving = true;}
                if (ks[SDL_SCANCODE_A]) { nextX -= me->speed * deltaTime; isMoving = true; me->flip = true;}
                if (ks[SDL_SCANCODE_D]) { nextX += me->speed * deltaTime; isMoving = true; me->flip = false;}

                // isMoving är redan satt ovan och används även för chest-interaktion.

                if (!isWall(nextX, me->y) &&
                    !isWall(nextX + me->w, me->y) &&
                    !isWall(nextX, me->y + me->h) &&
                    !isWall(nextX + me->w, me->y + me->h) &&
                    !blockedByClosedExitDoor(nextX, me->y, me->w, me->h))
                    me->x = nextX;

                if (!isWall(me->x, nextY) &&
                    !isWall(me->x + me->w, nextY) &&
                    !isWall(me->x, nextY + me->h) &&
                    !isWall(me->x + me->w, nextY + me->h) &&
                    !blockedByClosedExitDoor(me->x, nextY, me->w, me->h))
                    me->y = nextY;
            }
            me->is_moving = isMoving;

            /* ── LJUD: fotsteg / spökljud ──────────────────────────────────
             * Vi använder en dedikerad kanal (SFX_CHANNEL_STEPS) och loopar
             * ljudet (-1 = loop för alltid) så länge spelaren rör sig.
             * När rörelsen slutar stoppas kanalen omedelbart med HaltChannel.
             * ──────────────────────────────────────────────────────────── */
            if (MY_ID != 0) {
                Mix_Chunk *moveSfx = (me->is_alive) ? sfxSteps : sfxGhost;
                if (isMoving && moveSfx) {
                    /* Starta bara om kanalen inte redan spelar RÄTT ljud */
                    if (!stepsPlaying || !Mix_Playing(SFX_CHANNEL_STEPS)) {
                        Mix_PlayChannel(SFX_CHANNEL_STEPS, moveSfx, -1);
                        stepsPlaying = true;
                    }
                } else {
                    /* Spelaren rör sig inte – stoppa direkt */
                    if (stepsPlaying) {
                        Mix_HaltChannel(SFX_CHANNEL_STEPS);
                        stepsPlaying = false;
                    }
                }
            }

            /* ────────────────────────────────────────────────────────────
             * 2. KIST-INTERAKTION (klientauktoritativ, synkad via server)
             *
             * Survivor öppnar kista med E om han är tillräckligt nära.
             * Flaggan chest_states[i] skickas i varje PlayerPacket.
             * Servern OR:ar alla klienters chest_states och broadcastar
             * den samlade sanningen.  → Alla ser öppnade kistor.
             *
             * VARFÖR INTE BARA LOKALT?
             * Om vi bara satte chests[i].is_open = true lokalt skulle
             * andra klienter aldrig veta att kistan öppnats.
             * ──────────────────────────────────────────────────────────── */
            if (gameResult == GAME_RUNNING && MY_ID != 0 && me->is_alive) 
            {
                // Om vi trycker på E (för att starta/hålla igång öppningen)
                if (ks[SDL_SCANCODE_E]) 
                {
                    if (!me->is_opening_chest) 
                    {
                        // Leta efter kista att börja öppna
                        for (int i = 0; i < 8; i++) 
                        {
                            float dx = me->x - chests[i].x;
                            float dy = me->y - chests[i].y;
                            float dist = sqrtf(dx*dx + dy*dy);
                            
                            if (dist < 50.0f && !chests[i].is_open) 
                            {
                                player_start_chest(me, i);
                                if (sfxOpening) Mix_PlayChannel(SFX_CHANNEL_CHEST, sfxOpening, -1);
                                break;
                            }
                        }
                    }
                } 
                else 
                {
                    // E släpptes – stoppa ljud och avbryt öppning
                    if (me->is_opening_chest) {
                        Mix_HaltChannel(SFX_CHANNEL_CHEST);
                    }
                    player_cancel_chest(me);
                }

                // --- Hantera timern och kolla efter avbrott ---
                if (me->is_opening_chest) 
                {
                    // Om spelaren börjar röra sig avbryts öppningen
                    if (isMoving) 
                    {
                        Mix_HaltChannel(SFX_CHANNEL_CHEST);
                        player_cancel_chest(me);
                        printf("Interaction cancelled (player moved)\n");
                    }
                    else 
                    {
                        me->is_moving = false;

                        if (player_tick_chest(me, deltaTime))
                        {
                            Mix_HaltChannel(SFX_CHANNEL_CHEST);
                            chests[me->target_chest_id].is_open = true;
                            player_cancel_chest(me);
                            if (sfxChestSuccess) Mix_PlayChannel(-1, sfxChestSuccess, 0);
                            if (sfxDing) Mix_PlayChannel(-1, sfxDing, 0);
                            printf("Kista %d öppnad via timer!\n", me->target_chest_id);
                        }
                    }
                }
            }

            /* När 6 kistor är öppnade blir dörren en exit.
               En survivor som går in i dörren är klar och kan inte gå tillbaka. */
            if (gameResult == GAME_RUNNING && MY_ID != 0 && me->is_alive && !me->has_escaped && isExitDoorOpen() &&
                player_overlaps_rect(me, EXIT_DOOR_TRIGGER))
            {
                player_escape(me);
                if (sfxExitDoor) Mix_PlayChannel(-1, sfxExitDoor, 0);
                printf("Du gick ut genom dörren och är klar!\n");
            }

            /* ────────────────────────────────────────────────────────────
             * 3. REAPERNS ATTACKLOGIK (FIXAD FÖR NÄTVERKSSYNK)
             *
             * PROBLEMET I DIN URSPRUNGSKOD:
             * Reapern kontrollerade kollision mot players[i] lokalt men
             * satte bara players[i].is_alive = false i sin EGNA lokal
             * array.  Den flaggan skickades INTE till servern på ett
             * sätt som fick servern att propagera döden till andra.
             *
             * LÖSNINGen:
             * Vi sätter players[target].is_alive = false lokalt PLUS
             * sätter players[MY_ID].is_attacking = true i PlayerPacket.
             * Men viktigare: vi skickar OMEDELBART ett extra paket
             * med den döde survivorns state (is_alive=false) så att
             * servern får veta och broadcastar det till alla.
             *
             * I praktiken: vi uppdaterar players[i].is_alive lokalt,
             * och nästa gång vi skickar vårt PlayerPacket innehåller
             * det is_attacking=true.  Servern vet redan om survivorns
             * dödsstate via det SENASTE paketet från survivorn, ELLER
             * via reaperns paket som signalerar attacken.
             *
             * ✅ RÄTT LÖSNING för detta projekt:
             * Reapern skickar ett speciellt döds-paket med target-info.
             * Men för att hålla protokollet enkelt (inga extra pakettyper)
             * använder vi en enklare approach:
             *
             * Reapern detekterar träff lokalt → sätter is_alive=false
             * lokalt på den drabbade → nästa nätverksuppdatering
             * broadcastas är_alive=false av REAPERN (via sin players[]-array).
             * Servern uppdaterar gs.players[target].is_alive = false och
             * broadcastar det till alla.
             *
             * OBS: Detta fungerar eftersom reapern skickar SITT eget
             * PlayerPacket (ID=0), inte survivorns.  Survivorns dödsstatus
             * måste kodas in i GameState av servern.
             *
             * FÖR ATT DET SKA FUNGERA: Vi måste skicka ett SEPARAT
             * "force-update" paket för den döde spelaren direkt när
             * träffen detekteras.  Se koden nedan.
             * ──────────────────────────────────────────────────────────── */
            if (gameResult == GAME_RUNNING && MY_ID == 0 && isAttacking) {
                for (int i = 1; i < MAX_PLAYERS; i++) {
                    if (!players[i].is_alive) continue;

                    int atkX = players[0].flip
                        ? (int)players[0].x - 40
                        : (int)players[0].x + players[0].w;
                    SDL_Rect atkBox = {atkX, (int)players[0].y, 40, players[0].h};
                    SDL_Rect tgtBox = {(int)players[i].x, (int)players[i].y,
                                       players[i].w, players[i].h};

                    if (SDL_HasIntersection(&atkBox, &tgtBox)) {
                        player_kill(&players[i]);
                        if (sfxBlood) Mix_PlayChannel(-1, sfxBlood, 0);

                        /* ✅ SKICKA OMEDELBART ett paket som meddelar servern
                         * om denna spelares dödsstate.
                         *
                         * Vi bygger ett "proxy-paket" för spelaren som dog.
                         * Servern tar emot det och sätter gs.players[i].is_alive=false
                         * och broadcastar det till ALLA klienter direkt.
                         *
                         * Utan detta: servern uppdaterar bara reaperens (ID=0)
                         * state och vet inte att survivor i är dead förrän
                         * survivor i själv skickar ett paket (med is_alive=true,
                         * för han vet inte att han dog!).
                         */
                        PlayerPacket deathNotice;
                        memset(&deathNotice, 0, sizeof(deathNotice));
                        deathNotice.player_id    = i;
                        deathNotice.x            = players[i].x;
                        deathNotice.y            = players[i].y;
                        deathNotice.is_moving    = false;
                        deathNotice.flip         = players[i].flip;
                        deathNotice.is_attacking = false;
                        deathNotice.is_alive     = false;  /* ← NYCKELN */
                        for (int c = 0; c < 8; c++)
                            deathNotice.chest_states[c] = chests[c].is_open;

                        memcpy(sendPacket->data, &deathNotice, sizeof(PlayerPacket));
                        sendPacket->len     = sizeof(PlayerPacket);
                        sendPacket->address = serverAddr;
                        SDLNet_UDP_Send(sd, -1, sendPacket);

                        printf("Reaper killed player %d – death notice sent!\n", i);
                    }
                }
            }

            /* ────────────────────────────────────────────────────────────
             * 4. SKICKA EGET PAKET TILL SERVERN (~60 Hz)
             *
             * Vi skickar ett PlayerPacket med VÅR EGNA state ca 60 gånger
             * per sekund (var 16:e ms).
             *
             * Varför 60 Hz?  Det balanserar bandbredd mot uppdateringsfrekvens.
             * Föreläsningens mönster: memcpy → sätt address → Send.
             *
             * INNEHÅLL I PAKETET:
             * - player_id: vem vi är
             * - x, y:      vår position (bestäms lokalt)
             * - is_moving: animationshint för andra
             * - flip:      riktning (spegla sprite)
             * - is_attacking: om reapern attackerar just nu
             * - is_alive:  alltid true för oss (vi kan inte döda oss själva)
             *              UNDANTAG: om vi redan är dead (survivorn fick
             *              death notice från servern)
             * - chest_states: vilka kistor vi lokalt vet är öppnade
             * ──────────────────────────────────────────────────────────── */
            Uint32 now = SDL_GetTicks();
            if (now - lastSendTime > 16) {
                lastSendTime = now;

                sendPlayerState(sd, sendPacket, serverAddr, me, isAttacking, MY_ID);
            }

            /* ────────────────────────────────────────────────────────────
             * 5. TA EMOT GAMESTATE FRÅN SERVER
             *
             * SDLNet_UDP_Recv() är icke-blockerande.
             * Vi tömmer recv-bufferten i en loop för att inte missa
             * paket om servern skickat flera sedan sist.
             *
             * VART UPPDATERAR VI STATE?
             * - Andra spelares position/rörelse/attack: alltid från nätverket
             * - Vår EGNA position: ALDRIG från nätverket
             *   (annars gummibands vi tillbaka vid lagg)
             * - Dödsstate: från nätverket FÖR ALLA (inklusive oss själva!)
             *   Om servern säger att vi är dead respekterar vi det.
             * - Kistor: OR-logik (open = open)
             * ──────────────────────────────────────────────────────────── */
            while (SDLNet_UDP_Recv(sd, receivePacket) == 1) {
                if (receivePacket->len != sizeof(GameState)) continue;

                GameState gs;
                memcpy(&gs, receivePacket->data, sizeof(GameState));

                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (gs.players[i].player_id == i)
                        lobby_connected[i] = true;
                }

                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (i == MY_ID) {
                        /* ────────────────────────────────────────────────
                         * VÅR EGNA SPELARE:
                         * Vi uppdaterar INTE position från nätverket –
                         * det skulle skapa laggig rörelse.
                         *
                         * MEN vi uppdaterar dödsstate!
                         * Om servern säger att vi är dead (t.ex. reapern
                         * skickade death notice) sätter vi oss själva dead.
                         * ──────────────────────────────────────────────── */
                        if (!gs.players[i].is_alive && me->is_alive) {
                            player_kill(me);
                            spectateTargetId = findNextSpectateTarget(players, lobby_connected, MY_ID, -1);
                            printf("Du dog! (meddelat av servern)\n");
                        }
                        continue;
                    }

                    /* ────────────────────────────────────────────────────
                     * ANDRAS SPELARE:
                     * Uppdatera position, rörelse och attack direkt.
                     *
                     * Dödslogik: is_alive kan bara gå true→false.
                     * Om spelaren redan är dead lokalt, håll fast vid det
                     * även om ett gammalt paket säger is_alive=true
                     * (UDP garanterar inte ordning!).
                     * ──────────────────────────────────────────────────── */
                    players[i].x          = gs.players[i].x;
                    players[i].y          = gs.players[i].y;
                    if (players[i].x < -5000.0f && players[i].y < -5000.0f) {
                        players[i].has_escaped = true;
                        players[i].is_moving = false;
                    }
                    players[i].is_moving  = gs.players[i].is_moving;
                    players[i].flip       = gs.players[i].flip;
                    players[i].is_attacking = gs.players[i].is_attacking;

                    /* Dödssynk: om servern bekräftar att spelaren är dead */
                    if (!gs.players[i].is_alive && players[i].is_alive) {
                        player_kill(&players[i]);
                        printf("Player %d died (Confirmed by server)\n", i);
                    }

                    /* Uppdatera reaperns attack-animation på andra klienter */
                    if (i == 0) {
                        /* Om servern säger att reapern attackerar men vi
                         * inte visste om det lokalt, starta animationen */
                        if (gs.players[0].is_attacking && !players[0].is_attacking) {
                            players[0].current_frame = 0;
                        }
                    }
                }

                /* Kistsynk: OR-logik */
                for (int c = 0; c < 8; c++) {
                    if (gs.chest_states[c])
                        chests[c].is_open = true;
                }
            }

            /* ────────────────────────────────────────────────────────────
             * 6. VINST/FÖRLUST
             *
             * - Om minst en survivor har escaped (kan bara ske när 6 kistor
             *   är öppnade och exit-dörren är öppen) vinner survivors.
             * - Om alla tre survivors är döda vinner reapern och survivors får Game Over.
             * ──────────────────────────────────────────────────────────── */
            gameResult = getGameResult(players, lobby_connected);
            if (gameResult != GAME_RUNNING) {
                isAttacking = false;
                me->is_moving = false;
                me->is_opening_chest = false;
                Mix_HaltMusic();
            }

            /* Spela exit-dörr-ljud en gång när 6 kistor är öppnade */
            if (!exitDoorSoundPlayed && isExitDoorOpen()) {
                exitDoorSoundPlayed = true;
                if (sfxExitDoor) Mix_PlayChannel(-1, sfxExitDoor, 0);
            }

            bool isSpectating = (gameResult == GAME_RUNNING && MY_ID > 0 &&
                                 !me->is_alive);
            if (isSpectating && !canSpectatePlayer(players, lobby_connected, spectateTargetId, MY_ID)) {
                spectateTargetId = findNextSpectateTarget(players, lobby_connected, MY_ID, spectateTargetId);
            }
            Player* viewPlayer = (isSpectating && spectateTargetId != -1)
                ? &players[spectateTargetId]
                : me;

            /* ────────────────────────────────────────────────────────────
             * 7. KAMERA
             * ──────────────────────────────────────────────────────────── */
            camera.x = (int)(viewPlayer->x + viewPlayer->w / 2) - (int)((SCREEN_WIDTH  / zoom) / 2);
            camera.y = (int)(viewPlayer->y + viewPlayer->h / 2) - (int)((SCREEN_HEIGHT / zoom) / 2);
            if (camera.x < 0) camera.x = 0;
            if (camera.y < 0) camera.y = 0;
            if (camera.x > LEVEL_WIDTH  - (int)(SCREEN_WIDTH  / zoom))
                camera.x = LEVEL_WIDTH  - (int)(SCREEN_WIDTH  / zoom);
            if (camera.y > LEVEL_HEIGHT - (int)(SCREEN_HEIGHT / zoom))
                camera.y = LEVEL_HEIGHT - (int)(SCREEN_HEIGHT / zoom);

            /* ────────────────────────────────────────────────────────────
             * 7. ANIMERING
             * ──────────────────────────────────────────────────────────── */
            now = SDL_GetTicks();
            for (int i = 0; i < MAX_PLAYERS; i++)
            {
                Player* p = &players[i];
                if (now > lastFrameTime[i] + 100) {
                    lastFrameTime[i] = now;

                    if (i == 0) {
                        /* Reaper */
                        bool reaperAttacking = (i == MY_ID) ? isAttacking
                                                             : p->is_attacking;
                        SDL_Texture* curTex = reaperAttacking ? reaperAttackTex
                                            : p->is_moving    ? reaperRunTex
                                                              : reaperIdleTex;
                        int texW;
                        SDL_QueryTexture(curTex, NULL, NULL, &texW, NULL);
                        int fc = texW / REAPER_FW;
                        p->current_frame++;
                        if (p->current_frame >= fc) {
                            p->current_frame = 0;
                            if (i == MY_ID && isAttacking)
                                isAttacking = false;
                            if (i != MY_ID)
                                p->is_attacking = false;
                        }
                    } else {
                        int si = i - 1;
                        if (!p->is_alive) {
                            if (!p->is_dead_anim_done) {
                                p->current_frame++;
                                if (p->current_frame >= survDeadFrames[si]) {
                                    p->current_frame     = survDeadFrames[si] - 1;
                                    p->is_dead_anim_done = true;
                                }
                            }
                        } else {
                            int fc = p->is_moving ? SURV_RUN_FC : SURV_IDLE_FC;
                            p->current_frame = (p->current_frame + 1) % fc;
                        }
                    }
                }
            }

            /* ────────────────────────────────────────────────────────────
             * 8. RENDERING – TILEMAP
             * ──────────────────────────────────────────────────────────── */
            SDL_RenderSetScale(renderer, zoom, zoom);

            int tileset_tile_w = 16, tileset_tile_h = 16;
            int dungeon_cols   = 10;
            int mainlev_cols   = 64;

            SDL_Rect bgDst = {
            0 - camera.x, 
            0 - camera.y, 
            MAP_COLS * TILE_SIZE, // 1920
            MAP_ROWS * TILE_SIZE  // 960
            };
            SDL_RenderCopy(renderer, backgroundTex, NULL, &bgDst);

            for (int row = 0; row < MAP_ROWS; row++) {
                for (int col = 0; col < MAP_COLS; col++) {
                    SDL_Rect destRect = {
                        col * TILE_SIZE - camera.x,
                        row * TILE_SIZE - camera.y,
                        TILE_SIZE, TILE_SIZE
                    };
                    if (destRect.x + TILE_SIZE > 0 && destRect.x < SCREEN_WIDTH &&
                        destRect.y + TILE_SIZE > 0 && destRect.y < SCREEN_HEIGHT)
                    {
                        int tile1 = tilemap[row][col];
                        SDL_Rect s1 = {(tile1 % dungeon_cols) * tileset_tile_w,
                                       (tile1 / dungeon_cols) * tileset_tile_h,
                                        tileset_tile_w, tileset_tile_h};
                        SDL_RenderCopy(renderer, grassTex, &s1, &destRect);

                        int tile2 = tilemap_layer2[row][col];
                        if (tile2 != VV) {
                            SDL_Rect s2 = {(tile2 % dungeon_cols) * tileset_tile_w,
                                           (tile2 / dungeon_cols) * tileset_tile_h,
                                            tileset_tile_w, tileset_tile_h};
                            SDL_RenderCopy(renderer, grassTex, &s2, &destRect);
                        }

                        int tile3 = tilemap_items[row][col];
                        if (tile3 != VV) {
                            SDL_Rect s3 = {(tile3 % mainlev_cols) * tileset_tile_w,
                                           (tile3 / mainlev_cols) * tileset_tile_h,
                                            tileset_tile_w, tileset_tile_h};
                            SDL_RenderCopy(renderer, mainLevTex, &s3, &destRect);
                        }
                    }
                }
            }

            /* Rita exit-dörren ovanför spawn. Den byter automatiskt från stängd till öppen vid 6/8 kistor. */
            drawExitDoor(renderer, mainLevTex, &camera);

            /* Rita kistor */
            int chest_tile_w = 48, chest_tile_h = 32;
            for (int i = 0; i < 8; i++) {
                SDL_Rect srcRect = chests[i].is_open
                    ? (SDL_Rect){chest_tile_w,  chest_tile_h * 3, chest_tile_w, chest_tile_h}
                    : (SDL_Rect){0,              chest_tile_h * 2, chest_tile_w, chest_tile_h};
                SDL_Rect destRect = {
                    (int)chests[i].x - camera.x,
                    (int)chests[i].y - camera.y,
                    64, 42
                };
                SDL_RenderCopy(renderer, chestTex, &srcRect, &destRect);
            }

            /* Rita mörker runt synradien ovanpå banan, innan spelarna ritas. */
            drawLocalVisionMask(renderer, visionTex, viewPlayer, camera, zoom, SCREEN_WIDTH, SCREEN_HEIGHT);
            SDL_RenderSetScale(renderer, zoom, zoom);

            /* ────────────────────────────────────────────────────────────
             * 9. RENDERING – SPELARE
             * ──────────────────────────────────────────────────────────── */
            for (int i = 0; i < MAX_PLAYERS; i++)
            {
                Player*      p       = &players[i];

                if (p->has_escaped)
                    continue;

                if (i != MY_ID && p != viewPlayer && !isInsideLocalVision(viewPlayer, p, camera, zoom, 320))
                    continue;

                SDL_Texture* tex     = NULL;
                SDL_Rect     srcRect = {0};
                int          drawSize = 78;

                /* Rita alltid dig själv, men dölj andra om de är utanför
                   din nuvarande spelares synfält. Reaper har större radie. */
                if (i != MY_ID && p != viewPlayer && !player_can_see(viewPlayer, p)) {
                    continue;
                }

                if (i == 0) {
                    bool reaperAttacking = (i == MY_ID) ? isAttacking
                                                        : p->is_attacking;
                    tex = reaperAttacking ? reaperAttackTex
                        : p->is_moving   ? reaperRunTex
                                         : reaperIdleTex;
                    srcRect = (SDL_Rect){p->current_frame * REAPER_FW, 0,
                                         REAPER_FW, REAPER_FH};
                } else {
                    int si = i - 1;
                    if (!p->is_alive) {
                        tex = survDeadTex[si];
                    } else if (p->is_moving) {
                        tex = survRunTex[si];
                    } else {
                        tex = survIdleTex[si];
                    }
                    srcRect = (SDL_Rect){p->current_frame * SURV_FW, 0,
                                         SURV_FW, SURV_FH};
                }

                if (tex) {
                    int renderX, renderY;
                    if (i == 0) {
                        renderX = (int)p->x - (drawSize - p->w) / 2;
                        renderY = (int)p->y - (drawSize - p->h) / 2;
                    } else {
                        renderX = (int)p->x - (drawSize - p->w) / 2;
                        renderY = (int)p->y + p->h - drawSize;
                    }
                    SDL_Rect playerDest = {
                        renderX - camera.x,
                        renderY - camera.y,
                        drawSize, drawSize
                    };
                    SDL_RendererFlip flipFlag = p->flip
                        ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE;
                    SDL_RenderCopyEx(renderer, tex, &srcRect, &playerDest, 0, NULL, flipFlag);
                    if (players[i].is_opening_chest) 
                    {
                        // Positionera mätaren ovanför spelarens huvud
                        int barW = 40;
                        int barH = 6;
                        int barX = (int)players[i].x - camera.x + (players[i].w / 2) - (barW / 2);
                        int barY = (int)players[i].y - camera.y - 30; //30 pixlar över huvudet

                        // Rita bakgrund (Mörkgrå)
                        SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
                        SDL_Rect bgRect = { barX, barY, barW, barH };
                        SDL_RenderFillRect(renderer, &bgRect);

                        // Rita framsteg (Grön)
                        float progress = players[i].chest_timer / 10.0f;
                        if (progress > 1.0f) progress = 1.0f;
                        
                        SDL_SetRenderDrawColor(renderer, 50, 205, 50, 255);
                        SDL_Rect progressRect = { barX, barY, (int)(barW * progress), barH };
                        SDL_RenderFillRect(renderer, &progressRect);

                        // Rita en vit ram
                        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                        SDL_RenderDrawRect(renderer, &bgRect);
                        
                        // Glöm inte att återställa färgen så att nästa debug-ritning inte blir vit
                        SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); 
                    }
                }
            }

            /* Debug-hitbox för din spelare 
            if (!me->has_escaped) {
                SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
                SDL_Rect debugRect = {
                    (int)me->x - camera.x,
                    (int)me->y - camera.y,
                    me->w, me->h
                };
                SDL_RenderDrawRect(renderer, &debugRect);
            }  */

            /* Rita UI sist utan kamera/zoom så den alltid ligger fast i övre vänstra hörnet. */
            drawChestCounter(renderer, font, chestTex);
            if (isSpectating && spectateTargetId != -1) {
                drawSpectatorBanner(renderer, font, spectateTargetId, SCREEN_WIDTH);
            }

            /* Rita slutskärmen allra sist så den ligger ovanpå hela spelet. */
            drawEndScreen(renderer, font, players[MY_ID].is_killer,
                          gameResult, SCREEN_WIDTH, SCREEN_HEIGHT);
        }

        SDL_RenderPresent(renderer);
    }

    /* ──────────────────────────────────────────────────────────────────────
     * STÄDA UPP
     * ────────────────────────────────────────────────────────────────────── */
    Mix_HaltMusic();
    Mix_FreeMusic(bgmLobby);
    Mix_FreeMusic(bgmGame);
    Mix_FreeChunk(sfxAttack);
    Mix_FreeChunk(sfxChestSuccess);
    Mix_FreeChunk(sfxExitDoor);
    Mix_FreeChunk(sfxOpening);
    Mix_FreeChunk(sfxSteps);
    Mix_FreeChunk(sfxGhost);
    Mix_FreeChunk(sfxBlood);
    Mix_FreeChunk(sfxDing);
    Mix_CloseAudio();
    SDL_DestroyTexture(reaperIdleTex);
    SDL_DestroyTexture(reaperRunTex);
    SDL_DestroyTexture(reaperAttackTex);
    for (int i = 0; i < 3; i++) {
        SDL_DestroyTexture(survIdleTex[i]);
        SDL_DestroyTexture(survRunTex[i]);
        SDL_DestroyTexture(survDeadTex[i]);
    }
    SDL_DestroyTexture(grassTex);
    SDL_DestroyTexture(mainLevTex);
    SDL_DestroyTexture(chestTex);
    SDL_DestroyTexture(menuTex);
    SDL_DestroyTexture(backgroundTex);
    SDL_DestroyTexture(visionTex);
    TTF_CloseFont(font);
    SDLNet_FreePacket(receivePacket);
    SDLNet_FreePacket(sendPacket);
    SDLNet_UDP_Close(sd);
    SDLNet_Quit();
    TTF_Quit();
    SDL_Quit();
    return 0;
}