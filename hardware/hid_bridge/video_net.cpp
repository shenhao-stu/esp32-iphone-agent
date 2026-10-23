extern "C" {
#include <esp_mem.h>
}
#include "video_net.h"
#include "netcfg.h"
#include "src/video/write_deadline.h"
#include <atomic>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_util.h>

static constexpr uint16_t PORT = 7002;
static constexpr char IDENTITY[] = "gagent-video-v1";
static const int CIPHERS[] = {MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256, 0};
static portMUX_TYPE offerLock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t offeredKey[32];
static bool offered;
static std::atomic<uint32_t> revision;
static std::atomic<uint32_t> applied;
static int listener = -1, peer = -1;
static mbedtls_ssl_config config;
static mbedtls_ssl_context tls;
static bool initialized, armed;
static std::atomic<bool> ready;
static uint32_t deadline, windowAt, windowBytes, connectedAt;
static std::atomic<uint32_t> rate, rateAt;
static uint32_t socketProgress, longestStall;
static std::atomic<TaskHandle_t> tlsOwner;

static void *allocateTLS(size_t count, size_t size) {
  if (size && count > SIZE_MAX / size) return nullptr;
  if (xTaskGetCurrentTaskHandle() == tlsOwner.load() && count * size >= 16 * 1024)
    return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return esp_mbedtls_mem_calloc(count, size);
}

void video_net_begin() {
  // The SDK allocator hook is global, but routing is limited to the sole TLS owner and large buffers.
  // Both heaps use heap_caps_free; small TLS objects and all other tasks keep the SDK's allocation policy.
  mbedtls_platform_set_calloc_free(allocateTLS, esp_mbedtls_mem_free);
}

static void closePeer(const char *reason, int result = 0, int socketError = 0) {
  if (peer >= 0 || initialized)
    Serial.printf("video Wi-Fi: close reason=%s ready=%u tls=%d errno=%d age=%u revision=%u applied=%u\n",
                  reason, ready ? 1u : 0u, result, socketError,
                  ready ? (unsigned)(millis() - connectedAt) : 0u,
                  (unsigned)revision.load(), (unsigned)applied.load());
  ready = false;
  if (peer >= 0) { shutdown(peer, SHUT_RDWR); close(peer); peer = -1; }
  if (initialized) { mbedtls_ssl_free(&tls); mbedtls_ssl_config_free(&config); initialized = false; }
}

static int randomBytes(void *, unsigned char *out, size_t length) { esp_fill_random(out, length); return 0; }
static int writeSocket(void *, const unsigned char *p, size_t n) {
  // lwIP's nonblocking send can reject a whole TLS record larger than TCP_SND_BUF.
  // Partial writes let TLS advance using the available TCP window.
  int result = send(peer, p, min(n, (size_t)1460), 0);
  if (result > 0) {
    uint32_t now = millis();
    longestStall = max(longestStall, now - socketProgress);
    socketProgress = now;
  }
  return result >= 0 ? result : errno == EAGAIN || errno == EWOULDBLOCK
      ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}
static int readSocket(void *, unsigned char *p, size_t n) {
  int result = recv(peer, p, n, 0);
  return result >= 0 ? result : errno == EAGAIN || errno == EWOULDBLOCK
      ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

String video_net_offer(const String &key) {
  if (key == "off") { video_net_close(); return "ok"; }
  if (!net_up()) return "err Wi-Fi not connected";
  if (key.length() != 64) return "err invalid video session key";
  uint8_t parsed[32] = {};
  for (int i = 0; i < 64; i++) {
    char c = key[i];
    int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (value < 0) { mbedtls_platform_zeroize(parsed, sizeof parsed); return "err invalid video session key"; }
    parsed[i / 2] |= value << (i % 2 ? 0 : 4);
  }
  portENTER_CRITICAL(&offerLock);
  memcpy(offeredKey, parsed, sizeof parsed);
  offered = true;
  revision++;
  portEXIT_CRITICAL(&offerLock);
  mbedtls_platform_zeroize(parsed, sizeof parsed);
  return "ok " + String(PORT);
}

void video_net_close() {
  portENTER_CRITICAL(&offerLock);
  mbedtls_platform_zeroize(offeredKey, sizeof offeredKey);
  offered = false;
  revision++;
  portEXIT_CRITICAL(&offerLock);
}

static bool prepare() {
  uint8_t key[32];
  closePeer("offer_changed");
  portENTER_CRITICAL(&offerLock);
  applied = revision.load();
  armed = offered;
  memcpy(key, offeredKey, sizeof key);
  mbedtls_platform_zeroize(offeredKey, sizeof offeredKey);
  offered = false;
  portEXIT_CRITICAL(&offerLock);
  if (!armed) { mbedtls_platform_zeroize(key, sizeof key); return false; }
  mbedtls_ssl_init(&tls);
  mbedtls_ssl_config_init(&config);
  initialized = true;
  int result = mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
  mbedtls_ssl_conf_rng(&config, randomBytes, nullptr);
  mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_max_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_ciphersuites(&config, CIPHERS);
  mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_NONE); // PSK authenticates both peers; no certificates
  mbedtls_ssl_conf_renegotiation(&config, MBEDTLS_SSL_RENEGOTIATION_DISABLED);
  mbedtls_ssl_conf_session_tickets(&config, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
  // Bound hardware AES staging allocations when the TLS record buffer resides in PSRAM.
  if (!result) result = mbedtls_ssl_conf_max_frag_len(&config, MBEDTLS_SSL_MAX_FRAG_LEN_1024);
  if (!result) result = mbedtls_ssl_conf_psk(&config, key, sizeof key,
                                          (const unsigned char *)IDENTITY, sizeof IDENTITY - 1);
  mbedtls_platform_zeroize(key, sizeof key);
  if (!result) result = mbedtls_ssl_setup(&tls, &config);
  // Keep headroom for Wi-Fi/BLE control even though the two large TLS buffers now live in PSRAM.
  if (result || heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 12 * 1024) {
    Serial.printf("video Wi-Fi: TLS unavailable result=%d heap=%u largest=%u\n", result,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    closePeer("tls_setup", result); armed = false; return false;
  }
  if (listener < 0) {
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    if (listener < 0 || bind(listener, (sockaddr *)&address, sizeof address) || listen(listener, 1)) {
      const int socketError = errno;
      if (listener >= 0) close(listener);
      listener = -1; closePeer("listen_failed", 0, socketError); armed = false; return false;
    }
    fcntl(listener, F_SETFL, O_NONBLOCK);
  }
  deadline = millis() + 10000;
  return true;
}

void video_net_poll() {
  tlsOwner = xTaskGetCurrentTaskHandle();
  if (applied != revision.load()) prepare();
  if (!armed) return;
  const bool networkUp = net_up();
  if (!networkUp || (!ready && (int32_t)(millis() - deadline) >= 0)) {
    closePeer(networkUp ? "connect_timeout" : "net_down"); armed = false; return;
  }
  if (peer < 0) {
    peer = accept(listener, nullptr, nullptr);
    if (peer < 0) return;
    fcntl(peer, F_SETFL, O_NONBLOCK);
    int yes = 1;
    setsockopt(peer, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
    mbedtls_ssl_set_bio(&tls, nullptr, writeSocket, readSocket, nullptr);
  }
  if (!ready) {
    errno = 0;
    int result = mbedtls_ssl_handshake(&tls);
    if (!result) { ready = true; connectedAt = windowAt = millis(); windowBytes = 0; }
    else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
      const int socketError = errno;
      Serial.printf("video Wi-Fi: TLS handshake failed (%d)\n", result);
      closePeer("handshake_failed", result, socketError); armed = false;
    }
  } else {
    uint8_t byte;
    errno = 0;
    int result = mbedtls_ssl_read(&tls, &byte, 1);
    if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
      closePeer(result == 0 ? "read_eof" : result > 0 ? "unexpected_data" : "read_error", result, errno);
      armed = false; // this one-way channel accepts no application commands; never log the received byte
    }
  }
}

bool video_net_ready() { return ready && applied == revision.load(); }

static bool writeAll(const uint8_t *p, size_t length) {
  VideoWriteDeadline deadline(millis());
  socketProgress = deadline.started;
  longestStall = 0;
  int result = 0;
  while (length && video_net_ready()) {
    int n = mbedtls_ssl_write(&tls, p, length);
    result = n;
    // TLS may return WANT_WRITE after its BIO sent part of a record. Track TCP progress,
    // not just completed TLS records; retry with the same buffer until TLS accepts it.
    deadline.progress(socketProgress);
    if (n > 0) { p += n; length -= n; continue; }
    if ((n != MBEDTLS_ERR_SSL_WANT_WRITE && n != MBEDTLS_ERR_SSL_WANT_READ) ||
        deadline.expired(millis())) break;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  if (!length) {
    if (longestStall >= 2000)
      Serial.printf("video Wi-Fi: write resumed stalled=%u elapsed=%u\n",
                    (unsigned)longestStall, (unsigned)(millis() - deadline.started));
    return true;
  }
  const int socketError = errno;
  Serial.printf("video Wi-Fi: write failed tls=%d errno=%d heap=%u remaining=%u elapsed=%u stalled=%u\n",
                result, socketError, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)length,
                (unsigned)(millis() - deadline.started), (unsigned)(millis() - socketProgress));
  closePeer("write_failed", result, socketError); armed = false;
  return false;
}

bool video_net_send(const uint8_t *p, size_t n) {
  if (!video_net_ready() || n > 16003 || n < 3) return false;
  uint8_t header[2] = {(uint8_t)(n >> 8), (uint8_t)n};
  if (!writeAll(header, sizeof header) || !writeAll(p, n)) return false;
  windowBytes += n;
  uint32_t elapsed = millis() - windowAt;
  if (elapsed >= 1000) {
    rate = windowBytes * 8 / elapsed; rateAt = millis(); windowBytes = 0; windowAt = millis();
  }
  return true;
}

uint32_t video_net_kbps() { return millis() - rateAt.load() < 3000 ? rate.load() : 0; }
