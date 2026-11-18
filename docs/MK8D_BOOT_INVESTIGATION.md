<!-- SwitchRecomp - Copyright 2026 MZ Labs
     Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
     Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. -->
# Mario Kart 8 Deluxe: from a crash at 1.4 s to the title screen

Status 2026-11-17: Mario Kart 8 Deluxe (a Switch 1 program, NintendoSDK 4.4.0, built from the owner's dump
`P:/Games/Nintendo/DUMPS/Mario Kart 8 Deluxe/dump` into the launcher entry's buildDir
`.../SwitchRecomp Builds/Mario Kart 8 Deluxe 1G1U1D`) boots. Graphics, audio and the DualSense come up, it plays
its intro logo and shows the title screen key art at about 5 s. It then crashes at about 6.8 s, when the title
menu builds its Mii renderer: the MiiModel system archive it reads is still served empty. The archive's format
is fully mapped (section 5); writing the synthesizer is open: see [Open items](#open-items).

Before: `Mario Kart 8 Deluxe 1G1U1D.log` ended at 1.45 s with a crash in `nnosInitialize`, before any window
content.

Five defects were found in sequence. Each fix moved the crash later.

| # | Crash (time, place) | Cause | Fix |
|---|---|---|---|
| 1 | 1.45 s, `nn::os::detail::InternalCriticalSectionImplByHorizon::Enter`, access to `ffffffffffffffff` | code from the new engine run by a runtime built before it (ABI not bumped) | e440705: code ABI 2, the build rebuilds an older prebuilt runtime |
| 2 | 1.90 s, main thread right after `DeviceInitialize`, access to `0x20` | NVN API 55.15 reported to an SDK 4 game, which requires 53.x | 2f8f44e: `dm_sdk_major()` reads the game's SDK |
| 3 | 6.6 s, worker thread, access to `0x48b4` | the Mii database answered no Miis, so the game validated an all-zero `CharInfo` | 2a42369, 5f80160: six default Miis; lookups answer NotFound |
| 4 | 6.6 s, `nvnMemoryPoolMap(NULL)` in the runtime | MiiModel (0802) served empty: the game read part sizes from heap garbage | 9d0acd6: MiiModel synthesized with zeroed part tables |
| 5 | 6.8 s, `vkt_reg_tex` (runtime), access to `0` | the Mii renderer, with no parts, gives the game a NULL face texture | open |

## 1. The build and runtime pairing (root cause of the reported crash)

### Evidence

Build log (`... 1G1U1D.build.log`, 2026-11-17 13:07): translation, 161 C files, compile and link all finished.
`Build complete.` So it was not a build problem.

Runtime log, first line and crash:

```
[   0.00] SwitchRecomp - Mario Kart 8 Deluxe 1G1U1D (runtime 0.4.1, built Nov 15 2026)
*** Crash (signal -1073741819) at host pc=00007ff9de6839f0, access to ffffffffffffffff ***
  inside the recompiled code of game function 9aebee8 (+c0 on the host)
  pc <_ZN2nn2os6detail36InternalCriticalSectionImplByHorizon5EnterEv+0>  lr <_ZN2nn2os6detail13ThreadManagerC2Ev+78>
   #0 9ae6ffc <_ZN2nn2os6detail13ThreadManagerC2Ev+78>
   #1 9aebd64 <_ZN2nn2os6detail17OsResourceManagerC2Ev+9c>
   #2 9aebb10 <nnosInitialize+38>
```

The code at `9aebee8` is `... d53bd068 f940fd08 b941b914 aa0003f3 885ffe68`: `mrs x8, tpidrro_el0`, two loads,
then `885ffe68` = `ldaxr w8, [x19]`. The first exclusive load of the program faults.

`Enter`'s generated C (`_build/code_129.c`) shows the load as `c->x[8]=(u64)(ldx(c,M,c->x[19],2));`. `ldx` is
inlined from `runtime/cpu.h`, so its behaviour is fixed when the game's DLL is compiled.

### What happened

- The launcher's library (`%LOCALAPPDATA%\RecompLauncher\RecompLauncher\library.json`) uses
  `toolRoot = D:/Projects/RecompLauncher/third_party/Switch2RECOMP`, the submodule. It was at b89e9e5
  ("Exclusive monitor on for Switch 1 programs only").
- `BuildManager::arguments` always passes `--runtime <toolRoot>/bin/runtime_win64_win32.exe`, and
  `switchrecomp.py` just copied it. `bin/` is in `.gitignore`. The file was dated 2026-11-15 23:55, built from
  the checkout before the submodule moved to b89e9e5. The runtime log's `built Nov 15` is that file.
- `git diff 3eac6c2 b89e9e5 -- runtime/cpu.h` shows the ABI change:
  - `RtAPI` gained a field at the end, `u32* excl_mon` ("appended (old games never read it)"),
  - `CPU.pad_` became `u32 excl_seq`,
  - `ldx` now starts with `swr_xm_mark(c,a)`, which reads `c->rt->excl_mon`.
- New code reads one pointer past the end of the old runtime's `RtAPI` (`g_rtapi`). It takes whatever follows as
  the monitor table, hashes the address into it, and faults.
- `main.c` checks `if(g_mod->abi!=1) fatal(...)`, but the engine's `ABI` was still 1. So the old runtime
  accepted the new code. The comment on `excl_mon` ("an old runtime without the table leaves the value-only
  behavior") assumed the reverse pairing, an old game with a new runtime, which is safe.

The same hazard hit the submodule's `build/nvn/libnvn.a` (also Nov 15 23:55). It predated b3bae4d's `nvn_tex.c`
change (the Switch 1 GOB texture order), so it was rebuilt by hand with `tools/build_nvn.py --jobs 4`.

### Fix (e440705)

- `switchrecomp.py`: `ABI = 2`.
  - `place_runtime()` replaces the copy. A runtime is accepted only if its bytes contain
    `SwitchRecomp runtime ABI 2\0`.
  - If `bin/`'s prebuilt runtime lacks the marker, it is rebuilt in place ("The prebuilt runtime is older than
    this engine (code ABI 2), rebuilding it..."). Any other `--runtime` file without it is refused with a message.
  - If there is no prebuilt runtime at all, the runtime is still built straight into the game folder, as before.
- `runtime/main.c`: `const char g_runtime_abi[]="SwitchRecomp runtime ABI 2"`.
  - It accepts code ABI 1 and 2 (ABI 1 code never touches the appended fields).
  - The fatal message names both ABIs. An old runtime with new code now stops with "The recompiled code is from
    another SwitchRecomp version" instead of crashing.
- `.github/workflows/engine-ci.yml`: the built runtimes must contain the marker (Windows and Linux).

Verified on the real build: the CLI rebuild printed the rebuild line, and the game's exe hash then equals
`bin/runtime_win64_win32.exe`. The log shows `built Nov 17`, `cpu: exclusive monitor on`, and the game got past
`nnosInitialize`. A runtime that's older but has no ABI change (e.g. a stale `libnvn.a`) is still not detected.
See open items.

## 2. NVN API version for Switch 1 games

### Evidence

```
[   1.90] [nvn] DeviceInitialize 0000000113410020 flags 0 at 1.725 s (graphics backend vk)
[   1.90] [nvn] device API 55.15 (SDK 19)
*** Crash ... access to 0000000000000020 (game address 20) ***
  the address is a null pointer plus an offset: the game probably got NULL from its allocator (out of memory?)
  thread 80 (main)   pc=8629364 lr=87e95d8
```

The runtime's OOM hint was wrong. Memory was normal: heap 3100 MB, `GetInfo 6/0 -> cd500000` = 3285 MB total.

- `F_87E95B4` loads `x0 = **(GOT 0x930AFF0)` and calls `F_861F768`, which reads `[x0+0x20]`.
- Resolving the GOT slot through the module's relocations: `R_AARCH64_RELATIVE`, addend `main+0x1353408`. So the
  global is `0x9358408`, a main `.bss` pointer, still NULL (the dump shows `mem x21 9358408: 0 0 0 0`).
- The global's only writer, found by scanning every generated function that loads that GOT slot, is in
  `F_8A88D24` (`WR64((c->x[27]),c->x[23])` after `L_8A88FC0`). It is reached only if two device queries pass:

```
 c->x[8]=(u64)(RD32((c->x[31]+32)));  subs32(..., 53)   if(!c->z) goto L_8A89638;          // major == 53
 c->x[8]=(u64)(RD32((c->x[31]+24)));  subs32(..., 105)  if(!(c->n==c->vf)) goto L_8A89638;  // minor >= 105
```

These are `nvnDeviceGetInteger(API_MAJOR / API_MINOR)`. MK8D refuses anything but NVN API 53.x.

`runtime/nvn/hle_nvn.c` `nvn_api()` already had the right rule: `DM_SDK_OLD(m) ? 53 : g_devinfo[0]`, i.e. 53.313
for SDK 10 and older. But `runtime/nvn/host/nvn_host.c` had `int dm_sdk_major(void) { return 19; }`, a stub
since 0.4.0. So every Switch 1 game was told 55.15. MK8D's sdk module carries `NintendoSdk_nnSdk-4_4_0-Release`.

### Fix (2f8f44e)

- `dm_sdk_major()` scans the sdk module (`g_sdk_base/size`, the same range `dm_sdk_ounce` scans) for
  `NintendoSdk_nnSdk-<major>_` and caches the result. It stays 19 when the string is absent.
- Its only user is `nvn_api`, so Switch 2 titles (Ounce, API 231) and SDK 11+ games see no change.
- Log: `sdk: NintendoSDK 4`, `[nvn] device API 53.313 (SDK 4)`. The game then creates its graphics system.

## 3. The Mii database

### Evidence

After fix 2 the game ran: 180 frames, the shader cache filling, audio voices, the DualSense attached. It loaded
`/UI/cmn/menu_title/mnt_L_MenuTitle_00.szs`, then crashed on worker thread 101 at access `0x48b4`.

- `F_88E9750` gets `x0 = 0` (`x19=0`) and reads `this+0x489c`. It is reached through the thin wrapper `F_88E624C`
  (`x0 = *(x0)`), called from `F_881A724` on `obj+200`. So the impl pointer at `obj+200` was never set.
- The impl is created in `F_88E609C`. Resolving the PLT stubs it calls (`R_AARCH64_JUMP_SLOT` names from main's
  dynamic symbols):
  - `8B59A10` = `nn::mii::ParamCharInfoAccessor::ParamCharInfoAccessor(CharInfo const&)`
  - `8B59A20` = `nn::mii::ParamCharInfoAccessor::IsValid()`. If false, it skips creating the impl.
  - `8B59A30..60` = `nn::mii::detail::BufferAllocator` (ctor, `AlignUp`, `Advance`, dtor)
- The validators are exported by the sdk module: `IsValid` → `CharInfoRaw::IsValid` → `CharInfoRaw::Verify() == 0`
  (`9C0B8C8`, a chain of range checks with one return code each).
- `SWR_TRACE_FN=9C0B134,9C0B15C,9C0B8C8,88E609C` (see `kernel.c` `hle_install`) showed `Verify` returning `32`
  for every CharInfo the game checks, including at 2.42 s right after `IDatabaseService cmd 3`. The trace prints
  hex, so that is 0x32 = 50, the first check: invalid `CreateId`.
- A temporary dump of `x0` in the trace hook (not committed) showed all 0x58 bytes zero.

The runtime's `mii_db` answered `GetCount` / `Get` / `Get1` with 0 for every source flag. On a console, source
flag Default (bit 1) always yields the six built-in Miis (yuzu `mii_manager.cpp` `BuildDefault`, `DefaultMiiCount`
= 6). The log only shows the first call of each command (`cmd 3 (source flag 1)`). The game asks again later
with the Default flag and uses element 0, which here was never written.

Once the game had a save, which an earlier run created, it also called `cmd 5` (UpdateLatest, passing back its
stored default Mii; the log shows its create id bytes `5b3e119a` as the "flag") and `cmd 21` (GetIndex). Both
answered success with no data, i.e. a zero CharInfo again, and the crash at `0x48b4` came back.

### Fix (2a42369, 5f80160)

- `mii_charinfo(ci, index)`: the runtime's existing default CharInfo ("no name", values inside every `Verify`
  range; `CreateId::IsValid` needs non-zero and `(byte 8 & 0xC0) == 0x80`). Each default Mii gets its own create
  id (byte 15 ^ index), three of each gender.
- `GetCount` (2) answers 6 with the Default flag. `Get` (3, 0x5C-byte `CharInfoElement` with source = Default) and
  `Get1` (4, 0x58-byte `CharInfo`) fill the out buffer.
- `UpdateLatest` (5) and `GetIndex` (21) return `RC(126,4)`, Mii NotFound, because the database is empty. The
  game then keeps its own copy.
- A/B on the real build: runtime without the change → `0x48b4` twice. With it → the renderer initializes and the
  crash moves (3 of 3 runs).

## 4. MiiModel (system data 0100000000000802)

### Evidence

Next crash: `in the runtime itself (exe offset +e1cd5)`, access `0x4`. `x86_64-w64-mingw32-nm -n` on the runtime
gives `adapter_nvnMemoryPoolMap`, called with a NULL pool from `F_88EF594` (the dump shows the `nn::gfx`
MemoryPool object at `13b15c8f8` all zero, i.e. never initialized).

The game reads `MiiResource:/` (strings in main at `0x8f1287d..`): `NXTextureLowLinear.dat`,
`NXTextureLowSRGB.dat`, `NXTextureMidLinear.dat`, `NXTextureMidSRGB.dat`, `ShapeHigh.dat`, `ShapeMid.dat`, from
system data 0802. `svc_fs.c` served it empty (`fs: system data 0100000000000802 (not available on PC: provided
empty)`).

How the renderer reads it (`F_88E7D3C`, `F_88E8D40` and helpers in `_build/code_077.c`):

- `F_88EF9D4(shapeFile, partType<=11, index)` → `file + base[type] + index*0x58`. 12 kinds, bases 108, 460,
  1516, 2572, 14188, 25804, 37420, 49036, 60652, 72268, 72356, 73940 (the `0x10000|` forms in the generated C
  are only how constants ≥ 65536 are materialized, not flags). Tables tile exactly, end 75524.
- `F_88EFBC8(textureFile, partType<=10, index)` → `file + base[type] + index*0x1C`. 11 kinds, bases 100 to
  8752, counts 132, 62, 24, 2, 12, 12, 20, 2, 37, 6, 18 (327 entries; tables end 9256).
- `F_88EF5E0(part)`: a part is valid when its `u32` size at `+4` is not 0. Valid parts are copied into a memory
  pool and mapped. Invalid ones are skipped (`goto L_88E922C`).

What was tried, and the result of each:

| MiiModel served as | Result |
|---|---|
| empty archive (before) | sizes read from heap garbage → valid-looking parts → `nvnMemoryPoolMap(NULL)` |
| yuzu's synthetic files: 16-byte `NFTR`/`NFSR` headers alone (`file_sys/system_archive/mii_model.cpp`) | same crash: the tables lie past the end of a 16-byte buffer |
| the real archive from the console firmware (Ryujinx `bis/system/Contents/registered`, `70c80ddf93c02507c230372bb457a951.nca`, PublicData, 6 files, 5.8 MB) | **ran 40+ s, 2100 frames, reached character select** |
| headers zero-padded to 64 KB (textures) / 128 KB (shapes) | renderer initializes, skips every part → crash 5 |

The firmware route was dropped on the owner's decision: a static recompilation must not depend on firmware dumps.
That code (`--firmware`, `<data>/system/<id>/` folders) was never pushed, and no firmware data is in the build
folder.

yuzu's 16-byte files work under yuzu only because yuzu's memory gives zeros for reads past the end and through
NULL. On Windows this runtime is identity-mapped (`memory: identity mapped`) and cannot map page 0 (`mem.c`: "page
0 can't be mapped"), so NULL reads crash in both game code and the HLE. Guarding a few HLE entry points
(`nvnMemoryPoolMap`, `FlushMappedRange`, `GetSize`/`GetFlags`/`GetBufferAddress`/`Finalize` returning 0 for a NULL
pool) only exposed the next one (`nvnBufferMap(NULL)`), so those guards were reverted.

### Fix (9d0acd6)

`sysdata_open` synthesizes 0802 like NgWord: the six files carry yuzu's headers (`NFTR\1` / `NFSR\1`)
zero-padded so every part table reads size 0. Shape tables end below about 86 KB (largest base 73940 plus 132
entries × 0x58), texture tables below about 13 KB. The renderer then initializes and skips all parts.

## 5. Open: the Mii face texture (crash at 6.8 s)

```
[   6.82] *** Crash ... access to 0000000000000000 (game address 0) ***
          in the runtime itself (exe offset +13f735)      -> vkt_reg_tex (runtime/nvn/vk/vk_tex.c)
   #0 881a7b4   (F_881A724 -> F_862E0BC -> nvnTexturePoolRegisterTexture(pool, 0x1279, NULL, NULL))
```

With no parts, the Mii renderer returns a part holder (`F_88E624C` → `x23`) whose face texture at `+8` is NULL,
and the game registers it.

A guard that leaves the slot empty (`if (!t) return;` in `nvnTexturePoolRegisterTexture`) was tried. The game
then crashed in its own Mii drawing code (`88f0fd0`, `F_88EB1C0`, under `88e6550`) on a garbage pointer
(`0x23b0c1010`), because it draws from parts that do not exist. The guard was not committed.

Getting further needs MiiModel content the renderer accepts, made up by the runtime.

### The format (deep dive 2026-11-17)

A verification pass re-read the generated C and the module bytes (main base 0x8005000) and cross-checked public
reverse engineering of the same archive: the Wii U FFL resource format it descends from (the public
`aboood40091/ffl` decomp), and three synthesizers written against real firmware data — S2Arm
`pack/sysdata_mii.py`, nxnative `dm/host/hle_mii.c` (another MK8D native host, same SDK 4.4.0), and the Lichii
yuzu fork's `mii_model.cpp`. The layouts agree. Corrections to earlier notes: the sdk module carries no
`nn::mii` resource code — the loader is entirely in main — and the `0x10000|` bases are constant
materialization, not flags.

- The renderer validates almost nothing: no `NFTR`/`NFSR` magic, no file length, no index bounds. The only gate
  is entry `+4 != 0` (`F_88EF5E0`). The file pool is sized from `GetFileSize`; the model's decompress pool is
  sized from the header's largest-size and alignment arrays (nxnative cites MK8D `main+0x8f2e6c`), so the
  headers must be filled — zero headers only got this far because every part was skipped.
- Every payload is zlib: the copy call `F_8B59EA0` is `nn::util::DecompressZlib(dst, size@+4, file+offset@+0,
  compressedSize@+8, work, 7152)`. The Wii U raw escape (strategy 5) did not survive; synthesized payloads must
  be valid zlib streams — a stored deflate block plus Adler-32 is enough. The `0x806` params `u32` at `+0xC`
  is metadata MK8D never reads.
- Texture entry, 0x1C at `file + base[type] + index*0x1C`: `+0` payload offset, `+4` decompressed size (the
  gate, also zlib dstSize), `+8` compressed size, `+0xC` zlib params (unread), `+0x10` alignment (unread),
  `+0x14` width, `+0x16` height, `+0x18` format index into the game's 7-entry palette (`0x201`, `0x901`,
  `0xb01`, `0x1d01`, `0x1e01`, `0x2001`, `0x2d01`; index 0 = R8_UNORM, all supported by the vk backend),
  `+0x19` mip count, `+0x1A` tile index (0 = plain; the `0x2380`/`0x2381` path for larger values is unprobed —
  stay at 0).
- Shape entry, 0x58 at `file + base[type] + index*0x58`: `+0/+4/+8` payload offset / decompressed / compressed
  size, `+0xC` zlib params (unread), `+0x10..+0x37` five attribute strides then five attribute offsets
  (position stride 8: four f16), `+0x38` index offset, `+0x3C` index byte count (u16 indices, count =
  bytes/2), `+0x40` bounding box (6 floats). After the tables: hair transforms 132×0x48 at 75524 and faceline
  transforms 12×0x24 at 85028 (end 85460). Zero-filled transforms are safe — float-only consumers, the model
  just sits mispositioned.
- What the title-menu model actually touches: texture kinds 0 (hair), 10 (nose) and 6 (glasses — only when the
  CharInfo glassType is non-zero; the runtime's default Miis have 0). Shapes kind 0 and 1 (faceline) for the
  draw, plus kinds {5, 6, 7, 8, 3, 4, 10, 11} only when the flag bytes `impl+9338..9345` (CharInfo bits) are
  set. An invalid shape entry just drops the part, but the draw runs in the same frame as registration
  (`F_881A724` inserts the model, `F_88F0F78` uploads), so the missing faceline position buffer is exactly the
  guard crash above: texture-only validity does not survive the title frame.
- Prior art agrees on strategy: yuzu's 16-byte stubs only survive because few games look inside; nxnative,
  S2Arm and Lichii all ship all-entries-valid degenerate archives and report blank Miis without crashes.

### Plan

1. `sysdata_open` (`runtime/svc_fs.c`) builds the six files for real instead of zero-padded headers:
   - Header: magic, version 1, file length, then largest-size `u32[11 or 12]` and alignment arrays — one page
     (0x1000) per kind, first alignment 0x1000 (nxnative's tested values; the largest sizes size the model's
     decompress pool).
   - 11 texture tables (counts 132, 62, 24, 2, 12, 12, 20, 2, 37, 6, 18; 100..9256) or 12 shape tables (counts
     4, 12, 12, 132, 132, 132, 132, 132, 132, 1, 18, 18; 108..75524) plus the two transform tables, then the
     shared payloads (texture file ≥ 9256 bytes, shape file ≥ 85460).
   - Textures: every entry valid, pointing at one shared 8×4 R8_UNORM texture (palette index 0, mips 1,
     tile 0) as a zlib stored block.
   - Shapes: every entry valid, pointing at one shared degenerate mesh — 1 vertex (8-byte f16 position),
     u16 indices (0, 0, 0), strides {8, 0, 0, 0, 0} — as a zlib stored block; zero transform tables.
   - Reference implementation: nxnative `dm/host/hle_mii.c`, cloned at `D:\Projects\nxnative` (MIT; Apple-only
     host, but the stand-in writer is plain portable C — the same 0x20 zero bytes inflate to both the blank
     texture and the degenerate mesh: zlib `78 01 01 20 00 df ff` + Adler-32). Same layout serves the Switch 2
     MiiResource 0400000000000849, which adds `OunceTexture{Low,Mid,High}{Linear,SRGB}.dat`.
2. Probe run: `SWR_TRACE_FN=8b59ea0,88ef71c,88ef76c,88ef5e0,88ef7bc,88ef840,88efb08,88efd40,858badc,88e7d3c,88e609c,88ef904,88ef920,88f0f78,881a724`
   shows the (file, kind, index) traffic, zlib acceptance, and what gets registered.
3. Iteration is runtime-only: edit `svc_fs.c`, `switchrecomp.py --build-runtime`, relaunch. `runtime/cpu.h` is
   untouched, so no ABI bump.
4. Expected end state: the title menu renders with blank/placeholder Miis (nxnative and S2Arm report the same),
   no crash. Residual unknowns, probe-only: tile-index values ≥ 2, whether anything reads entry `+0x10`, and
   what the character-select path (reached at 40 s with the firmware archive) requests beyond the title path —
   the all-valid archive covers it by construction.

## How to reproduce and analyse

Real build and library only (owner's rules). Build through the CLI with the launcher's arguments:

```
python -u src/recomp_cli.py "P:/Games/Nintendo/DUMPS/Mario Kart 8 Deluxe/dump" ^
  --outdir "P:/Games/Nintendo/DUMPS/Mario Kart 8 Deluxe/SwitchRecomp Builds/Mario Kart 8 Deluxe 1G1U1D" ^
  -o "Mario Kart 8 Deluxe 1G1U1D" --target win64 --backend win32 ^
  --cc third_party/gcc/Library/bin/x86_64-w64-mingw32-gcc.exe --jobs 4 -O 2 ^
  --runtime third_party/Switch2RECOMP/bin/runtime_win64_win32.exe
```

After changing `runtime/nvn/*`, run `tools/build_nvn.py --jobs 4` and `switchrecomp.py --build-runtime` first. The
build only re-copies the runtime; it rebuilds it automatically only for an ABI mismatch.

Techniques that found each step:

- **Generated C for a guest address:** `grep "^void F_<ADDR>(CPU"` in `<game>_build/code_*.c`. A call site is
  found by its return address: `c->x[30]=<decimal ret addr>;`. The crash dump's `pc` is the last block marker;
  `lr` and the `#n` frames are the reliable chain.
- **Runtime offsets:** the dump prints `exe offset +X`; `x86_64-w64-mingw32-nm -n bin/runtime_win64_win32.exe`,
  last text symbol `<= 0x140000000+X`.
- **GOT slots and PLT stubs:** with `nxloader.load_program(<dump>)`, read the module's `DT_RELA`/`DT_JMPREL`
  entries. `RELATIVE` gives `base+addend`; `JUMP_SLOT` gives the symbol name (`symlist[r_info>>32]`). A PLT stub's
  GOT slot is its `adrp` page + the `ldr` offset × 8.
- **Function arguments and results live:** `SWR_TRACE_FN=<hex addr>,...` logs the first 8 calls with x0–x3 and
  the return value (hex). It works for functions reached through the dispatch table (PLT and indirect calls).
- **SDK version:** `NintendoSdk_nnSdk-*` string in the sdk module.

## Verification (2026-11-17)

- Engine tests: `unittest discover -s tests -p "test_n*.py"` (17 OK), `test_python_selection.py` (OK),
  `test_controllers.py` (PASS), `test_mkw_runtime.py` (PASS).
- The CLI rebuild into the real buildDir; the game's exe hash equals `bin/runtime_win64_win32.exe`.
- Launch log: `built Nov 17`, `sdk: NintendoSDK 4`, `device API 53.313`, `system data 0802 (synthetic copy)`,
  `60 presents (4.4 s)`, then crash 5 at 6.82 s.
- PrintWindow captures: intro logo at about 5 s, title-screen key art at 5.2 s. With the firmware archive
  (experiment only), character select after 40 s.

## Open items

1. **MiiModel content** (crash 5): the format is fully mapped (section 5, deep dive 2026-11-17); what remains
   is writing the synthesizer in `sysdata_open` and probing. No firmware dump or emulator is needed — the game's
   own code is the spec, and nxnative (another MK8D recomp host) ships the same synthesizer as prior art.
2. **Stale `libnvn.a` / runtime without an ABI change:** after a submodule bump the build still copies
   `bin/`'s runtime and links `build/nvn/libnvn.a` as they are. Only an ABI mismatch triggers a rebuild, so fixes
   in runtime sources are silently missing until `build_nvn.py` and `--build-runtime` are run.
3. **Picture in the top-left third of the 4K fullscreen window** in captures. This is likely the 300% display
   scaling (DPI awareness, present size), not investigated.
4. **Save data from test sessions** in `<buildDir>/Mario Kart 8 Deluxe 1G1U1D_data/save`. One run reached the
   menus and saved. Delete it for a fresh start.
5. **The runtime's crash hint** ("got NULL from its allocator (out of memory?)") misled twice here. Both NULLs
   were unset globals or members, with plenty of memory free.

## Commits

Engine (`turbo`): e440705 ABI guard · 2f8f44e SDK version → NVN API · 2a42369 default Miis · 5f80160
UpdateLatest/GetIndex NotFound · 9d0acd6 MiiModel zeroed tables. Launcher (`main`): 64c10f5 submodule bump.
