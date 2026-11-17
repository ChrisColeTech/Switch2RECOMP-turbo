# Gameplay investigation (PLZA, DKB, MKW): hand-off

Branch `gameplay-perf` (worktree `D:\Projects\Switch2RECOMP-plza`), based on turbo `4b4b239`. Not merged and not
released. The owner asked for a hand-off: the main graphics bug (Donkey Kong Bananza terrain drawn black) was located
but **not fixed**. This document is the complete state for the next agent.

Owner's report: PLZA and DKB do not work past their title screens; in gameplay textures and graphics do not load (black
screens, black gaps); MKW plays at ~30 FPS instead of 60.

Display: console session (not RDP), Intel display adapter at 60 Hz, Vulkan on an RTX 5090 (driver 616.92). FPS numbers
below are on the local display.

## Commits on the branch

| commit | what |
|---|---|
| `0ae160a` | Switch 2 video decoder: fences that wait, re-release accepted, bitstream back-pressure (`runtime/svc_ns2.c`) |
| `8f5410f` | Metrics, input record/replay, dropped-work counters, logs; gameplay harness (`tools/plza_probe.py`, `tools/plza_inputs/`) |
| (this doc) | hand-off |

`python -m unittest discover -s tests -p "test_n*.py"`: 17 tests OK. All three library builds (DKB, MKW; PLZA not
rebuilt this round) were rebuilt with the branch runtime through the engine CLI, into their real `buildDir`s.

## Result per game

### Donkey Kong Bananza: past the title, plays; terrain black (OPEN)

Fixed (`0ae160a`): the game aborted (svcBreak, `nn::codec::ReleaseVp9DecoderOutputBuffer`, result `0xf201` =
NotFound) the first time a tutorial video ended ("Y Punch" prompt, ~1 s into gameplay). After that was patched the game
spun its video thread at a full core (~280 000 IPC/s) and the world stayed frozen. Three decoder bugs:

1. **Decoder fences were complete at once.** `nn::gs::TryWaitNativeSync` -> `nn::syncpt::sdk::TryWaitSyncPoint` (soft
   backend, sdk `0xad834f4` -> `0xad83550`) reads the syncpoint cache (transfer memory, `g_sc_ro + 0x40 + id*8`) and
   treats an entry of `~0` ("unknown") as **signalled, without requesting a refresh**. The runtime seeds all 192 entries
   with `~0` and only fills requested ones, so the decoder's syncpoint (20) was never filled. Fix: the decoder's syncpoint
   is *published* (kept current every ms by the cache thread, `sc_publish`). Only runtime-owned syncpoints are published
   on purpose: publishing GPU counters the emulation never advances could turn passing waits into hangs.
2. **Re-release after flush.** After cmd 8 (flush) the game releases images 1, 2, 1; the SDK aborts on any error.
   cmd 314 on an image the decoder already has is now accepted (same fence, newer client fence).
3. **Pictures dropped.** The game submits a whole clip in ~0.1 s; pictures were decoded at submit into a 32-picture
   queue and the oldest dropped (135 of 181). Now the bitstream waits in a packet FIFO and is decoded 6 pictures ahead as
   output images return; ready event follows the FIFO. The 181-picture clip now plays in 3.34 s, 0 dropped.

Verified: `build/plza-investigation/20261117-133337-dkb-punch` and `...-141344-dkb-cpuwins`: DK walks, punches gold
(counter 0 -> 57.13), breaks the banana crystal, 45-60 FPS. The tutorial video panel still shows a flat yellow box
(not investigated).

**OPEN: the destructible terrain is drawn black.** Evidence and analysis in the next section. This is the
"black gaps" the owner reports, at least for DKB.

### Mario Kart World: race reached, ~45 FPS (OPEN)

`build/plza-investigation/20261117-142014-mkw-race`: title -> L+R -> A x8 -> Grand Prix, Mario Bros. Circuit, racing
(`tools/plza_inputs/mkw_drive.txt`). Graphics look correct in the race screenshots. FPS in the race (t >= 150 s):
**45.1 mean** on the 60 Hz local display (not RDP). Frame time p50 17.0 ms, p95 28.3, p99 96.1.

Where the time goes (harness `report` metrics summary, last 30 % of frames): replay thread CPU **21.1 ms/frame**,
GPU 6.5 ms/frame, replay waits 5.0 ms, pipelines 2.8 ms, texture uploads 3.7 ms. 135 shaders missed and 133 arrived
from the live translator during the run (2011 draws skipped for a missing shader, pop-in). The exclusive monitor is off
for 04 titles. **The limiter is the NVN replay thread's CPU time (> 16.7 ms), not the GPU and not guest CPU.** Not
investigated further; next step: `gamectl set phases 1` / `NXRT_VK_PHASES` and the per-frame `frames.csv` columns to
split the 21 ms (descriptor sets, `vkm_use` syncs, write-backs ~1000/frame).

### PLZA: not driven into gameplay this round

Earlier rounds reached Language -> Appearance at 30 FPS. Next screens (likely name entry: software keyboard applet,
then the intro) were not attempted. Use `tools/plza_probe.py` with `--metrics --record`.

## DKB black terrain: what is known

Run with the switches below to reproduce; every experiment ran in the live game via the control pipe
(`tools/plza_probe.py action RUN ctl --ctl "<cmd>"`), dumps moved into the run folders.

**Frame structure** (pass dump `20261117-135123-dkb-pd`, frame at the start of gameplay, `pdscan.py`-style analysis):

- passes 13-18: **terrain depth pre-pass**, 720 non-indexed-indirect draws, VS `f9c807930abd1234`, FS `7551b531372e364c`,
  depth LESS write on, with a **geometry shader `5d537f7630ec3671`**: an NVIDIA *passthrough* GS
  (`SPV_NV_geometry_shader_passthrough`, `ShaderViewportMaskNV`; both extensions enabled on the device). The GS is a GPU
  culling/compaction stage: per triangle a visibility test; visible: subgroup ballot, the highest lane does
  `atomicAdd(gp_s0[0], 3*count)`, the base is shuffled, indices are written as u16 into `gp_s1` at byte
  `(base + prefix*3)*2` (16-bit CAS helper), `gl_ViewportMask = 1`; culled: mask 0. **No wrap/modulo on the offset.**
- pass 28: **terrain shading**, 721 *indexed indirect* draws, VS `d661413914c84d55` (same program as the pre-pass VS
  minus the VertexIndex output), FS `6d1b110994db5413`, depth **EQUAL**, depth write off. Indirect args = the GS's
  `gp_s0` (per chunk, `ind=` equals the GS's slot0 address), indices = `gp_s1` (15 000 bytes = 7 500 indices per chunk).

**Observations:**

1. Texdumps (`...-134433-dkb-rt`, `...-135735-dkb-hide`): the material/ID target (RGBA8 1920x1080) has no terrain (clear
   colour, blue); depth **has** the terrain; lit output black there.
2. `hide f9c807930abd1234` (pre-pass hidden): the shading pass then draws terrain everywhere and the lit frame shows a
   detailed tunnel (beams, walls). `hide d661413914c84d55`: ID target blue again. So the shading draws do rasterize; they
   lose the depth test against the pre-pass.
3. Relaxing depth EQUAL to LEQUAL for test-only draws (applied to ~850 draws/frame): **no visible change**. So it is not
   an ulp mismatch; the pre-pass depth has geometry nearer than anything the shading pass draws (the cave walls around
   the camera: they are in the pre-pass, missing from the appended lists).
4. `set no_gs 1` (draw without the GS): identical screen and depth.
5. **The append counters never return to 0.** Guest copies of `gp_s0` (written back from the GPU) at pass-dump time:
   counts like 612 759 indices for a 587-vertex chunk, 544 of 721 chunks above the 7 500-index capacity; ratio vs frame
   number fits ~50-740 appends/frame accumulated over ~830 frames. Instance word 0 in the guest copy. With accumulated
   counters every append lands past the end of `gp_s1`, so after the first frames the shading pass draws only stale or
   no indices, and the walls exist only in depth. **This is the most likely root cause.** On the console something
   resets the counters every frame.
6. The args addresses change between dumps (`0x2490721c0...` vs `0x249051540...`): per-frame sub-allocation from a
   ring.
7. What does *not* reset them (pass dump `...-dkb-pd2`, with logging of buffer ops added): no `ClearBuffer` (fill) and
   no `CopyBufferToBuffer` in the frame; the 86 `UpdateUniformBuffer`s all target `0x15bec8xxx` (uniform data). None of
   the frame's compute dispatches binds the `0x249...` range.
8. Write-back race checked and **ruled out**: a version of `wr_land` that keeps a guest word the CPU changed after the
   GPU work began (`g != base && g != after`) counted 0 such words over a whole run (metric `wb_cpu_newer`, kept as a
   counter). Starting with `NXRT_VK_WB_OFF=1`: guest args read 0 (as expected with no write-back), image unchanged.
9. Live switches with no effect on the terrain: `use_fast 0`, `use_seen 0`, `wb_off 1`, `set_reuse 0`, `dyn_dedupe 0`,
   `tex_memo 0`, `up_barrier 0`, `wb_whole 1` (`...-dkb-switches`).

**Next steps (in order):**

1. Find who zeroes a chunk's args on the console. Most direct: a write watch in the dirty-tracking fault path
   (`runtime/nvn/gfx/nvn_dirty.c`): for faults inside a watched range (`SWR_WATCH=<addr>,<len>`, e.g. the ring around
   `0x249040000`), log the host RIP mapped to the guest function and the guest LR of the current thread, once per site.
   If the CPU zeroes the slot: check whether `pool_sync`/pending-merge uploads it before the GS runs (the shadow may
   already equal 0 when write-back is off; with write-back on, look at the ordering of landing vs the CPU write).
   If nothing on the CPU writes it: look for a GPU write path the replay does not model (semaphore/report writes,
   `nvnCommandBufferReportCounter`/`SetMemory`-style NVN2 commands, copy-engine ops), or check whether NVN2 resets
   SSBO-bound atomics through the 0x644/c0 driver block.
2. Alternatively find the allocator in the game: the args address is the GS's `gp_s0`, loaded from the driver constant
   block at `c0 + 1040` (SSBO binding table); trace the command-buffer code that binds it
   (`nvnCommandBufferBindStorageBuffer` HLE, `OP_BIND_SSBO`) back to the guest caller (`kthread` LR) and read the
   recompiled C for the reset.
3. Verify with the black audit: `tools/plza_probe.py report` prints per-screenshot near-black fraction and black cells;
   the start-of-gameplay view is ~68 % near-black, 11-12/64 black cells today. A fixed frame should look like the
   `hide f9c807...` frame in `...-dkb-hide/texdump`, but with the near walls drawn.

Ruled-out theories, for the record: shader cache misses (the live translator fills the cache, ~0.7 s pop-in; three
boot-time programs once failed to dump, timing dependent, now logged with the reason); missing NV extensions;
indirect-count draws; conditional rendering (`draw_render_skip` = 0); depth-EQUAL invariance; vertex inputs missing.

## Instrumentation (`8f5410f`): how to turn it on

| switch | what |
|---|---|
| `SWR_METRICS=1` (or a path), or `[Debug] metrics=1` in the game's ini (launcher per-game ini) | `metrics-<time>.csv` next to the log, once a second: frames, FPS, process/guest CPU, 3 busiest threads, time blocked per wait kind (condvar, mutex, address, waitsync, sleep, IPC) and counts, xmon, draws, skipped draws by reason, dispatches, shaders missing/arrived/failed/stale, textures made/dropped by reason, views failed, shaders unusable, draws dropped (primitive, mesh, conditional render), `wb_cpu_newer`, GOB order. Also turns on `SWR_FRAME_CSV` (`frames-<time>.csv`, per frame: present CPU/acquire/fence/wait, replay CPU/GPU/waits/pipelines/textures, cumulative). Off: one branch per SVC. |
| `SWR_INPUT_RECORD=<file>` | every pad change: `<ms> <buttons hex> <lx> <ly> <rx> <ry>` |
| `SWR_INPUT_REPLAY=<file>` | plays such a file over the pad when it appears; again whenever it is rewritten |
| logs (always) | each texture format/layout combination once (linear / block-linear + GOB order); a draw skipped for a missing shader names the program and its keys; why a shader could not be dumped; NV passthrough/viewport-array2 in the features line; pass dumps show buffer copies/updates/fills |
| `gamectl set no_gs 1` | draws without geometry shaders (diagnostic) |

Harness `tools/plza_probe.py` (works for any game folder via `--game`): `start LABEL --game DIR --seconds N
--shot-every S [--metrics] [--record] [--stacks] [--verbose] [--env K=V]`; `action RUN {a,b,x,y,lr,plus,up,down,left,
right,shot,ctl,replay,stop} --observed TEXT [--settle S] [--ctl "cmd"] [--file script]`; `report RUN` (result, black
audit per screenshot, metrics summary). Input scripts: `tools/plza_inputs/{dkb_walk,dkb_punch,mkw_drive}.txt`.

Gotchas: `start` refuses while the game exe is running (wait for `result.json`); the ctl `texdump` writes PNGs beside
the game log (move them out); the harness only ever stops processes it started.

## Evidence

All under `D:\Projects\Switch2RECOMP-plza\build\plza-investigation\`:
`20261117-125403-dkb-play` (the abort), `-130601-dkb-play3` (spin/freeze), `-131949-dkb-video2` (syncpoint cache ~0),
`-132645-dkb-sync`, `-133337-dkb-punch` (gameplay works), `-134433-dkb-rt` and `-135735-dkb-hide` (texdumps),
`-135123-dkb-pd` and the `dkb-pd2` run (pass dumps), `-1347xx-dkb-gs` (GS A/B), `dkb-eq`, `dkb-switches`, `dkb-wboff`,
`-141344-dkb-cpuwins`, `-142014-mkw-race`. Each has screenshots, `run.log`, `progress.jsonl`, `actions.jsonl`, and with
`--metrics` `metrics.csv` + `frames.csv`.

## Not done (from the task)

DKB terrain fix; MKW 60 FPS; PLZA gameplay; regressions on all three plus Zelda OoT / SMB Wonder; merge to turbo; Engine
CI; launcher submodule bump and VERSION 0.1.0 -> 0.2.0 release. Nothing was merged or released.
