#pragma once
#include <cstdint>

struct VideoWriteDeadline {
  uint32_t started;
  uint32_t progressed;

  explicit VideoWriteDeadline(uint32_t now) : started(now), progressed(now) {}
  void progress(uint32_t now) { progressed = now; }
  bool expired(uint32_t now) const {
    // Allow the SDK's initial 3 s TCP retransmission before discarding the H.264 reference stream.
    return now - progressed >= 5000 || now - started >= 10000;
  }
};
