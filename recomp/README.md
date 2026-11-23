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

- `gx_shader.c` – TEV as generated GLSL (cached per configuration): up to 16
  stages, all color/alpha inputs, konst colors and registers, bias/scale/
  subtract/clamp, swap tables, alpha test.
- Per-vertex on the CPU: position/normal matrices, lighting channels
  (material/ambient sources, up to 8 lights, spot + distance attenuation),
  texgen with texture matrices (ST/STQ, position/normal/texcoord sources).
- Vertices are submitted as arrays; texture hashes are checked once per frame.

Not yet: fog, indirect textures, specular lights, dual-tex (post) matrices,
scissor, TEV compare modes, render-to-texture (EFB copies to RAM).
`gx_stats` prints average fps and where GX time goes.

### Input (`boot/input.c`)

Port 1 = keyboard (while the game window has focus) + XInput pad 1;
ports 2-4 = XInput pads 2-4 (loaded at runtime from `xinput1_4.dll`).

| GameCube | Keyboard | XInput |
|---|---|---|
| Control stick | Arrows (Shift = half tilt) | Left stick |
| C-stick | I / J / K / L | Right stick |
| A / B / X / Y | X / Z / C / S | A / B / X / Y |
| Z | D | RB |
| L / R | Q / W | LT / RT (analog) |
| START | Enter | Start |
| D-pad | T / G / F / H | D-pad |

`INPUT_AUTOPRESS="START:22,A:30"` presses buttons at given seconds (testing).

### Audio (`boot/audio/`)

The AX library runs translated; the DSP running the AX microcode is replaced:

- a 5 ms real-time tick raises the AI DMA interrupt; `__AXOutAiCallback` is
  native: it runs the translated `__AXOutNewFrame` (voice sync, CRI/ADX
  callbacks, next command list) and then mixes one frame;
- `ax_mix.c` mixes the 64 DSP-side parameter blocks (`0x8028FB00`, 0xF4 bytes
  each): ADPCM / PCM16 / PCM8 from ARAM, loops (incl. ADPCM loop context),
  sample-rate conversion (linear), envelope and L/R volume ramps; it writes
  state, current address, ADPCM history and volumes back like the DSP;
- `audio_out.c` plays 32 kHz stereo through waveOut (20 ms blocks, latency
  capped).

Not yet: aux buses (reverb/chorus), surround, ITD, low-pass filter,
4-tap polyphase SRC, per-ms parameter updates.

`GX_WINDOW=background` opens the window minimized without taking focus.

### REL modules

`ppc2c.py --rel module.rel=symbols.txt` translates a REL linked at runtime:

- relocations against the DOL are resolved at translation time (absolute
  addresses, direct calls into the DOL; mid-function targets such as
  `_savefpr_N` get DOL entry points);
- relocations against the module itself become `ppc_relbase_mN[section] + off`;
- switch tables are found through the module's own relocations;
- on the first indirect call into unknown memory, the runtime walks the OS
  module list (`0x800030C8`), reads the section addresses OSLink assigned and
  registers the module's functions at their real addresses.

Current state: boot logos, license and title screens render; START on the
title opens the main menu (`k_main_menu.prd`).

## Not done yet

- Full TEV / lighting / render-to-texture.
- Audio: AX runs without a DSP (silent); AI/ADX output not mixed.
- Input: the pad is always neutral (SDL planned).
- Alarms (`OSSetAlarm`) / decrementer are not emulated yet.
- FPSCR rounding modes / exceptions are ignored.
- `movie_module.rel` is not translated yet.
