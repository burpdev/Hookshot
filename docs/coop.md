# Co-op

Each player runs their own game and their own save; co-op connects the games of up to 8 players through a relay. The
game has no multiplayer of its own and its `nn::socket` stays offline: co-op is the runtime's own, a UDP socket of its
own on a thread of its own.

What exists so far: the relay (`scripts/coop_relay.py`) and the wire format both ends speak (`host/coop_proto.c`):
rooms named by a secret, the datagrams between a game and the relay, and the sealed messages between the players. The
game does not use them yet; the session, the menu's Co-op tab and progress sync come next.

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

A session is drawn for every join and the counter starts at 1, so a nonce never repeats under `K` (two sessions
collide with probability about n²/2^65). The kind byte and the message are inside the ciphertext: a new kind of message
needs only a number, and a later envelope version can change the outer fields without touching the messages. A message
is at most `COOP_MAX_MESSAGE` = 1146 bytes after its kind.

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
