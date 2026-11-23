#!/usr/bin/env python3
"""Name Dolphin SDK anchors from the "<< Dolphin SDK - XX ... >>" version strings.

Each SDK library keeps `const char* __XXVersion = "<< Dolphin SDK - XX ...>>"`
in .sdata and calls `OSRegisterVersion(__XXVersion)` from its init function.
This script:
  * names the version string object `@XX_version_str` -> left as is (compiler-generated),
  * names the pointer `__XXVersion`,
  * names the function that loads that pointer `XXInit` (if it is unnamed and the only loader),
  * names the most common callee right after the load `OSRegisterVersion`.

Usage: python tools/name_sdk_anchors.py <asm_dir> <symbols.txt> [--apply]
"""
import argparse
import re
from collections import Counter, defaultdict
from pathlib import Path

HDR_RE = re.compile(r"^\.obj\s+\"?([^\",]+)\"?,")
STR_RE = re.compile(r'^\s*\.string\s+"<< Dolphin SDK - (\w+)')
PTR_RE = re.compile(r"^\s*\.4byte\s+\"?([^\"\s]+)\"?\s*$")
FN_RE = re.compile(r"^\.fn\s+(\w+)")
ENDFN_RE = re.compile(r"^\.endfn\b")

# Library tag -> init function name in the Dolphin SDK.
INIT_NAMES = {
    "OS": "OSInit", "DVD": "DVDInit", "VI": "VIInit", "PAD": "PADInit", "SI": "SIInit",
    "EXI": "EXIInit", "AI": "AIInit", "AR": "ARInit", "ARQ": "ARQInit", "AX": "AXInit",
    "DSP": "DSPInit", "CARD": "CARDInit", "GX": "GXInit",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("asm_dir", type=Path)
    ap.add_argument("symbols", type=Path)
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    files = sorted(args.asm_dir.glob("*.s"))
    # 1. version string objects
    ver_obj = {}
    for f in files:
        if "_text" in f.name:
            continue
        cur = None
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            m = HDR_RE.match(line)
            if m:
                cur = m.group(1)
                continue
            m = STR_RE.match(line)
            if m and cur:
                ver_obj[cur] = m.group(1)
    # 2. pointers to them
    ptr_of = {}
    for f in files:
        if "_text" in f.name:
            continue
        cur = None
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            m = HDR_RE.match(line)
            if m:
                cur = m.group(1)
                continue
            m = PTR_RE.match(line)
            if m and cur and m.group(1) in ver_obj:
                ptr_of[cur] = ver_obj[m.group(1)]
    # 3. loaders
    loaders = defaultdict(set)
    callee_votes = Counter()
    for f in files:
        if "_text" not in f.name:
            continue
        cur = None
        pending = False
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            m = FN_RE.match(line)
            if m:
                cur = m.group(1)
                continue
            if ENDFN_RE.match(line):
                cur = None
                continue
            if not cur:
                continue
            for p, lib in ptr_of.items():
                if re.search(rf"\b{re.escape(p)}@sda21", line) and "lwz r3" in line:
                    loaders[lib].add(cur)
                    pending = True
            if pending and "\tbl " in line:
                callee_votes[line.split("\tbl ")[1].strip()] += 1
                pending = False

    renames = {}
    for p, lib in ptr_of.items():
        if p.startswith(("lbl_", "@")):
            renames[p] = f"__{lib}Version"
        fns = loaders.get(lib, set())
        init = INIT_NAMES.get(lib)
        if init and len(fns) == 1:
            fn = next(iter(fns))
            if fn.startswith("fn_"):
                renames[fn] = init
    if callee_votes:
        callee, n = callee_votes.most_common(1)[0]
        if callee.startswith("fn_") and n >= 3:
            renames[callee] = "OSRegisterVersion"

    for lib in sorted(set(ptr_of.values())):
        print(f"{lib:5} loaders={sorted(loaders.get(lib, []))}")
    for old, new in sorted(renames.items(), key=lambda kv: kv[1]):
        print(f"  {old} -> {new}")

    if args.apply:
        lines = args.symbols.read_text(encoding="utf-8").splitlines()
        existing = {l.split("=", 1)[0].strip() for l in lines}
        out = []
        for line in lines:
            name = line.split("=", 1)[0].strip()
            new = renames.get(name)
            if new and new not in existing:
                line = new + line[line.index(" ="):]
            out.append(line)
        args.symbols.write_text("\n".join(out) + "\n", encoding="utf-8")
        print("applied")


if __name__ == "__main__":
    main()
