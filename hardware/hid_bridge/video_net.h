// Optional TLS-PSK video transport. The random session key arrives only over the bonded BLE command link.
#pragma once
#include <Arduino.h>

String video_net_offer(const String &key); // 32-byte hex key, or "off"; never returns or logs the key
void video_net_close();
void video_net_poll();                    // video task owns all sockets and TLS state
bool video_net_ready();
bool video_net_send(const uint8_t *data, size_t length); // u16 BE length + existing video chunk, inside TLS
uint32_t video_net_kbps();
