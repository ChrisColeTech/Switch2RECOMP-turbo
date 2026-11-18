# Bundled components and source

SwitchRecomp retains its AGPLv3 license, MZ Labs attribution, startup screen, and
`NOTICE.txt`. The project source is included in the toolkit and is also attached
to each automated release. `build-info.json` identifies the tested commit.

| Component | Notices and corresponding source |
| --- | --- |
| Qt 6.8.3 (dynamic libraries) | `licenses/qt/`; original qtbase, qtimageformats, and qtsvg source archives in `sources/`. These include their third-party sources and license notices. Qt libraries can be replaced with compatible builds. |
| GCC/G++ 16.2 and binutils 2.46.1 | `gcc/licenses/<package>/info/licenses/` and `info/recipe/` preserve upstream notices, conda build recipes, patches, exact source URLs, and checksums. GNU source: https://ftp.gnu.org/gnu/gcc/gcc-16.2.0/ and https://ftp.gnu.org/gnu/binutils/. Runtime exceptions apply where specified by the package. |
| MinGW-w64 / Linux sysroot and headers | Package-specific notices and build recipes under `gcc/licenses/`. Source locations and pinned revisions are recorded in each recipe; Linux sysroot packages identify their AlmaLinux RPM versions (https://repo.almalinux.org/vault/9.5/). |
| CPython 3.14 | Python's `LICENSE.txt`/license directory. Windows uses the official NuGet distribution; Linux uses python-build-standalone 20261110. Source and build recipes: https://github.com/python/cpython and https://github.com/astral-sh/python-build-standalone/tree/20261110. |
| .NET runtime 10.0.12 | `dotnet/LICENSE.txt` and `dotnet/ThirdPartyNotices.txt`; https://github.com/dotnet/runtime/tree/v10.0.12. |
| SPIRV-Tools 2026.4 | `shader-tools/licenses/` contains notices and the exact source/build recipe. https://github.com/KhronosGroup/SPIRV-Tools. |
| Shader translator | `tools/nxshader/xlate_bin/LICENSE-*`; translator dependencies and their versions are listed in `dmsh_xlate.deps.json`. Existing project notices apply. |
| hactool 1.4.0 | ISC license in `tools/hactool/LICENSE`; used only to decrypt and inspect NCA headers supplied by the user. Source: https://github.com/SciresM/hactool/tree/1.4.0. |
| libvpx 1.14.0 (VP9 decoder, linked statically into the runtime) | BSD-3-Clause with patent grant: `licenses/libvpx/`; the Ubuntu source package and patches in `sources/`. Built as described in `runtime/vp9/win/README.md` and `runtime/vp9/lin/README.md`. |
| dav1d 1.5.5 (AV1 decoder, linked statically into the runtime) | BSD-2-Clause: `licenses/dav1d/LICENSE-dav1d.txt`; the unmodified release archive `dav1d-1.5.5.tar.xz` in `sources/`. Built by `runtime/av1/build_dav1d.py` as described in `runtime/av1/README.md`. https://code.videolan.org/videolan/dav1d. The code that drives it (`runtime/av1video.inc`) ports logic from nxnative (MIT, (c) seedds, https://github.com/seedds/nxnative); see `NOTICE.txt`. |
| Opus decoder (libopus 1.5.2, BSD-3-Clause) | `runtime/opus/LICENSE`; source archive, hashes and build in `runtime/opus/README.md`. Linked statically into the runtime. |
| Native runtime dependencies | Notices remain with the vendored source under `runtime/`, including NVN, zstd, texture decoders, and their third-party directories. |
| Linux SDL2 and related libraries | `lib/licenses/`, from the checksum-pinned pysdl2-dll distribution. https://github.com/libsdl-org/SDL and https://github.com/py-sdl/py-sdl2-dll. |
| Linux system libraries copied into the bundle | `linux-libraries.json` records the selected files; `licenses/linux/` retains available distribution copyright/source references. Ubuntu sources are available from https://archive.ubuntu.com/ubuntu/pool/. |
| Microsoft Visual C++ runtime | Redistributable app-local DLLs supplied by VS 2022 Build Tools; Microsoft redistribution terms apply. |

`dependency-manifest.json` records download URLs and cryptographic checksums.
Retain these notices, recipes, and source-access information when redistributing a
bundle. GPU vendor drivers and operating-system components are supplied by the host.
