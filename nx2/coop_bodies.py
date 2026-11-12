"""Co-op (host/oot_coop.c): the actors another player is drawn as.

The game has a second Link: PouchPlayer_Child / PouchPlayer_Adult, the Link of the pause menu. It is the right model, but its
animation graph has menu poses only, so as another player it stands stiff and slides about. To move as a player moves the body
needs Link's own animation graph (host/oot_coop.c, "how another player moves": the same commands in the same slots, the same
inputs). Changing PouchPlayer itself would break the pause menu's figure, so this makes actors of its own beside it:

    CoopPlayer_Child, CoopPlayer_Adult   PouchPlayer_* with ASRef -> AS/Player_*.root.asb, Link's ASInfo (how many slots and
                                         results the graph needs), his sound and effect users, and a sword and a shield
    CoopFoe_Child, CoopFoe_Adult         the same body as something the game's combat can hit (PvP): a life, the hit
                                         component, an enemy's side, lock-on, and one sensor on the body, read from Dark Link's
                                         files, a Link-shaped enemy

A new actor is three things, all made here from the player's own game files into <game folder>/mods/00-coop, a mod as
host/mods.c loads them:
  * its pack                      romfs/Pack/Actor/Coop*.pack.zs
  * a row in the actor tables     romfs/RSDB/ActorInfo.*  and  GameActorInfo.*  (copies of PouchPlayer's rows)
  * entries in the size table     romfs/System/Resource/ResourceSizeTable.* : the game drops a file larger than its entry, and
                                  a file with no entry at all. The mod's table is the game's own with these entries added
                                  (host/prompts.c merges the button prompts' font entry into a mod's table by itself).
Without the mod co-op draws the other players as PouchPlayer, and there is no PvP. Another mod that carries the same tables
and sorts after 00-coop wins over them: run sizes() on its table then, or the bodies are dropped.

    python -I nx2/coop_bodies.py <game folder>

Needs zstandard. Everything written is made from the player's own game files and stays in the game folder: never commit or
share it.
"""
import sys, os, glob, struct, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (byml_rw.py beside this file, under python -I too)
MOD = os.path.join("mods", "00-coop")
TABLE = os.path.join("romfs", "System", "Resource")

# ---- SARC, as the game's actor packs have it ----
def nhash(name, key=0x65):
    v = 0
    for ch in name.encode(): v = (v * key + ch) & 0xFFFFFFFF
    return v

def sarc_read(buf):
    assert buf[:4] == b"SARC", "not a pack"
    data_off = struct.unpack_from("<I", buf, 0xC)[0]
    count = struct.unpack_from("<H", buf, 0x1A)[0]
    names_off = 0x20 + count * 0x10 + 8
    files = {}
    for i in range(count):
        _h, attr, s, e = struct.unpack_from("<IIII", buf, 0x20 + i * 0x10)
        o = names_off + (attr & 0xFFFFFF) * 4
        files[buf[o:buf.index(b"\0", o)].decode()] = bytes(buf[data_off + s:data_off + e])
    return files

def sarc_write(files):
    names = sorted(files, key=nhash)
    blob, offs = b"", []
    for n in names:
        offs.append(len(blob) // 4); blob += n.encode() + b"\0"; blob += b"\0" * (-len(blob) % 4)
    align = lambda n: 8 if n.endswith((".bgyml", ".byml")) else 16
    head = 0x14 + 0xC + 0x10 * len(names) + 8 + len(blob)
    data_off = (head + 15) // 16 * 16
    nodes, data = b"", b""
    for n, o in zip(names, offs):
        data += b"\0" * (-(data_off + len(data)) % align(n))
        s = len(data); data += files[n]
        nodes += struct.pack("<IIII", nhash(n), 0x01000000 | o, s, len(data))
    out = b"SARC" + struct.pack("<HHIIHH", 0x14, 0xFEFF, data_off + len(data), data_off, 0x100, 0)
    out += b"SFAT" + struct.pack("<HHI", 0xC, len(names), 0x65) + nodes + b"SFNT" + struct.pack("<HH", 8, 0) + blob
    return out + b"\0" * (data_off - len(out)) + data

# (what a body carries: the child's and the adult's sword and shield)
GEAR = {"Child": ("Weapon_Sword_Kokiri", "Weapon_Shield_Deku"), "Adult": ("Weapon_Sword_Master", "Weapon_Shield_Hylian")}

# ---- the size table ----
def table_get(tab, key):
    ncrc = struct.unpack_from("<I", tab, 14)[0]
    h = zlib.crc32(key.encode()) & 0xFFFFFFFF
    lo, hi = 0, ncrc
    while lo < hi:
        m = (lo + hi) // 2
        if struct.unpack_from("<I", tab, 0x16 + m * 8)[0] < h: lo = m + 1
        else: hi = m
    found = lo < ncrc and struct.unpack_from("<I", tab, 0x16 + lo * 8)[0] == h
    return (0x16 + lo * 8, struct.unpack_from("<I", tab, 0x16 + lo * 8 + 4)[0] if found else None)

def table_set(tab, key, size):
    """the entry for key is at least size (made if there is none); True when the table changed"""
    at, have = table_get(tab, key)
    if have is not None:
        if have >= size: return False
        struct.pack_into("<I", tab, at + 4, size)
        return True
    tab[at:at] = struct.pack("<II", zlib.crc32(key.encode()) & 0xFFFFFFFF, size)
    struct.pack_into("<I", tab, 14, struct.unpack_from("<I", tab, 14)[0] + 1)
    return True

def reader(game):
    """(unz, the dictionaries): unz reads one of the game's .zs files, with the dictionary it was packed with or with none"""
    import zstandard as zstd
    dics = sarc_read(zstd.ZstdDecompressor().decompressobj().decompress(open(os.path.join(game, "romfs", "Pack", "ZsDic.pack.zs"), "rb").read()))
    def unz(raw):
        for name in ("pack.zsdic", "zs.zsdic", None):
            try: return zstd.ZstdDecompressor(dict_data=zstd.ZstdCompressionDict(dics[name]) if name else None).decompressobj().decompress(raw)
            except Exception: pass
        raise SystemExit("could not read a game file (another game version?)")
    return unz, dics

def sizes(game, table=None):
    """the bodies' entries (mods/00-coop/sizes.txt, written by main), into the mod's size table: the game's own table with them
    added; or into table, another mod's that is served in its place. Says what it did"""
    import zstandard as zstd
    want_file = os.path.join(game, MOD, "sizes.txt")
    if not os.path.exists(want_file): return "no co-op bodies made yet: nothing to add"
    want = [l.split("\t") for l in open(want_file, encoding="utf-8").read().splitlines() if "\t" in l]
    unz, _ = reader(game)
    read = lambda p: bytearray(unz(open(p, "rb").read()))
    def put(path, tab):
        assert tab[:6] == b"RESTBL", "not a size table"
        changed = [key for key, size in want if table_set(tab, key, int(size))]
        if changed or not os.path.exists(path):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "wb").write(zstd.ZstdCompressor(level=12).compress(bytes(tab)))
            assert read(path) == tab
        return len(changed)
    if table: return "%d size entries in %s (%d new or raised)" % (len(want), table, put(table, read(table)))
    game_tab = glob.glob(os.path.join(game, TABLE, "ResourceSizeTable.*.rsizetable.zs"))
    if len(game_tab) != 1: raise SystemExit("size table: expected one ResourceSizeTable file in the game, found %d" % len(game_tab))
    own = os.path.join(game, MOD, TABLE, os.path.basename(game_tab[0]))
    return "%d size entries in %s (%d new or raised)" % (len(want), os.path.relpath(own, game), put(own, read(game_tab[0])))

def main(game):
    import zstandard as zstd
    import byml_rw
    romfs = os.path.join(game, "romfs")
    unz, dics = reader(game)
    out = os.path.join(game, MOD, "romfs")
    game_tab = glob.glob(os.path.join(game, TABLE, "ResourceSizeTable.*.rsizetable.zs"))
    tab = bytearray(unz(open(game_tab[0], "rb").read()))
    want = []
    for age in ("Child", "Adult"):
        old, new = "PouchPlayer_" + age, "CoopPlayer_" + age
        raw = unz(open(os.path.join(romfs, "Pack", "Actor", old + ".pack.zs"), "rb").read())
        files = sarc_read(raw)
        link = sarc_read(unz(open(os.path.join(romfs, "Pack", "Actor", "Player_" + age + ".pack.zs"), "rb").read()))
        key = "Actor/%s.engine__actor__ActorParam.bgyml"
        param = byml_rw.read(files.pop(key % old))
        info = "Component/ASInfo/Player_%s.engine__component__ASInfo.bgyml" % age
        if info not in link: info = "Component/ASInfo/Player.engine__component__ASInfo.bgyml"   # (the adult uses the shared one)
        param["Components"]["ASRef"] = "?AS/Player_%s.root.asb" % age
        param["Components"]["ASInfoRef"] = "?" + info
        for n in link:
            if n.startswith("Component/ASInfo/"): files.setdefault(n, link[n])
        # Link's sound and effect users (XLink: what an animation's events emit). With the pause menu's own
        # ("ActorBasic") the body was silent: no voice, no swing, nothing when it was hit.
        theirs = dict(byml_rw.read(link["Actor/Player_Base.engine__actor__ActorParam.bgyml"])["Components"])
        theirs.update(byml_rw.read(link["Actor/Player_%s.engine__actor__ActorParam.bgyml" % age])["Components"])
        def take(name):                                  # (a file of Link's pack, and the ones it is made from)
            if name not in link or name in files: return
            files[name] = link[name]
            made_of = byml_rw.read(link[name])
            parent = made_of.get("$parent") if isinstance(made_of, dict) else None
            if parent: take(parent.replace("Work/", "", 1).replace(".gyml", ".bgyml"))
        for part in ("XLinkRef", "ELinkRef", "SLinkRef"):
            if not theirs.get(part): continue
            param["Components"][part] = theirs[part]
            take(theirs[part].lstrip("?"))
        # A sword and a shield. The pause menu's figure has an equipment user, but nothing in its files says what it
        # holds (the menu's own code hands it that). A character that is not the player is told in its files: the
        # equipment user's blackboard table names the actors (EquipmentUser_Weapon, EquipmentUser_Shield), and the
        # user's own rows say they do not come from the pouch, that there is a sheath, and the state they start in.
        # Dark Link is such a character: his rows are read from the game's own files and used here as they are.
        dark = sarc_read(unz(open(os.path.join(romfs, "Pack", "Actor", "Enemy_MiddleBoss_DarkLink.pack.zs"), "rb").read()))
        dark_user = byml_rw.read(dark["Component/EquipmentUserParam/Enemy_MiddleBoss_DarkLink.game__component__EquipmentUserParam.bgyml"])
        dark_table = byml_rw.read(dark["Component/Blackboard/BlackboardParamTable/Enemy_MiddleBoss_DarkLink_EquipmentUser.engine__component__BlackboardParamTable.bgyml"])
        row = next(r for r in dark_table["BlackboardParamStringArray"] if r["BBKey"] == "EquipmentUser_Weapon")
        sword, shield = GEAR[age]
        work = lambda name: "Work/" + name[:-len(".bgyml")] + ".gyml"       # (how one file names another it is made from)
        user_old = param["Components"]["EquipmentUserRef"].lstrip("?")
        table_ref, at = None, user_old
        while at in files and table_ref is None:           # (the table the figure's equipment user reads: its own, or one it is made from)
            made_of = byml_rw.read(files[at])
            table_ref = made_of.get("BlackboardTableRef")
            parent = made_of.get("$parent")
            at = parent.replace("Work/", "", 1).replace(".gyml", ".bgyml") if parent else None
        if table_ref is None: raise SystemExit("the pause menu figure's equipment table was not found (another game version?)")
        table_new = "Component/Blackboard/BlackboardParamTable/%s.engine__component__BlackboardParamTable.bgyml" % new
        user_new = "Component/EquipmentUserParam/%s.game__component__EquipmentUserParam.bgyml" % new
        named = lambda bbkey, actor: dict(row, BBKey=bbkey, InitValConverted=actor)
        files[table_new] = byml_rw.write({"$parent": table_ref, "BlackboardParamStringArray": [
            named("EquipmentUser_Weapon", sword), named("EquipmentUser_Shield", shield), named("EquipmentUser_Bow", "")]})
        user = {"$parent": work(user_old), "BlackboardTableRef": work(table_new)}
        for k in ("IsPouchUser", "IsSheathUser", "ShieldInitEquipState", "WeaponInitEquipState"): user[k] = dark_user[k]
        files[user_new] = byml_rw.write(user)
        assert byml_rw.read(files[user_new]) == user
        param["Components"]["EquipmentUserRef"] = "?" + user_new
        files[key % new] = byml_rw.write(param)
        assert byml_rw.read(files[key % new]) == param
        pack = sarc_write(files)
        assert sarc_read(pack) == files
        dst = os.path.join(out, "Pack", "Actor", new + ".pack.zs")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(zstd.ZstdCompressor(dict_data=zstd.ZstdCompressionDict(dics["pack.zsdic"]), level=12).compress(pack))
        assert unz(open(dst, "rb").read()) == pack
        room = (table_get(tab, "Pack/Actor/%s.pack" % old)[1] or len(raw) + 512) + len(pack) - len(raw) + 4096
        want.append(("Pack/Actor/%s.pack" % new, room))
        want.append((key % new, (table_get(tab, key % old)[1] or 8192) + 1024))
        # (the two files of the sword and the shield: a file with no entry is dropped, and the body then has none)
        want.append((user_new, (table_get(tab, user_old)[1] or 4096) + 2048))
        want.append((table_new, (table_get(tab, table_ref.replace("Work/", "", 1).replace(".gyml", ".bgyml"))[1] or 4096) + 2048))
        # ---- the same body as a target: CoopFoe_* (PvP). What makes a character one the game's combat can hit is read
        # from Dark Link's files, a Link-shaped enemy: a life, the hit component, the side it is on (an enemy's, so the
        # player's weapons count), being a target and being locked on to; and one sensor on the body that takes the
        # hits. The figure keeps its own physics otherwise: with Dark Link's whole physics the body had a character
        # controller of its own, walked off and was not where its player was.
        foe = "CoopFoe_" + age
        ffiles = {n: v for n, v in files.items() if n != key % new}
        fparam = byml_rw.read(files[key % new])
        dcomp = {}
        def dup(name):
            if name not in dark: return
            made_of = byml_rw.read(dark[name])
            if made_of.get("$parent"): dup(made_of["$parent"].replace("Work/", "", 1).replace(".gyml", ".bgyml"))
            dcomp.update(made_of.get("Components", {}))
        dup("Actor/Enemy_MiddleBoss_DarkLink.engine__actor__ActorParam.bgyml")
        def named(v):                                    # (the files a value names)
            if isinstance(v, str):
                yield v.lstrip("?")
                if v.startswith("Work/"): yield v[5:].replace(".gyml", ".bgyml")
            elif isinstance(v, dict):
                for x in v.values(): yield from named(x)
            elif isinstance(v, list):
                for x in v: yield from named(x)
        def ftake(name):                                 # (a file of Dark Link's pack, and all it names)
            if name not in dark or name in ffiles: return
            ffiles[name] = dark[name]
            if name.endswith(".bgyml"):
                try: inside = byml_rw.read(dark[name])
                except Exception: return
                for n in named(inside): ftake(n)
        for part in ("LifeRef", "SensorHitRef", "AffiliationRef", "TargetRef", "AttentionSenderRef"):
            if part not in dcomp: raise SystemExit("Dark Link has no %s (another game version?)" % part)
            fparam["Components"][part] = dcomp[part]
            for n in named(dcomp[part]): ftake(n)
        # What is seen and heard when a hit lands (the flash, the sparks, the sound) is an enemy's Reaction part: the
        # game's shared one, as a Deku Baba has it (Dark Link's own is a guarding metal one). Where lock-on and the
        # camera look at him, and being perceived as an enemy, are Dark Link's.
        fparam["Components"]["ReactionRef"] = "?Component/Reaction/Default.engine__component__ReactionParam.bgyml"
        # Lock-on: Dark Link's own asks his blackboard whether the fight has him in lock-on range, which nothing sets on
        # a body, so no body could be locked on to. The ordinary enemy's (his is made from it: its files came along).
        common = "Component/AttentionSenderParam/EnemyCommon.game__component__AttentionSenderParam.bgyml"
        if common in ffiles: fparam["Components"]["AttentionSenderRef"] = "?" + common
        for part in ("ActorPositionCalculatorRef", "PerceptionEmitterRef"):
            if part not in dcomp: continue
            fparam["Components"][part] = dcomp[part]
            for n in named(dcomp[part]): ftake(n)
        phys_old = fparam["Components"]["PhysicsRef"].lstrip("?")
        set_old = byml_rw.read(ffiles[phys_old])["ControllerSetPath"]
        sensor = "Phive/RigidBodyControllerSensorParam/Enemy_MiddleBoss_DarkLink.phive__RigidBodyControllerSensorParam.bgyml"
        hit_body = "Phive/RigidBodySensorParam/Enemy_MiddleBoss_DarkLink_Tgt_Body.phive__RigidBodySensorParam.bgyml"
        shape = "Phive/ShapeParam/Player_Tgt_Body.phive__ShapeParam.bgyml"
        receipt = "Phive/ContactCollectionSensorParam/EnemyReceipt.phive__ContactCollectionSensorParam.bgyml"
        for n in (sensor, hit_body, shape, receipt): ftake(n)
        cset = "Phive/ControllerSetParam/%s.phive__ControllerSetParam.bgyml" % foe
        phys = "Component/Physics/%s.engine__component__PhysicsParam.bgyml" % foe
        ffiles[cset] = byml_rw.write({"$parent": set_old,
            "AliasSensorList": [{"AliasSensor": "Tgt", "ControllerName": "Tgt", "RigidBodyName": "Body_Tgt"}],
            "CColSensorNamePathAry": [{"FilePath": work(receipt), "Name": "Tgt"}],
            "ControllerSensorNamePathAry": [{"FilePath": work(sensor), "Name": "Tgt"}],
            "RigidBodySensorNamePathAry": [{"FilePath": work(hit_body), "Name": "Body_Tgt"}],
            "ShapeNamePathAry": [{"FilePath": work(shape), "Name": "Player_Tgt_Body"}]})
        ffiles[phys] = byml_rw.write({"$parent": work(phys_old), "ControllerSetPath": work(cset)})
        fparam["Components"]["PhysicsRef"] = "?" + phys
        ffiles[key % foe] = byml_rw.write(fparam)
        fpack = sarc_write(ffiles)
        assert sarc_read(fpack) == ffiles
        dst = os.path.join(out, "Pack", "Actor", foe + ".pack.zs")
        open(dst, "wb").write(zstd.ZstdCompressor(dict_data=zstd.ZstdCompressionDict(dics["pack.zsdic"]), level=12).compress(fpack))
        assert unz(open(dst, "rb").read()) == fpack
        want.append(("Pack/Actor/%s.pack" % foe, room + len(fpack) - len(pack) + 65536))
        want.append((key % foe, (table_get(tab, key % old)[1] or 8192) + 8192))
        want.append((cset, 8192)); want.append((phys, 8192))
    for table in ("ActorInfo", "GameActorInfo"):
        src = glob.glob(os.path.join(romfs, "RSDB", table + ".Product.*.rstbl.byml.zs"))
        if len(src) != 1: raise SystemExit("actor table: expected one %s file, found %d" % (table, len(src)))
        raw = unz(open(src[0], "rb").read())
        rows = byml_rw.read(raw)
        names = [r.get("__RowId") for r in rows]
        for age, kind in (("Child", "CoopPlayer_"), ("Adult", "CoopPlayer_"), ("Child", "CoopFoe_"), ("Adult", "CoopFoe_")):
            old, new = "PouchPlayer_" + age, kind + age
            if new in names or old not in names: continue
            row = dict(rows[names.index(old)]); row["__RowId"] = new
            at = len([n for n in names if n < new])     # (the rows are in the names' order)
            rows.insert(at, row); names.insert(at, new)
        made = byml_rw.write(rows)
        assert byml_rw.read(made) == rows
        dst = os.path.join(out, "RSDB", os.path.basename(src[0]))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(zstd.ZstdCompressor(level=12).compress(made))
        key = "RSDB/" + os.path.basename(src[0])[:-3]
        want.append((key, (table_get(tab, key)[1] or len(raw) + 4096) + len(made) - len(raw) + 4096))
    os.makedirs(os.path.join(game, MOD), exist_ok=True)
    open(os.path.join(game, MOD, "sizes.txt"), "w", encoding="utf-8").write("".join("%s\t%d\n" % w for w in want))
    return "the other players' bodies made (CoopPlayer_ and CoopFoe_, Child and Adult); " + sizes(game)

if __name__ == "__main__":
    if len(sys.argv) < 2: sys.exit("coop_bodies.py <game folder>")
    print(main(sys.argv[1]))
