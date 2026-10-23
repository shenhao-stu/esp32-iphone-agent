// HID, AirPlay and the app GATT service share one board; BLE HID is the default.
// Power the board separately for wireless use. USB HID remains an explicit option.
#include <esp_task_wdt.h>
#include <esp_mac.h>
#include "hid_link.h"
#include "src/hid/serial_line.h"
#include "src/hid/command.h"
#include "ble_link.h"
#include "netcfg.h"
#include "net_scan.h"
#include "video_link.h"
#include "video_net.h"
#include "src/airplay/airplay.h"

#ifndef DEBUG_SERIAL
#define DEBUG_SERIAL 0
#endif
#if DEBUG_SERIAL
#include "crypto_probe.h"
#endif
const int WATCHDOG_MS = 8000;
static String receiverName = "ESP32 Agent";
String statusJson(bool full);

const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_TASK_WDT: case ESP_RST_INT_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_SW: return "software";
    case ESP_RST_USB: return "usb";
    default: return "other";
  }
}

// Board-level commands from the app; everything else is an HID command for hid_run().
String board(const String &line, const String &id, uint32_t generation) {
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  long a = 0, b = 0;
  int n = sscanf(arg.c_str(), "%ld %ld", &a, &b);
  if (cmd == "status" && !arg.length()) return "ok " + statusJson(true);
  if (cmd == "link" && !arg.length()) return "ok " + ble_link_info();
  if (cmd == "vnet") return video_net_offer(arg);
  if (cmd == "wifi-scan") return net_scan_command(arg);
  if (cmd == "phy") {
    if (arg != "1" && arg != "2") return "err usage: phy 1|2";
    return ble_request_phy(arg == "2" ? 2 : 1);
  }
  if (cmd == "stream") {
    if (arg != "on" && arg != "off") return "err usage: stream on|off";
    video_stream(arg == "on");
    return "ok";
  }
  if (cmd == "app" || cmd == "log") {  // debug relay: Mac -> app ("app run ...") and app -> Mac ("log ...")
    ble_reply(cmd + " " + arg);         // every peer sees it; each side ignores lines it doesn't handle
    return "ok";
  }
  if (cmd == "snap") { video_snap(id); return ""; }  // replies when the GOP is sent
  if (cmd == "bench" || cmd == "wbench") {
    if (n < 1 || a <= 0 || a > 4096) return "err usage: bench KB (1-4096)";
    video_bench(id, a, cmd == "wbench");
    return "";
  }
  if (cmd == "res") {
    if (n < 2 || a < 160 || b < 160 || a > 1920 || b > 2560) return "err usage: res W H";
    cfg_set_res(a, b);
    airplay_set_display(a, b, cfg_fps());
    return "ok";
  }
  if (cmd == "fps") {
    if (n < 1 || a < 1 || a > 60) return "err usage: fps N (1-60)";
    cfg_set_fps(a);
    airplay_set_display(cfg_width(), cfg_height(), a);
    return "ok";
  }
  if (cmd == "coex") {  // A/B knob, not stored: every boot starts at balance
    if (arg.length() && !net_coex(arg)) return "err usage: coex bt|balance|wifi";
    return String("ok ") + net_coex_mode();
  }
  if (cmd == "unpair") {
    ble_reply(id + " ok");  // answer before the link goes down
    ble_unpair();
    return "";
  }
  return hid_run(line, generation);
}

String statusJson(bool full) {
  static const char *apStates[] = {"idle", "connected", "streaming", "failed"};
  VideoStats v = video_stats();
  char s[768];
  snprintf(s, sizeof s,
           "{\"hid_caps\":\"mouse,keyboard,consumer\",\"up\":%lu,\"rst\":\"%s\",\"heap\":%u,\"psram\":%u,\"usb\":\"%s\",\"hid\":\"%s\",\"hid_ready\":%s,\"ble_hid\":\"%s\",\"net\":\"%s\",\"ip\":\"%s\",\"sta\":%d,"
           "\"ap\":\"%s\",\"w\":%u,\"h\":%u,\"kbps\":%lu,\"fps\":%.1f,\"idr\":%lu,\"gop\":%lu,\"ovf\":%lu,\"trim\":%lu,"
           "\"anc\":%d,\"lag\":%ld,\"vnet\":1,\"vtransport\":\"%s\",\"vkbps\":%lu,\"ble\":{\"mtu\":%u,\"vmtu\":%u,\"phy\":%u,\"kbps\":%lu,\"drop\":%lu,\"rdrop\":%lu,\"n\":%d}}",
           millis(), resetReason(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), hid_usb_state(), hid_transport(), hid_ready() ? "true" : "false", hid_ble_state(), net_mode(), net_ip().c_str(), net_peers(),
           apStates[airplay_state()], cfg_width(), cfg_height(), (unsigned long)v.kbps, v.fps, (unsigned long)v.idr_ms,
           (unsigned long)v.gop_bytes, (unsigned long)v.overflows, (unsigned long)v.trims, v.anchored ? 1 : 0, (long)v.lag,
           video_net_ready() ? "wifi" : "ble", (unsigned long)video_net_kbps(),
           ble_mtu(), ble_video_mtu(), ble_phy(), (unsigned long)ble_kbps(), (unsigned long)ble_drops(),
           (unsigned long)ble_reply_drops(), ble_peers());
  if (full) return s;
  // A compact notification for smaller MTUs; the readable characteristic always keeps full status.
  snprintf(s, sizeof s, "{\"up\":%lu,\"usb\":\"%s\",\"hid\":\"%s\",\"hid_ready\":%s,\"ble_hid\":\"%s\",\"net\":\"%s\",\"ap\":\"%s\",\"lag\":%ld,\"fps\":%.1f,\"ble\":{\"mtu\":%u}}",
           millis(), hid_usb_state(), hid_transport(), hid_ready() ? "true" : "false", hid_ble_state(), net_mode(), apStates[airplay_state()], (long)v.lag, v.fps, ble_mtu());
  return s;
}

void statusTask(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (!ble_connected()) continue;
    ble_status(statusJson(true), statusJson(false));
  }
}

void logLine(const char *msg) {
#if DEBUG_SERIAL
  Serial.printf("[%lu] %s\n", millis(), msg);
#endif
}

void startAirplay() {
  static airplay_cfg cfg;
  cfg.name = receiverName.c_str();
  esp_read_mac(cfg.hwaddr, ESP_MAC_WIFI_STA);
  cfg_ed25519_seed(cfg.ed25519_seed);
  cfg.width = cfg_width();
  cfg.height = cfg_height();
  cfg.max_fps = cfg_fps();
  cfg.on_config = video_on_config;
  cfg.on_au = video_on_au;
  cfg.log = logLine;
  if (!airplay_start(&cfg)) Serial.println("airplay: start failed");
}

void setup() {
  Serial.setRxBufferSize(1024); // a whole bounded command fits even while the main loop waits for BLE
  Serial.begin(115200);
#if DEBUG_SERIAL
  for (int i = 0; i < 30 && !Serial; i++) delay(100);  // give a serial monitor 3 s to catch the boot lines
#endif
  video_net_begin();
  hid_begin();
  esp_task_wdt_config_t wdt = {.timeout_ms = WATCHDOG_MS, .idle_core_mask = 0, .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(NULL);

  net_begin();
  if (!video_begin()) Serial.println("video: no PSRAM for the GOP buffer");
  ble_begin("ESP32 Agent " + board_suffix());
  startAirplay();
  xTaskCreatePinnedToCore(statusTask, "status", 4096, NULL, 1, NULL, 1);
  Serial.printf("boot %s: free internal heap %u (largest %u), PSRAM %u, reset %s\n", receiverName.c_str(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), resetReason());
}

void loop() {
  static bool linked = false;
  esp_task_wdt_reset();
  net_poll();
  hid_poll_stop();
  if (linked && !ble_control_connected()) { video_stream(false); video_net_close(); }
  linked = ble_control_connected();

  static SerialLine serial;
  if (serial.expire(millis())) Serial.println("0 err incomplete serial command discarded");
  String line;
  bool serialReady = false;
  while (Serial.available() && !serialReady) {
    auto result = serial.add((char)Serial.read(), millis());
    if (result == SerialLine::rejected) Serial.println("0 err invalid serial command discarded");
    if (result == SerialLine::complete) { line = serial.text(); serialReady = line.length() > 0; }
  }
  if (serialReady) {  // USB serial takes the same complete `<id> <command>` lines as BLE
#if DEBUG_SERIAL
    if (line == "0 crypto-test" || line == "0 crypto-pressure-test") {
      if (airplay_state() == AIRPLAY_IDLE) crypto_probe(line == "0 crypto-pressure-test");
      else Serial.println("crypto-test: stop screen mirroring first");
      return;
    }
#endif
    if (line.startsWith("0 net ")) { net_request(line.substring(6)); Serial.println("0 ok"); return; }
    int sp = line.indexOf(' ');
    String id = sp < 0 ? line : line.substring(0, sp);
    String rest = sp < 0 ? "" : line.substring(sp + 1);
    uint32_t generation = hid_is_stop(rest.c_str()) ? hid_request_stop() : hid_generation();
    Serial.println(id + " " + board(rest, id, generation));
    return;
  }
  uint32_t generation;
  if (!ble_pop_cmd(line, generation, 50)) return;
  int sp = line.indexOf(' ');
  String id = sp < 0 ? line : line.substring(0, sp);
  String rest = sp < 0 ? "" : line.substring(sp + 1);
#if DEBUG_SERIAL
  unsigned long started = millis();
#endif
  String reply = id.toInt() || id == "0" ? board(rest, id, generation) : "err missing request id";
  if (reply.length()) ble_reply(id + " " + reply);
#if DEBUG_SERIAL
  Serial.printf("%s -> %s (%lu ms)\n", rest.startsWith("vnet ") ? "vnet [redacted]" :
                rest.startsWith("type ") ? "type [redacted]" : line.c_str(),
                reply.c_str(), millis() - started);
#endif
}
