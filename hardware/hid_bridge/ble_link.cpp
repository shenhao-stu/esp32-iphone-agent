// BLE GATT link, see ble_link.h. Host: h2zero NimBLE-Arduino 2.5.x, buffer sizes in nimble_cfg.h.
//
// Send path: replies and status go through a queue drained by one TX task (callers never block on
// the radio); video is sent from the video task and always yields to them. NimBLE's NOTIFY_TX event
// fires when a notify is queued in the host, not when it is on air, so it cannot pace anything.
// Video is paced on the controller's ACL credits instead (the host's count, returned by HCI Number
// of Completed Packets): while commands flow, at most VIDEO_CTRL_PKTS packets in the controller, so a
// reply or an ATT write response never queues behind more than ~2 KB of video. It also leaves MBUF_RESERVE host
// blocks free, and never sends while a reply or status is waiting.
#include "ble_link.h"
#include <atomic>
#include <NimBLEDevice.h>
#include "nimble/nimble/host/include/host/ble_hs.h"
#include "netcfg.h"
#include "hid_link.h"
#include "src/hid/command.h"

static_assert(MYNEWT_VAL(MSYS_1_BLOCK_COUNT) == 64 && MYNEWT_VAL(MSYS_1_BLOCK_SIZE) == 320,
              "nimble_cfg.h not applied: build with hardware/flash.sh");
static_assert(MYNEWT_VAL(MSYS_2_BLOCK_COUNT) == 0 && MYNEWT_VAL(BLE_TRANSPORT_ACL_FROM_LL_COUNT) == 24, "nimble_cfg.h");
static_assert(MYNEWT_VAL(BLE_ATT_PREFERRED_MTU) == 517 && MYNEWT_VAL(BLE_HS_LOG_LVL) == 5, "nimble_cfg.h");
static_assert(MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM) == 1 && MYNEWT_VAL(BLE_L2CAP_COC_MPS) == 247, "nimble_cfg.h");

#define UUID(s) "6e400001-a9e8-4b1b-9c39-6d6f8a3c" s
const size_t CMD_MAX = 512;
const size_t TEXT_MAX = BLE_ATT_ATTR_MAX_LEN;  // attribute value limit, even at MTU 517
const uint32_t VIDEO_TIMEOUT_MS = 1500;    // a full buffer waits; only a stalled link drops the chunk
const uint32_t REPLY_TIMEOUT_MS = 3000;    // replies retry this long (video is paused meanwhile)
const uint32_t STATUS_TIMEOUT_MS = 500;    // a newer status replaces it a second later anyway
const int MBUF_RESERVE = 16;               // of 64 msys blocks; video never takes these
const int VIDEO_CTRL_PKTS = 8;             // ACL packets video may have in the controller while commands flow
const uint32_t CMD_ACTIVE_MS = 1000;       // ... i.e. for this long after a `cmd` write; otherwise all of them
const int MBUF_DATA = MYNEWT_VAL(MSYS_1_BLOCK_SIZE) - 16;  // payload per block (16 B mbuf header)
// Up to MAX_PEERS centrals at once: the phone's app, plus a Mac as a debug harness.
const int MAX_PEERS = 2;

enum { CH_REPLY, CH_STATUS, CH_VIDEO, CH_COUNT };

struct Command {
  uint32_t generation;
  char line[CMD_MAX];
};

struct Text {
  uint16_t len;
  char s[TEXT_MAX];
};

struct StatusText {
  Text full, compact;
};

static NimBLEServer *server;
static NimBLECharacteristic *chars[CH_COUNT], *netChar;
static QueueHandle_t cmdQueue, replyQueue;
static SemaphoreHandle_t statusLock;
static StatusText statusText;
static bool statusDirty;
static std::atomic<int> ctrlPending;  // replies queued or sending + a status waiting; video yields to them
static volatile int peers[MAX_PEERS] = {-1, -1};
static volatile bool subscribed[MAX_PEERS][CH_COUNT];
static uint32_t winStart, winBytes;  // touched by the video task only
static volatile uint32_t kbps;
static std::atomic<uint32_t> drops, replyDrops;
static volatile uint32_t lastCmdMs;  // last `cmd` write: replies are expected, keep video's share small

#if DEBUG_SERIAL
#define BLE_LOG(...) Serial.printf("ble: " __VA_ARGS__)
#else
#define BLE_LOG(...) ((void)0)
#endif

static int slotOf(int h) {
  for (int i = 0; i < MAX_PEERS; i++)
    if (peers[i] == h) return i;
  return -1;
}

// The peer's slot, claimed on first sight: a bonded central's restored subscriptions (and its
// encryption) can be reported before onConnect, and must not be lost.
static int slotFor(int h) {
  int i = slotOf(h);
  if (i < 0 && (i = slotOf(-1)) >= 0) {
    for (int c = 0; c < CH_COUNT; c++) subscribed[i][c] = false;
    peers[i] = h;
  }
  return i;
}

static int peerCount() {
  int n = 0;
  for (int i = 0; i < MAX_PEERS; i++) n += peers[i] >= 0;
  return n;
}

static int indexOf(NimBLECharacteristic *c) {
  for (int i = 0; i < CH_COUNT; i++)
    if (chars[i] == c) return i;
  return -1;
}

// One eligibility rule for sizing and sending. A status-only debug link must not limit video.
static uint16_t recipientMtu(int handle, int ch) {
  int slot = slotOf(handle);
  if (handle < 0 || slot < 0 || !subscribed[slot][ch]) return 0;
  if (ch != CH_STATUS) {
    auto ci = server->getPeerInfoByHandle(handle);
    if (!ci.isEncrypted() || !ci.isBonded()) return 0;
  }
  return server->getPeerMTU(handle); // queries this handle directly; 0 if it disconnected
}

static uint16_t channelMtu(int ch) {
  uint16_t mtu = 0;
  for (int i = 0; i < MAX_PEERS; i++) {
    uint16_t peerMtu = recipientMtu(peers[i], ch);
    if (peerMtu && (!mtu || peerMtu < mtu)) mtu = peerMtu;
  }
  return mtu; // no eligible recipient: no notification capacity
}

static size_t notificationRoom(uint16_t mtu) {
  size_t room = mtu > 3 ? mtu - 3 : 0;
  return room < BLE_ATT_ATTR_MAX_LEN ? room : BLE_ATT_ATTR_MAX_LEN;
}

static void logConn(const char *what, NimBLEConnInfo &ci) {
  BLE_LOG("%s conn %u: interval %.2f ms, latency %u, timeout %u ms, mtu %u, %s\n", what, ci.getConnHandle(),
          ci.getConnInterval() * 1.25f, ci.getConnLatency(), ci.getConnTimeout() * 10u, ci.getMTU(),
          ci.isEncrypted() ? (ci.isBonded() ? "encrypted+bonded" : "encrypted") : "plain");
}

// 15 ms interval, no latency, 4 s supervision; reliable 1M PHY; DLE 251.
static void tuneLink(uint16_t h) {
  // Keep the connection on 1M: forcing 2M makes weak links repeatedly time out.
  server->setDataLen(h, 251);
  server->updateConnParams(h, 12, 12, 0, 400);
}

class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &ci) override {
    if (slotFor(ci.getConnHandle()) < 0) { s->disconnect(ci.getConnHandle()); return; }
    logConn("connect", ci);
    tuneLink(ci.getConnHandle());
    if (peerCount() < MAX_PEERS) NimBLEDevice::startAdvertising();  // room for the debug harness
  }
  void onDisconnect(NimBLEServer *s, NimBLEConnInfo &ci, int reason) override {
    int i = slotOf(ci.getConnHandle());
    bool controlled = i >= 0 && subscribed[i][CH_REPLY];
    if (i >= 0) {
      for (int c = 0; c < CH_COUNT; c++) subscribed[i][c] = false;
      peers[i] = -1;
    }
    hid_ble_disconnect(ci.getConnHandle());
    if (controlled) hid_request_stop();
    BLE_LOG("disconnect conn %u, reason 0x%x\n", ci.getConnHandle(), reason);
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo &ci) override { BLE_LOG("conn %u: mtu %u\n", ci.getConnHandle(), mtu); }
  void onConnParamsUpdate(NimBLEConnInfo &ci) override { logConn("params", ci); }
  void onPhyUpdate(NimBLEConnInfo &ci, uint8_t tx, uint8_t rx) override {
    BLE_LOG("conn %u: phy tx %uM rx %uM\n", ci.getConnHandle(), tx, rx);
  }
  void onAuthenticationComplete(NimBLEConnInfo &ci) override { logConn("security", ci); }
};

class CharCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &ci) override {
    const NimBLEAttValue &raw = c->getValue();
    String v(raw.c_str(), raw.length());
    if (!ci.isEncrypted() || !ci.isBonded()) return;
    if (c == netChar) { net_request(v); return; }
    if (raw.length() >= CMD_MAX || v.indexOf('\n') >= 0 || raw.length() != strlen(raw.c_str())) return;
    Command command{};
    char *line = command.line;
    strncpy(line, v.c_str(), CMD_MAX - 1);
    // The app's 2 s keepalive `<id> ping` is not a command flow: arming the video cap for it held the stream
    // at ~130-170 kbps (hardware/tools/README.md TODO) — too slow to drain a mirroring backlog.
    const char *sp = strchr(line, ' ');
    if (!(sp && !strcmp(sp + 1, "ping"))) lastCmdMs = millis();
    command.generation = sp && hid_is_stop(sp + 1) ? hid_request_stop() : hid_generation();
    if (line[0] && xQueueSend(cmdQueue, &command, 0) != pdTRUE) {
      String id = sp ? String(line).substring(0, sp - line) : "0";
      ble_reply(id + " err command queue full");
    }
  }
  void onRead(NimBLECharacteristic *c, NimBLEConnInfo &ci) override {
    if (c != netChar) return;
    String j = net_json();
    c->setValue((const uint8_t *)j.c_str(), j.length());
  }
  void onSubscribe(NimBLECharacteristic *c, NimBLEConnInfo &ci, uint16_t sub) override {
    int i = indexOf(c), p = sub & 1 ? slotFor(ci.getConnHandle()) : slotOf(ci.getConnHandle());
    bool lostControl = i == CH_REPLY && p >= 0 && subscribed[p][CH_REPLY] && !(sub & 1);
    if (i >= 0 && p >= 0) subscribed[p][i] = sub & 1;
    if (lostControl) hid_request_stop();
    BLE_LOG("conn %u: %s %s\n", ci.getConnHandle(), sub & 1 ? "subscribed" : "unsubscribed",
            i == CH_REPLY ? "reply" : i == CH_STATUS ? "status" : "video");
  }
};

static CharCb charCb;

#if DEBUG_SERIAL
static int onGap(ble_gap_event *e, void *) {  // NimBLEServerCallbacks has no data-length event
  if (e->type == BLE_GAP_EVENT_DATA_LEN_CHG)
    BLE_LOG("data length tx %u B / %u us, rx %u B / %u us\n", e->data_len_chg.max_tx_octets,
            e->data_len_chg.max_tx_time, e->data_len_chg.max_rx_octets, e->data_len_chg.max_rx_time);
  return 0;
}
#endif

extern "C" uint16_t ble_hs_hci_avail_pkts;  // free controller ACL buffers (NimBLE host, ble_hs_hci.c)
static uint16_t ctrlMax;                     // the controller's total, read at boot with no links up

// Room for a video notify of n bytes in the controller: its ACL fragments plus those already in flight.
static bool ctrlRoom(size_t n) {
  if (ble_hs_hci_avail_pkts > ctrlMax) ctrlMax = ble_hs_hci_avail_pkts;
  bool cmds = millis() - lastCmdMs < CMD_ACTIVE_MS;
  int frags = (int)((n + 3 + 4 + 250) / 251), limit = cmds && VIDEO_CTRL_PKTS < ctrlMax ? VIDEO_CTRL_PKTS : ctrlMax;
  return ctrlMax - ble_hs_hci_avail_pkts + frags <= (frags > limit ? frags : limit);
}

// msys blocks one queued notify of n bytes holds: ATT/L2CAP/HCI headers + packet header in the first
// block, plus one for the ACL fragment the host splits off.
static int blocksFor(size_t n) { return (int)((n + 3 + 4 + 4 + 8 + MBUF_DATA - 1) / MBUF_DATA) + 1; }

// One notify through the NimBLE host directly: unlike NimBLECharacteristic::notify() it reports
// "out of buffers", so we can wait instead of silently losing the packet.
static bool notifyOne(int slot, int ch, const uint8_t *p, size_t n, uint32_t start, uint32_t timeout_ms) {
  bool video = ch == CH_VIDEO;
  int need = blocksFor(n) + (video ? MBUF_RESERVE : 0);
  const int h = peers[slot]; // retries stay on this connection; never retarget a reused peer slot
  for (;;) {
    uint16_t mtu = recipientMtu(h, ch);
    if (!mtu || n > notificationRoom(mtu)) return false; // never let the stack truncate a queued chunk
    if (!(video && (ctrlPending > 0 || !ctrlRoom(n))) && os_msys_num_free() >= need) {
      os_mbuf *om = ble_hs_mbuf_from_flat(p, n);
      int rc = om ? ble_gatts_notify_custom(h, chars[ch]->getHandle(), om) : BLE_HS_ENOMEM;  // om consumed
      if (rc == 0) return true;
      if (rc != BLE_HS_ENOMEM) {
        BLE_LOG("notify ch %d conn %d failed rc=%d\n", ch, h, rc);
        return false;
      }
    }
    if (millis() - start > timeout_ms) {
      BLE_LOG("notify ch %d conn %d gave up after %lu ms (free %d)\n", ch, h, millis() - start, os_msys_num_free());
      return false;
    }
    vTaskDelay(1);
  }
}

// To every subscribed peer within one deadline; true if at least one got it.
static bool notifyRaw(int ch, const uint8_t *p, size_t n, uint32_t timeout_ms) {
  uint32_t start = millis();
  bool any = false;
  for (int i = 0; i < MAX_PEERS; i++) any |= notifyOne(i, ch, p, n, start, timeout_ms);
  return any;
}

static void sendReply(const Text &t) {
  size_t room = notificationRoom(channelMtu(CH_REPLY));
  if (!room) return;
  if (!notifyRaw(CH_REPLY, (const uint8_t *)t.s, t.len < room ? t.len : room, REPLY_TIMEOUT_MS) &&
      channelMtu(CH_REPLY)) {
    replyDrops++;
    BLE_LOG("reply dropped: %.*s\n", (int)t.len, t.s);
  }
}

static void sendStatus(const StatusText &status) {
  uint32_t start = millis();
  for (int i = 0; i < MAX_PEERS; i++) {
    size_t room = notificationRoom(recipientMtu(peers[i], CH_STATUS));
    const Text &text = status.full.len <= room ? status.full : status.compact;
    if (!room || text.len > room) continue; // skip only this peer; never truncate JSON
    notifyOne(i, CH_STATUS, (const uint8_t *)text.s, text.len, start, STATUS_TIMEOUT_MS);
  }
}

static void txTask(void *) {
  static Text t;
  static StatusText status;
  for (;;) {
    if (xQueueReceive(replyQueue, &t, pdMS_TO_TICKS(20)) == pdTRUE) {
      sendReply(t);
      ctrlPending--;
    }
    xSemaphoreTake(statusLock, portMAX_DELAY);
    bool due = statusDirty;
    if (due) status = statusText;
    statusDirty = false;
    xSemaphoreGive(statusLock);
    if (due) {
      sendStatus(status);
      ctrlPending--;
    }
  }
}

static void toText(Text &t, const String &s) {
  t.len = s.length() < TEXT_MAX ? s.length() : TEXT_MAX;
  memcpy(t.s, s.c_str(), t.len);
}

void ble_begin(const String &name) {
  cmdQueue = xQueueCreate(6, sizeof(Command));
  replyQueue = xQueueCreate(8, sizeof(Text));
  statusLock = xSemaphoreCreateMutex();
  NimBLEDevice::init(name.c_str());
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setPower(9);  // dBm: retain link margin during setup, including a board powered by a Mac.
  NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_1M_MASK, BLE_GAP_LE_PHY_1M_MASK);
  NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, Just Works, LE Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
#if DEBUG_SERIAL
  NimBLEDevice::setCustomGapHandler(onGap);
#endif

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCb());
  NimBLEService *svc = server->createService(UUID("0000"));
  NimBLECharacteristic *cmd = svc->createCharacteristic(UUID("0001"), NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
  chars[CH_REPLY] = svc->createCharacteristic(UUID("0002"), NIMBLE_PROPERTY::NOTIFY);
  chars[CH_STATUS] = svc->createCharacteristic(UUID("0003"), NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
  chars[CH_VIDEO] = svc->createCharacteristic(UUID("0004"), NIMBLE_PROPERTY::NOTIFY);
  netChar = svc->createCharacteristic(UUID("0005"), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
                                                        NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::WRITE_ENC);
  cmd->setCallbacks(&charCb);
  netChar->setCallbacks(&charCb);
  for (auto *c : chars) c->setCallbacks(&charCb);
  hid_ble_begin(server);
  server->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData data, scan;
  data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  data.setCompleteServices(NimBLEUUID(UUID("0000")));
  data.addServiceUUID(NimBLEUUID((uint16_t)0x1812));
  data.setAppearance(0x03c0);
  scan.setName(name.c_str());
  adv->setAdvertisementData(data);
  adv->setScanResponseData(scan);
  NimBLEDevice::startAdvertising();
  xTaskCreatePinnedToCore(txTask, "ble_tx", 3072, NULL, 3, NULL, 1);
  ctrlMax = ble_hs_hci_avail_pkts;
  Serial.printf("ble: %s, msys %d x %d (%d free), acl rx %d, controller acl tx %u, mtu %d, coc %d x %d\n",
                NimBLEDevice::getVersion(), os_msys_count(), MYNEWT_VAL(MSYS_1_BLOCK_SIZE), os_msys_num_free(),
                MYNEWT_VAL(BLE_TRANSPORT_ACL_FROM_LL_COUNT), ctrlMax, ble_att_preferred_mtu(),
                MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM), MYNEWT_VAL(BLE_L2CAP_COC_MPS));
}

bool ble_connected() { return peerCount() > 0; }
bool ble_control_connected() { return channelMtu(CH_REPLY) != 0; }

bool ble_pop_cmd(String &line, uint32_t &generation, uint32_t wait_ms) {
  Command command;
  if (xQueueReceive(cmdQueue, &command, pdMS_TO_TICKS(wait_ms)) != pdTRUE) return false;
  line = command.line;
  generation = command.generation;
  return true;
}

void ble_reply(const String &line) {
  if (!channelMtu(CH_REPLY)) return;
  Text t;
  toText(t, line);
  ctrlPending++;
  if (xQueueSend(replyQueue, &t, pdMS_TO_TICKS(200)) != pdTRUE) {  // 8 replies stuck: the link is gone
    ctrlPending--;
    replyDrops++;
  }
}

void ble_status(const String &full, const String &compact) {
  // Characteristic reads support ATT long reads and must not inherit the notification's MTU limit.
  chars[CH_STATUS]->setValue((const uint8_t *)full.c_str(), full.length());
  if (!channelMtu(CH_STATUS)) return;
  xSemaphoreTake(statusLock, portMAX_DELAY);
  toText(statusText.full, full);
  toText(statusText.compact, compact);
  if (!statusDirty) ctrlPending++;
  statusDirty = true;
  xSemaphoreGive(statusLock);
}

uint16_t ble_video_mtu() { return channelMtu(CH_VIDEO); }

size_t ble_video_room() {
  size_t room = notificationRoom(ble_video_mtu());
  return room > 3 ? room - 3 : 0; // seq + flags; subtraction must not wrap when nobody receives video
}

bool ble_video_send(const uint8_t *p, size_t n) {
  if (!notifyRaw(CH_VIDEO, p, n, VIDEO_TIMEOUT_MS)) {
    drops++;
    return false;
  }
  uint32_t now = millis();
  winBytes += n;
  if (now - winStart >= 1000) {
    kbps = winBytes * 8 / (now - winStart);
    winBytes = 0;
    winStart = now;
  }
  return true;
}

uint32_t ble_kbps() { return millis() - winStart > 2000 ? 0 : kbps; }
uint32_t ble_drops() { return drops; }
uint32_t ble_reply_drops() { return replyDrops; }
int ble_peers() { return peerCount(); }

uint16_t ble_mtu() {  // diagnostic minimum over all connected peers; not a channel's sending limit
  uint16_t mtu = 0;
  for (int i = 0; i < MAX_PEERS; i++) {
    int h = peers[i];
    uint16_t m = h >= 0 ? server->getPeerMTU(h) : 0;
    if (m && (!mtu || m < mtu)) mtu = m;
  }
  return mtu ? mtu : 23;
}

uint8_t ble_phy() {
  uint8_t tx = 1, rx = 1;
  int h = peers[0] >= 0 ? peers[0] : peers[1];
  if (h >= 0) ble_gap_read_le_phy(h, &tx, &rx);
  return tx == BLE_GAP_LE_PHY_2M ? 2 : 1;
}

String ble_link_info() {
  String result = "[";
  for (int i = 0; i < MAX_PEERS; i++) {
    int h = peers[i];
    if (h < 0) continue;
    auto ci = server->getPeerInfoByHandle(h);
    uint8_t tx = 0, rx = 0;
    ble_gap_read_le_phy(h, &tx, &rx);
    char json[240];
    snprintf(json, sizeof json,
             "{\"handle\":%d,\"mtu\":%u,\"tx_phy\":%u,\"rx_phy\":%u,\"interval_ms\":%.2f,\"latency\":%u,\"timeout_ms\":%u,\"encrypted\":%s,\"video\":%s}",
             h, ci.getMTU(), tx, rx, ci.getConnInterval() * 1.25f, ci.getConnLatency(), ci.getConnTimeout() * 10u,
             ci.isEncrypted() ? "true" : "false", recipientMtu(h, CH_VIDEO) ? "true" : "false");
    if (result.length() > 1) result += ',';
    result += json;
  }
  return result + ']';
}

String ble_request_phy(uint8_t phy) {
  if (phy != 1 && phy != 2) return "err usage: phy 1|2";
  // A comparison must not silently measure a second central or change its link.
  if (peerCount() != 1) return "err PHY test requires exactly one connected phone";
  int h = peers[0] >= 0 ? peers[0] : peers[1];
  if (!recipientMtu(h, CH_VIDEO)) return "err phone video subscription is not ready";
  uint8_t mask = phy == 2 ? BLE_GAP_LE_PHY_2M_MASK : BLE_GAP_LE_PHY_1M_MASK;
  return server->updatePhy(h, mask, mask, 0) ? "ok requested (verify with link)" : "err PHY request rejected";
}

void ble_unpair() {
  for (int i = 0; i < 100 && ctrlPending > 0; i++) vTaskDelay(pdMS_TO_TICKS(10));  // let the `ok` go out
  NimBLEDevice::deleteAllBonds();  // unpairs each bond (also drops it from the controller's resolving list)
  ble_store_clear();               // and any CCCD/peer records left in NVS
  bool any = false;
  for (int i = 0; i < MAX_PEERS; i++)
    if (peers[i] >= 0) { server->disconnect(peers[i]); any = true; }  // onDisconnect restarts advertising
  if (!any) NimBLEDevice::startAdvertising();
}
