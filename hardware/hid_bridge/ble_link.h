// BLE GATT link to the iOS app (docs/ble-protocol.md): cmd/reply, status heartbeat,
// video chunks and the encrypted `net` characteristic. h2zero NimBLE-Arduino; build with flash.sh
// (it force-includes nimble_cfg.h, the host buffer sizes).
#pragma once
#include <Arduino.h>

void ble_begin(const String &name);
bool ble_connected();
bool ble_control_connected(); // app reply subscription; system HID alone is not a controller
bool ble_pop_cmd(String &line, uint32_t &generation, uint32_t wait_ms);  // next `cmd` write, oldest first
void ble_reply(const String &line);                // queued; sent before any further video
// Reads always serve full JSON. Each subscriber gets full/compact only when it fits its own MTU.
void ble_status(const String &full, const String &compact);  // latest wins; before further video
size_t ble_video_room();                           // payload bytes per video notify; 0 = nobody listening
bool ble_video_send(const uint8_t *p, size_t n);   // waits for free buffers; false = gave up
void ble_unpair();

uint16_t ble_mtu();          // diagnostic minimum over connected peers; 23 with none
uint16_t ble_video_mtu();    // minimum over encrypted, bonded video subscribers; 0 with none
uint8_t ble_phy();       // 1 or 2 (Mbit)
uint32_t ble_kbps();     // video bytes out, last second
uint32_t ble_drops();    // video notifies we failed to send
uint32_t ble_reply_drops();  // replies that never got out to a subscribed peer
int ble_peers();         // connected centrals
