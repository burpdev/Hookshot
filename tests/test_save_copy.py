"""The copy of the save folder co-op makes before a room's progress reaches the save (host/nn_fs.c hos_save_copy), never
in the middle of a save.

Linux only (the posix layer's file functions, as tests/test_fs_links.py). Builds, from the sources as they are, nn_fs.c's
save lock, its bookkeeping and the copy over host/posix/win32_posix.c's file functions, then on a temporary user folder:

- a writer thread saves as the game does (OpenFile for writing, the file written in pieces, CloseFile, the next file of
  the save a moment later, through the same bookkeeping the save mount's HLEs call) while the co-op thread copies
  again and again: every copy made holds one whole save (each file complete, both of the same save), and the copy
  waits (-1) while a file is open for writing or the save changed less than SAVE_QUIET_MS ago;
- a removal, a new file or a rename in the save folder waits for a copy in progress, and paths outside it do not;
- the newest eight copies are kept, a prefixed one ("first-") is kept whatever their number, and an interrupted copy
  (".partial-") is cleared away;
- nothing saved yet (no save mounted, or its folder missing) is a success with no copy.

    python -I -B tests/test_save_copy.py [--cc gcc]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"windows": "the posix layer's file functions: Linux"}

ROOT = Path(__file__).resolve().parents[1]

# host/nn_fs.c's save copy, in source order
FS_PIECES = ["#define SAVE_QUIET_MS", "static SRWLOCK save_lock", "static char save_root[", "static volatile LONG save_writers;",
             "static volatile LONG64 save_changed_ms;", "static void save_lock_shared(void) {", "static int in_save_root(",
             "static void save_stamp(void)", "static int save_change_begin(", "static void save_change_end(",
             "static int save_writer_open(", "static void save_writer_close(void) {", "static void save_mounted(",
             "static void delete_tree(const char *dir);", "static int copy_tree(", "static int cmp_name(", "static void prune_copies(",
             "int hos_save_copy(const char *folder, const char *prefix, int keep, char *out, size_t cap) {",
             "static void delete_tree(const char *dir) {", "int hos_fs_save_root(char *out, size_t cap) {"]


def piece(text, marker):
    """A #define's line, a function to its closing brace, or a declaration to its ';'."""
    if text.count(marker) != 1:
        raise SystemExit("FAIL: %r is not once in the source" % marker)
    start = text.index(marker)
    if marker.startswith("#define"):
        return text[start:text.index("\n", start)]
    brace, semi = text.find("{", start), text.find(";", start)
    head = text[start:brace] if brace != -1 else ""
    function = brace != -1 and (semi == -1 or brace < semi) and ")" in head and "=" not in head
    end, depth = start, 0
    while True:
        ch = text[end]
        depth += (ch == "{") - (ch == "}")
        end += 1
        if (function and ch == "}" and depth == 0) or (not function and ch == ";" and depth == 0):
            return text[start:end]


FIXTURE = r'''
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s (Win32 error %lu)\n", __LINE__, #x, (unsigned long)GetLastError()); exit(1); } } while (0)
enum { FILE_BYTES = 1 << 16, PIECES = 16, BIG_FILES = 128, BIG_BYTES = 1 << 19 };
static char root[PATH_MAX], user[PATH_MAX], save[PATH_MAX];
static int stop;                                         /* (the fixture's own flags: __atomic) */
static unsigned saves_done, slow_saves, deletes_in_save, deletes_outside;
static int64_t now_us(void) { return hos_now_ns() / 1000; }

/* a file of the save as host_path names it: the mount's root (as MountSaveData made it, hos_user_dir's / kept) and the
   rest, every / then made \ */
static void host_of(const char *rel, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", save, rel);
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
}

/* one file of save number gen: FILE_BYTES bytes of the 4-byte gen, written in PIECES pieces pause_us apart, as the game
   writes through OpenFile / WriteFile / CloseFile (the bookkeeping their HLEs call) */
static void write_file(const char *rel, uint32_t gen, unsigned pause_us) {
    char path[PATH_MAX], fs_path[PATH_MAX];
    host_of(rel, path, sizeof path);
    const int in_save = save_writer_open(path);
    CHECK(in_save);
    slashes(fs_path, sizeof fs_path, path);
    FILE *f = fopen(fs_path, "r+b");
    if (!f) f = fopen(fs_path, "wb");
    CHECK(f);
    uint32_t buf[FILE_BYTES / 4 / PIECES];
    for (size_t i = 0; i < sizeof buf / 4; i++) buf[i] = gen;
    for (int p = 0; p < PIECES; p++) {
        CHECK(fwrite(buf, sizeof buf, 1, f) == 1);
        fflush(f);
        usleep(pause_us);
    }
    fclose(f);
    if (in_save) save_writer_close();
}

static unsigned seed = 12345;
static unsigned rnd(unsigned n) { seed = seed * 1103515245u + 12345u; return (seed >> 8) % n; }

static void *writer(void *arg) {
    (void)arg;
    for (uint32_t gen = 1001; !__atomic_load_n(&stop, __ATOMIC_ACQUIRE); gen++) {   /* (after the first save, 1000) */
        /* now and then a file written more slowly than the quiet window (a slow disk): only the open file holds the copy */
        const int slow = rnd(4) == 0;
        write_file("slot_00/progress.sav", gen, slow ? 50000 : 150);
        if (slow) __atomic_add_fetch(&slow_saves, 1, __ATOMIC_ACQ_REL);
        usleep(1000 * (5 + rnd(40)));          /* the next file of the same save, soon after */
        write_file("slot_00/caption.sav", gen, 150);
        __atomic_store_n(&saves_done, gen - 1000, __ATOMIC_RELEASE);
        const unsigned gap = rnd(3) ? 50 + rnd(250) : 650 + rnd(500);   /* mostly saves close together, now and then a pause */
        usleep(1000 * gap);
    }
    return NULL;
}

/* a copy holds one whole save: both files complete and of one save number */
static uint32_t check_copy(const char *dir) {
    uint32_t gen = 0;
    static const char *const names[] = {"slot_00/progress.sav", "slot_00/caption.sav"};
    for (int k = 0; k < 2; k++) {
        char p[PATH_MAX + 64];
        snprintf(p, sizeof p, "%s/%s", dir, names[k]);
        FILE *f = fopen(p, "rb");
        if (!f) { fprintf(stderr, "FAIL: %s is not in the copy\n", p); exit(1); }
        static uint32_t buf[FILE_BYTES / 4 + 1];
        const size_t n = fread(buf, 4, FILE_BYTES / 4 + 1, f);
        fclose(f);
        if (n != FILE_BYTES / 4) { fprintf(stderr, "FAIL: %s holds %zu words, not a whole file\n", p, n); exit(1); }
        for (size_t i = 0; i < n; i++)
            if (buf[i] != buf[0]) { fprintf(stderr, "FAIL: %s mixes saves %u and %u\n", p, buf[0], buf[i]); exit(1); }
        if (k && buf[0] != gen) { fprintf(stderr, "FAIL: %s holds save %u, the other file save %u\n", dir, buf[0], gen); exit(1); }
        gen = buf[0];
    }
    return gen;
}

/* the folders in parent: those whose name starts with prefix, or with a digit (digits), dot-folders included; their
   names in order into names when given */
static int list_dirs(const char *parent, const char *prefix, int digits, char names[][96], int cap) {
    DIR *d = opendir(parent);
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (digits ? !(e->d_name[0] >= '0' && e->d_name[0] <= '9') : strncmp(e->d_name, prefix, strlen(prefix))) continue;
        if (names && n < cap) snprintf(names[n], 96, "%s", e->d_name);
        n++;
    }
    if (d) closedir(d);
    if (names) qsort(names, (size_t)(n < cap ? n : cap), 96, (int (*)(const void *, const void *))strcmp);
    return n;
}

/* a removal in (or outside) the save folder, as DeleteFile's HLE makes it: it must wait for a copy in progress */
static void *deleter(void *arg) {
    const char *path = arg;
    const int held = save_change_begin(path);
    if (held) __atomic_add_fetch(&deletes_in_save, 1, __ATOMIC_ACQ_REL);
    else __atomic_add_fetch(&deletes_outside, 1, __ATOMIC_ACQ_REL);
    save_change_end(held);
    return NULL;
}

/* the game saving while a large copy is under way: from the moment the copy's .partial- folder appears, how long the
   save's OpenFile waits (it must wait for the whole copy) */
static char partial_parent[PATH_MAX];
static int64_t seen_us, opened_us;
static void *saver_during_copy(void *arg) {
    (void)arg;
    char names[4][96];
    for (int i = 0; i < 200000 && !list_dirs(partial_parent, ".partial-", 0, names, 4); i++) usleep(50);
    __atomic_store_n(&seen_us, now_us(), __ATOMIC_RELEASE);
    char path[PATH_MAX];
    host_of("slot_00/progress.sav", path, sizeof path);
    const int in_save = save_writer_open(path);
    __atomic_store_n(&opened_us, now_us(), __ATOMIC_RELEASE);
    if (in_save) save_writer_close();
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    snprintf(root, sizeof root, "%s", argv[1]);
    snprintf(user, sizeof user, "%s/user", root);
    /* (MountSaveData's root: hos_user_dir, then \save\<uid>; host_path then names files with every / made \) */
    snprintf(save, sizeof save, "%s\\save\\5a3c9e1f0b7d24680000000000000001", user);
    hos_user_dir = user;
    char out[PATH_MAX], cmd[3 * PATH_MAX], parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s/coop-backup", user);

    /* nothing saved yet: success, no copy */
    CHECK(hos_save_copy("coop-backup", "first-", 8, out, sizeof out) == 1 && !out[0]);
    CHECK(hos_fs_save_root(out, sizeof out) == 0);   /* (the control bridge's: none before the mount) */
    save_mounted(save);
    /* (one save_root, kept by save_mounted, is the copy's and the control bridge's) */
    CHECK(hos_fs_save_root(out, sizeof out) == 1 && !strcmp(out, save));
    CHECK(hos_save_copy("coop-backup", "first-", 8, out, sizeof out) == 1 && !out[0]);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/save/5a3c9e1f0b7d24680000000000000001/slot_00'", user);
    CHECK(system(cmd) == 0);

    /* the first save, then the first copy (kept out of the rotation) */
    write_file("slot_00/progress.sav", 1000, 150), write_file("slot_00/caption.sav", 1000, 150);
    CHECK(hos_save_copy("coop-backup", "first-", 8, out, sizeof out) == -1);   /* changed a moment ago */
    usleep(1000 * (SAVE_QUIET_MS + 50));
    CHECK(hos_save_copy("coop-backup", "first-", 8, out, sizeof out) == 1 && strstr(out, "/coop-backup/first-"));
    CHECK(check_copy(out) == 1000);
    char first[PATH_MAX];
    snprintf(first, sizeof first, "%s", out);

    /* a writer racing the copies */
    pthread_t w;
    CHECK(pthread_create(&w, NULL, writer, NULL) == 0);
    int copies = 0, waits = 0;
    uint32_t last = 0;
    const time_t until = time(NULL) + 9;
    while (time(NULL) < until) {
        const int r = hos_save_copy("coop-backup", NULL, 8, out, sizeof out);
        if (r < 0) { waits++; usleep(5000); continue; }
        CHECK(r == 1 && out[0]);
        const uint32_t gen = check_copy(out);
        CHECK(gen >= last);
        last = gen, copies++;
        usleep(20000);
    }
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    pthread_join(w, NULL);
    printf("%d copies during %u saves (%u written slower than the quiet window), each one whole save; the copy waited %d times\n",
           copies, saves_done, slow_saves, waits);
    CHECK(copies >= 4 && waits >= 20 && saves_done >= 6 && slow_saves >= 1);

    /* the newest eight kept, in the order they were made (many in one second), the first copy too, nothing partial left */
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/.partial-20000101-000000Z/slot_00'", parent);
    CHECK(system(cmd) == 0);
    usleep(1000 * (SAVE_QUIET_MS + 50));
    char made[30][96], kept[16][96];
    for (int i = 0; i < 30; i++) {
        CHECK(hos_save_copy("coop-backup", NULL, 8, out, sizeof out) == 1);
        check_copy(out);
        snprintf(made[i], sizeof made[i], "%s", strrchr(out, '/') + 1);
    }
    CHECK(list_dirs(parent, "", 1, kept, 16) == 8);
    for (int i = 0; i < 8; i++)
        if (strcmp(kept[i], made[22 + i])) { fprintf(stderr, "FAIL: kept %s where the copy made %s was expected\n", kept[i], made[22 + i]); exit(1); }
    CHECK(list_dirs(parent, "first-", 0, NULL, 0) == 1 && GetFileAttributesA(first) != INVALID_FILE_ATTRIBUTES);
    CHECK(list_dirs(parent, ".partial-", 0, NULL, 0) == 0);

    /* the game saving while a copy is under way waits for the whole copy (a large save: BIG_FILES files) */
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/save/5a3c9e1f0b7d24680000000000000001/slot_09' && cd '%s/save/5a3c9e1f0b7d24680000000000000001/slot_09' "
             "&& for i in $(seq %d); do head -c %d /dev/zero > big_$i.bin; done", user, user, (int)BIG_FILES, (int)BIG_BYTES);
    CHECK(system(cmd) == 0);
    usleep(1000 * (SAVE_QUIET_MS + 50));
    snprintf(partial_parent, sizeof partial_parent, "%s", parent);
    pthread_t s;
    CHECK(pthread_create(&s, NULL, saver_during_copy, NULL) == 0);
    usleep(20000);
    const int64_t t0 = now_us();
    CHECK(hos_save_copy("coop-backup", NULL, 8, out, sizeof out) == 1);
    const int64_t t1 = now_us();
    pthread_join(s, NULL);
    const int64_t seen = __atomic_load_n(&seen_us, __ATOMIC_ACQUIRE), opened = __atomic_load_n(&opened_us, __ATOMIC_ACQUIRE);
    printf("a copy of %d MB took %.1f ms; a save that began %.1f ms into it waited %.1f ms\n", (int)(((int64_t)BIG_FILES * BIG_BYTES) >> 20),
           (t1 - t0) / 1e3, (seen - t0) / 1e3, (opened - seen) / 1e3);
    CHECK(t1 - t0 >= 5000 && seen < t1);                 /* (a copy long enough to tell, and seen under way) */
    CHECK(opened - seen >= (t1 - seen) / 2);            /* the save waited for (most of) the rest of the copy */

    /* a change in the save folder waits for a copy in progress (the copy holds the lock alone); one outside does not */
    char in_path[PATH_MAX], out_path[PATH_MAX];
    host_of("slot_00/x.tmp", in_path, sizeof in_path);
    snprintf(out_path, sizeof out_path, "%s\\temp\\x.tmp", user);
    for (char *p = out_path; *p; p++) if (*p == '/') *p = '\\';
    AcquireSRWLockExclusive(&save_lock);   /* (as hos_save_copy holds it while it copies) */
    pthread_t d1, d2;
    CHECK(pthread_create(&d1, NULL, deleter, in_path) == 0 && pthread_create(&d2, NULL, deleter, out_path) == 0);
    usleep(200000);
    const unsigned during = __atomic_load_n(&deletes_in_save, __ATOMIC_ACQUIRE);
    ReleaseSRWLockExclusive(&save_lock);
    pthread_join(d1, NULL), pthread_join(d2, NULL);
    CHECK(during == 0 && deletes_in_save == 1 && deletes_outside == 1);
    char other[PATH_MAX];
    snprintf(other, sizeof other, "%s-other\\x", save);   /* (a folder whose name starts like the save's is not in it) */
    CHECK(!save_change_begin(other));
    printf("PASS: every copy holds one whole save; a save waits for a copy under way; eight kept in order, the first kept, partials cleared\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    if os.name == "nt" or sys.platform == "darwin":
        print("SKIP: the posix layer's checks run on Linux")
        return 0
    posix = (ROOT / "host/posix/win32_posix.c").read_text(encoding="utf-8")
    fs = (ROOT / "host/nn_fs.c").read_text(encoding="utf-8")
    pieces = [
        piece(posix, "DWORD GetLastError(void) {"),
        piece(posix, "static DWORD error_of(int e) {"),
        "static BOOL fail_errno(void) { last_error = error_of(errno); return FALSE; }",
        "enum { H_THREAD = 0x48540001, H_EVENT, H_SEMA, H_TIMER, H_FILE, H_FIND };",
        "typedef struct { int type; } HObj;",
        "static int is(HANDLE h, int type) { return h && h != INVALID_HANDLE_VALUE && (uintptr_t)h > 0x10000 && ((HObj *)h)->type == type; }",
        piece(posix, "static void slashes(char *dst, size_t cap, const char *src) {"),
        piece(posix, "DWORD GetFileAttributesA(LPCSTR name) {"),
        piece(posix, "BOOL CreateDirectoryA(LPCSTR name, void *attr) {"),
        piece(posix, "BOOL RemoveDirectoryA(LPCSTR name) {"),
        piece(posix, "BOOL DeleteFileA(LPCSTR name) {"),
        piece(posix, "BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {"),
        piece(posix, "BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {"),
        "typedef struct { HObj h; DIR *d; char dir[PATH_MAX], pat[256]; } HFind;",
        piece(posix, "static BOOL find_next(HFind *f, WIN32_FIND_DATAA *fd) {"),
        piece(posix, "HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd) {"),
        piece(posix, "BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) {"),
        piece(posix, "BOOL FindClose(HANDLE h) {"),
    ] + [piece(fs, m) for m in FS_PIECES]
    head = r'''#define _GNU_SOURCE
#include <windows.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static __thread DWORD last_error;
#define HOS_FS_PATH 1024   /* (host/nn_fs.c's path buffers) */
static const char *hos_user_dir;
static int hos_vcore_away(void) { return 0; }
static void hos_vcore_back(void) {}
static int64_t hos_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }
'''
    with tempfile.TemporaryDirectory(prefix="save-copy-") as tmp:
        tmp = Path(tmp)
        src = tmp / "fixture.c"
        src.write_text(head + "\n".join(pieces) + "\n" + FIXTURE, encoding="utf-8")
        exe = tmp / "fixture"
        r = subprocess.run([a.cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
                            "-Wno-format-truncation", "-fsanitize=thread", "-I", str(ROOT / "host/posix"), str(src), "-o", str(exe), "-lpthread"],
                           capture_output=True, text=True)
        if r.returncode:   # (no ThreadSanitizer with this compiler: without it)
            r = subprocess.run([a.cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                                "-Wno-unused-parameter", "-Wno-format-truncation", "-I", str(ROOT / "host/posix"), str(src), "-o", str(exe), "-lpthread"],
                               capture_output=True, text=True)
            if r.returncode:
                print(r.stdout + r.stderr)
                print("FAIL: the fixture does not compile")
                return 1
            print("(without ThreadSanitizer)")
        work = tmp / "work"
        work.mkdir()
        r = subprocess.run([str(exe), str(work)], capture_output=True, text=True, timeout=120)
        sys.stdout.write(r.stdout)
        if r.returncode:
            sys.stdout.write(r.stderr)
            print("FAIL: the save copy fixture exited %d" % r.returncode)
            return 1
        if "WARNING: ThreadSanitizer" in r.stderr:
            sys.stdout.write(r.stderr)
            print("FAIL: ThreadSanitizer found a data race")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
