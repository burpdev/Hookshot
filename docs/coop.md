# Co-op

Each player runs their own game and their own save; co-op connects the games of up to 8 players through a relay. The
game has no multiplayer of its own and its `nn::socket` stays offline: co-op is the runtime's own, a UDP socket of its
own on a thread of its own.

What exists so far: the relay (`scripts/coop_relay.py`), the wire format both ends speak (`host/coop_proto.c`: rooms
named by a secret, the datagrams between a game and the relay, the sealed messages between the players), and the
game's session (`host/coop.c`, `host/coop_net.c`): the game makes or joins a room from the Insert menu's Co-op tab, or
at start from its settings, and stays in it; and progress sync (`host/oot_coop.c`, [below](#progress-sync)): what one
player's game gains, the others' games get.

## Running a relay

```
python -I scripts/coop_relay.py [--host 0.0.0.0] [--port 24680] [--log-addrs] [--log-joins] [--max-rooms 1000] [--rooms-per-network 8] [--member-copies 480] [--member-bytes 262144]
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
  later, and every 5 s: a datagram is lost now and then, and roster updates are not acknowledged.
- Limits: a source address answers a cookie challenge before the relay sends it anything larger than it sent; each
  member is limited to 480 forwarded copies and 256 KiB per second, a datagram forwarded to seven counted seven times
  in both budgets (bursts 960 copies and 512 KiB). `--member-copies` and `--member-bytes` change the rates; each burst
  is twice its rate. A control datagram or one sent to nobody still costs one packet and its bytes. Each
  network (an IPv4 /24, an IPv6 /48) gets 20 challenges a second, a join every 3 s after a burst of 20, and 8 open rooms;
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

A full room can sustain 30 broadcasts per second from each of eight players: 210 forwarded copies per sender,
1,680 for the room. Even at the 1,200-byte datagram ceiling that is 252,000 bytes/s per sender and 2,016,000 for the
room. Design for one body update at 15 Hz and one batch of all enemies a game leads at 10–15 Hz, not one packet per
enemy. Smaller state messages leave room for events and progress; 30 maximum-sized broadcasts leave only 10,144
bytes/s of each sender's byte budget, not room for another full-rate stream. Progress sections and pushes still pace
themselves at 32 KiB/s before relay fan-out (8 KiB burst); the periodic and on-join digests are separate. This is a
ceiling, not bandwidth reserved in addition to gameplay. The relay's unchanged
8 MiB/s total and equal shares support four such full rooms by bytes alone; more active rooms share less. Room shares
and the relay's total can limit traffic before a member's own ceiling. None of these rates promises delivery.

### Hosting a room on your own PC

Set `coop_host=1` in `hookshot_options.ini`, or `HOS_COOP_HOST=1` in your own environment, then **Make a room**.
Nothing else needs starting. This is off by default and applies only to Make, not Join. Leave **Relay** empty for UDP
port 24680, or use `127.0.0.1:<port>` to choose another. An address in `coop_server` must be `localhost`, loopback or a
numeric address of this PC; a different host is refused with a clear log line, not silently replaced. `coop_port`
still selects the game's own client socket, not the relay port: normally leave it at 0.

On one home network, give friends one of the addresses printed as `hosting: players on this network can try ...`,
and the room secret separately. They put that address in **Relay** and join with the secret. The maker's own game
connects over loopback; the relay listens for IPv4 and IPv6 on its own UDP socket and thread, forwards the sealed
messages without reading them, and never touches game memory.

Across the internet, forward that **one UDP port** on the router to the maker's PC, allow it through the PC's firewall,
and give friends the router's public address and port. A router behind carrier-grade NAT may not accept a port forward;
use a reachable relay elsewhere in that case. The game does not discover a public address or change a router or firewall.
Windows may ask whether to allow Hookshot on a network: allow the network you intend to host on, not every network
indiscriminately. That prompt and Windows networking have not been tested here.

The maker pays for forwarding: each member has 480 forwarded copies and 256 KiB per second, with a two-second burst;
a broadcast to seven costs seven copies and seven times its bytes. With eight members this bounds DATA forwarding at
2 MiB/s sustained, a 4 MiB burst, plus the rate-limited connection and roster traffic. The relay's outer forwarding
cap remains 8 MiB/s. Playing below the limits uses less. This is not a bandwidth measurement from a game run.

Unlike a relay elsewhere, this room ends when the maker leaves, turns co-op off or closes the game. The other games
can keep playing their own saves, but cannot exchange messages until the maker hosts again. No room state is saved.

The hosted relay uses the same datagrams, cookie challenge, roster repeats, rejoin tokens, silence/drop times and
network budgets as the script. It accepts only the room and local maker socket authorized by Make; remote CREATEs
cannot start another room. Multiple-room fairness and per-network room counts therefore reduce to one room. Its
opaque cookies use keyed BLAKE2b rather than HMAC-SHA256, still bound to the address, port and current/previous minute.
It remembers 4096 sessions and refusals, and at most 4096 network budgets. Unlike the script's unbounded removal set,
it keeps at most 4096 removals; once full, it refuses new members rather than forgetting a removed player.
Only the latest refusal of a join attempt is remembered: when the reason changes, the attempt pays the network's
join budget again, even if the reason later changes back.

`tests/test_coop_host_relay.py` runs the applicable hostile-client and fake-clock cases from the Python relay tests
against both relays, using real loopback UDP for the C fixture. It also runs the session fixture through the C relay
at 0% and 30% simulated receive loss, and counts threads and sockets across starts, stops and hosted settings.

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
(below) changes them while the game runs.

| `hookshot_options.ini` | Environment | What it does | Default |
|---|---|---|---|
| `coop=1` | `HOS_COOP=1` | co-op on | off |
| `coop_name=` | `HOS_COOP_NAME` | the player's name in the room: at most 24 bytes of UTF-8 (a control or text-direction character, or a byte that is not UTF-8, shows as `?`) | `Player-` and four random hex digits, new at each start |
| `coop_server=` | `HOS_COOP_SERVER` | the relay: `host`, `host:port`, `[IPv6]:port` or a bare IPv6 address | none (the players run one); port 24680 |
| `coop_port=` | `HOS_COOP_PORT` | the game's own UDP port (one forwarded on the router, say) | 0: any free one |
| `coop_host=1` | `HOS_COOP_HOST=1` | Make a room hosts the relay on this PC; `coop_server` selects its local address/port, as above | off |
| | `HOS_COOP_AUTO=create\|join` with `HOS_COOP_ROOM=<secret>` | make or join that room at start, and stay: a room the relay lost (a restart) is made again by its maker and waited for by the others. `create` without a room makes a new one. For tests; with `HOS_COOP_ROOM` alone, the tab's Make a room makes the room of that secret | off |
| | `HOS_COOP_DROP=<percent>` | a test's loss: that share of the received datagrams is thrown away | 0 |
| | `HOS_COOP_TRACE=1` | a log line for every datagram sent and received, and the relay's address | off |

None of these variables is taken from `hookshot_env.txt`: the game leaves out every line of it whose name begins
`HOS_COOP`. That file is passed around between players and comes with packs; one that could set them would switch
co-op on, name its author's relay and put whoever uses it into its author's room. Co-op is switched on in
`hookshot_options.ini` and on the menu's Co-op tab; the variables are for the player's own environment and for
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
  address and is lost there. State messages continue; reliable events for that member wait for an acknowledgement.
- **after 8 s** without a datagram from the member, the relay's roster marks it away: the others' logs say
  `<name> (member <n>) is away`, and the Co-op tab shows the member as away. A game whose network
  only stalled sends again, and is `is back`.
- **after 15 s** the relay drops the member: its log says `<name> (member <n>) dropped, silent 15 s`, and the others'
  `<name> (member <n>) left the room`, as after a goodbye. The relay keeps the member id for a minute, so the same game,
  still running, joins again as the same member (`rejoined room`); a game started again is a new member.

So a killed game shows as present for 8 s and as away for 7 s more. Its progress up to its last message is with the
others already; nothing is taken from anyone when it goes.

### The Co-op tab

The Insert menu's last tab does the same from the game, with no setting written by hand. **Your name** and **Relay**
are `coop_name` and `coop_server`. **Make a room** and **Join a room** (the secret typed or pasted) switch co-op on for
this run and make or join the room. In a room the tab shows its state and the relay's round trip, the **Room secret**
(masked until Enter shows it), and a line per player with the fingerprint of the key their game signed; **Accept the
room** shows while the room waits for your acceptance (see "What joining a room means for your save"), **Leave the
room** always. The game that holds the maker's key also gets **Remove a player** and **Reset the pools**, once it has
accepted the room. **Room rules**, outside a room and for that game, opens a page with the rules of a room this game
makes (`coop_progress` .. `coop_ammo`, see "What is shared") and **Co-op at start** (`coop`). Accept, the remedies and
Leave take a second press. `HOS_COOP_DEMO=1` runs the tab on a made-up room, for screenshots: no session starts and
nothing is sent.

## The log

Every line of the session starts with `coop: `. The room's secret never reaches the log: a room is named by its first
three characters and `…` (`room H7K…`). A remote relay's address is logged only with `HOS_COOP_TRACE` on (`=1`; `=0`
is off, as for every on/off switch: `env_on`). Hosting deliberately logs this PC's candidate LAN addresses and port
so that the maker can give one to friends; it never logs the room secret.

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
off, on and off again as the Co-op tab does, each off leaving no thread and no socket behind (counted in `/proc/self/task`
and `/proc/self/fd`), these two also under ThreadSanitizer. Before taking the baseline, the fixture waits for each joined
helper's specific Linux thread ID to leave `/proc/self/task`: a userspace join alone can precede that removal. The
wait is bounded and the exact thread/socket-count checks remain. It also runs two sessions in
one process through a relay of the test's own that also acts as a hostile one (changed, forged and replayed messages,
garbage, forged rosters, lies to a player who connects), with no loss and with 30 % of the datagrams thrown away; and
the same two through `scripts/coop_relay.py` on 127.0.0.1, restarted midway, with the game's own session started from
the settings and, on Linux, a child process ended by SIGTERM, which says nothing: the others see it away, then gone
when the relay drops it 15 s later. Two games on one PC: start a relay
(`python -I scripts/coop_relay.py --host 127.0.0.1 --port 0 --log-joins`), then each game with `HOS_COOP=1
HOS_COOP_SERVER=127.0.0.1:<port>`, the first with `HOS_COOP_AUTO=create HOS_COOP_ROOM=<secret>`, the second with
`HOS_COOP_AUTO=join` and the same secret, each with its own `--user` folder.

The game and relay reserve their requested UDP addresses on Windows with `SO_EXCLUSIVEADDRUSE` before binding.
Without it, Windows permits another program of the same user to bind a specific IPv4 address beside a wildcard
socket, including a dual-stack socket: it can receive or inject datagrams and disrupt the room, though it cannot
decrypt sealed player messages without the room secret. Addresses and relay-control metadata are not encrypted.
The Linux paths set neither `SO_REUSEADDR` nor `SO_REUSEPORT`, so overlapping binds are refused there already.
The socket tests cover wildcard/specific IPv4 coexistence in both orders, and a competing socket requesting reuse,
on each OS; the compiled Windows-call fixture also checks option order and failure cleanup. The session modes still
run concurrently. See [Microsoft's socket-binding rules](https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse).
The Windows fixture drivers declare `_CRT_SECURE_NO_WARNINGS` before any header, including forced headers and each
separately compiled runtime source; their existing warning-as-error settings and assertions remain enabled.

On Windows, the second game folder can hard-link the game's read-only data files instead of copying them; give each game its own user folder.
A headless Windows game has no window to receive a polite close request; a scripted forced stop sends no goodbye, so an external relay drops it as `silent 15 s`.

`tests/test_coop_env_file.py` checks that `hookshot_env.txt` sets no variable whose name begins `HOS_COOP` (every such
name the host's sources hold, and one made up), and that the environment and `HOS_ENV_FILE`'s file still do.
`tests/test_coop_options_text.py` checks that a `coop_server=` line of 159 bytes (the longest relay text the game
keeps) is read whole from `hookshot_options.ini`, and that text inside such a line is not taken for a setting.

Wire byte counts in diagnostics are bounded by `COOP_MAX_DGRAM` and printed as `unsigned`, so the same source and
fixtures pass Microsoft's printf-format checking without changing the format attribute or requiring GNU stdio.

Besides what they compile and run, the protocol, session, progress and menu tests check in the source where the pieces
are wired in (the build, the game's start and its ways out, the present, the save mount, the menu's interface and the
host's table). The menu tests use scripted input and a simulated session; they do not cover a real window's events,
keyboards or controllers.

## Reliable events

`coop_send_event(member, kind, bytes, n)` and `coop_on_event(kind, callback, user)` are the one session helper for
one-off events and large initial state. Ordinary `coop_send` messages are still replaceable datagrams. The helper
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

`tests/test_coop_events.py` runs the real session sender, codecs and receiver over sealed loopback datagrams with a
deterministic relay clock: loss, reordering, duplicated envelopes, full queues, departure, consent and fragmentation.
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

Design to 1,146 payload bytes per message (1,200 bytes including the sealed datagram), about 30 state datagrams/s per
player, and one batch for **all** enemies that game leads at 10–15 Hz. Keep POS small; a richer body description uses
another registered kind, not a larger POS. Pace progress, events and state within the same relay ceilings, not as
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
  player who sent it, so someone who knows the room's secret and runs the relay can still make no message in another
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

**The relay keeps no progress.** A room is only its players: a player who joins while the others are away gets their
progress when one of them is in the room again (from that player's save, so only what their game had saved), not
before. That player must bring the room's rules: the maker in the run of the game that made the room, or a player who
accepted those rules. A maker whose game started again holds the room's key but brings no rules until such a player is
there, so with only those two nothing is offered and nothing is shared. A room nobody is in closes (10 minutes after
the last player left, a minute if only its maker was ever in it). Play together, or at least overlap: what a player
did alone reaches the others the next time both are in the room.

The game keeps its progress as flags (a database keyed by 32-bit hashes of flag names). At the end of each frame
(`host/oot_coop.c`, called last in the present, on the game's own thread) the runtime reads a quarter of the shared
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
