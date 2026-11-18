# AV1 decoder (dav1d)

`av1video.inc` (included by `svc_ns2.c`) decodes the AV1 movies Switch 2 games play through the video decoder
service (`nn::codec`, `InitializeAv1DecoderByTransferMemory`): The Legend of Zelda: Ocarina of Time's title and
opening movies are AV1. It drives [dav1d](https://code.videolan.org/videolan/dav1d), VideoLAN's AV1 decoder
(BSD-2-Clause, `LICENSE-dav1d.txt`, unmodified). How dav1d is driven is ported from
[nxnative](https://github.com/seedds/nxnative)'s `dm/host/hle_movie_av1.c` (MIT, (c) seedds); see
`docs/AV1_DECODER.md`.

`win/libdav1d.a` and `lin/libdav1d.a` are built from dav1d's release archive by `build_dav1d.py`, with both bit
depths and the x86 assembly (SSE2 to AVX-512, picked at run time from the CPU). `switchrecomp.py --build-runtime`
links the one for the target when it exists and defines `SWR_HAVE_AV1`. Without it the runtime still builds, and an
AV1 movie runs its length with black pictures.

## Source

| | |
| --- | --- |
| Version | dav1d 1.5.5 ("Sonic"), tag `1.5.5`, commit `6c7bee685677fd997a431302be4fea8ea4017606` (2026-11-16) |
| Archive | `https://downloads.videolan.org/pub/videolan/dav1d/1.5.5/dav1d-1.5.5.tar.xz` |
| SHA-256 | `ca96b1d0ba3f01f55de7231d775892b64994ba061aaeeaaa4f662a390b3abf9e` (the same as VideoLAN's `dav1d-1.5.5.tar.xz.sha256`) |
| License | BSD-2-Clause: `LICENSE-dav1d.txt` is the archive's `COPYING` |

No prebuilt dav1d binary was used and the source is not patched. `include/dav1d/` holds the archive's public headers
(`common.h`, `data.h`, `dav1d.h`, `headers.h`, `picture.h`, `version.h`), unchanged.

## The archives

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `win/libdav1d.a` | 3,387,086 | `f64d2ee3a9846c40c08fa7b6992ae53b8ccc7384b9a4023ca7695419ce46f96a` |
| `lin/libdav1d.a` | 3,584,964 | `f7d3af175b5cac993fdd0146db610f8e0a58b2b56820cf7dd5f5c6cc7a2320ca` |

RecompLauncher's `scripts/release-deps.py` pins both hashes and its packaging refuses a release whose archive
differs; `tests/test_av1.py` checks the archive of the host against this table.

## Reproducing the build

`build_dav1d.py` downloads the source archive (and, on Windows, NASM), checks their SHA-256, installs meson and
ninja at fixed versions into a virtual environment inside its work folder (`build/dav1d` by default; nothing is
installed system-wide), builds, and copies the archive, the headers and the license here. It prints the archive's
size and SHA-256. Two builds from fresh work folders gave identical bytes on each system: the objects hold no build
path (`-ffile-prefix-map`, and the build always runs in `<work>/src` and `<work>/out`) and no time stamp
(`NASMENV=--reproducible`).

meson options, the same on both systems:

```
--buildtype=release --default-library=static -Dbitdepths=8,16 -Denable_asm=true -Denable_tools=false
-Denable_tests=false -Denable_examples=false -Dlogging=true -Db_lto=false -Db_staticpic=true
-Dc_args=-ffile-prefix-map=../src=dav1d
```

Tools, both systems: meson 1.12.1 and ninja 1.13.2 (from PyPI), Python 3.12.

### Windows

The checked-in archive was built on Windows 10 with:

- GCC 16.2.0 (`conda-forge gcc 16.2.0-7`, thread model win32) and GNU Binutils 2.46.1: the MinGW-w64 toolchain
  `Install.bat` / `tool/get_deps.py gcc` puts in `gcc/Library/bin`, the one that builds the runtime
- NASM 2.16.03 from `https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip`
  (SHA-256 `3ee4782247bcb874378d02f7eab4e294a84d3d15f3f6ee2de2f47a46aa7226e6`), downloaded by the script

```powershell
python runtime\av1\build_dav1d.py --gcc-bin gcc\Library\bin
```

dav1d uses its own Win32 threads (`src/win32/thread.c`: SRW locks and condition variables), so, unlike libvpx, it
decodes on several threads with this toolchain's win32 thread model.

### Linux

The checked-in archive was built on Ubuntu 24.04 (x86_64, WSL) with GCC 13.3.0
(`gcc-13 (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`, the Ubuntu 24.04 default), GNU Binutils 2.42 and NASM 2.16.01
(Ubuntu `nasm`):

```sh
sudo apt-get install build-essential nasm python3-venv
python3 runtime/av1/build_dav1d.py            # --cc gcc-13 where gcc is another version
```

The archive is position independent. It needs glibc's pthreads, `dlsym` and `libm`, which the runtime links already.

## Verification

```sh
python tests/test_av1.py
```

compares the archive's SHA-256 with the table above, then builds `tests/test_av1.c` against the archive and
`av1video.inc` and decodes two synthetic streams (`tests/av1/vectors.h`: ffmpeg's test pattern encoded with libaom,
8-bit 4:2:0 and 10-bit 4:4:4; no game data): every picture has to come out once, in order, with its unit's time, as
the NV12 libaom's decoder gives, on one frame at a time and on frame threads, with drains in mid-stream.

```sh
sha256sum runtime/av1/lin/libdav1d.a runtime/av1/win/libdav1d.a
objdump -f runtime/av1/lin/libdav1d.a      # every member: elf64-x86-64 (win: pe-x86-64)
nm -g --defined-only runtime/av1/lin/libdav1d.a | grep ' T dav1d_'
```

The global symbol table defines `dav1d_open`, `dav1d_send_data`, `dav1d_get_picture`, `dav1d_flush`, `dav1d_close`
and `dav1d_version`. A runtime that links it contains `dav1d %s decoding AV1` (the service's log line) and dav1d's
own `Error parsing OBU data`; engine CI and the launcher's packaging check for both.

## Updating dav1d

Change `VERSION`, `COMMIT` and the archive's SHA-256 in `build_dav1d.py`, run it on Windows and on Linux, then
update this file's tables, `DAV1D_ARCHIVES` and `DAV1D_SOURCE` in RecompLauncher's `scripts/release-deps.py`, and
run `python tests/test_av1.py` on both systems.
