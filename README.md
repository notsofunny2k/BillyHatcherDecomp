Billy Hatcher and the Giant Egg – Decompilation
================================================

A work-in-progress decompilation of *Billy Hatcher and the Giant Egg* (GameCube), with the long-term goal of a native PC port.

This repository does **not** contain any game assets or assembly. An existing copy of the game is required.

Supported versions:

- `GEZE8P`: Rev 0 (USA)

Based on [dtk-template](https://github.com/encounter/dtk-template) and [decomp-toolkit](https://github.com/encounter/decomp-toolkit).

Dependencies
============

Windows
-------

- [Python](https://www.python.org/downloads/) 3.8+ in `%PATH%`
- [Ninja](https://github.com/ninja-build/ninja/releases) in `%PATH%` (or pass the full path)

decomp-toolkit, objdiff and the Metrowerks compilers are downloaded automatically on the first build (or can be passed with `--dtk` / `--objdiff`).

Building
========

1. Clone the repository.
2. Copy your disc image (or the extracted `sys/main.dol` and `files/movie_module.rel`) into `orig/GEZE8P/`.
3. Configure:

   ```sh
   python configure.py
   ```

4. Build:

   ```sh
   ninja
   ```

The build verifies that the rebuilt `main.dol` and `movie_module.rel` match the original SHA-1 hashes in `config/GEZE8P/build.sha1`.

Project status
==============

- Initial analysis done: 15 559 functions in `main.dol`, 767 in `movie_module.rel`.
- Libraries identified: Dolphin SDK (April 2003 build), CRI ADX (ADXT/ADXF/ADXB, SJ, LSC, CVFS), CRI Sofdec (in `movie_module.rel`), Sonic Team Ninja engine (`gj*`).
- Function naming helpers in `tools/`:
  - `name_from_strings.py` – names functions from the error/assert strings they reference.
  - `name_sdk_anchors.py` – names SDK init functions from `<< Dolphin SDK - XX >>` version strings.

Diffing
=======

Use [objdiff](https://github.com/encounter/objdiff): open this folder as a project after running `python configure.py` (it generates `objdiff.json`).
