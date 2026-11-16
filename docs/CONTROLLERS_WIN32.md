<!-- SwitchRecomp - Copyright 2026 MZ Labs
     Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
     Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. -->
# Windows controller investigation, 2026-11-15

## Device and causes

Read-only PnP enumeration identified Sony **054c:0ce6, DualSense**, not DualShock 4. The active game-controller interface is USB `MI_03`, usage page 1, usage 5. Windows reports **64 input bytes and 48 output bytes**. A shared read-only HID handle received valid report `01`, neutral buttons and centered sticks. Bluetooth PnP entries also exist, but no active Bluetooth game-controller interface was opened in this check. Remembered pairing entries alone do not establish the current transport.

The old native fallback was WinMM (`joyGetPosEx`). It provided input, but had no LED or Sony output implementation. All physical rumble went through `XInputSetState`, which cannot address a native Sony HID controller. The old setter also returned without recording output when XInputSetState was unavailable.

Two service defects independently prevented vibration:

- `vib_side` read the player from byte 0 and style from byte 1. The actual handle is `{style, player, device, reserved}`. Player-one FullKey handles are `00000003` and `00010003`; the old code rejected them as player 3. Handheld uses `00002004` and `00012004`.
- Command 201 (`SendVibrationValue`) carries the four floats inline at raw offset 4. The old code looked for input buffer 1 and never consumed a normal inline request. Command 206 uses two pointer buffers, which is a different layout.

The prior rendering agent had already fixed the WinMM L1/R1 versus L3/R3 bit swap. That change and its Cross-to-confirm preference are preserved. Focus gating is another independent explanation for an apparently dead controller: host gameplay input is suppressed while the game is unfocused or its controls page is open. Exclusive scripted-input tests intentionally suppress all host controllers.

## Implementation

`runtime/controller_sony.h` implements the bounded report decoder and packet builder. `runtime/controller_sony_win32.h` enumerates HID capabilities through Windows' existing hid.dll/setupapi.dll, opens overlapped input, handles disconnects, and writes the selected protocol. Both are included by `runtime/backend_win32.c`; no build-list change, installed package, controller driver or system setting is required.

Selection order remains XInput, supported native Sony HID, then WinMM. Only the selected device owns player-one output. An XInput device takes priority over its possible native duplicate. Unsupported Sony products and unfamiliar report-capability combinations receive no Sony output packets and retain the old input fallback.

| Model | Sony product IDs | USB input/output | Bluetooth input/output |
| --- | --- | --- | --- |
| DualSense / DualSense Edge | `0ce6`, `0df2` | `01`, 64 bytes / `02`, 48 bytes (63-byte advertised variant is zero padded) | simple `01` input or enhanced `31`, 78 bytes / `31`, 78 bytes |
| DualShock 4 | `05c4`, `09cc` | `01`, 64 bytes / `05`, 32 bytes | simple `01` input or enhanced `11`, 78 bytes / `11`, 78 bytes |

Bluetooth enhanced reports validate input CRC32 with prefix `A1`; output uses prefix `A2`. DualSense output uses the four-bit rolling sequence and `10` tag. DualShock 4 uses the HID/CRC enable flags and four-ms poll interval. Unknown models, report IDs, full-report lengths and bad enhanced CRCs are rejected. The Windows backend only enables protocols whose advertised report sizes match the table. Bluetooth and DS4 hardware remain untested on this host.

Output enables classic two-motor compatibility rumble, a blue/cyan player-one light, and the DualSense center player indicator. DualSense lightbar animation reset is separate from normal updates. Audio routing, microphone controls and adaptive triggers are not enabled by these packets. Guest low/high-frequency amplitudes map to the host low/high motors; this is an approximation of Switch HD rumble, not frequency-accurate HD haptics.

Output access denies concurrent writers. If Windows refuses that handle, the backend opens shared read-only input and logs the ownership failure; it does not force access or compete with another writer. Refocusing the game or reconnecting retries ownership. Device I/O and output state share the backend critical section. `InitOnceExecuteOnce` initializes it even if a stop arrives before XInput discovery. Amplitudes are made finite and clamped before integer conversion. Nonzero updates coalesce to about 60 Hz, while zero bypasses that throttle.

Disconnect/error/one-second input timeout clears cached state and closes the handle; rescanning is limited to once per second. Reopening starts with zero rumble. Focus loss closes Sony output after sending zero and turning off its player lights, and sends zero to the old XInput owner before forgetting its index. The player light therefore means this runtime currently owns the active player, rather than merely that a cable is present. Closing the window, Esc, window-thread termination, normal C `exit`, and the backend's timed exit stop output. The output probe also stops before closing. Failed output and short writes are logged and disable subsequent ordinary writes for that connection; teardown still attempts zero once.

`svc_hid.c` fixes handle decoding, inline command 201, one aggregate per batch, permission toggles without losing activation, and a four-byte stack overread in the eight-byte command 208 reply. Service vibration state is serialized separately from backend I/O. Active player styles and the existing input shared-memory layout are preserved.

## Buttons and focus

| Physical Sony button | Guest Switch button / bit |
| --- | --- |
| Cross | A / 0 (existing confirm preference) |
| Circle | B / 1 |
| Triangle | X / 2 |
| Square | Y / 3 |
| L3 / R3 | left/right stick click / 4, 5 |
| L1 / R1 | L / R / 6, 7 |
| L2 / R2 | ZL / ZR / 8, 9 |
| Options / Create (Share) | Plus / Minus / 10, 11 |
| D-pad left/up/right/down | 12, 13, 14, 15 |

Sticks use the existing 7000 deadzone and positive-up guest Y convention. Diagonal hats produce both directions. PS, touchpad, microphone, Edge paddles and physical gyro are not mapped by this change. XInput retains its existing physical Switch convention: Xbox B is Switch A, Xbox A is Switch B. A virtual XInput backend can therefore have a different confirm button from native Sony.

- `SWR_CONTROLLER_TRACE=1`: opt-in Sony report/button changes, final `g_pad` masks, HID shared-memory sample/masks, and detailed output status. These expose the input stages without pretending a synthetic request was a physical press.
- `SWR_CONTROLLER_BACKGROUND=1`: explicitly permit controller input while unfocused. Keyboard/mouse remain focus gated. The controls page and exclusive input probe still suppress controllers.
- `SWR_NO_SONY_HID=1`: use the existing XInput/WinMM path for comparison.
- `SWR_INPUT_EXCLUSIVE=1` has its prior meaning only together with `SWR_INPUT_FILE`. Existing acknowledged input probes are preserved.

Normal logs identify model, VID/PID, transport, backend, input/output lengths, ownership and focus suppression. Initial output successes and failures are bounded; trace mode enables further per-output success logging. Service logs show the first activation/single/batch requests and their decoded handles/values.

## Verification and evidence

Used the existing **`.venv\Scripts\python.exe`**, Python 3.14.9. The project root and ancestor had no applicable AGENTS.md; the nested lz4 guide does not apply. Read `DEVELOPER_GUIDE.md` and `docs/MKW_RENDERING_INVESTIGATION.md`. All prior dirty work and concurrent agents' changes are preserved. No game, full GPU test, runtime build/deployment, commit, driver change or pairing change was performed by this controller task.

Run isolated tests:

```powershell
& .venv\Scripts\python.exe tests\test_controllers.py
& .venv\Scripts\python.exe tests\test_controllers.py --device
# Only with the device available for output; 24/255 and 16/255 for 150 ms, then zero:
& .venv\Scripts\python.exe tests\test_controllers.py --pulse
```

Each command creates a unique `build/controllers-*/` directory with its own binaries and `checks.log`. It never writes `bin/runtime*` or a game executable.

- `build/controllers-20261115-193615-424985800/checks.log` (final): full changed Win32 backend and HID-service translation units pass syntax checking. Production packet/mapping code passes USB/Bluetooth fixtures, all face/shoulder/stick/menu/hat mappings, axis endpoints, short input/output bounds, stop fields, LED reset, sequence wrap and independently calculated Python zlib CRCs. Actual production service dispatch passes exhaustive small-handle validation, single/batch parsing, truncated requests/buffers, finite amplitude clamp, permission/reactivation, and g_pad-to-shared-memory ring wrap/release. Production backend code passes WinMM mapping, pre-XInput lock initialization, amplitude conversion, motor ownership, coalescing, immediate zero, and shutdown using a mock XInput device. `git diff --check` also passes.
- `build/controllers-20261115-193136-981751900/checks.log`: actual shared read-only USB open succeeded, with valid report 01, neutral buttons and centered sticks. This proves a live device/report path, not physical button correctness.
- `build/controllers-20261115-193231-788109100/checks.log`: output ownership was refused with **Windows error 32** (sharing violation). The probe fell back to shared read-only input and deliberately sent **no output reports**. Another agent's `Mario Kart World 180` process (PID 29744 at the subsequent process check) was running. The exact handle owner was not established; no process was stopped or altered. The earlier 48-versus-63 capability mismatch and test-harness link failures remain in separate evidence directories.

The USB light and vibration response, physical buttons, focus switching, physical disconnect/reconnect and Bluetooth operation still require manual checks after deployment. No synthetic test establishes those physical results.

## Build-owner handoff

The controller changes are in `runtime/backend_win32.c`, `runtime/svc_hid.c`, the two controller headers and controller-specific tests. The old rendering changes in backend_win32.c remain intact. Headers are included directly, so the normal Win32 runtime build picks up the implementation without adding a library or source-file entry. The other agent/parent owns the full runtime build and deployment.

The backend exports `backend_controller_shutdown(void)` for renderer-owned immediate exits. **Before direct `ExitProcess` / `TerminateProcess` calls outside this backend, call it on Win32** (an `extern` declaration at the call site is sufficient). These APIs bypass `atexit`; the controller task did not edit the other agent's NVN paths or main.c. Normal WM_CLOSE/Esc and guest `exit()` are already covered. Abrupt external process termination cannot guarantee a final hardware write.

Suggested changelog entry for the parent to merge with its existing dirty changelog:

> Windows: native DualSense and DualShock 4 input now uses checked HID reports and supports player lights and classic rumble over supported USB/Bluetooth report layouts. Fixed vibration handle byte order and inline SendVibrationValue parsing. Preserved Sony Cross-to-confirm and the corrected shoulder/stick-click mapping. Output ownership failures are logged and leave shared input available.

Manual acceptance after the parent deploys:

1. Focus the game. Check the model/transport/ownership log, blue player light and DualSense center indicator.
2. At the L+R prompt use L1+R1, not stick clicks. Verify every mapped button, both sticks, diagonals and trigger release. Cross confirms; Circle goes back.
3. With vibration enabled in the game, verify a rumble event and its stop. Trace should show valid service handles/values followed by a successful Sony output, not just a service request.
4. Alt-tab and return, then unplug/reconnect. Buttons must release, rumble must stop, and reconnect must start with zero rumble before new guest requests.
5. Close normally while rumble is active. Test direct renderer shutdown after adding the exported shutdown hook. Repeat on Bluetooth separately; do not infer it from USB.

## Protocol references and attribution

- Linux [`hid-playstation.c`, v6.12](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c): Sony report layouts, compatible rumble, lightbar setup, sequence/tag and CRC. Copyright (c) 2020-2022 Sony Interactive Entertainment, GPL-2.0-or-later. Adapted protocol code retains that attribution in `controller_sony.h`; this project's combined code is AGPL v3.
- SDL [`SDL_hidapi_ps5.c`, SDL2](https://github.com/libsdl-org/SDL/blob/SDL2/src/joystick/hidapi/SDL_hidapi_ps5.c): cross-checked the 48-byte USB output used on Windows and simple Bluetooth layout. No SDL library or copied SDL implementation is added.
- libnx [`services/hid.c`](https://github.com/switchbrew/libnx/blob/master/nx/source/services/hid.c) and [`services/hid.h`](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/hid.h): vibration handle field order, style indices, inline command 201 and pointer-buffer command 206. Used as protocol references, not imported implementation.
