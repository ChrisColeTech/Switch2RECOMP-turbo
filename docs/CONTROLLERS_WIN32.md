<!-- SwitchRecomp - Copyright 2026 MZ Labs
     Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
     Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. -->
# Windows controllers: input, player lights and rumble

Status 2026-11-16: native DualSense input, player lights and rumble work end to end in Switch 2 games. On the
owner's PC (DualSense `054c:0ce6` over USB, firmware update version `0630`) a Donkey Kong Bananza run sent the
game's own rumble to the pad (1251 successful 48-byte output reports, 0 failures), and the owner confirmed the
rumble by hand. Bluetooth, DualShock 4 and DualSense Edge hardware have not been tested; see [Open items](#open-items).

## Why lights and rumble did not work (fixed 2026-11-16)

Two independent defects. Either one alone was enough to give a dark, silent controller.

1. **The output handle could never be opened** (`runtime/controller_sony_win32.h`, introduced with the native HID
   path in a0581fe). `sony_open` asked for `GENERIC_READ|GENERIC_WRITE` with share mode `FILE_SHARE_READ` only, to
   "deny concurrent writers". Windows refuses that open with `ERROR_SHARING_VIOLATION` (32) whenever any other
   handle on the device already has write access. On the owner's PC that is always the case: the error reproduces
   with no game, launcher or Steam running, and the turbo probe (`controller_device --pulse` built from
   origin/turbo) prints `Sony output ownership unavailable error=32; trying shared input` / `ownership=shared input
   only` / `output_ok=0`. The fallback then opened the pad read-only, so input worked but every lights/rumble
   write was skipped by `sony_write`'s `!d->writable` check, without a log line. The 2026-11-15 investigation saw
   the same error 32 and attributed it to a running game; it was the design. Fix: open read+write with
   `FILE_SHARE_READ|FILE_SHARE_WRITE`, as SDL/hidapi do. The read-only fallback remains for a refused open and now
   always logs `read+write open failed ... input only, no lights or rumble`.
2. **Switch 2 games' rumble never reached the backend** (`runtime/svc_hid.c`). Mario Kart World and Donkey Kong
   Bananza never call `SendVibrationValue` (201) or `SendVibrationValues` (206). After activating their handles
   (`00000003`, `00010003`, `00002004`, `00012004`) they send every frame
   `hid cmd 214 (in 2 0 1234 0)`: `SendVibrationValuesInMode` (17.0.0+), raw `u64 mode` + aruid, with the same two
   pointer buffers as 206 (handles, 16-byte values). 214 fell through to `stub_ok`, so the values were dropped.
   Both games' logs from the owner's Nov 14 builds already show 12 such calls in the first 4 s. Fix: 214 shares
   206's parser. 213 (`SendVibrationValueInMode`, value at raw 0x08, handle at 0x04 or 0x00) and 220
   (`IsOunceVibrationDeviceMounted`, Switch 2) are handled as well.

"It used to work" could not be matched to any engine version: the owner's installed game executables are
`runtime 0.4.1, built Nov 14 2026` and contain no `XInputSetState`, no Sony HID code and no 214 handler (their logs
contain no `controllers:` lines at all). XInput rumble arrived in 64237e0 (Nov 15) and Sony output in a0581fe
(Nov 15); both shipped with the two defects above. e24b2a9 (startup splash) only changes painting and does not
affect focus or controllers. The machine has ViGEmBus and HidHide installed, so an earlier working setup was most
likely a DS4Windows-style virtual Xbox pad, which lights the pad itself; through XInput a game still only rumbles
if it uses 201/206.

## Design

`runtime/controller_sony.h` holds the bounded report decoder and packet builder. `runtime/controller_sony_win32.h`
enumerates HID interfaces through hid.dll/setupapi.dll (loaded at runtime), opens overlapped input, handles
disconnects, reads the DualSense firmware feature report and writes output reports. Both are included by
`runtime/backend_win32.c`; no build-list change, driver or system setting is needed.

Selection order is XInput, supported native Sony HID, then WinMM. Only the selected device gets output. An XInput
device takes priority over a native duplicate (for example a DS4Windows/ViGEm virtual pad with the real pad hidden by
HidHide: then lights are the wrapper's business and rumble goes through `XInputSetState`). Unsupported Sony products
and unfamiliar report-size combinations get no output and keep the old input fallback.

| Model | Sony product IDs | USB input/output | Bluetooth input/output |
| --- | --- | --- | --- |
| DualSense / DualSense Edge | `0ce6`, `0df2` | `01`, 64 bytes / `02`, 48 bytes (63-byte advertised variant is zero padded) | simple `01` input or enhanced `31`, 78 bytes / `31`, 78 bytes |
| DualShock 4 | `05c4`, `09cc` | `01`, 64 bytes / `05`, 32 bytes | simple `01` input or enhanced `11`, 78 bytes / `11`, 78 bytes |

Bluetooth enhanced reports validate input CRC32 with prefix `A1`; output uses prefix `A2`. DualSense Bluetooth output
uses the four-bit rolling sequence and the `10` tag; DualShock 4 Bluetooth uses the HID/CRC flags and a 4 ms poll
interval (`C4`). Unknown models, report IDs, lengths and bad CRCs are rejected.

Output report contents (offsets in the common block, after the report ID / Bluetooth header):

- DualSense: `valid_flag0=03` (compatible vibration + haptics select), `valid_flag1=14` (lightbar + player
  indicator), byte 2 = high-frequency (right) motor, byte 3 = low-frequency (left) motor, byte 43 = player LEDs
  (`04`, the centre LED = player 1), bytes 44-46 = lightbar RGB `00 28 80`. On open, a separate packet with
  `valid_flag2=02` / `lightbar_setup=02` releases the firmware's lightbar animation. From firmware update version
  2.21 (and always on Edge, or when the feature report cannot be read) `valid_flag2=04` (compatible vibration 2,
  the improved rumble emulation) is added, following Linux hid-playstation; the version comes from feature report
  `0x20`, bytes 44-45.
- DualShock 4: `valid_flag0=07`, bytes 3/4 = right/left motor, bytes 5-7 = lightbar RGB.

Example, the owner's pad, "lights on" (USB, 48 bytes):
`02 03 14 00 00 ... [39]=04 ... [44]=04 00 28 80`.

Guest low/high-frequency amplitudes (the maximum over the active left/right handles) map to the host low/high
motors. This approximates Switch HD rumble; it is not frequency-accurate. Amplitudes are made finite and clamped.
Nonzero updates coalesce to about 60 Hz; zero bypasses the throttle.

Disconnect, error or a one-second input gap clears state and closes the handle; rescans are limited to one per
second, and a reopened pad starts with zero rumble. Losing focus (unless `SWR_CONTROLLER_BACKGROUND=1`) sends zero
and turns the player lights off, so a lit player LED means the game currently owns player 1. Rumble stops on window
close, Esc, the window thread ending, normal C `exit`, the timed `seconds=` exit, `fatal()` (before its message box,
which also covers crashes and the NVN layer's `nxrt_die`) and `NXRT_EXIT_AT_PRESENT`. Those immediate exits call
`backend_controller_shutdown()`, which waits at most ~200 ms for the controller lock so an exit can never hang on
it. A failed or short write is logged and disables further ordinary writes for that connection; teardown still
tries one zero.

`svc_hid.c` decodes handles as `{style, player, device, reserved}`, takes 201's four floats inline, 206/214 from the
two pointer buffers, 213 from raw data, keeps activation across permission toggles, and serializes vibration
state separately from backend I/O.

## Buttons and button layout

Every pad path reports buttons **by position** in the Switch bit order: XInput (`xinput_buttons`), native Sony HID
(`sony_input`), WinMM (`joy_buttons`, Sony and generic pads alike) and Linux SDL2 (`PAD_SDL`, with
`SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0`). `runtime/pad_layout.h` then maps that positional mask to the guest once,
in `video_poll`/`pad_poll`, from two INI keys (section `[Controls]`, both apply live when the INI is saved):

- `button_layout=nintendo` (default; aliases `position`, `positions`): the button where a Switch pad has A is A.
- `button_layout=xbox` (aliases `label`, `labels`): Xbox labels, i.e. the bottom button is A.
- `button_map=physical:guest ...` (optional, empty by default): per-button overrides on top of the layout,
  separated by spaces, commas or semicolons, case-insensitive. Physical: `south east west north` (or `cross circle
  square triangle`), `l r zl zr lstick rstick plus minus up down left right` (the Switch names of those positions).
  Guest: `a b x y l r zl zr lstick rstick plus minus up down left right none`. Example: `button_map=south:a east:b`
  on the nintendo layout swaps only A/B. Bad entries are logged and skipped; an unknown layout falls back to
  `nintendo`.

| Position | Xbox | Sony | `nintendo` | `xbox` |
| --- | --- | --- | --- | --- |
| right (east) | B | Circle | A / bit 0 | B / bit 1 |
| bottom (south) | A | Cross | B / bit 1 | A / bit 0 |
| top (north) | Y | Triangle | X / bit 2 | Y / bit 3 |
| left (west) | X | Square | Y / bit 3 | X / bit 2 |

| Other physical buttons (all layouts) | Guest Switch button / bit |
| --- | --- |
| L3 / R3 | left/right stick click / 4, 5 |
| L1 / R1 (LB / RB) | L / R / 6, 7 |
| L2 / R2 (LT / RT) | ZL / ZR / 8, 9 |
| Options / Create (Share), Start / Back | Plus / Minus / 10, 11 |
| D-pad left/up/right/down | 12, 13, 14, 15 |

Why `nintendo` is the default: the games' button prompts and muscle memory refer to the Switch positions, XInput and
Linux were already positional, and emulators (Ryujinx, yuzu) map by position by default. Before this change the two
Sony paths were the odd ones out with a hybrid (Cross = A and Circle = B by "confirm" label, but Triangle = X and
Square = Y by position), so on a DualSense A/B felt swapped against the positional pads and X/Y felt swapped against
Xbox labels. Now every pad follows the same layout, and `xbox` gives the full label layout (Cross = A, Circle = B,
Square = X, Triangle = Y) for players who want PC-style confirm/back. The keyboard is not remapped. RecompLauncher
writes `button_layout` (Settings > Controller default, per-game override in Properties > Runtime) into the game's INI.

The log shows the active layout at startup and after a live change, e.g. `controllers: button layout nintendo
(A=right B=bottom X=top Y=left)` or `controllers: button layout xbox (A=bottom B=right X=left Y=top) button_map:
zl:zr zr:zl`; the Sony connect line names the face buttons for the layout.

Sticks use the 7000 deadzone and positive-up guest Y. Diagonal hats produce both directions. PS, touchpad,
microphone, Edge paddles, gyro, adaptive triggers and DualSense audio are not mapped or driven.

## Switches and diagnostics

- `SWR_CONTROLLER_TRACE=1`: Sony input/button changes, final `g_pad` masks, every output write, and every nonzero
  guest vibration batch (`hid: vibration batch cmd=214 mode=2 ...`). Without it the first 8 of each are logged.
- `SWR_CONTROLLER_SELFTEST=1`: once a rumble-capable pad is active in the game, lights on, low motor 0.5 s, high
  motor 0.5 s, stop. It goes through the same flush and device-write path as the game's rumble.
- `SWR_CONTROLLER_BACKGROUND=1`: allow controller input and output while the window is unfocused.
- `SWR_NO_SONY_HID=1`: skip native Sony HID (XInput/WinMM only).
- `SWR_INPUT_EXCLUSIVE=1` together with `SWR_INPUT_FILE`: scripted input only.

Normal logs show model, VID/PID, transport, input/output sizes, output access, firmware update version and rumble
mode, e.g. `controllers: DualSense 054c:0ce6 USB backend=native-HID input=64 output=48 output-access=read+write
(lights+rumble) firmware-update=0630 rumble=compatible-v2`, followed by `Sony output sent ...` lines. Rumble that
cannot reach a pad logs `rumble ... dropped: the Sony controller is open read-only`.

### Physical check (one command, about 5 s, no game needed)

```powershell
$env:PATH = "D:\Projects\RecompLauncher\third_party\gcc\Library\bin;$env:PATH"
& D:\Projects\RecompLauncher\.venv\Scripts\python.exe tests\test_controllers.py --rumble
```

Hold the pad: blue lightbar and the centre player LED come on, then the left (low-frequency) motor, the right
(high-frequency) motor, both, and everything turns off. Each write prints its report bytes and `write=OK bytes=48`.
`--device` is the read-only variant (decodes input, sends nothing); `--pulse` is a short weak check.
`--buttons [SECONDS]` (default 20, read-only) prints for every button you press the Switch button it becomes under
both layouts, e.g. `Circle (right)   -> nintendo: a      xbox: b`.

In a game: set `SWR_CONTROLLER_SELFTEST=1` (and `SWR_CONTROLLER_TRACE=1`) before starting it; the pad lights and
pulses about 1.5 s after the window opens, then follows the game's rumble.

## Verification

`tests/test_controllers.py` (CI) compiles the production units and checks: packet bytes for both models and
transports (lights, motors, stop, lightbar release, Bluetooth sequence and CRC against Python's zlib), every pad
path (XInput, WinMM generic and Sony, Sony HID DualSense and DualShock 4, the SDL table) under both button layouts,
`button_map` overrides/aliases/errors and the logged layout description, the DualSense
vibration-v2 flag and its firmware gate, input decoding fixtures, the shared-write open mode, the real service
dispatch for 201/206/213/214/220 (bounds, clamp, permission, activation) and shared-memory rings, and the backend's
rate limit/stop/shutdown with a mock XInput device.

Evidence 2026-11-16, owner's PC:

- Turbo code, `controller_device --pulse`: error 32, shared input only, `output_ok=0` (no game running).
- Fixed code, `--rumble`: 7 writes `write=OK bytes=48 error=0`, firmware update `0630`, v2 rumble.
- Donkey Kong Bananza copy with the rebuilt runtime, `SWR_CONTROLLER_TRACE=1 SWR_CONTROLLER_SELFTEST=1
  SWR_CONTROLLER_BACKGROUND=1`, 75 s: read+write open, self-test writes, 4 handles activated, 214 batches from
  5.0 s, nonzero game rumble from 20.0 s mapped to Sony output (`low=0.250 -> low=64`), 1251 writes, 0 failures,
  final `low=0 high=0 player1=0` at the timed exit. Owner: "rumble works now".

## Open items

These need hardware or input that this machine could not provide; the code paths exist and are fixture-tested.

- Lights were sent (byte-exact, accepted by the driver) but only rumble has been confirmed by the owner so far.
- Bluetooth DualSense and any DualShock 4 / DualSense Edge: no such pad was connected (only a remembered Bluetooth
  pairing of the same DualSense). Run `--rumble` once over Bluetooth.
- Physical buttons, focus switching and unplug/replug were not exercised by hand in this pass.
- Who holds the other write handle on the owner's DualSense was not identified (no hid.dll user is visible to an
  unelevated process); it no longer matters since output shares write access, but a second writer (Steam, a
  browser using the Gamepad API, DS4Windows) can still send its own lights/rumble packets in between.
- Abrupt external termination (Task Manager, `TerminateProcess` from outside) cannot send a final stop.

## Protocol references and attribution

- Linux [`hid-playstation.c`, v6.12](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c):
  Sony report layouts, compatible rumble (v1/v2 and the 2.21 firmware gate), lightbar setup, sequence/tag and CRC.
  Copyright (c) 2020-2022 Sony Interactive Entertainment, GPL-2.0-or-later. Adapted protocol code keeps that
  attribution in `controller_sony.h`; this project's combined code is AGPL v3.
- SDL [`SDL_hidapi_ps5.c`, SDL2](https://github.com/libsdl-org/SDL/blob/SDL2/src/joystick/hidapi/SDL_hidapi_ps5.c):
  cross-checked the 48-byte Windows USB output and the shared-access open. No SDL code is added.
- libnx [`services/hid.c`](https://github.com/switchbrew/libnx/blob/master/nx/source/services/hid.c) and
  [`services/hid.h`](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/hid.h), and
  [switchbrew HID services](https://switchbrew.org/wiki/HID_services): handle field order, style indices, commands
  201/206 and the names of 213/214/220. The 213/214 layouts were confirmed from the games' own requests.
