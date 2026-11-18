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
| Ocarina of Time | did not start (svcBreak 1.6 s in) | title screen and menu; its AV1 movies are decoded (dav1d, `AV1_DECODER.md`) and reach the game's textures | the movie network (`nn::ndve`) between those textures and the screen does not compute yet, so the movies still show black; the game itself not reached yet |

Sections 1 to 5 are the first round as it was written; section 6 is the second round, which found the causes sections 1
and 3 left open. The AV1 decoder that section 6 left open for Ocarina of Time is in `AV1_DECODER.md`.

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
