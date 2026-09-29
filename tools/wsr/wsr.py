"""Reader for Wormsign match recordings (.wsr), for bug reports. Standard library only.

  python wsr.py info <file.wsr>              header, chunk list, counts, setup and notes
  python wsr.py ticks <file.wsr> [from] [to] one line per tick: tick engine mods c0..c5 rng inputs
  python wsr.py inputs <file.wsr>            one line per recorded input
  python wsr.py diff <a.wsr> <b.wsr>         first tick whose hashes differ, and which parts
  python wsr.py energy <file.wsr>            every worm energy change in the detail records, with the position
"""
import json
import struct
import sys
import zlib

COMPS = ["time+rng", "turn", "worms", "tasks", "projectiles", "teams"]
SEND_TYPES = ["msg", "int", "int2", "float", "float2", "string"]


class Wsr:
    def __init__(self, path):
        d = open(path, "rb").read()
        if d[:4] != b"WSR1":
            raise ValueError("not a .wsr file")
        self.chunks, pos, index_at = [], 4, None
        while pos + 20 <= len(d):
            typ, flags, raw_len, stored_len, crc = struct.unpack_from("<4sIIII", d, pos)
            body = d[pos + 20:pos + 20 + stored_len]
            if flags & ~1 or len(body) != stored_len or zlib.crc32(body) != crc:
                break
            try:
                payload = zlib.decompress(body, -15) if flags & 1 else body
            except zlib.error:
                break
            if len(payload) != raw_len:
                break
            self.chunks.append((typ.decode("ascii", "replace"), pos, payload))
            pos += 20 + stored_len
            if typ == b"INDX":
                index_at = self.chunks[-1][1]
                break
        self.complete = (index_at is not None and pos + 12 == len(d) and
                         struct.unpack_from("<Q", d, pos)[0] == index_at and d[pos + 8:pos + 12] == b"WSRE")
        self.head = self.json("HEAD")[0] if self.of("HEAD") else {}

    def of(self, typ):
        return [p for t, _, p in self.chunks if t == typ]

    def json(self, typ):
        out = []
        for p in self.of(typ):
            try:
                out.append(json.loads(p))
            except ValueError:
                out.append({"raw": len(p)})
        return out

    def ticks(self):
        for p in self.of("TICK"):
            if len(p) < 4:
                continue
            tick, = struct.unpack_from("<I", p, 0)
            at = 4
            while at < len(p):
                kind = p[at]
                at += 1
                if kind == 0 and at + 76 <= len(p):
                    v = struct.unpack_from("<QQ6QIIHH", p, at)
                    yield {"tick": tick, "engine": v[0], "mods": v[1], "c": list(v[2:8]), "rng": v[8], "rng2": v[9],
                           "fpucw": v[10], "inputs": v[11]}
                    tick += 1
                    at += 76
                elif kind == 1 and at + 4 <= len(p):
                    tick += struct.unpack_from("<I", p, at)[0]
                    at += 4
                else:
                    break

    def inputs(self):
        for p in self.of("INPT"):
            at = 0
            while at + 24 <= len(p):
                typ, mid, a, b, time, call_t, caller, n = struct.unpack_from("<BHIIIIIB", p, at)
                s = p[at + 24:at + 24 + n].decode("utf-8", "replace")
                yield {"type": typ, "id": mid, "a": a, "b": b, "time": time, "callT": call_t, "caller": caller, "str": s}
                at += 24 + n

    def details(self):
        """Yields (tick, worms) per DETL record; worms maps slot -> {pos, energy, energySrc, weapon}."""
        for typ, _, p in self.chunks:
            if typ != "DETL":
                continue
            prev, at = b"", 0
            while at + 3 <= len(p):
                kind, n = p[at], struct.unpack_from("<H", p, at + 1)[0]
                at += 3
                if kind == 0:
                    cur = p[at:at + n]
                    at += n
                else:
                    buf, i = bytearray(prev), 0
                    while i < n:
                        same, diff = struct.unpack_from("<HH", p, at)
                        at += 4
                        i += same
                        for k in range(diff):
                            buf[i + k] ^= p[at + k]
                        at += diff
                        i += diff
                    cur = bytes(buf)
                prev = cur
                yield unpack_detail(cur)

    def fixed(self, typ, size):
        return sum(len(p) // size for p in self.of(typ))


def unpack_detail(r):
    tick = struct.unpack_from("<I", r, 0)[0]
    n, at, worms = r[20], 21, {}
    for _ in range(n):
        slot, w = r[at], r[at + 1:at + 94]
        worms[slot & 0x7f] = {"pos": struct.unpack_from("<3f", w, 24), "energySrc": struct.unpack_from("<H", w, 75)[0],
                              "weapon": struct.unpack_from("<i", w, 81)[0], "energy": struct.unpack_from("<H", w, 89)[0]}
        at += 94
    return tick, worms


def info(path):
    w = Wsr(path)
    print(f"{path}: {'complete' if w.complete else 'INCOMPLETE'}, {len(w.chunks)} chunks")
    print("HEAD", json.dumps(w.head))
    counts = {}
    for t, _, p in w.chunks:
        c = counts.setdefault(t, [0, 0])
        c[0] += 1
        c[1] += len(p)
    print("chunks", " ".join(f"{t}={n}/{b}B" for t, (n, b) in counts.items()))
    ticks = list(w.ticks())
    ins = list(w.inputs())
    print(f"ticks {len(ticks)} ({ticks[0]['tick'] if ticks else '-'}..{ticks[-1]['tick'] if ticks else '-'}), "
          f"inputs {len(ins)} ({sum(1 for i in ins if i['type'] == 5)} string), seeds {w.fixed('SEED', 13)}, "
          f"pre-match draws {w.fixed('PDRW', 13)}, remote inputs {w.fixed('RMTI', 14)}, dispatches {w.fixed('DISP', 6)}")
    for s in w.json("SETP"):
        print("SETP", json.dumps(s))
    for s in w.json("DVRG"):
        print("DVRG", json.dumps({k: v for k, v in s.items() if k != "detailLocal"}))
    for s in w.json("ENGV"):
        print("ENGV", json.dumps(s))
    for s in w.json("NOTE"):
        print("NOTE", json.dumps(s))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "info":
        info(argv[2])
    elif cmd == "ticks":
        lo = int(argv[3]) if len(argv) > 3 else 0
        hi = int(argv[4]) if len(argv) > 4 else 1 << 32
        for t in Wsr(argv[2]).ticks():
            if lo <= t["tick"] <= hi:
                print(t["tick"], f"{t['engine']:016x}", f"{t['mods']:016x}", " ".join(f"{c:016x}" for c in t["c"]),
                      f"{t['rng']:08x}", t["inputs"])
    elif cmd == "inputs":
        for i in Wsr(argv[2]).inputs():
            print(SEND_TYPES[i["type"]] if i["type"] < 6 else i["type"], f"{i['id']:04x}", f"{i['a']:08x}",
                  f"{i['b']:08x}", i["time"], i["callT"], f"{i['caller']:08x}", i["str"])
    elif cmd == "energy":
        last = {}
        for tick, worms in Wsr(argv[2]).details():
            for slot, w in worms.items():
                e = (w["energy"], w["energySrc"])
                if slot in last and last[slot][0] != e:
                    p0, p1 = last[slot][1], w["pos"]
                    print(f"tick {tick} worm[{slot}] energy {last[slot][0][0]} -> {e[0]} (source {last[slot][0][1]} -> "
                          f"{e[1]}), pos {p0[0]:.2f},{p0[1]:.2f},{p0[2]:.2f} -> {p1[0]:.2f},{p1[1]:.2f},{p1[2]:.2f}")
                last[slot] = (e, w["pos"])
    elif cmd == "diff":
        a = {t["tick"]: t for t in Wsr(argv[2]).ticks()}
        b = {t["tick"]: t for t in Wsr(argv[3]).ticks()}
        common = sorted(set(a) & set(b))
        for k in common:
            x, y = a[k], b[k]
            if x["engine"] != y["engine"] or x["mods"] != y["mods"]:
                parts = [COMPS[i] for i in range(6) if x["c"][i] != y["c"][i]]
                if x["mods"] != y["mods"]:
                    parts.append("mods")
                print(f"first difference at tick {k} ({', '.join(parts)}); {common.index(k)} earlier ticks equal")
                return 1
        print(f"{len(common)} common ticks, all equal")
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
