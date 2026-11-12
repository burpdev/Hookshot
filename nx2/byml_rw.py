"""The game's little-endian BYML files ('YB', version 7: the .byml and .bgyml files of its actor packs and tables), read and
written without loss: a file read and written again holds the same nodes of the same types. nx2/coop_bodies.py makes co-op's
actors with it.

A value keeps its exact node type across a round trip:
  str 0xA0   bool 0xD0   S32 0xD1   F32 0xD2   U32 0xD3   S64 0xD4   U64 0xD5   Double 0xD6
Containers: array 0xC0, dict 0xC1. null 0x00 is None.
"""
import struct

class S32(int): byml=0xD1
class U32(int): byml=0xD3
class S64(int): byml=0xD4
class U64(int): byml=0xD5
class F32(float): byml=0xD2
class Double(float): byml=0xD6

NUL = bytes(1)
ZERO4 = bytes(4)

# ---------- read ----------
def read(data):
    assert data[:2] == b"YB", "not LE BYML"
    hko = struct.unpack_from("<I", data, 4)[0]
    sto = struct.unpack_from("<I", data, 8)[0]
    root = struct.unpack_from("<I", data, 12)[0]
    def strtab(off):
        if not off or data[off] != 0xC2: return []
        n = struct.unpack_from("<I", data, off)[0] >> 8
        offs = [struct.unpack_from("<I", data, off+4+i*4)[0] for i in range(n+1)]
        return [data[off+offs[i]:off+offs[i+1]].split(NUL)[0].decode("utf-8") for i in range(n)]
    keys, strs = strtab(hko), strtab(sto)
    def val(t, o):
        v = struct.unpack_from("<I", data, o)[0]
        if t == 0xA0: return strs[v]
        if t in (0xC0, 0xC1): return node(t, v)
        if t == 0xD0: return bool(v)
        if t == 0xD1: return S32(struct.unpack_from("<i", data, o)[0])
        if t == 0xD2: return F32(struct.unpack_from("<f", data, o)[0])
        if t == 0xD3: return U32(v)
        if t in (0xD4, 0xD5, 0xD6):
            q = struct.unpack_from("<Q", data, v)[0]
            if t == 0xD4: return S64(struct.unpack_from("<q", data, v)[0])
            if t == 0xD5: return U64(q)
            return Double(struct.unpack_from("<d", data, v)[0])
        if t == 0x00: return None
        raise ValueError("byml type %#x" % t)
    def node(t, off):
        n = struct.unpack_from("<I", data, off)[0] >> 8
        if t == 0xC0:
            types = data[off+4:off+4+n]; vb = off+4+((n+3)&~3)
            return [val(types[i], vb+i*4) for i in range(n)]
        if t == 0xC1:
            out = {}
            for i in range(n):
                e = off+4+i*8
                ki = struct.unpack_from("<I", data, e)[0] & 0xFFFFFF
                out[keys[ki]] = val(data[e+3], e+4)
            return out
        raise ValueError("container %#x" % t)
    return node(data[root], root)

# ---------- write ----------
def _type_of(v):
    if isinstance(v, bool): return 0xD0
    if isinstance(v, str): return 0xA0
    if isinstance(v, dict): return 0xC1
    if isinstance(v, list): return 0xC0
    if v is None: return 0x00
    for cls in (S32, U32, S64, U64, F32, Double):
        if isinstance(v, cls): return cls.byml
    if isinstance(v, int): return 0xD5 if v > 0x7fffffff or v < -0x80000000 else 0xD1
    if isinstance(v, float): return 0xD2
    raise TypeError(type(v))

def _key(v, memo):
    """What makes two nodes the same node to the writer: type and exact bits."""
    t = _type_of(v)
    if t in (0xC0, 0xC1):
        k = memo.get(id(v))
        if k is None:
            if t == 0xC1: k = (t, tuple(sorted((name, _key(x, memo)) for name, x in v.items())))
            else: k = (t, tuple(_key(x, memo) for x in v))
            memo[id(v)] = k
        return k
    if t == 0xD2: return (t, struct.pack("<f", float(v)))
    if t == 0xD6: return (t, struct.pack("<d", float(v)))
    if t == 0xA0: return (t, v)
    if v is None: return (t, 0)
    return (t, int(v))

def _q(v, t):
    if t == 0xD4: return struct.pack("<q", int(v))
    if t == 0xD5: return struct.pack("<Q", int(v))
    return struct.pack("<d", float(v))

def write(root):
    """Lays the file out the way the game's own files are laid out, so a file read and
    written back unchanged is the same bytes:
        header, key table, string table,
        every 64-bit value once, in the order a depth-first walk first meets it,
        then the containers depth-first, parent before children, identical ones shared.
    The sharing matters: without it a file comes out 10-25% larger than the size the
    game's resource size table allows for it, and the stage loads with pieces missing."""
    keys, strs = set(), set()
    def collect(n):
        if isinstance(n, dict):
            for k, v in n.items(): keys.add(k); collect(v)
        elif isinstance(n, list):
            for v in n: collect(v)
        elif isinstance(n, str): strs.add(n)
    collect(root)
    keys = sorted(keys, key=lambda s: s.encode()); strs = sorted(strs, key=lambda s: s.encode())
    kidx = {k:i for i,k in enumerate(keys)}; sidx = {s:i for i,s in enumerate(strs)}

    def strtab(lst):
        if not lst: return b""
        b = bytearray(); b += struct.pack("<I", (len(lst)<<8)|0xC2)
        data = bytearray(); offs = []
        base = 4 + (len(lst)+1)*4
        for s in lst:
            offs.append(base+len(data)); data += s.encode()+NUL
        offs.append(base+len(data))
        for o in offs: b += struct.pack("<I", o)
        b += data
        while len(b) % 4: b += NUL
        return bytes(b)

    out = bytearray(16)
    hko = len(out) if keys else 0; out += strtab(keys);  _pad(out)
    sto = len(out) if strs else 0; out += strtab(strs);  _pad(out)

    def children(node):
        if isinstance(node, dict): return [v for _, v in sorted(node.items(), key=lambda kv: kidx[kv[0]])]
        return node

    # 64-bit values: once each, in depth-first first-met order, ahead of the containers
    q_at = {}; memo = {}; walked = set()
    def gather(node):
        k = _key(node, memo)
        if k in walked: return
        walked.add(k)
        for v in children(node):
            t = _type_of(v)
            if t in (0xD4, 0xD5, 0xD6):
                bits = _q(v, t)
                if bits not in q_at: q_at[bits] = len(out); out.extend(bits)
            elif t in (0xC0, 0xC1): gather(v)
    gather(root)

    at = {}
    def emit(node):
        k = _key(node, memo)
        if k in at: return at[k]
        off = at[k] = len(out)
        t = k[0]; kids = children(node)
        if t == 0xC1:
            names = sorted(node, key=lambda name: kidx[name])
            out.extend(struct.pack("<I", (len(kids)<<8)|0xC1))
            for name, v in zip(names, kids):
                out.extend(struct.pack("<I", kidx[name])[:3] + bytes([_type_of(v)]) + ZERO4)
            slots = [off+4+i*8+4 for i in range(len(kids))]
        else:
            out.extend(struct.pack("<I", (len(kids)<<8)|0xC0))
            out.extend(bytes(_type_of(v) for v in kids))
            _pad(out)
            vbase = len(out); out.extend(ZERO4*len(kids))
            slots = [vbase+i*4 for i in range(len(kids))]
        for v, pos in zip(kids, slots):
            vt = _type_of(v)
            if vt in (0xC0, 0xC1): struct.pack_into("<I", out, pos, emit(v))
            elif vt in (0xD4, 0xD5, 0xD6): struct.pack_into("<I", out, pos, q_at[_q(v, vt)])
            elif vt == 0xA0: struct.pack_into("<I", out, pos, sidx[v])
            elif vt == 0xD0: struct.pack_into("<I", out, pos, 1 if v else 0)
            elif vt == 0xD1: struct.pack_into("<i", out, pos, int(v))
            elif vt == 0xD2: struct.pack_into("<f", out, pos, float(v))
            elif vt == 0xD3: struct.pack_into("<I", out, pos, int(v))
        return off
    root_off = emit(root)
    struct.pack_into("<2sHIII", out, 0, b"YB", 7, hko, sto, root_off)
    return bytes(out)

def _pad(b):
    while len(b) % 4: b.extend(NUL)
