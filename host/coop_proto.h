// The co-op wire format, as pure functions on bytes: no sockets, no threads, no game state. docs/coop.md ("The link and
// the wire") is the description a reviewer checks this file against.
//
// One player hosts and the others are joined to that game: a TCP link each way, and the hosting game passes every
// message on (coop.c). There is no relay, no room to name and no code to type: the room is the host's socket, and the
// only thing a joiner gives is the host's address and port.
//
// A frame is an 8-byte header (little-endian) and a payload. HELLO, WELCOME, ROSTER, PING, PONG, BYE, KICK and ERROR are
// between a game and the host; DATA carries a player message. The host is member 1, gives every member its id, keeps the
// roster, and stamps the sender's id into a DATA frame before forwarding it, so a member cannot claim to be another.
// There is no encryption on this link: anyone who can reach the host's port can join, and the players' saves are
// protected by what their own keys sign (below), not by the link.
//
// A player message carries its sender's session (drawn for every join) and a counter from 1: the others take a message
// only of the session the roster lists for its sender and once within a window of 64 counters, so a message recorded
// before a rejoin, or sent again within the window, is refused. Four older ones in a row start the window again, after
// which a recorded message of that session can come again: a layer that must take a message once keeps a counter of its
// own, signed with its key, which only that player can move. tests/coop_vectors.txt holds byte vectors of these
// messages. Every parser checks every length against the bytes it was given and fails closed.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    COOP_PROTO = 3,              // protocol major: a host and a joiner talk only when it is equal
    COOP_FRAME_HDR = 8,          // u8 type, u8 src, u8 dst, u8 kind, u32 payload length
    COOP_MAX_PAYLOAD = 1160,     // a frame's payload at most
    COOP_MAX_FRAME = COOP_FRAME_HDR + COOP_MAX_PAYLOAD,
    COOP_DST_ALL = 0xFF,         // dst: every other member of the room
    COOP_MAX_MEMBERS = 8,
    COOP_NAME_MAX = 24,          // a player's name in bytes of UTF-8
    COOP_ROOM_ID_LEN = 16,       // the host's random id for its room: what a signature says the room it was made for is
    COOP_ROOM_TEXT = 16,         // shown as 16 hex characters
    COOP_MSG_V1 = 1,             // the envelope of a player message
    COOP_MSG_HDR = 13,           // u8 envelope version, u64 session, u32 counter
    COOP_MAX_MESSAGE = 1146,     // a player message's bytes after its kind, at most
};

// Frame types. HELLO is a joiner's first frame, WELCOME the host's answer with its member id and the room; ROSTER is the
// host's, PING and PONG keep the link warm and measure the round trip, BYE says a member is going, KICK that the host
// removed one, ERROR why a join was refused. DATA carries a player message.
enum { COOP_F_HELLO = 1, COOP_F_WELCOME = 2, COOP_F_ROSTER = 3, COOP_F_DATA = 4, COOP_F_PING = 5, COOP_F_PONG = 6,
       COOP_F_BYE = 7, COOP_F_KICK = 8, COOP_F_ERROR = 9 };
enum { COOP_E_ROOM_FULL = 1, COOP_E_PROTO = 2, COOP_E_CLOSED = 3, COOP_E_REMOVED = 4 };   // ERROR's reasons
enum { COOP_FLAG_AWAY = 1, COOP_FLAG_MAKER = 2 };   // roster flags: not heard from for a while; the host itself

// The kinds of player messages (the first byte of a message). The session carries application kinds and owns
// EVENT / EVENT_ACK for reliable delivery. A kind of 0xF0 and above is free for tests, not production layers.
enum { COOP_K_HELLO = 1, COOP_K_POS = 2, COOP_K_DIGEST = 3, COOP_K_SECTION = 4, COOP_K_SYNC_NEED = 5, COOP_K_NOTICE = 6,
       COOP_K_BYE = 7, COOP_K_EVENT = 16, COOP_K_EVENT_ACK = 17, COOP_K_LINE = 32, COOP_K_TEST = 0xF0 };

typedef struct {
    uint8_t type, src, dst, kind;   // member ids 1..8; src 0 is the host; kind: a DATA frame's message kind
    uint32_t len;                   // payload bytes
} CoopFrame;

// ---- frames
// The frame of f (its len is ignored: n is the payload length) and payload into out; bytes written, 0 when it does not
// fit cap or n is too long.
size_t coop_frame_pack(uint8_t *out, size_t cap, const CoopFrame *f, const void *payload, size_t n);
// 1 when d is exactly one frame of this format (a length that matches); *payload points into d.
int coop_frame_parse(const uint8_t *d, size_t n, CoopFrame *f, const uint8_t **payload);

// ---- names: a player's name from untrusted bytes. At most COOP_NAME_MAX bytes of valid UTF-8; a control character
// (C0, DEL, C1) and each byte that is not valid UTF-8 become '?', and so do the characters that reorder text (bidi
// controls). out is NUL-terminated (cap at least COOP_NAME_MAX + 1); the length.
size_t coop_name_clean(char *out, size_t cap, const uint8_t *raw, size_t n);

// ---- the host and a joiner
// ROSTER (host to everyone): u8 n, n x (u8 id, u8 flags, u64 session, u8 name length, name). The host is member 1 and
// carries COOP_FLAG_MAKER; id 1 is never given to a joiner.
typedef struct { uint8_t id, flags; uint64_t session; char name[COOP_NAME_MAX + 1]; } CoopMemberInfo;
typedef struct { uint8_t n; CoopMemberInfo m[COOP_MAX_MEMBERS]; } CoopRoster;
size_t coop_roster_pack(uint8_t *out, size_t cap, const CoopRoster *r);
// used (may be NULL): the bytes the roster took.
int coop_roster_parse(const uint8_t *p, size_t n, CoopRoster *r, size_t *used);

// HELLO (joiner to host): u8 the joiner's protocol major, u64 session (random for each join), u8 name length, name. A
// host refuses a joiner of another major with an ERROR rather than talk a language neither side understands.
typedef struct { uint8_t proto; uint64_t session; char name[COOP_NAME_MAX + 1]; } CoopHello;
size_t coop_hello_pack(uint8_t *out, size_t cap, const CoopHello *h);
int coop_hello_parse(const uint8_t *p, size_t n, CoopHello *h);

// WELCOME (host to the joiner it accepted): u8 its member id, u8[16] the room's id, u8 the host's protocol major,
// roster. A joiner whose protocol major is not the host's is told an ERROR and joins nothing.
typedef struct { uint8_t member, proto; uint8_t room_id[COOP_ROOM_ID_LEN]; CoopRoster roster; } CoopWelcome;
size_t coop_welcome_pack(uint8_t *out, size_t cap, const CoopWelcome *w);
int coop_welcome_parse(const uint8_t *p, size_t n, CoopWelcome *w);

// ERROR: u8 reason, u8 text length, text.
typedef struct { uint8_t reason; char text[61]; } CoopError;   // text for the log, at most 60 bytes
size_t coop_error_pack(uint8_t *out, size_t cap, const CoopError *e);
int coop_error_parse(const uint8_t *p, size_t n, CoopError *e);

// KICK (host to everyone): u8 the member removed, u64 its session.
size_t coop_kick_pack(uint8_t *out, size_t cap, uint8_t member, uint64_t session);
int coop_kick_parse(const uint8_t *p, size_t n, uint8_t *member, uint64_t *session);

size_t coop_u32_pack(uint8_t *out, size_t cap, uint32_t v);   // PING and PONG: the token
int coop_u32_parse(const uint8_t *p, size_t n, uint32_t *v);

// The room's id, shown: 16 hex characters, for the log and for what a signature says which room it was made for.
void coop_room_id_hex(const uint8_t id[COOP_ROOM_ID_LEN], char out[COOP_ROOM_TEXT + 1]);

// ---- player messages: the payload of a DATA frame
//   u8 envelope version (COOP_MSG_V1), u64 sender's session, u32 counter, u8 kind, bytes
// The host stamps the sender's member id into the frame it forwards; the session and counter say which join of the
// sender's a message is of and which of that join's messages this one is. A later envelope version can carry other
// fields; the kind byte and the message inside stay as they are, so a new kind needs only a number.
// Pack: the message into out (cap); its length, 0 when n is above COOP_MAX_MESSAGE or it does not fit.
size_t coop_msg_pack(uint8_t *out, size_t cap, uint64_t session, uint32_t counter, uint8_t kind, const void *bytes,
                     size_t n);
// 1 and the envelope's fields, with *bytes pointing into p. 0 when p is not an envelope of a version this build reads.
int coop_msg_parse(const uint8_t *p, size_t n, uint64_t *session, uint32_t *counter, uint8_t *kind,
                   const uint8_t **bytes, size_t *nout);

// ---- reliable events (session-owned kinds; layers register their inner kind with coop_on_event).
// EVENT: u8 version=1, u8 inner kind, u64 recipient session, u64 event id, u16 total bytes, u8 part,
// then exactly that part's bytes. Its id is also the sender's lowest live id for this recipient; lower unfinished
// ids are abandoned. EVENT_ACK: u8 version=2, u64 event sender session, u64 event id, u16 parts received, u64 epoch.
// epoch changes whenever the receiver discards a partial assembly; ACK masks from different epochs never combine.
// Parts are 1024 bytes except the last; an empty event has one empty part. All integers are little endian.
enum { COOP_EVENT_MAX = 16384, COOP_EVENT_PART = 1024, COOP_EVENT_HEAD = 21, COOP_EVENT_ACK_SIZE = 27 };
typedef struct {
    uint64_t recipient_session, id;
    uint16_t total;
    uint8_t kind, part;
    const uint8_t *bytes;
} CoopEventPart;
typedef struct { uint64_t sender_session, id; uint16_t parts; uint64_t epoch; } CoopEventAck;
int coop_event_kind(uint8_t kind);
uint16_t coop_event_parts(size_t total);   // full mask, 0 when too large
size_t coop_event_part_pack(uint8_t *out, size_t cap, const CoopEventPart *p);
int coop_event_part_parse(const uint8_t *bytes, size_t n, CoopEventPart *out);
size_t coop_event_ack_pack(uint8_t *out, size_t cap, const CoopEventAck *a);
int coop_event_ack_parse(const uint8_t *bytes, size_t n, CoopEventAck *out);

// ---- signatures: EdDSA over Curve25519 with BLAKE2b (the library's crypto_eddsa). The link is not encrypted and the
// host stamps no player message of its own, so what only one player may make is signed with that player's key: its
// identity in the room, the host's rules, and each pool contributor's counts (coop_state.c). Only the key's holder can
// make or change one, whoever forwarded it.
enum { COOP_PK_LEN = 32, COOP_SK_LEN = 64, COOP_SIG_LEN = 64 };
void coop_sign_keypair(const uint8_t seed[32], uint8_t sk[COOP_SK_LEN], uint8_t pk[COOP_PK_LEN]);
void coop_sign(uint8_t sig[COOP_SIG_LEN], const uint8_t sk[COOP_SK_LEN], const void *msg, size_t n);
int coop_sign_check(const uint8_t sig[COOP_SIG_LEN], const uint8_t pk[COOP_PK_LEN], const void *msg, size_t n);   // 1: valid
// BLAKE2b of msg keyed with key (klen at most 64, 0 for none), outlen bytes (1 to 64): the sync layer's derivations.
void coop_blake2b(uint8_t *out, size_t outlen, const void *key, size_t klen, const void *msg, size_t n);

// ---- POS (COOP_K_POS): where a player is and what Link plays, for showing the other players in the world, sent about 15
// times a second as a player message (above). The bytes after the kind, little endian: u8 version
// (COOP_POS_VERSION), u32 pseq (the sender's counter: one more for each POS it sends), u32 t_ms (the sender's clock), u8
// flags, u8 area length (<= COOP_POS_TEXT) and the area's name (the game's current-area text), f32 x y z, f32 yaw
// (radians, 0 facing +z), u8 slot count (<= COOP_POS_SLOTS), per AS slot u8 node name length (<= COOP_POS_TEXT), the
// node's name, f32 frame. A parser takes exactly one message of its version: another version is another format this
// build cannot read (version 1 had a u16 pseq). The names come from another player's game and reach this one's log and
// its animation lookup, so a name with a byte outside printable ASCII (0x20..0x7e) is refused, and so is a number that
// is not finite.
#define COOP_POS_VERSION 2
#define COOP_POS_TEXT 47
#define COOP_POS_SLOTS 4
enum { COOP_POS_ADULT = 1, COOP_POS_IN_WORLD = 2 };
typedef struct {
    uint32_t pseq;
    uint32_t t_ms;
    uint8_t flags, nslots;
    char area[COOP_POS_TEXT + 1];
    float pos[3], yaw;
    struct { char node[COOP_POS_TEXT + 1]; float frame; } slot[COOP_POS_SLOTS];
} CoopPos;
size_t coop_pos_pack(uint8_t *out, size_t cap, const CoopPos *p);   // 0 when it does not fit cap
int coop_pos_parse(const uint8_t *p, size_t n, CoopPos *out);

// The receiver's counter of one POS sender (a message can arrive more than once: docs/coop.md, "Replay"). A pose is
// taken only above the last one taken from the same member id in the same session, so neither a pose sent again nor one
// recorded earlier in the session moves the puppet back while that sender's poses keep being taken; the first pose of
// another session (another player given the id, the member back in a new session, or a roster that lists an earlier
// session) starts the counter again. After COOP_POS_QUIET_MS with none taken, whoever holds the sender's messages back
// that long can have its recorded poses of the session taken again: only the puppet moves.
// session: the one the roster lists for the member (CoopView). Returns 0 to drop the pose, 1 to take it, 2 to take it
// and drop what was kept of the sender's earlier poses.
#define COOP_POS_QUIET_MS 3000
typedef struct {
    uint64_t session, at_ms;    // the session of the last pose taken, and when it was taken
    uint32_t last;              // its counter
    int any;                    // a pose was taken
} CoopPosSeen;
int coop_pos_fresh(CoopPosSeen *s, uint64_t session, uint32_t pseq, uint64_t now_ms);
