#pragma once
#include <Arduino.h>

// Called only by loop(), so scanning, result ownership and network changes cannot race.
String net_scan_command(const String &argument);
void net_scan_poll();
void net_scan_cancel();
bool net_scan_running();
