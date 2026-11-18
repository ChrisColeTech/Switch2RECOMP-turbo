<!-- SwitchRecomp - Copyright 2026 MZ Labs
     Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
     Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. -->
# Gap analysis against nxnative

Status 2026-11-18. nxnative (https://github.com/seedds/nxnative, MIT, (c) seedds; a checkout at `D:\Projects\nxnative`) hosts
the same Switch and Switch 2 games natively on Apple hardware and has played Donkey Kong Bananza, Mario Kart World and Ocarina
of Time, with a profile for Pokemon Legends Z-A. This engine's NVN layer (`runtime/nvn`) is a port of an older nxnative, and
its shaders are translated by an older build of nxnative's translator (`tools/nxshader/xlate_bin`). This document lists what
nxnative implements that this engine did not, ranked by its effect on the five library games, with the nxnative reference for
each and the state here. References are to nxnative's `docs/reference/<file>.md` section titles and to
`dm/games/<title id>/README.md`.

What was ported in this round names nxnative in the source where the logic came from it.

## Fixed in the second round (2026-11-17, evening)

| # | Feature | nxnative reference | Games | Was | Now |
|---|---|---|---|---|---|
| 1 | Raw global memory: a shader's loads and stores at an address it loaded from memory go through a table of the memory pools | shaders.md "Global memory at an address loaded from memory"; DKB README, Rendering 7 | Donkey Kong Bananza (terrain), Ocarina of Time (foliage), any gsys game | the translator emitted the access; the Vulkan host bound nothing for the table (the reflection read it as a storage buffer in uniform slot 252), so every such store was lost | `vk_mem.c` `vkm_gmem_table` / `vkm_gmem_log`, `vk_replay.c` `gm_frame` / `gm_writes`: the table of pool mirrors by device address, the write log read once a frame, logged pages copied back to guest memory. The invented reset of DKB's append counters (`append_reset`) is off |
| 2 | Layered render passes: a vertex shader picks the layer | PLZA README, Rendering ("a black world once the opening reached Lumiose"); shaders.md "Layer entry points..." | Pokemon Legends Z-A | `VkRenderingInfo.layerCount` was 1: the 32x32x32 colour grading table got one plane, the tone mapping multiplied the world by black | `vk_replay.c` `pass_begin`: as many layers as every attachment's view holds |
| 3 | Transform feedback | rendering.md "Transform feedback captured by a vertex entry point"; PLZA README, Rendering | Pokemon Legends Z-A (116-190 captures a frame: every skinned mesh) | recorded and ignored | `vk_shader.c` `vksh_xfb_layout` / `vksh_xfb_variant` (the layout from the program's control section, the outputs as XFB variables), `vk_replay.c` `tf_begin` / `tf_end` (VK_EXT_transform_feedback, write-back of the captured bytes) |
| 4 | The Switch 2's system tick is 31.25 MHz | frame-rate.md "The Switch 2's system tick is 31.25 MHz" | every Switch 2 title | 19.2 MHz for every program: a Switch 2 game measured every span at 0.6144 of its length | `main.c` `rt_ticks`: 31.25 MHz for title IDs 04... (`SWR_TICK_HZ` overrides) |
| 5 | The thread ID at ThreadLocalRegion+0x110 | threads.md "The thread ID at ThreadLocalRegion+0x110 (SDK 22)" | Ocarina of Time (SDK 22.2.8) | 0: every unlocked mutex read as held by the caller, the SDK's allocator aborted at its first lock (svcBreak 1.6 s in) | `kernel.c` `kthread_create` |
| 6 | `WaitForAddress` type 3 (WaitIfEqual64) | PLZA README, Services ("`WaitForAddress` type 3 ... added for SDK 19") | Ocarina of Time | InvalidEnumValue, the SDK aborted | `kernel.c` `svc_wait_addr` |
| 7 | A 2D array in a cube slot | PLZA README, Rendering ("a 2D texture in a cube slot") | Pokemon Legends Z-A (environment maps 64x64x6, 128x128x6) | viewed as a 2D array through a cube sampler | `vk_tex.c`: square arrays of six layers or more are created cube compatible and viewed as cubes. A texture with fewer layers is still viewed as an array (nxnative binds a zero cube) |
| 8 | GPU event waits park one NVN queue while another runs (Linux: item 15) | nvn-objects.md "GPU event waits order work across queues" (`dm/gfx/nvn/nvn_queue.inc`) | Donkey Kong Bananza (two queues), Ocarina of Time | a submission was replayed whole; a wait another queue's commands signal timed out after 250 ms | `hle_nvn.c` `nvn_aq_park`: submissions translated on fibers, parked at an unmet wait while another queue's submission goes first (Windows; Linux as before) |
| 9 | The time service's shared page as SDK 22 reads it | services.md "`clock_gettime` is `nn::time`'s steady clock (SDK 22)" (nxnative takes the functions over; here the page is served) | Ocarina of Time | ContinuousAdjustmentTimePoint entries laid out as 0x48 bytes: `clock_gettime(CLOCK_REALTIME)` failed | `svc_core.c` `time_shm_update`: 0x38 bytes, the source ID at +0x28 |
| 10 | Event descriptors without a network | services.md (gRPC's `EventFd` wake-ups, Ocarina of Time) | Ocarina of Time | `EventFd` failed, gRPC aborted | `svc_core.c` `bsd_evfd` |
| 11 | `tex_flush_upload` / `tex_dirty_upload` by title | Ocarina README "What it needs" | Ocarina of Time, Tears of the Kingdom | one set of profile constants (Mario Kart World's) for every title | `game_profile.h` / `nvn_host.c` `dm_game_tex_rewrite` (on for the two titles; not yet seen at work: Ocarina does not reach its world) |

Also in the second round, not from nxnative: the newer `bsd` registration commands, AV1 movies accepted with black pictures (there
is no decoder), the completion thread woken by a submission instead of a 1 ms sleep, write-back ranges joined.

## Fixed in the third round (2026-11-17, night)

| # | Feature | nxnative reference | Games | Was | Now |
|---|---|---|---|---|---|
| 12 | Write-backs of GPU-written pool memory the CPU never reads (was rank 1) | nxnative has none: its memory is the game's (memory.md) | Donkey Kong Bananza (630 terrain chunks a frame), Mario Kart World (its compute buffers) | every GPU write copied back to guest memory at completion, compared, written and sent to the device again: about 8 MB a frame each way in DKB | write-back on demand (`vk_mem.c` "Write-back on demand", `nvn_dirty.c` `PG_LAZY`): a page GPU writes keep coming to is handed to the device and made inaccessible; the first CPU access reads it back. DKB 48.7 -> 57.2 FPS in the tunnel; 7.8 MB a frame not copied in DKB, 25 MB in a Mario Kart World race. `NXRT_VK_WB_LAZY=0` |
| 13 | GPU time in counter reports: per report, without the host's translation gaps, nanoseconds for NVN2 (was rank 2) | frame-rate.md "GPU timestamps per submission segment", "NVN2 timestamps are nanoseconds" | DKB, MKW, Ocarina (dynamic resolution, the frame-rate controller) | one time for every report of a submission, the guest clock at completion, in 614.4 MHz units for every title (divided by 256 unless `game_dynamic_resolution` was on) | `vk_replay.c` "Counter reports: the GPU's own time": a Vulkan timestamp query per report (top of the pipeline for `TIMESTAMP_TOP`), brought to the guest clock, the idle time between the parts of one NVN submission left out, nanoseconds for a Switch 2 SDK. `game_dynamic_resolution=auto` (on with this renderer) is the default. Beyond nxnative: the idle time between all the parts of a frame is left out, not only inside one submission (`rep_gaps`): with nxnative's rule DKB, 8 submissions a frame, read its frame time as GPU time and sat at its smallest size, 2095x1179; now its scene is at 2880x1620 |
| 14 | Maps past the application pool fail (was rank 5) | memory.md "Maps past the application pool fail as on Horizon" (`mem_map_limit`) | Pokemon Legends Z-A, Ocarina of Time | svc 0x2C failed only when the host could not commit | `kernel.c` svc 0x2C: ResultLimitReached past the pool for the two titles nxnative has it on for (`SWR_MAP_LIMIT=1/0`). Z-A: one 4 MB map refused with 3,174 MB mapped, the SDK gives its cached block back and goes on |
| 15 | GPU event waits park a submission: Linux (item 8 was Windows only) | nvn-objects.md "GPU event waits order work across queues" | DKB, Ocarina | Windows fibers only | `host/linux/windows.h`: the three fiber calls on `ucontext`; built in WSL, not run on a Linux GPU here |
| 16 | The operation mode: docked | services.md, `hle_oe.c` (`DM_OPMODE`; docked 1920x1080, tier 0 graphics scale in both modes) | all five | `docked=0` was the written-out default of every ini: handheld. DKB 1920x1080, MKW 1920x1080, Ocarina 1920x1080, Z-A a 1280x720 picture of a 1920x1080 scene, MK8D 1280x720 | `docked=1` by default, older inis moved once (`settings_version`): DKB 2880x1620, MKW 2560x1440, Ocarina 2560x1440, Z-A 1920x1080, MK8D 1920x1080. Not an nxnative gap: the graphics scale already answered tier 0 |

Also in this round, not from nxnative: the MiiEdit applet answers "cancelled" (Mario Kart 8 Deluxe crashed on its first screen's Create a Mii), `--key=value`
applies any ini option for one run, a GPU wait that runs out is logged with what it waited for, the pass sizes a game draws at are in the log
(`render sizes up to frame`), `tools/play_scenario.py`.

## Fixed in the fourth round (2026-11-18)

| # | Feature | nxnative reference | Games | Was | Now |
|---|---|---|---|---|---|
| 17 | NVN2 push constants named as a driver bank | `nvn_refl.m` (`NVN_RB_PUSH`: a `drvcb` binding of bank 24 is the push block) | Ocarina of Time (the movie's colour conversion and resampler) | bound to zeros: the conversion's matrix was zero, every movie picture black | `vk_replay.c` `write_set_`: the pushed bytes |
| 18 | The NDVE movie network by the host (was rank 1) | shaders.md "The NDVE movie network by the host" | Ocarina of Time | the game's 14 dispatches: tensors through raw global memory (52 MB a frame written back), the resampler's output garbage, 27 FPS | `vk_replay.c` `ndve_host`: recognised by the model's shader keys as in nxnative; the convolutions skipped, the picture copied, the resampler a linear blit; 60 FPS. Not nxnative's kernels: the network's correction of the picture is left out (`NXRT_VK_NDVE=0`: the game's dispatches). Image stores without the IMAGE flag are on |
| 19 | Opus through the hardware decoder service | audio.md "Hardware Opus decoders built on the SDK's software decoders" (nxnative uses the SDK's decoder; here libopus) | Ocarina of Time (movie sound), any game with Opus streams | the repository had no libopus archive: every packet decoded to silence | `runtime/opus` (Opus 1.5.2, `build_opus.py`, both archives, `tests/test_opus.py`) |
| 20 | Audio renderer mixes and splitters as the SDK sends them since REV7 | audio.md (nxnative runs the SDK's own renderer) | every Switch 2 game here (DKB, MKW, Ocarina) | a changed-mixes-only update read as "no mixes": voices straight to stereo, no mix volumes | `audio.c` `swr_audio_update_routing`: mixes kept by ID, splitters parsed and kept |

Also in this round, not from nxnative: the present ticket race (`hle_nvn.c` `aq_push`), the fullscreen choice written to the ini, the control
pipe per process, `plza_probe.py --as-launcher`, the output level in the log.

## Open, ranked by expected effect on the library games

| Rank | Feature | nxnative reference | Games | State here | Symptom |
|---|---|---|---|---|---|
| 1 | Ocarina of Time's world: the first scene after the opening | the game's README ("What it needs", "Open"); nvn-objects.md "GPU event waits order work across queues"; shaders.md (raw global memory through pointers, mesh and task shaders) | Ocarina of Time | reached for the first time in the fourth round (`GAMEPLAY_INVESTIGATION.md` section 8, "Open: the game itself"). Missing or wrong: the picture (black), the queue order (the game's thread waits in `nvnSyncWait` for a submission of another queue parked at an event wait; done ops are published in order), `tex_pool` / `samp_pool` bindings (bound as empty), render targets whose content comes from their storage, two shaders the translator refuses (Ampere instruction 243), 90 MB of write-back a frame. The movie network's real kernels (item 18) are a second, smaller part | after the opening: a black picture with sound, 1 to 5 frames a second, a stop after about 80 frames |
| 2 | Current translator chain | shaders.md "Raw global memory through a pointer read when the draw is bound", "128-bit raw global loads...", "Early fragment tests from the control section", "Bindless constant buffer handles..." | Ocarina of Time (foliage: 2,241 shaders with raw loads; pools reached only through pointers), DKB (2,855 pixel shaders with the NVN2 early-test flag) | missing: `xlate_bin` has none of `FindPointerSite`, `GenerateRawVectorLoad`, `ABindlessConstant`, `F2ip`; `nxs_front.py` reads the early-test flag at +0x718 only; the pool table has 4,096 entries (nxnative 8,192: Ocarina holds about 4,170 ranges). Blocked on a build of nxnative's translator: it needs the .NET 10 SDK (this machine has 6, 7 and 9), nxnative's patched Ryujinx sources built with it, the chain's Python front end (`nxs_front.py`, `nxs_refl.py`, `dmsh_abi.h`) moved to the new ABI, and every game's shader cache made again | missing foliage and moss in Ocarina; depth-tested-late storage writes |
| 3 | First-use texture work spread over frames (not an nxnative item: its answer is Metal's placement sparse textures) | frame-rate.md | Mario Kart World | missing: a burst of new ASTC textures is transcoded and uploaded on the translation thread in the frame that first uses them (`vk_tex.c`); the prefetch (`NXRT_VK_TEX_PREFETCH`) showed no gain | 4 to 8 frames of 60-180 ms when a new part of the course streams in (GAMEPLAY_INVESTIGATION.md section 7) |
| 4 | Passthrough geometry draws gathered into one dispatch | shaders.md "Mesh geometry draws gathered into one dispatch", "Passthrough geometry shaders drawn by the vertex shader" | DKB (630 culling draws a frame) | the geometry shaders run natively (`VK_NV_geometry_shader_passthrough`), one draw each; their write-backs are gone (item 12) | translation thread time: DKB at 57 FPS, not a flat 60 |
| 5 | A pass that samples the texture it renders (mip chains, upsample chains, depth read in the pass that writes it) | rendering.md "Mip-chain pass sampling the level it renders", "Upsample pass reading the level above...", "Depth sampled in the pass that writes it" | DKB (environment cube NaNs, bloom), Ocarina | missing: `VK_EXT_attachment_feedback_loop_layout` is probed (`vk_dev.c`) and not used | undefined in Vulkan; not seen on the test card (NVIDIA) |
| 6 | Occlusion conditions read once landed | rendering.md "Occlusion conditions read once landed" | PLZA, MK8D, DKB | partial (`vk_replay.c` `rc_value`: unknown for any address a report wrote) | culled draws are drawn: GPU time only |
| 7 | Reads past a uniform binding's size | shaders.md "Reads past a uniform binding's size" | all | partial: `robustBufferAccess`, on by default for NVIDIA only | undefined reads on other vendors |
| 8 | A recording's end and links in control memory | nvn-objects.md "A recording's end and links are in control memory" | Super Mario Bros. Wonder S2, Drag x Drive; DKB needed the call-copy part | missing (`hle_nvn.c`: control memory use always 0; `OP_CALL` keeps the handle) | a called list replayed after its memory was reused |
| 9 | GetInfo 31/32 (remote region use) | DKB README, Boot 2 | Switch 2 titles | partial: 29/30 answer 0, 31/32 warn | a warning line |

Checked and present here (no gap): conditional rendering evaluated at replay, depth-stencil views in STENCIL mode, min/max
reduction samplers, the base instance at c0+0x644, samples-passed queries, large sparse textures with shared physical tiles,
the Switch 2's device values and NVN2 entry points (ProgramBuilder, recording modes), task and mesh draws, passthrough
geometry shaders, `nvnCommandBufferFenceSync`, the NVN state getters, the game-chosen present interval, the pipeline manifest
and cache, depth mode zero, csrng, `GetAudioOutBufferCount`, the graphics scale, the Mii database stand-in, the Switch 2's
9 GB pool, the Tegra X1 GOB order for Switch 1 programs (`main.c` sets `NXRT_TEX_ORDER=tx1` for title IDs 01...).

Not applicable: what nxnative does for Metal (pools wrapped in chunks, MetalFX, pipeline archives, draw barriers) and its
cuts of SDK functions (this engine runs the SDK against its own services).

## Next, in order

1. Ocarina of Time's first scene (rank 1): first the queue order (an unmet event wait parks its queue only, a queue's fence is its own work: nxnative's
   rule; the per-queue fence alone did not change the rate), then a pass dump of one frame of the scene for the black picture (`tex_pool` bindings,
   the render targets read from storage), then the translator (item 3 below).
2. First-use texture work over frames (rank 3): the small levels of a new texture now, the large ones in later frames, sampled through
   `VK_EXT_image_view_min_lod` so that a shader's size queries do not change; the view made again when the texture is whole. Mario Kart World's
   long frames.
3. A newer translator (rank 2, which rank 1 waits for): install the .NET 10 SDK, build nxnative's chain, move the Python front end with it, make every game's shader
   cache again. With it, pointer sites so that raw loads find current memory (Ocarina's culling), and the 8,192-entry table.
4. DKB's culling draws in one dispatch (rank 4).
