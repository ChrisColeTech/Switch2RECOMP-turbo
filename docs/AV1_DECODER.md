# AV1 movies: dav1d in the Switch 2 video decoder service

Work of 2026-11-17 (night), branch `av1-dav1d`. The Legend of Zelda: Ocarina of Time (Switch 2, `0400486024DEC000`,
v131072) plays its title and opening movies as AV1 through `nn::codec` (`InitializeAv1DecoderByTransferMemory`). Until
this round the video decoder service accepted the codec and gave one black picture a packet
(`GAMEPLAY_INVESTIGATION.md` section 6). Now the packets are decoded with dav1d and the pictures are in the game's own
textures. **They are not on screen yet**: between those textures and the screen is the game's movie network
(`nn::ndve`), which this renderer does not compute yet (below, and Open).

Reference: [nxnative](https://github.com/seedds/nxnative) (MIT, (c) seedds), which serves the same decoder interface:
`dm/host/hle_movie_av1.c` (its dav1d backend), `dm/host/hle_codec_video.c`, `docs/reference/movies.md` and the game's
notes, `dm/games/0400486024DEC000/README.md`. Logic was ported, not code for Apple's frameworks; the ported parts are
named below and in `NOTICE.txt`.

## What was built

| | |
|---|---|
| Decoder | dav1d 1.5.5 (BSD-2-Clause), tag `1.5.5`, commit `6c7bee685677fd997a431302be4fea8ea4017606`, unmodified, from `dav1d-1.5.5.tar.xz` (SHA-256 `ca96b1d0...3abf9e`) |
| Archives | `runtime/av1/win/libdav1d.a` (3,387,086 bytes, SHA-256 `f64d2ee3...ce46f96a`; MinGW-w64 GCC 16.2.0, NASM 2.16.03) and `runtime/av1/lin/libdav1d.a` (3,584,964 bytes, SHA-256 `f7d3af17...7a2320ca`; GCC 13.3.0 on Ubuntu 24.04, NASM 2.16.01) |
| Contents | both bit depths (8, and 10/12), the x86 assembly from SSE2 to AVX-512 (picked at run time), no tools; on Windows dav1d's own Win32 threads, so it decodes on several threads with the toolchain's win32 thread model (libvpx cannot) |
| Build | `runtime/av1/build_dav1d.py`: one script for both systems, everything it downloads checked by SHA-256, meson 1.12.1 and ninja 1.13.2 in a virtual environment of its own. Two builds from fresh folders gave identical bytes on each system |
| Headers, license | `runtime/av1/include/dav1d/`, `runtime/av1/LICENSE-dav1d.txt`; `runtime/av1/README.md` has the full hashes, the flags and how to rebuild or update |

How to rebuild dav1d:

```
python runtime\av1\build_dav1d.py --gcc-bin gcc\Library\bin          (Windows)
python3 runtime/av1/build_dav1d.py                                   (Ubuntu 24.04: build-essential nasm python3-venv)
python tests/test_av1.py                                             (each system: the archive's hash, then the decode test)
```

`switchrecomp.py --build-runtime` links `runtime/av1/<win|lin>/libdav1d.a` when it is there and defines
`SWR_HAVE_AV1`; without it the runtime builds and AV1 movies run their length black, as before.

## The decode path

`runtime/av1video.inc` holds everything that touches dav1d, with nothing of the runtime in it, so the test builds the
same code. `runtime/svc_ns2.c` (the video decoder service) calls it where it calls libvpx for VP9, and the pictures go
into the same queue: the packet FIFO, the pictures decoded ahead (`VDEC_AHEAD`), the syncpoint fences, the re-release
after a flush (`GAMEPLAY_INVESTIGATION.md` sections 1 and 6) are the VP9 path's, unchanged.

Ported from nxnative's `hle_movie_av1.c`:

- **Frame threads.** dav1d is opened with 4 threads on 3 frames at once (fewer on a CPU with fewer cores). A temporal
  unit of a hierarchical stream holds several frames (the hidden ones the next units only show); decoded one after the
  other they make that unit many times longer than the others. With frame threads they, and the units behind them,
  decode side by side.
- **Each unit's time rides with it.** A picture then comes out later than its unit went in, so the packet's time goes
  in `Dav1dData.m.timestamp` and is read back from `Dav1dPicture.m.timestamp`.
- **One get for each send.** A second `dav1d_get_picture` in a row is dav1d's drain, which waits for the frames in
  flight and so decodes them one after the other.
- **A drain goes to its end.** Data sent between two drain calls can leave dav1d waiting for ever (nxnative found the
  title movie frozen in 3 runs of 6), so a drain is always taken to `EAGAIN` before the next unit.
- **NV12.** The planar picture becomes the 8-bit NV12 the service's image pools hold: luma copied, or shifted down from
  10 or 12 bits; chroma interleaved, subsampled from 4:2:2 and 4:4:4, grey for 4:0:0.

Decided here:

- **The pictures dav1d holds are taken out when nothing else is left to show** (`vdec_pump`): no picture queued, no
  packet waiting, an output image free. Without that a movie's last pictures (up to two with three frames at once)
  would wait for a packet that never comes. While the game sends ahead, which it does, there is no drain: 2 in the
  title movie's first 1,628 units.
- **Busy instead of out of resource.** Ocarina's player does not wait for the service's ready event: with the real
  bitstream kept, its title movie's first 1,560 packets came in 1.4 s, their bytes passed the FIFO's 64 MB and the
  "out of resource" answer made the SDK abort (`nn::diag::detail::Abort`), 8.6 s in. With the black pictures no
  bitstream was kept, so that limit was never met. A submission that finds 32 packets waiting (`VDEC_PK_HOLD`) is now
  refused with `nn::codec`'s "busy" (description 3603, `0x1c266f`), which `NintendoSDK_movie` turns into its retry
  code (nxnative, movies.md "A refusal is no fault"). The ready event still goes down earlier, at 24. The rule is the
  queue's, so it holds for VP9 too (the regression runs below).
- **Errors are not fatal.** A unit dav1d refuses, or a frame it cannot decode, is counted and warned about (8 lines at
  most) and the movie goes on without that picture.

Switches: `SWR_AV1=0` (black pictures, as a runtime without dav1d), `SWR_AV1_THREADS=<n>`, `SWR_AV1_FRAMES=<n>` (1:
one frame at a time, every picture out when its unit is in).

The log says what the decoder does: `switch 2 video: dav1d 1.5.5 decoding AV1, 4 threads on 3 frames at once`, and
every 600 units, at the stream's end and when the decoder closes, `AV1 decode ...: N units, X ms a unit in dav1d, Y ms
the longest`: the time the submitting thread spent inside dav1d, which is what the game's reader thread loses.

## The blocker behind the decoder: image stores into textures without the IMAGE flag

With real pictures decoded the title was as black as before. `gamectl texdump` showed where they stopped: the game's
own R8 1920x1080 and RG8 960x540 textures (it reads the decoder's NV12 the CPU route, `GetCpuImageDescriptor` /
`ReadDecoderOutputImage`, and uploads the planes itself) held the movie's picture, Hyrule Field, and every texture
after them was zero. The log had the cause:

```
[vk] [RED] image binding of texture ID 1938 (ser 6230, 1920x1080, format 0x25, flags 0, a GPU texture) whose image has no storage use: writes are lost
```

Between the planes and the screen is `nn::ndve`'s movie enhancement network (`Movie/Ndve/Ndve-Nvn2-vid_seq5.dat`: 14
compute dispatches a frame). It writes its pictures by image store into an RGBA8 1920x1080 texture and then into the
RGBA8_SRGB 2560x1440 one the title draws, and it made both without `NVN_TEXTURE_FLAGS_IMAGE` (flags 0 and 8). The
console takes that. The Vulkan layer gave an image storage use only with the flag, refused the binding and dropped the
writes.

Change (`runtime/nvn/vk/vk_tex.c`, `vk_replay.c`), **off by default**: the storage of a texture that is registered as an image, or bound as
one, without the flag is remembered (`vkt_want_image`), and every image made over that storage has storage use, under
the same key for every texture over it, so the texture that samples the picture sees what was written. An sRGB texture
used as an image is created in its UNORM twin (`storage_format`: Vulkan has no sRGB storage images); the store writes
the bytes as they are and the sRGB view it is sampled and drawn through is one more view of the same image. nxnative
upgrades its Metal texture in place for the same case ("texture upgraded to allow image writes"). Textures made with
the flag, and every texture never used as an image, are created exactly as before.

With it on (`NXRT_VK_IMAGE_NOFLAG=1`, or `gamectl set image_noflag 1`) both bindings are taken and the writes land,
and what lands shows the next blocker: the network's first picture (the RGBA8 1920x1080 texture) is all zeros with
alpha 255, and its last one (the 2560x1440 the title draws) is noise in the top third of the screen. The kernels run
(14 dispatches a frame, no shader missing) but do not compute the picture. Their tensors go through raw global memory
(`raw global memory: a shader reads or writes through pointers it loaded from memory`, then `more than 1024 pages
written through loaded pointers in a frame`, 1,076 pages learned in 20 ranges after a second), which is the renderer
and translator work `NXNATIVE_GAP_ANALYSIS.md` lists, not the codec. Noise behind the title is worse than the black
the lost writes leave, so the switch is off until the network computes; then it has to be on.

This is renderer code, outside the codec. It was looked at because the movie cannot be shown without it, and left
behind a switch because it cannot be finished from the codec's side.

## Evidence

All runs are the real library build (`P:\...\SwitchRecomp Builds\The Legend of Zelda Ocarina of Time`), rebuilt through
the engine CLI with this branch's runtime (the harness records the hashes), driven by `tools/plza_probe.py`. The route:
boot, three shots of the title 55, 65 and 75 s in, A, A, three shots of the opening. Console session, RTX 5090.
Run folders: `D:\Projects\Switch2RECOMP-main\build\evidence\av1\plza-investigation\<run>` (not in git).

| run | what | title (near-black, black cells of 64) | opening | notes |
|---|---|---|---|---|
| `20261117-213837-zelda-av1-off` | this runtime with `SWR_AV1=0`: the black pictures of before | 81.4 %, 42; 81.3 %, 43 | 98.9 % to 100 %, 56 to 64 | the same numbers as `-204152-zelda-final` of the round before (81.5 %, 42) |
| `20261117-214046-zelda-av1-on` | dav1d, before the busy answer | none: abort 8.6 s in | | 1,560 packets in 1.4 s, then "out of resource" |
| `20261117-214936-zelda-texdump` | dav1d; `gamectl texdump` 61 s in | | | the game's R8 1920x1080 texture: mean luma 59.9, 0 % near-black, the picture of Hyrule Field (`texdump-luma-960.png`, in colour from both planes: `movie-picture-from-textures.png`); every texture after it zero |
| `20261117-221642-zelda-av1-final` | dav1d, final runtime | 80.9 %, 43; 81.5 %, 42; 81.3 %, 42 | 98.8 % to 100 %, 56 to 64 | 2,334 units of the title movie and 2,332 pictures, 0 dropped; 27.6 FPS, present interval p50 35.8 / p95 38.9 / p99 40.2 ms; no abort, crash or device loss |
| `20261117-220639-zelda-av1-on4` | the same with the image change on (before it got its switch) | 71.3 % to 72.4 %, 24 to 26 | 87.9 % to 94.7 %, 28 to 40 | what is no longer black is noise (`0004-shot-settled.png`, `0007-shot-settled.png`) |
| `20261117-220937-zelda-texdump2` | the same; `gamectl texdump` | | | planes as above (`movie-picture-from-textures.png`); the network's RGBA8 1920x1080 all zero with alpha 255; the RGBA8_SRGB 2560x1440 94 % near-black, the rest noise |
| `20261117-222045-zelda-av1-serial` | `SWR_AV1_THREADS=1 SWR_AV1_FRAMES=1` | | | decode time on one thread (below) |

So on screen the black audit is unchanged, 81 % and 42 or 43 cells at the title before and after, and the opening 99 %
to 100 %: the decoder's pictures stop one step short of the screen. In the game's plane textures the picture is real.

Decode time, the title movie (1920x1080, 60 pictures a second), measured around the dav1d calls of each unit:

| | first 600 units (fade in) | units 600 to 1,200 (the heavy part) | longest unit |
|---|---|---|---|
| 4 threads, 3 frames at once (the default): what the submitting thread waits | 1.40 ms | 2.52 ms | 73.5 ms |
| 1 thread, 1 frame: the whole decode of a unit | 4.28 ms | 16.02 ms | 178.7 ms |

Over the whole title movie 2.12 ms a unit. The longest units are the ones that hold a hierarchy's hidden frames; the six
pictures decoded ahead cover them (no picture was late in the log: `0 dropped`, and the queue stayed at 5 or 6). The
busy answer was given between 256 and 511 times in the title movie's 86 s (the log counts at powers of two): 3 to 6 a
second, so the player backs off, it does not spin.

Regression runs (the same runtime, library builds rebuilt through the CLI; saves copied aside first and unchanged
after: `build\evidence\av1\save-backups`):

| game | run | result |
|---|---|---|
| Mario Kart World | `20261117-222302-mkw-av1-regress` | menus and a race: 12 screenshots, 0 black cells in each (near-black 0 to 2.8 %). VP9 through the service: four 640x360 clips of 300 pictures, each to its end marker, and the 2,560x1,440 movie's 1,424 pictures; 0 dropped, no decode failure; 18 logged busy answers, which its player takes (the clips are whole). Race 55.1 FPS over 110 to 190 s and 57.1 over 150 to 190 s, against 56.4 and 58.4 in `-202635-mkw-nx` (the other agent's game was running at the same time). The save the run wrote was put back |
| Donkey Kong Bananza | `20261117-222847-dkb-av1-regress, 20261117-223316-dkb-vp9-tutorial` | the route of the round before (title, Continue, walk, punch, walk): terrain shaded, 0 or 1 black cells of 64, 52.7 FPS mean, no video on this route with the library's save. The tutorial video is shown by the save of 2026-11-14 (kept with the controller round's evidence), put in for one run and the library's save put back and compared after it: the 960x540 VP9 clip plays in its panel (`0006-replay-settled.png`), four passes of 180 pictures each to the end marker, 0 dropped, no decode failure; its player takes the busy answers too |

## Tests

- `python tests/test_av1.py` (new; Windows and WSL Ubuntu 24.04): the archive's hash against `runtime/av1/README.md`,
  then `tests/test_av1.c`: two synthetic streams (`tests/av1/vectors.h`, made by `tests/av1/make_vectors.py` from
  ffmpeg's test pattern with libaom: 8-bit 4:2:0 64x48, 12 units; 10-bit 4:4:4 50x38, 5 units; no game data) through
  `av1video.inc`. Every picture comes out once, in order, with its unit's time, and its NV12 has the hash of the
  picture libaom's decoder gives (a second decoder, not dav1d): on one frame at a time, on frame threads, with a drain
  after each unit in turn, over 300 rounds with drains at random places, and after a unit of junk (refused, counted).
  A wrong expected hash fails it (checked by changing one).
- `python -m unittest discover -s tests -p "test_n*.py"` (17), `tests/test_controllers.py`, `tests/test_mkw_runtime.py`,
  `tests/test_mkw_perf.py`, `tests/test_python_selection.py`: pass on Windows.
- Linux: `tools/build_nvn.py --linux` and `switchrecomp.py --build-runtime --target linux --backend x11` build in WSL
  with GCC 13; the runtime holds `dav1d %s decoding AV1` and dav1d's `Error parsing OBU data`, and still needs no
  glibc newer than 2.34.
- Engine CI (`.github/workflows/engine-ci.yml`) runs `tests/test_av1.py` on both systems and checks both runtimes for
  the two markers beside the VP9 one. Green on the branch (run 44229180865) before the merge.
- RecompLauncher: `scripts/release-deps.py` pins the source archive and both archives (`DAV1D_SOURCE`,
  `DAV1D_ARCHIVES`) and puts the source under `legal/sources`; `scripts/package-release.py` ships
  `legal/licenses/dav1d/LICENSE-dav1d.txt`, checks the source's hash, the archive against the manifest
  (`verify_av1_provenance`) and the runtime's markers; `tests/test_release.py` covers each.

## Open

1. **The movies are still black on screen.** `nn::ndve`'s network has to compute: its kernels' tensors through raw
   global memory first (a hunch worth one look: a pool page the GPU wrote through a loaded pointer and that was never
   copied back, because of the 1,024 pages a frame, may be uploaded from guest memory later, zeros over the result:
   the log has "39 MB uploaded ... never copied" at the movie's start), then `NXRT_VK_IMAGE_NOFLAG` on by default.
   `gamectl texdump` shows each stage: the planes (right), the RGBA8 1920x1080 (zeros), the RGBA8_SRGB 2560x1440.
   nxnative runs the network as host kernels by default; its notes are shaders.md "The NDVE movie network by the
   host" and "`nn::ndve`'s movie enhancement file".
2. **The movie runs at the game's frame rate, not at its own.** The player takes one picture a game frame; at the
   title's 27.6 FPS a 60-picture movie plays at 0.46 of its speed (120 pictures in 4.3 s in the log) while its sound
   runs on real time. nxnative's notes for this game (README "Frame rate"; movies.md "A player limited to one picture a
   frame falls behind its sound") need the host's frame step in the game's frame ratio and three cuts in the player.
   Here the frame rate itself is the item: the title's frame is the network's dispatches (GPU 24 to 26 ms a frame).
3. The decode runs inside the submission, on the game's reader thread, as VP9 does. With frame threads that thread
   waits 2.1 ms a unit; nxnative moved the decode to a thread of its own after measuring stutter at 15 ms a unit on an
   M2 Pro. Not needed at these numbers; the place for it is `vdec_pump`.
4. H.264 through this service is still refused (no game here asks for it); AV1 with more than 8 bits is shifted to 8
   (tested on the synthetic stream only: no library game has such a movie).
