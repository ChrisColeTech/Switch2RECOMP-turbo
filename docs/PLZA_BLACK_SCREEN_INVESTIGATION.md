<!-- SwitchRecomp - Copyright 2026 MZ Labs
     Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
     Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. -->
# Pokemon Legends Z-A (Switch 2 Edition): black screen at boot

Status 2026-11-16: **not fixed.** Pokemon Legends Z-A Switch 2 Edition (`0100F43008C44000`, built by RecompLauncher
with turbo a9a4add) opens a black window with working audio and never renders. The cause is a concurrency failure
in the game's own job/resource system under this runtime: loading jobs stall partway and every thread goes to sleep.
The evidence points at threads that share a guest core running at the same time on the PC (on the Switch they
never do). Two engine changes were made and committed on branch `plza-blackscreen` (not merged, not pushed); neither
makes the game render yet. See [Open items](#open-items) for what is left.

## Symptom

- The window stays black. Audio starts (`audout: state=started`, samples advancing) and the process keeps running.
- The Vulkan statistics never move: `[vk] textures: 0 images, 0 MB ... 0 uploads`, render targets 0, device memory
  ~150 of 31419 MB. `warning: 6 s without a new image (frames=0, GPU submits=0)` from 7.5 s on.
- The process is idle: about 0.1 CPU-seconds in 5 s once it has stalled. Nothing spins.
- The renderer is not the cause: with `renderer=opengl` (the SDK's own NVN driver instead of the NVN takeover layer)
  it stalls the same way (`gpu: frames=0 submits=1 draws=0`).

## What the game is waiting for

Main always ends up waiting for an asynchronous resource load to finish, and the load never finishes. Where exactly
changes from run to run:

| Run | Main thread (`threads.txt`) | What it waits for |
| --- | --- | --- |
| owner, log=1 | condvar wait, stack `9a84f84 8037a14 8034c24 97df91c` (after `bcat:u`, `acc:u0`, savedata probes) | a resource future |
| copy, log=2 | condvar wait, stack `9a84f84 8037a14 8ecb6b0 8ec8908 ...` | a resource future |
| copy, log=1 | `looping on SleepThread` from `lr=8bfdec8`, 16 ms sleeps | a dependency list of fonts and resources |

The wait functions read the same object type. `F_9A84D50` (called through `F_8037A04`, "wait for resource") returns when
the resource's state at `+0x158` is 5. The loop at `8bfdec8` sleeps 16 ms and calls `F_9288690`, a state machine at
`obj+484`. Its state 3 walks a list at `obj+136`, and every entry must pass `F_9291850`: either a resource whose
loaded bit (`res+440`, bit 0) is set, or a shared font whose `pl:u` GetLoadState is 1. The four `pl:u` cmd 1 calls
per 16 ms (118,000 in 80 s in the verbose log) are that check for fonts 1-4. The runtime answers Loaded, and fonts
are not the problem.

Read from the live process (guest memory is identity-mapped: guest address == host address), the list held 20
entries. 4 shared fonts were fine, 7 resources were loaded (state 5), and 8 resources were stuck in state 1 or 2:

| Stuck (state 1/2) | Loaded (state 5) |
| --- | --- |
| `data:/ui/font/bin/font_din_24_28.bffnt`, `font_din_26_30.bffnt`, `font_din_52_64_bold.bffnt`, `font_din_num.bffnt`, `FOT-RodinNTLGPro-B.bfotf`, `FOT-RodinNTLGPro-DB.bfotf`, `FOT-UDKakugoC80Pro-B.bfotf`, `FOT-UDKakugoC80Pro-DB.bfotf` | `UniversNextPro-Cond.bfotf`, `UniversNextPro-BoldCond.bfotf`, `or_font_eng.bfotf`, `font_din_26_30_bold.bffnt`, `font_din_30_34_bold.bffnt`, ... |

Both file types are in both columns, so the format doesn't matter. The batch was processed partway, then stopped.

## Ruled out (with evidence)

- **The dump, the NUP and reading in place.** `data.trpfs` is 4.4 GB, but `nup_pread`, `read_full`, `plat_pread`
  (OVERLAPPED offset high/low) and `romfs_read` are 64-bit throughout. All 605 reads finish by 2.7 s and there is
  no `RomFS: NUP read failed`.
- **Missing services or stubs.** The distinct IPC command list of a verbose run contains nothing unhandled. The only
  `[stub]` is `acc:u0 cmd 160`, and there is no NVN `[RED]` stub line. `bcat` answers like an offline console.
- **The shader cache.** The `[RED] shader ... is not in the shader cache` lines appear only in runs that got
  further, and `nxs_live` translates those shaders within a second (`39 translated in 1.00 s`).
- **Thread 95 exiting.** One of three workers (93/94/95, same entry) exits at ~3.2 s in every run. A temporary log
  in both exit paths showed `svcExitThread lr=e2c6abc` (the SDK thread wrapper). It is not a branch to null. The
  game's thread routine `F_9B560F0` loops `do body(); while(!(this+568 & 1))`, so the game asked it to stop.
- **Lost wakeups in the kernel primitives.** In the stalled process, the counted `nn::os::Semaphore`s
  (`e2be83c`: count at `+20`, mutex `+28`, cv key `+32`), the `LightEvent` (state 1, waiters) and the job
  scheduler's event count (`F_9CFE0F0` / `F_9CFE050`) were all consistent: counts 0, mutexes free, keys 1. No
  thread slept with its condition met. Note: the threads.txt `condvar [addr X]` is the **mutex** address; the key
  is at `X+4` (`svc 0x1C(mutex, key, tag, timeout)`). Signal (`e2c78c0`) and Broadcast (`e2c78d4`) in the SDK do
  nothing when the key reads 0, so a key cleared while waiters remain would lose wakeups; it was 1 every time.
- **SignalToAddress type 2.** `svc_signal_addr` matches Horizon's SignalAndModifyByWaitingCountIfEqual
  (no waiters +1, all woken -1, some left unchanged). Mario Kart World uses it too.

## Why it is a concurrency failure

| Experiment (old DLL unless noted) | Result |
| --- | --- |
| Normal, several runs | stalls at a different point each time, 0 frames |
| Whole process on one host CPU (`start "" /affinity 1 <Game>.exe`) | gets past the resource stall: 56 textures (167 MB), 4 frames presented, bcat/savedata reached, then stalls later with main in the job scheduler (`9cfe14c 9cfd9ec 9cfb804`) |
| Each guest core pinned to one host CPU (temporary `SWR_CORE_AFFINITY=1`, removed) | past the resource stall, 56 textures, 1 frame, then the same later stall |
| Recompiled with atomic LDAR/STLR (commit `efa9b2a`, see below) | no change: 0 frames, same resource wait |
| One-thread-per-core scheduler (`SWR_CORE_SCHED=1`, commit `ade68bf`) | no change yet: thread 95 holds core 0 (below) |

Serializing threads changes the outcome a lot, so this is a race and not missing functionality. The job system
(`F_9CFD6F0` and friends) keeps a work queue per core. The index comes from the thread's core (`+3092`, set through
GetCurrentProcessorNumber), and the owner side of those queues is not atomic. On the Switch, threads that share a
core never execute at the same moment, and a running thread is only displaced by a better priority. Here all the
"core 0" threads (main 80, 82-92, 95, 99-101, 105-108, ...) run in parallel. `kernel.c` already documents the same
class of problem for `sead::ExpHeap` ("threads that share a core on the switch run at once here"), but PLZA's
Trinity engine doesn't use sead, so that lock doesn't cover it.

## What was changed (branch `plza-blackscreen`)

1. `efa9b2a` **Translate LDAR/LDAPR/STLR and LDXR as atomic accesses** (`a64.py`, `runtime/cpu.h`).
   LDAR was emitted as `*(volatile T*)MP(a)` and STLR as `{ WR(a,v); SWR_FENCE(); }`. A volatile access does not
   order plain accesses, so GCC may reuse a load from before an LDAR or schedule a store after an STLR. `cpu.h`
   had `LDAR_`/`STLR_` for this all along, but the generator never used them. PLZA's code has 1,594 STLR and
   ~33,000 LDAR sites. It is a real correctness fix, but on its own it does **not** fix PLZA. It only takes effect
   when a game is recompiled. Not regression-tested on other games.
2. `ade68bf` **Experimental one-thread-per-core scheduler**, off unless `SWR_CORE_SCHED=1` (`runtime/kernel.c`,
   `runtime/plat.c`, `runtime/rt.h`). Each guest core has a token, and a guest thread runs game code only while it
   holds its core's token. It gives the token up in blocking SVCs (`0A 0B 18 1A 1C 21 22 23 34 43 44`), and to a
   better-priority waiter at its next SVC. On Windows a better-priority waiter also preempts a holder at once if the
   holder's host PC is inside the `_code` module (`SuspendThread`; never inside runtime code, which could hold a
   lock), and the holder resumes when the core is free for it. A holder that keeps the core for `SWR_CORE_SPIN_MS`
   (default 500) without a blocking SVC is overtaken, with a log line. The ExpHeap HLE gives the token up while it
   waits for its lock. Linux has no preemption (stubs).

## Where the scheduler gets stuck

With `SWR_CORE_SCHED=1`, preemption works (`kernel: thread 80 preempted thread 82 on core 0`). But thread 95
(priority 44, core 0) then runs for more than 10 s without a blocking SVC, in
`while (obj->state(+80) != 4) F_816F5C4(obj);` (`F_816F55C`, pc `816f584`). Equal-priority core-0 threads (main
80, 92) and worse ones (89) wait for the overtake time. The log shows
`kernel: thread 80 ran past thread 95, which kept core 0 for 10.0 s without an SVC`. With 0.5 s the overtakes
reintroduce the parallelism and it stalls as before. Either thread 95 yields (svcSleepThread 0/-1) and the token
does not go to equal priorities yet (no FIFO hand-off), or it polls for work that a same-core thread produces.
Which one is not established.

## Switches and diagnostics

| Switch | Effect |
| --- | --- |
| `SWR_CORE_SCHED=1` | turn on the experimental one-thread-per-core scheduler |
| `SWR_CORE_SPIN_MS=n` | overtake a core holder after n ms without a blocking SVC (default 500) |
| `start "" /affinity 1 "<Game>.exe"` | the whole game on one host CPU, the strongest serialization available |
| `log=2` in the `.ini` | IPC commands, thread start/exit (`[thread N] starts at`, `exits`) |

Reading guest memory from outside: with `renderer=vulkan` guest addresses are host addresses, so `ReadProcessMemory`
at the guest address works. If the first try at reserving the address space fails, the runtime starts a **child copy
of itself** (`plat_identity_early`) and the parent only waits. Read the child (`Win32_Process` with
`ParentProcessId` = the PID you started), or every read fails with `ERROR_PARTIAL_COPY` (299). Useful addresses in
this build: the font/resource state machine at `e132980` (state at `+0x1E4`, list head at `+0x88`), resource state at
`+0x158`, loaded bit at `+0x1B8` bit 0, name string pointer at `+0x40`.

The generated C in `<Game>_build/code_*.c` is the quickest way to read a function: `grep -l "^void F_<ADDR>(CPU.*{"`.
Return addresses in threads.txt stacks are `case <decimal>:` labels in the caller.

## Verification

Done on copies (`D:\Projects\plza-work` = the owner's build with swapped runtimes, `D:\Projects\plza-build` = the
game recompiled from the NUP with `efa9b2a`). The owner's build folder and the NUP were not modified.

- `python -m unittest discover -s tests -p "test_n*.py"`: 17 tests OK.
- Runtime builds (`switchrecomp.py --build-runtime --target win64 --backend win32`) with both commits.
- Game results: see the experiment table above. None reaches rendering.

## Open items

- Give `svcSleepThread(0/-1/-2)` real yield semantics in the scheduler: a FIFO ticket per priority, so the token
  goes to the next thread of equal priority instead of back to the caller.
- Find what thread 95's loop (`F_816F5C4`, `F_816F820`) waits for. If it is work from another core-0 thread, the
  Switch would need that thread on another core or at a better priority. Check the core and priority the game asks
  for at CreateThread / SetThreadCoreMask (the log shows the core after `-2` is resolved to the default core).
- The later stall that also happens on one host CPU: main idles in the job scheduler (`9cfb804 -> F_9CFD6F0 ->
  F_9CFE0F0`) on the pool condvar `344801858`. Inspect the per-core queues (`F_9CFD4F0`, `F_9CFC400`, scheduler
  `+3104`) in a live run to see whether a job is queued while everyone sleeps.
- Preemption on Linux (signal-based suspend) is not implemented.
- `efa9b2a` changes the code of every game on its next recompile. Run Mario Kart World and Donkey Kong Bananza on
  a recompile before merging.
- Workaround for the owner until then: one host CPU (`/affinity 1`) gets past the black screen to a few frames but
  stalls later. It's slow and not a fix.
