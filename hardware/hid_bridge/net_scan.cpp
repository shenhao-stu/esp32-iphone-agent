#include "net_scan.h"
#include "src/airplay/airplay.h"
#include <WiFi.h>
#include <esp_wifi.h>

static constexpr int LIMIT = 20;
static String networks[LIMIT];
static String names[LIMIT];
static int count;
static uint32_t revision, started;
static const char *state = "idle";
static bool running, restoreAP;

static void finish() {
  running = false;
  WiFi.scanDelete();
  if (restoreAP) { WiFi.enableSTA(false); restoreAP = false; }
}

void net_scan_cancel() {
  if (running) esp_wifi_scan_stop();
  finish();
  state = "idle";
  count = 0;
  for (auto &entry : networks) entry = "";
  for (auto &name : names) name = "";
}

void net_scan_poll() {
  if (!running) return;
  int result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING && millis() - started < 10000) return;
  state = result >= 0 ? "ready" : "failed";
  // The SDK sorts by signal strength; keep a bounded snapshot, then free the scan allocation.
  for (int i = 0; i < result && count < LIMIT; i++) {
    String ssid = WiFi.SSID(i), escaped;
    if (ssid.isEmpty()) continue;
    bool duplicate = false;
    for (int k = 0; k < count; k++) if (names[k] == ssid) duplicate = true;
    if (duplicate) continue;
    bool printable = true;
    for (uint8_t ch : ssid) {
      if (ch < 32 || ch == 127) { printable = false; break; }
      if (ch == '"' || ch == '\\') escaped += '\\';
      escaped += (char)ch;
    }
    if (!printable) continue;
    auto auth = WiFi.encryptionType(i);
    bool supported = auth == WIFI_AUTH_OPEN || auth == WIFI_AUTH_WPA_PSK || auth == WIFI_AUTH_WPA2_PSK ||
                     auth == WIFI_AUTH_WPA_WPA2_PSK || auth == WIFI_AUTH_WPA3_PSK || auth == WIFI_AUTH_WPA2_WPA3_PSK;
    names[count] = ssid;
    networks[count++] = String("{\"ssid\":\"") + escaped + "\",\"rssi\":" + WiFi.RSSI(i) +
        ",\"secure\":" + (auth == WIFI_AUTH_OPEN ? "false" : "true") +
        ",\"supported\":" + (supported ? "true" : "false") + "}";
  }
  if (result == WIFI_SCAN_RUNNING) esp_wifi_scan_stop();
  finish();
}

String net_scan_command(const String &argument) {
  if (argument == "start") {
    if (airplay_state() != AIRPLAY_IDLE) return "err stop screen mirroring before scanning";
    if (running) return "err scan already running";
    net_scan_cancel();
    restoreAP = WiFi.getMode() == WIFI_AP;
    // A failed router connection must not race the driver's scan; net_poll suspends retries below.
    if (WiFi.status() != WL_CONNECTED) WiFi.disconnect(false);
    int result = WiFi.scanNetworks(true, false, false, 120);
    revision++;
    started = millis();
    running = result == WIFI_SCAN_RUNNING;
    state = running ? "scanning" : "failed";
    if (!running) finish();
    return "ok " + String(revision);
  }
  if (argument.isEmpty()) {
    return String("ok {\"id\":") + revision + ",\"state\":\"" + state + "\",\"count\":" + count + "}";
  }
  unsigned long requested;
  int index;
  char extra;
  if (sscanf(argument.c_str(), "%lu %d %c", &requested, &index, &extra) != 2 || requested != revision ||
      running || strcmp(state, "ready") || index < 0 || index >= count) return "err scan result expired or unavailable";
  return "ok " + networks[index];
}

bool net_scan_running() { return running; }
