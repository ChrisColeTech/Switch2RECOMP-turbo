# SwitchRecomp Qt launcher

The launcher is a desktop game library and build utility for **SwitchRecomp by MZ Labs**. Add your own
game dumps, build them, then launch the finished games from the same library. No per-game desktop shortcuts
are needed. The library is saved automatically and is restored the next time you open the launcher.

## Start it

For all-in-one releases, run `Switch2RECOMP-turbo.cmd` (Windows) or
`./Switch2RECOMP-turbo.sh` (Linux). See [RELEASES.md](RELEASES.md) for platform requirements.
The following path is for a Windows developer checkout:

```powershell
& "D:\Projects\Switch2RECOMP-main\bin\launcher\SwitchRecompLauncher.exe"
```

The entire `bin/launcher/` directory contains the Qt DLLs and plugins needed by the frontend. Do not move
only the EXE. The toolkit is detected by looking above the launcher's directory for `switchrecomp.py`.
If you relocate the frontend separately, choose the toolkit under **Tools → Settings → Toolkit**.

## Add, build, play

1. **Add game:** select a `.nup` or `.nro`, or drop it into the library. The dropdown also accepts extracted
   game folders and existing native builds.
2. Choose the display name and build location in Properties. Per-game compiler settings are under **Build**.
3. Click **Build** on the game's card. Live output appears in a dock that can be resized, hidden, or undocked. Progress stays
   in the status bar even when the dock is hidden.
4. After a successful build, the card's button changes to **Play**.
5. Click **Play**, double-click the cover/list row, or press Enter on the selection. Subsequent sessions use
   the existing build; launching does not recompile it.

To add a game you already compiled, choose **File → Import build…** and select its EXE. The corresponding
`_code.dll` and `_data` folder must be next to it. On Linux select the extensionless executable with its
matching `_code.so`. NUP sources are recovered from `_data/romfs.nup` when present.

The original NUP must remain accessible while playing. Changing the source in Properties requires a rebuild.
Removing a library entry does not delete its game, source, saves, or caches. Cancelled/failed builds are kept
marked as needing a build rather than treating partially overwritten artifacts as ready.

## Desktop layout

- Native Windows title bar and window controls.
- File, Game, Build, View, Tools, and Help menus.
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

Controller, audio, graphics, and vibration support belong to the underlying runtime. This GUI does not add
missing runtime emulation; native controller input exists, but vibration and some Switch 2 audio/video paths
remain incomplete.

## Library storage and keyboard shortcuts

The default library is `library.json` under Qt's local application-data directory for
`MZLabs/SwitchRecompLauncher` (normally `%LOCALAPPDATA%\MZLabs\SwitchRecompLauncher`). It stores paths,
display metadata, per-game build options, view preferences, and window/dock layout. Writes are atomic.
An unreadable/corrupt library is not silently overwritten.

| Shortcut | Action |
|---|---|
| Ctrl+O | Add NUP/NRO |
| Ctrl+B | Build selected game |
| Ctrl+F | Open Search & Filter and focus title search |
| Esc | Close the search/filter popout |
| Ctrl+L | Show build output |
| Alt+Enter | Game Properties |
| Enter / double-click | Play ready game; open build options if unbuilt |
| F5 | Refresh library and tools |

## Build the launcher

Requirements: Visual Studio 2022 C++ x64 tools, Qt 6 MSVC x64 (Core, Gui, Widgets, Test, and the Qt Image Formats
module), and Python.
The script discovers Visual Studio with `vswhere`, installs CMake/Ninja in a project-local `.venv`, builds,
runs the core tests, and deploys Qt beside the EXE.

```powershell
Set-Location -LiteralPath "D:\Projects\Switch2RECOMP-main"
& ".\scripts\build-launcher.bat"
# Explicit Qt installation:
& ".\scripts\build-launcher.bat" -QtPrefix "C:\Qt\6.8.3\msvc2022_64"
```

Output: `bin/launcher/SwitchRecompLauncher.exe`; full build/test/deploy output: `logs/launcher_release.log`.

## Verification

```powershell
& ".\python\python.exe" tests/test_launcher_workflow.py
```

The workflow test uses a synthetic NUP (no copyrighted game data), a disposable library, and paths containing
spaces, brackets, and `&`. It exercises the deployed launcher, actual GCC compilation and linking, shader-tool
staging, and persistence of the successful ready-to-play entry. It does not run the synthetic guest code.

Core Qt tests cover library round trips/corruption protection, argument boundaries, artifact readiness,
per-game build settings, preservation of unrelated game INI settings, combined filters, and theme contrast.
UI tests cover filter clearing, keyboard dismissal, WebP decoding, refreshing an image at the same path, background
updates, and per-card action routing. Core tests also check running-EXE identification and equivalent Windows path
spellings. Dark/light gallery, edge-to-edge list, Properties, Appearance, and popout views were inspected.

## Release integration

The launcher is a frontend, not a replacement for the toolkit. A Windows release should contain this deployed
Qt folder together with `switchrecomp.py` and its Python modules, `bin/runtime_win64_win32.exe`, shader tools
and their ABI header/licenses, scripts/docs, and LICENSE.txt/NOTICE.txt. Portable Python/GCC may be installed
by `scripts/setup.bat`; .NET 10 is required for the live Vulkan shader translator. Game builds now stage shader
tools without requiring the development-only `build/nvn/libnvn.a` to exist on the player's machine.

Do not package personal `library.json` files, games, saves, extracted data, shader caches, captures, or logs.
The existing SDL launcher is separate; this native Qt launcher is `SwitchRecompLauncher.exe`.
