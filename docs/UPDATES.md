<!-- SwitchRecomp - Copyright 2026 MZ Labs
     Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
     Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. -->
# Game updates in a package of their own

A Switch 1 update is a second title (`...800`) with its own Program NCA: a **complete ExeFS** (the new code) and a
**patch RomFS**, a BKTR section that is only meaningful over the base game's RomFS section.
[COMBINED_NSP.md](COMBINED_NSP.md) builds a game from a package that holds both. This document is the other case:
the game in one package, the update in another, as an eShop dump has them.

```bat
set "SWR_PROD_KEYS=D:\Keys\prod.keys"
python switchrecomp.py "D:\Games\Game [v0].nsp" --update "D:\Games\Game [UPD][v262144].nsp" --outdir D:\Games\build -o Game
python switchrecomp.py "D:\Games\Game (1G+1U).nsp" --no-update --outdir D:\Games\build -o Game
python switchrecomp.py "D:\Games\Game [v0].nsp" --update-info "D:\Games\Game [UPD][v262144].nsp"
```

## 1. What an update changes, and why it means a build

An emulator applies an update when the game starts: it loads the update's ExeFS instead of the game's and reads the
RomFS through the patch (section 5 has how Eglantine does it). Here the code is recompiled ahead of time, so the two
halves of an update land in two places:

| Part of the update | Where it goes | When |
|---|---|---|
| ExeFS: `main`, `sdk`, `subsdk*`, `rtld`, `main.npdm` | recompiled into `<name>_code`, staged in `<name>_data/exefs` | **at build time** |
| patch RomFS (BKTR) | not copied; `<name>_data/romfs.nsp` records where it is and how it is read | at run time, on every read |
| control data: version, icon | `<name>_data/control.nacp`, the icon | at build time |

So applying an update, changing to another one, or taking it away again is **a build of the game**. Nothing in the
packages is written; the RomFS stays served in place from the game's package and the update's package, which both
have to stay where they are.

## 2. Options

- `--update PACKAGE`: the update to apply to the NSP being built. An update inside the game's own package (a
  combined one) is left out in favour of this one.
- `--no-update`: build a combined package's game without the update it holds (the game as it was released). On a
  package without an update it changes nothing.
- `--update-info PACKAGE...`: one line a package and exit, for the launcher:
  `UPDATE<tab>path<tab>title id<tab>version<tab>display version`, or `UPDATE error: <path>: <reason>` (exit code 1 when
  any failed). With the game's package as the input, each update is opened over that game with the keys, which
  proves it belongs to it and can be read.

One update applies at a time. The generated C and its objects go to `<name>_build_v<version>` when a separate
update is applied (`<name>_build_base` for `--no-update` on a combined package, `<name>_build` otherwise), so
changing between versions compiles each one once and only links after that.

## 3. Packages

| Update package | Read as |
|---|---|
| `.nsp` | PFS0; the Program NCA with a BKTR section and the game's title id is the update. A combined package (game + update, also + DLC) serves too: its update is taken, its game is ignored |
| `.xci` | the card image's secure partition, then as above (a dump with the 0x1000 key area in front is recognized) |
| `.nca` | the update's Program NCA on its own; a ticket for it is looked for beside it (`*.tik`). It has no metadata: the version shows as unknown and the control data stays the game's |
| `.nsz`, `.xcz` | **refused**: compressed data cannot be read in place while the game runs. Decompress to NSP / XCI (`nsz -D`) |

The game itself has to be an NSP. A NUP holds its update already, and an extracted folder has no base section for
the patch to refer to; both are refused with that reason.

Refused, with the reason in the message: an update of another game (`this update is for <id>, not for this game`),
a package without an update, a ticket bound to one console or missing, keys without the generation, sparse or
compressed NCA storage, an update whose relocations do not fit the game's RomFS section.

## 4. Reader and runtime

`nca.Package(game, keys, update=None, embedded=True)`:

- `open_ncas(path)` lists a package's NCAs for all three containers. Each NCA remembers its own file, so a section
  read goes to the right package.
- With `update`, the base Program NCA comes from the game's package and the patch Program NCA from the update's;
  `Patched(patch section, base section)` composes them exactly as for a combined package. Version and control data
  come from the update's content metadata (type 0x81) and the Control NCA it names; the add-on content hint still
  comes from the game's package.
- The base's ExeFS is still decrypted once at build time: that proves the base's key before the game needs it.

`romfs.nsp` (version 2) gets one more row. The patch row's offsets are then offsets in that file:

```
SWRNSP2
<the game's package>
keys <prod.keys>
romfs <offset> <size>
base <file offset> <size> <offset in NCA> 3 <counter> titlekek_0a:<wrapped key>
update <the update's package>
patch <file offset> <size> <offset in NCA> 4 <counter> titlekek_0c:<wrapped key> <indirect offset> <size> <AesCtrEx offset> <size>
```

`runtime/nsp.c` opens the second file for the patch layer; everything else (AES, the two BKTR tables, the binary
search per read) is the code of COMBINED_NSP.md. The log says `RomFS: NSP <game> (read in place, ... the update of
its own package applied over the game, decrypted with prod.keys)` and `RomFS: update package <path>`. When the
update's package is gone the game stops with `the update package <path> is missing ... remove the update in the
launcher (Game > Install Updates) and build the game again`, not with wrong data.

When the build changes version, `exefs/` loses the modules only the other version had, and `control.nacp` follows.

## 5. How Eglantine (yuzu) does it, for comparison

- **Which update.** Installed and external updates of a title are listed by version, newest first
  (`ExternalContentProvider::ListUpdateVersions`, `registered_cache.cpp`). The per-game add-ons list shows each as a
  checkbox, at most one ticked (`configure_per_game_addons.cpp`); an unticked one is stored as `Update@<version>` in
  `disabled_addons`. `PatchManager` takes the first that is not disabled, so the newest by default, an older one
  when the newer are unticked, none when all are (`patch_manager.cpp` `PatchExeFS`, `PatchRomFS`).
- **Code.** `PatchExeFS` replaces the game's ExeFS with the update NCA's when the update has one.
- **Data.** `PatchRomFS` opens `NCA(update_raw, base_nca)`. A section with encryption type AesCtrEx marks an update;
  without a base it is `ErrorMissingBKTRBaseRomFS` (`content_archive.cpp`). `NcaFileSystemDriver` stacks the storages:
  AES-CTR-EX (the counter generation of each range from the AesCtrEx bucket tree) under the indirect storage (the
  relocation bucket tree, source 0 = the base's section, 1 = the patch's), then IVFC. The title key comes from the
  ticket and `titlekek`.
- **Control data and version** come from the update's Control NCA.

The same here, moved to where a recompiler has to do it: the choice is made before the build (the launcher's
"Version to build": newest, a particular one, or none), the ExeFS replacement is the recompilation, and the storage
stack is `nca.Patched` at build time and `runtime/nsp.c` at run time.

## 6. Execution hints belong to one version

A running game reports addresses it jumped to that the build had not found (`<name>.hints`); the next build makes
them functions. That file used to be one per game. With an update applied and taken back, the base version was
built with the hints of 1.1.1 (and the other way round): 44 addresses, half of them inside the other version's
functions. Pokémon Legends: Arceus 1.0.0 built that way stopped at frame 7 with no GPU work, every time
(`upd-3-reverted`, `upd-3-reverted-b`). `program_hints()` now stamps the file with the program it belongs to
(`<name>.hints.for`, from the modules' build ids); a build of another program puts it aside as `<name>.hints.<id>`
and brings that program's own file back when there is one.

## 7. Tests

`tests/test_nsp_combined.py`, class `SeparateUpdateTest` (synthetic packages with test keys): the update of its own
package against the same content as the combined package, the marker's `update` row, a combined package without its
update and with another one, XCI and bare NCA, the refusals, `--update-info`, the staged build taking an update
back, the hints staying with their program, and `runtime/nsp.c` reading the patch from the second file
(AES-NI and portable) and naming the missing update package.

## 8. Results (2026-11-18)

No game with its update in a separate file exists among the owner's dumps, so the two packages were derived from
the combined `Pokémon Legends - Arceus (1G+1U).nsp`: its members copied byte for byte into a game package (7
members, 6,161.6 MB) and an update package (7 members, 88.9 MB), the layout an eShop dump has
(`build/evidence/updates/split_combined.py`; the combined package was only read).

Reader, on the real packages: `Package(game, update=...)` gives the same version (1.1.1, v262144), the same ExeFS
and, at 400 random ranges, the same RomFS bytes as the combined package; the game alone is 1.0.0 with a different
`main`. Mario Kart 8 Deluxe's combined package offered as this game's update is refused: `this update is for
0100152000022000, not for this game (01001f5010dfa000)`.

Builds into `packages\SwitchRecomp Builds\Pokemon Legends Arceus base` (a library entry), runs with
`tools/plza_probe.py`, renderer `vulkan`, each run under the game lock with a memory watchdog:

| Step | Build | Run | Result |
|---|---|---|---|
| the game alone | engine CLI, launcher's arguments, 558 C units, 35 min | `upd-1-base-vk` | log `title: version 1.0.0`, RomFS 6,104.2 MB; the opening with Arceus |
| update added in the launcher, **Rebuild** | launcher (`--build`), 561 C units in `_build_v262144`, 35 min | `upd-2-updated` | `title: version 1.1.1`, `RomFS: ... 6123.9 MB, the update of its own package applied over the game`, `RomFS: update package ...`; the opening to "What is thy name?" |
| update removed, **Rebuild** | launcher, 107 s (the base's objects are still there; it links) | `mem-base` | `title: version 1.0.0`, RomFS 6,104.2 MB again; the opening to "Select your appearance", 6,601 frames at 30 FPS |
| update added again, **Rebuild** | launcher, about 2 min | `mem-update`, `upd-returns` | `title: version 1.1.1` again; "Select your appearance", 6,601 frames at 30 FPS |

Memory (private bytes sampled every 2 s, `memory-*.csv`): the game holds **5.15 GB flat** from 60 s to the end of
a 234 s run, the same for 1.0.0 and 1.1.1; it does not grow. A launcher-driven rebuild peaks at **5.6 GB** for the
whole process tree (Python 1.8 GB for the analysis, eight compilers).

Two faults on the way, both visible in the evidence folder:

- The frame-7 stall after the first removal: the hints of the other version (section 6). Fixed.
- A `MemoryError` in the first removal build and two starts that stopped at 0.26 s: the machine was out of memory
  at the time because another game was running at 61 GB. Neither repeated on a quiet machine (the removal build
  and three runs since).

Open: NSZ / XCZ updates (refused), an update for a game that is itself an XCI or NSZ, and a real update dump that
was never part of a combined package (the derived one is byte-identical in its members, but it is the only one
tried).
