#!/usr/bin/env python3
"""Recompile the functions used by recomp/tests, build the test binary with
clang and run it against the original DOL.

Usage: python recomp/run_tests.py [--cc path/to/clang]
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FUNCS = ["__init_registers", "memset", "memcpy", "strlen", "memcmp", "strncpy",
         "__div2i", "__mod2i", "__shl2i",
         "PSMTXIdentity", "PSMTXConcat", "PSMTXCopy", "PSMTXTranspose",
         "PSVECAdd", "PSVECCrossProduct", "PSVECNormalize"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="clang")
    ap.add_argument("--dol", default=str(ROOT / "orig/GEZE8P/sys/main.dol"))
    args = ap.parse_args()

    out = ROOT / "build/recomp_test"
    subprocess.run([sys.executable, str(ROOT / "recomp/ppc2c.py"), args.dol,
                    str(ROOT / "config/GEZE8P/symbols.txt"), str(out),
                    "--only", ",".join(FUNCS), "--closure"], check=True)
    sources = sorted(out.glob("*.c")) + [ROOT / "recomp/runtime/ppc_runtime.c",
                                          ROOT / "recomp/tests/test_basic.c"]
    exe = out / "test_basic.exe"
    cmd = [args.cc, "-O2", "-std=c11", "-ffp-contract=off", "-fno-strict-aliasing",
           "-Wno-unused-label", "-I", str(out), "-I", str(ROOT / "recomp/runtime"),
           *map(str, sources), "-o", str(exe), "-lm"]
    subprocess.run(cmd, check=True)
    r = subprocess.run([str(exe), args.dol])
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
