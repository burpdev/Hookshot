"""The same hostile-client/fake-clock assertions against Python and the hosted C relay, over real loopback UDP.

The C fixture supplies time and a virtual source address separately; every request and every reply still crosses a
real UDP socket. Production start/stop and settings run separately, with exact thread/socket counts, ASan/UBSan,
then TSan where available. The existing two-session scenarios (including restart, loss, kicks and a killed client)
run unchanged against the C relay. No game or GPU is used.

Multi-room fairness/capacity, Python's command-line options and optional address logging are Python-only. The host
has exactly one pre-authorized locator/maker, and a fixed ban table that closes admission instead of forgetting bans.
These intentional differences have dedicated assertions below. --source-root checks another source tree with this
fixture; --mutations proves the new behavioral assertions reject disabled protection, not just missing files.
"""
import argparse
import concurrent.futures
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import types
import unittest

RUN_ALL_ARGS = ["--cc", "gcc"]
ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


shared = load("host_relay_shared", ROOT / "tests/test_coop_relay.py")
session = load("host_relay_session", ROOT / "tests/test_coop_session.py")
cr = shared.cr
PythonRelay = cr.Relay

# These assertions require more than the one room a maker's game can host, or Python's operator logging switch.
PYTHON_ONLY = {
    "test_server_full", "test_joins_per_network_are_limited", "test_rooms_per_network_are_limited",
    "test_one_ipv6_48_opens_no_more_rooms_than_one_network",
    "test_the_rejoins_of_rooms_someone_made_to_flood_with_leave_other_rooms_their_share",
    "test_a_flood_of_rejoins_with_a_members_token_on_a_full_relay_keeps_neither_its_messages_nor_its_own_rejoin_out",
    "test_a_paced_flood_with_every_token_of_a_room_keeps_no_members_own_rejoin_out_also_on_a_full_relay",
    "test_one_room_cannot_take_the_forwarding_budget_of_another",
    "test_idle_rooms_do_not_shrink_the_forwarding_share_of_a_busy_one",
    "test_rooms_that_forward_to_nobody_do_not_shrink_the_share_of_a_busy_one",
    "test_a_room_its_maker_left_alone_closes_at_once",  # second half creates an unrelated locator; covered below
    "test_log_has_no_secret_locator_or_address",  # second half enables Python-only log_addrs; host log tested in C
    "test_member_limit_command_line_options",  # operator CLI; retained in the complete Python relay test
}


def address(text):
    if text.startswith("["):
        ip, port = text[1:].split("]:")
    else:
        ip, port = text.rsplit(":", 1)
    return ip, int(port)


class SnapshotRoom(types.SimpleNamespace):
    def live(self):
        return [m for m in self.members.values() if m.live]


class CRelay:
    """Bridge to the actual C state, never a second implementation of a relay decision."""
    def __init__(self, exe, **options):
        self.p = subprocess.Popen([str(exe), "--drive"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        line = self.p.stdout.readline()
        if not line.startswith("listening "):
            error = self.p.stderr.read()
            self.p.wait()
            raise RuntimeError("C relay did not start: " + line + error)
        self.port = int(line.split()[1])
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(5)
        self.addresses = {}
        self.log_fn = options.get("log")
        self.authorized = False
        self.snapshot = {}
        keys = ("pkt_rate", "pkt_burst", "byte_rate", "byte_burst", "join_rate", "join_burst", "challenge_rate",
                "prefix_challenge_rate", "prefix_challenge_burst", "egress_rate", "rejoin_challenge_rate")
        defaults = (480, None, 262144, None, 1 / 3, 20, 5000, 20, 40, 8 << 20, 1000)
        values = [options.get(k, d) for k, d in zip(keys, defaults)]
        for burst in (1, 3):
            if values[burst] is None:
                values[burst] = 2 * values[burst - 1]
        self.limits = tuple(values[:4])  # exactly the limits supplied to the C state below
        self.call("N " + " ".join(map(str, values)))

    def close(self):
        self.p.stdin.write("Q\n")
        self.p.stdin.flush()
        self.p.stdin.close()
        code = self.p.wait(timeout=5)
        errors = self.p.stderr.read()
        self.p.stdout.close()
        self.p.stderr.close()
        self.socket.close()
        if code:
            raise AssertionError("C relay fixture exited %d: %s" % (code, errors))

    def call(self, command, data=None):
        self.p.stdin.write(command + "\n")
        self.p.stdin.flush()
        if data is not None:
            self.socket.sendto(data, ("127.0.0.1", self.port))
        out = []
        self.last_cookie = None
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise AssertionError("C relay ended: " + self.p.stderr.read())
            if line == "END\n":
                break
            kind, value = line.rstrip("\n").split(" ", 1)
            if kind == "O":
                target, size = value.split()
                raw = self.socket.recvfrom(65536)[0]
                assert len(raw) == int(size)
                parsed_addr = address(target)
                out.append((self.addresses.get(parsed_addr, parsed_addr), raw))
            elif kind == "S":
                self.snapshot = json.loads(value)
            elif kind == "L" and self.log_fn:
                for message in bytes.fromhex(value).decode().splitlines():
                    self.log_fn(message)
            elif kind == "C":
                self.last_cookie = bytes.fromhex(value)
        return out

    def authorize(self, client):
        self.call("A %x %s %d %s" % (client.cid, client.addr[0], client.addr[1], client.key.hex()))
        self.authorized = True

    def handle(self, data, addr, now):
        self.addresses[(str(ipaddress.ip_address(addr[0])), addr[1])] = addr
        dg = cr.parse(data)
        # Test setup, not production auto-authorization: the first valid CREATE tells the fixture which room its
        # imaginary local Make action authorized. Production passes this before starting the listener.
        if not self.authorized and dg and dg.ver == cr.PROTO and dg.type == cr.T_CONNECT:
            conn = cr.parse_connect(dg.payload)
            if conn and conn[0] == cr.INTENT_CREATE:
                self.call("A %x %s %d %s" % (conn[3], addr[0], addr[1], conn[2].hex()))
                self.authorized = True
        return self.call("H %.9f %s %d" % (now, addr[0], addr[1]), data)

    def tick(self, now):
        return self.call("T %.9f" % now)

    def cookie(self, addr, minute):
        self.call("C %d %s %d" % (minute, addr[0], addr[1]))
        return self.last_cookie

    @property
    def stats(self):
        return self.snapshot["stats"]

    @property
    def rooms(self):
        if not self.snapshot["room"]:
            return {}
        members = {}
        for row in self.snapshot["members"]:
            row = dict(row)
            row["session"] = int(row["session"], 16)
            row["addr"] = self.addresses.get(address(row["addr"]), address(row["addr"]))
            members[row["id"]] = types.SimpleNamespace(**row)
        room = SnapshotRoom(members=members)
        return {self.snapshot["room"]: room}

    @property
    def locators(self):
        return {bytes.fromhex(self.snapshot["locator"]): None} if self.snapshot["room"] else {}

    @property
    def join_buckets(self):
        return range(self.snapshot["netjoins"])

    @property
    def challenge_buckets(self):
        return range(self.snapshot["netchallenges"])


class HostOnly(unittest.TestCase):
    factory = None

    def setUp(self):
        shared.Client._next_port = 40000
        self.relay = self.factory()
        self.addCleanup(self.relay.close)

    def test_only_the_local_maker_can_create_the_fixed_room(self):
        a = shared.Client("A")
        self.relay.authorize(a)
        wrong = shared.Client("other", secret=a.secret)
        self.assertEqual(wrong.join(self.relay, 0, cr.INTENT_CREATE)[0][0], cr.E_SERVER_FULL)
        self.assertEqual(self.relay.rooms, {})
        self.assertEqual(a.join(self.relay, 1, cr.INTENT_CREATE)[0][0], 1)
        other_room = shared.Client("other room")
        self.assertEqual(other_room.join(self.relay, 2, cr.INTENT_CREATE)[0][0], cr.E_SERVER_FULL)
        self.assertEqual(other_room.join(self.relay, 3, cr.INTENT_JOIN)[0][0], cr.E_NO_ROOM)
        self.assertEqual(len(self.relay.rooms), 1)

    def test_a_spoofed_local_client_id_cannot_create(self):
        a = shared.Client("A", cid=17)
        self.relay.authorize(a)
        thief = shared.Client("thief", cid=17, secret=a.secret)
        self.assertEqual(thief.join(self.relay, 0, cr.INTENT_CREATE)[0][0], cr.E_SERVER_FULL)
        self.assertEqual(self.relay.rooms, {})

    def test_bans_have_a_bound_without_forgetting_removed_players(self):
        a = shared.Client("A")
        a.join(self.relay, 0, cr.INTENT_CREATE)
        b = shared.Client("B", secret=a.secret)
        b.join(self.relay, 0)
        a.send(self.relay, cr.T_KICK, cr.pack_kick(b.id, b.session), now=1)
        self.relay.call("B")
        self.assertEqual(b.join(self.relay, 2)[0][0], cr.E_REMOVED)
        c = shared.Client("C", secret=a.secret)
        refused, _ = c.join(self.relay, 2)
        self.assertEqual(len(refused), 3, "an ERROR, not a JOINED whose member id happens to equal ROOM_FULL")
        self.assertEqual(refused[0], cr.E_ROOM_FULL)
        self.assertEqual(a.join(self.relay, 3)[0][0], 1)  # an existing member still rejoins

    def test_the_unshared_room_closes_and_only_its_maker_can_recreate_it(self):
        a = shared.Client("A")
        a.join(self.relay, 0, cr.INTENT_CREATE)
        a.send(self.relay, cr.T_LEAVE, now=1)
        self.assertEqual(self.relay.rooms, {})
        a.session += 1
        self.assertEqual(a.join(self.relay, 2, cr.INTENT_CREATE)[0][0], 1)
        self.relay.tick(18)
        self.relay.tick(79)
        self.assertEqual(self.relay.rooms, {})

    def test_packet_copies_not_just_bytes_are_charged(self):
        a = shared.Client("A")
        a.join(self.relay, 0, cr.INTENT_CREATE)
        for i in range(7):
            shared.Client(str(i), secret=a.secret, ip="10.9.%d.1" % i).join(self.relay, 0)
        forwarded = sum(len(a.send(self.relay, cr.T_DATA, b"x", dst=cr.DST_ALL, now=0)[0]) for _ in range(200))
        self.assertEqual(forwarded, 7 * (960 // 7))

    def test_cookie_is_bound_to_ip_port_and_previous_minute_only(self):
        a = shared.Client("A")
        self.relay.authorize(a)
        cookie = self.relay.cookie(a.addr, 0)
        for addr, now in ((("10.99.0.1", a.addr[1]), 0), ((a.addr[0], a.addr[1] + 1), 0), (a.addr, 120)):
            out = self.relay.handle(a.connect(cr.INTENT_CREATE, cookie), addr, now)
            self.assertEqual(cr.parse(out[0][1]).type, cr.T_CHALLENGE)
        self.assertEqual(cr.parse(self.relay.handle(a.connect(cr.INTENT_CREATE, cookie), a.addr, 119)[0][1]).type, cr.T_JOINED)

    def test_every_datagram_length_and_truncation_is_checked(self):
        a = shared.Client("A")
        base = a.connect(cr.INTENT_CREATE)
        for size in range(len(base)):
            self.assertEqual(self.relay.handle(base[:size], a.addr, 0), [])
        for size in (1201, 2048, 65507):
            self.assertEqual(self.relay.handle(base + bytes(size - len(base)), a.addr, 0), [])
        self.assertEqual(self.relay.rooms, {})

    def test_history_and_network_storage_are_bounded(self):
        # Existing spoof-flood tests exercise the table's normal bound; this also fills the session ring and refusal
        # ring through real handshakes, with enough time for each network's join and challenge budgets to refill.
        a = shared.Client("A")
        a.join(self.relay, 0, cr.INTENT_CREATE)
        for i in range(4100):
            a.session += 1
            a.join(self.relay, (i + 1) * 3)
        self.assertEqual(self.relay.snapshot["sessions"], 4096)
        missing = shared.Client("missing")
        for i in range(4100):
            self.assertEqual(missing.join(self.relay, 13000 + i * 3)[0][0], cr.E_NO_ROOM)
        self.assertEqual(self.relay.snapshot["failed"], 4096)
        self.relay.tick(26000)
        self.assertEqual(self.relay.snapshot["failed"], 0)

    def test_refused_joins_still_pay_the_network_budget(self):
        results = [shared.Client(str(i)).join(self.relay, 0)[0][0] for i in range(22)]
        self.assertEqual(results, [cr.E_NO_ROOM] * 20 + [cr.E_RATE] * 2)
        self.assertEqual(shared.Client("another network", ip="10.1.0.1").join(self.relay, 0)[0][0], cr.E_NO_ROOM)
        self.assertEqual(shared.Client("refilled").join(self.relay, 3)[0][0], cr.E_NO_ROOM)

    def test_a_changed_refusal_replaces_the_attempts_old_reason(self):
        # Same client/address/sequence/locator, but a different intent changes the refusal. Python remembers only
        # the latest reason: switching back must pay again, not resurrect an old free answer.
        for relay in (PythonRelay(), self.relay):
            a = shared.Client("A")
            a.join(relay, 0, cr.INTENT_CREATE)
            b = shared.Client("B", secret=a.secret, ip="10.5.0.1")
            b.session = a.session
            before = relay.stats["join_charges"]
            self.assertEqual(b.join(relay, 0)[0][0], cr.E_SESSION_USED)
            for intent, reason in ((cr.INTENT_CREATE, cr.E_ROOM_EXISTS), (cr.INTENT_JOIN, cr.E_SESSION_USED)):
                out = relay.handle(b.connect(intent, relay.cookie(b.addr, 0)), b.addr, 0)
                self.assertEqual(cr.parse_error(cr.parse(out[0][1]).payload)[0], reason)
            self.assertEqual(relay.stats["join_charges"] - before, 3)


def compile_fixture(cc, folder, source, tsan=False, relay_text=None, coop_text=None):
    flags = ["-fsanitize=thread"] if tsan else ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
    libs = ["-pthread"]
    if os.name == "nt":
        if tsan:
            return None
        flags, libs = ["-D__USE_MINGW_ANSI_STDIO=1", "-D_CRT_SECURE_NO_WARNINGS"], ["-lws2_32", "-lbcrypt", "-liphlpapi"]
    exe = folder / ("host_relay_tsan.exe" if tsan else "host_relay.exe")
    mono = folder / ("mono_tsan.o" if tsan else "mono.o")
    relay = source / "host/coop_relay.c"
    coop = source / "host/coop.c"
    if relay_text is not None:
        relay = folder / "mutated_relay.c"
        relay.write_text(relay_text.replace('"../gpu/vk/compat/hookshot_env.h"', '"%s"' % (source / "gpu/vk/compat/hookshot_env.h").as_posix()))
    if coop_text is not None:
        coop = folder / "mutated_coop.c"
        coop.write_text(coop_text.replace('"../gpu/vk/compat/hookshot_env.h"', '"%s"' % (source / "gpu/vk/compat/hookshot_env.h").as_posix()))
    fixture = (ROOT / "tests/coop_host_relay_test.c").read_text()
    fixture = fixture.replace('#include "coop_session_test.c"', '#include "%s"' % (ROOT / "tests/coop_session_test.c").as_posix())
    fixture = fixture.replace('#include "../host/coop_relay.c"', '#include "%s"' % relay.as_posix())
    path = folder / "fixture.c"
    path.write_text(fixture)
    commands = [
        [cc, "-std=c99", "-O2", "-w", "-c", str(source / "third_party/monocypher/monocypher.c"), "-o", str(mono)] + flags,
        [cc, "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-I" + str(source / "host"),
         str(path), str(coop), str(source / "host/coop_net.c"), str(source / "host/coop_proto.c"), str(mono), "-o", str(exe)] + flags + libs]
    for command in commands:
        result = subprocess.run(command, capture_output=True, text=True, timeout=300)
        if result.returncode:
            if tsan:
                print("SKIP: compiler cannot link ThreadSanitizer: " + result.stderr)
                return None
            raise AssertionError("fixture compilation failed:\n" + result.stdout + result.stderr)
    print("compiled %s %s" % (cc, " ".join(flags)), flush=True)
    return exe


def run_shared(exe):
    selected = sorted(set(unittest.defaultTestLoader.getTestCaseNames(shared.RelayTest)) - PYTHON_ONLY)
    class PythonShared(shared.RelayTest):
        def setUp(self):
            shared.Client._next_port = 40000
            super().setUp()

    suite = unittest.TestSuite(PythonShared(n) for n in selected)
    print("Python relay: %d unchanged shared cases" % len(selected), flush=True)
    if not unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful():
        return False

    class HostShared(shared.RelayTest):
        def setUp(self):
            shared.Client._next_port = 40000
            original = cr.Relay
            instances = []

            def factory(**kwargs):
                relay = CRelay(exe, **kwargs)
                instances.append(relay)
                return relay

            cr.Relay = factory
            self.addCleanup(setattr, cr, "Relay", original)
            self.addCleanup(lambda: [relay.close() for relay in instances])
            super().setUp()

    print("C relay: the same %d unchanged cases, real UDP carrier" % len(selected), flush=True)
    result = unittest.TextTestRunner(verbosity=1).run(unittest.TestSuite(HostShared(n) for n in selected))
    HostOnly.factory = staticmethod(lambda: CRelay(exe))
    extra = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(HostOnly))
    print("Python-only cases: " + ", ".join(sorted(PYTHON_ONLY)), flush=True)
    return result.wasSuccessful() and extra.wasSuccessful()


def run_lifetime(exe, tsan=False):
    modes = [["--lifetime"], ["--settings", "ini"], ["--settings", "1"], ["--settings", "0"],
             ["--settings", "false"], ["--settings", "off"], ["--settings", "no"], ["--settings", ""], ["--settings", "foreign"]]
    for mode in modes:
        result = subprocess.run([str(exe)] + mode, capture_output=True, text=True, timeout=45)
        text = result.stdout + result.stderr
        if tsan and result.returncode and ("unexpected memory mapping" in text or "ADDR_NO_RANDOMIZE" in text):
            print("SKIP: ThreadSanitizer cannot start on this kernel: " + text)
            return True
        print(text, end="", flush=True)
        if result.returncode:
            return False
    return True


def run_sessions(exe, folder):
    class HostProc(session.RelayProc):
        def start(self):
            self.p = subprocess.Popen([str(exe), "--serve", str(self.port)], stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, text=True)
            line = self.p.stdout.readline()
            self.log.append(line.rstrip())
            if not line.startswith("listening "):
                raise AssertionError("C relay failed to listen: " + line)
            import threading
            self.reader = threading.Thread(target=self._read, args=(self.p.stdout,), daemon=True)
            self.reader.start()

    original = session.RelayProc
    session.RelayProc = HostProc
    try:
        works = [folder / ("session%d" % drop) for drop in (0, 30)]
        for work in works:
            work.mkdir()
        with concurrent.futures.ThreadPoolExecutor(2) as pool:
            jobs = [pool.submit(session.run_mode, exe, "C relay %d%% loss" % drop, None, drop, work)
                    for drop, work in zip((0, 30), works)]
            results = [job.result() for job in jobs]
        for _, _, text in results:
            print(text, flush=True)
        return all(ok for _, ok, _ in results)
    finally:
        session.RelayProc = original


def mutations(cc, folder, source):
    relay = (source / "host/coop_relay.c").read_text()
    coop = (source / "host/coop.c").read_text()
    cases = [
        ("foreign room", "if (!same || c->client_id != r->maker || !coop_addr_eq(addr, &r->maker_addr)) return COOP_E_SERVER_FULL;", "if (0) return COOP_E_SERVER_FULL;", "test_only_the_local_maker_can_create_the_fixed_room"),
        ("source proof", "!coop_addr_eq(addr, &r->maker_addr)", "0", "test_a_spoofed_local_client_id_cannot_create"),
        ("packet copies", "relay_take(&m->packets, targets - 1, now)", "relay_take(&m->packets, 0, now)", "test_packet_copies_not_just_bytes_are_charged"),
        ("cookie expiry", "(int64_t)(now / 60)", "(int64_t)(now * 0)", "test_cookie_is_bound_to_ip_port_and_previous_minute_only"),
        ("ban bound", "if (!rejoin && r->banned_count == RELAY_HISTORY) return COOP_E_ROOM_FULL;", "if (0) return COOP_E_ROOM_FULL;", "test_bans_have_a_bound_without_forgetting_removed_players"),
        ("empty room", "if (!r->shared) { relay_delete_room(r); return; }", "if (0) { relay_delete_room(r); return; }", "test_the_unshared_room_closes_and_only_its_maker_can_recreate_it"),
        ("history bound", "RELAY_HISTORY = 4096", "RELAY_HISTORY = 2048", "test_history_and_network_storage_are_bounded"),
        ("malformed silence", "if (!coop_dgram_parse(data, n, &h, &p)) { r->stats[R_MALFORMED]++; return; }",
         "if (!coop_dgram_parse(data, n, &h, &p)) { relay_error(r, addr, COOP_E_PROTO, 0, NULL, 0, 0); return; }", "test_every_datagram_length_and_truncation_is_checked"),
        ("join budget", "!relay_take(budget, 1, now)", "!relay_take(budget, 0, now)", "test_refused_joins_still_pay_the_network_budget"),
    ]
    for label, old, new, case in cases:
        assert old in relay
        sub = folder / label.replace(" ", "_")
        sub.mkdir()
        exe = compile_fixture(cc, sub, source, relay_text=relay.replace(old, new))
        HostOnly.factory = staticmethod(lambda: CRelay(exe))
        result = unittest.TextTestRunner(verbosity=1).run(unittest.TestSuite([HostOnly(case)]))
        if result.wasSuccessful():
            raise AssertionError("mutation not detected: " + label)
        print("CAUGHT mutation: " + label, flush=True)
    sub = folder / "host_call"
    sub.mkdir()
    old = "cmd == COOP_CMD_CREATE && s->process_session && coop_relay_enabled()"
    assert old in coop
    exe = compile_fixture(cc, sub, source, coop_text=coop.replace(old, "0 && coop_relay_enabled()"))
    result = subprocess.run([str(exe), "--settings", "1"], capture_output=True, text=True, timeout=45)
    print(result.stdout + result.stderr)
    assert result.returncode and "maker joins its own relay" in result.stderr
    print("CAUGHT mutation: process host start call", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default=os.environ.get("CC") or shutil.which("gcc") or "clang")
    ap.add_argument("--source-root", type=Path, default=ROOT)
    ap.add_argument("--only", choices=("shared", "lifetime", "sessions"))
    ap.add_argument("--mutations", action="store_true")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="hookshot-host-relay-") as tmp:
        folder = Path(tmp)
        if args.mutations:
            mutations(args.cc, folder, args.source_root)
            return 0
        exe = compile_fixture(args.cc, folder, args.source_root)
        subprocess.run([str(exe), "--defaults"], check=True, timeout=10)
        ok = True
        if args.only in (None, "shared"):
            ok = run_shared(exe) and ok
        if args.only in (None, "lifetime"):
            ok = run_lifetime(exe) and ok
            tsan = compile_fixture(args.cc, folder, args.source_root, tsan=True)
            if tsan:
                ok = run_lifetime(tsan, tsan=True) and ok
        if args.only in (None, "sessions"):
            ok = run_sessions(exe, folder) and ok
    print("%s: host relay %s" % ("PASS" if ok else "FAIL", args.only or "shared wire, bounded state, lifecycle/settings and session cases"))
    return not ok


if __name__ == "__main__":
    sys.exit(main())
