# Gearhead Garage for Apple Silicon: technical details

## Build from Terminal

What you need:

- A Mac with Apple Silicon (M1 or later), macOS 11 or newer.
- Xcode Command Line Tools: `xcode-select --install`.
- A native ARM64 Python 3.11 or newer, available as `python3`.
- Your own copy of the game (see [Which copy works](../README.md#which-copy-works)).
- An internet connection for the first build, to install two pinned Python packages (capstone and pefile) into a private environment.

In this folder, run:

```sh
./build.sh "/path/to/your/disc.cue"
open "out/Gearhead Garage.app"
```

The same script takes any supported input:

```sh
./build.sh disc.bin            # raw 2352-byte CD image (MODE1 or MODE2)
./build.sh disc.iso            # 2048-byte ISO image
./build.sh /Volumes/GEARHEAD   # the mounted disc or a copy of its files
./build.sh GEAR.MSI            # the installer package from the disc
./build.sh "C:/.../Gearhead Garage"   # an installed copy, copied from a PC
./build.sh "/Applications/Gearhead Garage.app"   # the game inside an app built before
```

The build reads your copy without changing it, checks every game file, translates the game, compiles it and creates an ad hoc signed `out/Gearhead Garage.app`. It takes about a minute on an M1 Pro, after the first run's package download. You can move the app anywhere; it carries the complete game.

Keep the app private: it contains the game's assets and translated game code.


The player guide is the [README](../README.md).

**This repository contains no game code or assets. You must supply your own game copy.** Everything derived from it is written to the ignored `local/`, `gen/` and `out/` folders.

## How the build works

1. `tools/disc.py` reads `GEARHEAD/GEAR.MSI` from the ISO9660 file system of the image. `tools/msi.py` reads the package's OLE compound file, string pool and File, Component and Directory tables, and unpacks its MSZIP cabinet with zlib, placing every file where the installer would. Nothing from the disc is executed.
2. `tools/game_source.py` checks the result against `tools/game_manifest.json`.
3. `tools/prepare.py` links `ghg.exe` and the two video driver DLLs (`d3d.vd` at 0x01100000 and `soft8bit.vd` at 0x01000000, relocated) into one 32-bit guest image, and `tools/genall.py` and `tools/lift.py` translate about 9,000 x86 functions into C.
4. `tools/compile.py` compiles them, with the runtime, into one ARM64 executable with Apple clang. `tools/package.py` adds the launcher, the game files and an icon made from the game's own Windows launcher, and signs the bundle ad hoc.

## Checked game files

Every one of the 122 installed files is checked against `tools/game_manifest.json`:

| What is checked | Size in bytes | SHA-256 |
| --- | ---: | --- |
| `GEAR.MSI` | 31,229,440 | `8df931e84afeb470f9bca15b9d2926bd5102591c919462b94a8c291c66617f6e` |
| `ghg.exe` (the game) | 1,015,849 | `f9d797d580d5a42f2f46b912cd6c107bad34c27a32d050cbee13379a65cc498c` |
| `Data/d3d.vd` (Direct3D driver) | 171,008 | `c9394cd571654de11ccadcf823683021b7bd49895487350178a0dc560452491f` |
| `Data/soft8bit.vd` (software driver) | 348,160 | `3462e455687546681f510b2d2b84ff93f5475aee0b7503f7f27b4701499517c8` |
| All 122 installed files combined | 43,698,121 | `8c6d26a2e43072bddcfe22c27b14e888552397b2a795802cedbaf0c70f13616e` |

The combined hash is calculated from the sorted, lower-cased relative paths, each followed by a NUL byte and that file's raw SHA-256 digest. Other releases (for example an original 1999 retail CD with a different installer) are rejected with the list of files that differ; their files have not been checked yet.

## How native is it?

The app executes compiled ARM64 code. It does not use Wine, Rosetta, an x86 interpreter, a JIT or a Windows virtual machine. The translated game keeps a 32-bit guest address space, explicit x86 register and flag state and its original data structures. Its Windows calls go to a native compatibility layer:

- **DirectDraw** (`runtime/ddraw.c`): surfaces in guest memory, flipping, blits with color keys, palettes. The software driver's 8-bit screen is converted through its palette and shown with Metal.
- **Direct3D 5** (`runtime/d3d.c`): `IDirect3D`/`IDirect3D2`, `IDirect3DDevice`/`IDirect3DDevice2`, viewports, materials and textures. The driver sends pre-transformed vertices; they are drawn by a Metal renderer with the game's render states. The game reads the screen back through pointers it keeps locked (video memory on Windows), so the finished scene is copied to guest memory at `EndScene`.
- **DirectSound** with 3D listener and buffers (`runtime/sound.c`, `runtime/sound3d.c`), played through CoreAudio.
- Files, registry, timers, threads, WinMM file I/O, window messages and the game's mouse and keyboard hooks.

Things the port had to match about Windows: the Direct3D driver initializes only on Windows 9x, so the port reports Windows 98; the game needs its launcher's `-MustBeHere` argument; it reads the mouse and keyboard through `WH_MOUSE` and `WH_KEYBOARD` hooks; it draws its pointer on the front buffer and redraws only when something changes.

The original `Gearhead.exe` launcher (an MFC configuration and update program) is not ported: the Mac launcher replaces it. Floating-point code uses host double precision rather than every x87 80-bit intermediate.

## Limitations

Exercised so far: creating and signing in a mechanic, accepting the first job, the Workshop's 3D car, its rotation, picking up the impact wrench, highlighting and opening the body assembly, the Catalog, the JunkYard, sound and music, quitting from the game and from the Mac window, windowed and fullscreen display, and both graphics modes. A full job from start to finish, the Car Lot, the Auction and snapshots have not been played through yet. The e-mail (MAPI) and online update features are not supported.

## Development

`./run.sh [seconds]` runs the last build from `out/` with the game in `local/game`. `tools/dev/drive.py SCRIPT [seconds]` plays a script of input commands through a control FIFO (`click X Y`, `type TEXT`, `key SCAN VK DOWN`, `capture N`, `wait S`) and writes `out/run.log`; `capture N` saves the N-th following presented frame to `out/frame-*.png`. Diagnostic switches: `GEARHEAD_D3D_TRACE=1..3`, `GEARHEAD_DDRAW_TRACE=2`, `GEARHEAD_INPUT_TRACE=1`, `GEARHEAD_TRACE=1` (every Windows call).

The Builder is a Swift package in `builder/` (`BuilderCore` with the checks and the pipeline runner, `GearheadBuilder` with the window). `swift run --package-path builder GearheadBuilder` runs it from this checkout; `tools/builder_cli.py` is the Python side it drives, printing `GEARHEAD-EVENT` JSON lines. `tools/make_builder.py --version X --output dist` assembles the app; for unattended runs pass `-gameCopy PATH -destination DIR -AutoBuild YES -SnapshotFolder DIR` after the binary (`-OpenWhenDone YES` then opens the new app, as the launcher's Update does). Tests: `python3 -B -m unittest discover -s tests -t tests -p 'test_*.py'` and `swift test --package-path builder`. They use small generated CD images, never game files.

## Releases

`.github/workflows/build.yml` runs the tests and builds `Gearhead-Garage-Builder.dmg` on every push and pull request (kept as a workflow artifact). To publish a release, set `VERSION`, add a `## X.Y.Z` section to `CHANGELOG.md` and push the tag `vX.Y.Z`: the workflow attaches the disk image and its SHA-256 to a new GitHub release with that section as its notes. The workflow never sees a game file.

## Legal and notices

The repository contains build tools, the native compatibility layer, the launcher, a Metal shader and third-party notices (`third_party/`). The NFSU2Recomp-derived portions permit personal local use and modification; redistribution requires the copyright holder's permission under the retained license. No game or app download is supplied by this repository.
