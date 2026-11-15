# Building And Running A NUP In Place

This guide explains direct `.nup` input on Windows and Linux. SwitchRecomp materializes the package's small ExeFS for
code translation, but does not expand or copy RomFS. The game continues reading RomFS from the original NUP while it
runs.

## Recommended Command

Windows:

```bat
scripts\build-from-nup.bat ^
  "D:\Games\Mario Kart World [0400C3F00006E000][v0].nup" ^
  --outdir "D:\Games\0400C3F00006E000-build" ^
  -o "Mario Kart World"
```

Linux:

```sh
sh scripts/build-from-nup.sh \
  "/games/Mario Kart World [0400C3F00006E000][v0].nup" \
  --outdir "/games/0400C3F00006E000-build" \
  -o "Mario Kart World"
```

Use a stable `--outdir` so generated C, object files, settings, saves, and renderer caches remain reusable if the NUP
moves. Only use dumps from games you own.

## What Stays In The NUP

The build does not modify, move, expand, or copy RomFS. It reads:

- the last package title's title ID, ExeFS directory, and RomFS directory;
- ExeFS modules and `main.npdm` needed by the recompiler and runtime loader;
- RomFS directory and file metadata;
- raw RomFS file ranges directly from the package;
- package-level zstd entries by decompressing them on demand.

At runtime, the original package must remain connected and accessible at the path recorded during the build.

## What The Build Writes

```text
0400C3F00006E000-build/
|-- Mario Kart World.exe
|-- Mario Kart World_code.dll
|-- Mario Kart World.ini
|-- Mario Kart World_build/        generated C and reusable object files
`-- Mario Kart World_data/
    |-- exefs/                     ordinary copies of code modules and main.npdm
    |-- romfs.nup                  absolute path to the original NUP
    |-- save/                      persistent saves, created on demand
    `-- sdmc/                      virtual SD card, created on demand
```

There is no `romfs/` copy. Generated code, compiled shaders, renderer caches, saves, and captures still consume space,
but none is a second full copy of game data.

## Runtime Reading

On startup, `runtime/svc_fs.c` checks sources in this order:

1. Developer directory override, when set internally.
2. `SWR_ROMFS_NUP`, when present.
3. `Game_data/romfs.nup`.
4. `Game_data/romfs.bin`.
5. `Game_data/romfs/`.

For a NUP, `runtime/nup.c` validates the package table, normalizes its RomFS into a sorted tree, and supplies that tree
to the existing RomFS image builder. The game's own SDK continues seeing a normal RomFS storage image.

Raw files use direct ranged reads. Codec-1 files are complete zstd frames; they are decoded lazily and retained in a
256 MiB least-recently-used cache. Entries larger than that limit are decoded for the current read and not retained.

## First And Incremental Builds

A first build:

1. Validates the NUP header, table bounds, names, tree, file ranges, and codecs.
2. Reads ExeFS modules and `main.npdm` from the package.
3. Scans RomFS for deferred `.nro` modules without extracting the tree.
4. Analyzes ARM64 code and generates C.
5. Compiles the native code module and runtime.
6. Materializes ExeFS and writes `Game_data/romfs.nup`.

Later builds reuse unchanged generated C and object files. The current recompiler does not use a separate stage-key
database, so console output and file timestamps are the practical indication of reuse.

| Change | Next build or launch |
|---|---|
| Same NUP and output directory | Reuses generated C and current objects where inputs are unchanged |
| NUP moved, same output directory | Re-run the wrapper to update `romfs.nup`; code can still be reused |
| NUP moved temporarily | Set `SWR_ROMFS_NUP` for the launch instead of editing the marker |
| Different game version | Re-run the build; changed NSOs are retranslated and copied |
| Runtime source changed | Rebuild the runtime or omit `--runtime` so the game build does it |
| Entire output directory deleted | Full recompile; saves and caches there are also lost |

## Moving The NUP

1. Close the game. The runtime can have the package open and compressed entries cached.
2. Move or rename the NUP.
3. Re-run `build-from-nup` with the new exact path and the same `--outdir`.
4. Launch the existing executable after the build updates `Game_data/romfs.nup`.

For a one-off launch, override the recorded path:

```bat
set "SWR_ROMFS_NUP=E:\Games\Game.nup"
"D:\Games\Game-build\Game.exe"
```

```sh
SWR_ROMFS_NUP=/media/games/Game.nup /games/Game-build/Game
```

Do not launch with a stale marker and expect automatic package discovery. The runtime deliberately opens only the
recorded or overridden path.

## Storage Layouts

### External NUP, Internal Build

Recommended when the NUP is large and the internal disk should retain fast generated code and caches:

```text
External SSD:  Game.nup
Internal SSD:  Game-build/
```

The external SSD must remain connected while playing. A slow flash drive can stall every uncached resource read.

### NUP And Build On External SSD

Use this when internal storage is limited:

```text
External SSD:
|-- Game.nup
`-- Game-build/
```

NTFS, ext4, and other filesystems supporting large files are suitable. FAT32 is not: one file cannot exceed 4 GiB.
The direct-NUP design does not require symbolic links, so exFAT can hold both the package and generated build, though
native filesystems are preferable for many generated object and cache files.

## Package Format Used

The direct reader supports the indexed Switch 2 NUP layout identified by `NUP\x01` and `TOC\x01`:

- one or more 64-byte title records; the last record with valid ExeFS and RomFS indexes is selected, so trailing
  auxiliary-content records do not replace the executable game title;
- 24-byte directory records;
- 80-byte file records;
- a child-index list and UTF-8 name pool;
- codec 0 raw ranges or codec 1 whole-file zstd frames.

This is not an NSP, XCI, NCA, or arbitrary file renamed to `.nup`. Unsupported or structurally damaged package
metadata fails validation before code analysis starts. File I/O and zstd errors are reported when payloads are read;
the format's 32-byte file-record field is retained but not interpreted because its digest semantics are not publicly
specified.

## Troubleshooting

| Message or behavior | Action |
|---|---|
| `no NUP magic` | Verify this is an indexed NUP package, not NSP/XCI/NCA |
| `table of contents is out of range` | The package is truncated or damaged; dump it again |
| `invalid codec or stored size` | The package uses an unsupported entry encoding or is corrupt |
| `RomFS: cannot open NUP` | Connect the drive, restore the path, rebuild the marker, or set `SWR_ROMFS_NUP` |
| Game starts, then files read as zero | Check the log for NUP read/decompression failures and verify package integrity |
| First access to a packed file pauses | Expected for a large uncached zstd entry; later reads may use the 256 MiB cache |
| Windows cannot open a path containing non-ASCII characters | Move the NUP to a path representable in the active Windows code page |

Implementation tests are `python tests/test_nup.py` and, on Linux or WSL, `sh tests/run_nup.sh`.
