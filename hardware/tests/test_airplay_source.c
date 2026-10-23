#include <assert.h>
#include <stdio.h>
#include "../hid_bridge/src/airplay/source_state.h"

int main(void) {
  ap_source_state s = {0};
  assert(!ap_source_streaming(s, 0));
  assert(ap_source_frame_age_ms(s, 0) == -1);
  s = ap_source_packet(s, 1, 0x0116, 1000);
  assert(!ap_source_streaming(s, 1000));
  s = ap_source_packet(s, 0, 0, 2000);
  s = ap_source_frame(s, 2000);
  assert(ap_source_streaming(s, 2000));
  assert(ap_source_streaming(s, 4502000));  // measured report gap: a quiet source is not a stopped one
  assert(!ap_source_streaming(s, 2000 + AP_SOURCE_QUIET_US));

  // A static screen may send only reports, but its frame age must keep increasing.
  s = ap_source_packet(s, 5, 0, 5002000);
  assert(ap_source_streaming(s, 5002000));
  assert(s.frames == 1 && s.reports == 1);
  assert(ap_source_frame_age_ms(s, 5002000) == 5000);

  // The codec header changes source state even with no codec payload.
  for (unsigned pause = 0x0156; pause <= 0x015e; pause += 8) {
    s = ap_source_packet(s, 1, pause, 6000000);
    assert(s.suspended && !ap_source_streaming(s, 6000000));
    for (unsigned type = 0; type < 256; type++) {
      if (type == 1) continue;
      ap_source_state other = ap_source_packet(s, type, 0x0116, 6001000);
      assert(other.suspended && !ap_source_streaming(other, 6001000));
    }
    // No unknown option, including a different upper byte, may resume the source.
    for (unsigned option = 0; option <= 0xffff; option++) {
      if (option == 0x0116 || option == 0x011e) continue;
      assert(ap_source_packet(s, 1, option, 6002000).suspended);
    }
    s = ap_source_frame(s, 6003000);
    assert(s.suspended && !ap_source_streaming(s, 6003000));
    s = ap_source_packet(s, 1, pause - 0x40, 6004000);
    assert(!s.suspended && !ap_source_streaming(s, 6004000));
    s = ap_source_frame(s, 6005000);
    assert(ap_source_streaming(s, 6005000));
    // Repeated normal codec packets must not discard an already active frame.
    s = ap_source_packet(s, 1, pause - 0x40, 6006000);
    assert(ap_source_streaming(s, 6006000));
  }
  assert(s.transitions == 4 && s.frames == 5);
  assert(!ap_source_streaming(s, s.last_packet_us - 1));
  s = (ap_source_state){0};
  s = ap_source_packet(s, 2, 0x011e, 9000000);
  assert(!s.suspended && !ap_source_streaming(s, 9000000));
  assert(s.frames == 0 && ap_source_frame_age_ms(s, 9000000) == -1);
  puts("AirPlay source pause/resume, heartbeat, frame age and reset passed");
}
