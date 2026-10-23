#include <cassert>
#include <cstdio>
#include "../hid_bridge/src/video/write_deadline.h"

int main() {
  VideoWriteDeadline slow(100);
  for (uint32_t time = 1000; time < 10000; time += 1000) {
    slow.progress(time);
    assert(!slow.expired(time)); // a slow TCP window still makes progress
  }
  assert(slow.expired(10100)); // bound total time even with a trickling peer
  VideoWriteDeadline stalled(50);
  stalled.progress(800);
  assert(!stalled.expired(3800)); // retain the reference stream across a short TCP retransmission stall
  assert(!stalled.expired(5799));
  assert(stalled.expired(5800)); // stop a peer which made no progress for 5 s
  VideoWriteDeadline wrap(UINT32_MAX - 1000);
  wrap.progress(100);
  assert(!wrap.expired(5099));
  assert(wrap.expired(5100)); // millis() rollover does not extend the deadline
  std::puts("Video write deadline tests passed");
}
