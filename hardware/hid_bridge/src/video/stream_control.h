#pragma once
#include <atomic>
#include <cstdint>

// One atomic snapshot keeps enablement and replay generation together across task switches.
class VideoStreamControl {
  std::atomic<uint32_t> value{0};
public:
  uint32_t epoch() const { return value.load(); }
  bool isCurrent(uint32_t observed) const { return (observed & 1) && value.load() == observed; }
  void set(bool on) {
    uint32_t before = value.load();
    while (bool(before & 1) != on && !value.compare_exchange_weak(before, before + 1)) {}
  }
};
