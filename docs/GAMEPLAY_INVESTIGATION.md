<!-- SwitchRecomp - Copyright 2026 MZ Labs
     Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
     Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. -->
# Gameplay investigation: Donkey Kong Bananza, Mario Kart World, Pokemon Legends Z-A

Status 2026-11-17. Owner's report: PLZA and DKB do not work past their title screens; in gameplay textures and graphics
do not load (black screens, black gaps); MKW plays at about 30 FPS instead of 60.

| game | before | now | open |
|---|---|---|---|
| Donkey Kong Bananza | gameplay reached, terrain black | terrain shaded, tutorial video plays | the console's own reset of the append counters is not identified (emulated by rule); queue order across GPU events; 40-60 FPS |
| Mario Kart World | race at 44-51 FPS | race at 57 FPS mean, 59 in steady stretches | streaming and pipeline stalls (p99) |
| Pokemon Legends Z-A | title -> Language -> Appearance | loads the save into the world; UI drawn, **world black** | deferred lighting / post-processing output black; transform feedback (skinned characters) not implemented |

Display: console session, 60 Hz, Vulkan on an RTX 5090. All numbers are from the real library builds, rebuilt through
the engine CLI into each entry's `buildDir`, driven by `tools/plza_probe.py` (pad replay, runtime screenshots).

## 1. Donkey Kong Bananza: black terrain

### What the frame does

- Depth pre-pass (6 passes, 630-720 draws): each terrain chunk is drawn with VS `f9c807930abd1234`, FS
  `7551b531372e364c` and a geometry shader `5d537f7630ec3671`, an NVIDIA passthrough GS. Two storage buffers are bound to
  the geometry stage per chunk: slot 0 a 64-byte record, slot 1 a 15,000-byte list. Per visible triangle (front facing,
  on screen, covering a pixel centre) the GS adds 3 to word 0 of the record (one `ATOMG` add for the whole warp) and
  stores the triangle's three 16-bit indices into the list behind the old count (three `STG.U16`). The original Ampere
  code has exactly these four memory instructions (checked with the opcode table of `runtime/sm86.c`); the translated
  SPIR-V does the same.
- Shading pass (one pass, same chunks): `nvnCommandBufferDrawElementsIndirect(TRIANGLES, U16, index buffer = the list,
  arguments = the record)`, VS `d661413914c84d55`, FS `6d1b110994db5413`, depth EQUAL, no depth write.

So the record is both the GS's append counter and the draw's `NVNdrawElementsIndirectData`. The engine's own layout
object for the record (class with vtable `0xa3dfe90`, the object at draw object + 120) says the same: one field of type
19 with 5 components, element size 64.

### What was wrong here

The records were `{count, 0, 0, 0, 0}` with a count that only ever grew (+54 a frame for an 18-triangle chunk; 612,759
after some minutes). Two things follow: every append lands past the end of its 15,000-byte list, and the draw has **0
instances**, so the shading pass drew nothing. The pre-pass depth was there, nothing was shaded over it: black.

### What writes the record (everything that was checked)

Nothing but the GS:

- CPU: no store, ever. A polling thread on 8 records from their first use saw only write-back landings change them. A
  page watch (PAGE_NOACCESS, re-armed every millisecond) on a record's page saw no guest read or write in 700 frames.
  The pools the records come from (256 KB, flags CPU_CACHED | GPU_CACHED) were write-protected from their creation:
  the guest never wrote a page of them. A scan of all 9 GB of committed guest memory found no array of
  `{n, 1, 0, 0, 0}` at a 64-byte stride (no CPU-side copy to upload from). A watch on the engine's object for a
  record saw only the command-list recorder read it.
- GPU commands: the game never calls ClearBuffer, CopyBufferToBuffer or CopyTextureToBuffer (call census,
  `SWR_NVN_CALLS=1`); its UpdateUniformBuffer calls all target the uniform pool; no dispatch and no other draw binds the
  records (pass dump with every record listed); the terrain shaders have no memory instruction besides the GS's four.
- Not the cause: write-back races (ruled out before), the two NVN queues' order (below), device information the game
  asks for (only item 0x36 per frame).

The same engine class is used once elsewhere with CPU initialisation: `F_819xxxx` (return address `0x81a01fc`) creates
a counter record and an arguments record and writes `{0, 1, 0, 0, 0}` into the latter. The terrain's records get no such
store.

**Not found: what the console does so that each frame's list starts at 0 and is drawn once.** Either NVN2 has a
behaviour not visible in the command stream, or a path of the game that does it does not run here. This is the open
question of this investigation.

### The fix (emulation by rule, `7b5a28c`)

`runtime/nvn/vk/vk_replay.c`, "Append counters that are indirect draw arguments": an indirect draw whose arguments a
draw-stage shader wrote as a storage buffer in the same frame

- gets one instance when its record holds none (written into the record once),
- has its count set back to 0 when the next frame begins, before anything appends again (`vkCmdFillBuffer` in command
  order, mirrored to guest memory like every GPU write).

It is keyed on the structure, not on the title: records written by compute dispatches or by the CPU are not touched.
`gamectl set append_reset 0` (`NXRT_VK_APPEND_RESET=0`) turns it off. Metric `append_counter_reset` counts the resets
(about 630 a frame in the mining tunnel).

### Second fix: storage buffers bound with size 0

DKB's second NVN queue runs one dispatch (`7516216f8dcdb9e1`: 64 threads over the depth buffer, appending points to a
list) whose storage buffer is bound with **size 0** (`BindStorageBuffer(compute, 2, addr, 0)` from the engine's generic
bind at `0x82c7a38`). An NVN shader holds the buffer's address and reads and writes through it; the size is only what
the driver tells the shader. The replay bound such a buffer to nothing, so every entry was lost while the CPU reads the
buffer back each frame (`InvalidateMappedRange` of its 0x440 bytes). Now bound to the end of its pool (at most 1 MB).
With this the game started a compute pass and several draws it had never issued before, and the tutorial video panel,
a flat yellow box before, shows its video.

### Result

Same scripted run (title, Continue, walk, punch, walk):

| | before (`20261117-141344-dkb-cpuwins`) | after (`20261117-164844-dkb-after`) |
|---|---|---|
| start of gameplay: near-black pixels / black cells of 64 | 68.6 % / 11 | 21.6 % / 0 |
| after walking | 73.7 % / 33 | 3.0 % / 0 |
| after punching | 56.0 % / 22 | 6.6 % / 0 |
| after the second walk | 56.6 % / 21 | 0.1 % / 0 |
| FPS in gameplay (mean) | 45.8 (nothing shaded) | 39.6 at that commit; see section 4 for the final build |

### Open for DKB

- The console's reset (above).
- GPU events across queues. The frame's two queues hand over with events: main `Signal(E0=0)`, pre-pass, `Wait(E1==1)`,
  two passes and three dispatches, `Signal(E0=2)`, `Wait(E1==3)`, shading; second queue `Wait(E0==0)`, `Signal(E1=1)`,
  `Wait(E0==2)`, its dispatch, `Signal(E1=3)`. The replay runs each submission whole, in submit order, so the second
  queue's dispatch runs before the main queue's frame instead of between its pre-pass and its shading (it reads last
  frame's depth), and the waits time out once ("a GPU wait on an event ... was not met in 250 ms") and are then skipped.
  Doing it right means suspending a submission's walk at a wait and continuing another queue's (fibers). Pass dumps now
  show the waits and signals.
- One `[RED] a 3D texture read as another type` (texture 1x1x1, shader `4419f04c58bf1535`).

## 2. Mario Kart World: frame rate

### Where the time went

A race frame's period is the translation thread's time for the frame plus the landing of the frame's GPU write-backs:
the game waits for the frame's fence every frame (`SyncWait` 4.8 ms a frame), and a fence is reported after the
submission's completion work, which includes writing the GPU's storage-buffer changes back to guest memory. Neither the
GPU (6.5 ms), nor the guest CPU (busiest thread 26 % of a core), nor the translation thread alone (12.7 of 18.4 ms)
was saturated.

New tool: `SWR_HOST_PROF=<start s>,<seconds>` samples the translation thread (or, with
`SWR_HOST_PROF_THREAD=completion`, the completion thread) 2000 times a second; `tools/host_prof.py` prints the table by
function, with time in other modules by export and by calling function. Findings in the race (30 s):

| where | cost | cause |
|---|---|---|
| completion thread, `prep_write` | 4-5 ms a frame | each page a write-back changed scanned all 5,200 tracked ranges (pools) |
| completion thread, `wb_land` compare | 3 ms a frame | 14.7 MB a frame of write-back compared (six buffers of 0.5-4.7 MB written by compute every frame; 214 KB really change) |
| translation thread, `gpu_pending` + `guest_copy` | 2 ms a frame | 3,700 pages a frame copied aside when a GPU write is recorded; the pending counters' cache lines shared with the completion thread |
| translation thread, `vkm_use_` + `nvn_dirty_since_span` | ~1 ms | each of 16,000 uses a frame looked at its pages' stamps again |
| translation thread, `fl_draw` | 0.2 ms | two `strtoull` of the shader keys per draw |

Left as they are (measured, not changed): descriptor set contents (texture and buffer lookups, 5 ms of the thread; the
allocation and `vkUpdateDescriptorSets` are only 1.5 ms of it), `walk` (cache misses on the game's command lists),
`vkt_resolve`, the Vulkan driver itself (4 ms).

### Fixes (`5758e24`)

- `nvn_dirty.c`: the tracked ranges sorted by address behind a reader lock, rebuilt when a pool comes or goes
  (`NXRT_DIRTY_INDEX=0`: the old scan).
- `vk_mem.c`: a range used once in a submission stays as copied for that submission (`use_fast 3`; `gamectl set
  use_fast 1` restores the per-use look). The pending counts are taken down by the translation thread
  (`NXRT_VK_PENDING_DIRECT=1`: as before). Large page copies and compares are split among the caller and three helper
  threads (`NXRT_VK_PAR=0`: alone).
- `vk_replay.c`: a shader's key as a number once.

### Result

Same scripted run (title, L+R, A x8, Grand Prix, Mario Bros. Circuit, 2 x 40 s of driving), window 150-185 s:

| | before (`20261117-170738-mkw-prof2`) | after (`20261117-173453-mkw-perf3`) |
|---|---|---|
| FPS mean | 50.8 | 57.3 |
| seconds at 58 FPS or more | 6 of 35 | 25 of 35 |
| present interval p50 / p95 / p99 | 18.46 / 21.41 / 36.17 ms | 16.68 / 18.05 / 27.31 ms |
| intervals over 20 ms / over 33 ms | 300 / 20 | 41 / 12 |
| translation thread per frame | 12.7 ms | 9.5 ms |
| game threads in SyncWait per frame | 4.8 ms | 0.2-1.9 ms |

(`20261117-165429-mkw-base`, with phase tracing on: 44.3 FPS mean; the hand-off's run: 45.1.) The log's own 300-frame
windows read 59.0 FPS (p50 16.66-16.70 ms) in steady stretches; the remaining dips are single long frames from texture
streaming and first-use pipelines (p99), as in `MKW_PERFORMANCE_INVESTIGATION.md`. Graphics checked in the race
screenshots: no black areas (near-black 0.0-0.3 %).

Metric `ind_draw_no_instances` counts indirect draws whose record holds 0 instances *as guest memory has it at replay*;
in MKW (about 50 a frame) those are records a compute pass of the same submission writes, not missing draws.

## 3. Pokemon Legends Z-A: into the world, world black

The library build has a save now (the owner chose language and appearance), so the game boots to its title, and A
loads the save: a train station, speech bubbles, "A Talk". **Only the UI is on screen; the world is black**
(`20261117-174600-plza-play2/0003-shot-settled.png`, near-black 98 %). The save was copied aside before the runs
(`build/plza-investigation/plza-save-backup-20261117`) and is byte-identical after them.

What the texture dump shows (`20261117-174922-plza-td/gbuffer_sheet.png`, pass dump in its `run.log`):

- The G-buffer pass (1920x1080 targets, the game renders 1280x720 into them) is drawn: albedo (`RGBA8_SRGB`) has the
  station, tiles, wall and train; normals and the material target too; depth is written.
- The lit target (`R11G11B10F`, ser24979) is black except the sky and the train's lights. Two inputs of the lighting are
  all zero: the fourth G-buffer target (`R11G11B10F`) and an `RGBA16F` pair. The final targets hold only the UI.
- 190 transform feedback draws a frame (points, 3 buffers of 12 + 4 + 4 bytes a vertex: skinned positions, normals,
  tangents). **Transform feedback is not implemented in the Vulkan replay** (`[RED] transform feedback is not captured
  yet`): skinned characters cannot appear. Pass dumps now list the feedback buffers and begin/end.
- `[RED] a cube view of a 64x64 / 128x128 texture that is not a cube map: viewed as a 2D array` (8 times): the
  environment maps the lighting reads.
- 160 draws skipped at first for 4 shaders the live translator then delivered.

Not done: finding why the lighting output is black (suspects in order: the cube-view textures, the three 26x1x1
dispatches that build the light lists, the shadow target), transform feedback (needs the varyings in the translated
vertex shaders, `VK_EXT_transform_feedback` pipelines, capture and write-back).

## 4. Regression checks and final numbers

All five library builds were rebuilt through the engine CLI with the final engine (Ocarina of Time and Mario Kart 8
Deluxe were recompiled in full: their generated code was older than the engine).

| game | run | result |
|---|---|---|
| Donkey Kong Bananza | `20261117-180124-final-dkb` | title, Continue, walk, punch through the gold wall (counter 0 -> 152.63), walk: terrain shaded in every screenshot, 0-1 black cells of 64 (near-black 1-45 %: the cave beyond the wall is dark). 36.9 FPS mean in gameplay while a 4-job compile ran beside it (39.6 without, `-164844-dkb-after`); present interval p50 25 ms. DKB is below 60 FPS: its translation thread takes 12-14 ms a frame |
| Mario Kart World | `20261117-173453-mkw-perf3` | section 2 |
| Pokemon Legends Z-A | `20261117-174600-plza-play2`, `-174922-plza-td` | section 3: in the world, world black |
| Mario Kart 8 Deluxe | `20261117-185027-final-mk8d` | as before this round: the Mii renderer's null access 6.6 s in (`MK8D_BOOT_INVESTIGATION.md`), not worse |
| The Legend of Zelda: Ocarina of Time | `20261117-184326-final-zelda`, `-184812-final-zelda2` | **was not starting before this round either**: "LZ4 failed in main", also in the owner's own log of the day (kept as `prior-run.log`). The recompiler read the ZBIC-compressed NSO, the runtime's loader reads LZ4 only. Fixed in the build step: such an NSO is written into the build with its segments unpacked (`nxloader.nso_for_runtime`). The game now loads and runs its start-up, then stops with svcBreak 1.6 s in, right after SetHeapSize (6316 MB): open |

Tests: `python -m unittest discover -s tests -p "test_n*.py"` (17), `tests/test_controllers.py` (4 groups),
`tests/test_mkw_runtime.py`, `tests/test_mkw_perf.py`: all pass. Linux: the NVN archive and the runtime build
(`tools/build_nvn.py --linux`, `--build-runtime --target linux --backend x11`). Engine CI on `turbo`: green.

## Instrumentation and switches added in this round

| switch | what |
|---|---|
| `SWR_HOST_PROF=<start s>,<seconds>[,<file>]`, `SWR_HOST_PROF_THREAD=completion` | sampling profile of the translation or completion thread; `python tools/host_prof.py <Game>.log.hostprof.txt` |
| `SWR_NVN_CALLS=1` | every 30 s, the NVN entries the game called and how often |
| metrics columns `append_counter_reset`, `sbuf_size0`, `ind_draw_no_instances` | per second, with the other dropped-work counters (`SWR_METRICS`) |
| pass dumps (`gamectl passdump 1`) | GPU event waits and signals, transform feedback buffers and begin/end |
| `gamectl set append_reset 0`, `use_fast 1`; `NXRT_DIRTY_INDEX=0`, `NXRT_VK_PAR=0`, `NXRT_VK_PENDING_DIRECT=1` | each fix off, for A/B |

Earlier switches (metrics, input record/replay, harness) are in `PLZA_BLACK_SCREEN_INVESTIGATION.md` and in
`tools/plza_probe.py`'s header. The harness's `ctl texdump` writes its images beside the game's log: move them into the
run folder afterwards.

## Evidence

All under `build/plza-investigation/` of the worktree (not in git):

- DKB: `20261117-143517-argdbg` .. `-161911-obj3` (the record watches and engine object dumps), `-152753-pdops` and
  `-160514-early` (pass dumps with every record), `-150633-hack` (first proof that reset + one instance gives the
  right image), `-164844-dkb-after`.
- MKW: `-165429-mkw-base`, `-170738-mkw-prof2` (`hostprof.txt`), `-172357-mkw-perf2` (completion thread profile),
  `-173453-mkw-perf3`.
- PLZA: `-174600-plza-play2`, `-174922-plza-td`.
