#!/usr/bin/env python3
"""Name unnamed DOL functions by matching them against reference objects
(e.g. Dolphin SDK objects built with build_sdk_ref.py).

A reference function matches a DOL function when both have the same size and
identical bytes, ignoring the instruction words that carry relocations in the
reference object. Only unique matches are kept. Then, for every matched pair,
`bl` targets are followed: if the reference calls symbol S, the DOL function at
the corresponding branch target is named S (when unnamed and unambiguous).

Usage:
  python tools/match_ref_objects.py <ref_obj_dir> <main.dol> <symbols.txt> [--apply] [--report out.tsv]
"""
import argparse
import re
import struct
from collections import defaultdict
from pathlib import Path

SYM_RE = re.compile(r"^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)")
R_PPC_REL24 = 10
MIN_SIZE = 0x10


class Dol:
    def __init__(self, path):
        d = path.read_bytes()
        self.d = d
        self.off = struct.unpack(">18I", d[0:72])
        self.addr = struct.unpack(">18I", d[72:144])
        self.size = struct.unpack(">18I", d[144:216])

    def read(self, addr, n):
        for k in range(18):
            if self.size[k] and self.addr[k] <= addr and addr + n <= self.addr[k] + self.size[k]:
                o = self.off[k] + addr - self.addr[k]
                return self.d[o:o + n]
        return None


def read_elf_functions(path):
    """Yield (name, is_global, code bytes, {word_offset: (reloc_type, symbol_name)})."""
    d = path.read_bytes()
    if d[:4] != b"\x7fELF":
        return
    shoff, = struct.unpack(">I", d[0x20:0x24])
    shentsize, shnum, shstrndx = struct.unpack(">HHH", d[0x2E:0x34])
    secs = []
    for i in range(shnum):
        secs.append(struct.unpack(">10I", d[shoff + i * shentsize: shoff + i * shentsize + 40]))
    shstr = secs[shstrndx]

    def cstr(tab_off, o):
        e = d.index(b"\0", tab_off + o)
        return d[tab_off + o:e].decode("latin-1")

    names = [cstr(shstr[4], s[0]) for s in secs]
    symtab_i = next((i for i, s in enumerate(secs) if s[1] == 2), None)
    if symtab_i is None:
        return
    st = secs[symtab_i]
    strtab = secs[st[6]]
    syms = []
    for i in range(st[5] // 16):
        nm, val, size, info, other, shndx = struct.unpack(">IIIBBH", d[st[4] + i * 16: st[4] + i * 16 + 16])
        syms.append((cstr(strtab[4], nm) if nm else "", val, size, info, shndx))

    relocs = defaultdict(dict)  # section index -> {word offset: (type, sym name)}
    for s in secs:
        if s[1] == 4:  # SHT_RELA
            target = s[7]
            for j in range(s[5] // 12):
                r_off, r_info, _add = struct.unpack(">IIi", d[s[4] + j * 12: s[4] + j * 12 + 12])
                sym = syms[r_info >> 8]
                relocs[target][r_off & ~3] = (r_info & 0xFF, sym[0])

    for nm, val, size, info, shndx in syms:
        if (info & 0xF) != 2 or not nm or size < 4 or shndx >= len(secs):
            continue
        if names[shndx] not in (".text", ".init"):
            continue
        sec = secs[shndx]
        code = d[sec[4] + val: sec[4] + val + size]
        rel = {o - val: r for o, r in relocs[shndx].items() if val <= o < val + size}
        yield nm, (info >> 4) == 1, code, rel


def masked(code, rel_offsets):
    b = bytearray(code)
    for o in rel_offsets:
        b[o:o + 4] = b"\0\0\0\0"
    return bytes(b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("refdir", type=Path)
    ap.add_argument("dol", type=Path)
    ap.add_argument("symbols", type=Path)
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--report", type=Path)
    args = ap.parse_args()

    dol = Dol(args.dol)
    sym_lines = args.symbols.read_text(encoding="utf-8").splitlines()
    dol_fns = {}  # addr -> (name, size)
    by_size = defaultdict(list)
    existing = set()
    for line in sym_lines:
        existing.add(line.split("=", 1)[0].strip())
        m = SYM_RE.match(line)
        if m:
            addr, size = int(m.group(3), 16), int(m.group(4), 16)
            dol_fns[addr] = (m.group(1), size)
            by_size[size].append(addr)

    # 1. direct matches
    ref_matches = defaultdict(set)   # ref name -> {dol addr}
    pair_info = []                   # (ref name, dol addr, rel)
    n_ref = 0
    global_names, local_names = set(), set()
    for obj in sorted(args.refdir.rglob("*.o")):
        for nm, is_global, code, rel in read_elf_functions(obj):
            n_ref += 1
            (global_names if is_global else local_names).add(nm)
            if len(code) < MIN_SIZE:
                continue
            mref = masked(code, rel.keys())
            hits = [a for a in by_size.get(len(code), [])
                    if masked(dol.read(a, len(code)) or b"", rel.keys()) == mref]
            if len(hits) == 1:
                ref_matches[nm].add(hits[0])
                pair_info.append((nm, hits[0], rel))

    proposals = defaultdict(set)  # dol addr -> {names}
    sources = {}
    for nm, addrs in ref_matches.items():
        if len(addrs) == 1:
            a = next(iter(addrs))
            proposals[a].add(nm)
            sources[(a, nm)] = "match"

    # 2. follow bl targets of matched functions
    for nm, a, rel in pair_info:
        if len(ref_matches[nm]) != 1:
            continue
        for off, (rtype, target) in rel.items():
            if rtype != R_PPC_REL24 or not target or target.startswith("."):
                continue
            ins, = struct.unpack(">I", dol.read(a + off, 4))
            if (ins >> 26) != 18 or ins & 2:
                continue
            li = ins & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            t = a + off + li
            if t in dol_fns:
                proposals[t].add(target)
                sources.setdefault((t, target), "call")

    renames = {}
    claimed = defaultdict(list)
    for a, names in proposals.items():
        if len(names) != 1:
            continue
        new = next(iter(names))
        old = dol_fns[a][0]
        if old == new or not old.startswith("fn_"):
            continue
        claimed[new].append(a)
    for new, addrs in claimed.items():
        if len(addrs) == 1 and new not in existing:
            renames[dol_fns[addrs[0]][0]] = (new, sources[(addrs[0], new)])

    n_match = sum(1 for v in renames.values() if v[1] == "match")
    print(f"reference functions: {n_ref}  new names: {len(renames)} "
          f"({n_match} by match, {len(renames) - n_match} by call)")
    if args.report:
        with args.report.open("w", encoding="utf-8") as f:
            f.write("old\tnew\tsource\n")
            for old, (new, src) in sorted(renames.items()):
                f.write(f"{old}\t{new}\t{src}\n")

    if args.apply:
        out = []
        for line in sym_lines:
            name = line.split("=", 1)[0].strip()
            if name in renames:
                new = renames[name][0]
                line = new + line[line.index(" ="):]
                # static functions in the SDK stay local so they can't clash with game symbols
                if new in local_names and new not in global_names and "scope:" not in line:
                    line += " scope:local"
            out.append(line)
        args.symbols.write_text("\n".join(out) + "\n", encoding="utf-8")
        print(f"applied {len(renames)} renames")


if __name__ == "__main__":
    main()
