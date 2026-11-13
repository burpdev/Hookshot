# Co-op

Each player runs their own game and their own save; co-op connects the games of up to 8 players with one TCP link each:
one player's game is the room and the others connect to it. The game has no multiplayer of its own and its `nn::socket`
stays offline: co-op is the runtime's own, a thread of its own that owns one link.

What exists so far: the wire format both ends speak (`host/coop_proto.c`: rooms named by a secret, the frames between a
game and the room it is in, the sealed messages between the players), the game's session (`host/coop.c`,
`host/coop_net.c`): the game hosts a room or joins one from the Insert menu's Co-op tab, or at start from its settings,
and stays in it; and progress sync (`host/coop_oot.c`, [below](#progress-sync)): what one player's game gains, the
others' games get.

## Hosting a room

The room is a game, not a program someone has to start. Press **Host a room** on the Co-op tab: this game listens on a
TCP port, is member 1 and the room's maker, and every other player connects to it. Nothing else runs, and nobody
installs anything on a third machine.

| | |
|---|---|
| **Host a room** | the Co-op tab's button: this game is the room, on the port its **Port** row names |
| `coop_port=43400` | the port it waits on: 1 to 65535, 43400 when nothing is given (`hookshot_options.ini`, the tab's Port row, or `HOS_COOP_PORT`) |
| `coop_server=` | what a joiner goes to: `host`, `host:port`, `[IPv6]:port` or a bare IPv6 address (`hookshot_options.ini`, the tab's **Join address** row, or `HOS_COOP_SERVER`) |

The host's log says the room and where it can be reached:

```
coop: hosting room H7K… on port 43400
coop: players on this network can join 192.168.1.12:43400
```

up to four of those address lines, from the networks this PC is on, with the port already in them. Give one of them,
and the room's secret, to whoever joins; an address that names no port means 43400.

- A room holds up to 8 players. The host is member 1 and the only one that says who is in it: the roster (who is in
  the room, each one's session, which one made it) goes out at a change, again 0.75 s later, and every 5 s whatever
  else changed.
- A link that ends is noticed at once, in the host's next step (it waits at most 8 ms while a room is open): whoever
  it carried leaves the room. A link that is open but silent marks its member away after 15 s, and back again if it
  speaks. The host keeps a member that left its rejoin token for a minute, so the same game joins again as the same
  member; a game started again is a new member.
- A link that has not become a member within 10 s is closed. The host serves at most four links that are still
  handshaking: a fifth knock is refused with `too many players are being let in at once`, and the knocking game tries
  again on its own schedule.
- A frame is at most 1200 bytes (`COOP_MAX_DGRAM`) and a player message at most 1146 bytes after its kind
  (`COOP_MAX_MESSAGE`); a session has 64 messages waiting to go at most (`OUT_SLOTS`) and a link holds two frames'
  worth of bytes the socket has not taken yet; an event is at most 16 KiB split into 1,024-byte parts. Nothing queues
  beyond those bounds.
- The host forwards what one member sends to the other members and can read none of it, sealed as it is with the
  room's message key; it never touches game memory from the link. The room's secret stays with the games that hold it.

A full room can sustain 30 broadcasts per second from each of eight players: 210 copies per sender, 1,680 for the room,
and the host pays all of them because everything goes through it. Even at the 1,200-byte frame ceiling that is 252,000
bytes/s per sender and 2,016,000 bytes/s out of the host. Design for one body update at 15 Hz and one batch of all
enemies a game leads at 10–15 Hz, not one packet per enemy. Smaller state messages leave room for events and progress;
1,200-byte broadcasts leave little room for another full-rate stream. Progress sections and pushes still pace
themselves at 32 KiB/s before the host's fan-out (8 KiB burst); the periodic and on-join digests are separate. This is
a ceiling, not bandwidth reserved in addition to gameplay, and it is arithmetic rather than a measurement from a game
run.

### Getting there from another network

On one home network an address from those lines works as it is. Between homes the joiner needs a route to the host's
address, and the game provides none of it: forward that **one TCP port** on the host's router and allow Hookshot
through the PC's firewall, or put the players on one VPN that gives them addresses (Radmin VPN does, and every session
played so far used it — the steps are in [coop-network.md](coop-network.md)), or host on a machine that is already
reachable. A router behind carrier-grade NAT may not accept a port forward. The game does not discover a public address
or change a router or firewall. Windows may ask whether to allow Hookshot on a network: allow the network you intend to
host on, not every network indiscriminately. That prompt and Windows networking have not been tested here.

The listening socket is IPv6 with IPv4 as `::ffff:a.b.c.d`, so one port serves both.

### The room lives in the host's game

The room exists as long as the host's game does. When the host leaves co-op, turns it off or closes the game, the room
is over: nothing is saved, no roster, no member ids, no progress. The others can keep playing their own saves, but
nothing passes between them until someone hosts again.

A game that left the room keeps its rejoin token for a minute. Host the same secret again within that minute and that
game comes back as the same member; after the minute, or when the game was started again, it is a new member with a
new member id.

A host that was only restarted keeps the room's maker key, so it can host the same room again, but it does not carry
the room's rules — progress rules live with the player who holds them, see [progress sync](#progress-sync).

`HOS_COOP_ROOM=<secret>` makes **Host a room** use a secret you know beforehand, which is how the second game in the
tests joins a room it was told about. Without it, every room you host has a fresh random secret.

## Rooms and secrets

A room is its secret: 18 characters of `23456789BCDFGHJKMNPRSTVWXYZ` (no vowels, no 0/1/O/I/L, no Q or E), typed in
any case, with or without `-` and spaces, shown `XXXXXX-XXXXXX-XXXXXX`. The game makes a new random one for each room it
hosts, and the maker passes it on to the others (by voice, a chat message). A join is accepted only by a game that is
hosting that exact secret (`the host does not know this room key` for any other), so nobody ends up in a room they did
not mean to join by pointing at an address at random. The secret is never logged in full and is never on the wire as
plain text.

## Switching it on: the game's settings

Co-op is off unless switched on. Off, the game opens no socket and starts no thread (`coop_init` in
`host/hos_main.c` returns at once). The settings are read once, when the game starts: `hookshot_options.ini` beside the
game, and the environment over it (a variable that is set wins over the file's line). The Insert menu's Co-op tab
(below) changes them while the game runs.

| `hookshot_options.ini` | Environment | What it does | Default |
|---|---|---|---|
| `coop=1` | `HOS_COOP=1` | co-op on | off |
| `coop_name=` | `HOS_COOP_NAME` | the player's name in the room: at most 24 bytes of UTF-8 (a control or text-direction character, or a byte that is not UTF-8, shows as `?`) | `Player-` and four random hex digits, new at each start |
| `coop_server=` | `HOS_COOP_SERVER` | the host this game joins: `host`, `host:port`, `[IPv6]:port` or a bare IPv6 address | none (this game hosts); an address with no port means 43400 |
| `coop_port=` | `HOS_COOP_PORT` | the TCP port this game hosts its room on (the one to forward on a router) | 43400; 1 to 65535, and 43400 is used when the number is outside that |
| | `HOS_COOP_AUTO=host\|join` with `HOS_COOP_ROOM=<secret>` | host or join that room at start, and stay: a host that was restarted is hosted again by its maker and waited for by the others. `host` without a room makes a new one. For tests; with `HOS_COOP_ROOM` alone, the tab's **Host a room** hosts the room of that secret | off |
| | `HOS_COOP_DROP=<percent>` | a test's loss: that share of the frames a game reads is thrown away before it looks at one. The frame still counts as having arrived, so the loss moves the room's messages, not the timers that decide whether the host is answering | 0 |
| | `HOS_COOP_TRACE=1` | a log line for every frame sent and received, and the host's address | off |

None of these variables is taken from `hookshot_env.txt`: the game leaves out every line of it whose name begins
`HOS_COOP`. That file is passed around between players and comes with packs; one that could set them would switch
co-op on, name its author's host and put whoever uses it into its author's room. Co-op is switched on in
`hookshot_options.ini` and on the menu's Co-op tab; the variables are for the player's own environment and for
tests (the file `HOS_ENV_FILE` names, a test run's own, sets them like anything else).

In a room a player that is not the host sends the host a PING every 2 s (the keep-alive that also holds a home router's
mapping open; the PONG gives the round trip, which the Co-op tab and the stats line show), and takes 8 s without any
frame from the host, or a host link that has closed, as the host gone: it joins the same room again as the same member
(`the host is gone; rejoining`). Reaching the host is a TCP connect that waits at most 3 s, tried four times 250 ms
apart, then a wait of 1, 2, 4, 8 and 15 s between bursts of four. When the player leaves the room, switches co-op off
or closes the game's window, the game says BYE to the others and LEAVE to the host — twice each, in the same step — and
then closes the link, so the others see the player go at once. One host thread does all of it, as the Discord status's
does; it never touches the game's memory. The thread exists only while co-op is switched on: switching co-op off, or
closing the game, asks it to end and waits for it (at most 0.4 s), with the link closed; switching co-op on starts it
again.

### A game that ends without a goodbye

The goodbye is said on the game's own ways out only (`hos_window_closed_exit` in `host/hos_relaunch.c`, and the control
bridge's `quit` in `host/control.c`). Co-op installs no signal handler. With a window, SIGTERM and SIGINT close the
window (SDL turns them into its quit event), so the goodbye is said. A game that is killed (SIGKILL; SIGTERM or SIGINT
for a game without a window, as `HOS_HEADLESS` test runs are), crashes, or loses its network says nothing — and a TCP
link cannot stay half open the way a UDP address can:

- **The host sees it in its next step.** A game that ends closes its socket, the host's next step (which waits at most
  8 ms while a room is open) finds the link closed, ends it, and sends the roster again without that member: the
  others' logs say `<name> (member <n>) left the room`, the same line as after a goodbye. Nothing is waited for.
- **A link that is open but silent** — the player's PC suspended, its network stalled without dropping the connection
  — is not a departure: the host marks the member away after 15 s (`<name> (member <n>) is away`, and the Co-op tab
  shows the member as away), and back again (`is back`) when it speaks. The member keeps its id the whole time.
- **The other side of the same line.** A player whose link to the host closes, or that hears nothing from the host for
  8 s, says `the host is gone; rejoining` and joins the same room again as the same member; if the host is not there
  yet it keeps trying on the burst-and-backoff schedule above. The host keeps that member's rejoin token for a minute,
  so a game that is still running comes back as the same member (`joined the room` again with the same number); a game
  started again is a new member.

So a killed game is gone from the room in the host's next step, not after a timeout. Its progress up to its last
message is with the others already; nothing is taken from anyone when it goes.

### The Co-op tab

The Insert menu's last tab does the same from the game, with no setting written by hand. **Your name** and **Join
address** are `coop_name` and `coop_server`; the **Port** row is `coop_port`, the port this game hosts its room on and
the port the others put in their Join address. **Host a room** makes this game the room (its secret appears on the
**Room secret** row, masked until Enter shows it) and **Join a room** goes to the game the Join address names; both
switch co-op on for this run. The secret is typed only when it was given to you some other way — otherwise the host
says it on the link. In a room the tab shows its state and the round trip to the host, and a line per player with the
fingerprint of the key their game signed; **Accept the room** shows while the room waits for your acceptance (see
"What joining a room means for your save"), **Leave the room** always. The game that holds the maker's key also gets
**Remove a player** and **Reset the pools**, once it has accepted the room. **Room rules**, outside a room and for that
game, opens a page with the rules of a room this game makes (`coop_progress` .. `coop_ammo`, see "What is shared") and
**Co-op at start** (`coop`). Accept, the remedies and Leave take a second press. `HOS_COOP_DEMO=1` runs the tab on a
made-up room, for screenshots: no session starts and nothing is sent.

The same tab also carries **Play as**, **Host address**, **Player name**, **Connection**, **Status**, **PvP** and
**Go to player**: that is `host/oot_coop.c`, the older direct link this branch grew from, which runs on the same
**Port** row and is documented in [coop-network.md](coop-network.md). The two are separate links with separate rooms,
and both read the same `coop_port` key; making the body-state layer ride the session link instead is the next step.

## The log

Every line of the session starts with `coop: `. The room's secret never reaches the log: a room is named by its first
three characters and `…` (`room H7K…`). The host's address is logged when the attempt is made (`connecting to the host`
names it only with `HOS_COOP_TRACE` on; `=1`; `=0` is off, as for every on/off switch: `env_on`). Hosting deliberately
logs this PC's candidate LAN addresses and port so that the maker can give one to friends; it never logs the room
secret. Lines about what was thrown away are said at most every 30 s each, so a flood cannot fill the log.

| Line | When |
|---|---|
| `on, as <name>; joining <host> (HOS_COOP_SERVER)` (or `hookshot_options.ini`, or `the menu`), or `on, as <name>; hosting on port <port> until a host is given` | at start, co-op on |
| `HOS_COOP_AUTO=<host\|join>: room XXX… at start` (or `a new room at start`) | the room this game hosts or joins at start |
| `players on this network can join <address>:<port>` | when this game starts hosting: up to four of them, one per network it is on |
| `connecting to the host`, `the host answered in <n> ms` | each attempt; the first answer of one |
| `hosting room XXX… on port <port>`, `joined room XXX… as member <n> of <k>`, `rejoined room XXX… as member <n>` | this game is the room, or is in one |
| `<name> (member <n>) joined the room`, `... left the room`, `... is away`, `... is back`, `member <n> is now <name>` | from the roster, on every game in the room (the host hears its own too) |
| `stats: sent <n>, accepted <n>, thrown away <n> (tag, sender, not co-op, replay), dropped by the test <n>, outbox full <n>, round trip <n> ms` | every minute in a room while frames flow (the keep-alives count) |
| `the host is gone; rejoining`, `joining the room again as <name>`, `the host is in room XXX…` | the host's link ended or went silent; the name changed |
| `left room (<why>)` | leaving: `leave requested`, `co-op turned off`, `the host changed`, `the room's maker removed this player`, `encrypted message counter exhausted; join the room again`, ... |
| `no room: <why>` | an attempt refused: `that game is not hosting a room`, `the room is full`, `update required: that game speaks another co-op version`, `the host does not know this room key`, `that game cannot take another player`, `the host refused`, ... |
| `refused a player: <why>`, `refused a player from <address>: the room is full`, `a player that never finished joining was let go` | on the host, about a knock it turned away |
| `a player came back with a member id this room does not know: it is asked to prove the room's keys` | on the host, at most every 30 s: the room was started again, so the id it holds is not one of this room's, and the player is answered as a first-time knock instead of refused |
| `removing <name> (member <n>) from the room` | the room's maker removed a member |
| `a frame that is not a co-op frame was thrown away, and the link with it`, `member <n> sent a message as member <m>: thrown away`, `a message that does not open with the room's key was thrown away`, `a message of member <n> that is not of its present session, or seen before, was thrown away` | at most every 30 s each; the stats line has the counts |
| `cannot listen on port <port>: is another game hosting on it?`, `cannot resolve the host's name`, `no link to the host at <address>`, `no host address given (coop_server= in hookshot_options.ini or HOS_COOP_SERVER)`, `the coop_server setting is not host or host:port`, `cannot start the co-op thread`, `the system's random source failed; co-op stays off`, `HOS_COOP_AUTO must be off, host or join`, `HOS_COOP_ROOM is not a room secret (18 characters, XXXXXX-XXXXXX-XXXXXX)`, `the port (HOS_COOP_PORT or coop_port=) is not 1 to 65535: 43400 is used` | what went wrong |
| with `HOS_COOP_TRACE`: `the host is <server>`, `the host is at <address>`, `a player connected from <address>`, `tx <TYPE> to link <n>, <n> bytes`, `rx <TYPE> from link <n>, <n> bytes`, `rx message kind <k> from member <n>, <n> bytes` | every frame, on every link |

The two sides of one room are in the same kind of log: the host's has `a player connected from <address>` and the
refusals, the joiner's has `the host is at <address>` and what the host told it to go for.

## Testing it

`tests/test_coop_session.py` (in `tests/run_all.py`) compiles `host/coop.c` and `host/coop_net.c` with
`tests/coop_session_test.c` (with `host/coop_proto.c` and Monocypher) and runs seven builds at once, each its own
process. The unit run covers the sockets on loopback (IPv4 and IPv6, a frame too large dropped),
`coop_server_parse`'s cases, a stop's order, and co-op off at start (no thread, no socket) unless `HOS_COOP` or the
options file's `coop=1` switches it on. The menu run switches co-op off, on and off again as the Co-op tab does, each
off leaving no thread and no socket behind (counted in `/proc/self/task` and `/proc/self/fd`). Before taking the
baseline, the fixture waits for each joined helper's specific Linux thread ID to leave `/proc/self/task`: a userspace
join alone can precede that removal. The wait is bounded and the exact thread/socket-count checks remain. Those two
runs are repeated under ThreadSanitizer. The hostile run is a host that plays a player for a fool, on a port of its
own: it answers nothing, sends bytes that are not a frame, a frame of another version, every refusal it has, a message
sealed with another room's key or one it has already sent, or a roster that makes the player the maker. The mock runs
are a whole room inside one process, with nothing thrown away and then with 30 % of the frames each side receives
thrown away.

Sanitizers need the compiler to have their runtime libraries, which is not a given. This machine's gcc 16 has no
`libasan`, `libubsan` or `libtsan` installed: `--cc gcc` builds the five plain runs and prints `not with -fsanitize=…`
for the two ThreadSanitizer runs, so nothing is sanitized at all. `--cc clang` gets all seven. Pass `--cc clang` when
you want the sanitizers; a ThreadSanitizer report ends its run with exit code 66.

What the mock runs do is what a pair of games does: Alice hosts a room on a port of her own and the others reach her
directly. Bob joins with the address and the room secret; Dana joins with only one of the addresses Alice's log
printed. The room fills to eight players and the ninth is told `the room is full`. Alice stops and hosts the same room
again on the same port, and Bob notices the link closed and gets back in. A member whose link closed abruptly comes
back with the id it held. The maker removes a member, which ends that member's link and leaves the room's key good —
the same player is let in again; a removal asked for by someone who is not the maker removes nobody. The game's own
session is started from the settings and messages go both ways, and on Linux a child process in the room is ended by
SIGTERM, which says nothing: Alice sees it gone when its link closes, well before the silence time. Two games on one
PC: in one game's folder, `HOS_COOP=1 HOS_COOP_AUTO=host`; in a second game with its own `--user` folder,
`HOS_COOP=1 HOS_COOP_SERVER=127.0.0.1:<port> HOS_COOP_AUTO=join`. Give `HOS_COOP_ROOM=<secret>` only when the secret
reached you some other way; the second game cannot take the first one's port.

The host reserves its listening port on Windows with `SO_EXCLUSIVEADDRUSE` before it binds, in `coop_tcp_listen`
(`host/coop_net.c`). Without it, Windows permits another program of the same user to bind a specific IPv4 address
beside a wildcard socket, including a dual-stack socket: it could take part of the traffic or inject frames and
disrupt the room, though it cannot decrypt sealed player messages without the room secret. The listening socket is
IPv6 with `IPV6_V6ONLY` off, so an IPv4 player reaches it as well, as `::ffff:a.b.c.d`. On Linux it sets
`SO_REUSEADDR`, which is what lets a room be hosted again on the same port right after the game that held it closed;
a second socket is still refused a port something is listening on. The socket tests cover wildcard/specific IPv4
coexistence in both orders, and a competing socket requesting reuse, on each OS; the session modes probe the real
sockets in both orders, and the compiled Windows-call fixture checks option order and failure cleanup — the
exclusive-ownership request must come before the bind, and the socket must be closed if that request, the bind, the
listen or the move to nonblocking fails. Addresses are not encrypted. See [Microsoft's socket-binding
rules](https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse).
The Windows fixture drivers declare `_CRT_SECURE_NO_WARNINGS` before any header, including forced headers and each
separately compiled runtime source; their existing warning-as-error settings and assertions remain enabled.

On Windows, the second game folder can hard-link the game's read-only data files instead of copying them; give each game its own user folder.
A headless Windows game has no window to receive a polite close request; a scripted forced stop sends no goodbye, so the room drops that member when its link closes.

`tests/test_coop_env_file.py` checks that `hookshot_env.txt` sets no variable whose name begins `HOS_COOP` (every such
name the host's sources hold, and one made up), and that the environment and `HOS_ENV_FILE`'s file still do.
`tests/test_coop_options_text.py` checks that a `coop_server=` line of 159 bytes (the longest host address the game
keeps) is read whole from `hookshot_options.ini`, and that text inside such a line is not taken for a setting.

Wire byte counts in diagnostics are bounded by `COOP_MAX_DGRAM` and printed as `unsigned`, so the same source and
fixtures pass Microsoft's printf-format checking without changing the format attribute or requiring GNU stdio.

Besides what they compile and run, the protocol, session, progress and menu tests check in the source where the pieces
are wired in (the build, the game's start and its ways out, the present, the save mount, the menu's interface and the
host's table). The menu tests use scripted input and a simulated session; they do not cover a real window's events,
keyboards or controllers.

## Reliable events

`coop_send_event(member, kind, bytes, n)` and `coop_on_event(kind, callback, user)` are the one session helper for
one-off events and large initial state. Ordinary `coop_send` messages are still replaceable. The helper
wraps a layer's kind in EVENT (16), sends only to accepted current members, and dispatches only to that kind's event
handler. It never dispatches an event through `coop_on_message`, nor lets an ordinary message invoke an event handler.

- Each sender allocates a 64-bit event id for its session. It snapshots the recipients when queued (0 or
  `COOP_DST_ALL` means all accepted peers then). For each recipient it sends only the oldest event it still keeps.
  Thus the id in every fragment is also that recipient's delivery floor: a higher id retires any lower unfinished
  event, which the sender abandoned. The receiver retains this floor even when it discards partial bytes, so delayed
  pieces cannot revive an abandoned event. Events that are not abandoned remain FIFO per sender/recipient across
  kinds. No order across senders or with state messages. Cancellation cannot retract a callback already delivered.
- A recipient sends a small sealed EVENT_ACK (17) with the event id, received-part mask and assembly epoch, at most
  once per 50 ms per sender while something changed or a duplicate arrived. Discarding a partial assembly advances
  that epoch. A newer epoch replaces the sender's old mask and makes it resend missing pieces; an older epoch is
  ignored. Masks from two assemblies never combine. Consent revocations are recorded even if acceptance is granted
  again before the next socket step. Completed receipts survive revocation, preventing a second callback.
  ACKs are their own messages rather than changing every layer's wire format. A lost ACK is answered again when
  the sender retries. The final ACK means the whole event reached its callback, not that a hit or authority claim
  was valid or applied. ACK body version2 is 27 bytes; version1 is refused, so all peers need the updated helper.
- Missing parts are retried after 250 ms; every retry is sealed anew, with a new envelope counter and the same event
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

`tests/test_coop_events.py` runs the real session sender, codecs and receiver over sealed frames on loopback TCP, the
fixture holding the other end of each link so loss and reordering are its own choice: loss, reordering, duplicated
envelopes, full queues, departure, consent and fragmentation.
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

Design to 1,146 payload bytes per message (1,200 bytes including the sealed frame), about 30 state messages/s per
player, and one batch for **all** enemies that game leads at 10–15 Hz. Keep POS small; a richer body description uses
another registered kind, not a larger POS. Pace progress, events and state within the same ceilings, not as
separate bandwidth allowances. State should heal itself: for damage, keep a running total per player and enemy, merge
each player's total by taking the highest value seen, then add the players' totals together (one shared total merged
by "highest wins" loses a hit when two players strike at once: totals of 2 and 2 would stay 2, not 4; and a
subtraction sent once can be lost). Don't turn every animation frame into a reliable event.

Before received bytes count: check accepted current member/session, exact version and length, finite numbers and
bounded text/counts; signed rule **present and on** (absent is off); matching area/encounter and object generation;
the sender's authority and plausible values/timing; and a state counter that rejects stale/repeated updates. The
session has checked roster, destination, session and the sealed envelope, not those gameplay facts. Only the game
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

## The room's secret and the wire

For a reviewer: what is derived from what, what each frame carries and what a receiver checks. The code is
`host/coop_proto.c` (C); `tests/coop_vectors.txt` holds byte vectors of every message that it is checked against
(`tests/test_coop_proto.py`).

**Library.** Monocypher 4.0.2 (`third_party/monocypher/`, two files, unmodified, BSD-2-Clause, dual-licensed with CC0)
for BLAKE2b, XChaCha20-Poly1305 and EdDSA. It is small, has no dependencies, builds the same on Windows and Linux, and
its primitives are standard (RFC 7693, the XChaCha20-Poly1305 construction of the IETF draft, EdDSA over Curve25519).
It is used only in `coop_proto.c`, behind `coop_room_keys`, `coop_seal`, `coop_open`, the frame tags and, for what
one player signs, `coop_sign_keypair`, `coop_sign`, `coop_sign_check` and `coop_blake2b`, so another library can replace
it there.

**The secret.** 18 symbols, each drawn uniformly from 27 (bytes from the operating system's random source, a byte
below 243 taken modulo 27): log2(27^18) = 85.6 bits. It is made by the game that creates the room and never written to
a log. The host hands it to every player that reaches its port (the ROOM frame): that is how a joiner proves it belongs
to the room and how it can seal its messages, and it is also why reaching the port is enough to be in the room.

**Derivation.** With `S` the 18 canonical ASCII characters as the key of BLAKE2b:

- locator `L` = BLAKE2b-128(key `S`, "hookshot-coop/2 room"): the host's name for the room, and the key of the tags
  of the frames between a game and its host. The host and anyone who reaches its port learn `L`; getting `S` from it is
  a search over 2^85.6 secrets.
- message key `K` = BLAKE2b-256(key `S`, "hookshot-coop/2 messages"): seals every message between the players. Only
  holders of `S` have it.

**The frames between a game and its host.** One link per player, and everything on it is one of these. A 24-byte
header (magic, version 2, type, the room id — random for each time a game hosts, 0 until this link has joined — src,
dst, length, sequence) whose last 8 bytes are its tag, BLAKE2b-64(key `L`, header[0:16] ‖ payload). CONNECT carries
`L`, a client id (random for each start of the game), a session (random for each join), a nonce (random for each
attempt), the name, the host's cookie and the rejoin token. CHALLENGE, ROOM, JOINED and ERROR echo the nonce and carry
`L`'s tag; a game takes only answers with its own nonce and its own `L`, so a frame put on the link by something that
does not hold `L` neither ends an attempt nor puts the player into another room (an untagged "protocol version" error
is taken only as the first answer of an attempt). JOINED carries a rejoin token, random per member: the host gives a
member's id (and the maker's mark) back to a CONNECT that carries the token of a member this room still knows; a token
it does not know is answered as a first CONNECT is, so a player that comes back to a room started again proves the
keys and joins as a member of its own. The ROSTER lists each member's id, flags (away, maker), session and name. KICK
(maker to the host) names a member and its session. These frames are authenticated against outsiders who do not know
`L` only; a holder of the room's secret, or anyone who can read a CONNECT, can make them, which lets them disturb a
session (drop, reorder, invent members that cannot speak), not change a save.

**The player message envelope** (the payload of DATA, version byte 1):

```
u8 envelope version (1) | u64 sender's session | u32 counter | XChaCha20-Poly1305 ciphertext of (u8 kind, message) | u8[16] tag
nonce (24 bytes) = session (8, LE) | counter (4, LE) | sender's member id (1) | 11 zero bytes
associated data = destination member id, envelope version
```

A session is drawn for every join and the counter starts at 1. Before the counter wraps, the sender leaves the room
with an error and requires a new join; it never reuses a counter in that session. Random sessions can collide with
probability about n²/2^65. The kind byte and the message are inside the ciphertext: a new kind of message needs only
a number, and a later envelope version can change the outer fields without touching the messages. A message is at
most `COOP_MAX_MESSAGE` = 1146 bytes after its kind.

**What a receiver checks, in order:** the sender is in the roster and is not itself, the destination is it or
everyone, the envelope's session is the one the roster lists for that sender, the counter is new within a window of
the last 64 of that session, the tag opens with `K` for that sender and destination; only then is the counter recorded
and the message delivered. A counter older than the window is refused, unless it opens and is the fourth of that
sender's messages in a row below the window, each later than the one before: then the window starts again from it and
it is delivered. `K` is the whole room's, so whoever knows the secret can seal one message in a member's name with a
counter far ahead, which leaves that member's later messages below the window; the member's next four start it again,
and the first three of them are lost.

**Replay.** A message recorded before a player joined again carries the old session, which the roster no longer
lists: refused. Within a session the window refuses only what it still remembers: a message sent again within the
last 64 it took is refused, and an older one alone is refused too. But four older ones in a row, each later than the
one before, start the window again (that is how it recovers from a counter far ahead), and from then on the window
has forgotten what it took: anyone who recorded a member's messages of its present session (the host too, without
the secret) can have every one of them delivered again, in order. Someone who knows the secret can also seal a
recorded message again under a new counter. So the envelope does not make a message arrive once; a layer for which a
repeat matters checks a counter of its own, and signs what only one player may make.

**POS** (kind 2), where a player is and what Link plays, for showing the other players in the world: the area's name,
child or adult, position, facing, and the node and frame of Link's first four animation slots, with a counter one
higher for each POS sent (`coop_proto.h`). A POS with a name that is not printable ASCII or a number that is not finite
is refused. A receiver takes a pose only above the last one it took from the same member in the same session, and,
once none was taken for 3 s, the next whatever its counter (`coop_pos_fresh`).

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
  player who sent it, so someone who knows the room's secret and is the host can still make no message in another
  player's name that changes your save. For story flags and items acceptance is passed along (a player you accepted
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
only), so a maker whose game restarted is still the room's maker; that file, together with the room's secret, lets
whoever has both act as the room's maker: never hand out a copy of your user folder. The maker can change a rule
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
