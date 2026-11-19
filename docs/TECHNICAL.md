# Technical details

The [README](../README.md) gives the overview. This page has the details: what the fork changes, measured performance,
building from source, the command line and the renderers.

## What Turbo adds

Turbo builds on SwitchRecomp, which translates ARM64 game code to C and compiles it into a native Windows or Linux
program. The runtime supplies the Horizon OS services and graphics the game expects. This fork adds:

- **Direct package builds:** NUP and NSP packages build directly, and RomFS data is read in place from the original
  package. See [direct NUP](NUP_IN_PLACE.md) and [direct NSP](NSP_IN_PLACE.md).
- **Combined and encrypted NSPs:** a package that holds the game with its update, and optionally its DLC (1G+1U,
  1G+1U+1D), builds directly: the update's patch is applied per read and both are decrypted with your `prod.keys`
  while the game runs, nothing is extracted. See [combined NSP](COMBINED_NSP.md).
- **DLC:** `--install-dlc` installs add-on content from NSP, NSZ, XCI, XCZ and NCA packages ([DLC](DLC.md)).
- **Graphics:** native NVN calls replayed on Vulkan with live SPIR-V shader translation, a shader cache, and frame-time
  diagnostics.
- **Video and audio:** VP9 and AV1 (dav1d) video, Opus audio, and queued stereo output with smooth slowdown. Some
  Switch 2 audio and video paths are still incomplete.
- **Controllers (Windows):** DualSense and DualShock 4 input, player lights and classic rumble, including Switch 2
  games' rumble. USB DualSense rumble is confirmed by hand; Bluetooth and DualShock 4 are untested on hardware. See
  [controller status](CONTROLLERS_WIN32.md).
- **Startup splash:** the SwitchRECOMP startup splash is broken and disabled in this fork. Build with
  `-DSWR_STARTUP_SPLASH` to re-enable it.

## Measured performance

Frame rates come from the runtime's `[vk] pacing:` log lines, one for every 300 frames shown or every 5 seconds, whichever comes first. Each launch overwrites
the game's `.log`, so read it right after closing the game. Measured with the Vulkan renderer, runtime 0.4.1,
Windows, on a 60 Hz display, 2026-11-18:

| Game | FPS while loading | FPS after loading | p95 frame time | Worst frame | Notes |
| --- | --- | --- | --- | --- | --- |
| Mario Kart World | 22–45 | 57–59 | 17–21 ms | 230–810 ms | Hitches remain, mostly texture transcodes ([details](MKW_PERFORMANCE_INVESTIGATION.md)) |
| Donkey Kong Bananza | 55 | 52–60 (dips to 45) | 17–29 ms | 130–1420 ms | Stretches where every frame takes about 19 ms (about 55 FPS), plus hitches |
| Mario Kart 8 Deluxe | 56 | 60.0 | 17.1 ms | 31 ms | 22-second run, through to a race |
| Pokémon Legends: Z-A | 55–60 | 59.5 | 17.7 ms (p50 16.8, p99 29.6) | 82 ms | 60 FPS since the base-frame-rate patch (the game asks for 30 by itself); measured while another build was running on the machine |
| The Legend of Zelda: Ocarina of Time | 60 (title, movies) | 27–50 in the first scene | 30–50 ms | seconds | Not playable: the first scene after the opening shows, then the game slows to frames of seconds and stops. Its process grows by 1 GB a second from the start (the graphics driver's memory); since 0.5.5 a cap stops the game at half of the machine's memory (12 to 32 GB, `SWR_MEMORY_CAP_GB`) instead of exhausting the PC; first-visit pipeline compiles take 40–90 s |
| Pokémon Legends: Arceus | not measured | 25–30 | 34 ms | 34–1944 ms | The game's own 30 FPS; walking around the first field works (10-minute run from a new save) |
| Pokémon Violet | not measured | 30.0 | 34 ms | 75 ms | Menus and opening at 30 FPS; crashed once at the end of the opening (audio voice list), not reproduced or fixed |

Stable minimum 60 FPS has not been achieved in every game. The ongoing investigation is in
[GAMEPLAY_INVESTIGATION.md](GAMEPLAY_INVESTIGATION.md) and [NXNATIVE_GAP_ANALYSIS.md](NXNATIVE_GAP_ANALYSIS.md).

## Build from source

Run commands from the repository root. Full setup: [build environment guide](BUILD_ENVIRONMENT_GUIDE.md).

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
zero-filled reads through NULL pointers; this changes a system-wide protection. See [runtime instructions](../README.txt).

## More documentation

- [Player manual and INI options](../README.txt)
- [Code layout, diagnostics and developer guide](../DEVELOPER_GUIDE.md)
- [Changelog](../CHANGELOG.txt) and [regression checks](../REGRESSION.md)
- Investigations: [gameplay](GAMEPLAY_INVESTIGATION.md), [nxnative gap analysis](NXNATIVE_GAP_ANALYSIS.md),
  [Mario Kart World performance](MKW_PERFORMANCE_INVESTIGATION.md), [Mario Kart 8 Deluxe boot](MK8D_BOOT_INVESTIGATION.md),
  [Pokémon Legends: Z-A black screen](PLZA_BLACK_SCREEN_INVESTIGATION.md), [AV1 decoder](AV1_DECODER.md)
- [Third-party notices](THIRD_PARTY.md)
