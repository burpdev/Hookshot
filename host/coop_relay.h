/* The optional relay for one room made by this process. No game memory or player-message key is passed here.
   The caller owns start/stop; one relay thread owns every datagram and all room state. */
#pragma once
#include <stdint.h>
#include "coop_proto.h"

enum {
    COOP_RELAY_PACKET_RATE = 480, COOP_RELAY_PACKET_BURST = 960,
    COOP_RELAY_BYTE_RATE = 256 * 1024, COOP_RELAY_BYTE_BURST = 512 * 1024,
    COOP_RELAY_STOP_MS = 200
};
typedef struct CoopRelay CoopRelay;

/* Does not open anything. Environment, when present, wins over coop_host using env_on's rule. */
int coop_relay_enabled(void);
/* Binds all interfaces, IPv4 and IPv6. Only the named locator and the local maker's client/socket may CREATE.
   port=0 chooses a free port (tests); the game's configured default is 24680. NULL on any failure. */
CoopRelay *coop_relay_start(uint16_t port, const uint8_t locator[COOP_KEY_LEN], uint64_t maker, uint16_t maker_port);
uint16_t coop_relay_port(const CoopRelay *relay);
/* Ask, join within COOP_RELAY_STOP_MS, then free. On a scheduling timeout leaves *relay owned by the caller so a
   later stop can join it; its thread still closes its socket before returning. NULL is already stopped. */
int coop_relay_stop(CoopRelay **relay);
/* For the process's Make a room: check that server names this PC (or is empty), start, and log LAN addresses.
   Returns NULL on success or a plain error string. Never logs the locator or the secret. */
const char *coop_relay_host_start(CoopRelay **relay, const char *server, const uint8_t locator[COOP_KEY_LEN],
                                  uint64_t maker, uint16_t maker_port, uint16_t *port);
