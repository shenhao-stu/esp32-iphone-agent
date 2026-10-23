# BLE protocol (ESP32 ⇄ iOS app)

The board is a BLE peripheral named `ESP32 Agent XXXX` (XXXX = last 2 MAC bytes, hex).
Security: LE Secure Connections, bonding, Just Works. `cmd` and `net` writes require encryption
and a bond. `net` reads require encryption, so reading it triggers the iOS pairing prompt.
Replies and video are sent only to encrypted, bonded peers. Public status contains no network password.

The same server exposes standard HID service `0x1812`: mouse report 1 (5 bytes), keyboard report 2
(8 bytes), consumer report 3 (2 bytes). These IDs belong to BLE, independently of the USB descriptor.
HID notifications go to one subscribed, encrypted and bonded input host. The app's GATT control
connection alone does not prove that iOS has paired or enabled its input device.

Service `6e400001-a9e8-4b1b-9c39-6d6f8a3c0000`

| Characteristic | UUID suffix | Props | Purpose |
|---|---|---|---|
| `cmd`    | `...0001` | write, write-no-rsp | one command line, UTF-8, `<id> <command>` |
| `reply`  | `...0002` | notify | `<id> ok` or `<id> err <reason>` |
| `status` | `...0003` | notify, read | JSON heartbeat, every 1 s (also keeps the app alive in background) |
| `video`  | `...0004` | notify | H.264 chunks, see below |
| `net`    | `...0005` | read, write (encrypted) | network config JSON |

Full UUID = `6e400001-a9e8-4b1b-9c39-6d6f8a3c` + 4-hex suffix, e.g. cmd = `6e400001-a9e8-4b1b-9c39-6d6f8a3c0001`.

## cmd
`<id>` is a decimal request id chosen by the app; the reply echoes it.
Existing HID commands, unchanged semantics from the old TCP protocol:
`ping`, `home`, `move DX DY`, `press`, `release`, `click`, `scroll A B`, `type TEXT`, `key COMBO`.
New:
- `hid ble|usb` — select and persist the input transport (new installations default to `ble`).
  Switching releases held inputs. USB attachment is not an automatic transport switch.
- `stop` — interrupt active paced input, cancel older queued input and release held mouse/keyboard/consumer reports.
  Do not automatically replay a command after timeout: it may already have executed partially.
- `stream on|off` — continuously forward every access unit over `video` (default off).
- `snap` — send SPS/PPS + every AU since the last IDR (the current GOP), then reply `ok`.
- `res W H` — AirPlay resolution advertised in `/info` (default 480 1040), stored in NVS; takes effect on the next mirroring session.
- `fps N` — max fps advertised (default 5), stored in NVS.
- `bench KB` — send KB kilobytes of filler on `video` with the `bench` flag, then reply `ok <ms>`.
- `unpair` — erase all bonds and restart advertising.
- `coex [bt|balance|wifi]` — Wi-Fi/BLE radio-share preference (A/B knob, not stored; every boot starts at
  `balance`). Replies `ok <current>`; bare `coex` just reports it.
When the link drops, the board releases all buttons/keys (same as before).

## status (JSON, one notify per second, must fit one MTU; keep short keys)
```json
{"up":123456,"rst":"power-on","heap":81234,"psram":6200000,"usb":"ok|asleep|off",
 "net":"ap|sta|none","ip":"192.168.4.1","sta":1,
 "hid":"ble|usb","hid_ready":false,"ble_hid":"off|pairing|ready","hid_caps":"mouse,keyboard,consumer",
 "ap":"idle|connected|streaming|failed","w":480,"h":1040,"kbps":180,"fps":4.8,"idr":2000,"gop":42000,
 "ovf":0,"trim":0,"anc":1,"lag":0,
 "ble":{"mtu":247,"vmtu":247,"phy":1,"kbps":350,"drop":0,"rdrop":0,"n":1}}
```
`kbps` = AirPlay video bitrate in, `ble.kbps` = video bytes out over BLE, `idr` = ms between IDRs, `gop` = bytes buffered since last IDR.
`rst` = reset reason of the current boot (`power-on|brownout|watchdog|crash|software|usb|other`), `up` = ms since boot.
`rst` is diagnostic data available in full status; compact heartbeats omit it to fit smaller MTUs.
`ble.drop` = video notifies given up, `ble.rdrop` = replies that never reached a subscribed peer (should stay 0),
`ble.n` = connected centrals.
`ble.mtu` is the diagnostic minimum over all connected peers (23 with none); `ble.vmtu` is the
minimum over encrypted, bonded peers actually subscribed to video (0 with none). `vmtu` is present
only in full status and does not change the meaning of the legacy `mtu` field.
`hid_ready` refers only to the selected transport; `usb:"ok"` does not imply BLE input readiness.
`lag` is the number of bytes still pending in the continuous stream, or -1 when unavailable.
`ovf` counts GOP overflows, `trim` counts reclaimed sent prefixes, and `anc` indicates a retained IDR
anchor. Missing `hid` fields identify legacy USB firmware.

Send priority: replies and status are queued and always go out before further video; video keeps at most
~8 ACL packets (~2 KB) in the controller during command traffic. Heartbeat `ping` does not enable
that video cap. In `stream on`, every P-frame is retained in sequence: iOS may send only one IDR at
the start of mirroring. Already-sent prefixes are reclaimed as the GOP buffer fills. If recovery loses
the required IDR, restart system Screen Mirroring. Clients must pause actions while `lag` is positive.

## video
Each notify: `[seq u16 LE][flags u8][payload]`, payload up to `min(MTU - 3, 512) - 3` bytes.
The first subtraction is ATT overhead; the second is this protocol's chunk header. Thus a video
receiver MTU of 517 allows 509 payload bytes, and MTU 247 allows 241. The 512-byte cap is the
[Bluetooth ATT attribute-value limit](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-62/out/en/host/attribute-protocol--att-.html).
Reply/video sizing considers only their actual encrypted, bonded subscribers. A connected or
status-only low-MTU debug peer does not reduce video capacity. With two eligible video recipients,
their smaller MTU applies. No eligible recipient means
zero capacity. Before each send/retry, the board rechecks the target's subscription, security and MTU;
an oversized queued chunk fails instead of being silently truncated. A failed stream may need Screen
Mirroring restarted, as described above. The 1M PHY, requested MTU 517, DLE 251 and 15 ms connection
interval are unchanged; capacity changes are not measured throughput claims.
flags: bit0 START of access unit, bit1 END of access unit, bit2 IDR, bit3 CONFIG (payload is SPS+PPS),
bit4 BENCH (filler, ignore), bit5 SNAP (part of a snap reply).
Payload is Annex-B (`00 00 00 01` start codes). On a START chunk the first 4 payload bytes are a u32 LE
board timestamp (ms) for latency stats, followed by the Annex-B data.
`seq` increments per notify; the app counts gaps as drops and waits for the next IDR/CONFIG after a gap.

## net (JSON)
Read: `{"mode":"ap","ssid":"GAgent-1A2B","pass":"<random 12 chars>","ip":"192.168.4.1"}`

This fork migrates the matching factory `ESP32Agent-<board suffix>` name to `GAgent-<board suffix>` on boot. It preserves custom SSIDs, the hotspot password, station credentials and Bluetooth bonds. Bluetooth and AirPlay names are unchanged.
(SoftAP credentials are generated once at first boot and kept in NVS).
Write `{"mode":"ap"}` or `{"mode":"sta","ssid":"...","pass":"..."}` — stored in NVS, board restarts Wi-Fi.

## AirPlay
Receiver name `ESP32 Agent`, mirroring only (no audio), H.264 only, resolution/fps from NVS.

## Deviations (firmware notes)
- `status`: each public-status subscriber independently receives full JSON if it fits its MTU,
  otherwise a reduced object with connection/input fields and current `lag`/`fps`. If even that
  does not fit (e.g. MTU 23), only that subscriber's notification is skipped; a low-MTU debug peer
  cannot suppress the phone's heartbeat. No partial JSON or notification missing fresh video metrics
  is sent. A `status` read always returns the last full JSON, using ATT long reads.
- `sta` in `status` = number of stations on the board's hotspot (mode `ap`), or 1/0 = connected (mode `sta`).
- `net` read in `sta` mode returns the station network's `ssid`/`pass`.
- `snap` while mirroring has stopped returns the last buffered GOP (check `"ap"` in status); `snap` without
  any video replies `err no video ...`.

## Wi-Fi video (v1)

New firmware advertises `vnet:1` in full status. The paired app can send `vnet <64 lowercase hex characters>` over the existing encrypted, bonded command characteristic. The value is a fresh 32-byte random key for this connection, never a saved Wi-Fi password. Reply: `ok 7002`. Keys and commands containing them must not be logged. `vnet off`, loss of the BLE controller, an authentication/I/O failure, or a new offer closes the old session. A pending offer expires after 10 seconds.

TCP port 7002 requires TLS 1.2, identity `gagent-video-v1`, cipher `TLS_PSK_WITH_AES_128_GCM_SHA256`; there is no plaintext video endpoint or certificate verification bypass for certificate-based TLS. The PSK authenticates both peers. One client consumes an offer. Reconnection uses a fresh key. Only the video task touches TLS/socket state; the command task passes a bounded offer. The stock SDK allocates two 16KiB TLS buffers; setup fails if less than 12KiB internal heap remains.

AirPlay CTR decryption uses a fixed 1KiB internal working buffer, preserving counter/offset/partial keystream across chunks and packets. This avoids transient PSRAM DMA staging allocations under TLS/Wi-Fi memory pressure. Any AES or AVCC validation error latches `ap:failed` until the next mirroring session. Clients must invalidate the old frame and stop actions; heartbeat traffic alone does not prove a usable video chain. A live but quiet session can report `connected` after three seconds without data: disable frame-dependent actions until `streaming` resumes, but retain the decoder reference chain. Clear it on `idle` or `failed`, not on this transient state.

Debug builds offer serial-only `0 crypto-test` (ECB-reference versus CTR, synthetic public key/data) and explicit `0 crypto-pressure-test` (also reserves internal heap to reproduce allocation failures). Both require AirPlay idle. The pressure test can interrupt the video socket and is a fault-injection check, never a throughput/stability result; reconnect the app afterwards. It never reads a user's video or session keys. These commands are absent from release builds and BLE commands.

Inside TLS: `[length u16 BE][video chunk]`, length 3–16003. Chunks retain the BLE sequence/flags/timestamp format. Each access unit chooses one transport. The app stops the old stream and resets its assembler before switching, avoiding mixed BLE/Wi-Fi fragments. `stream on/off` and `snap` use authenticated Wi-Fi when ready, otherwise BLE. H.264 reference units are never skipped. `vtransport` reports `wifi` or `ble`, `vkbps` reports Wi-Fi video bytes per second ×8/1000; `ble.kbps` remains BLE-only. `lag` still measures pending source bytes, not TLS/TCP buffers.

`wbench KB` sends the same 0xA5 diagnostic payload as `bench` over authenticated Wi-Fi; `bench` remains BLE-only. `hardware/tools/wifi_video_probe.py` checks wrong-key rejection, record sequence, payload integrity and rate without viewing the phone. USB serial in this harness is only for setup/replies; all measured bytes travel over Wi-Fi. A synthetic test with AirPlay off is not concurrent mirroring or long-term acceptance.

Transport loss does not make P-frames independently decodable. Once the board has trimmed the IDR, a restarted decoder still needs a new mirror session. The app explains that recovery and blocks actions on stale frames.

## Nearby Wi-Fi discovery

`wifi-scan start` begins an asynchronous board-side scan and replies with a numeric revision. `wifi-scan` returns `{"id":1,"state":"scanning|ready|failed|idle","count":0}`. Once ready, `wifi-scan <revision> <index>` returns one `{"ssid":"Home","rssi":-50,"secure":true,"supported":true}` record. Each record fits one reply; at most 20 unique visible SSIDs are retained, strongest first. A stale revision or out-of-range index is rejected. No passwords are returned or saved by scanning.

Only loop() owns scan state. Scans time out after 10 seconds, release the driver's results, and suspend STA reconnect attempts until finished. Starting a new network configuration cancels the scan. AP mode temporarily enables STA scanning and restores AP-only mode afterwards; existing credentials and NVS are preserved. Scanning is rejected while AirPlay is active to avoid disrupting its shared radio. Hidden SSIDs remain manual input; enterprise/WEP authentication is shown as unsupported.

TLS writes allow at most 2 seconds without TCP BIO progress and 10 seconds overall. A partial socket write resets the idle deadline even when TLS still returns WANT_WRITE; the caller retains the same TLS buffer until accepted. Failure logs include elapsed and stalled milliseconds, never key material. This bounds a stalled peer without treating a slow, progressing record as an immediate disconnect.
