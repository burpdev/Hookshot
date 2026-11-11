/* (hookshot) Co-op progress sync, the game side: reads the game's flag database between frames, hands what changed to the
   merge engine (coop_state.c), and gives the game what the room has through the game's own set routines, so the game takes
   it as if the player had done it. Nothing is read or written while the game is on its title screen, loading or changing
   scene; what arrives meanwhile waits. docs/coop.md says what is shared. */
#ifndef OOT_COOP_H
#define OOT_COOP_H
#include <stdint.h>
#include "a64.h"
#include "coop_state.h"

/* nvnQueuePresentTexture's end (host/nvn_native.c), between frames on the presenting game thread with its core held.
   With co-op off it reads one value and returns. */
void hos_oot_coop_present(Ctx *c);
/* host/nn_fs.c, at every open of a file of the game's data on any thread: notes the start of the title screen, after
   which sync waits for a save to be loaded. With co-op off it reads one value and returns. */
void hos_oot_coop_rom_open(const char *guest_path);

/* For the menu (any thread, never waiting on the game): the room's offer as the co-op thread last saw it (1 when there
   is one), the player's acceptance of the offer of that number (1 when taken or already accepted), the maker's remedies
   (1 when this game is the maker and the request was queued) and the pools' contributors (their count, at most max). */
int hos_oot_coop_offer(CoopOffer *out);
int hos_oot_coop_accept(uint32_t token);
int hos_oot_coop_is_maker(void);
int hos_oot_coop_reset_pools(void);
int hos_oot_coop_remove_key(const char *fingerprint);
int hos_oot_coop_contributors(CoopContributor *out, int max);

/* host/nn_fs.c: the save folder copied, under the lock the save mount's writes take, to <user>/<folder>/<prefix><UTC
   time>/ (the newest keep copies without a prefix are kept; prefixed ones stay until the player deletes them). 1 and
   the copy's path in out ("" when nothing was saved yet), 0 when it failed, -1 when a save file is being written: try
   again later. */
int hos_save_copy(const char *folder, const char *prefix, int keep, char *out, size_t cap);
#endif
