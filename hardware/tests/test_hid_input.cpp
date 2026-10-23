#include <cassert>
#include <cstdio>
#include <string>
#include "../hid_bridge/src/hid/serial_line.h"
#include "../hid_bridge/src/hid/report_send.h"

static void complete_serial_commands_only() {
  SerialLine line;
  const std::string command = "948010 type " + std::string(418, 'a');
  for (char c : command) assert(line.add(c, 0) == SerialLine::pending);
  assert(line.add('\n', 5) == SerialLine::complete);
  assert(line.text() == command); // entire 418-byte payload, not the old 244-byte prefix

  for (char c : command.substr(0, 256)) assert(line.add(c, 10) == SerialLine::pending);
  assert(!line.expire(1009));
  assert(line.expire(1010)); // missing delimiter is never a command
  assert(!line.expire(1011));
  for (char c : command.substr(256)) assert(line.add(c, 1100) == SerialLine::pending);
  assert(line.add('\n', 1101) == SerialLine::rejected); // cannot execute a late tail either

  for (char c : std::string("7 ping\r")) assert(line.add(c, 1200) == SerialLine::pending);
  assert(line.add('\n', 1201) == SerialLine::complete);
  assert(std::string(line.text()) == "7 ping");
  for (int i = 0; i < 700; i++) assert(line.add('x', 1300) == SerialLine::pending);
  assert(line.add('\n', 1301) == SerialLine::rejected);
  for (char c : std::string("8 ping")) assert(line.add(c, 1400) == SerialLine::pending);
  line.add(0, 1401);
  assert(line.add('\n', 1402) == SerialLine::rejected);
  line.add('9', UINT32_MAX - 500);
  assert(!line.expire(498));
  assert(line.expire(499));
}

static void refused_reports_retry_but_accepted_reports_never_repeat() {
  uint32_t time = 0;
  int attempts = 0, accepted = 0;
  bool sent = hid_send_report([&] {
    attempts++;
    if (time < 120) return HidReportAttempt::congested;
    accepted++;
    return HidReportAttempt::accepted;
  }, [] { return true; }, [&] { return time; }, [&] { time += 5; });
  assert(sent && accepted == 1 && time == 120 && attempts > 10);

  attempts = 0;
  sent = hid_send_report([&] { attempts++; return HidReportAttempt::failed; },
                        [] { return true; }, [&] { return time; }, [&] { assert(false); });
  assert(!sent && attempts == 1); // a permanent failure must not be replayed

  time = UINT32_MAX - 100;
  const uint32_t start = time;
  sent = hid_send_report([] { return HidReportAttempt::congested; },
                        [] { return true; }, [&] { return time; }, [&] { time += 5; });
  assert(!sent && time - start == 1500); // bounded backpressure, including timer rollover

  bool sameOwner = true;
  attempts = 0;
  sent = hid_send_report([&] { attempts++; return HidReportAttempt::congested; },
                        [&] { return sameOwner; }, [&] { return time; }, [&] { sameOwner = false; });
  assert(!sent && attempts == 1); // a wait cannot redirect a refused report to a new host

  bool stopped = false;
  attempts = 0;
  sent = hid_send_report([&] { attempts++; return HidReportAttempt::congested; },
                        [&] { return !stopped; }, [&] { return time; }, [&] { stopped = true; });
  assert(!sent && attempts == 1); // stop interrupts the wait without another press
}

int main() {
  complete_serial_commands_only();
  refused_reports_retry_but_accepted_reports_never_repeat();
  std::puts("Serial framing and HID report admission tests passed");
}
