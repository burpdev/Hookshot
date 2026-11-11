# Co-op

Each player runs their own game and their own save; co-op connects the games of up to 8 players through a relay. The
game has no multiplayer of its own and its `nn::socket` stays offline: co-op is the runtime's own, a UDP socket of its
own on a thread of its own.

What exists so far: the relay (`scripts/coop_relay.py`), the wire format both ends speak (`host/coop_proto.c`: rooms
named by a secret, the datagrams between a game and the relay, the sealed messages between the players), and the
game's session (`host/coop.c`, `host/coop_net.c`): switched on in the settings, the game makes or joins a room at start
and stays in it. Nothing is shared yet: the menu's Co-op tab and progress sync come next.

## Running a relay

```
python -I scripts/coop_relay.py [--host 0.0.0.0] [--port 24680] [--log-addrs] [--log-joins] [--max-rooms 1000] [--rooms-per-network 8]
```

Python 3, standard library only. A player who can reach it by UDP (forward the port on the router, open it in the
firewall) is all it takes; the others give its address to their games. The relay forwards the sealed datagrams of a
room and can read none of what the players send each other. It keeps only who is in which room, writes nothing to
disk, and loses its rooms when it stops: the room's maker makes it again under the same secret. Its log has one line
per room made, join, leave and removal, with names, member ids and a random room id (never the secret or the locator);
addresses only with `--log-addrs`; a line when a network's joins ran out (at most every 10 s); and with `--log-joins`,
where each network's joins went.

- A room holds up to 8 players, is closed 10 minutes after the last one left (a minute, if nobody but its maker was ever
  in it), and a player the relay has not heard from for 15 s is dropped (the member id is kept for a minute so that the
  same game, with its rejoin token, joins again as the same member).
- The roster (who is in the room, each one's session, which one made it) goes out at a change, again 0.25 s and 0.75 s
  later, and every 5 s: a datagram is lost now and then, and nothing is acknowledged.
- Limits: a source address answers a cookie challenge before the relay sends it anything larger than it sent; each
  member is limited to 200 datagrams and 64 KB per second, a datagram forwarded to seven counted seven times; each
  network (an IPv4 /24, an IPv6 /48) to 20 challenges a second, a join every 3 s after a burst of 20, and 8 open rooms;
  each room that forwarded to someone in the last 2 s to an equal share of the forwarding (rooms that send nothing, or
  send to nobody, take nothing); the relay as a whole to 5000 challenges a second and 8 MB/s of forwarding. A join
  attempt costs its network one join whatever the answer, and the same attempt sent again (its answer was lost, or it
  asks again for a room that is not there yet) is answered from memory without a charge while the answer is the same
  refusal. The relay keeps the budgets of at most 4096 networks and forgets a budget that is full again, and checks its
  own challenge budget first, so a flood from spoofed addresses leaves no state behind.
- A member that rejoins with its token from its own address and in its own session needs no challenge; from another
  address, or with a new session, it answers one first, so someone who saw its CONNECT cannot take its place. The
  challenges for a CONNECT with a member's valid token come from that member's own challenges (2 a second, 8 at once,
  kept apart for its own network and for the others), never from its packets, and from a share set aside for rejoins:
  1000 a second split equally among the open rooms for CONNECTs from other networks than the member's, and 1000 a second
  split among the rooms and then among the eight member numbers of each for CONNECTs from the member's own network. So
  a flood of CONNECTs with a member's token, someone else's or seen on the way, from another network neither drops the
  member's messages nor keeps its rejoin from its own network out of the room. A session that was in the room before is
  refused (the last 4096 of each room are kept). A flood from many networks can still use up the relay's 5000
  challenges a second for newcomers: a relay has finite capacity, and its operator's firewall is the place for more.
- `--host ::` listens for IPv6 as well as IPv4.

## Rooms and secrets

A room is its secret: 18 characters of `23456789BCDFGHJKMNPRSTVWXYZ` (no vowels, no 0/1/O/I/L, no Q or E), typed in
any case, with or without `-` and spaces, shown `XXXXXX-XXXXXX-XXXXXX`. The game makes a new random one for each room it
makes, and the maker passes it on to the others (by voice, a chat message). The relay never learns it. A create of a
secret whose room is open on the relay is refused (it is not a join), so nobody can be put into a room they did not
mean to join.

## Switching it on: the game's settings

Co-op is off unless switched on. Off, the game opens no socket and starts no thread (`coop_init` in
`host/hos_main.c` returns at once). The settings are read once, when the game starts: `hookshot_options.ini` beside the
game, and the environment over it (a variable that is set wins over the file's line). The Insert menu's Co-op tab
will change them while the game runs.

| `hookshot_options.ini` | Environment | What it does | Default |
|---|---|---|---|
| `coop=1` | `HOS_COOP=1` | co-op on | off |
| `coop_name=` | `HOS_COOP_NAME` | the player's name in the room: at most 24 bytes of UTF-8 (a control or text-direction character, or a byte that is not UTF-8, shows as `?`) | `Player-` and four random hex digits, new at each start |
| `coop_server=` | `HOS_COOP_SERVER` | the relay: `host`, `host:port`, `[IPv6]:port` or a bare IPv6 address | none (the players run one); port 24680 |
| `coop_port=` | `HOS_COOP_PORT` | the game's own UDP port (one forwarded on the router, say) | 0: any free one |
| | `HOS_COOP_AUTO=create\|join` with `HOS_COOP_ROOM=<secret>` | make or join that room at start, and stay: a room the relay lost (a restart) is made again by its maker and waited for by the others. `create` without a room makes a new one. For tests and LAN parties until the tab exists | off |
| | `HOS_COOP_DROP=<percent>` | a test's loss: that share of the received datagrams is thrown away | 0 |
| | `HOS_COOP_TRACE=1` | a log line for every datagram sent and received, and the relay's address | off |

None of these variables is taken from `hookshot_env.txt`: the game leaves out every line of it whose name begins
`HOS_COOP`. That file is passed around between players and comes with packs; one that could set them would switch
co-op on, name its author's relay and put whoever uses it into its author's room. Co-op is switched on in
`hookshot_options.ini` (and, once it exists, the menu); the variables are for the player's own environment and for
tests (the file `HOS_ENV_FILE` names, a test run's own, sets them like anything else).

In a room the game sends the relay a PING every 2 s (the keep-alive that also holds a home router's mapping open; the
PONG gives the round trip), and takes 8 s without any datagram from the relay as the relay lost: it joins the same room
again as the same member (`relay lost; rejoining`). Reaching the relay is a burst of four CONNECTs 250 ms apart, then a
wait of 1, 2, 4, 8 and 15 s between bursts. When the player leaves the room, switches co-op off or closes the game's
window, the game says BYE to the others and LEAVE to the relay (twice each, in under half a second), so the others see
the player go at once. One host thread does all of it, as the Discord status's does; it never touches the game's
memory. The thread exists only while co-op is switched on: switching co-op off, or closing the game, asks it to end
and waits for it (at most 0.4 s), with the socket closed; switching co-op on starts it again.

### A game that ends without a goodbye

The goodbye is said on the game's own ways out only (`hos_window_closed_exit` in `host/hos_relaunch.c`, and the control
bridge's `quit` in `host/control.c`). Co-op installs no signal handler. With a window, SIGTERM and SIGINT close the
window (SDL turns them into its quit event), so the goodbye is said. A game that is killed (SIGKILL; SIGTERM or SIGINT
for a game without a window, as `HOS_HEADLESS` test runs are), crashes, or loses its network says nothing, and the
relay goes by silence:

- **0 to 8 s:** nothing changes for the others. The player is still in their roster; what they send still goes to its
  address and is lost there. Nothing of their own is held up: no message waits for an answer.
- **after 8 s** without a datagram from the member, the relay's roster marks it away: the others' logs say
  `<name> (member <n>) is away`, and the view the menu will draw has the member's `away` set. A game whose network
  only stalled sends again, and is `is back`.
- **after 15 s** the relay drops the member: its log says `<name> (member <n>) dropped, silent 15 s`, and the others'
  `<name> (member <n>) left the room`, as after a goodbye. The relay keeps the member id for a minute, so the same game,
  still running, joins again as the same member (`rejoined room`); a game started again is a new member.

So a killed game shows as present for 8 s and as away for 7 s more. Its progress up to its last message is with the
others already; nothing is taken from anyone when it goes.

## The log

Every line of the session starts with `coop: `. The room's secret never reaches the log: a room is named by its first
three characters and `…` (`room H7K…`). Nor does the relay's address, unless `HOS_COOP_TRACE` is on (`=1`; `=0` is off,
as for every on/off switch: `env_on`).

| Line | When |
|---|---|
| `on, as <name>; the relay from HOS_COOP_SERVER` (or `hookshot_options.ini`), or `...; no relay given (...)` | at start, co-op on |
| `HOS_COOP_AUTO=<create\|join>: room XXX… at start` (or `a new room at start`) | the room the game makes or joins at start |
| `connecting to the relay`, `the relay answered in <n> ms` | each attempt; the first answer of one |
| `room XXX… created (member 1)`, `joined room XXX… as member <n>`, `rejoined room XXX… as member <n>` | in the room |
| `<name> (member <n>) joined the room`, `... left the room`, `... is away`, `... is back`, `member <n> is now <name>` | the others, from the relay's roster |
| `stats: sent <n>, accepted <n>, thrown away <n> (tag, sender, not co-op, replay), dropped by the test <n>, outbox full <n>, round trip <n> ms` | every minute in a room while datagrams flow (the keep-alives count) |
| `relay lost; rejoining`, `room XXX… is gone from the relay`, `creating room XXX… again`, `room XXX… is open already; joining it` | the relay went silent or restarted |
| `left room (<why>)` | leaving: `leave requested`, `the game is closing`, `co-op turned off`, `the server changed`, `the room's maker removed this player`, ... |
| `no room: <why>` | an attempt refused: `no such room`, `the room is full`, `a room with this secret is already open on the relay: join it`, `update required: ...`, ... |
| `removing <name> (member <n>) from the room` | the room's maker removed a member |
| `a datagram that is not a co-op datagram was thrown away`, `... from an address that is not the relay ...`, `a message that does not open with the room's key ...`, `a message of member <n> that is not of its present session, or seen before, ...` | at most every 30 s each; the stats line has the counts |
| `cannot open a UDP socket`, `cannot resolve the relay's name`, `the system's random source failed; co-op stays off`, `cannot start the co-op thread`, `HOS_COOP_AUTO must be off, create or join`, `HOS_COOP_ROOM is not a room secret ...` | what went wrong |
| with `HOS_COOP_TRACE`: `the relay is <address>`, `tx <TYPE> to <member>, <n> bytes`, `rx <TYPE>, <n> bytes`, `rx message kind <k> from member <n>, <n> bytes` | every datagram |

The relay's own log has the other side: `room <id> created`, `room <id>: <name> joined as member <n> (<k>/8)`,
`... rejoined as member <n>`, `<name> (member <n>) left (<k>/8)`, `... dropped, silent 15 s`, `room <id> closed, ...`.

## Testing it

`tests/test_coop_session.py` (in `tests/run_all.py`) compiles `host/coop.c` and `host/coop_net.c` with
`tests/coop_session_test.c` (AddressSanitizer and UBSan on Linux) and runs, at once: the sockets on loopback (IPv4 and
IPv6, a datagram too large dropped), `coop_server_parse`'s cases, and co-op off (no thread, no socket); co-op switched
off, on and off again as the menu will, each off leaving no thread and no socket behind (counted in `/proc/self/task`
and `/proc/self/fd`), these two also under ThreadSanitizer; two sessions in
one process through a relay of the test's own that also acts as a hostile one (changed, forged and replayed messages,
garbage, forged rosters, lies to a player who connects), with no loss and with 30 % of the datagrams thrown away; and
the same two through `scripts/coop_relay.py` on 127.0.0.1, restarted midway, with the game's own session started from
the settings and, on Linux, a child process ended by SIGTERM, which says nothing: the others see it away, then gone
when the relay drops it 15 s later. Two games on one PC: start a relay
(`python -I scripts/coop_relay.py --host 127.0.0.1 --port 0 --log-joins`), then each game with `HOS_COOP=1
HOS_COOP_SERVER=127.0.0.1:<port>`, the first with `HOS_COOP_AUTO=create HOS_COOP_ROOM=<secret>`, the second with
`HOS_COOP_AUTO=join` and the same secret, each with its own `--user` folder.

`tests/test_coop_env_file.py` checks that `hookshot_env.txt` sets no variable whose name begins `HOS_COOP` (every such
name the host's sources hold, and one made up), and that the environment and `HOS_ENV_FILE`'s file still do.
`tests/test_coop_options_text.py` checks that a `coop_server=` line of 159 bytes (the longest relay text the game
keeps) is read whole from `hookshot_options.ini`, and that text inside such a line is not taken for a setting.

## The room's secret and the wire

For a reviewer: what is derived from what, what each datagram carries and what a receiver checks. The code is
`host/coop_proto.c` (C) and `scripts/coop_relay.py` (the relay's side, Python); `tests/coop_vectors.txt` holds byte
vectors of every message that both are checked against (`tests/test_coop_proto.py`, `tests/test_coop_relay.py`).

**Library.** Monocypher 4.0.2 (`third_party/monocypher/`, two files, unmodified, BSD-2-Clause, dual-licensed with CC0)
for BLAKE2b, XChaCha20-Poly1305 and EdDSA. It is small, has no dependencies, builds the same on Windows and Linux, and
its primitives are standard (RFC 7693, the XChaCha20-Poly1305 construction of the IETF draft, EdDSA over Curve25519).
It is used only in `coop_proto.c`, behind `coop_room_keys`, `coop_seal`, `coop_open`, the datagram tags and, for what
one player signs, `coop_sign_keypair`, `coop_sign`, `coop_sign_check` and `coop_blake2b`, so another library can replace
it there. The relay needs no library: it uses Python's own BLAKE2b.

**The secret.** 18 symbols, each drawn uniformly from 27 (bytes from the operating system's random source, a byte
below 243 taken modulo 27): log2(27^18) = 85.6 bits. It is made by the game that creates the room and never sent to
the relay or written to a log.

**Derivation.** With `S` the 18 canonical ASCII characters as the key of BLAKE2b:

- locator `L` = BLAKE2b-128(key `S`, "hookshot-coop/2 room"): the relay's name for the room, and the key of the tags
  of datagrams between a game and the relay. The relay and anyone who sees a CONNECT learn `L`; getting `S` from it is
  a search over 2^85.6 secrets.
- message key `K` = BLAKE2b-256(key `S`, "hookshot-coop/2 messages"): seals every message between the players. Only
  holders of `S` have it.

**The game and relay datagrams.** A 24-byte header (magic, version 2, type, the relay's room id, src, dst, length,
sequence) and an 8-byte tag, BLAKE2b-64(key `L`, header[0:16] ‖ payload). CONNECT carries `L`, a client id (random for
each start of the game), a session (random for each join), a nonce (random for each attempt), the name, the relay's
cookie and the rejoin token. CHALLENGE, JOINED and ERROR echo the nonce and carry `L`'s tag; a game takes only answers
with its own nonce and its own `L`, so a datagram someone else sends from the relay's address neither ends an attempt
nor puts the player into another room (an untagged "protocol version" error is taken only as the first answer of an
attempt). JOINED carries a rejoin token, random per member: the relay gives a member's id (and the maker's mark) back
only to a CONNECT with it. The ROSTER lists each member's id, flags (away, maker), session and name. KICK (maker to
relay) names a member and its session. These datagrams are authenticated against outsiders who do not know `L` only;
the relay and an observer of a CONNECT can make them, which lets them disturb a session (drop, reorder, invent members
that cannot speak), not change a save.

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
has forgotten what it took: anyone who recorded a member's messages of its present session (the relay too, without
the secret) can have every one of them delivered again, in order. Someone who knows the secret can also seal a
recorded message again under a new counter. So the envelope does not make a message arrive once; a layer for which a
repeat matters checks a counter of its own, and signs what only one player may make.

**POS** (kind 2), where a player is and what Link plays, for showing the other players in the world: the area's name,
child or adult, position, facing, and the node and frame of Link's first four animation slots, with a counter one
higher for each POS sent (`coop_proto.h`). A POS with a name that is not printable ASCII or a number that is not finite
is refused. A receiver takes a pose only above the last one it took from the same member in the same session, and,
once none was taken for 3 s, the next whatever its counter (`coop_pos_fresh`).
