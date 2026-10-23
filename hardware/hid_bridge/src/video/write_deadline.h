#pragma once
#include <cstdint>

struct VideoWriteDeadline {
  uint32_t started;
  uint32_t progressed;

  explicit VideoWriteDeadline(uint32_t now) : started(now), progressed(now) {}
  void progress(uint32_t now) { progressed = now; }
  bool expired(uint32_t now) const {
    return now - progressed >= 2000 || now - started >= 10000;
  }
};
