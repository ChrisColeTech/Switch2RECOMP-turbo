# Switch2RECOMP Turbo launcher

This enhanced fork is maintained by **ChrisColeTech**, based on **SwitchRecomp by MZ Labs**. It is a static
recompiler and desktop build/library utility, not an emulator product. Add your own
game dumps, build them, then launch the finished games from the same library. No per-game desktop shortcuts
are needed. The library is saved automatically and is restored the next time you open the launcher.

Start with the [screenshot walkthrough](../README.md#start-the-launcher). Its images are captures of the actual
Qt widgets using demonstration data, not game compatibility results. No games, keys or firmware are included.

## Start it

For all-in-one releases, run `Switch2RECOMP-turbo.cmd` (Windows) or
`./Switch2RECOMP-turbo.sh` (Linux). See [RELEASES.md](RELEASES.md) for platform requirements.
Download bundles from [the latest Turbo release](https://github.com/ChrisColeTech/Switch2RECOMP-turbo/releases/latest).
The following path is for a Windows developer checkout:

```powershell
& ".\bin\launcher\SwitchRecompLauncher.exe"
```

The entire `bin/launcher/` directory contains the Qt DLLs and plugins needed by the frontend. Do not move
only the EXE. The toolkit is detected by looking above the launcher's directory for `switchrecomp.py`.
If you relocate the frontend separately, choose the toolkit under **Tools → Settings → Toolkit**.

## Add, build, play

1. **Add game:** select a `.nup`, supported `.nsp`, or `.nro`, or drop it into the library. The dropdown also accepts extracted
   game folders and existing native builds.
2. In the add dialog, choose the display name under **General** and build location under **Build**, then click
   **Add game**. Later use the card's sliders button or **Alt+Enter** for Properties; save edits with **OK**.
3. Click **Build** on the game's card. Live output appears in a dock that can be resized, hidden, or undocked. Progress stays
   in the status bar even when the dock is hidden.
4. After a successful build, the card's button changes to **Play**.
5. Click **Play**, double-click the cover/list row, or press Enter on the selection. Subsequent sessions use
   the existing build; launching does not recompile it.

To add a game you already compiled, choose **File → Import build…** and select its EXE. The corresponding
`_code.dll` and `_data` folder must be next to it. On Linux select the extensionless executable with its
matching `_code.so`. NUP sources are recovered from `_data/romfs.nup` when present.

The original NUP must remain accessible while playing. Changing the source in Properties requires a rebuild.
The same applies to direct NSP builds. Before building an NSP, choose **Keys > Install keys** and select your legally
dumped `prod.keys`. The launcher copies it to its private application-data folder, not the toolkit or game build.
Removing a library entry does not delete its game, source, saves, or caches. Cancelled/failed builds are kept
marked as needing a build rather than treating partially overwritten artifacts as ready.

### Supported inputs and keys

The package picker explicitly offers `*.nup *.nsp *.nro`. The launcher checks a `.nsp` for PFS0 magic and labels it
**DIRECT NSP**. This is an import check, not full package validation. The recompiler supports application NSPs with
an NCA3 Program archive, plaintext PartitionFS ExeFS and plaintext IVFC RomFS. Encrypted filesystem sections,
rights-managed data, sparse/compressed layouts and BKTR update-only packages are unsupported. See
[NSP_IN_PLACE.md](NSP_IN_PLACE.md#supported-packages) for hactool/key requirements and
[NUP_IN_PLACE.md](NUP_IN_PLACE.md) for indexed NUP restrictions. Renaming another format does not convert it.

**Add extracted folder…** accepts a folder containing `main`, `exefs/main`, or `ExeFS/main`; keep its corresponding
game data available. **Import build…** accepts native artifacts rather than source packages.

The key installer checks for a header-key name and copies the selected file atomically to
`keys/prod.keys` inside the launcher's application-data directory. It does not verify that all key values are
correct. **Keys installed** confirms a copy, not successful decryption. The launcher passes the installed path
as `SWR_PROD_KEYS` to builds/games; key contents are not copied into build outputs. Missing keys block NSP builds
with **NSP builds need prod.keys. Use Keys > Install keys first.**

## Desktop layout

- Native Windows title bar and window controls.
- File, Game, Build, Keys, View, Tools, and Help menus.
- The command strip contains **Add game** and **Search & Filter**.
- Compact artwork cards with the title in the footer. Only unbuilt/problem entries show a corner badge.
- Each card has its own **Play / Build / Stop / Cancel build** button and a Properties button.
- The sortable list fills the central area edge-to-edge. Its context menu and Game/Build menus provide the same actions.
- **Search & Filter** contains search, state/source filters, sorting, Gallery/List selection, and the card-size slider.
- Active filters are counted on that command and in the status bar; **Clear filters** resets the view.
- Compact status bar showing selection, toolchain state, and active-build progress.
- **Settings** is at the bottom right of the status bar and remains available through the Tools menu.
- Dockable build output, opened by **Build → Show build output** or **Ctrl+L**.

There is no dashboard or permanent inspector. **Properties** holds General, Build, Runtime, and Artwork tabs.
Global tool paths, default build storage, worker count, and backdrop visibility are in **Tools → Settings**.

**Settings → Appearance** offers Dark/Light and Lime, Mint, Violet, Rose, or Cyan accents. Changes preview
immediately; OK saves them and Cancel restores the previous appearance. Focus/selection uses surface changes
instead of outline borders. Each theme uses appropriate foreground colors rather than simply inverting the
dark palette. Tests check primary/secondary text and accent-button text contrast for all ten combinations;
this is not a claim of a complete accessibility audit.

### Cover art and backgrounds

Open **Properties → Artwork** to choose a cover image and a separate background image. The selected game's
background is dimmed behind the gallery; if none is specified, its cover is used. Turn this off from
**View → Show selected game background** or Appearance settings. Images stay at their selected paths.

An unbranded gamepad placeholder appears when no image is assigned. Existing game icons are used when available.
PNG, JPEG, WebP, and other supported Qt formats can be used. The Qt Image Formats module, including `qwebp.dll`,
is required and deployed by the build script. Images that cannot be decoded produce a clear error when assigning
them. Artwork is cached by path/size/modification time; saving Properties or pressing F5 also invalidates the cache.
Preview/demo entries are not saved and do not contain bundled games or downloadable artwork.

## Build and runtime options

**Properties → Build** contains the output folder, compiler workers (0 inherits the global default), and
optimization level. O2 is the default. The output filename is fixed for an existing entry so renaming a
display title does not silently rename native modules and save directories.

**Properties → Runtime** selects Vulkan/native NVN or OpenGL. The renderer value is written to the game INI
on the next launch without discarding other settings. Vulkan needs a runtime built with NVN, deployed shader
tools, Python, and .NET 10. The launcher checks the latter tools and reports missing setup instead of silently
launching a Vulkan game with no shader translator.

The frontend uses the toolkit's prebuilt runtime and calls `switchrecomp.py` through `QProcess` with an argument
list. Spaces, brackets, and shell metacharacters in paths are not passed through a shell. Game processes are
detached, so closing the launcher does not terminate them. Windows running-state detection matches the full EXE
path, including games started outside the launcher. **Cancel build** affects only that card's compilation;
**Stop** requests a normal close of that card's game, after rechecking the process path. It does not force-kill
games. Properties remain available during a run/build for artwork and display-name edits; source/build/runtime
controls are locked until that operation finishes.

Controller, audio, graphics, and vibration support belong to the underlying runtime. The fork includes Windows
Sony HID input and classic-rumble/player-light code; physical output checks remain pending. See
[CONTROLLERS_WIN32.md](CONTROLLERS_WIN32.md). Queued stereo audio and smooth slowdown are available, while some
Switch 2 audio/video paths remain incomplete. The performance target is 60 FPS, not a guarantee: the documented
integrated race measured about 58.4 presentations/s with stutters, not stable minimum 60 FPS. See
[the performance investigation](MKW_PERFORMANCE_INVESTIGATION.md).

## Library storage and keyboard shortcuts

### Existing compiled games

Builds created from the command line are not automatically library entries. Use **File > Import build…** and
select the game's executable, or **File > Discover builds…** and select its build folder (or the parent containing
several builds). Discovery checks that folder and its immediate subfolders, without walking through game data.
Each build needs the executable, matching `_code.dll` / `_code.so`, and matching `_data` directory.
The importer reads `romfs.nsp` / `romfs.nup` to associate the source and reads the renderer from the game's INI.
It does not compile, copy, or launch the game. An unbuilt entry for that source receives the build paths while
keeping its title, artwork and build preferences. Separate completed builds can coexist; repeated imports of
the same executable do not duplicate it.

For example, import `build/mkw-nsp-1.8/Mario Kart World 180.exe`. It appears as **Mario Kart World 180**,
format **DIRECT NSP**, with **Play** when the NSP referenced by `_data/romfs.nsp` is available. The original
NSP must stay at that path because RomFS is read from it in place. Select the card's **Play** button or double-click
the card. Direct launch uses that same EXE; from PowerShell, set the interpreter for older runtime copies first:

```powershell
$env:SWR_PYTHON = (Resolve-Path '.\.venv\Scripts\python.exe').Path
Start-Process -FilePath '.\build\mkw-nsp-1.8\Mario Kart World 180.exe' -WorkingDirectory '.\build\mkw-nsp-1.8'
```

The current Qt launcher passes its selected Python to the game automatically, including older runtime copies.
Direct-launch interpreter discovery changes in `runtime/nvn_hook.c` require a later runtime rebuild/deployment.

The default library is `library.json` under Qt's local application-data directory for
`MZLabs/SwitchRecompLauncher` (normally `%LOCALAPPDATA%\MZLabs\SwitchRecompLauncher`). It stores paths,
display metadata, per-game build options, view preferences, and window/dock layout. Writes are atomic.
An unreadable/corrupt library is not silently overwritten.

| Shortcut | Action |
|---|---|
| Ctrl+O | Add NUP/NSP/NRO |
| Ctrl+B | Build selected game |
| Ctrl+F | Open Search & Filter and focus title search |
| Esc | Close the search/filter popout |
| Ctrl+L | Show build output |
| Alt+Enter | Game Properties |
| Enter / double-click | Play ready game; open build options if unbuilt |
| F5 | Refresh library and tools |

## Build the launcher

Windows requirements: Visual Studio 2022 C++ x64 tools, Qt 6 MSVC x64 (Core, Gui, Widgets, Test, and the Qt Image Formats
module), and Python.
The script discovers Visual Studio with `vswhere`, builds, runs the core/UI tests, and deploys Qt beside the EXE.
Python selection is: explicit `SWR_PYTHON`, `.venv`, `venv`, portable `python`, then PATH. CMake 3.31.10 and
Ninja 1.13.2 must already be installed in that interpreter. A missing dependency is reported; the build does not
create a venv or download packages. The launcher uses the same interpreter order and exports its selection as
`SWR_PYTHON` to builds and games. Settings > Toolkit shows the selected absolute Python path.

```powershell
# From the repository root:
& ".\scripts\build-launcher.bat"
# Explicit Qt installation:
& ".\scripts\build-launcher.bat" -QtPrefix "C:\Qt\6.8.3\msvc2022_64"
```

Output: `bin/launcher/SwitchRecompLauncher.exe`; full build/test/deploy output: `logs/launcher_release.log`.

Linux uses Qt 6.8.3 `gcc_64` with Widgets, Test and Image Formats, CMake, Ninja and a native C++ toolchain:

```sh
export SWR_QT_PREFIX=/path/to/Qt/6.8.3/gcc_64
sh scripts/build-launcher.sh
./bin/launcher-linux/SwitchRecompLauncher
```

Both frontend builds need an existing toolkit runtime to compile games. See
[BUILD_ENVIRONMENT_GUIDE.md](BUILD_ENVIRONMENT_GUIDE.md) or use a complete portable release.

## Verification

```powershell
& ".\.venv\Scripts\python.exe" tests/test_python_selection.py -v
& ".\.venv\Scripts\python.exe" tests/test_launcher_workflow.py LauncherWorkflow.test_import_existing_build_without_compiling
# Separate compilation smoke test, when runtime/build work is not in progress:
& ".\.venv\Scripts\python.exe" tests/test_launcher_workflow.py LauncherWorkflow.test_build_saves_ready_library_entry
```

The workflow test uses a synthetic NUP (no copyrighted game data), a disposable library, and paths containing
spaces, brackets, and `&`. It exercises the deployed launcher, actual GCC compilation and linking, shader-tool
staging, and persistence of the successful ready-to-play entry. It does not run the synthetic guest code.

Core Qt tests cover library round trips/corruption protection, argument boundaries, artifact readiness,
per-game build settings, preservation of unrelated game INI settings, combined filters, and theme contrast.
UI tests cover filter clearing and keyboard dismissal. Core tests also check per-card action states,
running-EXE identification, equivalent Windows path spellings, NSP classification, imported NSP builds and key-path
environment propagation. The documentation fixture below adds actual Window/dialog interaction checks.

## Screenshot capture and NSP verification

The ten PNGs in [`docs/images/launcher/`](images/launcher/) were rendered from the production C++ `Window`,
`GameCard`, Properties, menus, build dock and file-dialog code using Qt 6.8.3 on Windows. The separate
[`launcher-capture` fixture](launcher-capture/capture.cpp) links those same sources, uses Qt's `offscreen` platform,
Fusion style, Segoe UI at 10 pt and `QWidget::grab()`. It does not recreate the interface in HTML or draw replacement
UI. File dialogs use Qt's non-native implementation so they can be driven offscreen; their top 50-pixel address row
is cropped to omit the temporary absolute path. Native OS window frames are outside the widget capture.

All names, package headers, native artifacts and generated geometric cover images are demonstration data.
The **Play** card represents artifact detection, not an executed game. The build dock is intentionally captured
before a build starts. The key-installation screenshot uses an explicitly invalid demonstration string, never
console keys. The fixture uses a temporary library and QSettings directory, plus a unique test application
identity for its private key copy, which it removes on successful completion. It does not load the real library
or the real launcher's installed keys.

To reproduce, first build the launcher as above. In an **x64 Native Tools Command Prompt for VS 2022**, from the
repository root (adjust the Qt prefix if needed):

```bat
.venv\Scripts\cmake.exe -S docs/launcher-capture -B build/launcher-docs -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 "-DCMAKE_MAKE_PROGRAM=%CD%/.venv/Scripts/ninja.exe"
.venv\Scripts\cmake.exe --build build/launcher-docs --target launcher-capture --parallel 8
set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%"
set "QT_QPA_PLATFORM=offscreen"
set "QT_SCALE_FACTOR=1"
.venv\Scripts\python.exe -c "import subprocess; subprocess.run(['build/launcher-docs/launcher-capture.exe', '.', 'docs/images/launcher'], check=True, timeout=60)"
```

This builds only the documentation executable and launcher core. It verifies the actual NSP file filter, accepts
a synthetic NSP through **Add game**, checks **DIRECT NSP** in Properties, and verifies the saved library entry.
It drives **Keys > Install keys**, verifies the private fixture copy, checks the missing-key build guard, and
exercises the real `BuildManager`/`QProcess` with a temporary Python probe that checks NSP arguments and
`SWR_PROD_KEYS`, then deliberately exits with a failure code. No compiler or game is launched by the fixture.
This checks launcher dispatch, not real NCA decryption or a complete NSP compilation.

Verification for these docs (November 15, 2026):

| Check | Result |
| --- | --- |
| `scripts\build-launcher.bat` | Launcher-only build/deploy; both CTest suites passed |
| `launcher-capture` as above | Ten PNGs; NSP picker/import, key installation, build guard/dispatch passed |
| `.venv\Scripts\python.exe tests/test_nsp.py -v` | Three synthetic parser tests passed: ExeFS/RomFS, invalid PFS0, BKTR rejection |
| `.venv\Scripts\python.exe tests/test_launcher_workflow.py LauncherWorkflow.test_import_existing_build_without_compiling -v` | Deployed-launcher import/persistence test passed without compilation |

The Python NSP suite uses a synthetic header-inspection helper; it does not prove real-key decryption. No game
compilation, gameplay benchmark or physical controller-output test was performed for this documentation update.

## Release integration

The launcher is a frontend, not a replacement for the toolkit. A Windows release should contain this deployed
Qt folder together with `switchrecomp.py` and its Python modules, `bin/runtime_win64_win32.exe`, shader tools
and their ABI header/licenses, scripts/docs, and LICENSE.txt/NOTICE.txt. Portable Python/GCC may be installed
by `scripts/setup.bat`; .NET 10 is required for the live Vulkan shader translator. Game builds now stage shader
tools without requiring the development-only `build/nvn/libnvn.a` to exist on the player's machine.

Do not package personal `library.json` files, games, saves, extracted data, shader caches, captures, or logs.
The existing SDL launcher is separate; this native Qt launcher is `SwitchRecompLauncher.exe`.
