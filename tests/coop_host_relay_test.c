/* Real UDP carrier for the Python relay's fake-clock cases, plus the production thread's lifetime and the
   process's Host setting. Reuse the session fixture's existing assertions rather than a second session model. */
#define _CRT_SECURE_NO_WARNINGS
#define main session_fixture_main
#include "coop_session_test.c"
#undef main
#include "../host/coop_relay.c"

static void hex_out(const void *bytes, size_t n) {
    const uint8_t *p = bytes;
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
}
static int unhex(const char *s, uint8_t *out, size_t n) {
    if (strlen(s) != n * 2) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + i * 2, "%2x", &v) != 1) return 0;
        out[i] = (uint8_t)v;
    }
    return 1;
}
static void carrier_emit(CoopRelay *r, const CoopAddr *to, const uint8_t *data, size_t n) {
    CoopAddr *carrier = r->emit_user;
    CHECK(coop_udp_send(r->socket, carrier, data, n), "reply on real UDP carrier");
    char address[64];
    coop_addr_str(to, address, sizeof address);
    printf("O %s %u\n", address, (unsigned)n);
}
static void snapshot(CoopRelay *r) {
    static const char *const names[] = {"rx", "malformed", "bad_tag", "no_member", "wrong_addr", "rate_limited",
        "challenges", "joins", "rejoins", "leaves", "drops", "forwarded", "rooms_made", "kicks", "egress_limited", "repeats", "join_charges"};
    printf("S {\"room\":%u,\"locator\":\"", r->room);
    hex_out(r->locator, 16);
    printf("\",\"members\":[");
    int comma = 0;
    for (unsigned i = 1; i <= COOP_MAX_MEMBERS; i++) {
        RelayMember *m = &r->members[i];
        if (!m->used) continue;
        char address[64];
        coop_addr_str(&m->addr, address, sizeof address);
        printf("%s{\"id\":%u,\"live\":%d,\"maker\":%d,\"session\":\"%llx\",\"addr\":\"%s\"}",
               comma ? "," : "", i, m->live, m->maker, (unsigned long long)m->session, address);
        comma = 1;
    }
    unsigned joins = 0, challenges = 0, failed = 0;
    for (unsigned i = 0; i < RELAY_NET_SLOTS; i++) if (r->networks[i].state == 1) {
        if (r->networks[i].kind == 1) joins++; else challenges++;
    }
    for (unsigned i = 0; i < RELAY_HISTORY; i++) failed += r->failed[i].reason != 0;
    printf("],\"netjoins\":%u,\"netchallenges\":%u,\"failed\":%u,\"sessions\":%u,\"stats\":{", joins, challenges, failed, r->session_count);
    for (unsigned i = 0; i < R_STATS; i++) printf("%s\"%s\":%llu", i ? "," : "", names[i], (unsigned long long)r->stats[i]);
    printf("}}\n");
    if (log_len) { printf("L "); hex_out(log_text, log_len); printf("\n"); log_len = 0; log_text[0] = 0; }
    puts("END");
    fflush(stdout);
}
static int drive(void) {
    CoopRelay *r = calloc(1, sizeof *r);
    CHECK(r != NULL, "allocate relay");
    if (!r) return 1;
    CoopUdp *socket = coop_udp_open(0);
    CHECK(socket != NULL, "real UDP carrier");
    if (!socket) { free(r); return 1; }
    CoopAddr carrier = {0};
    char line[1024];
    printf("listening %u\n", (unsigned)coop_udp_port(socket));
    fflush(stdout);
    while (fgets(line, sizeof line, stdin)) {
        if (line[0] == 'Q') break;
        if (line[0] == 'N') {
            memset(r, 0, sizeof *r);
            relay_init(r, 0);
            RelayLimits *l = &r->limits;
            CHECK(sscanf(line + 1, "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", &l->pkt_rate, &l->pkt_burst,
                &l->byte_rate, &l->byte_burst, &l->join_rate, &l->join_burst, &l->challenge_rate, &l->prefix_rate,
                &l->prefix_burst, &l->egress_rate, &l->rejoin_rate) == 11, "limits");
            relay_bucket_init(&r->challenges, l->challenge_rate, l->challenge_rate, 0);
            relay_bucket_init(&r->rejoins, l->rejoin_rate, l->rejoin_rate > 8 ? l->rejoin_rate : 8, 0);
            relay_bucket_init(&r->egress, l->egress_rate, l->egress_rate, 0);
            CHECK(coop_random(r->cookie_key, sizeof r->cookie_key), "cookie key");
            r->socket = socket, r->emit = carrier_emit, r->emit_user = &carrier;
        } else if (line[0] == 'A') {
            char ip[128], locator[33];
            unsigned port;
            unsigned long long maker;
            CHECK(sscanf(line + 1, "%llx %127s %u %32s", &maker, ip, &port, locator) == 4, "authorization");
            r->maker = (uint64_t)maker;
            CHECK(coop_net_resolve(ip, (uint16_t)port, &r->maker_addr), "maker address");
            CHECK(unhex(locator, r->locator, 16), "locator");
        } else if (line[0] == 'H' || line[0] == 'C') {
            char ip[128];
            unsigned port;
            double now;
            CHECK(sscanf(line + 1, "%lf %127s %u", &now, ip, &port) == 3, "datagram address and clock");
            CoopAddr addr;
            CHECK(coop_net_resolve(ip, (uint16_t)port, &addr), "fake address");
            if (line[0] == 'C') {
                uint8_t cookie[16];
                relay_cookie(r, &addr, (int64_t)now, cookie);
                printf("C "); hex_out(cookie, 16); puts("");
            } else {
                uint8_t data[65536];
                int n = coop_udp_recv(socket, data, sizeof data, &carrier, 2000);
                CHECK(n >= 0, "receive real UDP carrier");
                relay_handle(r, data, n > 0 ? (size_t)n : 0, &addr, now);
            }
        } else if (line[0] == 'T') {
            double now;
            CHECK(sscanf(line + 1, "%lf", &now) == 1, "tick clock");
            relay_tick(r, now);
        } else if (line[0] == 'B') {
            /* Bound reached without 4096 handshakes: use the actual refusal and kick paths afterwards. */
            r->banned_count = RELAY_HISTORY;
        } else CHECK(0, "unknown driver command");
        snapshot(r);
    }
    coop_udp_close(socket);
    free(r);
    return fails != 0;
}
static int serve(unsigned port) {
    CoopRelay *r = calloc(1, sizeof *r);
    if (!r) return 1;
    relay_init(r, (double)coop_now_ns() / 1e9);
    uint8_t key[32];
    coop_room_keys(ROOM_SECRET, r->locator, key);
    r->maker = 0xA11CE;
    if (!coop_random(r->cookie_key, sizeof r->cookie_key) || !(r->socket = coop_udp_open((uint16_t)port))) { free(r); return 1; }
    r->emit = relay_udp_emit;
    log_echo = 1;
    printf("listening %u\n", (unsigned)coop_udp_port(r->socket));
    fflush(stdout);
    for (;;) {
        uint8_t bytes[COOP_MAX_DGRAM];
        CoopAddr addr;
        int n = coop_udp_recv(r->socket, bytes, sizeof bytes, &addr, 20);
        double now = (double)coop_now_ns() / 1e9;
        if (n > 0) {
            /* Only this standalone test's known Alice can claim the initial local socket. The production start
               gets that address from the session before it opens the relay to anybody. */
            CoopHdr h; const uint8_t *p; CoopConnect c;
            if (!r->maker_addr.port && coop_dgram_parse(bytes, (size_t)n, &h, &p) && h.type == COOP_T_CONNECT &&
                coop_connect_parse(p, h.len, &c) && c.client_id == r->maker && c.intent == COOP_INTENT_CREATE)
                r->maker_addr = addr;
            relay_handle(r, bytes, (size_t)n, &addr, now);
        }
        relay_tick(r, now);
    }
}
typedef struct { CoopUdp *socket; CoopAddr target; int stop; } RelayFlood;
static void relay_flood(void *arg) {
    RelayFlood *f = arg;
    uint8_t garbage[COOP_MAX_DGRAM] = {0};
    while (!__atomic_load_n(&f->stop, __ATOMIC_ACQUIRE))
        for (int i = 0; i < 16; i++) coop_udp_send(f->socket, &f->target, garbage, sizeof garbage);
}
static void host_lifetime(void) {
    uint8_t locator[16], key[32];
    coop_room_keys(ROOM_SECRET, locator, key);
    CHECK(sizeof(CoopRelay) < 2u * 1024u * 1024u, "fixed state below 2 MiB");
    int before_tasks = tasks(), before_files = files();
    for (int i = 0; i < 30; i++) {
        CoopRelay *r = coop_relay_start(0, locator, 17, 12345);
        CHECK(r != NULL, "relay starts");
        if (!r) break;
#ifdef __linux__
        CHECK(WAIT_FOR(tasks() == before_tasks + 1, 2000), "one relay thread");
        CHECK(files() == before_files + 1, "one relay socket");
#endif
        uint16_t port = coop_relay_port(r);
        CoopRelay *other = coop_relay_start(port, locator, 18, 12346);
        CHECK(!other, "occupied port fails without a leaked thread/socket");
        RelayFlood flood = {0};
        Thread flooding;
        if (i % 2) {
            flood.socket = coop_udp_open(0);
            CHECK(flood.socket && coop_net_resolve("127.0.0.1", port, &flood.target), "flood socket");
            flooding = thread_new(relay_flood, &flood);
            coop_sleep_ms(10);
        }
        uint64_t started = coop_now_ms();
        CHECK(coop_relay_stop(&r) && !r, "relay joined and freed");
        CHECK(coop_now_ms() - started < 400, "stop within the session's 400 ms bound");
        if (i % 2) {
            __atomic_store_n(&flood.stop, 1, __ATOMIC_RELEASE);
            thread_join(flooding);
            coop_udp_close(flood.socket);
        }
        CHECK(WAIT_FOR(tasks() == before_tasks && files() == before_files, 2000), "no thread or socket remains");
        CoopUdp *reuse = coop_udp_open(port);
        CHECK(reuse != NULL, "port can be bound again");
        coop_udp_close(reuse);
    }
    printf("host-relay: lifetime %s (30 starts/stops)\n", fails ? "FAILED" : "passed");
}
static CoopView process_view(void) { CoopView v; coop_view_get(&v); return v; }
static int step_joined(CoopSess *s) {
    CoopView v;
    coop_sess_step(s);
    coop_sess_view(s, &v);
    return v.state == COOP_JOINED && v.n == 2 && process_view().n == 2;
}
static void host_settings(const char *mode) {
    int before_tasks = tasks(), before_files = files();
    unsigned port = free_port();
    char server[64];
    snprintf(server, sizeof server, "127.0.0.1:%u", port);
    ini_set("coop_host", "1");
    /* (a Windows process cannot hold an empty variable: _putenv_s(name, "") removes it, so there the empty case is the
       unset one and the ini value decides; elsewhere an empty HOS_COOP_HOST is set, and means off) */
#ifdef _WIN32
    int unset = !strcmp(mode, "ini") || !mode[0];
#else
    int unset = !strcmp(mode, "ini");
#endif
    set_env("HOS_COOP_HOST", unset ? NULL : mode);
    CHECK(coop_relay_enabled() == (unset || !env_value_off(mode)), "host switch environment precedence");
    if (!strcmp(mode, "foreign")) {
        set_env("HOS_COOP_HOST", "1");
        snprintf(server, sizeof server, "relay.example:%u", port);
    }
    set_env("HOS_COOP", "1");
    set_env("HOS_COOP_SERVER", server);
    set_env("HOS_COOP_AUTO", "create");
    set_env("HOS_COOP_ROOM", ROOM_SECRET);
    coop_init();
    int enabled = unset || !env_value_off(mode);
    if (!strcmp(mode, "foreign")) {
        CHECK(WAIT_FOR(strstr(process_view().status, "must name this PC") != NULL, 2000), "foreign relay refused clearly");
        CHECK(files() == before_files, "foreign host opens no relay or session socket");
    } else if (!enabled) {
        coop_sleep_ms(200);
        CHECK(process_view().state != COOP_JOINED && !log_count("hosting on this PC"), "off override does not host");
#ifdef __linux__
        CHECK(files() <= before_files + 1, "only session socket, no listener");
#endif
    } else {
        CHECK(WAIT_FOR(process_view().state == COOP_JOINED, 5000), "maker joins its own relay");
        CHECK(process_view().maker == process_view().self, "local session is maker");
#ifdef __linux__
        CHECK(WAIT_FOR(tasks() == before_tasks + 2, 2000) && files() == before_files + 2, "session plus relay: two threads, two sockets");
#endif
        CHECK(log_count("hosting on this PC") == 1 && log_count("internet players need your public address"), "host address guidance logged");
        CHECK(!log_count(ROOM_SECRET) && !log_count("H7K3WM-BCDFGH-234567"), "no secret in host log");
        CoopParams params = {.server = server, .name = "Bob", .client_id = 0xB0B, .on = 1};
        CoopSess *bob = coop_sess_new(&params);
        CHECK(bob != NULL, "second session allocated");
        if (bob) {
            coop_sess_command(bob, COOP_CMD_JOIN, ROOM_SECRET);
            CHECK(WAIT_FOR(step_joined(bob), 5000), "a second session joins the hosted room");
            coop_sess_stop(bob);
            coop_sess_step(bob);
            coop_sess_free(bob);
        }
        CHECK(coop_command(COOP_CMD_LEAVE, ""), "leave queued");
        CHECK(WAIT_FOR(process_view().state == COOP_LEFT && files() == before_files, 3000), "leave stops both sockets");
        CHECK(coop_command(COOP_CMD_CREATE, ROOM_SECRET), "make another room");
        CHECK(WAIT_FOR(process_view().state == COOP_JOINED, 5000), "hosting starts again after leave");
        uint64_t off_at = coop_now_ms();
        CHECK(coop_configure(0, "Maker", server), "co-op off by menu");
        CHECK(coop_now_ms() - off_at < 400, "menu off within 400 ms");
        CHECK(WAIT_FOR(tasks() == before_tasks && files() == before_files, 2000), "menu off joins relay and session");
        CHECK(coop_configure(1, "Maker", server) && coop_command(COOP_CMD_CREATE, ROOM_SECRET), "menu on and Make");
        CHECK(WAIT_FOR(process_view().state == COOP_JOINED, 5000), "menu on hosts again");
    }
    uint64_t started = coop_now_ms();
    coop_shutdown();
    CHECK(coop_now_ms() - started < 400, "process shutdown within 400 ms");
    CHECK(WAIT_FOR(tasks() == before_tasks && files() == before_files, 2000), "process shutdown leaves no relay thread/socket");
    if (fails) printf("%s", log_text ? log_text : "");
    printf("host-relay: settings %s %s\n", mode, fails ? "FAILED" : "passed");
}
int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "--relay") || !strcmp(argv[1], "--signal-child"))) return session_fixture_main(argc, argv);
    coop_lock_init(&log_lock);
    setvbuf(stdout, NULL, _IOLBF, 0);
    thread_join(thread_new(no_work, NULL));
    /* Warm-up above starts the sanitizer helper. Give the OS a chance to remove the joined task's proc entry. */
    coop_sleep_ms(20);
    if (argc == 2 && !strcmp(argv[1], "--drive")) return drive();
    if (argc == 3 && !strcmp(argv[1], "--serve")) return serve((unsigned)atoi(argv[2]));
    if (argc == 2 && !strcmp(argv[1], "--lifetime")) host_lifetime();
    else if (argc == 3 && !strcmp(argv[1], "--settings")) host_settings(argv[2]);
    else if (argc == 2 && !strcmp(argv[1], "--defaults")) {
        CHECK(COOP_RELAY_PACKET_RATE == 480 && COOP_RELAY_PACKET_BURST == 960 && COOP_RELAY_BYTE_RATE == 262144 &&
              COOP_RELAY_BYTE_BURST == 524288, "promised member defaults");
        printf("host-relay: defaults %s; fixed state %u bytes\n", fails ? "FAILED" : "passed", (unsigned)sizeof(CoopRelay));
    } else return 2;
    return fails != 0;
}
