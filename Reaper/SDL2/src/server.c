/*
 * server.c  –  ShadowBound UDP-server
 *
 * Kompilera t.ex.:
 *   gcc server.c -o server -lSDL2 -lSDL2_net
 *
 * Kör:
 *   ./server
 *
 * Servern lyssnar på SERVER_PORT (2000) och brodcastar
 * ett GameState-paket till ALLA anslutna klienter varje gång
 * någon klient skickar ett PlayerPacket.
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_net.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../include/network.h"

int main(int argc, char** argv)
{
    /* ──────────────────────────────────────────────────────────────────────
     * 1. INITIERA SDL OCH SDL_NET
     *
     * SDL_Init(0) initierar SDL utan några subsystem (vi behöver
     * inget grafik-subsystem på servern).
     *
     * SDLNet_Init() startar nätverksbiblioteket.  Om det misslyckas
     * finns det inget att göra – vi avslutar direkt.
     * ────────────────────────────────────────────────────────────────────── */
    SDL_Init(0);

    if (SDLNet_Init() == -1) {
        printf("SDLNet_Init fel: %s\n", SDLNet_GetError());
        return 1;
    }

    /* ──────────────────────────────────────────────────────────────────────
     * 2. ÖPPNA UDP-SOCKET PÅ EN KÄND PORT
     *
     * SDLNet_UDP_Open(SERVER_PORT) binder socketen till port 12345.
     * Det är skillnaden mot klienten som anropar SDLNet_UDP_Open(0)
     * (port 0 = OS väljer en ledig port åt klienten).
     *
     * Servern måste ha en KÄND port så att klienterna vet vart de
     * ska skicka sina paket.
     * ────────────────────────────────────────────────────────────────────── */
    UDPsocket serverSocket = SDLNet_UDP_Open(SERVER_PORT);
    if (!serverSocket) {
        printf("SDLNet_UDP_Open fel: %s\n", SDLNet_GetError());
        SDLNet_Quit();
        SDL_Quit();
        return 1;
    }
    printf("Server lyssnar på port %d...\n", SERVER_PORT);

    /* ──────────────────────────────────────────────────────────────────────
     * 3. ALLOKERA PAKET
     *
     * SDLNet_AllocPacket(n) allokerar ett UDPpacket med en databuffert
     * på n byte.  Vi måste se till att storleken exakt matchar de
     * struct:ar vi vill skicka/ta emot, annars trunkeras eller
     * läser vi utanför minnet.
     *
     *   recvPacket – tar emot PlayerPacket från klienter
     *   sendPacket – skickar GameState till klienter
     * ────────────────────────────────────────────────────────────────────── */
    UDPpacket* recvPacket = SDLNet_AllocPacket(sizeof(PlayerPacket));
    UDPpacket* sendPacket = SDLNet_AllocPacket(sizeof(GameState));

    if (!recvPacket || !sendPacket) {
        printf("SDLNet_AllocPacket fel: %s\n", SDLNet_GetError());
        SDLNet_UDP_Close(serverSocket);
        SDLNet_Quit();
        SDL_Quit();
        return 1;
    }

    /* ──────────────────────────────────────────────────────────────────────
     * 4. KLIENT-ADRESSTABELL
     *
     * Vi håller en array med MAX_PLAYERS IPaddress-poster.
     * Varje gång ett paket anländer från en OKÄND (host, port)-kombination
     * lägger vi till den i tabellen.
     *
     * IPaddress innehåller:
     *   .host  – IP-adressen som ett 32-bitars heltal (nätverksbyteordning)
     *   .port  – portnumret (nätverksbyteordning)
     *
     * Vi jämför BÅDA fälten för att avgöra om vi sett klienten förut.
     * ────────────────────────────────────────────────────────────────────── */
    IPaddress clientAddrs[MAX_PLAYERS];
    int       numClients = 0;
    memset(clientAddrs, 0, sizeof(clientAddrs));

    /* ──────────────────────────────────────────────────────────────────────
     * 5. SPELSTATE PÅ SERVERN
     *
     * gs (GameState) är serverns auktoritativa bild av spelet.
     * Den uppdateras varje gång ett PlayerPacket anländer och
     * broadcastas sedan till alla klienter.
     *
     * Vi sätter is_alive = true för alla från början.
     * ────────────────────────────────────────────────────────────────────── */
    GameState gs;
    memset(&gs, 0, sizeof(gs));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        gs.players[i].is_alive  = true;
        gs.players[i].player_id = i;
    }

    /* ──────────────────────────────────────────────────────────────────────
     * 6. HUVUDLOOP
     *
     * SDLNet_UDP_Recv() är icke-blockerande:
     *   - Returnerar 1 om ett paket tagits emot.
     *   - Returnerar 0 om inget paket väntade.
     *   - Returnerar -1 vid fel.
     *
     * Vi kör loopen kontinuerligt.  Utan sleep drar den 100 % CPU
     * men det är acceptabelt för en enkel spelserver.
     * ────────────────────────────────────────────────────────────────────── */
    printf("Väntar på anslutningar...\n");

    while (1)
    {
        if (SDLNet_UDP_Recv(serverSocket, recvPacket) == 1)
        {
            /* ────────────────────────────────────────────────────────────
             * 6a. DESERIALISERA INKOMMANDE PAKET
             *
             * recvPacket->data är en rå byte-buffert.
             * Vi kopierar den till en PlayerPacket-struct med memcpy.
             *
             * recvPacket->address innehåller avsändarens IP + port –
             * det är den adressen vi ska svara på (broadcast till).
             * ──────────────────────────────────────────────────────────── */
            PlayerPacket pp;
            memcpy(&pp, recvPacket->data, sizeof(PlayerPacket));

            int id = pp.player_id;
            if (id < 0 || id >= MAX_PLAYERS) {
                printf("Ogiltigt player_id: %d – ignoreras\n", id);
                continue;
            }

            /* ────────────────────────────────────────────────────────────
             * 6b. REGISTRERA NY KLIENT
             *
             * Vi loopar igenom kända adresser.  Om ingen matchar
             * lägger vi till klienten i tabellen.
             *
             * VIKTIGT: Vi jämför BÅDE host och port.  Bara host räcker
             * inte om flera spelare delar samma IP (t.ex. samma dator,
             * LAN-party med NAT).
             * ──────────────────────────────────────────────────────────── */
            bool known = false;
            for (int i = 0; i < numClients; i++) {
                if (clientAddrs[i].host == recvPacket->address.host &&
                    clientAddrs[i].port == recvPacket->address.port) {
                    known = true;
                    break;
                }
            }
            if (!known && numClients < MAX_PLAYERS) {
                clientAddrs[numClients] = recvPacket->address;
                numClients++;

                /* Konvertera IP till läsbar form för debug-utskrift */
                Uint8* ip = (Uint8*)&recvPacket->address.host;
                printf("Ny klient ID=%d  IP=%d.%d.%d.%d  Port=%d  "
                       "Totalt anslutna: %d\n",
                       id, ip[0], ip[1], ip[2], ip[3],
                       SDL_SwapBE16(recvPacket->address.port),
                       numClients);
            }

            /* ────────────────────────────────────────────────────────────
             * 6c. UPPDATERA SERVERNS GAMESTATE
             *
             * Servern tar emot klientens PlayerPacket och sparar det
             * i gs.players[id].
             *
             * DÖDSREGELN (auktoritativ på servern):
             * Om en klient skickar is_alive = false för en spelare,
             * sätts den spelaren permanent till dead.  Vi tillåter
             * aldrig att en dead spelare "återuppstår" (is_alive
             * kan bara gå från true → false, aldrig tillbaka).
             *
             * Det löser bugg:
             *   Reapern dödar survivor X lokalt → skickar pp med
             *   players[X].is_alive = false till servern → servern
             *   broadcastar det till alla → alla klienter vet att X är dead.
             * ──────────────────────────────────────────────────────────── */

            /* Bevara dead-status: om servern redan vet att spelaren är dead,
             * låt inte ett nytt paket återuppväcka honom. */
            bool was_alive = gs.players[id].is_alive;

            gs.players[id] = pp;  /* Kopiera hela paketet till vår state */

            if (!was_alive) {
                /* Spelaren var redan dead hos servern – håll fast vid det */
                gs.players[id].is_alive = false;
            }

            /* ────────────────────────────────────────────────────────────
             * 6d. KISTSYNKRONISERING
             *
             * En kista som öppnats förblir öppen.
             * Vi OR:ar klientens chest_states med serverns:
             *   Om någon klient har öppnat kista i = true,
             *   sätts gs.chest_states[i] = true för alltid.
             * ──────────────────────────────────────────────────────────── */
            for (int c = 0; c < 8; c++) {
                if (pp.chest_states[c])
                    gs.chest_states[c] = true;
            }

            /* ────────────────────────────────────────────────────────────
             * 6e. BROADCAST GAMESTATE TILL ALLA KLIENTER
             *
             * Vi loopar igenom alla kända klientadresser och skickar
             * samma GameState-paket till var och en.
             *
             * Mönster från föreläsningen:
             *   1. memcpy data till sendPacket->data
             *   2. Sätt sendPacket->len
             *   3. Sätt sendPacket->address för varje mottagare
             *   4. Anropa SDLNet_UDP_Send(socket, -1, packet)
             *      (-1 = använd adress i paketet, inte bunden kanal)
             * ──────────────────────────────────────────────────────────── */
            memcpy(sendPacket->data, &gs, sizeof(GameState));
            sendPacket->len = sizeof(GameState);

            for (int i = 0; i < numClients; i++) {
                sendPacket->address = clientAddrs[i];
                int result = SDLNet_UDP_Send(serverSocket, -1, sendPacket);
                if (result == 0) {
                    Uint8* ip = (Uint8*)&clientAddrs[i].host;
                    printf("VARNING: Kunde inte skicka till %d.%d.%d.%d\n",
                           ip[0], ip[1], ip[2], ip[3]);
                }
            }
        }

        /* Liten CPU-vila: låt OS sova 1 ms om inget paket väntade.
         * Påverkar inte latensen märkbart (1 ms ≈ 1000 paket/s kapacitet). */
        SDL_Delay(1);
    }

    /* ──────────────────────────────────────────────────────────────────────
     * 7. STÄDA UPP (nås aldrig i denna implementation men bra vana)
     * ────────────────────────────────────────────────────────────────────── */
    SDLNet_FreePacket(recvPacket);
    SDLNet_FreePacket(sendPacket);
    SDLNet_UDP_Close(serverSocket);
    SDLNet_Quit();
    SDL_Quit();
    return 0;
}