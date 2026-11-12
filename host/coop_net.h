/* (hookshot) What co-op's session (host/coop.c) needs of the operating system, for Windows and Linux: the TCP link it
   keeps with the player it hosts (IPv6 with IPv4 mapped, so one socket reaches either), a name lookup, random bytes, a
   clock, a thread, a lock, an event and a once. Kept apart from the Win32 layer of host/posix so that tests compile host/coop.c with this file alone
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
   Used once when hosting to check the configured host and show LAN addresses. */
unsigned coop_net_local_addresses(CoopAddr *out, unsigned cap, uint16_t port);
/* ---- the direct link: one player's game listens on a TCP port and the others connect to it.
   A frame is the whole of it as coop_proto.h packs one (its header says how long it is); coop_tcp_next hands over one
   whole frame at a time, so a frame split over packets, or two frames in one read, is not the caller's problem. */
typedef struct CoopTcp CoopTcp;                 /* one connection */
typedef struct CoopTcpListen CoopTcpListen;     /* the port one player hosts on */
typedef struct CoopTcpSet CoopTcpSet;           /* the connections one thread serves with one wait */

CoopTcpListen *coop_tcp_listen(uint16_t port);            /* NULL when the port cannot be had */
uint16_t coop_tcp_listen_port(const CoopTcpListen *l);    /* the port it is on (0 when there is none) */
CoopTcp *coop_tcp_accept(CoopTcpListen *l, CoopAddr *from);   /* NULL when none is waiting */
void coop_tcp_listen_close(CoopTcpListen *l);
CoopTcp *coop_tcp_connect(const CoopAddr *to, uint32_t ms);   /* NULL when no link is up within ms */
void coop_tcp_close(CoopTcp *t);
int coop_tcp_alive(const CoopTcp *t);   /* 0 once the other end closed, or a read or a write failed */
int coop_tcp_send(CoopTcp *t, const void *p, size_t n);   /* 1 when all of it is taken; 0 when its backlog is full or it is gone */

CoopTcpSet *coop_tcp_set_new(CoopTcpListen *l, unsigned max);   /* at most max connections at once */
void coop_tcp_set_free(CoopTcpSet *set);
int coop_tcp_add(CoopTcpSet *set, CoopTcp *t);   /* the slot it takes (1..max), 0 when there is no room */
CoopTcp *coop_tcp_at(const CoopTcpSet *set, int slot);
void coop_tcp_drop(CoopTcpSet *set, int slot);   /* closes the connection in that slot */
/* One whole frame into buf (cap at least COOP_MAX_DGRAM): its size, 0 when nothing is waiting, -1 when the listener has
   a connection to take; *slot is the slot the frame came from (0 for none). Waits at most ms in all: call it again with 0
   to take the rest. A frame larger than cap, or a header that is not one, closes that connection. */
int coop_tcp_next(CoopTcpSet *set, void *buf, size_t cap, int *slot, uint32_t ms);
