"""Co-op (host/oot_coop.c): which of the game's save flags the players share.

Reads the game's own flag list (romfs/GameData/GameDataList.Product.*.byml.zs) in a built game folder and writes
<game folder>/coop/flags.bin: the name hashes (u32, little-endian, sorted) of the flags co-op keeps in step between players.
Without the file co-op still shares items; flags are each player's own.

A flag is shared when it is lasting progress:
  * a Bool, kept in the save file itself (SaveFileIndex 0),
  * with ResetTypeValue 8: the plain "stays until a new game" kind. The other kinds are reset by the game on a map change, a
    new day, a retry and so on: where a character stands, a mini-game's state,
  * and not one of the names below (where the names are known: those the game's code, image.bin, and its event files spell
    out): which age Link is, what the game is loading, mini-game and dream-world state, the options, Navi's hints. Those are
    each player's own.

    python -I nx2/coop_flags.py <game folder>

Needs zstandard. The output is made from the player's own game files and stays in the game folder: never commit or share it.
"""
import sys, os, re, glob, struct
import zstandard as zstd

if len(sys.argv) < 2: sys.exit("coop_flags.py <game folder>")
GAME = sys.argv[1]
ROMFS = os.path.join(GAME, "romfs")
OWN = ("Sequence_", "MiniGame", "Minigame", "UI_", "OptionData", "DreamWorld", "DreamBossStage", "DebugParam",
       "NaviHint", "NaviChat", "ActionGuide", "HavePlayedNaviBalloonMessage", "IsSkipRetrySequence", "IsLastSelected")

def sarc(buf):
    assert buf[:4] == b"SARC"
    data_off = struct.unpack_from("<I", buf, 0xC)[0]
    count = struct.unpack_from("<H", buf, 0x1A)[0]
    names = 0x20 + count * 0x10 + 8
    out = {}
    for i in range(count):
        _, attr, start, end = struct.unpack_from("<IIII", buf, 0x20 + i * 0x10)
        s = names + (attr & 0xFFFFFF) * 4
        out[buf[s:buf.index(b"\0", s)].decode()] = buf[data_off + start:data_off + end]
    return out

def mm3(b):
    c1, c2, h, n = 0xcc9e2d51, 0x1b873593, 0, len(b) & ~3
    rot = lambda v, r: ((v << r) | (v >> (32 - r))) & 0xffffffff
    for i in range(0, n, 4):
        k = struct.unpack_from("<I", b, i)[0]
        k = (rot((k * c1) & 0xffffffff, 15) * c2) & 0xffffffff
        h = (rot(h ^ k, 13) * 5 + 0xe6546b64) & 0xffffffff
    k = 0
    for j in range(len(b) & 3): k |= b[n + j] << (8 * j)
    if len(b) & 3: h ^= (rot((k * c1) & 0xffffffff, 15) * c2) & 0xffffffff
    h ^= len(b); h ^= h >> 16; h = (h * 0x85ebca6b) & 0xffffffff; h ^= h >> 13; h = (h * 0xc2b2ae35) & 0xffffffff
    return h ^ (h >> 16)

def byml_be(d):
    """The list is big-endian BYML; only what it uses."""
    u32 = lambda o: struct.unpack_from(">I", d, o)[0]
    def strtab(off):
        n = u32(off) & 0xFFFFFF
        offs = [u32(off + 4 + i * 4) for i in range(n + 1)]
        return [d[off + offs[i]:off + offs[i + 1]].split(b"\0")[0].decode("utf-8", "replace") for i in range(n)]
    keys, strs = strtab(u32(4)), (strtab(u32(8)) if u32(8) else [])
    def value(t, o):
        v = u32(o)
        if t == 0xA0: return strs[v]
        if t in (0xC0, 0xC1): return node(v)
        if t == 0xD0: return bool(v)
        if t == 0xD1: return struct.unpack_from(">i", d, o)[0]
        if t == 0xD3: return v
        if t in (0xD4, 0xD5): return struct.unpack_from(">Q", d, v)[0]
        return None
    def node(off):
        t, n = d[off], u32(off) & 0xFFFFFF
        if t == 0xC0:
            base = off + 4 + ((n + 3) & ~3)
            return [value(d[off + 4 + i], base + i * 4) for i in range(n)]
        if t == 0xC1:
            return {keys[u32(off + 4 + i * 8) >> 8]: value(d[off + 7 + i * 8], off + 8 + i * 8) for i in range(n)}
        return None
    return node(u32(12))

dicts = sarc(zstd.ZstdDecompressor().decompress(open(os.path.join(ROMFS, "Pack", "ZsDic.pack.zs"), "rb").read()))
dec = zstd.ZstdDecompressor(dict_data=zstd.ZstdCompressionDict(dicts["zs.zsdic"]))
lists = glob.glob(os.path.join(ROMFS, "GameData", "GameDataList.Product.*.byml.zs"))
assert lists, "no GameDataList in " + ROMFS
bools = byml_be(dec.decompress(open(lists[0], "rb").read(), max_output_size=1 << 27))["Data"]["Bool"]
lasting = {b["Hash"] for b in bools if b["SaveFileIndex"] == 0 and b["ResetTypeValue"] == 8}

# the names the game's code and its event scripts spell out, to leave each player's own flags alone
own = set()
prefixes = tuple(p.encode() for p in OWN)
pattern = re.compile(rb"(?:" + b"|".join(re.escape(p) for p in prefixes) + rb")[A-Za-z0-9_.]{0,90}")
def feed(buf):
    for m in pattern.finditer(buf):
        h = mm3(m.group())
        if h in lasting: own.add(h)
feed(open(os.path.join(GAME, "image.bin"), "rb").read())
packs = {k: zstd.ZstdDecompressor(dict_data=zstd.ZstdCompressionDict(v)) for k, v in dicts.items()}
for sub in ("Event", "Sequence", "Logic", "AI", "Component", "GameParameter", "Pack"):
    for path in glob.glob(os.path.join(ROMFS, sub, "**", "*"), recursive=True):
        if not os.path.isfile(path) or os.path.getsize(path) > (64 << 20): continue
        raw = open(path, "rb").read()
        if path.endswith(".zs"):
            for key in (("pack.zsdic",) if path.endswith(".pack.zs") else ()) + ("zs.zsdic",):
                try: raw = packs[key].decompress(raw, max_output_size=1 << 28); break
                except zstd.ZstdError: pass
        feed(raw)
shared = sorted(lasting - own)
os.makedirs(os.path.join(GAME, "coop"), exist_ok=True)
out = os.path.join(GAME, "coop", "flags.bin")
open(out, "wb").write(struct.pack("<%dI" % len(shared), *shared))
print("%d of the game's %d flags are lasting progress; %d of those are each player's own; %d shared -> %s"
      % (len(lasting), len(bools), len(own), len(shared), out))
