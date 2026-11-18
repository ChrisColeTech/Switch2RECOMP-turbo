# Building And Running An NSP In Place

Switch2RECOMP builds a game straight from its application NSP. ExeFS (the code, tens of MB) is copied into the
native build; the RomFS stays in the original NSP and is read from there while the game runs.

Three kinds of package work:

| Package | Example | How its RomFS is read |
|---|---|---|
| plaintext Program NCA (Switch 2 dumps) | Mario Kart World 1.8.0 | one byte range of the package |
| AES-CTR encrypted Program NCA (Switch 1 dumps) | a base game on its own | decrypted per read with your `prod.keys` |
| **combined**: the game and its update in one file, optionally with DLC (`1G+1U`, `1G+1U+1D`) | Pokémon Legends: Arceus, Pokémon Violet, Mario Kart 8 Deluxe | the update's BKTR patch composed over the base per read, both decrypted per read |

For a combined package the code, `main.npdm`, the title, the icon and the version are the **update's**; the RomFS
is the base with the update applied. Formats, the reader and the verification: [COMBINED_NSP.md](COMBINED_NSP.md).

## Keys

Use only keys dumped from your own console. In the Qt launcher choose **Keys > Install keys** and select `prod.keys`.
The launcher stores a private copy in its application-data folder and passes its path to the build. It does not put
keys or decrypted content keys in the toolkit, game build, logs, or releases.

Command-line builds can set `SWR_PROD_KEYS`:

```bat
set "SWR_PROD_KEYS=D:\Keys\prod.keys"
scripts\build-from-nsp.bat "D:\Games\Game.nsp" --outdir "D:\Games\Game-build" -o Game
```

An encrypted package needs the keys **when the game runs** too, because its data is decrypted while it is read: the
runtime takes the keys file the build used (its path is in the marker) or `SWR_PROD_KEYS`. If the file is gone the
game says which key it needs and does not start without its data.

## Runtime layout

The build writes ordinary ExeFS files and a small `Game_data/romfs.nsp` text marker, and for a package read with
the keys also `control.nacp` and the icon (the game's `GetDisplayVersion` answers from that `control.nacp`).

- `SWRNSP1`: the package path and the plaintext RomFS byte range.
- `SWRNSP2`: the package path, the keys file path, and where the RomFS section of the base and of the update sit in
  the package, with each section's counter and its content key **named and still wrapped** as it is in the package
  (`titlekek_0c:<32 hex digits>`). The line format is in [COMBINED_NSP.md](COMBINED_NSP.md#3-what-the-runtime-needs).

Moving or disconnecting the NSP requires rebuilding the marker or restoring the same path.

No whole NCA or RomFS is extracted. The original NSP is read-only and is never modified.

## Supported packages

- top-level PFS0 NSP;
- one Program NCA3 for the game, and at most one more for its update, in the same file;
- ExeFS and RomFS sections that are plaintext or AES-CTR; the update's RomFS as a BKTR patch (AesCtrEx);
- content keys from a **common** ticket in the package (title key) or from the NCA's key area.

Refused, each with its reason, before code analysis:

- an **update-only** NSP (no base in the same file): `this is an update without its game; use a package that holds
  both`;
- a ticket **personalized** for one console (its title key cannot be read with `prod.keys`);
- a Program NCA without its ticket, or keys that lack the content's generation (the message names the missing
  `titlekek_XX` / `key_area_key_application_XX`);
- **sparse** or **compressed** Program NCA storage, and `.nsz` / `.xci` as the game's own package (they work for
  DLC: [DLC.md](DLC.md));
- several games, or several updates, in one file.

The plaintext path inspects the NCA header with checksum-pinned hactool 1.4.0 using the installed keys. Encrypted
and combined packages are read by `nca.py` with the same keys and do not need hactool (it cannot open an update on
its own: `Invalid BKTR layout!`).

A PFS0 header is 16 bytes (magic, entry count, string table size, reserved) and the 24-byte entries follow it at
0x10. Reading the header as 24 bytes shifts the table by 8, so each entry's size lands in its offset field and a
valid package looks corrupt: that happened in two throwaway parsers while Mario Kart 8 Deluxe's packages were
examined. Both of those packages are valid end to end (every entry starts where the previous one ends and the last
one ends at the end of the file). `nsp.py` and `dlc.py` read the table at 0x10; `tests/test_nsp_dlc.py` holds the
regression case.

## Add-on content

DLC packages (NSP, NSZ, XCI, XCZ, a bare NCA, and an NSP that holds the game, its update and its DLC together) are
installed into a built game with `switchrecomp.py --install-dlc`: see [DLC.md](DLC.md). A build from a `1G+1U+1D`
package does not install the DLC by itself; it says `DLC: this package also holds add-on content <ids>`, and the
launcher then puts the package on the game's DLC list and installs it.

## Verification

`python tests/test_nsp.py` covers bounded PFS0/NCA metadata, ExeFS, RomFS traversal, and BKTR rejection by the
plaintext reader using synthetic data. `python tests/test_nsp_combined.py` builds encrypted and combined packages
with test keys and checks the reader, the marker, the staged files and the runtime (compiled there) byte for byte.
`sh tests/run_nsp.sh` checks the native runtime marker/range reader under sanitizers.

The Mario Kart World 1.8.0 validation build analyzed 244,016 functions and 13,924,869 instructions, compiled 460 C
units, staged seven ExeFS files, and read a 22.4 GB RomFS directly from the original NSP. Native Vulkan ran past 2,100
presented frames and reached about 60 FPS after disabling an obsolete system validation layer. Set
`NXRT_VK_VALIDATION=1` only for diagnostics with a current Vulkan SDK.
