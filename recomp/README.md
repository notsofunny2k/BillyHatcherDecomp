# Static recompiler (Gekko → C)

Goal: a native PC executable built from the user's own copy of the game.
`ppc2c.py` translates every function of `main.dol` to C; a runtime replaces the
console hardware (planned: Dolphin SDK calls intercepted by name and
implemented natively, GX on top of a modern graphics API, SDL for input/audio).

**Generated code is derived from the game and must never be committed or
distributed.** It is written to `build/` (ignored by git). This folder only
contains the recompiler, the runtime and tests.

## Layout

- `ppc2c.py` – recompiler. Uses `config/GEZE8P/symbols.txt` for function
  boundaries/names and `jumptable_*` objects for `switch` targets.
- `runtime/ppc_context.h` – CPU state (GPRs, FPRs + paired singles, CR, XER,
  SPRs/GQRs) and big-endian guest memory helpers.
- `runtime/ppc_runtime.c` – guest memory, DOL loader, indirect calls through the
  generated function table, error handlers.
- `tests/test_basic.c` + `run_tests.py` – runs translated functions on known
  inputs and checks the results against native C.

## Usage

```sh
# translate everything (≈5 s, ≈45 MB of C)
python recomp/ppc2c.py orig/GEZE8P/sys/main.dol config/GEZE8P/symbols.txt build/recomp_all

# translate a few functions plus everything they call
python recomp/ppc2c.py orig/GEZE8P/sys/main.dol config/GEZE8P/symbols.txt build/out --only PSMTXConcat --closure

# tests (needs clang, e.g. llvm-mingw)
python recomp/run_tests.py --cc path/to/clang
```

## Translation model

- One C function per guest function: `void ppc_XXXXXXXX(PPCContext* ctx)`.
- Branches inside a function → `goto`; `bl` → direct C call (sets `lr`);
  `blr` → `return`; branches to another function → tail call.
- `bctr` → `switch` over the function's jump-table targets, otherwise an
  indirect call through the sorted function table.
- Single-precision ops and paired singles round through `float`; `fmadd`-family
  ops use `fma()` (fused, as on Gekko). `psq_l`/`psq_st` honour GQR
  quantization (float, u8, u16, s8, s16 with scale).
- Guest memory: 24 MB RAM mirrored at `0x80000000`/`0xC0000000`, locked cache
  at `0xE0000000`.

## Status

- All 15 559 functions translate and compile (clang -O1, 0 errors).
- Unsupported: `mfsr`, `mtsr`, `rfi`, `twi` (47 instructions, OS/exception code
  that will be replaced natively).
- Tests: 247/247 checks pass (memset/memcpy/strlen/memcmp/strncpy, 64-bit
  `__div2i`/`__mod2i`/`__shl2i`, `PSMTX*`, `PSVEC*`).

## Not done yet

- Hardware registers (`0xCC00xxxx`) are not routed to device emulation.
- FPSCR rounding modes / exceptions are ignored.
- `movie_module.rel` is not translated yet.
- No native replacement (HLE) of SDK functions yet; no graphics/audio/input.
