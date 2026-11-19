<!-- Copyright 2026 ChrisColeTech
     SPDX-License-Identifier: MIT -->
# MKW performance investigation, 2026-11-15

## Outcome

Implemented frame/stutter telemetry, a measured writeback optimization, and reproducible capture-free race tooling. Warm same-binary A/B/A reduced writeback completion cost by about 10%; a meaningful end-to-end FPS gain from that change alone is not established. The final integrated executable completed a fatal-free measured race through lap 2 at **58.398 presentations/s**, p95 **18.810 ms**, p99 **22.171 ms**, maximum **239.596 ms**. Stable minimum 60 FPS is **not achieved**. CPU-side streaming/replay and new-pipeline creation remain the measured blockers. Intermittent decoder flush failures also remain, documented below.

## Protocol

Target: sustained 60 presentations/s with no long race stalls, preserving the corrected compute launch metadata and image. This is a target, not an achieved result.

Read DEVELOPER_GUIDE.md, MKW_RENDERING_INVESTIGATION.md, BUILD_ENVIRONMENT_GUIDE.md and the native Vulkan implementation's timing and synchronization contracts. No applicable AGENTS.md was found. References in native source to PLAN_NATIVE_GPU.md and m5-vulkan.md are not present in this checkout.

1. Preserve the initial dirty tree and installed corrected shader cache. Audit all 153 compute modules again. Use .venv/Scripts/python.exe for all tools and builds.
2. Establish an installed-binary baseline using the existing template-matched Mushroom Cup / Mario / Standard Kart / 50cc route. Disable periodic and button-action captures. Capture only route checks and initial/final race checks. Exclusive scripted A holds drive the same bounded race segment; never stop a pre-existing process.
3. Separate startup/menu/loading from racing with explicit measurement boundaries. Report present-call completion deltas (not physical scanout), FPS, p50/p95/p99/max, intervals over 16.667/33/50/100 ms. Reject minimized or dropped-presentation intervals as throughput evidence.
4. Add opt-in bounded buffered CSV telemetry, reusing cumulative native counters. Report CPU present/acquire/fence/queue costs, replay intervals, completed GPU timestamps, pipeline creation, texture uploads, sparse binds, queue waits, pool copies and writeback/cache activity. No GPU timestamp waits or per-frame disk flushes. Asynchronous counter deltas are attribution evidence, not additive frame components.
5. Measure baseline with telemetry, identify the dominant actual stalls, and implement a reversible mechanism-based fix. Build NVN first, then runtime with explicit GCC and venv. Deploy only with no game session running; preserve executable and hash both copies.
6. Repeat comparable warm race segments, plus telemetry off/on. Preserve every run in a unique directory. State cache context, visual evidence, stopped owned PIDs, remaining blockers and uncertainty. No saves/keys resets, no quality reduction, skipped frames or artificial sleeps.

## Initial evidence

The previous exclusive race is `build/mkw-investigation/20261115-185844-race-exclusive`. It reached laps/results, but captured every five seconds and on each input, and contains minimized-window intervals. Its 259.67 and 472.41 ms present maxima therefore are not a clean performance baseline. Vulkan API validation was OFF; zero validation counters do not certify API validity.

Existing `vk_replay.c:slow_frame` already samples sparse-bind, GPU-wait, texture-upload, pipeline-create and pool-copy counters. `vk_present.c:stats` measures host completion of presentation calls. `subres_done` reads completed GPU queries without a wait flag. Pipeline cache binary persistence already uses a background saver; changing it without measured contention is not justified.

## Instrumentation

Set `SWR_FRAME_CSV` to a new writable filename before launch. The file is created exclusively, so an existing run cannot be overwritten. Two 4096-record single-producer rings collect replay and present samples. The writer runs below normal priority, buffers 64 KB, and flushes every 250 ms on its own thread. Each stream stops accepting records after one million samples; ring/limit losses and write errors are logged. Window close uses ExitProcess, so the final 250 ms may be absent. End measurement before the final screenshot and shutdown.

CSV fields:

- `kind=present`: interval since the previous completed presentation call, CPU wall time inside `present_one`, acquire/fence/queue-call wall time, age since enqueue, and cumulative dropped presents. These are not display scanout times. Minimized periods reset interval statistics rather than producing a false giant frame on restore.
- `kind=replay`: frame-boundary wall interval and cumulative counters. `cpu_ns` is wall time inside command replay, not thread CPU usage. GPU time comes from existing completed submission queries without WAIT. `wait_ns` sums existing waits across calling threads, so it can exceed a frame's elapsed time. `texture_ns` includes conversion and staging; `astc_ns` specifically measures successful ASTC pack reads and expansion. `writeback_ns` times completion-side writeback, and `writeback_bytes` is the existing byte counter. `cache_ns` covers the background pipeline-cache snapshot/save. Nested and concurrent fields must not be added together.
- Pipeline compilation, pool copies and sparse binds reuse the existing counters. Runtime logs also retain the existing actual translation-thread CPU-time reports and optional `NXRT_VK_PHASES=1` breakdown.

Every 300 actual presentation intervals the normal log now reports FPS, p95/p99/max and counts over 16.667, 33, 50 and 100 ms. The existing `vkp_stats_line` hook exposes that string without editing the platform backend. The strict 16.667 ms count includes normal vsync jitter; it is not a count of missed display refreshes.

`tools/mkw_perf.py report --report RUN` writes `summary.json` and a standalone `frametimes.html` chart. It uses measurement markers rather than combining menus/loading with racing. `summary-partial.json` is a live, incomplete report. `NXRT_VK_PHASES` is held equal across A/B runs because it has its own measurement cost.

## Measured writeback fix

The baseline writes back roughly 16 MB per race frame while changing about 170 KB. `wr_land` already skipped unchanged pages when updating guest memory, but its shadow pass then revisited every word under the pool lock. `wr_shadow` now resolves each page once and uses the same page comparison to skip unchanged shadow pages. Changed words retain the original per-word semantics; unchanged shadow words and CPU changes are not overwritten. `NXRT_VK_WB_SHADOW_SKIP=0` restores the original path for same-binary A/B.

The native function is compiled directly from `vk_mem.c` by `tests/test_mkw_perf.py`. Two hundred deterministic randomized cases compare old/new output with partial pages, null page entries and shadows different from both GPU snapshots. The 16 MiB scattered-change microbenchmark measured 3.762 ms old versus 2.898 ms new on this machine. Test source and DLL are preserved under `build/mkw-investigation/shadow-test-*`. The first test invocation failed to find compiler dependencies because its PATH lacked the GCC directory; the corrected test explicitly supplies that directory.

## Runs

All paths below are under `build/mkw-investigation`.

| Run | Context / result |
| --- | --- |
| `20261115-192547-installed-baseline` | Original installed binary, warm persisted caches, captures disabled in race. PID 30556 exited 0 naturally at 146.56 s before measurement; no harness stop. Incomplete, excluded from comparisons. |
| `20261115-192924-telemetry-baseline` | Telemetry and phase counters on, old shadow loop. PID 29744 stopped by the harness using WM_CLOSE, exit 0. 172.44 to 294.73 s measured, 6824 intervals, no drops or telemetry loss. 55.803 FPS; p50 16.874, p95 20.616, p99 24.678, max 678.718 ms. 4813 intervals >16.667 ms, 26 >33, 15 >50, 8 >100. |

Baseline per-frame counters: replay wall 12.544 ms, GPU 4.074 ms, writeback completion 6.103 ms, cumulative queue waits 2.635 ms, texture upload 0.193 ms, pipeline creation 0.143 ms. Present acquire averages 0.008 ms and queue calls 0.180 ms. The 678.679 ms replay spike at frame 12982 contains **644.808 ms pipeline creation**, not GPU saturation. Other >100 ms spikes include texture uploads / sparse binding and first-use pipeline creation. Persisted caches are warm but newly encountered pipelines still compile. Repeated warm runs and the same-binary old-path switch are required to avoid calling cache warming a code improvement.

Baseline initial/final visual checks: `0049-shot-settled.png` shows lap 1/3 on the grid after countdown; `0050-shot-settled.png` shows lap 2/3 with track, kart, lighting and shadows intact. The protocol is repeatable input, not identical simulation: opponents and wall-clock pacing can change the precise trajectory.

### Warm reversal and normal-mode results

| Run | Phase trace | Shadow skip | Frames | FPS | p50 / p95 / p99 / max ms | >33 / >50 / >100 ms | Writeback ms/frame |
| --- | --- | --- | ---: | ---: | --- | --- | ---: |
| `20261115-193955-wb-shadow-after2` | on | on | 6852 | 56.210 | 17.052 / 20.868 / 24.321 / 163.249 | 27 / 11 / 7 | 4.749 |
| `20261115-194435-wb-shadow-reverse` | on | off | 6867 | 56.253 | 17.069 / 20.510 / 22.915 / 156.558 | 26 / 11 / 4 | 5.290 |
| `20261115-195426-wb-shadow-repeat2` | on | on | 6881 | 56.469 | 16.838 / 20.081 / 22.327 / 576.527 | 21 / 11 / 6 | 4.756 |
| `20261115-195848-normal-telemetry-on` | off | on | 7117 | 58.344 | 16.723 / 18.656 / 21.944 / 331.419 | 24 / 11 / 7 | 4.817 |

The three warm diagnostic A/B/A runs use the same executable SHA256 `1443b49b957e83130460844166214f071bc892fb371f89938f10a1a5fa6c6d48`. No dropped presents, minimized intervals, measurement screenshots or lost telemetry were observed in these completed runs. Strict >16.667 ms counts are respectively 4882, 5096, 4722 and 4073. Every initial race HUD matched lap 1/3 with IoU 1.0. Final screenshots show lap 2/3 and the restored image. Initial/final names are in each `measurement.json`.

The repeat demonstrates about **10.1% lower writeback completion cost** against the warm old loop (5.290 to 4.756 ms). Its end-to-end FPS difference is only 0.216 FPS and is too small to separate confidently from run variation. The fix does not establish stable 60 FPS. Do not attribute the lower maximum of one run to this fix: first-use pipeline stalls still vary between runs.

Normal-mode counters average replay wall 11.178 ms, translation-thread CPU 11.317 ms, completed GPU work 4.245 ms, writeback completion 4.817 ms, and cumulative queue waits 1.662 ms/frame. About 14.50 MiB/frame is written back. Present acquire averages 0.008 ms, queue calls 0.172 ms. Detailed phase tracing itself costs about 1.2 ms of measured translation CPU in this scenario, which is why the normal-mode result is reported separately.

### Remaining blockers

- Foreground pipeline creation is the proven worst-stall callsite. In the normal-mode run, frame 9689 takes 331.329 ms, of which pipeline creation is 303.399 ms and completed GPU work 6.471 ms. Frame 7636 takes 212.835 ms with 189.092 ms pipeline creation. The existing prewarm covers previously listed variants; it cannot prewarm new variants not yet encountered. No completed cache-save time was recorded in either interval (`cache_ns` delta 0). This counter alone cannot exclude contention with a save still in progress; the pipeline measurement is driver-call wall time, not compiler-only CPU time.
- Texture streaming remains on the replay critical path. Normal-mode frame 8084 takes 163.900 ms: replay wall 159.458 ms, uploads 89.920 ms, sparse binds 11.933 ms, ASTC pack reads/expansion 56.112 ms, GPU work 4.330 ms. ASTC worker totals overlap upload time. These counters do not explain every millisecond: sparse-tile processing outside `upload()` and other replay work are not separately timed. Do not label that remainder as disk wait without another trace.
- The usual serialized replay plus GPU/completion dependencies also exceeds the 16.667 ms budget in parts of the course. GPU saturation and swapchain acquisition are not supported as the primary explanation by these measurements. The recorded phase samples locate substantial CPU cost in descriptor construction and memory synchronization; changing their reuse/invalidation rules needs a separate correctness investigation.
- These runs retain the existing shader, pipeline and ASTC caches. They are warm-persisted-cache runs with occasional newly encountered variants, not a controlled empty-cache benchmark. No cache, save or key reset was used.

### Excluded runs and harness corrections

- `20261115-193538-wb-shadow-after`, PID 21556: screenshot handshake timed out before racing. The log shows a screenshot written to `diagnostic.shot-001387.png` instead of the requested `0017-shot-settled.png`. The request reader consumed the control file even if opening/reading it failed. It now retries unsuccessful reads before consuming the request. WM_CLOSE, exit 0; no race measurement.
- `20261115-194857-wb-shadow-repeat`, PID 30732: acknowledged 250 ms L+R did not leave the title. The template gate correctly refused to treat the title as Single Player, timed out, and closed its owned process, exit 0. Route setup now holds L+R for one second and permits one further press only after visually re-matching the unchanged title label. Race input remains twelve 10-second A holds.
- `20261115-200310-normal-telemetry-off`: natural exit 1 at 77.08 s before measurement. The log records movie EOS followed by svcBreak, result `0xF201`, at `GetAv1DecoderOutputBufferProperties` / `movie::VideoDecoder::Flush` (73.90 s). This is distinct from the earlier SessionClosed teardown assertion. No decoder change or EOS shortcut was made; the same CSV-off configuration is repeated separately.

- `20261115-201012-normal-telemetry-off2`, PID 28852: the same guest decoder assertion at 55.91 s left a black screen. The initial lap gate refused measurement and closed this owned process at 135.94 s. IPC history shows command 8 flush, successful command 314 returns for images 0x15/0x16/0x17/0x18, then another 314 for 0x15 returning `0xF201`. Source inspection locates that result in `svc_ns2.c`'s already-queued image rejection. No decoder source was edited. This timing-sensitive failure is unresolved; the performance changes cannot be called universally stable based on the successful runs.

### CSV on/off comparison

Both detailed phase tracing and CSV were disabled in the following controls:

| Run | Shadow skip | Whole log windows | FPS over those windows | Largest logged interval | Translation CPU mean |
| --- | --- | ---: | ---: | ---: | ---: |
| `20261115-201556-normal-old-off` | off | 6900 frames | 57.508 | 472.11 ms | 11.761 ms |
| `20261115-202134-normal-telemetry-off3` | on | 6600 frames | 56.949 | 507.05 ms | 11.813 ms |

Both completed the twelve-hold race interval, reached lap 2/3, passed the initial lap mask at IoU 1.0 and shut down through WM_CLOSE. The optimized path therefore also completed with telemetry off; the two preceding decoder failures were not treated as benchmark results. One successful old-path control does not establish the decoder failure's independence from changed timing.

For the same optimized path and phase tracing off, the CSV-on run's whole-log-window rate was 58.244 FPS (6600 frames), versus 56.949 FPS off. The on-run was about 2.3% faster in this pair. This is **not evidence that telemetry improves performance** or a reliable upper bound on overhead: first-use pipeline work and different simulation trajectories dominate this difference. The on/off experiment is complete, but a precise telemetry overhead percentage remains unestablished. Native telemetry performs no game-thread disk writes or new GPU waits; its bounded-ring behavior recorded no dropped samples in the completed CSV runs.

## Reproduction commands

From `D:\Projects\Switch2RECOMP-main`, with no existing game session:

```powershell
.\.venv\Scripts\python.exe tools\build_nvn.py --jobs 8
.\.venv\Scripts\python.exe switchrecomp.py --build-runtime --target win64 --backend win32 --cc gcc\Library\bin\x86_64-w64-mingw32-gcc.exe
.\.venv\Scripts\python.exe tools\mkw_probe.py deploy perf-build
.\.venv\Scripts\python.exe tools\mkw_probe.py verify
.\.venv\Scripts\python.exe tools\mkw_perf.py warm-race --env 'SWR_FRAME_CSV={run}/frames.csv' --env 'NXRT_VK_PHASES=0'
.\.venv\Scripts\python.exe tools\mkw_perf.py report --report RUN_DIRECTORY
.\.venv\Scripts\python.exe tools\mkw_probe.py report RUN_DIRECTORY
```

`mkw_probe.py` checks for an existing game before launch/deployment, keeps a deployment backup, and only closes its Popen-owned process and verified same-executable startup descendants. `mkw_perf.py` uses a 480-second outer bound. The report command produces `summary.json` plus `frametimes.html`; the probe report includes live pacing/stutter log lines. To reproduce the old shadow loop, add `--env NXRT_VK_WB_SHADOW_SKIP=0`. For CSV-off use no `SWR_FRAME_CSV` in the parent environment or arguments. Avoid `NXRT_VK_PHASES=1` for normal playback; it is a separate detailed diagnostic.

Checks used:

```powershell
.\.venv\Scripts\python.exe tests\test_mkw_perf.py -v
.\.venv\Scripts\python.exe tools\mkw_compute_audit.py --validate
git diff --check
```

The final compute audit covers 166 cached modules with available control headers: all launch dimensions match and all pass standalone SPIR-V validation. Vulkan API validation remains OFF. This pass builds and executes the Windows Win32 runtime; Linux targets were not rebuilt here. No Linux project venv was found, and no system/bundled interpreter fallback was silently selected.

## Owned changes

- `runtime/nvn/vk/vk_perf.c`, `vk_perf.h`: bounded opt-in telemetry transport and writer.
- `runtime/nvn/vk/gfx_vk.c`, `vk_present.c`, `vk_replay.c`: initialization, present/replay samples, pacing summaries and minimized-period reset.
- `runtime/nvn/vk/vk_mem.c`: writeback timing and page-wise shadow update.
- `runtime/nvn/vk/vk_astc.cpp`, `vk_pcache.c`: expose ASTC pack timing and cache-save timing.
- `runtime/nvn/vk/vk_shot.c`: retain screenshot requests across failed control-file reads.
- `tools/mkw_probe.py`, `tools/mkw_perf.py`: capture-free measurement, bounded scripted race, phase markers, reports, provenance and deployment verification.
- `tests/test_mkw_perf.py`, this document and `CHANGELOG.txt`.

Pre-existing rendering changes and concurrent controller, launcher and interpreter-selection work were preserved. No edits to `backend_win32.c`, controller code, launcher/library or Python selection were made by this performance task. No commit or push.

## Final integration and process ownership

Rebuilt NVN first and then the Win32 runtime again after the concurrent runtime/controller edits. Source `bin/runtime_win64_win32.exe` and deployed `build/mkw-nsp-1.8/Mario Kart World 180.exe` both hash to **`7d60a2f811d32d9eb90b79b731782763c87b74dfe4a3dbd95532e01f84aa0679`**. Deployment backup: `20261115-203011-deploy-perf-final-integrated`.

`20261115-203011-final-integrated` reached lap 2 with readable kart, track, lighting and shadows. Its measured present rate is 58.631/s, p50 16.710, p95 18.566, p99 21.462, max 167.616 ms; 18/11/5 intervals exceed 33/50/100 ms. **This is diagnostic evidence, not a clean stability pass:** the log contains the decoder svcBreak at 74.10 s even though other game threads continued to race.

The final run exposed a cleanup gap in the earlier probe. `plat_identity_early` can restart the executable when ASLR prevents reserving the guest address range. Popen-owned PID 3636 was a waiting wrapper; its game was PID 15156, created 0.020 s after launch with parent 3636 and the same executable/command line. The old harness sent WM_CLOSE only to 3636, timed out, and terminated that wrapper, leaving the actual game alive. The child was then explicitly verified by parent, executable path and creation time through an opened process handle and closed by WM_CLOSE, exit 0. Evidence: `owned-child-15156.json`. No unrelated user process was stopped.

The probe now discovers only same-executable descendants created within the owned startup window, verifies their creation time again through held process handles, and closes leaf processes before the wrapper. It records `owned-processes.json`. Tests reject unrelated parent IDs, late/old creations and different executables, and accept an out-of-order two-level owned chain. The separate `recover-child RUN PID` command applies the same direct-parent/path/startup-time checks for recovery of an earlier run.

`20261115-204031-cleanup-smoke`: PID 26736, 12-second bound, WM_CLOSE exit 0, no game process left afterward. Both process-ownership and 200-case shadow-equivalence tests pass. Latest shadow microbenchmark: 3.275 ms old, 2.478 ms new. This smoke did not encounter an ASLR relaunch; the actual child-handle recovery above and the ownership fixture cover that path. Summary reports now include fatal diagnostics and shutdown metadata, and automated comparison sequences stop if a run logs a fatal error.

### Final clean verification

`20261115-204319-final-integrated-repeat` uses the same final `7d60a2f8...` binary, CSV enabled and detailed phase tracing disabled. All eight menu states matched and the initial lap 1/3 mask scored 1.0. Measurement is 177.433 to 299.316 s, 7118 present intervals. Results:

- 58.398 presentations/s; mean 17.124 ms, p50 16.717, p95 18.810, p99 22.171, maximum 239.596 ms.
- 4193 intervals >16.667 ms, 19 >33 ms, 10 >50 ms, 4 >100 ms. Zero dropped presentations, minimized intervals, measurement captures or telemetry losses.
- Replay wall 11.235 ms/frame, translation-thread CPU 11.393 ms, GPU 4.110 ms, writeback completion 4.953 ms. 14.487 MiB/frame written back. No foreground pipeline creation or completed pipeline-cache saves during this measured segment.
- Worst replay frame 9489: 239.613 ms interval, 235.835 ms replay wall, 83.810 ms ordinary texture upload, 11.383 ms sparse binds, 53.155 ms ASTC pack read/expansion, and only 3.127 ms completed GPU work. ASTC overlaps upload/worker time. The residual replay time is not yet separately attributed. The measured upload alone exceeds the frame budget fivefold.
- `0053-shot-settled.png`: readable grid, Mario and kart, track, sky and shadows, lap 1/3 after countdown. `0054-shot-settled.png`: live lap 2/3, item effects and opponents, restored scene intact. These are the only captures bounding the measured segment.
- No svcBreak/FATAL/device-loss diagnostic in this run. PID 29908 closed by WM_CLOSE, exit 0 at 301.25 s. No game process remained. The final source/deployed hashes match.

Final evidence: `summary.json`, `frames.csv`, `frametimes.html`, `measurement.json`, `actions.jsonl`, `provenance.json`, `observations.json`, `owned-processes.json` and the initial/final PNGs in that directory. The repeat verifies this bounded race segment, not a universal crash-free or full-cup claim.

### Exact stopped PIDs

WM_CLOSE was used for owned game PIDs **29744, 21556, 6460, 21788, 30732, 18680, 21576, 28852, 25860, 26848, 15156, 26736, 29908**. Owned ASLR-wrapper PID **3636** was terminated after its original WM_CLOSE timeout; its verified game child 15156 was subsequently recovered and closed as described above. PIDs **30556** (exit 0) and **30504** (exit 1) exited without a harness stop. Supervisor/helper processes exited normally; no unrelated user process was stopped.
