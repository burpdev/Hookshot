// Co-op session layer: one player's game hosts and the others are joined to it over a TCP link each way, and the
// hosting game passes every message on, so three to eight players need nothing but the host's address and port. There is
// no relay and no room to name: the room is the host's listening socket, and it ends when the host's game closes. The
// game has no multiplayer of its own and its `nn::socket` stays offline: co-op is the runtime's own, its sockets on a
// thread of their own. It never touches game state: progress sync and the other players' puppets are layers above this
// one, which send with coop_send and receive with coop_on_message.
//
// Threads. Everything below the "process" heading may be called from any thread; the callbacks run on the coop thread
// (keep them short, they hold up the links) and must not call coop_command, coop_configure or coop_shutdown. coop_send
// copies the message into an outbox the coop thread flushes within about 8 ms; coop_view_get and coop_command take a
// lock the coop thread holds only to copy. The coop thread exists only while co-op is switched on.
//
// The session is off unless HOS_COOP or hookshot_options.ini's coop=1 switches it on (coop.c has the settings);
// docs/coop.md has the rest.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "coop_proto.h"

typedef enum {
    COOP_OFF,           // co-op is off: no socket, no thread work
    COOP_LEFT,          // on, in no room (the status says why after a failure)
    COOP_CONNECTING,    // binding the port to host, or reaching the host's address
    COOP_JOINED,        // in a room: hosting, or joined to one
    COOP_RECONNECTING,  // the host went silent; reaching it again
} CoopState;

// Host: this game holds the room (its own listening socket). Join: this game joins the room at the address given.
// Leave: out of the room, co-op stays on.
typedef enum { COOP_CMD_HOST = 1, COOP_CMD_JOIN, COOP_CMD_LEAVE } CoopCmd;

typedef struct {
    uint8_t id;                 // member id 1..8; 1 is always the host's
    uint8_t away;               // not heard from for a while (its link is still open)
    uint8_t maker;              // the member that holds the room (always id 1): the rules are the ones its key signs
                                // (coop_state.h)
    char name[COOP_NAME_MAX + 1];
    uint64_t session;           // its join's session, as the host's roster lists it (progress sync binds its identity to
                                // it); a member that joins again has a new one
    uint32_t heard_ms;          // milliseconds since the last message of this member that opened; UINT32_MAX never
} CoopPeer;

typedef struct {
    CoopState state;
    unsigned version;           // changes with the state, the room, the roster and the status text
    int hosting;                // this player's game holds the room (its own Host, not another's)
    uint16_t port;              // the port hosted on, or the port joined
    char addr[64];              // joined: the host's address as it was reached ("[26.1.2.3]:43400"); hosting: what the
                                // others type to reach this game, when one address stands out ("" when none does)
    int self;                   // this player's member id; 0 when in no room
    int maker;                  // the member id of the room's maker (the host: 1), 0 when in no room
    int made;                   // this game holds the room (its own Host: its settings are the room's rules, coop_state.h)
    char room[COOP_ROOM_TEXT + 1];  // the room's id as text (16 hex): not a secret and nothing to type — what a signature
                                // says the rules were made for, so a room's signed rules cannot be carried into another
    uint64_t session;           // this player's join session, the one the roster lists for it (0 when in no room); a
                                // roster may list another for it, which progress sync does not sign for
    char name[COOP_NAME_MAX + 1];   // this player's own name (COOP_NAME, cleaned), whatever the roster says
    int n;                      // members of the room, this player included, by id
    CoopPeer member[COOP_MAX_MEMBERS];
    uint32_t ping_ms;           // the round trip to the host; 0 not measured
    char status[96];            // what is going on or what went wrong, in words
    uint32_t tx, rx, lost;      // frames sent, accepted, and received but not accepted (a bad sender, not co-op, a replay)
} CoopView;

// ---- the process's session (the one the settings drive)

// Starts the coop thread when HOS_COOP (or, without it, hookshot_options.ini's coop=) is on: once, with the settings as
// they are then (host/hos_main.c, beside the Discord status). Off, nothing runs: no socket, no thread. Hosting or
// joining still takes the Co-op tab's Host or Join (or HOS_COOP_AUTO=host|join at the start).
void coop_init(void);
// Switches the process's session on or off and sets the player's name and the host's address ("host[:port]") now, from
// any thread (the menu's Co-op tab). On starts the coop thread when none runs (co-op was off at start, or was switched
// off; the first time the port and the test switches are read as coop_init reads them); a new host address leaves the
// room; a new name is told to the room a second after the last change, so typing a name does not shout at every letter.
// Off leaves the room (BYE), closes the links and ends the coop thread: it waits for that, at most 0.4 s, and when it
// returns the view says off unless the thread was held up (a name lookup), in which case the thread ends by itself a
// moment later. The hooks stay registered; tick hooks do not run while co-op is off. An empty name is "Player-" and four
// random hex digits. It changes this run only: the caller keeps hookshot_options.ini's coop=, coop_name=, coop_server=
// and coop_port=, which coop_init reads at the next start; this layer never writes the file. 1 when applied, 0 when the
// system's random source failed or the thread could not start.
int coop_configure(int on, const char *name, const char *server);
// The port hosted on, or joined (0: 43400), applied now: a room this game holds is opened again on it, a room it joined
// is left (the Co-op tab's Port row). hookshot_options.ini's coop_port= and HOS_COOP_PORT are what coop_init reads at the
// next start; this changes this run only.
void coop_set_port(uint16_t port);
// The game is closing (host/hos_relaunch.c, the window's close): says BYE to the room, closes the links and ends the coop
// thread, waiting at most 0.4 s for it; then returns whatever happened (the process ends next). A game that ends any
// other way (killed, crashed) says nothing: the host drops a member it has not heard from for SILENCE_MS, and a joined
// game that loses the host shows it on its Status row (docs/coop.md).
void coop_shutdown(void);
// Host, join or leave. text is required by JOIN (the host's address, "host", "host:port" or "[v6]:port") and is ignored
// by HOST and LEAVE. 1 when queued (also right after coop_configure switched co-op on, before the thread has applied
// it), 0 when the address is not one or co-op is off. The outcome shows in coop_view_get; the view's name is the cleaned
// name in use. A Host while joined, or a Join while hosting, moves this game into the new room.
int coop_command(CoopCmd cmd, const char *text);
// The room's maker removes a member (its id): every game drops its identity (progress sync signs the removal with the
// maker's key, coop_state.h), and the host closes that member's link and refuses it again under that identity. 1 when
// either was done. Only the host's game can have a link to close: a joiner's removal goes as a signed removal message.
int coop_remove_member(int member);
// fn(member, user) when coop_remove_member is called, on the caller's thread, before the host is asked: the layer that
// signs removals (progress sync) registers here; 1 when it removed the member. One hook.
typedef int (*CoopRemoveFn)(int member, void *user);
int coop_on_remove(CoopRemoveFn fn, void *user);
void coop_view_get(CoopView *out);
// This player's id: random for each start of the game, never sent to the other players.
uint64_t coop_client_id(void);

// Peer messages. kind is COOP_K_*; bytes follow it in the message. coop_send goes to every other member, coop_send_to to
// one (a member id). 1 when queued, 0 when in no room, the message is too long (COOP_MAX_MESSAGE bytes) or the outbox is
// full: a message can still be lost, when a link breaks.
int coop_send(uint8_t kind, const void *bytes, size_t n);
int coop_send_to(int member, uint8_t kind, const void *bytes, size_t n);
// fn(from member id, the message bytes after the kind, their count, user) for every message of that kind that came from
// the session the roster lists for its sender. One handler per kind; register before the message can arrive (at
// start-up). Not once only: the window refuses a repeat within the last 64 it took, but four older messages in a row
// start it again, after which a recorded message of that session can arrive again, and a game on the path can send one
// again under a new counter. A handler for which a repeat or an old message matters keeps a counter of its own per
// sender and session inside its messages and takes only one above the last it took (a position message, say, so that an
// old pose does not move a player back); progress sync signs its counter, which only the sender can then move.
typedef void (*CoopMsgFn)(int from, const uint8_t *bytes, size_t n, void *user);
int coop_on_message(uint8_t kind, CoopMsgFn fn, void *user);
// fn(view, user) after the state, the room, the roster or the status changed; up to 8 hooks. A layer that sends a HELLO
// to every new member listens here.
typedef void (*CoopViewFn)(const CoopView *view, void *user);
int coop_on_view(CoopViewFn fn, void *user);
// fn(now in ms, user) at every step of the coop thread (every 8 ms or less in a room, 250 ms idle); up to 4 hooks. A
// layer with timers of its own (digests, repeated sends) runs them here.
typedef void (*CoopTickFn)(uint64_t now_ms, void *user);
int coop_on_tick(CoopTickFn fn, void *user);

// Reliable events: one id per sender/session, delivered whole once, in enqueue order from each sender to each recipient
// (no order across senders or with ordinary messages). At most 8 queued events, each at most 16 KiB.
// member 0/COOP_DST_ALL snapshots the currently accepted peers; a later join is not a recipient. Leaving, reconnecting
// in a new session, or losing the member/session/acceptance cancels delivery to that member. No other timeout: eventual
// delivery needs a working path and a registered handler at the receiver.
// ACK means dispatch to the callback, NOT permission to apply gameplay; the callback still validates its payload.
// The bytes are borrowed for the callback only. It runs on the session thread, with no session lock held.
enum { COOP_EVENT_SLOTS = 8, COOP_EVENT_INVALID = -2, COOP_EVENT_FULL = -1,
       COOP_EVENT_UNAVAILABLE = 0, COOP_EVENT_QUEUED = 1 };
typedef void (*CoopEventFn)(int from, uint64_t sender_session, uint64_t event_id, const uint8_t *bytes, size_t n, void *user);
int coop_send_event(int member, uint8_t kind, const void *bytes, size_t n);
int coop_on_event(uint8_t kind, CoopEventFn fn, void *user);
// Current roster member, identified and accepted by this player, not removed; false outside a joined session.
int coop_member_accepted(int member);
// Consent owner only: publish bindings for exactly this view (bit per member id). NULL clears them. Stale views
// cannot grant acceptance, and a different member session never inherits a binding. No game code is called here.
void coop_accept_members(const CoopView *view, uint32_t members);

// "host", "host:port", "[v6]:port" or a bare IPv6 address into host (cap bytes) and port (default 43400). 0 when text is
// empty or does not fit.
int coop_server_parse(const char *text, char *host, size_t cap, uint16_t *port);

// ---- a session of its own (tests run two in one process; the process's session is one of these driven by the settings)
typedef struct CoopSess CoopSess;
typedef struct {
    const char *tag;            // names the session in log lines ("coop[A]: ..."); NULL for none
    const char *server;         // the host's "host[:port]" (a host's own session leaves it empty)
    const char *name;           // the player's name
    uint64_t client_id;         // this player's id, the same one across a reconnect
    int on;                     // 0: off (COOP_OFF); 1: on, in no room until Host or Join says otherwise
    uint16_t port;              // the port to host on, or to join when 0 (DEFAULT_PORT)
    unsigned drop_pct;          // percent of received frames thrown away, for tests
    int retry;                  // keep reaching the host (a restart) instead of ending the session
    int host_wanted;            // 1: this session hosts; 0: it joins `server` (what Host or Join asked for)
    // timings in ms, 0 for the defaults (2000, 8000, 1000, 15000); tests shorten them
    uint32_t ping_ms, silence_ms, backoff_first_ms, backoff_max_ms;
} CoopParams;
CoopSess *coop_sess_new(const CoopParams *p);   // p's strings are copied
void coop_sess_free(CoopSess *s);               // the stepping thread must be gone
// The stepping thread calls this in a loop: handles commands, the outbox, the frames waiting on the links and the
// timers, waiting at most 8 ms (in a room), 50 ms (reaching the host) or 250 ms (idle) for the next frame.
void coop_sess_step(CoopSess *s);
int coop_sess_command(CoopSess *s, CoopCmd cmd, const char *text);   // as coop_command, for this session
int coop_sess_remove(CoopSess *s, int member);                        // as coop_remove_member
int coop_sess_send(CoopSess *s, int member, uint8_t kind, const void *bytes, size_t n);   // member 0 or COOP_DST_ALL: all
int coop_sess_on_message(CoopSess *s, uint8_t kind, CoopMsgFn fn, void *user);
int coop_sess_on_view(CoopSess *s, CoopViewFn fn, void *user);
int coop_sess_on_tick(CoopSess *s, CoopTickFn fn, void *user);
int coop_sess_send_event(CoopSess *s, int member, uint8_t kind, const void *bytes, size_t n);
int coop_sess_on_event(CoopSess *s, uint8_t kind, CoopEventFn fn, void *user);
int coop_sess_member_accepted(CoopSess *s, int member);
void coop_sess_accept_members(CoopSess *s, const CoopView *view, uint32_t members);
void coop_sess_view(CoopSess *s, CoopView *out);
// Replaces the settings the session reads with those of p (the timings stay as they were made); the stepping thread
// applies them, on or off included. A new host address leaves the room; a new name, once in a room, is told to the room
// a second after the last change, so typing a name does not re-announce it at every letter.
void coop_sess_configure(CoopSess *s, const CoopParams *p);
// Ends the session as coop_sess_configure with on = 0 would: BYE, the links closed, state COOP_OFF.
void coop_sess_stop(CoopSess *s);
// Waits at most ms for the stepping thread to have carried out a stop (or a configure with on = 0); 1 when it did.
int coop_sess_wait_off(CoopSess *s, uint32_t ms);
