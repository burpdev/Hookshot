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
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int Sock;
#define SOCK_NONE (-1)
#define sock_close close
#endif

struct CoopUdp { Sock s; uint16_t port; };

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

/* ---- the socket */

CoopUdp *coop_udp_open(uint16_t port) {
    if (!net_up()) return NULL;
    Sock s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SOCK_NONE) return NULL;
#ifdef _WIN32
    /* Windows otherwise allows a same-user, specific-address socket beside this wildcard bind. It could take
       this socket's datagrams (including a dual-stack socket's IPv4 traffic). Own the entire requested port. */
    const int exclusive = 1;
    if (setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof exclusive)) {
        sock_close(s);
        return NULL;
    }
#endif
    int off = 0;   /* (IPv4 too, as ::ffff:a.b.c.d) */
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof off);
    struct sockaddr_in6 any;
    memset(&any, 0, sizeof any);
    any.sin6_family = AF_INET6;
    any.sin6_addr = in6addr_any;
    any.sin6_port = htons(port);
    if (bind(s, (struct sockaddr *)&any, sizeof any)) {
        sock_close(s);
        return NULL;
    }
    socklen_t len = sizeof any;
    getsockname(s, (struct sockaddr *)&any, &len);
#ifdef _WIN32
    /* (an ICMP "port unreachable" for an earlier datagram would end every later recvfrom with WSAECONNRESET) */
    BOOL no = FALSE;
    DWORD got = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &no, sizeof no, NULL, 0, &got, NULL, NULL);
#endif
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(s, FIONBIO, &nonblocking)) {
#else
    if (fcntl(s, F_SETFL, O_NONBLOCK) < 0) {
#endif
        sock_close(s);
        return NULL;
    }
    CoopUdp *u = calloc(1, sizeof *u);
    if (!u) {
        sock_close(s);
        return NULL;
    }
    u->s = s, u->port = ntohs(any.sin6_port);
    return u;
}

uint16_t coop_udp_port(const CoopUdp *u) { return u ? u->port : 0; }

int coop_udp_send(CoopUdp *u, const CoopAddr *to, const void *p, size_t n) {
    struct sockaddr_in6 sa;
    addr_to(to, &sa);
    return sendto(u->s, (const char *)p, (int)n, 0, (const struct sockaddr *)&sa, sizeof sa) == (int)n;
}

int coop_udp_recv(CoopUdp *u, void *buf, size_t cap, CoopAddr *from, uint32_t ms) {
#ifdef _WIN32
    fd_set r;
    FD_ZERO(&r);
    FD_SET(u->s, &r);
    struct timeval tv = {(long)(ms / 1000), (long)(ms % 1000) * 1000};
    int ready = select(0, &r, NULL, NULL, &tv);
#else
    struct pollfd pf = {u->s, POLLIN, 0};
    int ready = poll(&pf, 1, (int)ms);
    if (ready < 0 && errno == EINTR) return 0;
#endif
    if (ready < 0) return -1;
    if (ready == 0) return 0;
    struct sockaddr_storage sa;
    socklen_t len = sizeof sa;
#ifdef _WIN32
    int n = (int)recvfrom(u->s, (char *)buf, (int)cap, 0, (struct sockaddr *)&sa, &len);
#else
    int n = (int)recvfrom(u->s, buf, cap, MSG_TRUNC, (struct sockaddr *)&sa, &len);   /* (the datagram's own size) */
    if (n > (int)cap) return 0;
#endif
    if (n < 0) {
#ifdef _WIN32
        const int e = WSAGetLastError();
        return e == WSAEMSGSIZE || e == WSAECONNRESET || e == WSAEWOULDBLOCK ? 0 : -1;
#else
        return errno == EAGAIN || errno == EINTR || errno == ECONNREFUSED ? 0 : -1;
#endif
    }
    addr_from(from, (const struct sockaddr *)&sa);
    return n;
}

void coop_udp_close(CoopUdp *u) {
    if (!u) return;
    sock_close(u->s);
    free(u);
}
