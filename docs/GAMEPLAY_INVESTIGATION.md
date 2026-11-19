<!-- SwitchRecomp - Copyright 2026 MZ Labs
     Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
     Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. -->
# Gameplay investigation: Donkey Kong Bananza, Mario Kart World, Pokemon Legends Z-A

Status 2026-11-17. Owner's report: PLZA and DKB do not work past their title screens; in gameplay textures and graphics
do not load (black screens, black gaps); MKW plays at about 30 FPS instead of 60.

| game | before | now | open |
|---|---|---|---|
| Donkey Kong Bananza | gameplay reached, terrain black | terrain shaded by the game's own mechanism (section 6), queues interleaved at their event waits, 48.7 FPS in the tunnel | 60 FPS: the per-chunk write-backs (section 6) |
| Mario Kart World | race at 44-51 FPS | race at 58.6 FPS mean | texture streaming bursts (single frames of 60-170 ms) |
| Pokemon Legends Z-A | title -> Language -> Appearance | in the world, lit, characters skinned, walking; 30 FPS (the game's interval) | 32x32 cube placeholders viewed as arrays |
| Ocarina of Time | did not start (svcBreak 1.6 s in) | title and opening movies on screen at 60 FPS with their sound (section 8: push constants, the movie network by the host, libopus) | the game itself after the opening: black, 1 to 5 frames a second, then it stops (section 8, "Open: the game itself") |

Sections 1 to 5 are the first round as it was written; section 6 is the second round, which found the causes sections 1
and 3 left open; section 8 (2026-11-18) is Ocarina of Time as the owner starts it, and the window mode. The AV1 decoder that section 6 left open for Ocarina of Time is in `AV1_DECODER.md`.

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

## 5. Hand-off: done, remaining, next steps, quick wins, hunches

**The current hand-off is in section 8 ("Hand-off (2026-11-18)"): Ocarina of Time is not playable past its opening, and a new
investigation is needed. What follows is the first round's.**

### Done (on `turbo`, engine CI green; launcher release v0.2.0)

- DKB: terrain shaded (append counters reset per frame, one instance), size-0 storage buffers addressable, tutorial
  video plays. Verified in gameplay (walk, punch through the gold wall).
- MKW: race 50.8 -> 57.3 FPS mean, 59 in steady stretches (range index in the dirty tracker, one look per range per
  submission, pending counts on one thread, helper threads for the big copies and compares).
- Ocarina of Time: loads its ZBIC executables (it never had); now stops later.
- Tools: `SWR_HOST_PROF` + `tools/host_prof.py`, `SWR_NVN_CALLS`, new metrics, pass dumps with events and
  transform feedback; tests repaired (`test_controllers.py`, `test_mkw_perf.py`).

### Remaining

1. **PLZA: the world is black in gameplay** (UI only). G-buffer is drawn; the lighting output is black; transform
   feedback (skinned characters) is not implemented. This is the owner's "black screen past the title" for PLZA.
2. **DKB: the console's own counter reset is not identified**; the fix is a rule keyed on the frame's structure. DKB
   runs at 37-45 FPS in the tunnel.
3. **GPU events across NVN queues** are not honoured (DKB's second queue runs at the wrong point of the frame).
4. **Ocarina of Time**: svcBreak 1.6 s in, right after `SetHeapSize` 6316 MB.
5. **MK8D**: the Mii renderer crash (unchanged).
6. **MKW**: single long frames from texture streaming and first-use pipelines (p99 27 ms, a few over 100 ms).

### Next steps, in order

1. PLZA lighting: `gamectl texdump` + `passdump 1` in the station (`20261117-174922-plza-td` has one set). Hide the
   lighting pass's inputs one at a time (`gamectl hide <shader key>`) and look at which sampled texture is zero: the
   fourth G-buffer target (`R11G11B10F`, cleared to 0 and apparently never written) and the `RGBA16F` pair are all
   zero today. Find the draws that should write them in the pass dump (pass with `c1..c4`, no `c0`).
2. PLZA transform feedback: `OP_TF_BIND` / `OP_TF_STATE` are recorded and ignored (`vk_replay.c`). Needed: the
   varyings of the vertex shader as XFB decorations in the translated SPIR-V (the control section or
   `nvnProgramSetTransformFeedbackVaryings` names them), a pipeline variant, `vkCmdBindTransformFeedbackBuffersEXT` /
   `Begin` / `End` around the draws, and a write-back of the captured ranges. The device and the pool buffers already
   have the extension and the usage bits.
3. Events across queues: run each submission's walk on a fiber (Windows fibers, `ucontext` on Linux); at a wait that
   is not met, switch to the next queued submission of another queue; resume when the signal has been replayed.
4. Ocarina: log=2 run, read the function that calls svcBreak (return address in the log's stack) in the generated C.

### Quick wins

- **PLZA cube views**: 8 `[RED] a cube view of a ... texture that is not a cube map: viewed as a 2D array`
  (64x64 and 128x128: the environment maps). One attempt (create square 6-layer arrays cube compatible, in
  `vk_tex.c` image creation) did not change the message, so those textures are made on another path or are not 6-layer
  arrays: print target, layers and creation path in that RED line first. Cheap to settle and a direct lighting input.
- **Completion thread sleep**: `vk_submit.c completion_main` sleeps 1 ms when nothing is submitted; a condition
  variable woken at commit removes up to a millisecond from the fence the game waits on every frame.
- **Write-back volume**: six MKW buffers (0.5-4.7 MB) are copied back every frame and 214 KB change. Remember per
  range how often a write-back changed nothing and skip the snapshot copy for ranges the CPU never writes
  (the pending-merge path is the only reader of the snapshot).
- **Descriptor sets**: per (stage, shader) keep the gathered image and buffer infos and re-gather only the kinds
  whose bind version moved; 5 ms of the translation thread in MKW and most of DKB's 12-14 ms.
- **DKB perf**: the 630 per-frame resets go through `vkr_fill` one by one (a fill, a post and a pending count each);
  one batch of `vkCmdFillBuffer` without the guest mirror would do.

### Hunches

1. **PLZA black world = one missing lighting input, not the whole renderer.** Albedo, normals and depth are right;
   the sky and emissive lights survive. A deferred renderer that multiplies by an all-zero light or exposure target
   looks exactly like this. The zero `R11G11B10F` G-buffer target and the cube views are the first suspects; the
   transform feedback gap explains missing characters, not the darkness.
2. **DKB's reset lives in NVN2 driver behaviour for draw-stage storage buffers or indirect draws**, not in the game:
   the CPU provably never touches the records, the stream has no write to them, and the engine's own layout calls the
   record an indirect-arguments struct while leaving the instance count 0. A test on a console (read the record back
   after a frame) would settle it in minutes; without one, the rule in place reproduces the visible result.
3. **The same "raw address" assumption is behind more dropped work.** The size-0 storage buffer was bound to nothing
   although an NVN shader only ever holds the address. Anything else the replay bounds by a size the console does not
   enforce (storage buffer ranges, texture buffer views, index counts read from guest memory before the GPU wrote
   them) is a candidate for black gaps in other games; `ind_draw_no_instances` and `sbuf_size0` in the metrics count
   two of them.
4. **Frame rate is now bounded by two serial stages per frame, not by raw speed**: translation thread, then the
   landing of write-backs before the game's fence. Anything that takes work off either (fewer bytes written back,
   descriptor reuse) moves MKW from 57 to a flat 60 and helps DKB the same way; the GPU has two thirds of its time
   free in both.

## 6. Second round (2026-11-17, evening): nxnative as the reference

nxnative (MIT, `D:\Projects\nxnative`) plays the same games on Apple hardware. Its per-game notes and reference documents
were read against this engine; the comparison is `docs/NXNATIVE_GAP_ANALYSIS.md`. The causes below were all in it.

### Donkey Kong Bananza: what resets the terrain's records

The open question of section 1 ("what the console does so that each frame's list starts at 0 and is drawn once") has its
answer in nxnative's DKB notes: **gsys's vertex shader `c9a56f796ea4a4cd` (96 points a frame) loads each record's 64-bit
address from its storage buffer 0 and stores {0, 1, 0, 0, 0} there**: count 0, one instance, before the culling geometry
shader appends. NVN2's indirect draw draws nothing for instance count 0 and never resets a record (their decoding of the
driver's macro 40).

Why section 1 did not find it: the store goes through a pointer the shader loaded from memory, so no draw *binds* the
records for it, and the pass dump listed the shader with one storage buffer. The translator had translated the stores (its
notes say "traced to no storage buffer: it goes through the memory pool table"), but the reflection named the pool table
and the write log as storage buffers kept in uniform slot 252, and the replay bound nothing for them: five stores per
point went nowhere.

Fix (`78ece7b`, `vk_mem.c` "The memory pool table: raw global memory", `vk_replay.c` "Raw global memory: where the stores
went"; nxnative's design, `docs/reference/shaders.md` "Global memory at an address loaded from memory"):

- the table: every pool mirror that has memory, sorted by guest address, with the mirror's device address minus the guest
  address as its delta; written again when a pool comes, goes or gets memory;
- the write log: the guest pages such stores went to, copied out and cleared once a frame; pages seen there are learned
  and copied back to guest memory with each such draw (77 pages in 6 ranges in the mining tunnel);
- both buffers are in device memory (in host memory, an Ocarina of Time dispatch with 384 raw loads and stores ran into
  the GPU's time limit: device lost);
- `append_reset` (the rule of section 1) is off by default; `gamectl set append_reset 1` brings it back.

Limits: a raw *load* reads the pool mirror, which is current only where the replay has used the pool (nxnative's memory is
the game's own). The translator here is older than nxnative's chain: no pointer sites, a table of 4,096 ranges.

### Donkey Kong Bananza: the two queues

`hle_nvn.c` "Submissions parked at a GPU event wait" (`362b172`): every submission is translated on a fiber of the
translation thread. At an event wait no recorded signal meets, what is recorded so far is committed and the fiber is left;
the thread goes on with the next queued submission of another queue and comes back when a signal that meets the wait has
been recorded. A present, or the same queue's next submission, never overtakes a parked one. In the tunnel: 11,348 waits met
by the other queue's commands in 155 s, none unmet, none forced; the "not met in 250 ms" line is gone. The second queue's
depth-sampling dispatch now runs between the main queue's pre-pass and its shading. Windows only (fibers); `NXRT_AQ_PARK=0`.

### Donkey Kong Bananza: frame rate

Same scripted run (title, Continue, walk, punch, walk), window 100-150 s, console session at 60 Hz:

| | before (`-180124-final-dkb`) | now (`-203354-dkb-final`) |
|---|---|---|
| FPS mean | 36.9 (39.6 with nothing beside it) | 48.7 |
| present interval p50 / p95 / p99 | 25.1 / 32.9 / 89.7 ms | 20.0 / 26.3 / 28.6 ms (8 intervals over 33 ms of 2,435) |
| terrain: black cells of 64 in the gameplay screenshots | 1, 0, 0 | 0, 0, 0, 0 |
| `append_counter_reset` a second | about 25,000 | 0 |

Where the frame goes now (`-201836-dkb-park`, `SWR_HOST_PROF=100,25`): the translation thread is busy 67 % of the time
(15.5 ms a frame) and waits for work the rest; the game's threads use 1.2 cores in all (busiest 29 %) and spend 12 ms a frame
in `SyncWait`. The frame is serial: the game's frame, its translation, the GPU, the landing of the write-backs, then the
fence the game waits for. Of the translation thread's busy time the Vulkan driver is 29 % (descriptor sets, barriers,
copies), and what the 630 terrain chunks' write-backs cost is about a quarter: each chunk's 64-byte record and 15,000-byte
list are copied back to guest memory every frame (two barriers and a copy each, a page copy aside when the write is
recorded, a compare when it lands, and the page sent to the device again after it landed), although the CPU never reads
them. nxnative has no such copies (its memory is the game's). `wb_add` now joins ranges that follow each other, which
takes most of the per-range cost; the per-page cost stays. With it and the parked queues the tunnel went from 43.5 to
48.7 FPS (`-201836-dkb-park` with the profiler on, then the run of the table).

**Open, and the next step for 60 FPS**: write-back on demand. A draw-stage storage buffer the CPU has not touched for some
frames stops being copied back; its guest pages are made inaccessible, and the first CPU access brings the device's bytes
in (drain, read back, back to eager copies for that page). That removes about 10 MB a frame of copies in each direction
from both threads.

### Pokemon Legends Z-A: the black world

The lighting was not the cause. The frame's last passes (pass dump of `-174922-plza-td`): pass 69313 draws
`vs=b29f08619f379a83 inst=32` into a 32x32 target, and pass 69314 tone-maps the lit frame through a 32x32x32 texture. That
is the colour grading table: one instance per plane, the vertex shader writing the layer (built-in 9 in its reflection).
`pass_begin` began every pass with `layerCount` 1, so plane 0 was written and 31 planes stayed black: the tone mapping
multiplied the world by black, and the UI, drawn after it, was fine. nxnative's Z-A notes have the same finding ("a black
world once the opening reached Lumiose"). Fix: a pass has as many layers as every attachment's view holds (`78ece7b`).

Also for Z-A, in the same commit:

- **Transform feedback**: `vksh_xfb_layout` reads what the program captures from its control section (strides at +0x6dc,
  per-buffer output counts at +0x6e8, buffer count at +0x6ec, locations at +0xdc + 0x80 b, component counts at
  +0x4dc + 0x80 b: nxnative's `tf_layout`); `vksh_xfb_variant` gives each captured output a variable of its own with as
  many components as the layout names (the translator's outputs are vec4s; Z-A captures xyz of location 0 and x of
  locations 1 and 2, strides 12/4/4) and stores it at each return; `tf_begin` / `tf_end` bind the feedback buffers and
  wrap each draw in `vkCmdBegin/EndTransformFeedbackEXT`; the captured bytes are copied back like any GPU write. 116 captures
  a frame in the station; the characters are there. `gamectl set xfb 0` turns it off.
- **Cube views of 2D arrays**: square arrays of six layers or more are created cube compatible, and a cube view of one
  is a cube view (the 64x64x6 and 128x128x6 environment maps). 32x32 placeholders with fewer layers are still viewed as
  arrays (eight `[RED]` lines; nxnative binds a zero cube there).

Result (`-193450-plza-layer`): title, A, the station with its people, lit; near-black 1.0 %, 0 black cells of 64 in all
five gameplay screenshots (98 % near-black before, `-174600-plza-play2`); 30.0 FPS, the game's own interval 2
(present interval p50 33.3 / p95 34.0 / p99 34.3 ms); 23,900 draws and 3,480 captures a second, none skipped.

### Ocarina of Time: from svcBreak at 1.6 s to the title screen

Seven stops, each found from the log's stack and fixed; the first two and the fifth are in nxnative's notes:

| at | stop | cause | fix |
|---|---|---|---|
| 1.6 s | abort in `pthread_mutex_lock` under the SDK allocator's first lock | SDK 22's `InternalCriticalSection` compares a mutex's owner with the thread ID at ThreadLocalRegion+0x110; 0 there equals an unlocked mutex's owner | `kernel.c` `kthread_create` writes the ID (nxnative, threads.md) |
| 5.4 s | abort in `nn::os::WaitForAddress(const void*, long long)` (InvalidEnumValue) | `WaitForAddress` type 3, WaitIfEqual64 | `kernel.c` `svc_wait_addr` |
| 5.3 s | abort with a socket result | `bsd` command 36 (a registration newer SDKs make first) answered "-1, errno" | `svc_core.c` `bsd_disp`: success |
| 5.3 s | `abort()` in gRPC's start-up (the game links `nn::npln`) | `EventFd` failed: no wake-up descriptor | `svc_core.c` `bsd_evfd`: event descriptors with Write, Read, Poll, Close (nxnative, services.md) |
| 5.3 s / 10.3 s | `std::chrono::system_clock::now()` throws | `clock_gettime(CLOCK_REALTIME)` reads the time service's shared page; the ContinuousAdjustmentTimePoint entries there are 0x38 bytes with the clock source ID at +0x28 (read from the SDK's code), not 0x48 | `svc_core.c` `time_shm_update` |
| 7.7 s | abort in `InitializeAv1DecoderByTransferMemory` | the title movie is AV1; the video service refused it | `svc_ns2.c`: AV1 is accepted and every packet gives a black picture (`vdec_blank`). **There is no AV1 decoder yet** |
| 13.5 s | device lost in a dispatch of `d15370a382630d4e` (384 raw loads, 384 raw stores) | the pool table and the write log were in host memory | device memory (above) |

Result (`-201542-zelda-vram`): the title screen with its logo and menu ("New Game", "Options"), 2,700 frames in 108 s
with no abort, crash or device loss. The picture behind the logo is black (the AV1 movie). Open: an AV1 decoder (done since:
`AV1_DECODER.md`, which also says why the picture is still black); 26.7 FPS
at the title; what nxnative lists for the game itself (task and mesh shader culling through raw loads, 4,170 pool ranges
against the table's 4,096, texture rewrites: turned on for this title, not yet seen working).

### Other changes of this round

- **The system tick of a Switch 2 program is 31.25 MHz** (`main.c`; nxnative, frame-rate.md). Every span a Switch 2 game
  measured in ticks was 0.6144 of its length before.
- The completion thread is woken by a submission instead of sleeping a millisecond (`vk_submit.c`).
- `game_profile.h`: `tex_flush_upload` / `tex_dirty_upload` by title (Ocarina of Time, Tears of the Kingdom).

### Mario Kart World

Compared with nxnative's notes (frame-rate.md, the game's README): its remaining long frames are the same two kinds as
here, pipelines compiled at first use and texture streaming, and its answers are Metal's (pipeline archives, placement
sparse textures); this engine already has a pipeline manifest with prewarm and sparse textures with shared tiles. Nothing
of this round was aimed at them. What changed for the game is the system tick (31.25 MHz) and the completion thread's
wake-up. Same scripted run as section 2, window 150-185 s:

| | section 2 (`-173453-mkw-perf3`) | now (`-202635-mkw-nx`) |
|---|---|---|
| FPS mean (metrics) | 57.3 | 58.6 |
| seconds at 58 FPS or more | 25 of 35 | 32 of 36 |
| present interval p50 / p95 / p99 | 16.68 / 18.05 / 27.31 ms | 16.67 / 17.66 / 25.98 ms |
| intervals over 20 ms / over 33 ms | 41 / 12 | 33 / 18 |

The race is at the same point after the same script (lap 1 of 3, 24th, 8 coins against 7), so the game's speed did not
change with the tick. No black areas (near-black 0.0-3.7 % in the twelve screenshots, 0 black cells). **Open, unchanged**:
single long frames (the longest in the window 168 ms) from streaming and first-use pipelines. The window's one cluster
(frames 8866-8875: 64, 119, 154, 168, 159 ms) is texture streaming, not pipelines: `frames.csv`'s cumulative `texture`
and `astc` columns move by 50-90 ms in each of those frames and `pipeline` not at all, with the translation thread's CPU
time at 150-250 ms a frame. A burst of new ASTC textures is transcoded and uploaded on the translation thread at first
use; a per-frame budget for that work (drawing a frame or two with what is already there) is the next step.

### Regression checks of this round

All five library builds were rebuilt through the engine CLI into their `buildDir` with the final runtime (after the merge
of `origin/turbo` with the Mario Kart 8 Deluxe Mii change) and run with the harness. The games' saves were copied aside
before the runs (`build/plza-investigation/save-backups-20261117b`) and put back after them where a run had written one
(Donkey Kong Bananza, Mario Kart World; Z-A's was unchanged).

| game | run | result |
|---|---|---|
| Donkey Kong Bananza | `-203354-dkb-final` | title, Continue, walk, punch, walk: terrain shaded in every screenshot, 0 black cells of 64 (near-black 17-35 %: the tunnel); 48.7 FPS, present interval p50 20.0 / p95 26.3 / p99 28.6 ms; 11,669 event waits met by the other queue, none unmet; `append_counter_reset` 0 |
| Mario Kart World | `-202635-mkw-nx` | above |
| Pokemon Legends Z-A | `-203745-plza-final` | title, A, the station; two walks with the stick: the scene moves, people and Pokemon are drawn; near-black 0.1-1.1 %, 0 black cells; 30.0 FPS, present interval p50 33.3 / p95 34.0 / p99 34.5 ms, longest 35.7 ms |
| Ocarina of Time | `-204152-zelda-final` | title screen and menu; 2,700 frames, 27.6 FPS at the title (present interval p50 36.1 ms); after A the opening movie (black: no AV1 decoder) |
| Mario Kart 8 Deluxe | `-204430-mk8d-final` | not worse: with turbo's Mii change it runs on (3,600 frames at 60 FPS, no crash); before, the Mii renderer's null access 6.6 s in |

Tests: `python -m unittest discover -s tests -p "test_n*.py"` (17), `tests/test_controllers.py`, `tests/test_mkw_runtime.py`,
`tests/test_mkw_perf.py` pass (the last two now make `build/mkw-investigation` themselves). Linux: the NVN archive and the
runtime build in WSL (`tools/build_nvn.py --linux`, `--build-runtime --target linux --backend x11`).

Run folders of this round are under `build/plza-investigation/` of the worktree `D:\Projects\Switch2RECOMP-gameplay2`
(not in git): `-193016-dkb-gmem`, `-201836-dkb-park` (with `hostprof`), `-203354-dkb-final`, `-193450-plza-layer`,
`-203745-plza-final`, `-202635-mkw-nx`, `-201542-zelda-vram`, `-204152-zelda-final`, `-204430-mk8d-final`.

### Switches added

| switch | what |
|---|---|
| `gamectl set append_reset 1` (`NXRT_VK_APPEND_RESET=1`) | section 1's rule again (off now) |
| `gamectl set xfb 0` (`NXRT_VK_XFB=0`) | no transform feedback captures |
| `NXRT_AQ_PARK=0` | event waits waited out in place, submissions replayed whole |
| `NXRT_VK_COMPLETION_SLEEP=1` | the completion thread's 1 ms sleep |
| `SWR_TICK_HZ=<Hz>` | the system tick's rate |
| `NXRT_TEX_REWRITE=0/1` | textures the CPU rewrites after creating them, read again or not |
| metrics columns `gmem_pages_learned`, `tf_captures` | pages learned from the raw global memory log; transform feedback captures |

## 7. Third round (2026-11-17, night): the gap list, and the resolution

Worked from `docs/NXNATIVE_GAP_ANALYSIS.md`'s open list in order, plus one item the owner added: the games looked soft, more like 720p than
1080p or 1440p. All numbers are from the scripted runs of `tools/play_scenario.py` (new: one command per game, the same steps as the runs of
sections 4 to 6) on the library builds, windowed on the 3840x2160 60 Hz display. Another agent (the AV1 decoder) built and ran the same library
folders during this round: every run here was checked to have run this engine's runtime (`provenance.json`, exe hash = runtime hash), and three
that had not were thrown away and run again. Its compiles and runs still share the machine, so single long frames are noisier than in section 6.

### The games drew for the console's own screen

Measured first (`render sizes up to frame` in the log, new: the passes a frame by target size; the presented picture's size is in the
`screenshot: present` lines):

| game | handheld (`docked=0`, what every ini had) | docked (`docked=1`) |
|---|---|---|
| Donkey Kong Bananza | scene and picture 1920x1080 | scene and picture 2880x1620 (the scene at 2095x1179 when the game's dynamic resolution steps down) |
| Mario Kart World | 1920x1080 (half-size passes 960x540) | 2560x1440 (1280x720) |
| Pokemon Legends Z-A | scene 1920x1080, picture 1280x720 | scene and picture 1920x1080 |
| Ocarina of Time (title, menu) | 1920x1080 | 2560x1440 |
| Mario Kart 8 Deluxe | 1280x720 | 1920x1080 |

Cause: `docked` was 0 in every ini. The runtime writes every option with its default into the ini at each start, the default was handheld, and
the operation mode, the performance mode and the default display resolution (`svc_core.c` `am_csg` commands 5, 6, 60, 62) answer from it. So
each game drew for the console's panel and the window stretched that (bilinear, `vk_present.c`) to the monitor. The other suspects were clean:
the graphics scale already answered tier 0 (nxnative's finding), `game_dynamic_resolution=0` told the games the GPU takes no time (largest size,
never a step down), and the swapchain is the window's size.

Fix: TV mode is the default, and an ini written before (`settings_version` below 2) is moved to it once; `docked=0` set afterwards stays. The
docked sizes are the ones nxnative measures (DKB 2880x1620 with main passes 2095x1179, Ocarina 2560x1440). Crops of the same scene, handheld
left, docked right: `build/evidence/gaps/plza-investigation/{dkb,mkw,mk8d,plza}-crop-handheld-vs-docked.png`.

The launcher gets a "Screen mode" choice in Properties > Runtime (TV mode / Handheld mode) that writes `docked` into the game's ini.

### Counter reports with the GPU's own time (gap rank 2)

`vk_replay.c` "Counter reports: the GPU's own time", nxnative's design on Vulkan timestamp queries: each `TIMESTAMP` / `TIMESTAMP_TOP` report
is stamped where it was recorded (`vkCmdWriteTimestamp2`, top of the pipeline for `TIMESTAMP_TOP`), the GPU's clock is brought to the guest
clock by the smallest difference seen between a part's end and the guest clock at its completion, the time the GPU waited between the parts
of one NVN submission is left out, and a Switch 2 SDK's report holds nanoseconds (`dm_sdk_ounce`; `nvnDeviceGetTimestampInNanoseconds` is then
the identity). `game_dynamic_resolution=auto` is the new default: on with the Vulkan renderer, off with OpenGL (as before), and an old ini is
moved to it once.

Which idle time is left out matters. nxnative leaves out the gaps inside one NVN submission. Donkey Kong Bananza's frame is 8 submissions on
two queues, each translated after the one before went to the GPU, and with that rule the game read its whole frame as GPU time: it drew its
scene at 2095x1179, its smallest step, and upscaled to 2880x1620, on a GPU two thirds idle, and ran slower for it (55.6 FPS,
`-231722-dkb-final2`; 55.9, `-222528-dkb-docked-drs2`). So the idle time between all the parts of one frame, present to present, is left out
(`rep_gaps`, 2 by default; 1 is nxnative's rule): the scene is at 2880x1620 and the run at 57.0 FPS (`-233233-dkb-final3`). Mario Kart World
stays at 2560x1440 either way (GPU 3.6 to 4.0 ms a frame in 4 submissions).

### Donkey Kong Bananza: write-back on demand (gap rank 1)

`vk_mem.c` "Write-back on demand", `nvn_dirty.c` `PG_LAZY`. A page of pool memory that GPU writes keep coming to (48 of them), with no copy
on its way, not hot and not written by the CPU since its last upload, is handed to the device: the dirty tracker makes it inaccessible, writes
to it are no longer copied back, and nothing reads it from guest memory. The first access by the CPU, by the game or by the host (a fault, or
a host write announced through `dm_guest_write_prepare`), reads the page back from the device after every submission that skipped a copy into
it has reached the queue, and goes on. Such a page then stays eager for 600 frames, doubled each time it happens again. A range written partly
into lazy pages is copied back in its other parts only; a pool laid over lazy pages, or deleted, brings them in first.

In the mining tunnel 3,900 to 5,300 pages are the device's and 7.8 MB a frame are no longer copied back (and no longer sent to the device again);
the CPU comes for about 15 pages a second (chunks the game rebuilds). In a Mario Kart World race it is 25 MB a frame (its compute buffers), 5,400
pages, under one page a frame read back.

Same scripted run as section 6 (title, Continue, walk, punch, walk), window 100-150 s:

| | section 6 (`-203354-dkb-final`, handheld) | write-back on demand, handheld (`-213418-dkb-lazy2`, `-214125-dkb-handheld`) | docked 2880x1620, scene 2880x1620 (`-220905-dkb-docked2`) | docked 2880x1620, the game's dynamic resolution on (`-233233-dkb-final3`, the default now) |
|---|---|---|---|---|
| FPS mean | 48.7 | 57.2, 57.3 | 54.9 | 57.0 |
| present interval p50 / p95 / p99 | 20.0 / 26.3 / 28.6 ms | 16.7 / 21.6 / 24.8 and 16.7 / 21.3 / 24.6 ms | 16.8 / 23.0 / 32.4 ms | 16.7 / 21.2 / 26.5 ms |
| intervals over 20 ms / over 33 ms (of about 2,700) | 1,240 / 8 | 233 / 7 and 223 / 7 | 313 / 16 | 218 / 9 |
| black cells of 64 in the screenshots | 0 | 0 | 0 | 0 |

Not a flat 60: the translation thread still takes most of the frame for the 630 culling draws (rank 4 of the gap list: one dispatch for them).

### Other gap items

- **Maps past the application pool fail** (rank 5; `kernel.c` svc 0x2C): Pokemon Legends Z-A's log has the refusal nxnative describes, once, at
  1.5 s: `MapPhysicalMemory +4 MB refused: 3174 MB mapped, the application pool (3285 MB) is full`, then the game goes on as before.
- **Queue parking on Linux**: the three fiber calls on `ucontext` (`host/linux/windows.h`); the Linux NVN archive and runtime build in WSL. Not
  run on a Linux GPU here.
- **A newer translator chain** (rank 4): not done. It needs nxnative's translator built with the .NET 10 SDK (this machine has 6, 7 and 9), the
  Python front end moved to its ABI, and every game's shader cache made again.

### Mario Kart World: the long frames

Unchanged, and now attributed frame by frame (`frames.csv`'s cumulative columns over the long presents, window 150-185 s of
`-222919-mkw-docked-drs3`): one cluster of seven frames of 54 to 145 ms at 161.5 s, each with 28 to 115 ms of texture upload, 40 to 100 ms of
ASTC transcoding and up to 32 ms of sparse binds; one frame of 98 ms of texture upload alone; and one of 689 ms that is a single pipeline
compiled at first use (the first docked run: the pipeline cache had no 2560x1440 variants). The handheld run has the same cluster at the same
place in the race (`-215238-mkw-handheld`: 60, 124, 147, 162, 182 ms). So it is first-use texture work, in a burst when a new part of the course
streams in. Not fixed: the texture path uploads a texture whole in the frame that first uses it, and the existing prefetch
(`NXRT_VK_TEX_PREFETCH`) had shown no gain. The next step is in the gap list (rank 3): small levels now, large ones over the next frames, sampled
through `VK_EXT_image_view_min_lod`.

| | section 6 (`-202635-mkw-nx`, handheld) | handheld, this round (`-215238-mkw-handheld`) | docked 2560x1440 (`-223716-mkw-docked3`, `game_dynamic_resolution=0`) | docked 2560x1440, the game's dynamic resolution on (`-233605-mkw-final3`, the default now) |
|---|---|---|---|---|
| FPS mean | 58.0 | 58.1 | 58.1 | 58.2 |
| present interval p50 / p95 / p99 | 16.67 / 17.66 / 25.98 ms | 16.84 / 18.51 / 32.67 ms | 16.67 / 17.15 / 28.71 ms | 16.67 / 17.52 / 31.75 ms |
| intervals over 20 / 33 / 50 ms | 33 / 14 / 9 | 55 / 15 / 8 | 26 / 10 / 8 | 44 / 11 / 6 |

### Mario Kart 8 Deluxe

- **A crash on the first screen**: with the save as the owner has it (no Mii picked yet), L+R opens "Select a Mii" with the cursor on "Create a
  Mii"; A starts the MiiEdit applet, whose output here was all zeros, read as "a Mii was made, at index 0" of a database that holds none: a
  null Mii and a crash in `nn::mii::CreateId::IsValid` (`access to 48b4`). The applet now answers "cancelled" (`svc_core.c`); the scenario
  picks the first default Mii.
- **The first race on a new course** (1-2 FPS for about 4 minutes on a cold shader cache): the translator is not what takes the time. Its own
  log has 48 shaders in 1.6 s; the course's 423 took about 15 s of translator time in all. The frames were slow because the game's thread sat in
  two GPU waits of about 490 ms each a frame with the GPU idle (`GPU waits, last 5 s: 4925 ms in 10 waits`, GPU time 0.7 ms a frame), and
  shaders are only asked for as frames go by. This round's cold run (`NXRT_SHADER_CACHE` pointed at an empty folder, so the owner's cache was not touched; `-225901-mk8d-coldcache`) did not have those waits: 60 FPS, 700 draws a frame, 654 shaders delivered within the first minute, and the race 95 % black to its end. The game draws parts of a course's lighting once while the course loads, and those draws had been skipped. Fix (`vk_replay.c` `shader_wait`): a draw whose shaders the live translator has not delivered waits for them, once per set of programs, at most 3 s and at most 1.5 s a frame (`NXRT_VK_SHADER_WAIT_MS`; 0: skipped at once, as before). With it the cold run reaches a fully drawn race (`-233914-mk8d-coldcache4/0017`, 2.5 % near-black); its 407 waits took 160 s in all, 0.39 s each, from the first boot to the race, once per shader ever. That per-wait time is the translator's (a process start per batch): the next step there. A new shader met in play is now a short stop instead of a missing object (Mario Kart World: one of 415 ms in a race, `-232054-mkw-final2`).

### Regression checks of this round

All five library builds were rebuilt through the engine CLI into their `buildDir` with the final runtime and run with the harness, with the
defaults (TV mode, the game's dynamic resolution). The saves were copied aside first (`build/evidence/gaps/save-backups-20261117c`) and put back
after the runs (Mario Kart 8 Deluxe's had a Mii picked, DKB's and MKW's had moved).

| game | run | result |
|---|---|---|
| Donkey Kong Bananza | `-233233-dkb-final3` | title, Continue, walk, punch, walk at 2880x1620: terrain shaded in every screenshot, 0 black cells of 64; 57.0 FPS, present interval p50 16.7 / p95 21.2 / p99 26.5 ms (48.7 FPS and 20.0 / 26.3 / 28.6 ms at 1920x1080 before) |
| Mario Kart World | `-233605-mkw-final3` | to a race, two drives at 2560x1440: 0 black cells; 58.2 FPS, p50 16.67 / p95 17.52 / p99 31.75 ms, 6 intervals over 50 ms (the texture bursts) |
| Pokemon Legends Z-A | `-232801-plza-final2` | title, the station, two walks, picture 1920x1080 (1280x720 before): 0 black cells; 30.0 FPS (the game's interval 2), p50 33.3 / p95 34.0 / p99 34.5 ms, longest 42.6 ms |
| Ocarina of Time | `-232513-zelda-final2` | title and menu at 2560x1440, 28.6 FPS (p50 34.7 ms); dav1d decodes the movies (598 pictures, 1.36 ms a unit), the screen behind the logo is still black (`AV1_DECODER.md`: the `nn::ndve` network) |
| Mario Kart 8 Deluxe | `-232945-mk8d-final2`, `-225659-mk8d-final` | Select a Mii, the menus, a 50cc Grand Prix race at Mario Kart Stadium at 1920x1080 (1280x720 before): 0 black cells; 59.8 FPS, p50 16.66 / p95 17.03 / p99 17.46 ms (`-225659`). In `-232945` five new shader sets of the docked race came in one frame: a stop of 2.3 s, once (a frame's waits end after 1.5 s since) |

Tests: `python -m unittest discover -s tests -p "test_n*.py"` (17), `tests/test_controllers.py`, `tests/test_mkw_runtime.py`,
`tests/test_mkw_perf.py` pass. Linux: the NVN archive and the runtime build in WSL.

Run folders of this round: `build/evidence/gaps/plza-investigation/` of the main checkout (not in git).

### Seen and not followed up

- **Donkey Kong Bananza's tunnel is posterized**: flat patches with hard edges where the lighting should fall off smoothly (any gameplay
  screenshot of this round and of section 6, e.g. `-213418-dkb-lazy2/0006`; the large cave before the tunnel is smooth). Not from this round's
  changes. The log has one `[RED] a 3D texture read as another type ... it reads 0` (texture 1x1x1, shader `4419f04c58bf1535` binding 3) as
  the tunnel loads; a colour or light table read as zero is the first suspect.
- **Ocarina of Time**: title and menu at 2560x1440, 27 FPS (GPU time 25 ms a frame in 7 submissions at the title: the movie's conversion
  dispatches). Gameplay not reached in the scripted run (the opening movie is AV1).
- **Pokemon Legends Z-A's eight `[RED]` cube placeholder lines** are unchanged.

### Switches added

| switch | what |
|---|---|
| `docked=1` / `0` (ini, or `--docked=0` for one run) | TV mode (default now) or handheld mode |
| `game_dynamic_resolution=auto` / `1` / `0` | counter reports with the GPU's own time (auto: with the Vulkan renderer), or the old reports |
| `NXRT_VK_WB_LAZY=0` | no write-back on demand: every GPU write is copied back, as before |
| `gamectl set rep_gaps 1` (`NXRT_VK_REP_GAPS=1`) | counter reports leave out the GPU's idle time inside one NVN submission only (nxnative's rule), not across the frame |
| `NXRT_VK_SHADER_WAIT_MS=<ms>` (3000; 0 = never) | how long a draw waits for shaders the live translator has not delivered |
| `SWR_MAP_LIMIT=1` / `0` | maps past the application pool fail, for any title |
| metrics columns `wb_lazy_pages`, `wb_lazy_in`, `wb_lazy_skip_kb` | pages the device alone holds; pages read back for the CPU and KB not copied back, per second |
| log lines `render sizes up to frame`, `write-back on demand up to frame`, `a wait for fence ... ran out` | the sizes a game draws at; the lazy pages; a GPU wait of 100 ms or more that ran out |
| `tools/play_scenario.py <game> --game <buildDir> [--option k=v]`, `tools/plza_probe.py start --option k=v` | the scripted runs; an ini option for one run |

## 8. Fourth round (2026-11-18): Ocarina of Time as the owner starts it, window mode

Owner's report after release 0.4.0: Ocarina of Time's screens are still black and no video shows, there is no sound, it does not get past
the intro title; and the window mode chosen with F11 is forgotten at the next start. The rounds before had called the same state "title
screen and menu reached". This round started the game the way RecompLauncher's Play does (`tools/plza_probe.py start --as-launcher`: the exe
alone in its folder, the ini as it is, the launcher's environment; `window.cpp` `play()`), and read the owner's own log of 2026-11-18 00:09
(kept in `build/evidence/oot/owner-run`). The harness and the launcher gave the same picture: there was no difference between the two paths.
What the owner saw was what the harness had shown all along: a black movie behind the logo, then, after A, the opening movie: 3,480 packets
in 119 s (half speed), black, silent, until the window was closed.

| | before | now |
|---|---|---|
| title and opening movies | black (the logo and menu over black) | on screen: near-black 0.1 % at the title, 0 black cells of 64 (`-004218-oot-host1`) |
| frame rate at the title / in the movies | 27 to 28 FPS, so the 60-picture movies ran at half speed | 60.0 FPS; the opening's 9,254 pictures in 155 s (154.2 s is its length) |
| sound | output RMS 0 from the title on, except the menu's own sounds | title music and the opening's sound: RMS 0.01 to 0.17 (-37 to -15 dBFS), peak up to 0.99 |
| after the opening | not reached | **the first scene of the game itself is black**, at 1 to 5 frames a second, and stops after about 80 frames: open, below |
| F11 / Alt+Enter | forgotten at the next start | written to the ini; the next start opens the same way |

### The movies were black: push constants read as zeros

`gamectl passdump` and `texdump` on a title frame (through the harness: see "The control pipe" below) show the whole chain: two buffer
copies into the R8 1920x1080 and RG8 960x540 plane textures, the colour conversion `49dff010` into an RGBA8 picture, eleven convolutions of
`nn::ndve`'s model on tensors of 960x540x16 half floats, `5a7d69b0` (the last convolution added to the picture, into an RGB10A2 picture),
the resampler `4f41ccf2` into the RGBA8_SRGB 2560x1440 texture the title draws. `AV1_DECODER.md` had found the planes right and everything
after them zero, and had put that on the convolutions' raw global memory. The first zero is earlier: the colour conversion itself, a shader
with two textures, one image and no raw memory, wrote 0, 0, 0, 1. Its reflection has a `drvcb` binding of bank 24. Bank 24 is the NVN2 push
constants (`DMSH_PUSH_BANK`; the game pushes the conversion's matrix with `nvnCommandBufferPushComputeConstants`), which the replay bound
only when the reflection named them as a constant buffer in slot 21; as a driver bank they got the zero buffer. nxnative reads both forms
as the push block (`nvn_refl.m`, `NVN_RB_PUSH`). With the pushed bytes bound (`vk_replay.c` `write_set_`) and image stores into textures
made without the IMAGE flag on (`NXRT_VK_IMAGE_NOFLAG`, now the default), the picture is right up to the RGB10A2 texture
(`-003642-oot-push1/ser6231-small.png`: Hyrule Field under the moon).

### The movie network by the host

Two things were still wrong behind that. The resampler's output was the picture with dashes of white and black all over it
(`-003642-oot-push1/shot-small.png`): `4f41ccf2` is 128 threads a group with warp shuffles over 4,608 bytes of shared memory, which the old
translator build does not get right (nxnative's notes list six fixes in that area since). And the title ran at 27 FPS: the convolutions
write their tensors through raw global memory, 52 MB a frame copied back to guest memory and 89 MB a second uploaded again, with the game's
thread 30 to 50 ms a frame in GPU waits; the player takes one picture a frame, so the movies ran at half their speed and behind their sound.

`vk_replay.c` `ndve_host` takes the model's place, recognised by its shader keys as nxnative does (shaders.md "The NDVE movie network by the
host", MIT, (c) seedds): the colour conversion stays the game's, the convolutions are skipped, `5a7d69b0` becomes a copy of the picture and
`4f41ccf2` a linear blit to 2560x1440. What is lost is the network's correction, a learned sharpening of the decoded picture, not the picture.
GPU time at the title went from 24 ms to 0.2 ms a frame, the title and both movies run at 60.0 FPS. `NXRT_VK_NDVE=0` (`gamectl set ndve 0`)
runs the game's dispatches. nxnative runs the real network as host kernels; porting those (13 kernels with 8x8 matrix products) is the
next step for the picture's quality, a newer translator the other.

### No sound: the repository had no libopus

The title's music and the opening's sound are the movies' own audio: Opus streams the game decodes through the `hwopus` service into PCM
voices. `runtime/opus` held headers and a license, no archive (`.gitignore` kept `*.a` out except libvpx and dav1d), so
`switchrecomp.py` built every runtime with `SWR_NO_OPUS`, and the service answered each packet with silence of the right length. No log
line said so. Now: `runtime/opus/build_opus.py` builds Opus 1.5.2 from xiph.org's archive (SHA-256 checked, plain C, reproducible: two
builds gave the same bytes on Windows and on Ubuntu), `win/libopus.a` and `lin/libopus.a` are in the repository with their hashes in
`runtime/opus/README.md`, `tests/test_opus.py` checks the hash and decodes a tone, engine CI and the launcher's packaging check the
runtime for the decoder's log line, and a runtime without libopus says so once.

The mixed output is measured in the log (`audio: output, last 5 s: RMS ..., peak ...`, `audio.c` `swr_audio_submit_at`), which is how
"is there sound" is answered here without listening:

| run | title (10 to 50 s) | opening movie | first scene |
|---|---|---|---|
| the owner's build (`-002104-oot-baseline-launcher`, `-004218-oot-host1`) | RMS 0 | RMS 0, but for 5 s of the menu's confirm sound (0.052) | - |
| libopus, voices straight to stereo (`-010105-oot-play1`) | - | 0.009 to 0.22, peak 1.24 (clipping) | - |
| libopus and the mixes (`-010727-oot-stall3`, `-013723-oot-queue1`) | 0.007 / 0.025 to 0.036 / 0.046 (left / right) | 0.025 to 0.17, peak 0.99 | 0.026 to 0.028 |

### Mixes and splitters (every Switch 2 game)

The log's `audren mixer active (6 voices, 0 mixes), direct stereo fallback` was true of Donkey Kong Bananza and Mario Kart World as well:
since REV7 an update carries only the mixes that changed, behind a 0x20-byte header, and `audio.c` took any mix section that was not a whole
number of 0x930-byte mixes for "no mixes". Every voice then went to both stereo channels at its own volume: no mix volumes, no 5.1 downmix,
no effects. `swr_audio_update_routing` keeps the mixes by ID between updates and parses the splitter section (it sits between the effects
and the mixes; the mixes used to be read right behind the effects): `SNDI` records with their destination IDs, `SNDD` destinations of 0x70
bytes or, on this SDK, 0xa8 (the size is learned from the distance to the next record; the in-use flag is 12 bytes from the end). A voice
goes to its mix through its channel volumes or through its splitter's destinations, a mix to its mix or through a splitter. Ocarina of Time:
11 mixes, a 6-channel final mix, 384 destinations. One thing is not understood: the opening's sound plays on voices that name no mix and
whose splitters (13 to 17) are sent with 0 destinations. A console is not silent there. Such a voice is played in the final mix's front
pair and logged (`audio: voice N names no mix ...`), so nothing that was audible before is lost; where its real route is, is open.
`tests/test_audio.c` has a case for changed-mixes-only updates and splitter destinations.

### A stop minutes into the movie: the present ticket

With the frames cheap the game stopped a few minutes in, nothing waiting on the GPU, the translation, present and completion threads all
idle and the game's thread in `nvnWindowAcquireTexture` -> `vkp_wait` (native stacks, `-004902-oot-stall2/stall-1-stacks.txt`).
`nvnQueuePresentTexture` stored the queued present's marker in the window's ticket after pushing the present; the translation thread could
have translated it and stored the real ticket first, and the marker then overwrote it: the next acquire of that texture waited for a
present numbered by the marker, for ever. The marker is now written under the queue's mutex, before the translation thread can take the
present (`hle_nvn.c` `aq_push`). The race is any game's; it needed a translation thread with nothing else to do.

### The control pipe

`gamectl` commands went to the pipe `\\.\pipe\swr_ctl`, which belongs to whichever game started first. The DLC work's Mario Kart 8 Deluxe
was running, so this round's first `passdump` and `texdump` were answered ("in the log") by that game. The pipe is now
`\\.\pipe\swr_ctl_<process ID>` and the harness names its own game's process.

### Window mode

F11 and Alt+Enter only changed the window (`backend_win32.c`, `backend_x11.c` `set_fullscreen`); nothing wrote `fullscreen=` back, so the
ini kept its written-out default `fullscreen=1`. The launcher was not involved: Play writes `renderer` and `button_layout` and keeps every
other line (`library.cpp` `writeIniKey`), and the `settings_version` step only touches `docked` and `game_dynamic_resolution`. Now the two
keys go through `toggle_fullscreen`, which stores the choice (`ini_set`); the ini is written within a second, and at exit when the game is
closed before that (`main.c`). The controls window and the start still use `set_fullscreen` and store nothing.

Test, started the launcher's way each time (`build/evidence/oot/window_mode_test.py`, `wm_probe.py`; F11 as a message posted to the game's
own window): `fullscreen=1`, the window opens without a caption at the screen's size; F11: caption, `fullscreen=0` in the ini 10 s later
and after the exit; next start: `fullscreen=0`, style `0x14cf0000` (caption), 1296x740; the ini still `fullscreen=0` after it. The other
way round (0, F11, 1, next start fullscreen) as well. Before this test ran, the ini already held `fullscreen=0`: the owner had pressed F11
in this round's build. A launcher test keeps a `fullscreen=0` in the ini through Play's writes (`tests.cpp`). Seen in the test: the game's
process lives a few seconds after its window is gone, and a start in that time is refused by the harness.

### Open: the game itself

After the opening the game goes into its first scene (the Great Deku Tree's hollow). On a first visit the first frame took 106 s, 73 s of
it pipelines compiled at first use (the second visit: 11.6 s, 0.3 s of pipelines); 330 shaders were translated live in the minute before,
two failed (`Ampere instruction 243 ... is not implemented`). Then:

- **The picture is black** (`-011734-oot-play2/0006`, `-013723-oot-queue1/0004`), with the scene's sound playing.
- **1 to 5 frames a second.** The translation thread is idle 99.3 % of the time there (`SWR_HOST_PROF`, `prof1-hostprof.txt`: 0.47 s of CPU
  in 25 s, the rest in `aq_main`'s wait), the game's thread in `nvnSyncWait` -> `aq_wait_seq` (`-010727-oot-stall3/stall-1-stacks.txt`).
  Read: a submission of one queue is parked at an event wait and done ops are published in order, so the thread waits for the parked one
  (250 ms, its limit) instead of its own queue's work. A per-queue fence (`nvnQueueFenceSync` holding its queue's last op) was tried and
  did not change the rate (76 frames in 90 s, `-013723-oot-queue1`); it is not in the tree. nxnative's rule is in nvn-objects.md "GPU event
  waits order work across queues": an unmet wait parks its queue only, with no time limit, and CPU signals publish.
- **It stops** after about 80 frames of the scene (`-013723-oot-queue1`: frame 11627 at 314 s, nothing until the run's end at 445 s).
- The log there: `kind tex_pool is not replayed yet (bound as empty)`, `render target ... cannot be a colour attachment here (its content
  comes from its storage)`, 90 MB of write-back a frame, mesh draws (`task shader a5288d0a6c4971fd`).

So the owner's third point stands in part: the game now runs through the title and the whole opening with picture and sound, and what
comes after is not playable. That is the renderer work `NXNATIVE_GAP_ANALYSIS.md` ranks first now.

### Hand-off (2026-11-18): Ocarina of Time is NOT playable; a new investigation is needed

**State in one line: nothing after the opening movie is verified, because nothing after it works.** This round fixed what stood between
the boot and the end of the opening (picture, speed, sound) and the window mode. The owner's third point, "it does not get into the
game", is unresolved: after the opening the screen is black, the game runs at 1 to 5 frames a second and stops. No frame of gameplay
(a new file, Link's house, Kokiri Forest) has been seen on this engine. The work is not done until gameplay is on screen, controlled
through the harness, and at the frame rate goal (60 FPS in play; nxnative holds 59 to 60 there). Do not report this game as working from
the title or the movies again: that is what went wrong in the rounds before.

Done and verified (launched the launcher's way, fullscreen, `-004218-oot-host1`; sound by the log's output level):

- title movie and opening movie on screen, 60.0 FPS, the opening's 9,254 pictures in 155 s;
- title music and the opening's sound (RMS 0.01 to 0.17, no clipping with the mixes);
- F11 / Alt+Enter kept across starts, both ways;
- the stop minutes into the movie (present ticket race);
- DKB, MKW, PLZA not worse (table below); DKB and MKW now mix through their mixes.

Not done, in the order to take it:

1. **The first scene is black.** Nothing is known about why yet: no pass dump of a scene frame exists (the two tries, `-012538-oot-prof1`,
   gave nothing: `ctl passdump` / `texdump` were sent while frames took seconds and the run ended first). First step: a run with
   `NXRT_VK_PASSDUMP=<present>` / `NXRT_VK_TEXDUMP=<present>` set from the start for a present number inside the scene (the scene starts
   about 11,470 presents in, when A is pressed 50 s after the start; the log's `[nvn] N presents` lines give the count), then read which
   pass first holds nothing. Suspects from the log: `kind tex_pool is not replayed yet (bound as empty)` and `samp_pool` on the scene's
   shaders (bindless texture pools: every material texture), `render target ser=... cannot be a colour attachment here (its content comes
   from its storage)`, the two shaders the translator refuses (`Ampere instruction 243`), raw global memory (`gmem_map` on most of the
   scene's shaders; nxnative: "foliage: 2,241 shaders with raw loads").
2. **1 to 5 frames a second with an idle translation thread.** The game's thread sits in `nvnSyncWait` -> `aq_wait_seq`
   (`-010727-oot-stall3/stall-1-stacks.txt`), the translation thread in `aq_main`'s 1 ms wait with a submission parked at an event wait
   (`prof1-hostprof.txt`). The per-queue fence was tried alone and did not help; the next thing to try is nxnative's whole rule (an unmet
   wait parks its queue with no time limit, other queues go on, CPU signals publish) and to log, per parked wait, the event, who signals
   it and how long it waited. `NXRT_AQ_PARK=0` and `NXRT_VK_WAIT_EVENT=0` are the two switches to compare with first: one run each.
3. **The stop** about 80 frames into the scene (`-013723-oot-queue1`, frame 11627): take `--stacks` there.
4. The first visit's 106 s frame (73 s of pipelines at first use): the pipeline manifest only helps the second visit.
5. The movie network's real kernels (picture quality only).
6. Where the opening's voices really go (splitters 13 to 17 with no destination).
7. Mario Kart 8 Deluxe was not rerun in this round.

Quick wins for the next agent:

- One run to the scene costs 4.5 minutes (50 s to the title, A, 155 s of movie that cannot be skipped). A save made on nxnative or by a
  finished run would skip it; there is none yet (`_data/save/user` holds only `account.sav`). Until then: one run, several questions
  (`--env NXRT_VK_PASSDUMP=... --env NXRT_VK_TEXDUMP=... --stacks --metrics` together).
- The harness calls a first-visit pipeline frame a stall and its queued actions time out; give scene runs `--seconds 800` or more and
  read `slow frame` lines before believing a stall.
- `frames.csv`'s columns are cumulative: subtract the row before. In the scene the per-frame deltas were 90 MB of write-back and 1.5 s
  of `cpu_ns` against a 0.8 s frame.
- Two games at once share `P:` and the GPU; the control pipe is per process now, the frame rates are not comparable while another
  agent's game runs.

### Regression checks of this round

The library builds of Donkey Kong Bananza, Mario Kart World and Pokemon Legends Z-A were rebuilt through the engine CLI into their `buildDir`
with the final runtime and run with `tools/play_scenario.py` (the scenarios of section 7). Their saves were copied aside first
(`build/evidence/oot/save-backups`) and put back where a run had moved them (DKB, MKW). Another agent's Mario Kart 8 Deluxe was running at
the same time on the same GPU. **Mario Kart 8 Deluxe was not rerun**: its library build was in use by the DLC work, with that branch's runtime.

| game | run | result | sound (new: measured) |
|---|---|---|---|
| Donkey Kong Bananza | `-015750-dkb-oot` | title, Continue, walk, punch, walk: 0 or 1 black cells of 64 in each of 7 screenshots; 55.2 FPS over 100 to 150 s, present interval p50 16.7 / p95 21.3 / p99 26.0 ms, one stop of 1.4 s (57.0 FPS, 16.7 / 21.2 / 26.5 ms in section 7) | mix routing now; 27 of 28 five-second windows with sound, RMS up to 0.046 |
| Mario Kart World | `-020026-mkw-oot` | to a race, two drives: 0 black cells in 12 screenshots; 58.4 FPS over 150 to 185 s, p50 16.67 / p95 17.36 / p99 28.27 ms, 7 intervals over 50 ms (58.2 FPS, 16.67 / 17.52 / 31.75 ms, 6) | mix routing now; 36 of 36 windows with sound, RMS up to 0.042 |
| Pokemon Legends Z-A | `-020336-plza-oot` | title, the station, two walks: 0 black cells after the boot screenshot; 30.0 FPS, p50 33.3 / p95 34.0 / p99 34.6 ms, longest 42.5 ms (as section 7) | AudioOut, unchanged; 15 of 15 windows with sound |
| Ocarina of Time | `-004218-oot-host1` (the launcher's way, fullscreen), `-013723-oot-queue1` | title and both movies on screen at 60 FPS with sound; then the first scene: black (above) | above |

Not listened to: the levels say there is sound and that it does not clip (peaks under 1 with the mixes), not that the mix is the console's.

Tests: `python -m unittest discover -s tests -p "test_n*.py"`, `tests/test_controllers.py`, `tests/test_mkw_runtime.py`,
`tests/test_mkw_perf.py`, `tests/test_av1.py`, `tests/test_opus.py` (new), `tests/run_audio.sh` (WSL): pass on Windows.

Run folders and the saves copied aside before the runs: `build/evidence/oot/` of the main checkout (not in git).

### Switches added

| switch | what |
|---|---|
| `NXRT_VK_NDVE=0` (`gamectl set ndve 0`) | the game's own movie network dispatches instead of the host's copy and blit |
| `NXRT_VK_IMAGE_NOFLAG=0` | image stores into textures made without the IMAGE flag are lost again (the default before) |
| `tools/plza_probe.py start --as-launcher` | the game started as RecompLauncher's Play starts it |
| log lines `audio: output, last 5 s`, `audren: update sections`, `audio: mix section`, `audio: splitter destinations of`, `audio: voice N names no mix` | the mixed output's level; what an update holds; the routing |

## 9. Fifth round (2026-11-18): Ocarina of Time's first scene, Pokemon Legends Z-A at 60

State at the end of this round, in one paragraph: **Ocarina of Time is still not playable, and it takes tens of GB of memory (below; capped
at 32 GB now).** Its first scene after the opening (the Great
Deku Tree, the fairy) is on screen now, recognisable, at 27 to 50 FPS, where it was black at 1 to 5 FPS and stopped with the device lost.
About 200 s into that cutscene the game slows to frames of 5 to 24 s and crashes near 630 s with its heap full; controllable gameplay was
not reached. Pokemon Legends Z-A runs at 60 FPS. The pacing line is time based. Donkey Kong Bananza, Mario Kart World, Mario Kart 8 Deluxe,
Pokemon Violet and Pokemon Legends Arceus were not worked on beyond regression runs and one look at Violet's crash; their open points stand.

### Ocarina of Time: what the black scene was

Each of these is a general fix in `runtime/nvn`.

1. **The 1 to 5 FPS.** The game's thread sat in `nvnSyncWait` behind another queue's parked submission. A frame of this game is about 20
   NVN events that order work across its queues; our queue published finished ops in submission order, resumed a parked submission unmet
   at the next present, and gave a wait up after 250 ms. Now an unmet GPU event wait parks its NVN queue and only that queue, a fence is its
   own queue's last op, 32 fibers hold parked submissions, and a CPU `nvnEventSignal` wakes the translation thread (`hle_nvn.c` `aq_pick`,
   `aq_fence_of`, `aq_wait_fenced`; nxnative `docs/reference/nvn-objects.md`, "GPU event waits order work across queues").
   `NXRT_AQ_FORCE=1` is the old rule; the wait limit `NXRT_VK_WAIT_EVENT_MS` is 2000 by default. The log says how long submissions park.
2. **The stop near frame 11627** was `VK_ERROR_DEVICE_LOST`. Fifty of the scene's shaders index the game's whole texture and sampler pools
   (arrays of 1,048,576). Those arrays were allocated per draw and never written, and a staging list that overflowed leaked a 32 MB buffer
   per request (24 GB of device memory in 90 s). The pools are one descriptor set now (set 5, update-after-bind, written as the game
   registers textures: `vk_shader.c` `vksh_pool_dsl`, `vk_replay.c` `pool_set`; `NXRT_VK_POOLS=0` off), and a submission's staging
   allocations are one list entry (`vk_mem.c`).
3. `nvnCommandBufferDrawMeshTasksIndirect` was a stub: the terrain and the foliage (`vk_replay.c` `mesh_ind`).
4. The frame's last compute passes write an RGB9E5 texture, which cannot be a storage image: an R11G11B10F image stands in (`vk_tex.c`).
5. agl's NDSR networks (anti-aliasing and upscaling, 37 compute kernels of the `vr` packs) gave coloured noise and cost 29 ms of GPU time a
   frame: they are skipped and the frame is copied, as `nn::ndve`'s movie network is (`ndsr_host`; `NXRT_VK_NDSR=0` runs the game's
   kernels). With the newer translator (below) the game's own kernels may work; not tried.
6. **The shader translator** in `tools/nxshader/xlate_bin` is nxnative's of 2026-11-07 (commit 1a410e6e5; `BUILD.txt` there says how it
   was built and the one change kept on top): float atomic adds, `ATOMG.CAS`, the 8192-entry memory pool table. The cache key changes with
   the translator, so **every game translates its shaders again on its first run after this update**: the live translator takes 48
   shaders in 4 to 5 s and a draw waits up to 3 s for its shaders, so the first minutes of that first run show frames of seconds (Mario
   Kart World: 3224 shaders, about 5 minutes). The second run is as before.
7. Shaders that load through raw pointers (agl's probe analysis) read pools no draw binds, whose pages were never sent to the device:
   while such shaders draw, every page the game wrote goes up once a frame (`gfx_vk.c`, `NXRT_VK_SYNC=use` off, `=all` always).
8. A block-compressed texture as a render target (a 64x64x64 BC4 volume the game fills on the GPU through an RGBA16UI view) is drawn into
   through a stand-in whose texel is a block, copied in at the pass's end (`vk_replay.c` `cz_get`; `NXRT_VK_CZ=0` off). The shader that
   writes it got no integer-output variant because its module declares the integer types after the output; the variant moves them now.

### Ocarina of Time: what is open, in the order to take it

- **The slowdown and the crash.** From about 450 s after the start (some 200 s into the first scene, with A pressed every 25 s) frames take
  2 to 24 s, with none of the slow-frame line's categories (sparse binds, GPU waits, uploads, pipelines, pool copies) holding the time,
  and near 630 s the game crashes on a NULL pointer with `heap 6316 MB ... used 8628 MB of 9216 MB`. In the stall dumps of that stretch the main thread is inside the frame callback
  (`blr` at guest 0x842dd88 in the game's frame loop, which times the callback with two tick reads; the dump's 0x842dd90 is its return
  address), and the dump adds `thread 80 looping on SleepThread (919019 times) from lr=806e57c` (`lr=8077838` in another run): the game's
  main thread is polling for something with sleeps, hundreds of thousands of times, while its heap fills. Runs `-162145-g4-r17` (alone on
  the machine; `-053444-g4-r16` shows the same but another game was running), `stall-3-threads.txt` and `stall-4-threads.txt` there.
  Not yet known: what it polls for. Next step: disassemble around 0x806e57c and 0x8077838 (`gamectl peek 0x806e500 64` while it runs;
  `build/evidence/gameplay4/peekdis.py` turns a peek into ARM64) to find the flag or counter the loop reads, then who should set it (a
  file read, a job of one of the game's worker threads, a GPU sync or a texture the game waits to become ready).
- **The picture.** With `NXRT_VK_TO_GPU=0` (the default now) the scene looks right in shape and too dark: the two 256x256 RGBA16F sky
  tables (ser 4144 / 4150, drawn each frame by fragment shader `7f9331c268da06e7` from a CPU-written 256x32x16 table, then summed into
  the ambient light by compute `21d9f83e6027c565`) are read from guest memory, so the draws into them are lost and the ambient is 0.
  With `NXRT_VK_TO_GPU=1` they are drawn (means 1.9 / 1.4 / 1.0, ambient sums 0.41 / 0.53 / 0.81) and the picture is white with a blue
  veil; hiding the full-screen pass of fragment shader `397d2f2824fbf39c` (39 instances, the lit fog) gives a plausible dark scene, so the
  excess comes through that pass's light. nxnative's note for this game names the exposure read back from float atomic sums as the cause
  of "too bright, a pale veil". The sums do reach guest memory here (`gamectl peek 0x22fc1e000 8`: 78.8 / 63.8 / 52.3 / 67.0 in 10 of 14
  samples, 0 in 4, the CPU clears them each frame); whether the game reads them before our copy back lands is the open question
  (`vk_mem.c` keeps the CPU's word when both wrote it: the line `kept for the CPU`).
- Red lines left: `a 3D texture read as another type` (texture 258, a 4x4 dummy), `nvnCommandBufferSetComputeWorkloadPriority` (a hint),
  `attachment-less pass at the viewport's size 1x1`.
- First-visit cost: the first frame of the scene compiles pipelines for 38 to 94 s, the first pool pass writes 730,000 descriptors.
- Not done: the opening voices' mix route, gameplay input, the launcher-style fullscreen check of the scene.

### Ocarina of Time takes the machine's memory (NOT fixed; bounded by a cap now)

Ocarina of Time's process grows by about 1 GB a second from its start: 9.5 GB at 5 s, 61.6 GB at 60 s, then flat while the title and the
movies play, then more in the first scene until Windows had no commit left. That, not the game's heap, is what ended the long runs above, and
on 2026-11-18 it took the whole machine down twice (a low-virtual-memory event naming the game at 227 GB, the display driver resetting, an
unclean restart). Pokemon Legends Z-A holds 3.7 GB.

What is known (`build/evidence/gameplay4`: `mem-*.txt`, `ramp-*.txt`, `vq1.txt`, `vq3.txt`, `cdbva.txt`, `nst1.txt`):

- The memory is the NVIDIA driver's own, not this layer's: `VirtualAlloc` calls of 296, 152, 136 and 72 MB from inside `nvoglv64.dll`
  (`cdbva.txt`: a breakpoint on large `VirtualAlloc` calls), 16 blocks of 296 MB and 17 of 136 MB by the time the process holds 17.7 GB
  (`vq3.txt`). The callers are the pipeline prewarm's `vkp_graphics` -> `vkp_layout` (`vkCreatePipelineLayout`) and `get_l` -> `vksh_make`
  (shader creation). This layer's own `vkAllocateMemory` holds 94 MB of device memory and 741 MB of system memory at that time (the new
  log line `[vk] device memory allocations`).
- It grows while the 680 pipelines of `pipelines.tsv` are made ahead (15 a second) and stops when they are done.
- Ruled out by runs, each with the same curve: the pool set off (`NXRT_VK_POOLS=0`), the old queue rule, no per-frame sync; pool arrays
  of 65,536 and of 1,024 instead of 262,144 (`NXRT_VK_POOL_LOG2`); the declared length of the pool arrays in the shader modules cut from
  1,048,576 to the pool set's (`spv_pool_len`, kept: 0 shaders refused); the pool set without update-after-bind.
- Not known: which property of this game's shaders or layouts makes the driver take 136 to 296 MB per object. Not tried: the same run
  with the pipeline prewarm off, and with the runtime of turbo 51ea340 (before this round), which would say whether the pool set brought it.

What was done about it:

- **A cap on the process's memory** (`vk_dev.c` `vk_process_memory_cap`, Windows): the game puts itself into a job whose committed
  memory is limited to half of the machine's memory, at least 12 and at most 32 GB (`SWR_MEMORY_CAP_GB` sets it, 0 = off). Verified:
  run `-190309-g4-m13`, Ocarina of Time's private bytes went 9.1, 14.6, 19.1, 23.5, 27.0, 29.4, 31.6 GB and then stood at 32,768 MB for
  25 s, after which the game stopped by itself; the machine had 127 GB of free commit at the start, so about 94 GB stayed free. So the game still cannot be played, but it can no
  longer take the machine with it. Linux has no such cap yet.
- A count of this layer's `vkAllocateMemory` per heap in the log (a line each time a heap's live bytes pass another 512 MB), and a cap
  on what it takes from system-memory heaps (8 GB or a quarter of the heap; `NXRT_VK_HOST_MB`), refused allocations logged in red.
- Every game launch of this work now goes through `build/evidence/gameplay4/guard.py` (the lock file `build/evidence/GAME_LOCK`, no other
  game running, 40 GB of free commit) with a watchdog that stops the game by PID past 20 GB of private bytes or under 25 GB of free commit.

### Pokemon Legends Z-A: 60 FPS

The game sets the present interval to 2 itself: `main+0x1B296D8` loads the base frame rate from byte 0x20 of its settings object (an enum,
0 = 60, 1 = 30, 2 = 20, 3 = 15, 4 = 12) and the package's configuration leaves it at 1. `SWR_NVN_INTERVAL_BT=1` prints the guest stack of
`nvnWindowSetPresentInterval`, which is how it was found. A built-in patch in `nxloader.py` (`BUILTIN_PATCHES`, build id
`B1F12FD9...`, `ldrsb w20, [x9, #0x20]` to `mov w20, wzr`) makes it 60. The game's logic is time based: the scenario reaches the same
places at the same wall times as at 30 FPS (`-031349-g4-plza-60test/compare.jpg`). Numbers below. Not looked at: whether animations or
physics tied to the frame count differ anywhere later in the game. A different build id (an update of the game) is not patched and runs at
30 as before.

### The pacing line

`[vk] pacing:` closes its window at 300 intervals or after 5 s, whichever comes first, so a stall shows within 5 s.

### Pokemon Violet's crash (not fixed)

`nn::audio::VoiceInfoManager::AcquireVoiceInfo` at 444 s (`build/evidence/combined-nsp/20261118-033257-violet-boot2`): the manager at
0x5e4fe9000 passes its count checks and its free list's head (+0x28) is NULL, so the walk reads address 8. The list head was zeroed while
its counters were not; `audren_update` writes only inside the output buffer's size. Not reproduced in this round: run `-052858-g4-v1`
stopped at "Choose a look" (the left stick is needed there: `stick-left.txt` with `action replay`); run `-172311-g4-v3` chose a look, pressed A
and showed no new image from 148 s to its end at 580 s (`warning: 15 s without a new image`, GPU submits 0; the stall dumps are in the run
folder), where the run of the combined-NSP work went on to the name and the opening. So two different stops in three runs of this build,
neither understood. The memory line of the crash, `used 3578 MB of 3285 MB`, says the game was over its limit; `memory_gb=6` was not tried.

### Regression checks of this round

The library builds of Donkey Kong Bananza, Mario Kart World, Pokemon Legends Z-A and Ocarina of Time were rebuilt through the engine CLI
into their `buildDir` with the final runtime and run with `tools/play_scenario.py`, one game at a time. Saves were copied aside first and
put back after each run, checked against their hashes (`build/evidence/gameplay4/save-backups`).

**The frame rates of Donkey Kong Bananza and Mario Kart World below are not comparable with section 8.** Another agent was compiling on
the machine during every one of these runs (4 compiler processes, the machine's CPU at 91 to 94 %), and these frames are CPU bound
(Donkey Kong Bananza: about 1 ms of GPU time a frame). Under the same load, Donkey Kong Bananza gave 49.6 FPS with the defaults and 44.6
FPS with the old rules switched back on (`NXRT_AQ_FORCE=1 NXRT_VK_SYNC=use NXRT_VK_POOLS=0`; runs `-171617-g4-ab-def`, `-g4-ab-old`),
so the new scheduler and the pool set are not what lowers it. A measurement on an idle machine is still to be done.

| game | run | result |
|---|---|---|
| Pokemon Legends Z-A | `-170708-g4-reg2-plza` | title, the station, two walks, pictures right: **59.5 FPS** over 45 to 84 s, present interval p50 16.75 / p95 17.73 / p99 29.59 ms, longest 82 ms, 5 intervals over 50 ms (30.0 FPS, 33.3 / 34.0 / 34.6 before) |
| Donkey Kong Bananza | `-171236-g4-reg3-dkb`, `-171617-g4-ab-def` | title, Continue, walk, punch, walk: 0 black cells in every screenshot, pictures right; 42.7 and 49.6 FPS over 100 to 150 s, p50 18.9 / p95 27.9 / p99 35.4 and 18.5 / 29.3 / 36.1 ms, one stop of 6.6 s in the first (55.2 FPS in section 8, see above) |
| Mario Kart World | `-170924-g4-reg3-mkw` | to a race, two drives, 0 black cells: 52.5 FPS over 150 to 185 s, p50 17.9 / p95 22.9 / p99 36.2 ms, longest 300 ms (58.4 FPS in section 8, see above). Its first two runs after the translator change were 0.3 and 16 FPS while 3224 shaders were translated again |
| Ocarina of Time | `-162145-g4-r17` | title and movies as before; the first scene on screen (above); slowdown and crash at 630 s |

Mario Kart 8 Deluxe was not rerun (its save was written by someone else at 12:34 during this round and was left alone). Pokemon Violet
and Pokemon Legends Arceus were run as the combined-NSP work built them (turbo 51ea340's runtime), not rebuilt. **Pokemon Legends Arceus**
(`-173323-g4-a1`, a new save, A through the opening, then three stick walks): free roaming works, the character walks across the first
field, pictures right, 30.0 FPS (the game's own interval of 2) with p95 34.0 ms, and four windows at 25 to 29 FPS around hitches of 0.3 to 1.9 s while walking (the largest 1944 ms; the rest were
1176, 886 and 317 ms). Both games' save folders were empty before and are empty again.

Tests: `tests/test_n*.py`, `tests/test_controllers.py`, `tests/test_mkw_runtime.py`, `tests/test_mkw_perf.py`, `tests/test_av1.py`,
`tests/test_opus.py` pass on Windows; the NVN archive and the Linux runtime build in WSL.

### Switches added

| switch | what |
|---|---|
| `NXRT_AQ_FORCE=1` | the old queue rule: a present or the queue's next submission resumes a parked one unmet |
| `NXRT_VK_WAIT_EVENT_MS` (2000) | how long an event wait may stay unmet before it is given up |
| `NXRT_VK_POOLS=0` | no texture / sampler pool set |
| `NXRT_VK_NDSR=0` | the game's own NDSR kernels instead of the host's copy |
| `NXRT_VK_TO_GPU=1` | a texture read from its storage becomes a GPU texture when the game draws into it (off by default, above) |
| `NXRT_VK_SYNC=all` / `use` | every written page to the device each frame always / never (default: while raw-pointer shaders draw) |
| `NXRT_VK_CZ=0` | draws into block-compressed textures are lost again |
| `NXRT_VK_TD_RAW=1` | `texdump` also writes each texture's raw texels (`<file>.raw`) |
| `SWR_NVN_INTERVAL_BT=1` | the guest stack of each `nvnWindowSetPresentInterval` call (Windows) |

Evidence of this round: `build/evidence/gameplay4/` of the main checkout (not in git): run folders, the patch scripts, `r12-pd.txt` (a
pass dump of the scene), `hide-sheet.jpg`, the saves copied aside before the runs and their hashes.

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
