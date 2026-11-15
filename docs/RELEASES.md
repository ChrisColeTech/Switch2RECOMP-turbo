# Portable releases

Switch2RECOMP-turbo bundles the Qt launcher, recompiler, native NVN Vulkan runtime,
Python 3.14, GCC/G++ 16.2, .NET 10.0.12, SPIRV-Tools, and shader translator.
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

The launcher saves the personal library in the OS application-data directory. Games,
keys, firmware, saves, artwork, logs, captures, and shader caches are not release inputs.

## Automated builds

`.github/workflows/release.yml` runs on pushes and pull requests to `main`, and can
also be started manually. Windows and Linux build independently. Each job runs the
Python and Qt tests, packages its bundle, then extracts it to a path containing spaces
and brackets, outside the checkout. With a minimal tool search path, the smoke test:

1. Verifies the per-file SHA-256 manifest.
2. Starts bundled Python/zstd, .NET, and SPIRV tools.
3. Compiles and runs a C++ program with bundled GCC.
4. Builds a synthetic NUP through the deployed Qt launcher.
5. Checks the native artifacts and NUP pointer, then starts the staged shader helper.

Both jobs must pass before publishing. Pushes to `main` and manual runs on `main`
create a release tagged `turbo-<run number>-<attempt>`, targeting the exact tested
commit. PRs and manual runs on other branches only upload workflow artifacts.
Each release has a Windows ZIP, Linux tar.gz, archive checksums, and a project source
archive. `build-info.json` records the commit and whether a local packaging run had
tracked content changes. The archive checks do not establish game compatibility or
GPU rendering performance.

## Build locally

Developer builds need Git, Python 3.12+, Qt 6.8.3 with `qtimageformats`, and a native
C/C++ build environment. Windows needs VS 2022 Build Tools; Linux needs CMake, Ninja,
GCC/G++, X11/OpenGL development headers, and `glslang-tools`. Windows runtime builds
also need glslang on PATH; the workflow builds glslang 15.1.0 from source.

```text
python scripts/release-deps.py --dest build/release-deps
```

This downloads checksum-pinned portable dependencies into a separate directory.
The Python archive omits the unused terminfo database because its case-sensitive
symlinks cannot be represented reliably on Windows-mounted Linux filesystems.
If an archive extraction was interrupted, remove only that incomplete dependency
subdirectory and rerun the command.

Windows, from PowerShell:

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
These all-in-one fork releases intentionally exceed the upstream 20 MB lightweight
package target. See [THIRD_PARTY.md](THIRD_PARTY.md) for bundled components and source
locations. Update the download pins and source references together when upgrading.
