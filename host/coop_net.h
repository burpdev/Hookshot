/* (hookshot) What co-op's session (host/coop.c) needs of the operating system, for Windows and Linux: a UDP socket
   (IPv6 with IPv4 mapped, so one socket reaches either), a name lookup, random bytes, a clock, a thread, a lock, an
   event and a once. Kept apart from the Win32 layer of host/posix so that tests compile host/coop.c with this file alone
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
typedef struct CoopUdp CoopUdp;

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
/* A UDP socket bound to port (0: any) on every address, IPv6 and IPv4; NULL when none could be made */
CoopUdp *coop_udp_open(uint16_t port);
uint16_t coop_udp_port(const CoopUdp *u);   /* the port it is bound to */
int coop_udp_send(CoopUdp *u, const CoopAddr *to, const void *p, size_t n);   /* 1 when handed to the OS */
/* One datagram into buf, waiting at most ms: its size, 0 when none came, -1 on an error (a datagram larger than cap
   is dropped and counts as none) */
int coop_udp_recv(CoopUdp *u, void *buf, size_t cap, CoopAddr *from, uint32_t ms);
void coop_udp_close(CoopUdp *u);
