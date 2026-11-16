# Build Environment Guide

This guide covers a fresh checkout through a built runtime and a game recompiled directly from a `.nup` package.
The scripts resolve the repository from their own location, so they can be called from cmd, PowerShell, Git Bash,
or another working directory. Game files, keys, and firmware are not included or downloaded.

## 1. Toolchain

| Tool | Supported version | Source |
|---|---:|---|
| Python | Existing project venv for development; 3.14.9 bundled in releases | `.venv` or `venv`; explicit setup for missing portable tools |
| GCC/G++ | 16.2 portable package | conda-forge packages verified by SHA-256 in `tool/get_deps.py` |
| zstd decoder | 1.6.0 source | `runtime/edenvk/deps/zstd`, linked into every runtime |
| SDL2 | 2.32.10 on Linux | portable wheel installed by `Install.sh` |

Windows setup is portable. It writes `python\` and `gcc\` under the repository and requires no administrator
access. Linux can use system Python and GCC or the portable compiler downloaded by `Install.sh`.

Development scripts and launchers select a valid explicit `SWR_PYTHON` first, then `.venv`, then `venv`, then
the portable `python` directory, then PATH. Windows venvs use `Scripts/python.exe`; Linux uses `bin/python3`.
Python subprocess helpers use the current interpreter as their final fallback. Invalid explicit overrides fail.
No activation or system PATH change is needed. Build scripts do not create/download interpreters or venvs.
The launcher build also requires CMake/Ninja already installed in the selected environment.
Portable releases contain no development venv and continue to use their bundled Python.

The Vulkan renderers have additional dependencies described in `README.md`. They are optional; the standard
OpenGL runtime and direct-NUP filesystem do not require Vulkan or .NET.

## 2. One-Time Setup

Windows:

```bat
scripts\setup.bat
```

Linux:

```sh
sh scripts/setup.sh
```

The setup scripts call the existing `Install.bat` or `Install.sh`. Downloads are checksum-pinned where the project
provides portable packages. Re-running setup is safe: already installed components are reused.

## 3. Scripts

| Script | Purpose |
|---|---|
| `scripts\_env.bat` | Finds the project root, Python, and GCC for other Windows scripts |
| `scripts\setup.bat` | Installs portable Windows Python and GCC |
| `scripts\build-runtime.bat` | Builds `bin\runtime_win64_win32.exe` |
| `scripts\build-from-nup.bat` | Recompiles a NUP for Windows and leaves its RomFS in the package |
| `scripts\build-from-nsp.bat` | Recompiles a supported application NSP for Windows and leaves its RomFS in the package |
| `scripts/setup.sh` | Installs Linux dependencies |
| `scripts/build-runtime.sh` | Builds `bin/runtime_linux_x11` |
| `scripts/build-from-nup.sh` | Recompiles a NUP for Linux and leaves its RomFS in the package |
| `scripts/build-from-nsp.sh` | Recompiles a supported application NSP for Linux when hactool is installed |

Every runnable script exits 0 on success and nonzero on failure. All arguments after the NUP path are passed to
`switchrecomp.py`, including `-o`, `--outdir`, `-O`, `--jobs`, `--runtime`, and `--native`.

The Qt desktop library that builds and launches games lives in the separate RecompLauncher repository.

## 4. Build The Runtime

Windows:

```bat
scripts\build-runtime.bat
```

Linux:

```sh
sh scripts/build-runtime.sh
```

The direct-NUP backend and its decoder are part of every runtime. Existing root-level commands still work:

```sh
python3 switchrecomp.py --build-runtime --target linux --backend x11 --cc gcc
python3 switchrecomp.py --build-runtime --target linux --backend headless --cc gcc
```

```bat
scripts\build-runtime.bat
```

## 5. Build From A NUP

For NSP inputs, install `prod.keys` from the Qt launcher's **Keys > Install keys** menu, or set
`SWR_PROD_KEYS`, then use the matching `build-from-nsp` script. See [NSP_IN_PLACE.md](NSP_IN_PLACE.md).

Use a stable output directory. The generated executable, native code module, settings, saves, and caches live there;
the original NUP remains the only RomFS copy.

Windows:

```bat
scripts\build-from-nup.bat "D:\Games\Game [title][v0].nup" --outdir "D:\Games\Game-build" -o Game
```

Linux:

```sh
sh scripts/build-from-nup.sh "/mnt/games/Game [title][v0].nup" --outdir "/mnt/games/Game-build" -o Game
```

The first build performs code analysis, C generation, compilation, and linking. Later builds reuse unchanged generated
C and object files in `<Game>_build/`. They do not extract or copy RomFS.

The output contract is:

```text
Game-build/
|-- Game.exe                 Windows runtime, or Game on Linux
|-- Game_code.dll            recompiled code, or Game_code.so on Linux
|-- Game.ini
|-- Game_build/              generated C and reusable objects
`-- Game_data/
    |-- exefs/               small package ExeFS files, materialized for the loader
    |-- romfs.nup            text file containing the original package path
    |-- save/                created when the game saves
    `-- sdmc/                created when requested
```

At runtime, `Game_data/romfs.nup` directs the filesystem service to the original package. Raw entries are read with
ranged reads. Whole-file zstd entries are decompressed on demand and kept in a bounded 256 MiB LRU cache.

## 6. Daily Workflow

```bat
scripts\build-from-nup.bat "D:\Games\Game.nup" --outdir "D:\Games\Game-build" -o Game
"D:\Games\Game-build\Game.exe"
python tests\test_nup.py
```

```sh
sh scripts/build-from-nup.sh /games/Game.nup --outdir /games/Game-build -o Game
/games/Game-build/Game
python3 tests/test_nup.py
sh tests/run_nup.sh
```

Use an existing runtime to avoid rebuilding it while iterating on game code:

```bat
scripts\build-from-nup.bat "D:\Games\Game.nup" --outdir "D:\Games\Game-build" --runtime "bin\runtime_win64_win32.exe"
```

## 7. Build Contracts

| Contract | Reason |
|---|---|
| ExeFS files remain byte-identical to the package entries | `runtime/loader.c` validates NSO build IDs |
| `Game_data/romfs.nup` contains one absolute path | The runtime opens the package directly on every launch |
| NUP codec 0 has `stored size == logical size` | Raw reads address package bytes without transformation |
| NUP codec 1 is one zstd frame | Random reads use a whole-entry decompression cache |
| Last title with valid ExeFS and RomFS indexes is selected | Packages may end with auxiliary content whose ExeFS index is `0xFFFFFFFF` |
| Existing `romfs.bin` and `romfs/` inputs remain supported | NRO and extracted-folder workflows are unchanged |

`SWR_ROMFS_NUP` overrides the path in `Game_data/romfs.nup` for one launch. This is useful after moving a package or
mounting the same external drive at a different path.

## 8. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `[ENV] FAILED - Python not found` | setup was not run | Run `scripts\setup.bat` |
| `[ENV] FAILED - GCC not found` | portable compiler is absent | Run setup again and check network access |
| `Cannot load the game: ... no NUP magic` | wrong file or damaged package | Select the exact `.nup` file and verify the dump |
| `reading compressed NUP entries needs...` | older system Python lacks zstd | Use bundled Python 3.14 or install `zstandard` |
| `RomFS: cannot open NUP ...` | package moved, renamed, or drive disconnected | Rebuild, edit `Game_data/romfs.nup`, or set `SWR_ROMFS_NUP` |
| NUP opens on one Windows path but not another | runtime filesystem paths use the active Windows code page | Use an ASCII path, or one representable in the active code page |
| Game opens but data reads fail | package became unavailable while running | Reconnect it at the same path and restart the game |
| Build reports module mismatch at launch | stale ExeFS beside newly recompiled code | Re-run the NUP build into the same output directory |
| Link or generated-code errors after source changes | stale reusable objects | Delete only `<Game>_build/`, then rebuild |

## 9. Verification Record

Verified on 2026-11-14:

| Check | Result |
|---|---|
| `scripts\setup.bat` | Installed portable Python 3.14.9 and GCC/G++ 16.2.0; C++ compiler probe passed |
| `scripts\build-runtime.bat` | Built `bin\runtime_win64_win32.exe` successfully |
| Windows `build-from-nup.bat` smoke build | Generated C, compiled and linked `TestGame_code.dll`, staged ExeFS plus `romfs.nup`, and produced a PE32+ x86-64 executable |
| `python tests/test_nup.py` | 6 tests passed: title selection, raw/zstd reads, bounded decode, safe ExeFS staging, deferred NRO, marker staging, corruption rejection |
| `sh tests/run_nup.sh` under WSL | PASS with AddressSanitizer and UndefinedBehaviorSanitizer |
| Linux headless runtime build, GCC 12.4 | `bin/runtime_linux_headless` built successfully with the NUP backend |

### Vulkan deployment and black-screen troubleshooting

Build the optional NVN layer before rebuilding the Windows runtime:

```powershell
Set-Location -LiteralPath "D:\Projects\Switch2RECOMP-main"
& ".\.venv\Scripts\python.exe" tools/build_nvn.py --jobs 8
& ".\scripts\build-runtime.bat"
```

Then rerun the game build wrapper so the updated runtime and shader helper are deployed. Set `renderer=vulkan` in
the game's `[Graphics]` settings. A successful runtime link alone is insufficient: Vulkan needs the live shader
translator beside a game outside the source tree.

NVN-enabled Windows game builds now stage `tools/nxshader/` under the output directory, including the .NET translator
DLLs, `dmsh_abi.h`, and `python.path`. The latter records the Python executable used during the build. That interpreter
and the .NET 10 runtime must remain available. Rebuild after relocating the toolchain to refresh the path.

The helper writes `<Game>_nvn/cache/nxs_live.log`. `live shader translator started` in the game log only confirms process
creation; check the helper log for `ready` and successful translation counts. A game that presents black frames with
`0 shaders` and `no shader` draw skips is not necessarily stalled: it may be rendering without translated programs.

The Windows helper now monitors its parent with a read-only synchronization handle. Do not use `os.kill(pid, 0)` as a
Windows liveness probe; on Windows it can terminate the target process. Regression check:

```powershell
& ".\.venv\Scripts\python.exe" tests/test_nxs_parent.py
```

Verified on this machine after fixing deployment: 102 captured Bananza shaders translated with zero failures; a
bounded Vulkan/RTX 5090 run produced a visible, textured 1920x1080 title-screen capture at present 1000. The log reported
102 usable shaders, zero missing-shader draw skips in that sequence, and approximately 60 presents/s after startup.
This is a title-screen result, not a claim of full gameplay correctness or sustained gameplay performance. Native
DualSense input was confirmed by the user; vibration output remains unimplemented.
