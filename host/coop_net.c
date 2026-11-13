/* (hookshot) co-op's operating-system layer (coop_net.h): Winsock and the Win32 primitives on Windows, BSD sockets and
   pthreads on Linux, as host/discord_rpc.c splits its pipe. */
#include "coop_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <iphlpapi.h>
typedef SOCKET Sock;
#define SOCK_NONE INVALID_SOCKET
#define sock_close closesocket
#define sock_error() WSAGetLastError()
#define sock_would_block (WSAGetLastError() == WSAEWOULDBLOCK)
/* A non-blocking connect that has started and has not finished. Winsock says WSAEWOULDBLOCK; Linux says EINPROGRESS,
   which is not EAGAIN, and calling that a failure closes the link before the room is even reached. */
#define sock_connecting sock_would_block
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int Sock;
#define SOCK_NONE (-1)
#define sock_close close
#define sock_error() errno
#define sock_would_block (errno == EAGAIN || errno == EWOULDBLOCK)
#define sock_connecting (errno == EINPROGRESS || sock_would_block)
#endif

/* A link whose peer closed is an everyday thing in a room: whoever is writing to it must be told with an error, not
   with a signal. On Linux a write to that socket raises SIGPIPE, which ends the game process; MSG_NOSIGNAL asks the OS
   for EPIPE instead, and the link reads as broken like any other. Windows has no SIGPIPE. */
#ifdef _WIN32
#define sock_send_flags 0
#else
#define sock_send_flags MSG_NOSIGNAL
#endif

struct CoopTcp {
    Sock s;
    int listening;
    int broken;                     /* the OS says the link is gone: readable returns -1 until it is closed */
    unsigned char out[COOP_TCP_OUT_CAP];
    size_t out_len, out_at;       /* what the OS has not taken yet, from out_at */
    unsigned char in[COOP_TCP_OUT_CAP];
    size_t in_len, in_at;         /* what arrived and has not been read */
};

/* ---- lock, event, once, clock, thread, random */
#ifdef _WIN32
void coop_lock_init(CoopLock *l) { InitializeSRWLock(&l->l); }
void coop_lock(CoopLock *l) { AcquireSRWLockExclusive(&l->l); }
void coop_unlock(CoopLock *l) { ReleaseSRWLockExclusive(&l->l); }

void coop_event_init(CoopEvent *e, int manual, int initially_set) {
    InitializeConditionVariable(&e->c);
    InitializeSRWLock(&e->l);
    e->set = initially_set, e->manual = manual;
}
void coop_event_set(CoopEvent *e) {
    AcquireSRWLockExclusive(&e->l);
    e->set = 1;
    ReleaseSRWLockExclusive(&e->l);
    WakeAllConditionVariable(&e->c);
}
void coop_event_reset(CoopEvent *e) {
    AcquireSRWLockExclusive(&e->l);
    e->set = 0;
    ReleaseSRWLockExclusive(&e->l);
}
int coop_event_wait_ns(CoopEvent *e, uint64_t ns) {
    const uint64_t end = coop_now_ns() + ns;
    AcquireSRWLockExclusive(&e->l);
    while (!e->set) {
        const uint64_t now = coop_now_ns();
        if (now >= end) break;
        SleepConditionVariableSRW(&e->c, &e->l, (DWORD)((end - now + 999999) / 1000000), 0);
    }
    const int was = e->set;
    if (was && !e->manual) e->set = 0;
    ReleaseSRWLockExclusive(&e->l);
    return was;
}

typedef struct { void (*fn)(void *); void *arg; } OnceCall;
static BOOL CALLBACK once_call(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o, (void)ctx;
    ((OnceCall *)p)->fn(((OnceCall *)p)->arg);
    return TRUE;
}
void coop_once(CoopOnce *o, void (*fn)(void *arg), void *arg) {
    OnceCall c = {fn, arg};
    InitOnceExecuteOnce(&o->o, once_call, &c, NULL);
}

uint64_t coop_now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / freq.QuadPart) * 1000000000u + (uint64_t)(t.QuadPart % freq.QuadPart) * 1000000000u / (uint64_t)freq.QuadPart;
}
void coop_sleep_ms(uint32_t ms) { Sleep(ms); }
int coop_random(void *out, size_t n) {
    return BCryptGenRandom(NULL, (PUCHAR)out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

static DWORD WINAPI thread_main(void *p) {
    CoopThread *t = (CoopThread *)p;
    t->fn(t->arg);
    return 0;
}
int coop_thread_start(CoopThread *t, void (*fn)(void *arg), void *arg) {
    t->fn = fn, t->arg = arg;
    t->h = CreateThread(NULL, 256u << 10, thread_main, t, 0, NULL);
    return t->h != NULL;
}
int coop_thread_join(CoopThread *t, uint32_t ms) {
    if (!t->h) return 1;
    if (WaitForSingleObject(t->h, ms) != WAIT_OBJECT_0) return 0;
    CloseHandle(t->h);
    t->h = NULL;
    return 1;
}

static int net_up(void) {   /* WSAStartup once */
    static volatile LONG state;   /* 0 not yet, 1 up, 2 failed */
    if (state == 0) {
        WSADATA w;
        LONG want = WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 1 : 2;
        InterlockedCompareExchange(&state, want, 0);
    }
    return state == 1;
}
#else
void coop_lock_init(CoopLock *l) { pthread_mutex_init(&l->m, NULL); }
void coop_lock(CoopLock *l) { pthread_mutex_lock(&l->m); }
void coop_unlock(CoopLock *l) { pthread_mutex_unlock(&l->m); }

void coop_event_init(CoopEvent *e, int manual, int initially_set) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, &a);
    pthread_condattr_destroy(&a);
    e->set = initially_set, e->manual = manual;
}
void coop_event_set(CoopEvent *e) {
    pthread_mutex_lock(&e->m);
    e->set = 1;
    pthread_cond_broadcast(&e->c);
    pthread_mutex_unlock(&e->m);
}
void coop_event_reset(CoopEvent *e) {
    pthread_mutex_lock(&e->m);
    e->set = 0;
    pthread_mutex_unlock(&e->m);
}
int coop_event_wait_ns(CoopEvent *e, uint64_t ns) {
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    const uint64_t t = (uint64_t)end.tv_nsec + ns;
    end.tv_sec += (time_t)(t / 1000000000u), end.tv_nsec = (long)(t % 1000000000u);
    pthread_mutex_lock(&e->m);
    while (!e->set && pthread_cond_timedwait(&e->c, &e->m, &end) != ETIMEDOUT) {}
    const int was = e->set;
    if (was && !e->manual) e->set = 0;
    pthread_mutex_unlock(&e->m);
    return was;
}

/* (pthread_once takes no argument: the call's function and argument are handed to it through a thread's own value,
   so two threads at the same once write nothing they share) */
typedef struct { void (*fn)(void *); void *arg; } OnceCall;
static pthread_key_t once_key;
static pthread_once_t once_key_made = PTHREAD_ONCE_INIT;
static void make_once_key(void) { pthread_key_create(&once_key, NULL); }
static void once_call(void) {
    const OnceCall *c = pthread_getspecific(once_key);
    c->fn(c->arg);
}
void coop_once(CoopOnce *o, void (*fn)(void *arg), void *arg) {
    OnceCall c = {fn, arg};
    pthread_once(&once_key_made, make_once_key);
    pthread_setspecific(once_key, &c);
    pthread_once(&o->o, once_call);
}

uint64_t coop_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
void coop_sleep_ms(uint32_t ms) {
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
int coop_random(void *out, size_t n) {
    for (size_t got = 0; got < n;) {
        ssize_t k = getrandom((uint8_t *)out + got, n - got, 0);
        if (k < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        got += (size_t)k;
    }
    return 1;
}

/* (pthread_join has no timeout: the thread says when fn has returned, and is joined then, a few instructions from its end) */
static void *thread_main(void *p) {
    CoopThread *t = p;
    t->fn(t->arg);
    coop_event_set(&t->done);
    return NULL;
}
int coop_thread_start(CoopThread *t, void (*fn)(void *arg), void *arg) {
    t->fn = fn, t->arg = arg;
    coop_event_init(&t->done, 1, 0);
    if (pthread_create(&t->h, NULL, thread_main, t)) {
        pthread_cond_destroy(&t->done.c);
        pthread_mutex_destroy(&t->done.m);
        return 0;
    }
    t->live = 1;
    return 1;
}
int coop_thread_join(CoopThread *t, uint32_t ms) {
    if (!t->live) return 1;
    if (!coop_event_wait_ns(&t->done, (uint64_t)ms * 1000000u)) return 0;
    pthread_join(t->h, NULL);
    pthread_cond_destroy(&t->done.c);
    pthread_mutex_destroy(&t->done.m);
    t->live = 0;
    return 1;
}

static int net_up(void) { return 1; }
#endif

/* ---- addresses */

static void addr_from(CoopAddr *a, const struct sockaddr *sa) {
    memset(a, 0, sizeof *a);
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)sa;
        a->ip[10] = a->ip[11] = 0xff;
        memcpy(a->ip + 12, &v4->sin_addr, 4);
        a->port = ntohs(v4->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)sa;
        memcpy(a->ip, &v6->sin6_addr, 16);
        a->port = ntohs(v6->sin6_port);
        a->scope = v6->sin6_scope_id;
    }
}

static void addr_to(const CoopAddr *a, struct sockaddr_in6 *out) {
    memset(out, 0, sizeof *out);
    out->sin6_family = AF_INET6;
    memcpy(&out->sin6_addr, a->ip, 16);
    out->sin6_port = htons(a->port);
    out->sin6_scope_id = a->scope;
}

static int is_v4(const CoopAddr *a) {
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return !memcmp(a->ip, mapped, 12);
}

int coop_net_resolve(const char *host, uint16_t port, CoopAddr *out) {
    if (!net_up() || !host || !*host) return 0;
    struct addrinfo hint, *res = NULL;
    memset(&hint, 0, sizeof hint);
    hint.ai_family = AF_UNSPEC;
    hint.ai_socktype = SOCK_DGRAM;
    char service[8];
    snprintf(service, sizeof service, "%u", (unsigned)port);
    if (getaddrinfo(host, service, &hint, &res) || !res) return 0;
    const struct addrinfo *pick = NULL;
    for (const struct addrinfo *r = res; r; r = r->ai_next)
        if (r->ai_family == AF_INET || (r->ai_family == AF_INET6 && !pick)) pick = r;
    if (pick) addr_from(out, pick->ai_addr);
    freeaddrinfo(res);
    return pick != NULL;
}

int coop_addr_eq(const CoopAddr *a, const CoopAddr *b) {
    return !memcmp(a->ip, b->ip, 16) && a->port == b->port && (is_v4(a) || a->scope == b->scope);
}

void coop_addr_str(const CoopAddr *a, char *out, size_t cap) {
    char ip[64] = "?";
    if (is_v4(a)) inet_ntop(AF_INET, a->ip + 12, ip, sizeof ip);
    else inet_ntop(AF_INET6, a->ip, ip, sizeof ip);
    snprintf(out, cap, is_v4(a) ? "%s:%u" : "[%s]:%u", ip, (unsigned)a->port);
}

static void local_address_add(CoopAddr *out, unsigned *count, unsigned cap, const struct sockaddr *sa, uint16_t port) {
    if (!sa || (sa->sa_family != AF_INET && sa->sa_family != AF_INET6) || *count >= cap) return;
    CoopAddr a;
    addr_from(&a, sa);
    a.port = port;
    /* Link-local IPv6 needs the other PC's interface scope, not ours: do not advertise it as a join address. */
    if (a.ip[0] == 0xfe && (a.ip[1] & 0xc0) == 0x80) return;
    for (unsigned i = 0; i < *count; i++) if (coop_addr_eq(&out[i], &a)) return;
    out[(*count)++] = a;
}
unsigned coop_net_local_addresses(CoopAddr *out, unsigned cap, uint16_t port) {
    unsigned count = 0;
#ifdef _WIN32
    union { IP_ADAPTER_ADDRESSES align; unsigned char bytes[16384]; } buffer;
    ULONG size = sizeof buffer;
    IP_ADAPTER_ADDRESSES *list = (IP_ADAPTER_ADDRESSES *)buffer.bytes;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             NULL, list, &size) != NO_ERROR) return 0;
    for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
            local_address_add(out, &count, cap, u->Address.lpSockaddr, port);
    }
#else
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list)) return 0;
    for (const struct ifaddrs *a = list; a; a = a->ifa_next)
        if ((a->ifa_flags & IFF_UP) && !(a->ifa_flags & IFF_LOOPBACK))
            local_address_add(out, &count, cap, a->ifa_addr, port);
    freeifaddrs(list);
#endif
    return count;
}


/* ---- the TCP link: one player's game listens, the others connect to it */

static int sock_nonblocking(Sock s) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    int f = fcntl(s, F_GETFL, 0);
    return f >= 0 && fcntl(s, F_SETFL, f | O_NONBLOCK) == 0;
#endif
}

/* A peer that vanished without a goodbye shows here in about a minute, and a home router's mapping stays open. */
static void sock_keepalive(Sock s) {
    const int on = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&on, sizeof on);
#ifdef _WIN32
    struct { u_long onoff, keepalivetime, keepaliveinterval; } v = {1, 10000, 3000};
    DWORD got = 0;
    WSAIoctl(s, 0x98000004 /* SIO_KEEPALIVE_VALS */, &v, sizeof v, NULL, 0, &got, NULL, NULL);
#else
    static const int idle = 10, interval = 3;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
#endif
}

/* Windows permits a same-user, specific-address socket beside a default wildcard bind, including the IPv4 half of a
   dual-stack socket: another program could take this link's traffic, or send from an overlapping local endpoint. Own the
   whole requested port. Linux enables neither SO_REUSEADDR nor SO_REUSEPORT, so an overlapping bind is refused there. */
static int sock_exclusive(Sock s) {
#ifdef _WIN32
    const int exclusive = 1;
    return setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof exclusive) == 0;
#else
    (void)s;
    return 1;
#endif
}

static CoopTcp *tcp_new(Sock s, int listening) {
    CoopTcp *t = calloc(1, sizeof *t);
    if (!t) {
        sock_close(s);
        return NULL;
    }
    t->s = s, t->listening = listening;
    return t;
}

CoopTcp *coop_tcp_listen(uint16_t port) {
    if (!net_up()) return NULL;
    Sock s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s == SOCK_NONE) return NULL;
    if (!sock_exclusive(s)) {
        sock_close(s);
        return NULL;
    }
    int off = 0;   /* (IPv4 too, as ::ffff:a.b.c.d) */
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof off);
    struct sockaddr_in6 any;
    memset(&any, 0, sizeof any);
    any.sin6_family = AF_INET6;
    any.sin6_addr = in6addr_any;
    any.sin6_port = htons(port);
    if (bind(s, (struct sockaddr *)&any, sizeof any) || listen(s, COOP_MAX_PENDING)) {
        sock_close(s);
        return NULL;
    }
    socklen_t len = sizeof any;
    getsockname(s, (struct sockaddr *)&any, &len);
    if (!sock_nonblocking(s)) {
        sock_close(s);
        return NULL;
    }
    return tcp_new(s, 1);
}

uint16_t coop_tcp_port(const CoopTcp *t) {
    struct sockaddr_in6 at;
    socklen_t len = sizeof at;
    if (!t || getsockname(t->s, (struct sockaddr *)&at, &len)) return 0;
    return ntohs(at.sin6_port);
}

CoopTcp *coop_tcp_accept(CoopTcp *l, CoopAddr *from) {
    if (!l || l->broken) return NULL;
    struct sockaddr_storage sa;
    socklen_t len = sizeof sa;
    Sock s = accept(l->s, (struct sockaddr *)&sa, &len);
    if (s == SOCK_NONE) return NULL;   /* (the listener is non-blocking: nobody is waiting) */
    if (!sock_nonblocking(s)) {
        sock_close(s);
        return NULL;
    }
    sock_keepalive(s);
    if (from) addr_from(from, (const struct sockaddr *)&sa);
    return tcp_new(s, 0);
}

CoopTcp *coop_tcp_connect(const CoopAddr *to, uint32_t ms) {
    if (!net_up()) return NULL;
    Sock s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s == SOCK_NONE) return NULL;
    if (!sock_nonblocking(s)) {
        sock_close(s);
        return NULL;
    }
    struct sockaddr_in6 at;
    addr_to(to, &at);
    if (connect(s, (struct sockaddr *)&at, sizeof at) != 0 && !sock_connecting) {
        sock_close(s);
        return NULL;
    }
#ifdef _WIN32
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    struct timeval tv = {(long)(ms / 1000), (long)(ms % 1000) * 1000};
    const int ready = select(0, NULL, &w, NULL, &tv);
#else
    struct pollfd pf = {s, POLLOUT, 0};
    int ready = poll(&pf, 1, (int)ms);
    if (ready < 0 && errno == EINTR) ready = 0;
#endif
    int failed = ready <= 0;
    if (!failed) {
        int err = 0;
        socklen_t size = sizeof err;
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &size) == 0 && err) failed = 1;
    }
    if (failed) {
        sock_close(s);
        return NULL;
    }
    sock_keepalive(s);
    return tcp_new(s, 0);
}

/* Hand what is waiting for the OS to take it, then take p. 0 when the link cannot hold it: the caller drops that frame
   rather than growing without bound, and a link that keeps filling is a link that is going. */
static int tcp_pump(CoopTcp *t) {
    while (t->out_at < t->out_len) {
        const int wrote = (int)send(t->s, (const char *)t->out + t->out_at, (int)(t->out_len - t->out_at), sock_send_flags);
        if (wrote > 0) {
            t->out_at += (size_t)wrote;
            if (t->out_at >= t->out_len) t->out_len = t->out_at = 0;
            continue;
        }
        if (sock_would_block) return 1;   /* (the OS's buffer is full: tried again at the next step) */
        t->broken = 1;
        return 0;
    }
    return 1;
}

int coop_tcp_send(CoopTcp *t, const void *p, size_t n) {
    if (!t || t->broken || !n) return 0;
    if (!tcp_pump(t)) return 0;
    if (!t->out_len) {   /* (nothing waiting: hand it to the OS directly) */
        const int wrote = (int)send(t->s, (const char *)p, (int)n, sock_send_flags);
        if (wrote == (int)n) return 1;
        if (wrote < 0 && sock_would_block) {
            if (n > sizeof t->out) {
                t->broken = 1;
                return 0;
            }
            memcpy(t->out, p, n), t->out_len = n, t->out_at = 0;
            return 1;
        }
        t->broken = 1;
        return 0;
    }
    if (t->out_len + n > sizeof t->out) {
        if (t->out_at && t->out_len - t->out_at + n <= sizeof t->out) {   /* (slide what is left to the front) */
            memmove(t->out, t->out + t->out_at, t->out_len - t->out_at);
            t->out_len -= t->out_at, t->out_at = 0;
        } else
            return 0;
    }
    memcpy(t->out + t->out_len, p, n), t->out_len += n;
    return 1;
}

/* What the link holds, moved to the front so what arrives next lands after it: a frame the network cut in two is
   joined here, not thrown away. */
static void tcp_settle(CoopTcp *t) {
    if (t->in_at) {
        memmove(t->in, t->in + t->in_at, t->in_len - t->in_at);
        t->in_len -= t->in_at, t->in_at = 0;
    }
}

/* The size of the whole frame at the front of what is held, 0 when the bytes there are not a frame yet. */
static size_t tcp_frame_here(const CoopTcp *t) {
    const size_t have = t->in_len - t->in_at;
    if (have < COOP_TCP_HDR) return 0;
    const uint8_t *h = t->in + t->in_at;
    const uint32_t len = (uint32_t)h[4] | ((uint32_t)h[5] << 8u) | ((uint32_t)h[6] << 16u) | ((uint32_t)h[7] << 24u);
    if (len > COOP_TCP_MAX_BODY) return (size_t)-1;   /* (no reader of this room's can hold that: not a co-op stream) */
    return have >= COOP_TCP_HDR + (size_t)len ? COOP_TCP_HDR + (size_t)len : 0;
}

int coop_tcp_readable(CoopTcp *t, uint32_t ms) {
    if (!t || t->broken) return -1;
    if (t->listening) {
        /* A door is not a link: nothing is read from it, only whether someone is waiting behind it. recv here would
           answer "not connected", which is not the door being broken: it is the door having no one at it. */
#ifdef _WIN32
        fd_set r;
        FD_ZERO(&r);
        FD_SET(t->s, &r);
        struct timeval tv = {(long)(ms / 1000), (long)(ms % 1000) * 1000};
        const int waiting = select(0, &r, NULL, NULL, &tv);
#else
        struct pollfd pf = {t->s, POLLIN, 0};
        int waiting = poll(&pf, 1, (int)ms);
        if (waiting < 0 && errno == EINTR) return 0;
#endif
        if (waiting < 0) {
            t->broken = 1;   /* (the door itself is gone: not a join to take) */
            return -1;
        }
        return waiting > 0 ? 1 : 0;
    }
    tcp_settle(t);
    tcp_pump(t);   /* (what the OS has not taken yet of what this link is sending goes out now: the step that is
                    * waiting to hear is also the step that can finish sending) */
    if (tcp_frame_here(t) || t->in_len == sizeof t->in) return 1;   /* (what is already here is enough to take) */
#ifdef _WIN32
    fd_set r;
    FD_ZERO(&r);
    FD_SET(t->s, &r);
    struct timeval tv = {(long)(ms / 1000), (long)(ms % 1000) * 1000};
    const int ready = select(0, &r, NULL, NULL, &tv);
#else
    struct pollfd pf = {t->s, POLLIN, 0};
    int ready = poll(&pf, 1, (int)ms);
    if (ready < 0 && errno == EINTR) return 0;
#endif
    if (ready < 0) {
        t->broken = 1;
        return -1;
    }
    if (ready == 0) return 0;
    const int n = (int)recv(t->s, (char *)(t->in + t->in_len), (int)(sizeof t->in - t->in_len), 0);
    if (n == 0 || (n < 0 && !sock_would_block)) {
        t->broken = 1;   /* (the other end closed, or the link is gone) */
        return -1;
    }
    if (n < 0) return 0;
    t->in_len += (size_t)n;
    return 1;
}

size_t coop_tcp_read(CoopTcp *t, void *buf, size_t cap) {
    if (!t) return 0;
    const size_t have = t->in_len - t->in_at, n = have < cap ? have : cap;
    if (n) memcpy(buf, t->in + t->in_at, n);
    t->in_at += n;
    if (t->in_at >= t->in_len) t->in_len = t->in_at = 0;
    return n;
}

/* One whole frame off the stream: the header, then the payload its length asks for. What is left over stays here for
   the next call, and a frame that is not whole yet is left whole in here too: nothing is taken apart and thrown away. */
int coop_tcp_frame(CoopTcp *t, void *buf, size_t cap, size_t *out) {
    if (out) *out = 0;
    if (!t || t->broken || cap < COOP_TCP_HDR) return -1;
    const size_t have = t->in_len - t->in_at;
    if (have < COOP_TCP_HDR) return 0;
    const uint8_t *h = t->in + t->in_at;
    const uint32_t len = (uint32_t)h[4] | ((uint32_t)h[5] << 8u) | ((uint32_t)h[6] << 16u) | ((uint32_t)h[7] << 24u);
    if (len > (uint32_t)(cap - COOP_TCP_HDR)) return -1;   /* (not a frame this reader can hold: the stream is not co-op's) */
    if (have < COOP_TCP_HDR + (size_t)len) return 0;       /* (the rest of it has not arrived) */
    memcpy(buf, h, COOP_TCP_HDR + (size_t)len);
    t->in_at += COOP_TCP_HDR + (size_t)len;
    if (t->in_at >= t->in_len) t->in_len = t->in_at = 0;
    if (out) *out = COOP_TCP_HDR + (size_t)len;
    return 1;
}

int coop_tcp_backlog(const CoopTcp *t) { return t ? (int)(t->out_len - t->out_at) : 0; }

void coop_tcp_close(CoopTcp *t) {
    if (!t) return;
    sock_close(t->s);
    free(t);
}
