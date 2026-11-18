# Building From A Combined NSP (Game + Update, Optionally + DLC)

Status 2026-11-18: **built**. Pokémon Legends: Arceus `(1G+1U)` and Pokémon Violet `(1G+1U+1D)` build from their
combined packages and run their openings with the RomFS read in place. Sections 1 to 7 are the plan as it was
written before the code; [section 8](#8-results-2026-11-18) is what was built, what the real titles turned up, and
what is open.

Goal: `switchrecomp.py "Game (1G+1U).nsp"` builds a Switch 1 game straight from the package that holds the base
game and its update (and, in a `1G+1U+1D` package, its DLC), and the built game reads its RomFS from that package
in place. Nothing is extracted except the ExeFS (tens of MB), as with every other in-place build.

## 1. What a combined NSP holds

Measured on three packages (headers decrypted in memory; nothing written to disk):

| | Pokémon Legends: Arceus `(1G+1U)` | Pokémon Violet `(1G+1U+1D)` | Mario Kart 8 Deluxe `(1G+1U+1D)` |
|---|---|---|---|
| file size | 6.55 GB | 11.97 GB | 12.13 GB |
| base Program NCA | 6.0 GB, key generation 11, ticket | 6.3 GB, generation 14, ticket | 6.7 GB, generation 0, ticket |
| update Program NCA | 92 MB, generation 13, ticket | 4.9 GB, generation 17, ticket | 4.5 GB, generation 4, ticket |
| update version (CNMT) / display (NACP) | 262144 / 1.1.1 | 786432 / 4.0.0 | 1507328 / 4.0.1 |
| relocation entries (base ↔ patch) | 486 | about 16,000 | about 3,300 |
| add-on content | none | `01008F6008C5F001` | `0100152000023001` |

Layout, the same in all three:

- A PFS0 (16-byte header, 24-byte entries at 0x10) with, per title, a `.cnmt.nca` (Meta), the content NCAs and,
  for content with a rights id, `<rights id>.tik` + `.cert`. The update's CNMT also lists delta fragments
  (record type 6) that are *not* in the package and are not needed.
- **Tickets are common tickets** (title key type 0 at `+0x281`): the 16-byte title key at `+0x180` is wrapped with
  `titlekek_<generation-1>` from `prod.keys`. A personalized ticket (type 1) cannot be read with `prod.keys` and is
  refused with that reason.
- **Base Program NCA** (content type 0, title id `...000`): section 0 ExeFS (PartitionFS, hierarchical SHA-256,
  AES-CTR), section 1 RomFS (IVFC, AES-CTR), section 2 logo (plaintext).
- **Update Program NCA** (content type 0, the *same* title id in the header; the CNMT title is `...800`):
  section 0 is a **complete** ExeFS (AES-CTR), section 1 is the **patch RomFS**: encryption type 4 (AesCtrEx) with
  two BKTR tables named by the patch info at FS header `+0x100` (indirect) and `+0x120` (AesCtrEx).
- **Control NCAs** (type 2) for base and update have no rights id: their key is in the header's key area, wrapped
  with `key_area_key_application_<generation-1>`. RomFS: `control.nacp` (display version at `+0x3060`) and
  `icon_<Language>.dat`.
- None of the three has sparse or compressed storage in its Program NCAs.

### 1.1 Crypto

| What | How |
|---|---|
| NCA header (0xC00) | AES-128-XTS, key `header_key` (or derived from `master_key_00` and the `header_*`/`aes_*` sources), sector 0x200, tweak = sector number **big-endian** |
| content key, rights id set | `AES-ECB-decrypt(titlekek_<gen>, ticket[+0x180..+0x190])` |
| content key, no rights id | `AES-ECB-decrypt(key_area_key_application_<gen>, header[0x320..0x330])` (key area slot 2) |
| `<gen>` | `max(header[0x206], header[0x220])`, minus 1 when not 0 |
| section data, type 3 | AES-128-CTR, counter = FS header `+0x140..+0x148` reversed (secure value BE, generation BE) ‖ `(offset in the NCA) >> 4` as 64-bit BE |
| section data, type 4 | the same, but the generation (counter bytes 4..7) comes from the AesCtrEx table entry that covers the offset |

hactool 1.4.0 cannot be used for the update: it stops with `Invalid BKTR layout!` when given the header alone (it
wants the tables, which sit at the end of a multi-GB section), and it only reads whole files, never a range of a
package. The build therefore gets its own small reader (`nca.py`).

### 1.2 BKTR

Both tables live inside the patch section and are encrypted with the section's own counter (type 3 rules).

```
table   = header(0x4000) + buckets * 0x4000
header  = u32 pad, u32 bucket count, u64 total size, u64 bucket start offsets[]
bucket  = u32 pad, u32 entry count, u64 end offset, entries[]
indirect entry (0x14) = u64 virtual offset, u64 physical offset, u32 source (0 = base section, 1 = patch section)
AesCtrEx entry (0x10) = u64 offset in the patch section, u32 pad, u32 counter generation
```

The **virtual section** is what the patched RomFS section would look like on the console: IVFC hash levels first,
the RomFS image (IVFC level 6) after them, at the offset the *update's* IVFC header gives. An indirect entry maps
`[virtual, next virtual)` to the base section or the patch section starting at `physical`. Base offsets are
relative to the base's RomFS **section** (not to its level 6). The indirect header's total size is the size of the
virtual section; the AesCtrEx header's is the patch section's data size.

Verified: for all three packages the composed section was checked against its own IVFC level-5 SHA-256 hashes at
300 random 16 KiB blocks plus the two blocks around every relocation boundary (1,099 / 32,829 / 6,780 blocks):
**0 mismatches**. So base and patch offsets, both counters and the generations are right.

## 2. What the build needs

| Need | Source |
|---|---|
| modules (`main`, `sdk`, `subsdk*`, `rtld`) and `main.npdm` | the **update's** ExeFS (it is complete); the base's when the package has no update |
| RomFS tree (the build looks for `.nro` modules in it) | the patched RomFS |
| `control.nacp`, icons | the Control NCA the update's CNMT lists (record type 3); the base's otherwise |
| version | CNMT version of the update; display version from that `control.nacp` |

## 3. What the runtime needs

Today (`runtime/nsp.c`, marker `SWRNSP1`) the runtime reads one plaintext byte range of the package. The game sees
a raw RomFS image; `romfs_read(offset, size)` is the only access. A patched RomFS can be served the same way: no
extraction, the image is composed per read from the relocation table.

New marker `romfs.nsp` version 2 (text, written by the build):

```
SWRNSP2
<package path>
keys <prod.keys path used for the build>
romfs <offset of the RomFS image in the (virtual) section> <size>
base <file offset of the section> <section size> <section offset in its NCA> <crypto 1|3> <counter, 8 bytes hex> <key name>:<wrapped key hex>|-
patch <file offset> <size> <offset in NCA> 4 <counter> <key name>:<wrapped key> <indirect offset> <size> <AesCtrEx offset> <size>
```

- No key material from `prod.keys` enters the build folder. The marker names the key (`titlekek_0c`) and carries
  the content key **still wrapped**, exactly as it is in the package's ticket or NCA header; the runtime unwraps it
  at start with `prod.keys` (path from the marker, `SWR_PROD_KEYS` overrides it). Without the keys file the game
  reports that the keys are missing instead of starting without data.
- `base` alone (no `patch`) is an encrypted single-game package: this also lifts the "encrypted NSPs are not
  supported" limit for Switch 1 packages without an update.
- `patch` without `base` is refused at build time (an update-only package).
- Runtime work (`runtime/nsp.c`, behind the unchanged `nsp_open_marker / nsp_size / nsp_pread`): AES-128 (AES-NI
  when the CPU has it, portable C otherwise), `prod.keys` lookup of one named key, the two BKTR tables loaded and
  flattened at open (Violet: 16k entries, well under 1 MB), binary search per read.
- `nn::oe::GetDisplayVersion` (`svc_core.c`, hardcoded `1.0.0` today) answers from `<data>/control.nacp`.

## 4. DLC inside a 1G+1U+1D package

DLC is not part of the game's RomFS (docs/DLC.md): it is a separate title with its own storage. The build does not
install it. It reports `DLC: this package also holds add-on content <ids>`; the launcher then adds the package to
the game's DLC list after the first successful build from it, and its normal `--sync-dlc` step installs the
add-on titles (the installer already takes only the add-on titles from a combined package). From the command
line: `switchrecomp.py --install-dlc <package> --datadir <data>`.

## 5. Code

Engine:

| File | Change |
|---|---|
| `nca.py` (new) | AES-ECB through the OS (`bcrypt` on Windows, `cryptography` or OpenSSL's libcrypto elsewhere, plain Python as the last resort), XTS header, CTR sections, `Patched` (BKTR), `Package` (members, tickets, CNMT, Program/Control choice, ExeFS, RomFS tree, marker v2) |
| `nsp.py` | `romfs_tree()` shared; `load()` hands packages with encrypted sections, a BKTR section or two Program NCAs to `nca.Package`. The plaintext single-NCA path (Switch 2 packages, marker v1) is unchanged |
| `nxloader.py`, `switchrecomp.py` | the `nca.Package` case in `load_program` / `copy_data`: ExeFS files, marker, `control.nacp` + icon, the DLC hint |
| `runtime/nsp.c`, `runtime/rt.h` | marker v2, AES, BKTR |
| `runtime/svc_core.c` | display version from `control.nacp` |
| `tests/test_nsp_combined.py`, `tests/test_nsp_runtime.c` | synthetic encrypted base + BKTR update package with test keys: Python reader, marker, and the C runtime reading the same bytes |
| docs | this file, `NSP_IN_PLACE.md`, `README.md` |

Launcher: accept the v2 marker when discovering a build, add the package to `Game.dlc` after the first build when
the engine reports add-on content, show the game's version (from `control.nacp`), README formats.

## 6. Test plan

1. Engine tests (`unittest discover -s tests -p "test_n*.py"`), the runtime marker test (`tests/run_nsp.sh`).
2. Reader against the three real packages: IVFC hash check as in 1.2.
3. Real library, real build folders, engine CLI with the launcher's arguments, `--jobs 4`:
   - Pokémon Legends: Arceus `(1G+1U)` and Pokémon Violet `(1G+1U+1D)`: build, launch, title screen or menu,
     screenshots; version reported by the game's log.
   - Mario Kart 8 Deluxe `(1G+1U+1D)`: the same game is already built from an extracted dump, so its build from
     the package is a like-for-like check of the composed RomFS (same menus, a race).
   - Mario Kart World (NUP) rebuilt and run to its menu: plain packages still work.
4. Saves backed up before the runs and restored after, hashes recorded.

## 7. Risks

- **Runtime gaps unrelated to packaging.** Arceus and Violet have never run in this runtime. The package work is
  done when the game starts and reads its data; how far each title gets is reported separately.
- **Sparse or compressed Program NCAs** (some newer dumps): refused with a clear message; none of the three test
  packages uses them.
- **Personalized tickets**: refused with the reason.
- **Read speed**: every read is decrypted. AES-NI does more than 1 GB/s on one core; the portable path is slower
  but correct.
- **Package moved or keys removed after the build**: the game reports which one is missing; rebuilding (or
  restoring the path) fixes it, as with markers today.

## 8. Results (2026-11-18)

### 8.1 What was built

As planned in section 5, with these differences:

- **Marker paths are UTF-8.** `runtime/nsp.c` opens the package and the keys file through the wide API on Windows.
  The first run of Arceus failed here: `Pokémon Legends - Arceus [...] (1G+1U).nsp` was "missing" because the
  runtime opened the marker's UTF-8 path in the ANSI code page. The `SWRNSP1` path had the same fault.
- **Output names are ASCII** (`switchrecomp.py output_name`, the launcher's `outputNameFor`): `Pokémon Legends -
  Arceus (1G+1U)` builds as `Pokemon Legends - Arceus 1G1U`. With `é` in the build folder the Vulkan renderer stayed
  black: the live shader translator wrote its shaders into a second folder named `Pok<U+FFFD>mon ...`, because the
  runtime and the shader tools hand that path to each other as bytes. The display title keeps its spelling.
- **The base's key is proved at build time** too (its ExeFS has to decrypt), not only the update's: with a wrong
  base key the update's own tables still read fine and the fault would only show in the running game.
- The log's first lines say what is being read: `title: version 1.1.1 (control.nacp)` and `RomFS: NSP <package>
  (read in place, 6123.9 MB, the update applied over the game, decrypted with prod.keys)`.

Two runtime gaps that the titles hit, outside the package work, fixed because they stopped the boot:

- **GPU addresses were checked as CPU addresses** (`runtime/gpu.c`, `gmem_read / gmem_write / gmem_ptr`). A
  Switch 1 program has a GPU address space of its own; Arceus maps its command buffers at GPU addresses where the
  CPU has no memory, so every command list was dropped (`dropping unreadable GPFIFO entry`) and the game stood at
  frame 1. A range is now also accepted when the memory mapped behind it is readable or writable.
- **`btm:u` (Bluetooth LE)** handed out no interface: Violet 4.0.0 opens it at start and aborted in
  `nn::diag::detail::Abort` after 4 s. Command 0 now returns `IBtmUserCore`, whose four `Acquire...Event` commands
  return an event that never fires.

### 8.2 The titles

Built with the engine CLI and the launcher's arguments (`--jobs 4`, `-O 2`) into the library's build folders,
`<dump folder>\SwitchRecomp Builds\<name>`. Runs with `tools/plza_probe.py`, renderer `vulkan` as the launcher
sets it; evidence in `build/evidence/combined-nsp/` of the main checkout.

| | Pokémon Legends: Arceus `(1G+1U)` | Pokémon Violet `(1G+1U+1D)` |
|---|---|---|
| build | 561 C units, 33 min; ExeFS 46 MB staged, nothing else copied | 526 C units, 34 min |
| version | `Package: game with its update 1.1.1 (v262144)`, `title: version 1.1.1` | `4.0.0 (v786432)`, `title: version 4.0.0` |
| RomFS | 6,123.9 MB composed from the 6.55 GB package | 9,465.4 MB composed from the 11.97 GB package |
| DLC | none in the package | `01008f6008c5f001` installed by `--sync-dlc` from the same package (0.6 KB image); the game lists it (`ListAddOnContent ... -> 1`) and reads its storage at 3.4 s |
| reached | language selection (`0005`), the opening with Arceus (`0009`), "What is thy name?" and the text box (`0016`), the portrait confirmation (`0026`), loading, and the first scene in the Obsidian Fieldlands with Professor Laventon in full 3D (`0064`). 12,001 frames in 600 s, 30 FPS, no crash | language selection (`0004`), "Choose a look for yourself" (`0017`), the name box, the real-time opening in the academy (`0037`, `0050`, `0056`). 8,300 frames, 30 FPS while it ran |
| stops at | not reached in the 600 s run: free movement (the run ended in the first dialogue) | **a crash at 444 s, at the end of the opening**: null pointer (`access to 8`) in `nn::audio::VoiceInfoManager::AcquireVoiceInfo(int, int)+0x58`, inside the SDK's audio renderer client. An audio runtime gap, not a data one |
| run | `20261118-031020-pla-vulkan2` | `20261118-033257-violet-boot2` |

The earlier runs in that folder are the faults of 8.1 as they were found: `pla-boot` (package "missing"),
`pla-boot2` (one frame, GPFIFO entries dropped), `pla-vulkan` (black, shaders in the mis-named folder),
`violet-boot` (`svcBreak` at 4 s).

### 8.3 Open

- **Violet's audio crash** (8.2) blocks its gameplay. `bt` command 9 (`RegisterBleEvent`) still answers without an
  event; the game went on regardless.
- **Non-ASCII characters in the build folder's own path** (the dump folder, not the name the build gives itself)
  still break the Vulkan shader translator, and the NUP marker (`romfs.nup`) is still opened in the ANSI code page.
- **The game's own package as `.nsz` / `.xci`**, an **update in a separate file** from its base, and **sparse or
  compressed Program NCAs** are not built; the last two are refused with the reason.
- **DLC with an encrypted RomFS** is still unpacked once into `romfs.bin` (docs/DLC.md); the version 2 marker could
  serve it in place as well.
- No save was written in either run, so saving and loading are untested for both titles.
