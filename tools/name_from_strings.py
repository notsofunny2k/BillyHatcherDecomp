#!/usr/bin/env python3
"""Propose names for unnamed functions (fn_XXXXXXXX) from the error/assert
strings they reference, e.g. "E02080804 ADXT_Create: parameter error",
"'adxf' is NULL.(ADXF_Seek)" or "DVDOpen(): ...".

Strings are read straight from the original binary (DOL or REL), since the
analyzer does not always recognize them as strings. Inside each function a
small register tracker follows `lis/addi` pairs plus `addi rX, rBase, imm`
so references through a shared string base are counted too.

Usage:
  python tools/name_from_strings.py <asm_dir> <symbols.txt> <binary> [--apply] [--report out.tsv]
"""
import argparse
import re
import struct
from collections import Counter, defaultdict
from pathlib import Path

FN_RE = re.compile(r"^\.fn\s+(\w+)")
ENDFN_RE = re.compile(r"^\.endfn\b")
INSN_RE = re.compile(r"^/\*[^*]*\*/\s+(\S+)\s+(.*)$")
SYMREF_RE = re.compile(r"^(\w+)(\+0x[0-9A-Fa-f]+|-0x[0-9A-Fa-f]+)?@(ha|l|sda21)(?:\((r\d+)\))?$")
SYM_RE = re.compile(r"^(\S+) = (\.\w+):0x([0-9A-Fa-f]+);")

ID_PATTERNS = [
    re.compile(r"^[EW][0-9A-Fa-f]+[: ]?\s*:?\s*([A-Za-z_]\w*)\s*:"),     # E02080804 ADXT_Create:
    re.compile(r"\(([A-Za-z_]\w*)\)\s*$"),                                # ...(ADXF_Seek)
    re.compile(r"^(?:Warning:\s*|Error:\s*)?([A-Za-z_]\w*)\(\)\s*:"),    # DVDOpen():
    re.compile(r"^\s*([A-Za-z_]\w*_\w+)\s*:"),                            # Foo_Bar: msg
]
API_RE = re.compile(r"^(?:[A-Za-z]+_\w+|(?:OS|DVD|GX|VI|PAD|CARD|AI|ARQ|AR|AX|DSP|EXI|SI|GBA|mw|SFD|ADX)[A-Z]\w*)$")


class Binary:
    """Read C strings by (section, address) from a DOL or REL."""

    def __init__(self, path: Path):
        self.data = path.read_bytes()
        self.is_rel = path.suffix.lower() == ".rel"
        if self.is_rel:
            n, tab = struct.unpack(">II", self.data[0xC:0x14])
            self.rel_secs = []
            for i in range(n):
                off, size = struct.unpack(">II", self.data[tab + i * 8: tab + i * 8 + 8])
                self.rel_secs.append((off & ~1, size))
            # section name -> index, as reported by `dtk rel info`
            self.rel_index = {".text": 1, ".ctors": 2, ".dtors": 3, ".rodata": 4, ".data": 5}
        else:
            d = self.data
            self.off = struct.unpack(">18I", d[0:72])
            self.addr = struct.unpack(">18I", d[72:144])
            self.size = struct.unpack(">18I", d[144:216])

    def file_offset(self, section, addr):
        if self.is_rel:
            idx = self.rel_index.get(section)
            if idx is None or idx >= len(self.rel_secs):
                return None
            off, size = self.rel_secs[idx]
            return off + addr if off and addr < size else None
        for k in range(18):
            if self.size[k] and self.addr[k] <= addr < self.addr[k] + self.size[k]:
                return self.off[k] + addr - self.addr[k]
        return None

    def cstring(self, section, addr, minlen=6):
        fo = self.file_offset(section, addr)
        if fo is None:
            return None
        end = self.data.find(b"\0", fo, fo + 512)
        if end < 0 or end - fo < minlen:
            return None
        raw = self.data[fo:end]
        if any(b < 0x09 or (0x0D < b < 0x20) or b > 0x7E for b in raw):
            return None
        return raw.decode("ascii")


def identifiers(text):
    out = []
    for line in text.splitlines():
        for pat in ID_PATTERNS:
            for m in pat.finditer(line):
                ident = m.group(1)
                if len(ident) >= 4 and API_RE.match(ident):
                    out.append(ident)
    return out


def parse_imm(s):
    s = s.strip()
    try:
        return int(s, 0)
    except ValueError:
        return None


def scan_function_refs(lines, label_info):
    """Return (section, address) of data referenced by one function's instructions.

    A label loaded only to serve as a base for `addi rX, rBase, imm` is not
    itself counted, unless it is also passed directly (offset 0 use).
    """
    regs = {}  # reg -> ("hi"|"full", section, addr)
    direct, derived, used_as_base = [], [], set()
    _walk(lines, label_info, regs, direct, derived, used_as_base)
    out = [v for v in direct if v not in used_as_base] + derived
    return out


def _walk(lines, label_info, regs, direct, derived, used_as_base):
    for line in lines:
        m = INSN_RE.match(line)
        if not m:
            continue
        op, rest = m.group(1), m.group(2)
        ops = [o.strip() for o in rest.split(",")]
        dest = ops[0] if ops else None
        if op == "lis" and len(ops) == 2:
            sm = SYMREF_RE.match(ops[1])
            if sm and sm.group(3) == "ha" and sm.group(1) in label_info:
                sec, base = label_info[sm.group(1)]
                regs[dest] = ("hi", sec, base + (int(sm.group(2), 16) if sm.group(2) else 0))
            else:
                regs.pop(dest, None)
            continue
        if op in ("addi", "ori") and len(ops) == 3:
            src = regs.get(ops[1])
            sm = SYMREF_RE.match(ops[2])
            if sm and sm.group(3) in ("l", "sda21") and sm.group(1) in label_info:
                sec, base = label_info[sm.group(1)]
                val = (sec, base + (int(sm.group(2), 16) if sm.group(2) else 0))
                regs[dest] = ("full",) + val
                direct.append(val)
                continue
            imm = parse_imm(ops[2])
            if src and src[0] == "full" and imm is not None:
                used_as_base.add((src[1], src[2]))
                val = (src[1], src[2] + imm)
                regs[dest] = ("full",) + val
                derived.append(val)
                continue
            regs.pop(dest, None)
            continue
        if op == "mr" and len(ops) == 2:
            if ops[1] in regs:
                regs[dest] = regs[ops[1]]
            else:
                regs.pop(dest, None)
            continue
        if op.startswith("b") and op not in ("b",):
            # calls clobber volatile registers r0, r3-r12
            if op in ("bl", "bctrl", "blrl"):
                for r in ["r0"] + [f"r{i}" for i in range(3, 13)]:
                    regs.pop(r, None)
            continue
        if dest and re.fullmatch(r"r\d+", dest or "") and not op.startswith(("st", "cmp", "tw", "dcb", "icb")):
            regs.pop(dest, None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("asm_dir", type=Path)
    ap.add_argument("symbols", type=Path)
    ap.add_argument("binary", type=Path)
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--report", type=Path)
    ap.add_argument("--skip", default="", help="comma-separated functions to leave unnamed")
    args = ap.parse_args()
    skip = set(filter(None, args.skip.split(",")))

    binary = Binary(args.binary)
    sym_lines = args.symbols.read_text(encoding="utf-8").splitlines()
    label_info = {}
    existing = set()
    for line in sym_lines:
        m = SYM_RE.match(line)
        if m:
            label_info[m.group(1)] = (m.group(2), int(m.group(3), 16))
            existing.add(m.group(1))

    votes = defaultdict(Counter)
    evidence = defaultdict(list)
    for f in sorted(args.asm_dir.glob("*.s")):
        if "_text" not in f.name and "_init" not in f.name:
            continue
        cur, body = None, []
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            m = FN_RE.match(line)
            if m:
                cur, body = m.group(1), []
                continue
            if ENDFN_RE.match(line):
                if cur and cur.startswith("fn_"):
                    seen = set()
                    for sec, addr in scan_function_refs(body, label_info):
                        if (sec, addr) in seen:
                            continue
                        seen.add((sec, addr))
                        s = binary.cstring(sec, addr)
                        if not s:
                            continue
                        ids = identifiers(s)
                        for i in ids:
                            votes[cur][i] += 1
                        if ids:
                            evidence[cur].append(s.strip()[:90])
                cur = None
                continue
            if cur:
                body.append(line)

    picks = {}
    for fn, c in votes.items():
        if fn in skip:
            continue
        ranked = c.most_common()
        best, n = ranked[0]
        second = ranked[1][1] if len(ranked) > 1 else 0
        if n > second:
            picks[fn] = (best, n)
    by_name = defaultdict(list)
    for fn, (name, n) in picks.items():
        by_name[name].append((n, fn))
    final = {}
    for name, cands in by_name.items():
        cands.sort(reverse=True)
        if len(cands) > 1 and cands[0][0] == cands[1][0]:
            continue
        if name in existing:
            continue
        final[cands[0][1]] = name

    rows = sorted(final.items())
    if args.report:
        with args.report.open("w", encoding="utf-8") as out:
            out.write("function\tnew_name\tvotes\tevidence\n")
            for fn, name in rows:
                out.write(f"{fn}\t{name}\t{picks[fn][1]}\t{' | '.join(evidence[fn][:3])}\n")
    print(f"candidate fns: {len(votes)}  proposed names: {len(rows)}")

    if args.apply:
        out_lines = []
        for line in sym_lines:
            name = line.split("=", 1)[0].strip()
            if name in final:
                line = final[name] + line[line.index(" ="):]
            out_lines.append(line)
        args.symbols.write_text("\n".join(out_lines) + "\n", encoding="utf-8")
        print(f"applied {len(rows)} renames to {args.symbols}")


if __name__ == "__main__":
    main()
