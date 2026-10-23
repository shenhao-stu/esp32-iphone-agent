// Mirrored H.264 from AirPlay to BLE, see video_link.h.
#include "video_link.h"
#include <esp_heap_caps.h>
#include "ble_link.h"
#include "video_net.h"
#include "src/video/gop.h"
#include "src/video/stream_control.h"

// GOP since the latest IDR; overflow drops it until the next IDR. A still screen gets an IDR only every
// few minutes, so room for ~27 min of AUs at 5 fps (P-frames of a still screen are tiny).
const size_t GOP_CAP = 4 * 1024 * 1024;
const int MAX_AUS = 8192;
const size_t SCRATCH_CAP = 512 * 1024;   // largest access unit we forward
const size_t MAX_CHUNK = 512;
const size_t NET_CHUNK = 16000;

struct Req {
  char id[16];
  uint32_t kb;
  bool bench;
  bool wifi;
};

static gop_buf gop;
static SemaphoreHandle_t lock;
static uint8_t *scratch;
static QueueHandle_t reqs;
static TaskHandle_t task;
static VideoStreamControl stream;
// Stream position as units since the GOP's IDR (gop.base + index), so the mirror task can drop units the app
// already has (video_on_au) while this task sends. Only the video task writes these cursors.
static std::atomic<uint32_t> txEpoch{0}, txGen{UINT32_MAX}, txNext{0};
static uint16_t seq;
static uint8_t *chunk;
static uint32_t winStart, winBytes, winFrames, lastIdr;
static volatile uint32_t statKbps, statIdrMs, statAt;
static volatile float statFps;

// One access unit as notifies. A failed chunk still consumes its seq, so the app sees a gap; the stream
// retries a chunk first, as past a dropped front the GOP cannot be sent again.
static bool sendAu(const uint8_t *p, size_t n, uint8_t flags, uint32_t ts, int tries = 1, bool continuous = false) {
  const bool wifi = video_net_ready(); // one transport per AU; never splice an AU across connections
  size_t room = wifi ? NET_CHUNK : min(ble_video_room(), MAX_CHUNK);
  if (room < 5) return false;
  vchunk c;
  vchunk_begin(&c, p, n, flags, ts);
  for (size_t len; (len = vchunk_next(&c, chunk, room, seq)) > 0;) {
    if (continuous && !stream.isCurrent(txEpoch.load())) return false;
    bool ok = false;
    for (int t = 0; t < tries && !ok; t++) ok = wifi ? video_net_send(chunk, len) : ble_video_send(chunk, len);
    seq++;
    if (!ok) return false;
  }
  return true;
}

// Copies unit `at` (counted from the IDR) of generation gen into scratch; false if the GOP moved on.
static bool copyAu(uint32_t gen, uint32_t at, gop_au *au) {
  xSemaphoreTake(lock, portMAX_DELAY);
  int i = (int)(at - gop.base);
  bool ok = gop.gen == gen && at >= gop.base && i < gop.n && gop.aus[i].len <= SCRATCH_CAP;
  if (ok) {
    *au = gop.aus[i];
    memcpy(scratch, gop.arena + au->off, au->len);
  }
  xSemaphoreGive(lock);
  return ok;
}

// Snapshot of the GOP state: generation, its first and end unit (counted from the IDR), SPS/PPS.
static bool snapshot(uint32_t *gen, uint32_t *first, uint32_t *end, uint8_t *cfg, size_t *cfg_len,
                     uint32_t *cfg_ts) {
  xSemaphoreTake(lock, portMAX_DELAY);
  *gen = gop.gen;
  *first = gop.base;
  *end = gop.wait_idr ? gop.base : gop.base + gop.n;
  *cfg_len = gop.cfg_len;
  *cfg_ts = gop.cfg_ts;
  memcpy(cfg, gop.cfg, gop.cfg_len);
  xSemaphoreGive(lock);
  return *end > *first && *cfg_len > 0;
}

static String doSnap() {
  static uint8_t cfg[GOP_CFG_MAX];
  if (!video_net_ready() && !ble_video_room()) return "err video not subscribed";
  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t gen, first, end, cfg_ts;
    size_t cfg_len;
    bool have = snapshot(&gen, &first, &end, cfg, &cfg_len, &cfg_ts);
    if (!cfg_len) return "err no video (start screen mirroring)";
    // iOS sends an IDR only when mirroring starts: past an overflow (or a dropped front) only a new session has one
    if (!have || first) return "err waiting for keyframe (restart screen mirroring)";
    if (!sendAu(cfg, cfg_len, VF_CONFIG | VF_SNAP, cfg_ts)) return "err ble send failed";
    bool complete = true;
    for (uint32_t i = first; i < end && complete; i++) {
      gop_au au;
      complete = copyAu(gen, i, &au);
      if (complete && !sendAu(scratch, au.len, VF_SNAP | (au.idr ? VF_IDR : 0), au.ts)) return "err ble send failed";
    }
    if (complete) return "ok";  // else a new IDR replaced the GOP mid-send: start over with it
  }
  return "err video changing too fast";
}

static String doBench(uint32_t kb, bool wifi) {
  size_t room = wifi ? (video_net_ready() ? NET_CHUNK : 0) : min(ble_video_room(), MAX_CHUNK);
  if (!room) return "err video not subscribed";
  uint32_t start = millis();
  memset(chunk + VCHUNK_HDR, 0xA5, room);
  for (size_t left = kb * 1024; left;) {
    size_t n = min(left, room);
    chunk[0] = (uint8_t)seq;
    chunk[1] = (uint8_t)(seq >> 8);
    chunk[2] = VF_BENCH;
    bool ok = wifi ? video_net_send(chunk, VCHUNK_HDR + n) : ble_video_send(chunk, VCHUNK_HDR + n);
    seq++;
    if (!ok) return "err ble send failed";
    left -= n;
  }
  return "ok " + String(millis() - start);
}

// Continuous mode: send every AU once, catching up from the GOP's IDR when streaming starts (P-frames
// can't be skipped: each needs the one before, and the next IDR may be minutes away). After a failed
// send, start over from the GOP's IDR; once its front was dropped, only the next IDR can restart it.
static void streamStep() {
  static uint8_t cfg[GOP_CFG_MAX];
  const uint32_t epoch = stream.epoch();
  if (epoch != txEpoch.load()) {
    txGen = UINT32_MAX; // off/on between task ticks still requires a complete replay
    txEpoch = epoch;
  }
  if (!stream.isCurrent(epoch) || (!video_net_ready() && !ble_video_room())) {
    txGen = UINT32_MAX;  // resend config + whole GOP when streaming resumes
    return;
  }
  uint32_t gen, first, end, cfg_ts;
  size_t cfg_len;
  if (!snapshot(&gen, &first, &end, cfg, &cfg_len, &cfg_ts)) return;
  if (gen != txGen) {
    if (first) return;  // no IDR left to start from (restart screen mirroring)
    txNext = 0;
    txGen = gen;
    if (!sendAu(cfg, cfg_len, VF_CONFIG, cfg_ts, 3, true)) { txGen = UINT32_MAX; return; }
  }
  for (gop_au au; stream.isCurrent(epoch) && txNext < end && copyAu(gen, txNext, &au); txNext = txNext + 1)
    if (!sendAu(scratch, au.len, au.idr ? VF_IDR : 0, au.ts, 3, true)) { txGen = UINT32_MAX; return; }
}

static void videoTask(void *) {
  for (;;) {
    video_net_poll();
    Req r;
    if (xQueueReceive(reqs, &r, 0) == pdTRUE) {
      String reply = String(r.id) + " " + (r.bench ? doBench(r.kb, r.wifi) : doSnap());
#if DEBUG_SERIAL
      Serial.println("video: " + reply);  // the reply only goes over BLE; mirror it for serial-driven tests
#endif
      ble_reply(reply);
    }
    streamStep();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
  }
}

bool video_begin() {
  uint8_t *arena = (uint8_t *)heap_caps_malloc(GOP_CAP, MALLOC_CAP_SPIRAM);
  gop_au *aus = (gop_au *)heap_caps_malloc(MAX_AUS * sizeof(gop_au), MALLOC_CAP_SPIRAM);
  scratch = (uint8_t *)heap_caps_malloc(SCRATCH_CAP, MALLOC_CAP_SPIRAM);
  chunk = (uint8_t *)heap_caps_malloc(VCHUNK_HDR + NET_CHUNK, MALLOC_CAP_SPIRAM);
  if (!arena || !aus || !scratch || !chunk) return false;
  gop_init(&gop, arena, GOP_CAP, aus, MAX_AUS);
  lock = xSemaphoreCreateMutex();
  reqs = xQueueCreate(4, sizeof(Req));
  return xTaskCreatePinnedToCore(videoTask, "video", 6 * 1024, NULL, 2, &task, 1) == pdPASS;
}

void video_on_config(const uint8_t *sps_pps, size_t len) {
  xSemaphoreTake(lock, portMAX_DELAY);
  gop_set_config(&gop, sps_pps, len, millis());
  xSemaphoreGive(lock);
}

void video_on_au(const uint8_t *au, size_t len, bool idr) {
  uint32_t now = millis();
  winBytes += len;
  winFrames++;
  if (idr) {
    if (lastIdr) statIdrMs = now - lastIdr;
    lastIdr = now;
  }
  if (now - winStart >= 1000) {
    statKbps = winBytes * 8 / (now - winStart);
    statFps = winFrames * 1000.0f / (now - winStart);
    statAt = now;
    winBytes = winFrames = 0;
    winStart = now;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  // iOS sends an IDR only when mirroring starts, so the GOP outgrows the buffer within minutes. While the app
  // streams it already holds every unit sent: drop those rather than the whole GOP, and the stream stays live.
  if (!idr && !gop.wait_idr && !gop_fits(&gop, len) && stream.isCurrent(txEpoch.load()) && txGen == gop.gen)
    gop_drop_front(&gop, (int)(txNext - gop.base));
  gop_push(&gop, au, len, idr, now);
  xSemaphoreGive(lock);
  xTaskNotifyGive(task);
}

void video_stream(bool on) {
  stream.set(on);
  if (task) xTaskNotifyGive(task);
}

static void request(const String &id, bool bench, uint32_t kb, bool wifi = false) {
  Req r = {{0}, kb, bench, wifi};
  strncpy(r.id, id.c_str(), sizeof r.id - 1);
  if (xQueueSend(reqs, &r, 0) != pdTRUE) ble_reply(id + " err busy");
}

void video_snap(const String &id) { request(id, false, 0); }
void video_bench(const String &id, uint32_t kb, bool wifi) { request(id, true, kb, wifi); }

VideoStats video_stats() {
  bool fresh = millis() - statAt < 3000;  // mirroring stopped: report zeros, not stale numbers
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t gopBytes = gop.used, overflows = gop.overflows, trims = gop.trims;
  bool anchored = !gop.wait_idr && gop.base == 0 && gop.n > 0;
  int32_t lag = stream.isCurrent(txEpoch.load()) && txGen == gop.gen ? (int32_t)gop_pending(&gop, txNext) : -1;
  xSemaphoreGive(lock);
  return {fresh ? statKbps : 0, statIdrMs, gopBytes, fresh ? statFps : 0, overflows, trims, anchored, lag};
}
