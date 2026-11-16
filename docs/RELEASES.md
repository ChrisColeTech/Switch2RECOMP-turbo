# Portable releases

Switch2RECOMP-turbo bundles the Qt launcher, recompiler, native NVN Vulkan runtime,
Python 3.14, GCC/G++ 16.2, .NET 10.0.12, SPIRV-Tools, VP9 decoding, and shader translator.
Windows includes app-local MSVC runtime DLLs. Linux includes Qt, its XCB/WebP plugins,
SDL2, and the shared libraries discovered during packaging.

## Run a release

- Windows x64: extract the entire ZIP into a writable folder, then open
  `Switch2RECOMP-turbo.cmd`.
- Linux x64: extract the tar.gz, then run `./Switch2RECOMP-turbo.sh`.
  The release baseline is Ubuntu 24.04 (glibc 2.39) or a compatible newer desktop.
  An X11 session or XWayland is required. Extract onto a filesystem that supports
  Unix permissions and symbolic links.

No Python, compiler, Qt, .NET, or Vulkan SDK installation is needed to build games.
The host still supplies its GPU driver, display server, fonts, desktop audio services,
and OS libraries such as glibc. The NVN texture decoder requires an x64 CPU with
AVX2, FMA, F16C, and POPCNT. A compatible Vulkan GPU/driver is needed for Vulkan games.

Keep the toolkit directory together. Its startup script selects that copy even if
another toolkit was used previously. Games launched from the library inherit the
current bundled Python, .NET, shader tools, and Linux library paths. Launch through
the library after moving the toolkit; standalone game executables can retain an old
Python marker until rebuilt. Moving the toolkit does not move the original NUPs.

Development checkouts prefer `SWR_PYTHON`, then `.venv`, then `venv`, before the bundled interpreter.
Release archives exclude development venvs, so a normal release uses its bundled Python. A valid explicit
`SWR_PYTHON` overrides either layout. No launcher/build step creates a venv or downloads Python.
The bundled interpreter lets a release recompile games and translate shaders on a PC without Python installed.
It is separate from a developer's `.venv`; copying a development venv would retain machine-specific paths.

The launcher saves the personal library in the OS application-data directory. Games,
keys, firmware, saves, artwork, logs, captures, and shader caches are not release inputs.

## Automated builds

`.github/workflows/release.yml` runs on pushes and pull requests to `main`, and can
also be started manually. Windows and Linux build independently. Each job runs the
Python and Qt tests, packages its bundle, then extracts it to a path containing spaces
and brackets, outside the checkout. With a minimal tool search path, the smoke test:

1. Verifies the archive checksum and complete per-file SHA-256 manifest, including unlisted-file detection.
   Checks required runtime headers, shader helpers, Qt plugins, licenses, source archives, and provenance.
   Rejects personal data, development venvs, and broken, absolute, or escaping symlinks.
2. Starts bundled Python/zstd, .NET, and SPIRV tools.
3. Compiles and runs a C++ program with bundled GCC.
4. Builds a synthetic NUP through the deployed Qt launcher.
5. Confirms the copied runtime matches the packaged binary, checks the Vulkan setting and NUP pointer,
   imports that build twice without duplicating the library entry, then imports the staged shader modules
   and starts the shader helper. The synthetic game is never launched.

Packaging requires linked VP9 and NVN telemetry markers in both native runtimes and Sony HID loader markers
on Windows. `tools/build_nvn.py` discovers all renderer `.c` files, including `vk_perf.c`, and builds the static
NVN archive before the runtime links it. Sony HID and SetupAPI entry points are loaded from Windows system
DLLs by the controller backend, so no third-party controller DLL is required.
The clean workflow builds the OpenGL/NVN desktop runtime, its audio mixer, and VP9 decoder. Optional
Eden, OpenH264, Opus, and SPIRV-Cross/LSFG static libraries are not built by this workflow; their presence
in a developer's working tree must not be assumed for a GitHub build.

CI also runs packaging contract tests and interpreter-selection tests on both operating systems. The Windows
batch-selection test creates disposable venv redirectors without pip, including when CI uses a base Python
installation. Windows runs isolated controller and SPIR-V regression harnesses; Linux runs the audio mixer
and recording-time harnesses. Qt core/UI tests run as part of the launcher build.

Both jobs must pass before publishing. Pushes to `main` and manual runs on `main`
create a release tagged `turbo-<run number>-<attempt>`, targeting the exact tested
commit. PRs and manual runs on other branches only upload workflow artifacts.
Each release has a Windows ZIP, Linux tar.gz, archive checksums, and a project source
archive, each with its own checksum. `build-info.json` records the commit and whether a local packaging run had
tracked or untracked content changes. The archive checks do not establish game compatibility or
GPU rendering performance.

## Build locally

Release dependency preparation needs Git and Python 3.14+ (including zstd support). Launcher builds need
Qt 6.8.3 with `qtimageformats` and `qtsvg`, and a native
C/C++ build environment. Windows needs VS 2022 Build Tools; Linux needs CMake, Ninja,
GCC/G++, X11/OpenGL development headers, and `glslang-tools`. Windows runtime builds
also need glslang on PATH; the workflow builds glslang 15.1.0 from source with VS 2022 x64.
Linux dependency preparation also requires `dpkg-dev`, Make, GCC/G++, and NASM for the pinned VP9 build.
glslang compiles the native renderer's embedded shaders during the build. Game shader translation uses
the bundled .NET translator and SPIRV-Tools and does not need a glslang installation.

```text
python scripts/release-deps.py --dest build/release-deps
```

This downloads checksum-pinned portable dependencies into a separate directory.
Both bundles retain the libvpx 1.14.0 Ubuntu `1ubuntu2.3` source package and patches under `sources/`.
Windows verifies the checked-in `runtime/vp9/win/libvpx.a` against its documented hash. Linux applies the
complete Ubuntu patch series with `dpkg-source`, then builds a static PIC, multithreaded VP9-only decoder.
The resulting `runtime/vp9/lin/libvpx.a`, configure options, compiler version and binary hash are recorded
for packaging. An existing different Linux archive is preserved and must be moved aside explicitly.
The Python archive omits the unused terminfo database because its case-sensitive
symlinks cannot be represented reliably on Windows-mounted Linux filesystems.
If an archive extraction was interrupted, remove only that incomplete dependency
subdirectory and rerun the command.

Windows, from PowerShell:

Install CMake 3.31.10 and Ninja 1.13.2 in the selected build interpreter explicitly before running the launcher
build (for example, `.venv\Scripts\python.exe -m pip install cmake==3.31.10 ninja==1.13.2`). The release workflow
has a separate dependency-install step; `build-launcher.ps1` reports missing tools instead of installing them.

```powershell
$env:PATH = "$pwd\build\release-deps\gcc\Library\bin;$env:PATH"
python tools/build_nvn.py --jobs 8
python switchrecomp.py --build-runtime --target win64 --backend win32 --cc "$pwd\build\release-deps\gcc\Library\bin\x86_64-w64-mingw32-gcc.exe"
./scripts/build-launcher.ps1 -QtPrefix C:/Qt/6.8.3/msvc2022_64
python scripts/package-release.py --deps build/release-deps --launcher bin/launcher --qt C:/Qt/6.8.3/msvc2022_64
python scripts/smoke-release.py dist/Switch2RECOMP-turbo-windows-x64.zip
```

Linux:

```sh
export SWR_QT_PREFIX=/path/to/Qt/6.8.3/gcc_64
python3 tools/build_nvn.py --linux --jobs 8
python3 switchrecomp.py --build-runtime --target linux --backend x11 --cc gcc
sh scripts/build-launcher.sh
python3 scripts/package-release.py --deps build/release-deps --launcher bin/launcher-linux --qt "$SWR_QT_PREFIX"
python3 scripts/smoke-release.py dist/Switch2RECOMP-turbo-linux-x64.tar.gz
```

Packaging refuses to merge into an existing staging directory. Use another `--output`
directory for a new local run. `--work` selects a separate staging filesystem, useful
when working from a Windows-mounted drive under WSL. `--stage-only` skips compression.
Source files come from explicit project directories and Git's tracked/unignored file
list; dependencies come only from the selected release-dependency directory. The
personal working tree is never recursively archived.

## Notices and source

The MZ Labs credit, startup screen, `LICENSE.txt`, and `NOTICE.txt` are retained.
Linux Python's matching full distribution supplies `licenses/python/` and `PYTHON.json` because the
stripped install-only archive omits them. The full archive download is checksum-pinned in
`dependency-manifest.json`; only its license texts and build metadata are added to the release.
These all-in-one fork releases intentionally exceed the upstream 20 MB lightweight
package target. See [THIRD_PARTY.md](THIRD_PARTY.md) for bundled components and source
locations. Update the download pins and source references together when upgrading.
