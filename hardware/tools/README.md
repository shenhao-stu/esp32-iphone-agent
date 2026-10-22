# Testing the board from a Mac

`board_test.py` plays the iOS app over BLE (docs/ble-protocol.md); the Mac's own Screen
Mirroring feeds the board's AirPlay receiver, so AirPlay -> PSRAM GOP -> BLE -> H.264 can be
checked without the iPhone. Needs `uv`, `arduino-cli` with esp32 core 3.3.x, `ffmpeg`, and the BLE host
library (once): `arduino-cli lib install "NimBLE-Arduino"` (2.5.x). Always build with `hardware/flash.sh`:
it force-includes `hid_bridge/nimble_cfg.h` (NimBLE buffer sizes); a plain IDE build fails a static_assert.

## 1. Flash a test build (board plugged into the Mac)

```bash
cp hardware/hid_bridge/secrets.example.h hardware/hid_bridge/secrets.h   # once; fill in the Mac's Wi-Fi
hardware/flash.sh --sta --debug    # joins that Wi-Fi instead of opening its hotspot; logs on USB serial
```
Watch the log (AirPlay handshake steps, boot heap/PSRAM line):
```bash
arduino-cli monitor -p /dev/cu.usbmodem* -c baudrate=115200
```
Without `--sta` the board opens its own hotspot `ESP32Agent-XXXX` (password: `net` read below);
the Mac can join that instead, but then it has no internet.

## 2. BLE only (no mirroring yet)

```bash
uv run hardware/tools/board_test.py info        # pairs (macOS prompt), reads `net`, prints status
uv run hardware/tools/board_test.py bench --kb 512
uv run hardware/tools/board_test.py cmd "hid usb" "stop"  # explicitly target the USB host (Mac)
uv run hardware/tools/board_test.py hid         # moves/clicks the selected host's pointer
uv run hardware/tools/board_test.py stress --seconds 20 --kb 512   # bench filler + ping/move every 200 ms
```
`stress` prints reply latency p50/p95/max and missing replies (target: 0 missing, p95 < 500 ms);
`--load stream` uses `stream on` instead (needs mirroring). The Mac receives only ~100-200 kbps, so use the
iPhone for throughput: with `--debug`, send `N bench 512` on the USB serial; the result is echoed as
`video: N ok <ms>` (kbps = 512*8*1024/ms). A/B the radio: `N coex bt|balance|wifi` on serial, and
`hardware/flash.sh --ap-tune` (20 MHz hotspot, 300 TU beacons) vs a normal build.
`net` passwords are masked in the output.

## 3. AirPlay -> BLE

1. Control Center -> Screen Mirroring -> **ESP32 Agent** (the Mac must be on the same Wi-Fi).
   The serial log should show `GET /info`, `pair-setup`, `pair-verify 1/2`, `fp-setup 1/2`,
   `setup: mirror stream`, `mirror: data connection accepted`, `mirror: SPS ... PPS`.
2. Then:
```bash
uv run hardware/tools/board_test.py snap        # -> hardware/tools/out/snap.h264 + snap.png (snap latency)
uv run hardware/tools/board_test.py stream --seconds 20   # -> out/stream.h264 + stream.png, kbps, gaps
uv run hardware/tools/board_test.py all         # info, bench, snap, stream, hid, status
uv run hardware/tools/board_test.py cmd "res 480 1040" "fps 5"   # any command lines
```
`ffplay hardware/tools/out/stream.h264` plays the stream.

## 4. Back to the iPhone

Reflash without `--sta` (`hardware/flash.sh`). The firmware now defaults to **BLE HID**;
USB remains available through the explicit `hid usb` command.

### Wireless setup and transport contract

1. Power the board from a power bank or USB supply. The iPhone does not need a cable to the board.
2. In iOS Settings → Bluetooth, pair **ESP32 Agent XXXX**. The app GATT link and standard
   HID mouse/keyboard/consumer service share the same NimBLE server. Existing firmware bonds
   may cache the old service list: forget the board in iOS, run `unpair`, then pair again if HID
   is missing after the firmware update.
3. Enable AssistiveTouch, connect the board Wi-Fi (or configure an existing network), and start
   Screen Mirroring to **ESP32 Agent**. Wi-Fi carries AirPlay; it does not inject iOS touches.
4. Connect GAgent to the board and read status. `hid=ble`, `ble_hid=ready`, `hid_ready=true`
   mean that the encrypted, bonded HID host subscribed to mouse, keyboard and consumer reports.
   They do not prove AssistiveTouch, pointer calibration or AirPlay is ready.
5. After connecting/reconnecting, send `stop` to release inputs and clear a previous disconnect
   stop. Calibrate the pointer before starting an agent task.

`hid` reads the persisted selection (`ok ble` or `ok usb`). `hid ble` / `hid usb` changes it.
There is no automatic fallback: USB power must not accidentally control the supplying computer.
Switching sends releases on the old transport first. If USB is selected, connect the board to the
intended iPhone USB host. Unplugging USB data does not change the selected transport.

The status heartbeat preserves `usb=off|asleep|ok` and adds `hid=ble|usb`, `hid_ready` (boolean),
`ble_hid=off|pairing|ready`, and `hid_caps="mouse,keyboard,consumer"` (full status only). Missing
`hid` means an older USB-only firmware; clients should fall back to `usb == "ok"`.
`pairing` also covers HID suspension or incomplete report subscriptions. Errors identify an
unready transport, unsupported non-ASCII text, invalid coordinates, refused reports, or a stop.
Do not automatically retry a timed-out HID command: it may have partially executed.

`stop` interrupts paced moves/typing between reports and releases mouse, keyboard and consumer
keys. Losing the app reply subscription or the HID connection latches the same interruption;
a new `stop` clears it. `move` requires two integers in [-4000,4000], `scroll` two in [-127,127],
`type` at most 480 ASCII bytes, and `key` validates the entire combo before pressing anything.
These checks also apply to USB. Reports target only one bonded HID host, never every BLE peer.

This wireless firmware has host-test and build verification; iPhone Bluetooth pairing, actual
pointer/keyboard behavior, stop timing and AirPlay coexistence still require on-device acceptance.
Do not use the Mac BLE harness at the same time as this acceptance test.

## TODO / next optimizations (measured 2026-10-15, NimBLE-Arduino 2.5.1, board on the Mac + iPhone app connected)

- Throughput vs latency: `bench 512` to the iPhone (serial) was ~381 kbps (balance), ~412 (`coex bt`),
  ~364 (`coex wifi`) with only the msys reserve gate; 30 ms interval halved it (180 kbps). With the
  controller-credit cap while commands flow (VIDEO_CTRL_PKTS = 8, CMD_ACTIVE_MS = 1000) it is
  ~130-170 kbps, because the app's periodic pings keep the cap on. Mac stress (bench load, ping/move
  every 200 ms): 0 missing, p50 33-66 ms, p95 83-156 ms, max 93-262 ms (before the cap: p95 0.96-2.2 s).
  Next: cap per connection instead of globally, tune VIDEO_CTRL_PKTS/CMD_ACTIVE_MS on the iPhone, or
  ignore the app's keepalive pings when deciding "commands flow".
- L2CAP CoC for video (1 channel x 247 MPS is already configured, unused).
- Coex A/B on the iPhone with live mirroring (`coex`, `--ap-tune`); not measured with AirPlay running yet.
- Stream: IDR-only fallback (P-frames > 800 ms old are skipped) not yet exercised with live mirroring
  (`board_test.py stress --load stream`).
