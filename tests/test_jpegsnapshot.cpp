// Self-contained test for jpeg_stream snapshot_buf capping.
// Build and run:
//   g++ -std=c++17 tests/test_jpegsnapshot.cpp -o /tmp/jpg_test
//   && /tmp/jpg_test

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// ── Minimal jpeg_stream (mirrors globals.hpp) ────────────────────────────────

struct _stream; // not needed for the snapshot test

struct jpeg_stream {
  static constexpr size_t MAX_SNAPSHOT_BYTES = 512 * 1024; // 512KB

  std::vector<unsigned char> snapshot_buf;

  // Simulate JPEG worker: store a JPEG of given size.
  // Returns true if stored, false if skipped (oversized).
  bool store_snapshot(const uint8_t *data, size_t size) {
    if (!data || size == 0) return false;
    if (size > MAX_SNAPSHOT_BYTES) return false;
    snapshot_buf.assign(data, data + size);
    return true;
  }

  // Simulate reader: copy snapshot out.
  std::vector<unsigned char> read_snapshot() const {
    return snapshot_buf;
  }
};

// ── Tests ────────────────────────────────────────────────────────────────────

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name)                                                             \
  do {                                                                         \
    tests_run++;                                                               \
    fprintf(stderr, "  %-45s ... ", name);                                    \
  } while (0)
#define PASS() fprintf(stderr, "ok\n")
#define FAIL(msg)                                                              \
  do {                                                                         \
    fprintf(stderr, "FAIL: %s\n", msg);                                        \
    tests_failed++;                                                            \
  } while (0)

void test_store_normal_jpeg() {
  TEST("stores JPEG under the cap");

  jpeg_stream js;
  uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46};
  bool ok = js.store_snapshot(jpeg, sizeof(jpeg));

  if (!ok) { FAIL("store should succeed"); return; }
  auto out = js.read_snapshot();
  if (out.size() != sizeof(jpeg)) { FAIL("size mismatch"); return; }
  if (memcmp(out.data(), jpeg, sizeof(jpeg)) != 0) {
    FAIL("data mismatch"); return;
  }
  PASS();
}

void test_skip_oversized_jpeg() {
  TEST("skips JPEG exceeding the cap, retains previous");

  jpeg_stream js;
  uint8_t small[] = {0xFF, 0xD8, 0xFF};
  js.store_snapshot(small, sizeof(small));

  // Build a buffer larger than MAX_SNAPSHOT_BYTES
  size_t huge_size = jpeg_stream::MAX_SNAPSHOT_BYTES + 1;
  std::vector<uint8_t> huge(huge_size, 0xAA);

  bool ok = js.store_snapshot(huge.data(), huge.size());
  if (ok) { FAIL("oversized store should be rejected"); return; }

  // Previous snapshot should be intact
  auto out = js.read_snapshot();
  if (out.size() != sizeof(small)) {
    FAIL("previous snapshot should be retained");
    return;
  }
  PASS();
}

void test_store_exact_max_size() {
  TEST("stores JPEG exactly at the cap boundary");

  jpeg_stream js;
  std::vector<uint8_t> jpeg(jpeg_stream::MAX_SNAPSHOT_BYTES, 0xFF);
  jpeg[0] = 0xFF; jpeg[1] = 0xD8; // SOI marker

  bool ok = js.store_snapshot(jpeg.data(), jpeg.size());
  if (!ok) { FAIL("store at cap boundary should succeed"); return; }

  auto out = js.read_snapshot();
  if (out.size() != jpeg_stream::MAX_SNAPSHOT_BYTES) {
    FAIL("size mismatch at boundary"); return;
  }
  PASS();
}

void test_empty_after_constructor() {
  TEST("starts with empty snapshot");

  jpeg_stream js;
  auto out = js.read_snapshot();
  if (!out.empty()) { FAIL("should start empty"); return; }
  PASS();
}

void test_overwrite_smaller_jpeg() {
  TEST("overwrites with smaller JPEG, shrinks buffer");

  jpeg_stream js;
  uint8_t large[100] = {0};
  uint8_t small[10] = {0xFF, 0xD8, 0xFF};

  js.store_snapshot(large, sizeof(large));
  js.store_snapshot(small, sizeof(small));

  auto out = js.read_snapshot();
  if (out.size() != sizeof(small)) {
    FAIL("buffer should shrink to small size");
    return;
  }
  PASS();
}

void test_rejects_null_data() {
  TEST("rejects null data");

  jpeg_stream js;
  bool ok = js.store_snapshot(nullptr, 100);
  if (ok) { FAIL("null data should be rejected"); return; }
  PASS();
}

void test_rejects_zero_size() {
  TEST("rejects zero-size data");

  jpeg_stream js;
  uint8_t d[] = {0xFF};
  bool ok = js.store_snapshot(d, 0);
  if (ok) { FAIL("zero size should be rejected"); return; }
  PASS();
}

int main() {
  fprintf(stderr, "jpeg_stream snapshot cap tests\n");
  fprintf(stderr, "==============================\n");

  test_store_normal_jpeg();
  test_skip_oversized_jpeg();
  test_store_exact_max_size();
  test_empty_after_constructor();
  test_overwrite_smaller_jpeg();
  test_rejects_null_data();
  test_rejects_zero_size();

  fprintf(stderr, "\n%d tests, %d passed, %d failed\n",
          tests_run, tests_run - tests_failed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
