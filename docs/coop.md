# Co-op

Each player runs their own game and their own save; co-op connects the games of up to 8 players into one room. The
game has no multiplayer of its own and its `nn::socket` stays offline: co-op is the runtime's own, a TCP link of its own
on a thread of its own, built the same on Windows and on Linux.

There is one way in. **One player's game holds the room** — the Co-op tab's **Host** — and the others reach it at that
game's address and port with **Join**. No relay to run, no room to name, no code to type.

| The room | What it is |
|---|---|
| `host/coop.c` | the room and its session: Host and Join, the members and their ids, the roster, who is away, the round trip |
| `host/coop_net.c`, `host/coop_net.h` | its TCP sockets (listen, accept, connect, one frame at a time), portable through `host/posix/` |
| `host/coop_proto.h`, `host/coop_proto.c` | the frames and the player messages, as pure functions on bytes ([below](#the-link-and-the-wire)) |
| `host/coop_state.c`, `host/coop_oot.c` | the room's rules and progress sync: what a room shares, its pools, each player's accepted key, its signed rules ([below](#progress-sync)) |
| `host/oot_coop.c` | the players in the world: where each stands, their bodies and animations, the enemies' one life and one place, PvP, and hearts and Gold Skulltula tokens one for one |
| `host/coop_menu.c` | the Co-op tab's rows, and the settings that carry them to the room |
## One room: Host and Join

**Host** makes this game the room. It listens on port 43400 (the Port row changes it) and its Host address row shows what
the others type: an address of this PC when one stands out, and the log names it too (`hosting the room on port 43400
(room 3f1c…)`). **Join** reaches a room at the Host address row: `host`, `host:port`, `[IPv6]:port` or a bare IPv6
address. Each row switches co-op on for this run; each takes a second press when it would leave a room already in
progress, where the row reads **Stop hosting** or **Leave the room**.

Forward that port on the router and open it in the firewall, as for any game you host at home, or play on a LAN. The room
exists while its host's game runs: when the host closes, the room ends and the others are told. Nothing else is run by
hand and nothing about the room is kept on disk — its members, its rules and each player's place live in the games.

- A room holds up to 8 players. The host is member 1 and always the room's maker; an id is given out as a player joins
  and is not given to someone else while the room lasts, so a hit that names a member means the same player in every
  game.
- A joiner sends HELLO with its name and a session drawn for that join; the host answers WELCOME with the member id, the
  room's id (16 hex, what a signature says the rules were made for — not a secret and nothing to type) and the roster,
  then sends the roster at every change and every 5 s. At most 4 joins wait for their HELLO, which must come within 3 s of
  being accepted.
- The host has not heard from a member for 8 s: it is dropped; silent 5 s: shown away. A joined game sends its host a PING
  every 2 s — the keep-alive that also holds a home router's mapping open, whose PONG gives the round trip the tab shows —
  and takes 8 s without anything from the host as the link being gone: it reaches the same address again after 1, 2, 4, 8
  and 15 s. Its own Host row ends it at once.
- Each member may send 32 KB/s and 480 messages a second, each burst twice that, and a message sent to the room counts
  once for each member it reaches. A frame larger than 1168 bytes is not sent; a payload over 1160 bytes is refused.
- Every message the host forwards carries the member id it came from, stamped by the host, so a member cannot speak as
  another. The link itself is plain TCP, as the direct link this replaced was: what needs trust is signed, not encrypted —
  each player's Ed25519 identity key, the room's rules signed by its maker's key, each removal signed the same way. That
  is what makes a room safe to hold on a machine someone else can reach ("What joining a room means for your save",
  "The room's rules").
- On Windows the room takes its port with `SO_EXCLUSIVEADDRUSE` before binding, so another program of the same user cannot
  bind a specific address beside the wildcard socket and answer for the host; the Linux paths set neither `SO_REUSEADDR`
  nor `SO_REUSEPORT`, where an overlapping bind is refused already. See [Microsoft's socket-binding
  rules](https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse).
- `HOS_COOP_DEMO=1` shows the tab on a made-up room, for screenshots: no session starts and nothing is sent.
## Switching it on: the game's settings

Co-op is off unless switched on. Off, the game opens no socket and starts no thread (`coop_init` in
`host/hos_main.c` returns at once). The settings are read once, when the game starts: `hookshot_options.ini` beside the
game, and the environment over it (a variable that is set wins over the file's line). The Insert menu's Co-op tab
(below) changes them while the game runs and writes them back to that file.

| `hookshot_options.ini` | Environment | What it does | Default |
|---|---|---|---|
| `coop=1` | `HOS_COOP=1` | co-op on | off |
| `coop_name=` | `HOS_COOP_NAME` | the player's name in the room: at most 24 bytes of UTF-8 (a control or text-direction character, or a byte that is not UTF-8, shows as `?`) | `Player-` and four random hex digits, new at each start |
| `coop_server=` | `HOS_COOP_SERVER` | the host's address to join: `host`, `host:port`, `[IPv6]:port` or a bare IPv6 address | none (the others ask the host) |
| `coop_port=` | `HOS_COOP_PORT` | the port this game hosts on, or joins at. A port typed into the address (`host:port`) wins over it | 43400 |
| | `HOS_COOP_AUTO=host` or `join` | Test runs: hold a room, or reach `coop_server`, from the start, and stay in it | off |
| | `HOS_COOP_DROP=<percent>` | a test's loss: that share of the bytes read from the link is thrown away | 0 |
| | `HOS_COOP_TRACE=1` | a log line for every frame sent and received, with the room's id and the host's address | off |

None of these variables is taken from `hookshot_env.txt`: the game leaves out every line of it whose name begins
`HOS_COOP`. That file is passed around between players and comes with packs; one that could set them would switch
co-op on, name its author's host and put whoever uses it into its author's room. Co-op is switched on in
`hookshot_options.ini` and on the menu's Co-op tab; the variables are for the player's own environment and for
tests (the file `HOS_ENV_FILE` names, a test run's own, sets them like anything else).

When the player leaves the room, switches co-op off or closes the game's window, the game says BYE to the others and
closes its link (twice each, in under half a second), so the others see the player go at once. One thread does all of it,
as the Discord status's does; it never touches the game's memory. The thread exists only while co-op is switched on:
switching co-op off, or closing the game, asks it to end and waits for it (at most 0.4 s), with the socket closed;
switching co-op on starts it again. A Port change while in a room closes the room and reopens it as it was asked for
(hosting: on the new port; joined: at the same address on the new port).
### A game that ends without a goodbye

The goodbye is said on the game's own ways out only (`hos_window_closed_exit` in `host/hos_relaunch.c`, and the control
bridge's `quit` in `host/control.c`). Co-op installs no signal handler. With a window, SIGTERM and SIGINT close the window
(SDL turns them into its quit event), so the goodbye is said. A game that is killed (SIGKILL; SIGTERM or SIGINT for a game
without a window, as `HOS_HEADLESS` test runs are), crashes, or loses its network says nothing, and its host goes by
silence:

- **0 to 5 s:** nothing changes for the others. The player is still in the roster, and what is sent to it waits on its
  link (a member whose link cannot take more has the new message dropped, counted as `outbox full` in the stats line).
- **after 5 s** without a frame from the member, the roster marks it away: the others' logs say `<name> (member <n>) is
  away`, and the Co-op tab shows it away. A game whose network only stalled sends again, and is `is back`.
- **after 8 s** the host drops the member: the logs say `<name> (member <n>) left the room: its link closed` (or `: its
  game left the room`, after a goodbye), which is what the others see. Nothing is held back for the member whose id it
  had: no one else is given that id while the room lasts, so a body, a hit and an animation that name member 4 mean the
  same player in every game. The same person, playing again, joins as a new member with a new id.

So a killed game shows as present for 5 s and as away for 3 s more. Its progress up to its last message is with the others
already; nothing is taken from anyone when it goes. The other way round — a game joined, whose host goes silent — is the
same 8 s, and that game reaches its host again by itself (`nothing from the host for 8 s`, then waits of 1, 2, 4, 8 and
15 s) until it hosts nothing any more (`stopped trying to reach the room`) or the player leaves.
### The Co-op tab

The Insert menu's last tab does the same from the game, with no setting written by hand. **Host** and **Join** make this
game the room or reach one at an address; outside a room Join is what a player uses, in a room the same two rows read
**Stop hosting** and **Leave the room** (a second press). **Host address** is `coop_server` (up to 63 characters: letters,
digits and `.` `-` `:` `[` `]` `_`; `host:port` sets the port too), **Port** is `coop_port`, **Player name** is
`coop_name`. **Status** is the room's own words — what is going on, or what went wrong, with the round trip to the host.

In a room the tab shows a line per player: its number, its name, `you`, `the room` for the host, `away`, and the
fingerprint of the key that player's game signed (gold once you accepted it, "not identified yet" while the room waits
for it). **Accept the room** shows while the room waits for your acceptance (see "What joining a room means for your
save"); **Go to player** travels to one of the others. The game that holds the room gets **Remove a player** and **Reset
the pools**. **Room rules** — for the host, and for a game not in a room — opens the page of the rules a room this game
makes (`coop_progress` .. `coop_ammo`, see "What is shared"). Accept, the remedies and leaving take a second press.
`HOS_COOP_DEMO=1` runs the tab on a made-up room, for screenshots: no session starts and nothing is sent.
## The log

Every line of the session starts with `coop: `. A room is named by its id — 16 hex characters, `room 3f1c…` — which is
not a secret and nothing to type: it says which room a signature was made for. Hosting logs this PC's candidate addresses
and the port (`a joiner can type 192.168.1.20 (this PC's address)`) so the host can give one to friends; joining logs the
address it reached.

| Line | When |
|---|---|
| `on, as <name>, on port <port>; <the host's address, or no host given>` | at start, co-op on |
| `HOS_COOP_AUTO=host at start`, `HOS_COOP_AUTO=join at start`, `HOS_COOP_AUTO=join needs a host's address (HOS_COOP_SERVER or coop_server=)`, `HOS_COOP_AUTO must be off, host or join` | a test run's room at start |
| `hosting the room on port <port> (room <id>)`, `a joiner can type <address> (this PC's address)` | this game holds the room |
| `joining the room as <name>`, `joining the room at <address>`, `joined the room at <address> as member <n>` | reaching a room, and in it |
| `<name> (member <n>) joined the room`, `<name> (member <n>) joined the room again`, `<name> (member <n>) left the room`, `... left the room: <why>`, `<name> (member <n>) is away`, `... is back`, `member <n> is now <name>` | the others, from the host's roster |
| `stats: sent <n>, accepted <n>, thrown away <n> (sender <n>, not co-op <n>, replay <n>), dropped by the test <n>, outbox full <n>, round trip <n> ms` | every minute in a room while frames flow (the keep-alives count) |
| `nothing from the host for <n> s`, `the link to the host closed`, `stopped trying to reach the room`, `this join was taken over: joining the room again` | the link to the host |
| `left the room (<why>)` | leaving: `leave requested`, `the game is closing`, `co-op turned off`, `the host changed`, `the host removed this player`, ... |
| `no room: <why>` | an attempt that never got in: `no host's address given`, `the room is full`, `update required: that game speaks another co-op protocol`, ... |
| `removing <name> (member <n>) from the room`, `only the game that hosts can remove a player, and only one in the room` | the host's remedy |
| `a join was turned away: the room is full (<n> players)`, `a join that is not a co-op HELLO was turned away`, `a join that never said who it is was turned away`, `Join needs a host's address: host, host:port or [v6]:port`, `the coop_server setting is not host or host:port` | a join refused, or an address that is not one |
| `the system's random source failed; co-op stays off`, `cannot start the co-op thread`, `the port (HOS_COOP_PORT or coop_port=) is not 1 to 65535: <n> is used`, `out of memory`, `HOS_COOP_DROP: <n>% of the received frames are thrown away (a test)` | what went wrong, or a test's switch at work |
| with `HOS_COOP_TRACE`: `the room's id is <id>`, `the host is at <address>`, `tx <TYPE> to <member>, <n> bytes`, `rx <TYPE> from <member>, <n> bytes`, `rx message kind <k> from member <n>, <n> bytes`, `a join from <address> is waiting for its HELLO` | every frame, every message |

`HOS_COOP_TRACE` also logs the host's address for a joined game; without it, no address is logged but the one this game
reached or can be reached at.
## Testing it

`tests/test_coop_session.py` (in `tests/run_all.py`) compiles `host/coop.c` and `host/coop_net.c` with
`tests/coop_session_test.c` (AddressSanitizer and UBSan on Linux) and runs, at once: TCP links on loopback (IPv4 and
IPv6, an over-long frame refused), `coop_server_parse`'s cases, and co-op off (no thread, no socket); co-op switched off,
on and off again as the Co-op tab does, each off leaving no thread and no socket behind (counted in `/proc/self/task` and
`/proc/self/fd`), these two also under ThreadSanitizer. Before taking the baseline, the fixture waits for each joined
helper's specific Linux thread ID to leave `/proc/self/task`: a userspace join alone can precede that removal. The wait is
bounded and the exact thread and socket-count checks remain. It also runs two sessions in one process where the host's
side of the link is driven by a hostile fixture (forged and replayed frames, garbage, forged rosters, lies to a player who
connects), with no loss and with 30 % of the bytes read thrown away; and a child process ended by SIGTERM, which says
nothing: the others see it away, then gone when the host drops it 8 s later.

Two games on one PC: start one and press **Host**, then the other with `Host address` set to `127.0.0.1` and **Join** —
or, for a scripted run, `HOS_COOP=1` on both, `HOS_COOP_AUTO=host` on the first and `HOS_COOP=1 HOS_COOP_SERVER=127.0.0.1
HOS_COOP_AUTO=join` on the second, each with its own `--user` folder. A headless Windows game has no window to receive a
polite close request; a scripted forced stop sends no goodbye, so its host drops it as silent after 8 s.

`tests/test_coop_env_file.py` checks that `hookshot_env.txt` sets no variable whose name begins `HOS_COOP` (every such
name the host's sources hold, and one made up), and that the environment and `HOS_ENV_FILE`'s file still do.
`tests/test_coop_options_text.py` checks that a `coop_server=` line of 159 bytes (the longest host address the game
keeps) is read whole from `hookshot_options.ini`, and that text inside such a line is not taken for a setting.

Wire byte counts in diagnostics are bounded by `COOP_MAX_FRAME` and printed as `unsigned`, so the same source and
fixtures pass Microsoft's printf-format checking without changing the format attribute or requiring GNU stdio.

Besides what they compile and run, the protocol, session, progress and menu tests check in the source where the pieces
are wired in (the build, the game's start and its ways out, the present, the save mount, the menu's interface and the
host's table). The menu tests use scripted input and a simulated session; they do not cover a real window's events,
keyboards or controllers. On Windows, the second game folder can hard-link the game's read-only data files instead of
copying them; give each game its own user folder.
## Reliable events

`coop_send_event(member, kind, bytes, n)` and `coop_on_event(kind, callback, user)` are the one session helper for
one-off events and large initial state. Ordinary `coop_send` messages are still replaceable: the newest of a kind wins.
The helper
wraps a layer's kind in EVENT (16), sends only to accepted current members, and dispatches only to that kind's event
handler. It never dispatches an event through `coop_on_message`, nor lets an ordinary message invoke an event handler.

- Each sender allocates a 64-bit event id for its session. It snapshots the recipients when queued (0 or
  `COOP_DST_ALL` means all accepted peers then). For each recipient it sends only the oldest event it still keeps.
  Thus the id in every fragment is also that recipient's delivery floor: a higher id retires any lower unfinished
  event, which the sender abandoned. The receiver retains this floor even when it discards partial bytes, so delayed
  pieces cannot revive an abandoned event. Events that are not abandoned remain FIFO per sender/recipient across
  kinds. No order across senders or with state messages. Cancellation cannot retract a callback already delivered.
- A recipient sends a small EVENT_ACK (17) with the event id, received-part mask and assembly epoch, at most
  once per 50 ms per sender while something changed or a duplicate arrived. Discarding a partial assembly advances
  that epoch. A newer epoch replaces the sender's old mask and makes it resend missing pieces; an older epoch is
  ignored. Masks from two assemblies never combine. Consent revocations are recorded even if acceptance is granted
  again before the next socket step. Completed receipts survive revocation, preventing a second callback.
  ACKs are their own messages rather than changing every layer's wire format. A lost ACK is answered again when
  the sender retries. The final ACK means the whole event reached its callback, not that a hit or authority claim
  was valid or applied. ACK body version2 is 27 bytes; version1 is refused, so all peers need the updated helper.
- Missing parts are retried after 250 ms; every retry goes out anew, with a new envelope counter and the same event
  id. Event data is paced to one unicast fragment per 32 ms for the session, with recipients taking turns; ACKs have
  their separate bound. An event stuck at one recipient does not hold up delivery to the others or ordinary messages.
- Eight outgoing events fit; `COOP_EVENT_FULL` (-1) means none was queued. `COOP_EVENT_QUEUED` (1), `UNAVAILABLE` (0,
  no accepted recipient) and `INVALID` (-2, bad kind/size/destination or exhausted ids) are distinct. Never treat a
  full queue as success. An event holds at most 16 KiB, split into 1,024-byte parts: large enough for an initial body
  description without an unbounded allocation. There is one reassembly buffer per sender, no allocation per part,
  and no partial callback. Cancelling or resetting an assembly creates no extra buffer or queue. Its floor and epoch
  are fixed-size counters; they do not grow with churn and epochs never wrap. Conflicting metadata, lengths or
  versions are refused.
- While the same accepted sessions remain, retries have no time limit: eventual delivery requires packets and ACKs
  eventually passing and a handler registered at the receiver. Dispatch is at least once under that condition,
  without duplicates in that receiving session. The receiver remembers the highest delivered event id, not a
  time-limited replay window. Leaving, a new local session (including reconnect), or a recipient's departure,
  changed session or revoked acceptance cancels outstanding delivery. There is no durable delivery across restarts.
  Parts also name the recipient's session, so a new occupant of its member id cannot inherit an old event.

Callbacks run on the co-op thread and borrow the bytes only until they return. They must validate and copy bounded
work for the game's own update, never write game state there. Check area/encounter generations, authority, the signed
present-and-on rule, plausibility and age; a delayed hit must not become valid just because transport delivered it.
Send an application result when the other side needs to know whether the effect was accepted. The room key is shared:
a malicious holder can forge an event or ACK in another member's name. This helper is not an individual signature,
a gameplay authority or protection against a trusted member lying.

`tests/test_coop_events.py` runs the real session sender, codecs and receiver over loopback TCP links with a
deterministic session clock: loss, reordering, duplicated envelopes, full queues, departure, consent and fragmentation.
Reset cases cancel partial sends and deliver later ids in order, revoke between halves without combining ACK masks,
replay an older assembly's ACK and revoke/reaccept between socket steps. The original loss and concurrent caller
cases remain; the concurrent caller also runs under ThreadSanitizer on Linux when the compiler provides it.

### Accepted members

Layers call `coop_member_accepted(id)`. A roster entry alone is not consent. The answer is false before this player
accepted that member's signed identity and name, when that identity is disputed, ignored or removed, after a new save
needs acceptance, and outside a joined session. A roster's changed session immediately loses the old binding. An
accepted key can prove its new session with its signed identity; merely claiming the old name or member number cannot.

The knowledge lives in `coop_sync_member_accepted(engine, id, session)` in the game-free progress engine.
`oot_coop.c` publishes those answers with the current roster sessions through `coop_accept_members`. Incoming progress,
removal, roster, acceptance and save changes refresh them; the session invokes no game code to answer a query or
receive an event. A layer must still check its signed feature rule and its own authority/context. This call is not
the save-backup gate; progress continues to use `coop_sync_open` before handing anything to the game.

## Adding a layer

Keep gameplay in its own file; the session knows bytes, members and consent, not actors. Allocate a kind in the shared
co-op registry before using it. This is the allocation table; a reservation is not an implemented feature in this base.

| Kind | Name | Owner / status |
|---|---|---|
| 1 | HELLO | Draft definition; current progress identities ride in DIGEST |
| 2 | POS | Small position/area codec; `coop-players` sends it |
| 3 | DIGEST | Progress identities, signed room rules and section sums |
| 4 | SECTION | Signed progress values |
| 5 | SYNC_NEED | Draft definition; not emitted by current progress |
| 6 | NOTICE | Draft definition; not emitted by current progress |
| 7 | BYE | Session goodbye |
| 8 | HIT | Reserved by `coop-hits` (committed-hit observations there) |
| 9 | COMBAT_CONTROL | Reserved by `coop-hits` |
| 10 | ATTACK | Reserved by `coop-hits` |
| 11 | WORLD | Reserved by `coop-world` (batched enemy state) |
| 12 | WORLD_EVENT | Reserved by `coop-world` (authority/lifecycle events) |
| 13 | AREA | Reserved by `coop-area` (optional room clock) |
| 14 | WARP | Reserved by `coop-warp`; that branch currently uses POS |
| 15 | CHAT | Reserved for chat; no branch implements it |
| 16 | EVENT | Session helper, not an application payload |
| 17 | EVENT_ACK | Session helper, not an application payload |
| 0xF0 | TEST | Fixtures only; 0xF0–0xFF are not production allocations |

Use `coop_send(kind, bytes, n)` for a replaceable state broadcast, `coop_send_to(member, kind, bytes, n)` for one peer,
and `coop_on_message(kind, fn, user)` to receive it. Both sends return 1 when copied to the bounded outbox, not when
delivered. `coop_on_tick(fn, user)` supplies the monotonic millisecond clock for pacing; callbacks share the co-op
thread, so keep them short and never call `coop_command`, `coop_configure` or `coop_shutdown` from them. Register
callbacks before messages can arrive. `coop_on_view` reports roster changes; a sender/session pair, not a member
number alone, identifies pending work. Use `coop_send_event` / `coop_on_event` for one-offs or initial state above
1,146 bytes, with the bounds and cancellation rules [above](#reliable-events). Check `coop_member_accepted` before
counting state or events. The session helper already checks it for events, but game-thread work must check again
when it runs.

Design to 1,146 payload bytes per message (a frame is 1,168 bytes on the link at most), about 30 state messages per
second per player, and one batch for **all** enemies that game leads at 10–15 Hz. Keep POS small; a richer body
description uses another registered kind, not a larger POS. Pace progress, events and state within the room's ceilings
(the 32 KB/s and 480 messages a second per member), not as separate bandwidth allowances. State should heal itself: for damage, keep a running total per player and enemy, merge
each player's total by taking the highest value seen, then add the players' totals together (one shared total merged
by "highest wins" loses a hit when two players strike at once: totals of 2 and 2 would stay 2, not 4; and a
subtraction sent once can be lost). Don't turn every animation frame into a reliable event.

Before received bytes count: check accepted current member/session, exact version and length, finite numbers and
bounded text/counts; signed rule **present and on** (absent is off); matching area/encounter and object generation;
the sender's authority and plausible values/timing; and a state counter that rejects stale/repeated updates. The
session has checked roster, destination, session and the envelope, not those gameplay facts. Only the game
thread applies bounded queued work, rechecking consent and context. Durable save changes keep their signatures and
backup/acceptance gate; the room key alone is not an individual identity proof.

Ten-line sketch: `MY_STATE` / `MY_EVENT` are the layer's registered kinds; its `*_valid` functions do the checks above,
its queues copy the bytes for the game thread, and `state_due` enforces its rate. The action's caller handles every
event return code rather than silently losing a full-queue event.

```c
static void state(int from, const uint8_t *b, size_t n, void *u) {
    (void)u; if (coop_member_accepted(from) && state_valid(from, b, n)) queue_state(from, b, n);
}
static void event(int from, uint64_t session, uint64_t id, const uint8_t *b, size_t n, void *u) {
    (void)u; if (coop_member_accepted(from) && event_valid(from, session, b, n)) queue_event(from, session, id, b, n);
}
static void tick(uint64_t now, void *u) { (void)u; if (state_due(now)) coop_send(MY_STATE, state_bytes, state_size); }
void layer_start(void) { coop_on_message(MY_STATE, state, NULL); coop_on_event(MY_EVENT, event, NULL);
    coop_on_tick(tick, NULL); }
int layer_action(int peer) { return coop_send_event(peer, MY_EVENT, event_bytes, event_size); }
```

## The link and the wire

For a reviewer: what is derived from what, what each frame carries and what a receiver checks. The code is
`host/coop_proto.c` (pure functions on bytes) with `host/coop.c` (who sends what when); `tests/coop_vectors.txt` holds
byte vectors of every frame and message, which the C checks are run against (`tests/test_coop_proto.py`).

**Library.** Monocypher 4.0.2 (`third_party/monocypher/`, two files, unmodified, BSD-2-Clause, dual-licensed with CC0)
for BLAKE2b and EdDSA. It is small, has no dependencies, builds the same on Windows and Linux, and its primitives are
standard (RFC 7693, EdDSA over Curve25519). It is used in `coop_proto.c` and in the rules engine (`coop_state.c`,
`coop_oot.c`) behind `coop_sign_keypair`, `coop_sign`, `coop_sign_check` and `coop_blake2b`, so another library can
replace it there. Nothing on the link is encrypted, so no stream cipher is needed: what must be trusted is signed.

**Frames.** An 8-byte header, little-endian — `u8 type, u8 src, u8 dst, u8 kind, u32 payload length` — then the payload;
at most `COOP_MAX_PAYLOAD` = 1160 bytes of it, `COOP_MAX_FRAME` = 1168 in all. Member ids are 1..8 and `src` 0 is the
host; `dst` is a member or `COOP_DST_ALL` (0xFF, every other member). The major version is `COOP_PROTO` = 3 and travels
in HELLO and WELCOME: a host and a joiner talk only when it is equal, and ERROR (`COOP_E_PROTO`) says it was not.

| Frame | Between | Carries |
|---|---|---|
| HELLO (1) | joiner → host | its protocol major, its session (random for this join), its name |
| WELCOME (2) | host → joiner | the member id given, the protocol major, the room's 16-byte id, the roster |
| ROSTER (3) | host → all | each member's id, flags (away `COOP_FLAG_AWAY`, maker `COOP_FLAG_MAKER`), session and name |
| DATA (4) | either way, through the host | one player message (below) |
| PING (5), PONG (6) | both ways | nothing: the keep-alive and the round trip |
| BYE (7) | either way | a member is going |
| KICK (8) | host → the member removed | its id and session |
| ERROR (9) | host → a joiner | why a join was refused: room full, protocol, room closed, removed |

The host stamps `src` on every DATA frame it forwards: a member cannot claim to be another one. A joiner is accepted by
the host's listen, then HELLO within 3 s; `COOP_MAX_WAIT` = 4 joins may wait at once. A member not heard from for 8 s is
dropped, and its id is not given to anyone else while the room lasts.

**The room's id.** 16 random bytes the host draws when it starts listening, sent in WELCOME, shown in the log as 16 hex
characters. It is not a secret and nothing to type: it is what a signature says the rules were made for, so a room's
signed rules, its removals and its pools cannot be carried into another room. `coop_room_id_hex` is the only form it
takes in the log or in a message.

**The player message envelope** (the payload of DATA):

```
u8 envelope version (1) | u64 sender's session | u32 counter | u8 kind | message
```

A session is drawn for every join and the counter starts at 1. Random sessions can collide with probability about
n²/2^65. The kind byte and the message are outside any cipher — they are what the host forwards — so a new kind of
message needs only a number, and a later envelope version can change the outer fields without touching the messages. A
message is at most `COOP_MAX_MESSAGE` = 1146 bytes after its kind.

**What a receiver checks, in order:** the sender is in the roster and is not itself, the destination is it or everyone,
the envelope's session is the one the roster lists for that sender, and the counter is new within a window of the last 64
of that session; only then is the counter recorded and the message delivered. A counter older than the window is refused,
unless it is the fourth of that sender's messages in a row below the window, each later than the one before: then the
window starts again from it and it is delivered.

**Replay.** A message recorded before a player joined again carries the old session, which the roster no longer lists:
refused. Within a session the window refuses what it still remembers, and four older ones in a row start it again (that
is how it recovers from a counter far ahead); from then on the window has forgotten what it took, so anyone who recorded a
member's messages of its present session can have them delivered again, in order. So the envelope does not make a message
arrive once: a layer for which a repeat matters checks a counter of its own, and signs what only one player may make
(`coop_state.c` does this for progress, and `coop_oot.c` for what a player's game gained).

**POS** (kind 2), where a player is and what Link plays, for showing the other players in the world: the area's name,
child or adult, position, facing, and the node and frame of Link's first four animation slots, with a counter one higher
for each POS sent (`coop_proto.h`). A POS with a name that is not printable ASCII or a number that is not finite is
refused. A receiver takes a pose only above the last one it took from the same member in the same session, and, once none
was taken for 3 s, the next whatever its counter (`coop_pos_fresh`).

**LINE** (kind 32) is `host/oot_coop.c`'s layer: one short line of text — a place, a hit, an enemy's life, an animation,
what was found — as the room's message. The room gives it the sender's member id; the layer's own names, hits and hearts
are in `host/oot_coop.c`'s header comment. **DIGEST**, **SECTION** and **SYNC_NEED** (3, 4, 5) are progress sync's
(`host/coop_oot.c`), **NOTICE** (6) a line for the log, **EVENT** and **EVENT_ACK** (16, 17) the session's reliable
delivery ("Reliable events"), and a kind of `0xF0` and above is free for tests, not for a layer.
## What joining a room means for your save

Read this before you join a room.

- **Nothing from a room reaches your game until you accept it.** When you join, the Co-op tab names the other players
  (each by the name and the key fingerprint their game signed), the room's rules and the key that signs them, and asks
  you to accept. Until then your game takes nothing from the room. You accept players by their keys, not their names:
  a new player, a player whose game restarted, a second player with a name you know, other rules, rules signed by
  another key, and another save loaded in the room are each a new offer. A player in the room whose game never says who
  it is keeps the room from being accepted at all. Accepting every room without asking (`HOS_COOP_ACCEPT=always`, for
  tests) works only from the environment, never from `hookshot_env.txt`.
- **Your game takes values only from players you accepted.** Every message that can change your save is signed by the
  key of the player it came from, and the host stamps which member sent it, so neither another player nor the host can
  make one in someone else's name that changes your save. For story flags and items acceptance is passed along (a player you accepted
  hands on what they took from the players they accepted); pools count only what you and the players you accepted
  brought, picked up and spent.
- **Your save is copied first.** The moment you accept a room, the game copies your whole save folder (every slot) to
  `coop-backup/` in the user folder (beside `save/`), and applies nothing from the room until the copy is made. The
  copy waits while the game is writing its save (a file open for writing, or a change in the last half second), and a
  save the game starts meanwhile waits for the copy, so a copy never holds half a save. The first copy for a room in
  each start of the game is `first-<date-time>` and is kept until you delete it; later copies (another save loaded in
  the room, say) are `<date-time>`, of which the newest eight are kept. Times are UTC.
- **What the room can change in your save** is what its rules share. Values are applied through the game's own set
  routines, as if you had done it, and only raised: a flag can only be set to "done", an item count only raised and
  only up to 999, small keys only up to 15, a pooled counter only within its capacity; every value is checked against
  your game's own table of flags first. Anything else is dropped and counted in the log, and a player who sends a few
  dozen of those is ignored while they stay in the room. Before anything is shared, the game checks that its flags read
  as this build expects (against the game's own getter); a game whose flags read otherwise shares nothing and says so.
- **What a player you let in can still do.** With progress shared, what they do in their world happens in yours, and a
  player who wants to can send such flags without playing for them; with small keys in a pool they can spend the keys
  you share (at most what entered the pool). Flags stay set when a player is removed. Inside the game none of this can
  be undone: go back to the copy of your save. Invite only people you trust with your save.

### Going back to your save from before a session

1. Close the game.
2. In the user folder (`user` beside the game on Windows, `~/.local/share/hookshot` on Linux, or the one `--user`
   names), open `coop-backup` and pick the copy: `first-<date-time>` is the save as it was before you first accepted
   that room, `<date-time>` a later acceptance.
3. Move the folder `save/<numbers>` aside (keep it until you are sure), and copy the contents of the backup folder into
   a new `save/<numbers>` of the same name. `python -I scripts/oot_saves.py import --from <backup folder>` does this
   step and keeps a backup of its own first.
4. Start the game: every slot is as it was when you accepted the room. Progress made after that is in the folder you
   moved aside.

## Progress sync

When one player gains progress, the others' games gain it too while they play; a player who joins later catches up with
everything the players in the room at that moment have, and they with what the joiner brings. Each player's own save
keeps what it got: the game saves it as its own progress.

**The host keeps no progress.** A room is only its players: a player who joins while the others are away gets their
progress when one of them is in the room again (from that player's save, so only what their game had saved), not
before. That player must bring the room's rules: the maker in the run of the game that made the room, or a player who
accepted those rules. A maker whose game started again holds the room's key but brings no rules until such a player is
there, so with only those two nothing is offered and nothing is shared. A room nobody is in closes (10 minutes after
the last player left, a minute if only its maker was ever in it). Play together, or at least overlap: what a player
did alone reaches the others the next time both are in the room.

The game keeps its progress as flags (a database keyed by 32-bit hashes of flag names). At the end of each frame
(`host/coop_oot.c`, called last in the present, on the game's own thread) the runtime reads a quarter of the shared
flags, and sets the ones the room has and this game lacks through the game's own set routines, which queue the change
for the game to take as if the player had done it. Nothing is read or set on the title screen, while a save loads or a
scene changes, while the game's play clock stands (the pause menu), or before Link stands in the world: what arrives
meanwhile is set afterwards. A chest another player opened shows open at once (seen for one chest; doors not tried);
an item shows in the menu the next time it opens. With co-op off the per-frame call reads one value and returns.

What one room merged belongs to that room and that save: leaving the room, joining another or loading another save
forgets it. A save loaded while in a room is a new offer, and the save is copied again first. Games merge only with
the same schema (this layer's version, the game's build and the whole table of what is shared and up to which value).
An area change within the same save (including an ordinary door or loading zone) keeps the accepted room and its
existing backup. The gate uses total play time, not the counter that restarts in each area.

### What is shared

Which flags are which is worked out from the player's own game at run time: from each flag's reset kinds and save file
(a flag that goes to the save and is reset only by a new game or a load is progress), and the game's own groups of
flags (the item counts, the "got it once" markers, each dungeon's small keys, the capacities). A few dozen flag hashes
name what the groups cannot tell (the songs, the quest items, the counters that stay with each player). No list of the
game's flags is in the source.

| Category | Rule key | Modes (default first) | What |
|---|---|---|---|
| progress | `coop_progress` | `shared`, `own` | story, world and dungeon flags: events, talks, chests, doors, switches, bosses, map and compass |
| items | `coop_items` | `shared`, `own` | equipment, key items, wallet, bag and quiver sizes, the magic meter's size |
| songs | `coop_songs` | `shared`, `own` | |
| quest | `coop_quest` | `shared`, `own` | medallions, spiritual stones, the stone of agony, the gerudo card |
| keys | `coop_keys` | `pool`, `own` | small keys, per dungeon |
| rupees | `coop_rupees` | `own`, `pool` | |
| ammo | `coop_ammo` | `own`, `pool` | bombs, bombchus, arrows, deku nuts, seeds and sticks, magic beans |

- `shared`: a flag set by anyone stays set for everyone; of a count the highest wins.
- `own`: not synced; switching to it stops the merge and keeps what was merged.
- `pool`: one count that every player adds to and spends from, each player's gains and spends counted apart and signed
  by their game, so two players spending at once both pay; nobody's spends take more than entered the pool.
- Items, songs and quest items can be `own` only with progress `own` (in a shared world a chest one player opened is
  empty for the others); with progress `own`, keys are `own` too. A maker whose settings break this gets the nearest
  valid set, and the log says which.

Never shared: where Link is, his health and the magic in the meter, the time of day, bottle contents and the equipment
worn; heart containers and pieces and gold skulltula tokens (their pickups are flags of single objects this layer does
not write, so a shared count would pay twice); items the game takes away again (the trade sequence, rented masks, the
knife that breaks, amiibo items); flags the game resets itself at a scene change, a new day or a load.

An item another player got arrives in your inventory as if you had picked it up, but without the game's message and
pose for it (your game only sets the flags; no event runs); which sword, shield, tunic or boots you hold or wear stays
your choice in the pause menu.

### The room's rules

The rules belong to the room: every game in it merges the same way. They are the settings of the player who made the
room (`coop_progress` ... `coop_ammo` in `hookshot_options.ini`, or `HOS_COOP_PROGRESS` ... `HOS_COOP_AMMO` =
`shared`, `own` or `pool` over them), signed with that player's key: a game takes rules only signed by the key its
player accepted. The key the maker's game signs with is kept in the user folder (`coop-keys/`, readable by your account
only), so a maker whose game restarted is still that room's maker; that file alone is what lets whoever has it act as
the room's maker: never hand out a copy of your user folder. The maker can change a rule
during the session (each player accepts the new offer), remove a player by their key's fingerprint (every game drops
that key and what it took from the pools) and reset the pools.

The menu and control bridge use these named `coop_*` settings; the runtime reads the same keys from the options file.
There is no separate numeric host-option interface for room rules, so adding unrelated menu options does not renumber
them or require migrating a player's saved settings.

### Progress sync's switches and log lines

| Switch | What it does |
|---|---|
| `HOS_COOP_TRACE=1` | Also a line per value sent, received and applied, and what the game shows for each flag that changed (with its getter's reading) |
| `HOS_COOP_ACCEPT=always` | Every room's offer accepted without asking (tests); the environment only |
| `HOS_COOP_PROBE=1` | Once a save is played, the flag table as the runtime reads it, checked against the game's getter; nothing written. Co-op need not be on |
| `HOS_COOP_TEST_SET=<hash>=<value>@<frame>,...` | That flag (hex hash) set through the game's own routine at that frame of play (tests); the environment only |

Progress sync runs whenever co-op does: from the game's start with `coop=1` in `hookshot_options.ini` or `HOS_COOP=1`,
or from the moment co-op is switched on while the game runs. Switched on after a return to the title screen
(the pause menu's Title Screen), it shares nothing until a save is loaded: the log says `co-op was switched on after
the title screen came back: progress is shared once a save is loaded`.

`coop: sync: playing a save: N progress flags, ...` when a save is bound; `the flag table reads as the game's getter
does (N flags checked)` before anything is shared; `room accepted (...)`, `the save as it was before this room is in
<folder>`, `took N values from the room (...)` every 5 s while values arrive; `N values from the room were refused`.
