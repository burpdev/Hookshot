/* (hookshot) What co-op's session (host/coop.c) needs of the operating system, for Windows and Linux: a TCP link (one
   player's game listens, the others connect to it), a name lookup, random bytes, a clock, a thread, a lock, an event and
   a once. Kept apart from the Win32 layer of host/posix so that tests compile host/coop.c with this file alone
   (tests/test_coop_session.py). Nothing here touches the game. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
typedef struct { SRWLOCK l; } CoopLock;              /* zero-initialised: unlocked */
typedef struct { CONDITION_VARIABLE c; SRWLOCK l; int set, manual; } CoopEvent;
typedef struct { INIT_ONCE o; } CoopOnce;
typedef struct { HANDLE h; void (*fn)(void *); void *arg; } CoopThread;   /* zero-initialised: no thread */
#define COOP_ONCE_INIT {INIT_ONCE_STATIC_INIT}
#else
#include <pthread.h>
typedef struct { pthread_mutex_t m; } CoopLock;
typedef struct { pthread_mutex_t m; pthread_cond_t c; int set, manual; } CoopEvent;
typedef struct { pthread_once_t o; } CoopOnce;
typedef struct { pthread_t h; CoopEvent done; void (*fn)(void *); void *arg; int live; } CoopThread;   /* zero-initialised: no thread */
#define COOP_ONCE_INIT {PTHREAD_ONCE_INIT}
#endif
/* An address in one layout on every OS: IPv6 bytes (IPv4 as ::ffff:a.b.c.d), the port, the scope; two equal addresses
   compare equal (coop_addr_eq) however the OS reported them */
typedef struct { uint8_t ip[16]; uint16_t port; uint32_t scope; } CoopAddr;

void coop_lock_init(CoopLock *l);
void coop_lock(CoopLock *l);
void coop_unlock(CoopLock *l);

void coop_event_init(CoopEvent *e, int manual, int initially_set);
void coop_event_set(CoopEvent *e);
void coop_event_reset(CoopEvent *e);
int coop_event_wait_ns(CoopEvent *e, uint64_t ns);   /* 1 when the event was set, 0 at the timeout */

void coop_once(CoopOnce *o, void (*fn)(void *arg), void *arg);   /* fn(arg) once in the process, the others wait for it */

uint64_t coop_now_ns(void);   /* monotonic */
static inline uint64_t coop_now_ms(void) { return coop_now_ns() / 1000000u; }
void coop_sleep_ms(uint32_t ms);
int coop_random(void *out, size_t n);   /* bytes from the system's random source (BCryptGenRandom, getrandom); 1 when it gave them */
/* A host thread that is waited for. coop_thread_start: fn(arg) on a new thread; 1 when it started (t stays where it is
   until the thread is joined). coop_thread_join waits at most ms for fn to have returned: 1 when it had, and the thread
   is gone (t can start another); 0 at the timeout, when t stays as it is and can be joined again. One thread at a time
   starts and joins a given t. */
int coop_thread_start(CoopThread *t, void (*fn)(void *arg), void *arg);
int coop_thread_join(CoopThread *t, uint32_t ms);

/* host (a name or a literal address) and port into out, IPv4 preferred when both exist; 1 on success */
int coop_net_resolve(const char *host, uint16_t port, CoopAddr *out);
int coop_addr_eq(const CoopAddr *a, const CoopAddr *b);
void coop_addr_str(const CoopAddr *a, char *out, size_t cap);   /* "1.2.3.4:5" or "[::1]:5" */
/* Numeric, non-loopback addresses of this PC, no DNS or network traffic; at most cap, with port attached.
   Used once when hosting to show the addresses the others can type. */
unsigned coop_net_local_addresses(CoopAddr *out, unsigned cap, uint16_t port);

/* ---- the TCP link. One player hosts: coop_tcp_listen binds the port on every address (IPv4 and IPv6) and owns it;
   the others coop_tcp_connect to the host's address. Non-blocking both ways: what the OS has not taken yet is kept
   here (one link's bytes at a time, COOP_TCP_OUT_CAP at most) and what arrived is kept until it is read, so a link
   never holds up the thread that steps it. coop_tcp_readable waits at most ms; -1 means the link is closed or
   broken and is only cleared by closing it. */
enum { COOP_TCP_OUT_CAP = 2048, COOP_MAX_PENDING = 8 };   /* a link's own buffer, and how many joins may wait */
typedef struct CoopTcp CoopTcp;
CoopTcp *coop_tcp_listen(uint16_t port);            /* NULL when the port is taken or a socket failed */
uint16_t coop_tcp_port(const CoopTcp *l);           /* the port it is bound to */
CoopTcp *coop_tcp_accept(CoopTcp *l, CoopAddr *from);   /* NULL when nobody is waiting */
CoopTcp *coop_tcp_connect(const CoopAddr *to, uint32_t ms);   /* NULL when it could not be reached in ms */
int coop_tcp_send(CoopTcp *t, const void *p, size_t n);   /* 1 when the link took all n; 0 when it would not fit */
int coop_tcp_readable(CoopTcp *t, uint32_t ms);           /* 1 something may be read, 0 nothing, -1 closed or broken */
size_t coop_tcp_read(CoopTcp *t, void *buf, size_t cap);  /* what arrived, copied out (0: nothing waiting) */
/* One whole frame off the stream. A link is a stream of bytes: what one recv gives may be half a frame and the start
   of the next, so a reader that takes "whatever arrived" and calls it a frame loses both. coop_tcp_frame takes the
   header first, then the payload its length asks for, and holds what is left over for the next call.
   COOP_TCP_HDR is the frame header's size (host/coop_proto.h's COOP_FRAME_HDR), kept here so this layer does not
   need the rest of the wire format.
   1: a frame was taken (*out bytes); 0: fewer bytes are here than that frame asks for, take the rest when the link
   has more; -1: these are not a frame's bytes (a frame longer than cap, or no room even for the header): the stream
   is not co-op's, and whoever reads it should close the link. */
enum { COOP_TCP_HDR = 8, COOP_TCP_MAX_FRAME = 1168, COOP_TCP_MAX_BODY = COOP_TCP_MAX_FRAME - COOP_TCP_HDR };
int coop_tcp_frame(CoopTcp *t, void *buf, size_t cap, size_t *out);
int coop_tcp_backlog(const CoopTcp *t);                   /* bytes this link holds for the OS */
void coop_tcp_close(CoopTcp *t);
