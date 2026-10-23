// AirPlay source-state semantics from UxPlay raop_rtp_mirror.c (GPL-3.0-or-later).
// Packet arrival proves transport activity; only a validated video access unit counts as a frame.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool suspended, have_video;
  uint16_t codec_option;
  uint32_t packets, frames, reports, transitions;
  int64_t last_packet_us, last_frame_us;
} ap_source_state;

static inline ap_source_state ap_source_packet(ap_source_state s, uint8_t type,
                                               uint16_t option, int64_t now_us) {
  s.packets++;
  s.last_packet_us = now_us;
  if (type == 0x02 || type == 0x05) s.reports++;
  if (type != 0x01) return s;
  s.codec_option = option;
  bool suspended = s.suspended;
  if (option == 0x0156 || option == 0x015e) suspended = true;
  if (option == 0x0116 || option == 0x011e) suspended = false;
  if (suspended != s.suspended) {
    s.suspended = suspended;
    s.have_video = false;  // Resuming needs a new frame, not the pre-pause image.
    s.transitions++;
  }
  return s;
}

static inline ap_source_state ap_source_frame(ap_source_state s, int64_t now_us) {
  s.frames++;
  s.last_frame_us = now_us;
  s.have_video = !s.suspended;  // An unexpected frame cannot clear explicit suspension.
  return s;
}

static inline int64_t ap_source_frame_age_ms(ap_source_state s, int64_t now_us) {
  return s.frames ? (now_us - s.last_frame_us) / 1000 : -1;
}

// iOS reports arrive about once a second but gaps over 4 s were measured; a stalled source is detected
// by the app's pointer probe, so only a long silence ends "streaming" here.
#define AP_SOURCE_QUIET_US 10000000

static inline bool ap_source_streaming(ap_source_state s, int64_t now_us) {
  return !s.suspended && s.have_video && s.packets
      && now_us >= s.last_packet_us && now_us - s.last_packet_us < AP_SOURCE_QUIET_US;
}
