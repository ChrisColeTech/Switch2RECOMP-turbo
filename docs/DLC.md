<!-- Copyright 2026 ChrisColeTech
     SPDX-License-Identifier: MIT -->
# Add-on content (DLC)

A built game gets its DLC installed into its data folder. The game then finds it the way it does on a console:
`aoc:u` lists the add-on titles, and each title's own RomFS is mounted through fsp-srv `OpenDataStorageByDataId`.
DLC is never layered over the game's RomFS. Verified with Mario Kart 8 Deluxe's Booster Course Pass (section 5).

The design, the investigation behind it and the launcher side are in RecompLauncher's `docs/DLC_IN_PLACE.md`.

## 1. Installing

```bat
set "SWR_PROD_KEYS=D:\Keys\prod.keys"
python switchrecomp.py --install-dlc "D:\Games\Game DLC.nsp" --datadir "D:\Games\Game-build\Game_data"
python switchrecomp.py --list-dlc --datadir "D:\Games\Game-build\Game_data"
python switchrecomp.py --remove-dlc 0100152000023001 --datadir "D:\Games\Game-build\Game_data"
python switchrecomp.py --sync-dlc "D:\Games\A.nsp" "D:\Games\B.nsz" --datadir "D:\Games\Game-build\Game_data"
```

- `--datadir` is the built game's `<name>_data` folder (`--outdir DIR -o NAME` names it too). The game has to be
  built first: its title id is read from `exefs/main.npdm`, and a package for another game is refused.
- `--install-dlc` adds packages, `--remove-dlc` takes a title id or the package path, `--sync-dlc` makes the
  installed set exactly the given packages (none given: everything is removed). The launcher uses `--sync-dlc`
  after a build and before Play. A package that is already installed (same file, size and time) is not unpacked
  again.
- The exit code is 1 when any package was refused; each refusal is one `DLC error: <package>: <reason>` line.
- The package is only read. Keys come from `SWR_PROD_KEYS` like an NSP build; no key or decrypted key is written
  anywhere.

## 2. Packages

The same list as Eglantine's "Install Files to NAND" (`*.nca *.nsp *.nsz *.xci *.xcz`):

| Package | Read as |
|---|---|
| `.nsp` | PFS0. Every `*.cnmt.nca` is opened; the ones of type AddOnContent (0x82) name their Data NCA. A package that also holds the game and its update (1G+1U+1D) works: only its add-on titles are installed. |
| `.nsz` | the same, with `.ncz` members. The data of an NCZ is stored decrypted behind zstd; both the block (`NCZBLOCK`) and the solid form are read. Needs Python 3.14, `backports.zstd` or `zstandard`. |
| `.xci` | the card image's `secure` HFS0 partition, then as above. A dump with the 0x1000-byte key area in front is recognized. |
| `.xcz` | the same with `.ncz` members. |
| `.nca` | one Data NCA on its own. Its title id comes from the NCA header and its version is unknown (0). With a rights id it needs `<rights id>.tik` beside it. |

What happens to the title's RomFS depends on how the NCA stores it:

| RomFS section | Installed as |
|---|---|
| plaintext, in an uncompressed package | `dlc/<title id>/romfs.nsp`: a marker (the `SWRNSP1` format of a game's own `romfs.nsp`) with the package path and the byte range. Read in place, nothing is copied. |
| AES-CTR with the NCA's key area | `dlc/<title id>/romfs.bin`, decrypted once by hactool |
| AES-CTR with a title key (a rights id) | the same; the title key block comes from the package's ticket (`--titlekey`), which hactool unwraps with `titlekek_<generation>` |
| inside an `.ncz` | `dlc/<title id>/romfs.bin`, cut out of the decompressed stream |

Refused, with the reason in the message:

- a ticket bound to one console (a personalized ticket: its title key is RSA-wrapped for that console),
- data with a rights id and no ticket in the package,
- keys that don't open the content metadata, or that lack the content's key generation (the data then does not
  decrypt to a RomFS; the message names `titlekek_XX` / `key_area_key_application_XX`),
- a package without add-on content (the game, an update) or with add-on content of another game,
- a title that is already installed from another package in the same or a newer version (a standalone DLC file and
  a combined package that holds the same title): the installed one stays, a newer version replaces it,
- BKTR and AES-XTS data sections, a broken NCZ, a file that is not one of the five types.

Nothing is installed for a refused package: no manifest line, no partial files. `--dlc-stub` (diagnostics) installs
the title without data instead; section 5 shows why that is not offered as a fallback.

hactool reads files, not ranges, so for the time of a decryption the NCA is copied next to its output
(`dlc/<title id>/content.nca.tmp`) and deleted afterwards. hactool is called there with relative names, so the
game folder's path never has to pass through its command line.

## 3. The data folder

```
Game_data/dlc.manifest
Game_data/dlc/0100152000023001/romfs.bin      or romfs.nsp, or a romfs/ folder
Game_data/dlc/0100152000023001/install.json   the package, its size and time, what was written
```

`dlc.manifest` is text, one title a line, sorted by title id:

```
# SwitchRecomp add-on content: <title id> <version> <data|stub> <package>
0100152000023001 65536 data P:\Games\Mario Kart 8 Deluxe\Booster Course [0100152000023001][v65536].nsp
```

The third word is optional when the file is written by hand (`<title id> <version> <package>` is read too).
`dlc/<title id>/romfs/` can be filled by hand instead of installing: a folder of files is served as a RomFS, like
the game's own `romfs/` folder.

## 4. Runtime (`runtime/dlc.c`, `svc_core.c`, `svc_fs.c`)

- The manifest is read once, at the first `aoc:u` or data storage call. Without a manifest the runtime behaves as
  before: no DLC.
- A title belongs to the game when its id is in the game's add-on range (`0100152000022000` owns
  `0100152000023000`..`37ff`).
- `aoc:u` 2 `CountAddOnContent` and 3 `ListAddOnContent` (and their `ByApplicationId` forms 0 and 1) answer from the
  manifest. List writes the add-on indexes (title id & 0x7ff) in title id order and honors offset and count; an
  offset past the list is an error. 5 `GetAddOnContentBaseId` already answered the game's base + 0x1000; 7
  `PrepareAddOnContent` succeeds.
- A title in the manifest whose data is not there when the game starts (the package a marker points to was moved
  or its drive is offline, the image was deleted) is not offered to the game, with a warning in the log. The game
  then runs without that DLC instead of hanging on DLC it cannot read (section 5).
- fsp-srv 202 `OpenDataStorageByDataId` for an offered title opens, in this order, `romfs.nsp`, `romfs.bin`,
  `romfs/`. A title marked `stub` gets an empty RomFS. Other ids go on to the system archives as before.
- Log lines: `dlc: 1 add-on content title in ...dlc.manifest`, `aoc: ListAddOnContent offset 0 count 16 -> 1`,
  `aoc: PrepareAddOnContent 1`, `fs: add-on content 0100152000023001 (image, 4.6 KB)`, then the first 24 reads of
  each storage (`dlc: 0100152000023001 read 1200 +5`).
- No ABI change: a runtime rebuild is enough, games need no recompilation.

## 5. Mario Kart 8 Deluxe: the Booster Course Pass

The 48 courses are in the game's update, so they are already in the build's RomFS. The DLC title
(`0100152000023001`, v65536) is a 112 KiB package whose RomFS holds two files: `version.txt` (5 bytes, `10002`) and
`contents.dat` (4096 bytes). Its ticket is a common one (title key type 0), so the title key decrypts with
`titlekek_0c` and the RomFS installs as a 4.6 KB `romfs.bin`.

What the game does at 3.1 s, before the title screen:

```
aoc: ListAddOnContent offset 0 count 16 -> 1
aoc: PrepareAddOnContent 1
fs: add-on content 0100152000023001 (image, 4.6 KB)       (twice)
dlc: 0100152000023001 read 0 +50          RomFS header
dlc: 0100152000023001 read 50 +c ... 80 +58   directory and file tables
dlc: 0100152000023001 read 1200 +5        version.txt
dlc: 0100152000023001 read 200 +1000      contents.dat
```

| Data folder | Result | Run |
|---|---|---|
| no manifest | as before (`ListAddOnContent ... -> 0`). The game lists the 12 Booster Course Pass cups on the second cup page for everyone, but choosing one shows "You can play the new cups by purchasing the Booster Course Pass..." and returns to cup select. The main menu's shop icon has no check mark. | `20261118-010020-mk8d-no-dlc` (`0004`, `0012`, `0013`) |
| title listed, empty RomFS (`stub`) | the title screen comes up, then L+R leads to the loading screen and stays there (a minute, until the run was stopped). The game reads the tables, finds neither file, and never finishes entering the menu. A license without its data is worse than no DLC. | `20261118-002506-mk8d-dlc-stub` |
| title listed, real RomFS | the main menu's shop icon carries a check mark, Golden Dash Cup asks "Start Grand Prix?" and starts Paris Promenade (`Course/Cnsw_11`), which loads, renders and drives (56 FPS over the run, 3rd place under the Eiffel Tower) | `20261118-010258-mk8d-dlc-bcp` (`0002`, `0010`, `0011`, `0014`, `0015`) |

The runs are harness runs (`tools/plza_probe.py`) on the real build folder, rebuilt through the CLI first; the last
one has the DLC installed by the launcher from the standalone package. The first probe with real data
(`20261118-002837-mk8d-dlc-folder`) used a hand-filled `romfs/` folder. Evidence: `build/evidence/dlc/`.

## 6. Tests

`python tests/test_nsp_dlc.py` (also found by `unittest discover -s tests -p "test_n*.py"`), synthetic data only:

- the installer on made-up packages with a stand-in for hactool: title key, key area and plaintext NCAs, a combined
  game + update + DLC package, several titles, XCI with and without the key area, a bare NCA, NSZ solid and block,
  XCZ, every refusal above, sync and remove, the manifest's forms, the command line;
- the PFS0 header: entries start at 0x10 (a 24-byte header read turns sizes into offsets);
- `runtime/dlc.c` compiled with `tests/test_dlc_runtime.c` (GCC or Clang; with the address and undefined behaviour
  sanitizers on Linux): the manifest, the aoc list with offset and count, and the folder, image, marker and empty
  storages.
