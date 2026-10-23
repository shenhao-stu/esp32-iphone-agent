#pragma once
#include <cstdint>

enum class HidReportAttempt { accepted, congested, failed };

// Only a report refused by the host may be retried. Admission is not proof of delivery to the phone.
template<class Send, class Valid, class Clock, class Wait>
bool hid_send_report(Send send, Valid valid, Clock clock, Wait wait) {
  const uint32_t started = clock();
  for (;;) {
    if (!valid()) return false;
    switch (send()) {
      case HidReportAttempt::accepted: return true;
      case HidReportAttempt::failed: return false;
      case HidReportAttempt::congested: break;
    }
    if (clock() - started >= 1500) return false;
    wait();
  }
}
