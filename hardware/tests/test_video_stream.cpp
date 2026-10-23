#include <cassert>
#include <cstdio>
#include "../hid_bridge/src/video/stream_control.h"

int main() {
  VideoStreamControl stream;
  assert(!stream.isCurrent(stream.epoch()));
  stream.set(true);
  const uint32_t first = stream.epoch();
  assert(stream.isCurrent(first));
  stream.set(true);
  assert(stream.isCurrent(first)); // duplicate commands do not replay an active stream

  stream.set(false);
  stream.set(true); // both commands arrive before the video task runs again
  assert(!stream.isCurrent(first)); // old AU/chunk must stop, despite enablement being true again
  const uint32_t resumed = stream.epoch();
  assert(resumed != first && stream.isCurrent(resumed));
  stream.set(false);
  const uint32_t stopped = stream.epoch();
  assert(!stream.isCurrent(resumed) && !stream.isCurrent(stopped));
  stream.set(false);
  assert(stream.epoch() == stopped);
  stream.set(true);
  assert(stream.isCurrent(stream.epoch()) && stream.epoch() != resumed);
  std::puts("Video stream epoch tests passed");
}
