# Opus decoder (libopus)

`svc_core.c` (the `hwopus` service) decodes the Opus packets a game hands to the console's hardware decoder
(`nn::codec::HardwareOpusDecoder`): the sound of Ocarina of Time's title and opening movies (six mono streams, 48 kHz),
streamed music and voices of other games. It drives [libopus](https://opus-codec.org) (BSD-3-Clause, `LICENSE`,
unmodified).

`win/libopus.a` and `lin/libopus.a` are built from the release archive by `build_opus.py`.
`switchrecomp.py --build-runtime` links the one for the target when it exists; without it the runtime is built with
`SWR_NO_OPUS` and every packet decodes to silence of the right length. Until 2026-11-18 the repository held only the
headers, so every runtime built from it was silent there (the log now says `audio: opus decoder ...` when the decoder
is real, and `audio: this runtime was built without libopus ...` when it is not).

## Source

| | |
| --- | --- |
| Version | Opus 1.5.2 |
| Archive | `https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz` |
| SHA-256 | `65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1` (the same as xiph.org's `SHA256SUMS.txt`) |
| License | BSD-3-Clause: `LICENSE` is the archive's `COPYING` |

No prebuilt binary was used and the source is not patched. `include/` holds the archive's public headers (`opus.h`,
`opus_defines.h`, `opus_multistream.h`, `opus_types.h`), unchanged.

## The archives

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `win/libopus.a` | 583,600 | `dbd1fe710cdbd7ccbe0899ef7301c31725fcb32d55ccdeef4df028755bd79bbc` |
| `lin/libopus.a` | 678,108 | `fa5f11d91a2ddeba1ee594b9893e3966d8424753a9d2956a8db5bc2789b2d170` |

`tests/test_opus.py` checks the archive of the host against this table and decodes a tone through it.

## Reproducing the build

```
python runtime\opus\build_opus.py --gcc-bin gcc\Library\bin          (Windows)
python3 runtime/opus/build_opus.py                                   (Ubuntu 24.04: build-essential)
```

`build_opus.py` downloads the archive, checks its SHA-256 and compiles the sources Opus's own makefiles list
(`celt_sources.mk`, `silk_sources.mk`, `opus_sources.mk`: the float build, 137 files) with

```
-O2 -g0 -w -fno-math-errno -DOPUS_BUILD -DUSE_ALLOCA -DHAVE_LRINTF -DHAVE_LRINT -DPACKAGE_VERSION="1.5.2"
-ffile-prefix-map=.=opus -Iinclude -Icelt -Isilk -Isilk/float -I.          (Linux: also -fPIC)
```

in `<work>/src` (default `build/opus`), then `ar rcsD`. Plain C: no run-time CPU detection, no intrinsics, no deep
PLC. Two builds from fresh work folders gave identical bytes on each system.

- Windows archive: GCC 16.2.0 (`conda-forge gcc 16.2.0-7`, MinGW-w64), GNU Binutils 2.46.1, Windows 10.
- Linux archive: GCC 12.4.0 (`12.4.0-2ubuntu1~24.04.1`), GNU Binutils 2.42, Ubuntu 24.04 (WSL).
