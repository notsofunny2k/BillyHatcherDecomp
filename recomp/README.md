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

## Boot harness (`boot/`, `build_boot.py`)

Runs the translated `__start` on PC:

```sh
python recomp/build_boot.py --cc path/to/clang --run --timeout 30
# needs orig/GEZE8P/sys/main.dol, the extracted sys/ (boot.bin, bi2.bin, fst.bin)
# and the disc image (--sys / --iso to override the default paths)
```

- `boot_main.c` – sets up low memory like the IPL (disc ID, boot info, FST and
  BI2 at the top of RAM), runs the entry point; a watchdog dumps the last guest
  calls (with callers) if the game stops making progress.
- `hle.txt` – guest functions replaced by native code (`hle_<Name>`):
  - `hle_os.c`: OSReport/OSPanic, TRK/DB, DSP/AX bring-up, BAT setup.
  - `hle_irq.c`: interrupt enable/disable + delivery of device interrupts to
    the OS handler table.
  - `hle_thread.c`: native scheduler — every guest thread is a Windows fiber
    with its own CPU context; OS priority rules.
  - `hle_time.c`: real-time time base, 59.94 Hz VI retrace interrupt.
  - `hle_si.c`: controller bus (one neutral pad for now).
  - `hle_dvd.c`: DVD interface backed by the disc image (+ DI interrupt).
- `runtime/mmio.c` – hardware registers: EXI/SI instant transfers, ARAM
  (16 MB) with its DMA engine and interrupt.

Current state: OSInit, all SDK/CRI init, thread creation, main loop; the game
reads its files from the disc and reaches the license screen
(`k_boot.prd`, `egg01/02.mlt`, `bank06.mpb`, `amem_boot.nrc`, `k_license.prd`).

### Graphics (`boot/gx/`)

- `gx.c` – command processor for the write-gather pipe stream: CP/XF/BP
  registers, indexed XF loads, display lists, primitives with every vertex
  format (direct/indexed, all component/color types), position matrices +
  projection, viewport, blending/depth, TLUT loads, EFB copy to XFB = present,
  PE draw-done/token interrupts.
- `gx_tex.c` – texture decoding: I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8,
  C4, C8, C14X2 (TLUT), CMPR; cached GL textures.
- `gx_win.c` – Win32 window + OpenGL (legacy pipeline for this first version);
  `GX_SCREENSHOT_DIR=<dir>` saves a frame every second for debugging.

Approximations: TEV is "vertex/material color × texture 0"; no lighting,
texgen matrices, indirect textures, fog, scissor or render-to-texture yet.

Current state: boot logos render correctly; the game reaches the title,
where it links a REL module at runtime (not translated yet).

## Not done yet

- RELs loaded at runtime (`OSLink`): `movie_module.rel` must be translated
  with its relocations resolved against the load address.
- Full TEV / lighting / render-to-texture.
- Audio: AX runs without a DSP (silent); AI/ADX output not mixed.
- Input: the pad is always neutral (SDL planned).
- Alarms (`OSSetAlarm`) / decrementer are not emulated yet.
- FPSCR rounding modes / exceptions are ignored.
- `movie_module.rel` is not translated yet.
