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

Also in this round, not from nxnative: `bsd` event descriptors and the newer registration commands (Ocarina of Time's network
library aborted without them), the completion thread woken by a submission instead of a 1 ms sleep.

## Open, ranked by expected effect on the library games

| Rank | Feature | nxnative reference | Games | State here | Symptom |
|---|---|---|---|---|---|
| 1 | GPU event waits park one NVN queue while another runs | nvn-objects.md "GPU event waits order work across queues" (`dm/gfx/nvn/nvn_queue.inc`) | Donkey Kong Bananza (two queues), Ocarina of Time (about 25 waits a frame) | partial: `vk_replay.c` `wait_event` waits on the recording thread and gives up after 250 ms; a submission is replayed whole | DKB's second queue runs its depth-sampling dispatch before the main queue's pre-pass instead of after it (it reads last frame's depth); one wait a session times out |
| 2 | GPU time in counter reports: per submission segment, and nanoseconds for NVN2 | frame-rate.md "GPU timestamps per submission segment", "NVN2 timestamps are nanoseconds" | DKB, MKW, Ocarina (dynamic resolution, the frame-rate controller) | missing: `vk_submit.c` `post_run` writes `guest_ns()` (divided by 256 unless `game_dynamic_resolution` is on) in 614.4 MHz units for every title | the games' dynamic resolution never steps down; with `game_dynamic_resolution=1` a Switch 2 game reads 0.6144 of the real time |
| 3 | Per-game profile keys | Ocarina README "What it needs" (`tex_flush_upload`, `tex_dirty_upload`); nvn-objects.md "Textures made for GPU use kept from their CPU rewrites" | Ocarina of Time | partial: `runtime/nvn/host/game_profile.h` holds one set of constants (Mario Kart World's) for every title; the mechanism is ported and off | Ocarina's terrain height, blend and hole maps stay zero (flat blue holes, grey stone) once it reaches its world |
| 4 | Current translator chain | shaders.md "Raw global memory through a pointer read when the draw is bound", "128-bit raw global loads...", "Early fragment tests from the control section", "Bindless constant buffer handles..." | Ocarina of Time (foliage: 2,241 shaders with raw loads; pools reached only through pointers), DKB (2,855 pixel shaders with the NVN2 early-test flag) | missing: `xlate_bin` has none of `FindPointerSite`, `GenerateRawVectorLoad`, `ABindlessConstant`, `F2ip`; `nxs_front.py` reads the early-test flag at +0x718 only; the pool table has 4,096 entries (nxnative 8,192: Ocarina holds about 4,170 ranges) | missing foliage and moss in Ocarina; depth-tested-late storage writes |
| 5 | Maps past the application pool fail | memory.md "Maps past the application pool fail as on Horizon"; PLZA and Ocarina READMEs (`mem_map_limit`) | Pokemon Legends Z-A, Ocarina of Time | missing: `kernel.c` svc 0x2C fails only when the host cannot commit | the SDK's allocator never returns its cached pages; nxnative saw the pool overrun in Z-A's first minute |
| 6 | A pass that samples the texture it renders (mip chains, upsample chains, depth read in the pass that writes it) | rendering.md "Mip-chain pass sampling the level it renders", "Upsample pass reading the level above...", "Depth sampled in the pass that writes it" | DKB (environment cube NaNs, bloom), Ocarina | missing: `VK_EXT_attachment_feedback_loop_layout` is probed (`vk_dev.c`) and not used | undefined in Vulkan; not seen on the test card (NVIDIA) |
| 7 | AV1 movies | movies.md "`nn::codec` video decoders taken over"; Ocarina README, Files | Ocarina of Time | missing: `host1x.c` serves H.264 and VP9 | black movie panels |
| 8 | Passthrough geometry draws gathered into one dispatch; mesh paths | shaders.md "Mesh geometry draws gathered into one dispatch", "Passthrough geometry shaders drawn by the vertex shader" | DKB (630 culling draws a frame) | not applicable as such (Vulkan has `VK_NV_geometry_shader_passthrough`, used here); the cost here is the per-draw write-back of each chunk's record and list | DKB's frame time (see GAMEPLAY_INVESTIGATION.md) |
| 9 | Occlusion conditions read once landed | rendering.md "Occlusion conditions read once landed" | PLZA, MK8D, DKB | partial (`vk_replay.c` `rc_value`: unknown for any address a report wrote) | culled draws are drawn: GPU time only |
| 10 | Reads past a uniform binding's size | shaders.md "Reads past a uniform binding's size" | all | partial: `robustBufferAccess`, on by default for NVIDIA only | undefined reads on other vendors |
| 11 | A recording's end and links in control memory | nvn-objects.md "A recording's end and links are in control memory" | Super Mario Bros. Wonder S2, Drag x Drive; DKB needed the call-copy part | missing (`hle_nvn.c`: control memory use always 0; `OP_CALL` keeps the handle) | a called list replayed after its memory was reused |
| 12 | GetInfo 31/32 (remote region use) | DKB README, Boot 2 | Switch 2 titles | partial: 29/30 answer 0, 31/32 warn | a warning line |

Checked and present here (no gap): conditional rendering evaluated at replay, depth-stencil views in STENCIL mode, min/max
reduction samplers, the base instance at c0+0x644, samples-passed queries, large sparse textures with shared physical tiles,
the Switch 2's device values and NVN2 entry points (ProgramBuilder, recording modes), task and mesh draws, passthrough
geometry shaders, `nvnCommandBufferFenceSync`, the NVN state getters, the game-chosen present interval, the pipeline manifest
and cache, depth mode zero, csrng, `GetAudioOutBufferCount`, the graphics scale, the Mii database stand-in, the Switch 2's
9 GB pool, the Tegra X1 GOB order for Switch 1 programs (`main.c` sets `NXRT_TEX_ORDER=tx1` for title IDs 01...).

Not applicable: what nxnative does for Metal (pools wrapped in chunks, MetalFX, pipeline archives, draw barriers) and its
cuts of SDK functions (this engine runs the SDK against its own services).

## Next, in order

1. Events across queues (rank 1): a submission's walk suspended at an unmet wait while another queue's submission is
   replayed (fibers), as `nvn_queue.inc` parks a queue.
2. Per-title profile keys (rank 3): `game_profile.h` as data looked up by title ID; turns on Ocarina's texture rewrites.
3. Counter report times (rank 2), then `game_dynamic_resolution` can default to on.
4. A newer translator (rank 4): needs the chain's Python front end moved with it, and every game's shader cache is made again.
