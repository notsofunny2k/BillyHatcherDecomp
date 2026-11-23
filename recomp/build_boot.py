#!/usr/bin/env python3
"""Translate the whole DOL (with HLE replacements), compile it incrementally
with clang and link the boot executable. Then optionally run it.

Usage:
  python recomp/build_boot.py --cc path/to/clang [--run] [--timeout 20]
"""
import argparse
import hashlib
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CFLAGS = ["-O1", "-std=c11", "-ffp-contract=off", "-fno-strict-aliasing",
          "-Wno-unused-label", "-Wno-parentheses-equality"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="clang")
    ap.add_argument("--dol", default=str(ROOT / "orig/GEZE8P/sys/main.dol"))
    ap.add_argument("--sys", default=str(ROOT.parent / "extracted/sys"),
                    help="folder with boot.bin, bi2.bin, fst.bin")
    ap.add_argument("--iso", default=str(ROOT.parent / "BillyHatcherRom/BillyHatcher.iso"))
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--timeout", default="20")
    args = ap.parse_args()

    gen = ROOT / "build/recomp_boot"
    obj = gen / "obj"
    obj.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(ROOT / "recomp/ppc2c.py"), args.dol,
                    str(ROOT / "config/GEZE8P/symbols.txt"), str(gen),
                    "--hle", str(ROOT / "recomp/boot/hle.txt")], check=True)

    runtime = ROOT / "recomp/runtime"
    boot = ROOT / "recomp/boot"
    sources = sorted(gen.glob("*.c")) + sorted(runtime.glob("*.c")) + sorted(boot.glob("*.c"))
    headers = b"".join(p.read_bytes() for p in sorted(list(runtime.glob("*.h")) + [gen / "ppc_funcs.h"]))

    def compile_one(src):
        key = hashlib.sha1(src.read_bytes() + headers + " ".join(CFLAGS).encode()).hexdigest()[:16]
        out = obj / f"{src.stem}.{key}.o"
        if out.exists():
            return out, None
        for old in obj.glob(f"{src.stem}.*.o"):
            old.unlink()
        r = subprocess.run([args.cc, "-c", *CFLAGS, "-I", str(gen), "-I", str(runtime),
                            str(src), "-o", str(out)], capture_output=True, text=True)
        return out, (r.stderr if r.returncode else None)

    with ThreadPoolExecutor(os.cpu_count() or 8) as ex:
        results = list(ex.map(compile_one, sources))
    errors = [e for _, e in results if e]
    if errors:
        print("\n".join(errors)[:8000])
        sys.exit(1)
    exe = gen / "boot.exe"
    subprocess.run([args.cc, *[str(o) for o, _ in results], "-o", str(exe), "-lm"], check=True)
    print(f"built {exe}")
    if args.run:
        r = subprocess.run([str(exe), args.dol, args.sys, args.iso, args.timeout])
        print(f"exit code {r.returncode}")


if __name__ == "__main__":
    main()
