<!-- SwitchRecomp - Copyright 2026 MZ Labs
     Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
     Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. -->
# MKW rendering investigation, 2026-11-15

## Baseline and acceptance

Starting branch `feature/portable-release-bundles`, HEAD `f9c8469`, clean working tree. No commits or pushes in this investigation. The root developer guide was read; no applicable root/ancestor AGENTS.md exists. Eglantine's AGENTS.md and relocated policies under `docs/content/policies/` were read before reference use. Its files and git state were not modified.

Observed in preserved `build/mkw-investigation/run.log`: runtime 0.4.1; deployed `build/mkw-nsp-1.8/tools/nxshader/nxs_live.py`; RTX 5090 driver 616.92; Vulkan validation explicitly OFF. Preserved `press-shots/press-0007-key-20.png` shows the logo and L+R prompt over a nearly black 3D scene with repeated bright circles. This is a rendering failure, not a startup black screen. The existing temporary harness deletes prior evidence, posts focus-dependent keys, and waits for request deletion rather than PNG completion. It cannot establish race progress.

Success requires readable title/background, menus, character/course selection, race grid, countdown, and visible movement after countdown. Full-race completion must be separately evidenced. Free Roam and presentation counts are not evidence of a race. Physical controller inputs cannot be claimed from host detection or scripted input.

## Experiments and decision conditions

1. Audit executable/translator hashes, effective paths, cache generation and installed Vulkan tools. Preserve old logs and caches. Fail provenance if the deployed translator differs without explanation or generated SPIR-V cannot be traced to effective source.
2. Replace the temporary harness with a bounded repository tool using acknowledged runtime input and screenshot requests. One action at a time, screenshot on every action plus settled and periodic images, unique run directories, timestamped manifests. Advance only after inspecting the actual scene. Stop only the PID it launches.
3. Audit integer fragment outputs against actual shader instructions and reference semantics. Hypotheses: float conversion loses untyped register bit patterns; scalar access-chain types/loads are invalid; variant key aliases target 3. Validate generated SPIR-V for whole-vector and scalar access, readback, signed/unsigned and mixed outputs. A disappearing warning alone fails visual acceptance.
4. Audit 3D image views against enabled extension features, view usage, mip/layer ranges, and failures. Compare with real device capabilities. Fix only demonstrated invalid cases, then build NVN before relinking the runtime.
5. Run an unchanged baseline and controlled fix comparison at the same title/menu state. If corruption remains, compare async submission ON/OFF and capture a pass dump to locate the first bad resource/pass. No output clamps, render suppression or synthetic movie EOS.
6. Verify WinMM shoulder/stick-click mapping against the HID bit contract with executable mapping checks. Leave physical response explicitly unverified unless observed from real input.
7. Navigate through actual menus into a race, capturing every action, grid, countdown, movement and periodic race progress. If blocked, record the exact state, elapsed time, exit code/stack and next probe. Exercise the real GSV movie decoder and teardown if reachable, and distinguish natural exits from harness termination.

## Results

All evidence below is under `build/mkw-investigation/`. Prior `run.log`, `shots/` and `press-shots/` are preserved.

### Baseline and integer-output correction

- `20261115-174337-baseline`: unchanged executable SHA256 `d49f576487eebc83c9aadbb8759b435a5b8c14bd254a80408bb016124037c199`; source/deployed translator Python and DLL hashes match (full inventory in `provenance.json`). No FragmentFinite pass in either current copy. The selected shader metadata lists only `caps_trim v1`; no cache was deleted. `0006-periodic.png` reproduces the black title at about 31 seconds. Bounded stop at 35 seconds, WM_CLOSE exit 0.
- The actual cached fragment shader `dcb0504c844f1ecc` writes a scalar through an access chain into a float vec4 output. The old runtime pass assigns pointer type ID 0 because it only creates an integer scalar pointer for scalar output variables. Its definition map also omits access chains. `tests-20261115-174728/case-2.validation.txt` proves the old result is invalid (`Type Id is 0`). Both constructed mixed-output cases also fail. Earlier test harness compile attempts are preserved separately.
- Fixed scalar pointer declaration, access-chain definition lookup, bit-preserving stores, reverse bitcasts for output loads, and fragment variant cache key (bit 8 previously collided with target 3). Reference: Eglantine `emit_spirv_context_get_set.cpp:485-497` uses OpBitcast for integer outputs. Its policies were located and read under `docs/content/policies/*.mdx`; no Eglantine files were changed.
- `tests-20261115-174740-841343300`: actual cached shader and two signed/unsigned mixed vector/scalar/readback modules pass `spirv-val --target-env vulkan1.3`. Compiled WinMM mapping tests pass. L1/R1 are bits 6/7, stick clicks 4/5; physical controller response remains unverified.
- Built `libnvn.a` first, then Win32 runtime. Deployment backup `20261115-174741-deploy-int-output`; deployed hash `17b64373dadcba683e01a5cc343d479caea7706b887ef756db5acb886ae0fd69` matches the built binary.

### First interactive run and reproduced decoder teardown abort

`20261115-174741-int-output` uses acknowledged, focus-independent HID masks from `tools/mkw_probe.py`. Every button has press and settled PNGs, plus five-second periodic PNGs. `actions.jsonl` records elapsed time and the screen inspected before each action. Runtime acknowledgement also appears in the game log.

| Evidence | Observation |
| --- | --- |
| `0006-shot-settled.png` | Title still black with repeating light circles. Integer-output fix alone does not fix the scene. |
| `0010-lr-settled.png` | Main menu, Single Player highlighted. |
| `0014-a-settled.png` | Grand Prix highlighted. |
| `0019-a-settled.png` | Mario character selection readable. |
| `0024-a-settled.png` | Standard Kart selection readable. |
| `0038-periodic.png`, `0040-periodic.png` | Actual decoded movie scenes (Mario Bros. Circuit, DK course), not live race evidence. |

The decoder produced 249 output images including EOS, with 1175 queued frames dropped. The bounded dropping path is now exercised, but this pacing is poor and is not a smooth-playback success. At 177.15 seconds the game aborts in `ShimLibraryGlobal::DestroyVideoDecService`, return PC `c5159b8`, after decoder command 115. Exit code 1, not harness termination. No race grid/countdown/movement reached.

The crash instructions at `c388738` write TIPC request type 15, call SendSyncRequest, and compare its result to `0xF601` (SessionClosed). `ipc_handle` returns 0 for type 2/15 instead. This causes the SDK's teardown assertion even though finalize returned success. Eglantine's `service.cpp` also closes these requests without a normal reply. Fix: return SessionClosed while leaving the client handle for the SDK to close. Next experiment: repeat the exact menu route and observe movie completion/course selection.

### Vulkan view audit

`vulkaninfo` reports only validation layer 1.3.268 installed; runtime validation remains off. Standalone SPIR-V validation is distinct from Vulkan API validation. The RTX 5090 supports `image2DViewOf3D` and `sampler2DViewOf3D`, but the baseline runtime enabled neither. Khronos `VkDescriptorImageInfo` VUID 07796 requires `2D_VIEW_COMPATIBLE_BIT_EXT` for sampled 2D slices; VUID 06712 prohibits 2D-array views of 3D images in descriptors. The broad 3D conversion guard was insufficient. The targeted correction and limits of its testing are recorded below.

### IPC-close verification and race execution

`20261115-175321-ipc-close`, runtime SHA256 `3d90b58902d8c39df5313cd1f3639ba81d678efa5f3d1c907cf9534429385ebc`:

- `0076-shot-settled.png`: Mushroom Cup selection after the movie, with no teardown abort.
- `0084-a-settled.png`: 50cc selection; `0094-a-settled.png`: Start Grand Prix confirmation.
- `0115-shot-settled.png`: Mario Bros. Circuit loading map; `0128-shot-settled.png`: decoded cup movie (not a race).
- `0144-periodic.png`: grid lineup, heavily obscured by black rendering. `0147-periodic.png`: countdown 2 visible through corrupt bloom. `0148-periodic.png`: Lakitu's green start lights.
- `0168-shot-settled.png`: lap 1/3, 24th, Mario at the start on the minimap. `0179-a-settled.png`: after an acknowledged 10-second A hold, Mario has moved beyond the start line on the minimap. The scene itself remains too corrupt to verify a properly rendered moving kart. No full lap or race completion claimed.
- Log at 522.25 seconds: decoder service/session close type 15 returns SessionClosed; no svcBreak. The real bounded decoder/drop/EOS path ran. Movies still drop many frames (1098 dropped, 326 outputs in the logged second session).
- Bounded 900-second run ended by WM_CLOSE, exit 0. Presentation rate around 32/s, p95 often 57-88 ms. This is not stable 60fps.

### Graphics A/B and pass captures

- Implemented 3D slice extension/feature enabling and image creation flag, restricted dimensional conversions to supported 2D sampled slices, corrected layer clamp order and one-mip slice views, and removed the incompatible-original-view fallback on creation failure. `20261115-180956-view-features` logs both features enabled. Title remains corrupt (`0006-periodic.png`); no sampled-slice diagnostic appeared in this title run, so this path is not established as the black-scene cause.
- That run captures frame 665 (raw pass files use 666). Unique diagnostic prefix keeps dumps inside the run. `pass-report-utf8/stats.json` and previews are generated by `tools/mkw_pass_report.py`; the first report's text extraction failed on Windows encoding and was rerun into a new directory. Raw files were never modified. Pass 63 already contains repeated lights; pass 84 shows the nearly black scene. All sampled RGB values in this capture are finite; this does not justify a global NaN clamp.
- `20261115-181320-async-off`: same binary and translator, `SWR_ASYNC_SUBMIT=0`, still corrupt (`0006-periodic.png`). Capture frame 702 (raw files use 703) has 3363 nonfinite sampled RGB components in pass 71, target ser51754 RGBA16F, shader `a403174be00ce2d4`. It samples the 64x36x256 volume ser34530. This differs from the async-on capture, so nonfinite values are possible, but async disabling alone is not a fix. Both dump runs hit the harness's 15-second screenshot timeout while bulk readbacks ran, then stopped their own PID via WM_CLOSE, exit 0.
- `20261115-181902-sparse-off`: full backing (`NXRT_VK_SPARSE=0`) did not reach the title within its 55-second bound. Loading-map images alone make this experiment inconclusive.
- All four runtime builds passed: Win32, Win64 headless, Linux X11, Linux headless. Linux NVN was built first under WSL Ubuntu. No game test was run on Linux.

### Proven compute launch metadata loss

The pass traces log **every compute dispatch as local 1x1x1**. `tools/mkw_compute_audit.py` compared all 153 cached compute modules to their preserved NVN2 controls. 146 disagree: 71 request 32x1x1, 36 request 64x1x1, 26 request 8x8x1, and 13 request other larger shapes. Seven legitimately request 1x1x1.

`nxs_front.job_for` passed NVN2 register metadata but omitted `local` and `shared` unless an external facts table supplied them. Live dumps have no facts. The control header holds dimensions at 0x58/0x5c/0x60, shared bytes at 0x64. The translator consequently emitted `OpExecutionMode LocalSize 1 1 1` and omitted the requested shared allocation. This is a demonstrated upstream compute defect consistent with sparsely initialized lighting volumes.

Fix: supply header-derived launch dimensions and shared bytes, include them in the job digest, and advance front-end generation to 3. The live helper now checks chain/completeness instead of treating every JSON filename as done; otherwise a generation change leaves stale shaders permanently unrebuilt. Tests cover stale, missing, malformed and partial publications, local/shared metadata, and changed job digest.

Preserved the original cache and copied it to `20261115-182644-compute-cache/cache`. First regeneration attempt failed because the diagnostic script assumed a bundled dotnet.exe; corrected discovery to the installed `C:\Program Files\dotnet\dotnet.exe`. The subsequent `nxs_live.py --once` completed successfully using existing inner-cache hits for unchanged shaders. All 153 compute modules now match header dimensions and pass standalone spirv-val (zero failures, `compute-validation-*.json`). `20261115-183141-compute-geometry` explicitly selects this isolated cache and the deployed corrected translator; its visual results follow.

## Final verification and installed state

### Visual result of compute metadata correction

`20261115-183141-compute-geometry/0017-shot-settled.png` shows Mario, kart, road, traffic, terrain, sky and shadows instead of the black scene and repeated light circles. `0031-lr-settled.png`, `0043-a-settled.png`, `0057-a-settled.png`, and `0071-a-settled.png` verify main menu, mode selection, Mario and Standard Kart with the restored backdrop. This is the decisive visual A/B: same runtime as the prior corrupt view-feature run, with corrected compute jobs in the explicitly selected cache. Some triangular terrain defects remain visible in `0210-shot-settled.png` and `0223-shot-settled.png`. The 900-second bound ended that run by WM_CLOSE, exit 0.

Installed the regenerated cache at the game's normal path. The entire original cache was moved intact to `20261115-185005-original-cache`. The isolated regeneration cache also remains. No source NSP, keys, library or save data was externally edited or reset; normal in-game progression may save its own state.

`20261115-185010-race-final` verified the normal installed cache and template-matched all eight menu states (label-mask IoU 0.897-1.0). It reached live racing, but ended at 212.37 seconds with exit 0 without a harness stop or logged crash. Input from the host was still allowed in that run; movement there cannot be attributed exclusively to scripting. The next run explicitly excludes host gameplay inputs so this ambiguity is not used as evidence.

### Exclusive-input race test

`20261115-185844-race-exclusive` used the normal game cache, default async and sparse settings, and final deployed runtime SHA256 **`073c11e42b1e5fd7ff184a544a3e932dd0c04728cbc4f4141dbeb648dec93fd3`**. `SWR_INPUT_EXCLUSIVE=1` applies only with `SWR_INPUT_FILE`; ordinary launches retain normal keyboard/controller input. No physical controller presses are claimed.

The bounded `route` command compared visible UI label masks to previously inspected screenshots, required IoU >=0.72 before each action, and stopped on a state timeout. All eight states matched. The route specification is preserved as `race-route.json`; it names the evidence templates and regions, so there is no blind fixed menu schedule. Each button has a press PNG and settled PNG; ten-second holds also have a mid-hold PNG. Five-second periodic captures continue between actions. `actions.jsonl` contains elapsed times and observations before actions; `observations.json` records the final visual review.

| Screenshot | Observed milestone |
| --- | --- |
| `0008-shot-settled.png` | Title L+R matched, readable scene. |
| `0011-shot-settled.png` | Single Player selection. |
| `0014-shot-settled.png` | Grand Prix selection. |
| `0017-shot-settled.png` | Mario character selection. |
| `0020-shot-settled.png` | Standard Kart selection. |
| `0023-shot-settled.png` | Mushroom Cup selection. |
| `0026-shot-settled.png` | 50cc selection. |
| `0039-shot-settled.png` | Start Grand Prix confirmation. |
| `0080-shot-settled.png` | Visible grid, racers, Lakitu, red light and countdown. |
| `0106-shot-settled.png` | Countdown has ended; Mario stationary on the grid, lap 1/3, opponents have left. |
| `0126-a-settled.png` | After exclusive scripted A for 10 seconds: Mario is driving at the first curve, past the start. Kart, track and shadows readable. |
| `0157-a-settled.png` | Farther around the course, elevated dirt section, items and opponents rendered. |
| `0190-a-settled.png` | Lap 2/3. |
| `0221-a-settled.png` | Lap 3/3. |
| `0238-periodic.png` | Race results: Mario 24th, 1 point. |
| `0243-a-settled.png` | Post-race Next Race / View Replay / Quit menu. |
| `0246-a-settled.png` | 2nd Race grid. |
| `0249-a-settled.png` | Second-race countdown 1. |

This verifies the first race's lifecycle through results and transition into the next race, with scripted movement and both lap increments. It does **not** prove Mario personally crossed the final finish line: the game awarded last place while the test had long no-input review intervals, so the normal last-place timeout may have ended the race. It is not a four-race cup completion or a competitive driving test. Smart steering was already active (visible antenna); this test did not change that preference.

Final run reached its 900-second bound, shut down through WM_CLOSE and exited 0. No svcBreak/device-loss crash in this run. Process check after completion found no MKW/runtime game process left running.

### Checks and remaining defects

- All four runtimes build successfully. The final Win32-only exclusive-input addition was rebuilt and exercised in the final run; other targets are unaffected by that addition.
- `tests/test_mkw_runtime.py`: old integer-output pass fails on the real MKW shader and mixed fixtures; corrected pass validates all three, preserves register bits, and supports output load round-trip. WinMM button mapping and target-3 cache-key distinction pass. Latest evidence: `tests-20261115-191550-225558700`.
- `python -m unittest discover -s tests -p test_nxs_parent.py -v`: 3 tests pass (parent lifetime, stale/partial cache detection, compute launch metadata and digest).
- All 153 regenerated compute modules pass standalone SPIR-V validation. Vulkan API validation was **not enabled**, so validation-error counters do not certify API correctness.
- `git diff --check` passes. No commit or push. All original evidence retained.
- Final race often presents near 60fps at the logged 1280x720 window: at 444.75 seconds, 60.00/s, p95 16.94 ms, p99 17.25 ms. This is not stable 60fps across the run: at 521.56 seconds, 55.94/s, p99 52.04 ms, max 259.67 ms; at 662.29 seconds, 54.13/s, max 472.41 ms. Screenshot readbacks add overhead. The log also records several window-minimized intervals with dropped presentations; those must not be treated as GPU rendering throughput.
- The severe black-scene defect is fixed in the tested title/menu/race route. Triangular terrain gaps and some geometry/texture irregularities remain, especially in the course intro and distant track terrain. No general visual-perfect claim.
- Physical controller response still requires a human input test. The code-level WinMM L/R and stick-click swap is fixed and tested; host detection alone was not used as proof.
- Decoder/drop/EOS/teardown were exercised with real movie frames. Large decoder frame-drop counts remain a pacing issue. No synthetic EOS shortcut was added.

### Next concrete probes

1. Capture the corrected race's first terrain draw with missing triangles, including vertex/index buffers and compute-produced indirect draw records. Compare the depth prepass and material draw for that mesh, then validate indirect counts and base-vertex values against the source header; do not alter culling globally.
2. Trace the long GPU-wait callsite `exe+4dd2b` through `vk_mem` writeback/submit waits during a >100 ms race interval. Use the existing pass GPU timings to distinguish actual GPU execution from host polling/fence latency; report p95/p99 over active, non-minimized racing only.
3. Replace decoder's drop-heavy pacing with protocol-correct submission backpressure after comparing submission-ready event consumption and release fences. Retain the verified SessionClosed fix. Re-test real movie EOS and repeated create/finalize cycles.
4. Exercise sampled 2D-of-3D views under a modern validation layer in a targeted GPU test, including nonzero slice/mip. The current installed 1.3.268 layer is not a valid substitute for that test.
