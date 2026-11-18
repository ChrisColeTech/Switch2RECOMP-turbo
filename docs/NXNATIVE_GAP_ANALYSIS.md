<!-- SwitchRecomp - Copyright 2026 MZ Labs
     Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
     Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. -->
# Gap analysis against nxnative

Status 2026-11-17. nxnative (https://github.com/seedds/nxnative, MIT, (c) seedds; a checkout at `D:\Projects\nxnative`) hosts
the same Switch and Switch 2 games natively on Apple hardware and has played Donkey Kong Bananza, Mario Kart World and Ocarina
of Time, with a profile for Pokemon Legends Z-A. This engine's NVN layer (`runtime/nvn`) is a port of an older nxnative, and
its shaders are translated by an older build of nxnative's translator (`tools/nxshader/xlate_bin`). This document lists what
nxnative implements that this engine did not, ranked by its effect on the five library games, with the nxnative reference for
each and the state here. References are to nxnative's `docs/reference/<file>.md` section titles and to
`dm/games/<title id>/README.md`.

What was ported in this round names nxnative in the source where the logic came from it.

## Fixed in this round

| # | Feature | nxnative reference | Games | Was | Now |
|---|---|---|---|---|---|
| 1 | Raw global memory: a shader's loads and stores at an address it loaded from memory go through a table of the memory pools | shaders.md "Global memory at an address loaded from memory"; DKB README, Rendering 7 | Donkey Kong Bananza (terrain), Ocarina of Time (foliage), any gsys game | the translator emitted the access; the Vulkan host bound nothing for the table (the reflection read it as a storage buffer in uniform slot 252), so every such store was lost | `vk_mem.c` `vkm_gmem_table` / `vkm_gmem_log`, `vk_replay.c` `gm_frame` / `gm_writes`: the table of pool mirrors by device address, the write log read once a frame, logged pages copied back to guest memory. The invented reset of DKB's append counters (`append_reset`) is off |
| 2 | Layered render passes: a vertex shader picks the layer | PLZA README, Rendering ("a black world once the opening reached Lumiose"); shaders.md "Layer entry points..." | Pokemon Legends Z-A | `VkRenderingInfo.layerCount` was 1: the 32x32x32 colour grading table got one plane, the tone mapping multiplied the world by black | `vk_replay.c` `pass_begin`: as many layers as every attachment's view holds |
| 3 | Transform feedback | rendering.md "Transform feedback captured by a vertex entry point"; PLZA README, Rendering | Pokemon Legends Z-A (116-190 captures a frame: every skinned mesh) | recorded and ignored | `vk_shader.c` `vksh_xfb_layout` / `vksh_xfb_variant` (the layout from the program's control section, the outputs as XFB variables), `vk_replay.c` `tf_begin` / `tf_end` (VK_EXT_transform_feedback, write-back of the captured bytes) |
| 4 | The Switch 2's system tick is 31.25 MHz | frame-rate.md "The Switch 2's system tick is 31.25 MHz" | every Switch 2 title | 19.2 MHz for every program: a Switch 2 game measured every span at 0.6144 of its length | `main.c` `rt_ticks`: 31.25 MHz for title IDs 04... (`SWR_TICK_HZ` overrides) |
| 5 | The thread ID at ThreadLocalRegion+0x110 | threads.md "The thread ID at ThreadLocalRegion+0x110 (SDK 22)" | Ocarina of Time (SDK 22.2.8) | 0: every unlocked mutex read as held by the caller, the SDK's allocator aborted at its first lock (svcBreak 1.6 s in) | `kernel.c` `kthread_create` |
| 6 | `WaitForAddress` type 3 (WaitIfEqual64) | PLZA README, Services ("`WaitForAddress` type 3 ... added for SDK 19") | Ocarina of Time | InvalidEnumValue, the SDK aborted | `kernel.c` `svc_wait_addr` |
| 7 | A 2D array in a cube slot | PLZA README, Rendering ("a 2D texture in a cube slot") | Pokemon Legends Z-A (environment maps 64x64x6, 128x128x6) | viewed as a 2D array through a cube sampler | `vk_tex.c`: square arrays of six layers or more are created cube compatible and viewed as cubes. A texture with fewer layers is still viewed as an array (nxnative binds a zero cube) |
| 8 | GPU event waits park one NVN queue while another runs | nvn-objects.md "GPU event waits order work across queues" (`dm/gfx/nvn/nvn_queue.inc`) | Donkey Kong Bananza (two queues), Ocarina of Time | a submission was replayed whole; a wait another queue's commands signal timed out after 250 ms | `hle_nvn.c` `nvn_aq_park`: submissions translated on fibers, parked at an unmet wait while another queue's submission goes first (Windows; Linux as before) |
| 9 | The time service's shared page as SDK 22 reads it | services.md "`clock_gettime` is `nn::time`'s steady clock (SDK 22)" (nxnative takes the functions over; here the page is served) | Ocarina of Time | ContinuousAdjustmentTimePoint entries laid out as 0x48 bytes: `clock_gettime(CLOCK_REALTIME)` failed | `svc_core.c` `time_shm_update`: 0x38 bytes, the source ID at +0x28 |
| 10 | Event descriptors without a network | services.md (gRPC's `EventFd` wake-ups, Ocarina of Time) | Ocarina of Time | `EventFd` failed, gRPC aborted | `svc_core.c` `bsd_evfd` |
| 11 | `tex_flush_upload` / `tex_dirty_upload` by title | Ocarina README "What it needs" | Ocarina of Time, Tears of the Kingdom | one set of profile constants (Mario Kart World's) for every title | `game_profile.h` / `nvn_host.c` `dm_game_tex_rewrite` (on for the two titles; not yet seen at work: Ocarina does not reach its world) |

Also in this round, not from nxnative: the newer `bsd` registration commands, AV1 movies accepted with black pictures (there
is no decoder), the completion thread woken by a submission instead of a 1 ms sleep, write-back ranges joined.

## Open, ranked by expected effect on the library games

| Rank | Feature | nxnative reference | Games | State here | Symptom |
|---|---|---|---|---|---|
| 1 | Write-backs of draw-stage storage buffers the CPU never reads (nxnative has none: its memory is the game's); passthrough geometry draws gathered into one dispatch | shaders.md "Mesh geometry draws gathered into one dispatch", "Passthrough geometry shaders drawn by the vertex shader" | DKB (630 culling draws a frame) | the geometry shaders run natively (`VK_NV_geometry_shader_passthrough`); each chunk's record and list are copied back to guest memory every frame | DKB at 48.7 FPS instead of 60 (GAMEPLAY_INVESTIGATION.md section 6: write-back on demand is the next step) |
| 2 | GPU time in counter reports: per submission segment, and nanoseconds for NVN2 | frame-rate.md "GPU timestamps per submission segment", "NVN2 timestamps are nanoseconds" | DKB, MKW, Ocarina (dynamic resolution, the frame-rate controller) | missing: `vk_submit.c` `post_run` writes `guest_ns()` (divided by 256 unless `game_dynamic_resolution` is on) in 614.4 MHz units for every title | the games' dynamic resolution never steps down; with `game_dynamic_resolution=1` a Switch 2 game reads 0.6144 of the real time |
| 3 | AV1 movies | movies.md "`nn::codec` video decoders taken over"; Ocarina README, Files | Ocarina of Time | missing: `host1x.c` and `svc_ns2.c` decode H.264 and VP9; AV1 is accepted and gives black pictures | Ocarina's title and opening movies are black |
| 4 | Current translator chain | shaders.md "Raw global memory through a pointer read when the draw is bound", "128-bit raw global loads...", "Early fragment tests from the control section", "Bindless constant buffer handles..." | Ocarina of Time (foliage: 2,241 shaders with raw loads; pools reached only through pointers), DKB (2,855 pixel shaders with the NVN2 early-test flag) | missing: `xlate_bin` has none of `FindPointerSite`, `GenerateRawVectorLoad`, `ABindlessConstant`, `F2ip`; `nxs_front.py` reads the early-test flag at +0x718 only; the pool table has 4,096 entries (nxnative 8,192: Ocarina holds about 4,170 ranges) | missing foliage and moss in Ocarina; depth-tested-late storage writes |
| 5 | Maps past the application pool fail | memory.md "Maps past the application pool fail as on Horizon"; PLZA and Ocarina READMEs (`mem_map_limit`) | Pokemon Legends Z-A, Ocarina of Time | missing: `kernel.c` svc 0x2C fails only when the host cannot commit | the SDK's allocator never returns its cached pages; nxnative saw the pool overrun in Z-A's first minute |
| 6 | A pass that samples the texture it renders (mip chains, upsample chains, depth read in the pass that writes it) | rendering.md "Mip-chain pass sampling the level it renders", "Upsample pass reading the level above...", "Depth sampled in the pass that writes it" | DKB (environment cube NaNs, bloom), Ocarina | missing: `VK_EXT_attachment_feedback_loop_layout` is probed (`vk_dev.c`) and not used | undefined in Vulkan; not seen on the test card (NVIDIA) |
| 7 | Occlusion conditions read once landed | rendering.md "Occlusion conditions read once landed" | PLZA, MK8D, DKB | partial (`vk_replay.c` `rc_value`: unknown for any address a report wrote) | culled draws are drawn: GPU time only |
| 8 | Reads past a uniform binding's size | shaders.md "Reads past a uniform binding's size" | all | partial: `robustBufferAccess`, on by default for NVIDIA only | undefined reads on other vendors |
| 9 | A recording's end and links in control memory | nvn-objects.md "A recording's end and links are in control memory" | Super Mario Bros. Wonder S2, Drag x Drive; DKB needed the call-copy part | missing (`hle_nvn.c`: control memory use always 0; `OP_CALL` keeps the handle) | a called list replayed after its memory was reused |
| 10 | GetInfo 31/32 (remote region use) | DKB README, Boot 2 | Switch 2 titles | partial: 29/30 answer 0, 31/32 warn | a warning line |

Checked and present here (no gap): conditional rendering evaluated at replay, depth-stencil views in STENCIL mode, min/max
reduction samplers, the base instance at c0+0x644, samples-passed queries, large sparse textures with shared physical tiles,
the Switch 2's device values and NVN2 entry points (ProgramBuilder, recording modes), task and mesh draws, passthrough
geometry shaders, `nvnCommandBufferFenceSync`, the NVN state getters, the game-chosen present interval, the pipeline manifest
and cache, depth mode zero, csrng, `GetAudioOutBufferCount`, the graphics scale, the Mii database stand-in, the Switch 2's
9 GB pool, the Tegra X1 GOB order for Switch 1 programs (`main.c` sets `NXRT_TEX_ORDER=tx1` for title IDs 01...).

Not applicable: what nxnative does for Metal (pools wrapped in chunks, MetalFX, pipeline archives, draw barriers) and its
cuts of SDK functions (this engine runs the SDK against its own services).

## Next, in order

1. Write-back on demand for draw-stage storage buffers (rank 1): DKB's frame rate.
2. An AV1 decoder for the Switch 2 video service (rank 3): Ocarina's movies.
3. Counter report times (rank 2), then `game_dynamic_resolution` can default to on.
4. A newer translator (rank 4): needs the chain's Python front end moved with it, and every game's shader cache is made
   again. With it, pointer sites so that raw loads find current memory (Ocarina's culling), and the 8,192-entry table.
5. A per-frame budget for first-use texture transcodes (Mario Kart World's long frames; not an nxnative item).
