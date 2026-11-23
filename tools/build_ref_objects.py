#!/usr/bin/env python3
"""Compile a list of C/C++ sources from another dtk-style decomp project with
MWCC, producing reference objects for match_ref_objects.py.

Only compiles source files (no scripts from the other project are run).
Source names use that project's configure.py form, e.g.
"PowerPC_EABI_Support/Msl/MSL_C/MSL_Common/printf.c", and are resolved to
<root>/libs/<lib>/src/<rest> (or <root>/src/<name> as a fallback).

Usage:
  python tools/build_ref_objects.py --root <project> --compiler <GC/x.y dir> --out <dir>
         --flags "<mwcc flags>" --include <dir> [--include <dir> ...] <list.txt>
"""
import argparse
import shlex
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def resolve(root: Path, name: str):
    lib, _, rest = name.partition("/")
    for cand in (root / "libs" / lib / "src" / rest, root / "src" / name):
        if cand.exists():
            return cand
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, required=True)
    ap.add_argument("--compiler", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--flags", default="")
    ap.add_argument("--flags-file", type=Path, help="read extra flags from a file (one line)")
    ap.add_argument("--include", action="append", default=[])
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("list", type=Path)
    args = ap.parse_args()

    cc = (args.compiler / "mwcceppc.exe").resolve()
    root = args.root.resolve()
    out = args.out.resolve()
    flags = shlex.split(args.flags, posix=True)
    if args.flags_file:
        flags += shlex.split(args.flags_file.read_text(), posix=True)
    incs = []
    for i in args.include:
        incs += ["-i", str((root / i).resolve())]
    names = [l.strip() for l in args.list.read_text().splitlines() if l.strip() and not l.startswith("#")]

    def build(name):
        src = resolve(root, name)
        if src is None:
            return name, 1, "source not found"
        if src.suffix not in (".c", ".cp", ".cpp"):
            return name, 0, "skipped"
        obj = out / Path(name).with_suffix(".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        cmd = [str(cc), "-c", *flags, *incs, str(src), "-o", str(obj)]
        r = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
        return name, r.returncode, (r.stdout + r.stderr).strip()

    ok = fail = 0
    with ThreadPoolExecutor(args.jobs) as ex:
        for name, code, log in ex.map(build, names):
            if code == 0:
                ok += 1
            else:
                fail += 1
                last = [l for l in log.splitlines() if l.strip()]
                print(f"FAIL {name}: {last[-1] if last else code}")
    print(f"compiled {ok}, failed {fail}")


if __name__ == "__main__":
    main()
