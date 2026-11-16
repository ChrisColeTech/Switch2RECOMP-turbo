# Switch2RECOMP Turbo

**An enhanced fork maintained by [ChrisColeTech](https://github.com/ChrisColeTech).** This is a modified version of
SwitchRecomp, not the vanilla upstream release. It is a **static recompiler**, not an emulator product: ARM64 game
code is translated to C and compiled into a native Windows or Linux module. The runtime supplies Horizon OS services
and graphics compatibility.

This repository is the recompiler engine only. The Qt desktop launcher and the portable release bundles live in the
separate RecompLauncher repository, which vendors this fork as a git submodule.

No games, keys, or firmware are included. Use your own legally obtained dumps and console keys.

## What Turbo adds

Direct NUP and NSP recompilation with in-place RomFS reads, native NVN/Vulkan work, shader-cache and frame-time
diagnostics, and Windows DualSense/DualShock 4 input, player lights and classic rumble (Switch 2 games' rumble
included; USB DualSense rumble confirmed by hand, Bluetooth and DualShock 4 untested on hardware); see
[controller status](docs/CONTROLLERS_WIN32.md). The runtime also has queued
stereo audio with smooth slowdown; some Switch 2 audio/video paths remain incomplete. The SwitchRECOMP startup splash
is broken and disabled in this fork (build with `-DSWR_STARTUP_SPLASH` to re-enable it).

**60 FPS is a target, not a guarantee.** The documented integrated Mario Kart World race measured **58.4 FPS
(presentations/s), with stutters**. Stable minimum 60 FPS has not been achieved.
[Measurements and remaining bottlenecks](docs/MKW_PERFORMANCE_INVESTIGATION.md).

## Build from source

Run commands from the repository root. Full setup: [build environment guide](docs/BUILD_ENVIRONMENT_GUIDE.md).

Linux dependencies (Debian/Ubuntu names):

```sh
sudo apt install build-essential python3 libsdl2-dev libx11-dev libgl-dev libegl-dev \
    mingw-w64 gcc-mingw-w64-x86-64-win32 cmake ninja-build glslang-tools
```

The Windows cross-build uses the win32 thread model of `x86_64-w64-mingw32-gcc`.
Build either optional Vulkan library before its runtime:

```sh
python3 tools/build_nvn.py              # Windows: build/nvn/libnvn.a
python3 tools/build_nvn.py --linux      # Linux: build/nvn_lin/libnvn.a

cmake -S runtime/edenvk -B runtime/edenvk/build -G Ninja && ninja -C runtime/edenvk/build
cmake -S runtime/edenvk -B runtime/edenvk/buildw -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=runtime/edenvk/mingw.cmake && ninja -C runtime/edenvk/buildw
# For Eden, copy libedenvk.a and libsirit.a into runtime/edenvk/lin/ or runtime/edenvk/win/.
```

`tools/build_nvn.py` expects Vulkan-Headers in `external/deps/Vulkan-Headers` or `VULKAN_SDK`.
Without an optional renderer library, the runtime builds with the remaining renderers.

```sh
python3 switchrecomp.py --build-runtime --target win64 --backend win32 --cc x86_64-w64-mingw32-gcc
python3 switchrecomp.py --build-runtime --target linux --backend x11   --cc gcc
```

Runtime outputs go to `bin/`. On Windows, use the checkout's `.venv\Scripts\python.exe` when available.
The toolkit prefers explicit `SWR_PYTHON`, then `.venv`, `venv`, bundled Python, and PATH.

## Recompile from the command line

```sh
python3 switchrecomp.py /path/to/game -o MyGame
python3 switchrecomp.py /path/to/game.nup -o MyGame
SWR_PROD_KEYS=/path/to/prod.keys python3 switchrecomp.py /path/to/game.nsp -o MyGame
```

The folder input may contain `exefs/` and `romfs/`. Direct package builds materialize ExeFS and read RomFS
from the original supported package. Run the resulting native executable to play it.

## Renderers

| Game INI value | Renderer |
| --- | --- |
| `renderer=opengl` | Runtime GPU emulation with GLSL translation |
| `renderer=vulkan` | Native NVN calls rendered with Vulkan; live SPIR-V shader translation |
| `renderer=eden` | Runtime command processing through Eden's Vulkan GPU (separate optional source build) |

All three have Windows/Linux implementations. Vulkan caches translated shaders in `<game>_nvn/cache`.
On Linux its low-address mapping currently requires `sudo sysctl vm.mmap_min_addr=0` for games that expect
zero-filled reads through NULL pointers; this changes a system-wide protection. See [runtime instructions](README.txt).

## Further reading

- [Player manual and INI options](README.txt)
- [Code layout, diagnostics and developer guide](DEVELOPER_GUIDE.md)
- [Changelog](CHANGELOG.txt) and [regression checks](REGRESSION.md)
- [Direct NUP](docs/NUP_IN_PLACE.md) and [direct NSP](docs/NSP_IN_PLACE.md)

## Credits and license

**SwitchRecomp by MZ Labs. Copyright 2026 MZ Labs.** Turbo fork maintenance and modifications: **ChrisColeTech**.
This README describes the modified fork as of November 15, 2026; original authorship remains with the credited authors.

The project is distributed under [GNU AGPL v3](LICENSE.txt) with the additional terms in [NOTICE.txt].
Preserve those notices, the MZ Labs credit, and the unchanged, readable SwitchRECOMP startup logo/copyright screen
when distributing copies or modified builds. Eden components retain their GPL-3.0-or-later notices; other third-party
components retain their own licenses.

Credits include Mythrax-ZS, nxnative/seedds, pokopia-native, Eden/Eglantine, Lichii, Mesa, Ryujinx and the library
authors listed in [NOTICE.txt](NOTICE.txt), [third-party notices](docs/THIRD_PARTY.md) and [README.txt](README.txt).
