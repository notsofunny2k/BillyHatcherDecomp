#!/usr/bin/env python3
"""Compile the Dolphin SDK sources of a dolsdk20XX checkout (doldecomp/dolsdk2001
or dolsdk2004) with MWCC, producing reference objects for match_ref_objects.py.

Mirrors the release flags from that repo's Makefile.

Usage:
  python tools/build_sdk_ref.py <dolsdk_dir> <mwcc_dir> <out_dir> [--sdk-revision N] [--jobs N]
  e.g. python tools/build_sdk_ref.py D:/DecompRepos/dolsdk2004 build/compilers/GC/1.2.5n build/sdkref
"""
import argparse
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SIGNED_CHAR = ("src/dvd/", "src/card/CARDRename.c", "src/card/CARDOpen.c",
               "src/mtx/mtx.c", "src/mtx/mtx44.c", "src/demo/")
NO_SYM = ("src/gd/GDIndirect.c", "src/G2D/G2D.c")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sdk", type=Path)
    ap.add_argument("mwcc", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--sdk-revision", default="2")
    ap.add_argument("--jobs", type=int, default=8)
    args = ap.parse_args()

    cc = (args.mwcc / "mwcceppc.exe").resolve()
    out = args.out.resolve()
    sources = sorted((args.sdk / "src").rglob("*.c"))

    def build(src: Path):
        rel = src.relative_to(args.sdk).as_posix()
        obj = out / Path(rel).with_suffix(".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        char = "-char signed" if rel.startswith(SIGNED_CHAR) else "-char unsigned"
        sym = [] if rel in NO_SYM else ["-sym", "on"]
        cmd = [str(cc), "-c", "-O4,p", "-inline", "auto", *sym, *char.split(),
               "-nodefaults", "-proc", "gekko", "-fp", "hard", "-Cpp_exceptions", "off",
               "-enum", "int", "-warn", "off", "-requireprotos", "-pragma", "cats off",
               "-D__GEKKO__", f"-DSDK_REVISION={args.sdk_revision}", "-DRELEASE",
               "-I-", "-Iinclude", "-Iinclude/libc", "-ir", "src", rel, "-o", str(obj)]
        r = subprocess.run(cmd, cwd=args.sdk, capture_output=True, text=True)
        return rel, r.returncode, (r.stdout + r.stderr).strip()

    ok = fail = 0
    with ThreadPoolExecutor(args.jobs) as ex:
        for rel, code, log in ex.map(build, sources):
            if code == 0:
                ok += 1
            else:
                fail += 1
                print(f"FAIL {rel}: {log.splitlines()[-1] if log else code}")
    print(f"compiled {ok}, failed {fail}")


if __name__ == "__main__":
    main()
