// Host unit tests for the board's pure-C parts: bplist codec and the GOP buffer / video chunker.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hid_bridge/src/bplist/bplist.h"
#include "../hid_bridge/src/video/gop.h"
#include "../hid_bridge/src/hid/command.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_bplist_roundtrip(void) {
  bp_writer w;
  bp_writer_init(&w);
  int root = bp_w_dict(&w);
  bp_w_set(&w, root, "features", bp_w_uint(&w, 0x5A7FFEE6ull));
  bp_w_set(&w, root, "big", bp_w_uint(&w, 0x123456789ull));
  bp_w_set(&w, root, "name", bp_w_string(&w, "ESP32 Agent with a long name"));
  bp_w_set(&w, root, "ok", bp_w_bool(&w, true));
  bp_w_set(&w, root, "rate", bp_w_real(&w, 1.0 / 60));
  uint8_t pk[32];
  for (int i = 0; i < 32; i++) pk[i] = (uint8_t)i;
  bp_w_set(&w, root, "pk", bp_w_data(&w, pk, 32));
  int arr = bp_w_array(&w);
  bp_w_set(&w, root, "displays", arr);
  int d0 = bp_w_dict(&w);
  bp_w_append(&w, arr, d0);
  bp_w_set(&w, d0, "width", bp_w_uint(&w, 480));
  size_t len = 0;
  uint8_t *buf = bp_w_finish(&w, &len);
  bp_writer_free(&w);
  CHECK(buf != NULL);

  bp_reader r;
  int top = bp_open(&r, buf, len);
  CHECK(top >= 0 && bp_type(&r, top) == BP_DICT && bp_count(&r, top) == 7);
  uint64_t u = 0;
  CHECK(bp_get_uint(&r, bp_dict_get(&r, top, "features"), &u) && u == 0x5A7FFEE6ull);
  CHECK(bp_get_uint(&r, bp_dict_get(&r, top, "big"), &u) && u == 0x123456789ull);
  char s[64];
  CHECK(bp_get_string(&r, bp_dict_get(&r, top, "name"), s, sizeof s) && !strcmp(s, "ESP32 Agent with a long name"));
  bool b = false;
  CHECK(bp_get_bool(&r, bp_dict_get(&r, top, "ok"), &b) && b);
  double d = 0;
  CHECK(bp_get_real(&r, bp_dict_get(&r, top, "rate"), &d) && d == 1.0 / 60);
  const uint8_t *p;
  size_t n;
  CHECK(bp_get_data(&r, bp_dict_get(&r, top, "pk"), &p, &n) && n == 32 && !memcmp(p, pk, 32));
  int a = bp_dict_get(&r, top, "displays");
  CHECK(bp_type(&r, a) == BP_ARRAY && bp_count(&r, a) == 1);
  CHECK(bp_get_uint(&r, bp_dict_get(&r, bp_array_get(&r, a, 0), "width"), &u) && u == 480);
  CHECK(bp_dict_get(&r, top, "missing") < 0);
  CHECK(bp_array_get(&r, a, 5) < 0);

  // truncation and garbage never crash
  for (size_t cut = 0; cut < len; cut++) {
    bp_reader t;
    int tt = bp_open(&t, buf, cut);
    if (tt >= 0) bp_dict_get(&t, tt, "pk");
  }
  uint8_t junk[200];
  memcpy(junk, buf, len < 200 ? len : 200);
  for (int i = 8; i < 200 && (size_t)i < len - 32; i += 3) junk[i] ^= 0xA5;
  bp_reader t;
  int tt = bp_open(&t, junk, len < 200 ? len : 200);
  if (tt >= 0) { bp_dict_get(&t, tt, "pk"); bp_count(&t, tt); }
  free(buf);
}

// Hand-made plist as Apple writes it: UTF-16 string value and 1-byte refs.
static void test_bplist_utf16(void) {
  static const uint8_t pl[] = {
    'b','p','l','i','s','t','0','0',
    0xD1, 0x01, 0x02,                // dict {1: 2}         @8
    0x54, 'n','a','m','e',           // "name"              @11
    0x63, 0x00,'H', 0x20,0x19, 0x00,'s',  // u"H’s"        @16
    8, 11, 16,                       // offsets            @23
    0,0,0,0,0,0, 1, 1,
    0,0,0,0,0,0,0,3, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,23};
  bp_reader r;
  int top = bp_open(&r, pl, sizeof pl);
  char s[32];
  CHECK(top == 0);
  CHECK(bp_get_string(&r, bp_dict_get(&r, top, "name"), s, sizeof s) && !strcmp(s, "H\xE2\x80\x99s"));
}

static void test_gop(void) {
  static uint8_t arena[100];
  static gop_au aus[4];
  gop_buf g;
  gop_init(&g, arena, sizeof arena, aus, 4);
  uint8_t au[40] = {0, 0, 0, 1, 0x41};
  CHECK(gop_push(&g, au, 10, false, 1) == -1);  // waits for the first IDR
  CHECK(gop_push(&g, au, 30, true, 2) == 0);
  CHECK(gop_push(&g, au, 30, false, 3) == 1);
  CHECK(gop_push(&g, au, 30, false, 4) == 2 && g.used == 90);
  CHECK(gop_push(&g, au, 30, false, 5) == -1 && g.n == 0 && g.wait_idr && g.overflows == 1);
  CHECK(gop_push(&g, au, 10, false, 6) == -1);
  CHECK(gop_push(&g, au, 10, true, 7) == 0 && !g.wait_idr);

  uint8_t idr[] = {0, 0, 0, 1, 0x67, 1, 0, 0, 0, 1, 0x65, 9};
  CHECK(annexb_has_idr(idr, sizeof idr));
  CHECK(!annexb_has_idr(idr, 6));

  uint8_t avcc[] = {0, 0, 0, 2, 0x65, 1, 0, 0, 0, 1, 0x41};
  uint8_t want[] = {0, 0, 0, 1, 0x65, 1, 0, 0, 0, 1, 0x41};
  CHECK(avcc_to_annexb(avcc, sizeof avcc) && !memcmp(avcc, want, sizeof want));
  uint8_t bad[] = {0, 0, 0, 9, 0x65};
  CHECK(!avcc_to_annexb(bad, sizeof bad));
}

// iOS sends an IDR only when mirroring starts: once streamed, the oldest units are dropped to make room.
static void test_gop_trim(void) {
  static uint8_t arena[100];
  static gop_au aus[4];
  gop_buf g;
  gop_init(&g, arena, sizeof arena, aus, 4);
  uint8_t a[30], b[30], c[30], d[30];
  memset(a, 'a', 30), memset(b, 'b', 30), memset(c, 'c', 30), memset(d, 'd', 30);
  CHECK(gop_push(&g, a, 30, true, 1) == 0);
  CHECK(gop_push(&g, b, 30, false, 2) == 1);
  CHECK(gop_push(&g, c, 30, false, 3) == 2);
  CHECK(!gop_fits(&g, 30) && gop_fits(&g, 10));
  uint32_t gen = g.gen;
  gop_drop_front(&g, 2);  // a and b were streamed
  CHECK(g.n == 1 && g.used == 30 && g.base == 2 && g.gen == gen && g.trims == 1);
  CHECK(g.aus[0].off == 0 && g.aus[0].ts == 3 && !g.aus[0].idr && !memcmp(arena, c, 30));
  CHECK(gop_push(&g, d, 30, false, 4) == 1 && g.aus[1].off == 30 && !memcmp(arena + 30, d, 30));
  gop_drop_front(&g, 0);
  CHECK(g.n == 2 && g.base == 2 && g.trims == 1);
  gop_drop_front(&g, 5);  // more than buffered: empties it
  CHECK(g.n == 0 && g.used == 0 && g.base == 4 && !g.wait_idr && g.trims == 2);
  gop_drop_front(&g, 1);  // nothing left to drop
  CHECK(g.base == 4 && g.trims == 2);
  CHECK(gop_push(&g, b, 30, false, 5) == 0 && g.base == 4);  // still live, not waiting for an IDR
  CHECK(gop_push(&g, a, 30, true, 6) == 0 && g.base == 0 && g.gen == gen + 1);  // an IDR anchors it again
}

// Bytes still to stream from unit `at` (counted from the IDR): the app's backlog.
static void test_gop_pending(void) {
  static uint8_t arena[100];
  static gop_au aus[4];
  gop_buf g;
  gop_init(&g, arena, sizeof arena, aus, 4);
  uint8_t au[40] = {0};
  gop_push(&g, au, 30, true, 1), gop_push(&g, au, 20, false, 2), gop_push(&g, au, 10, false, 3);
  CHECK(gop_pending(&g, 0) == 60 && gop_pending(&g, 1) == 30 && gop_pending(&g, 2) == 10 && gop_pending(&g, 3) == 0);
  CHECK(gop_pending(&g, 9) == 0);
  gop_drop_front(&g, 1);  // base 1: unit 1 is now index 0
  CHECK(gop_pending(&g, 1) == 30 && gop_pending(&g, 2) == 10);
  CHECK(gop_pending(&g, 0) == 30);  // already dropped (sent): everything left is pending
}

static void test_chunker(void) {
  uint8_t data[100], out[64], got[100];
  for (int i = 0; i < 100; i++) data[i] = (uint8_t)i;
  vchunk c;
  vchunk_begin(&c, data, sizeof data, VF_IDR | VF_SNAP, 0xAABBCCDD);
  size_t n, got_len = 0;
  int chunks = 0;
  uint16_t seq = 65534;
  while ((n = vchunk_next(&c, out, 20, seq)) > 0) {
    CHECK(n <= VCHUNK_HDR + 20);
    CHECK(out[0] == (uint8_t)seq && out[1] == (uint8_t)(seq >> 8));
    CHECK(out[2] & VF_IDR && out[2] & VF_SNAP);
    size_t off = VCHUNK_HDR;
    if (chunks == 0) {
      CHECK(out[2] & VF_START);
      CHECK(out[3] == 0xDD && out[6] == 0xAA);
      off += 4;
    } else {
      CHECK(!(out[2] & VF_START));
    }
    memcpy(got + got_len, out + off, n - off);
    got_len += n - off;
    chunks++;
    seq++;
    if (got_len < 100) CHECK(!(out[2] & VF_END));
  }
  CHECK(out[2] & VF_END);
  CHECK(got_len == 100 && !memcmp(got, data, 100));
  CHECK(chunks == 6);  // 16 + 5 * 20 >= 100
  CHECK(seq == 4);     // wrapped

  vchunk_begin(&c, data, 3, VF_CONFIG, 1);  // tiny AU: one chunk is START and END
  n = vchunk_next(&c, out, 20, 0);
  CHECK(n == VCHUNK_HDR + 4 + 3 && (out[2] & (VF_START | VF_END | VF_CONFIG)) == (VF_START | VF_END | VF_CONFIG));
  CHECK(vchunk_next(&c, out, 20, 1) == 0);
}

static void test_hid_input(void) {
  uint8_t usage = 0, modifiers = 0, report[8];
  CHECK(hid_ascii('a', &usage, &modifiers) && usage == 4 && modifiers == 0);
  CHECK(hid_ascii('A', &usage, &modifiers) && usage == 4 && modifiers == 2);
  CHECK(hid_ascii('0', &usage, &modifiers) && usage == 39 && modifiers == 0);
  CHECK(hid_ascii(')', &usage, &modifiers) && usage == 39 && modifiers == 2);
  CHECK(hid_ascii('?', &usage, &modifiers) && usage == 56 && modifiers == 2);
  CHECK(hid_ascii('\\', &usage, &modifiers) && usage == 49 && modifiers == 0);
  for (unsigned ch = 32; ch <= 126; ch++) CHECK(hid_ascii(ch, &usage, &modifiers));
  CHECK(!hid_ascii(0, &usage, &modifiers) && !hid_ascii(128, &usage, &modifiers));
  CHECK(hid_ascii_text("Hello, GAgent!\t2026"));
  CHECK(!hid_ascii_text("Hello 中文"));
  CHECK(hid_combo("cmd+h", report) && report[0] == 8 && report[2] == 11 && report[3] == 0);
  CHECK(hid_combo("shift+cmd+a", report) && report[0] == 10 && report[2] == 4);
  CHECK(hid_combo("a+a", report) && report[2] == 4 && report[3] == 0);
  CHECK(hid_combo("ctrl+alt+enter", report) && report[0] == 5 && report[2] == 40);
  CHECK(!hid_combo("", report) && !hid_combo("cmd+", report) && !hid_combo("+a", report));
  CHECK(!hid_combo("cmd+oops", report) && !hid_combo("a+b+c+d+e+f+g", report));
  CHECK(hid_is_stop("stop") && hid_is_stop("stop now"));
  CHECK(!hid_is_stop("stopping") && !hid_is_stop("type stop"));
  long x, y;
  CHECK(hid_pair("-4000 4000", 4000, &x, &y) && x == -4000 && y == 4000);
  CHECK(!hid_pair("-4001 0", 4000, &x, &y));
  CHECK(!hid_pair("1", 4000, &x, &y) && !hid_pair("1 ", 4000, &x, &y));
  CHECK(!hid_pair("1 2 garbage", 4000, &x, &y));
  CHECK(!hid_pair("1 9999999999999999999999999999999", 4000, &x, &y));
  CHECK(!hid_pair("1 128", 127, &x, &y));
}

static void test_wifi_chunker(void) {
  uint8_t data[32769], out[16003], got[32769];
  for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 17);
  vchunk c;
  vchunk_begin(&c, data, sizeof data, VF_IDR, 0x12345678);
  size_t n, received = 0;
  uint16_t seq = 65535;
  while ((n = vchunk_next(&c, out, 16000, seq)) > 0) {
    CHECK(n <= sizeof out);
    CHECK(((uint16_t)out[0] | (uint16_t)out[1] << 8) == seq);
    size_t offset = VCHUNK_HDR;
    if (!received) {
      CHECK(out[2] & VF_START);
      CHECK(out[3] == 0x78 && out[6] == 0x12);
      offset += 4;
    } else CHECK(!(out[2] & VF_START));
    CHECK(received + n - offset <= sizeof got);
    memcpy(got + received, out + offset, n - offset);
    received += n - offset;
    CHECK((bool)(out[2] & VF_END) == (received == sizeof data));
    seq++;
  }
  CHECK(received == sizeof data && !memcmp(data, got, sizeof data));
  CHECK(seq == 2);
}

int main(void) {
  test_bplist_roundtrip();
  test_bplist_utf16();
  test_gop();
  test_gop_trim();
  test_gop_pending();
  test_chunker();
  test_wifi_chunker();
  test_hid_input();
  printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
  return fails != 0;
}
