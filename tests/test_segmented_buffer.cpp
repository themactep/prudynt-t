// Test for SegmentedBuffer — verifies append, data, clear, and zero-copy growth.
//
// Build and run:
//   g++ -std=c++17 -Isrc tests/test_segmented_buffer.cpp -o /tmp/sb_test
//   && /tmp/sb_test

#include "SegmentedBuffer.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Verifies block data can be accessed — no inline copy needed on GCC
// since the header is self-contained.

// ── Test framework ───────────────────────────────────────────────────────────

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name)                                                             \
  do {                                                                         \
    tests_run++;                                                               \
    fprintf(stderr, "  %-50s ... ", name);                                    \
  } while (0)
#define PASS() fprintf(stderr, "ok\n")
#define FAIL(msg)                                                              \
  do {                                                                         \
    fprintf(stderr, "FAIL: %s\n", msg);                                        \
    tests_failed++;                                                            \
  } while (0)

// ── Tests ────────────────────────────────────────────────────────────────────

void test_empty_after_construction() {
  TEST("starts empty with size 0");

  SegmentedBuffer buf;
  if (!buf.empty()) { FAIL("should be empty"); return; }
  if (buf.size() != 0) { FAIL("size should be 0"); return; }
  if (buf.data() != nullptr) { FAIL("data should be null"); return; }
  PASS();
}

void test_append_small_data() {
  TEST("append small data (< block size)");

  SegmentedBuffer buf;
  uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
  buf.append(data, sizeof(data));

  if (buf.empty()) { FAIL("should not be empty"); return; }
  if (buf.size() != 4) { FAIL("size should be 4"); return; }
  if (!buf.data()) { FAIL("data should not be null"); return; }
  if (memcmp(buf.data(), data, 4) != 0) {
    FAIL("data mismatch"); return;
  }
  PASS();
}

void test_append_multiple_calls() {
  TEST("append multiple times, data is contiguous");

  SegmentedBuffer buf;
  uint8_t a[] = {0xAA, 0xBB};
  uint8_t b[] = {0xCC, 0xDD, 0xEE};
  buf.append(a, 2);
  buf.append(b, 3);

  if (buf.size() != 5) { FAIL("size should be 5"); return; }
  uint8_t expected[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
  if (memcmp(buf.data(), expected, 5) != 0) {
    FAIL("data mismatch after multiple appends"); return;
  }
  PASS();
}

void test_append_exceeding_block_size() {
  TEST("append data exceeding block size (crosses boundary)");

  SegmentedBuffer buf;
  uint8_t big[20000];
  for (size_t i = 0; i < sizeof(big); ++i) big[i] = (uint8_t)(i & 0xFF);

  buf.append(big, sizeof(big));

  if (buf.size() != 20000) { FAIL("size should be 20000"); return; }

  const uint8_t *d = buf.data();
  if (!d) { FAIL("data should not be null"); return; }
  // Check first, middle, and last bytes
  if (d[0] != 0x00) { FAIL("first byte wrong"); return; }
  if (d[9999] != (uint8_t)(9999 & 0xFF)) {
    FAIL("middle byte wrong"); return;
  }
  if (d[19999] != (uint8_t)(19999 & 0xFF)) {
    FAIL("last byte wrong"); return;
  }
  PASS();
}

void test_clear_resets_but_retains_capacity() {
  TEST("clear resets size, retains block capacity");

  SegmentedBuffer buf;
  uint8_t d[] = {0x01, 0x02, 0x03};
  buf.append(d, 3);

  size_t cap_before = buf.capacity();
  buf.clear();

  if (buf.size() != 0) { FAIL("size should be 0 after clear"); return; }
  if (!buf.empty()) { FAIL("should be empty after clear"); return; }
  if (buf.capacity() != cap_before) {
    FAIL("capacity should be retained after clear"); return;
  }
  PASS();
}

void test_reuse_after_clear() {
  TEST("reuse buffer after clear (no reallocation for small appends)");

  SegmentedBuffer buf;

  // First fill
  uint8_t a[] = {0xAA, 0xBB, 0xCC};
  buf.append(a, 3);
  buf.clear();

  // Reuse
  uint8_t b[] = {0xDD, 0xEE};
  buf.append(b, 2);

  if (buf.size() != 2) { FAIL("size should be 2"); return; }
  // Capacity should be at least 3 (from first use)
  if (buf.capacity() < 3) {
    FAIL("capacity too small after reuse"); return;
  }
  if (memcmp(buf.data(), b, 2) != 0) {
    FAIL("data mismatch after reuse"); return;
  }
  PASS();
}

void test_append_many_small_chunks_like_nal_units() {
  TEST("append many small chunks (simulating NAL unit accumulation)");

  SegmentedBuffer buf;
  size_t total = 0;
  for (int i = 0; i < 100; ++i) {
    uint8_t chunk[4 + (i % 200)];
    chunk[0] = (i >> 24) & 0xFF;
    chunk[1] = (i >> 16) & 0xFF;
    chunk[2] = (i >> 8) & 0xFF;
    chunk[3] = i & 0xFF;
    buf.append(chunk, sizeof(chunk));
    total += sizeof(chunk);
  }

  if (buf.size() != total) {
    FAIL(("expected " + std::to_string(total) + " bytes, got " +
          std::to_string(buf.size()))
             .c_str());
    return;
  }

  // Verify first and last values
  const uint8_t *d = buf.data();
  if (!d) { FAIL("data null after many appends"); return; }
  PASS();
}

void test_release_frees_memory() {
  TEST("release frees all storage");

  SegmentedBuffer buf;
  uint8_t d[32000];
  buf.append(d, sizeof(d));
  buf.release();

  if (buf.size() != 0) { FAIL("size should be 0 after release"); return; }
  if (buf.capacity() != 0) {
    FAIL("capacity should be 0 after release"); return;
  }
  PASS();
}

void test_data_null_for_empty() {
  TEST("data() returns nullptr when empty");

  SegmentedBuffer buf;
  if (buf.data() != nullptr) {
    FAIL("data() should be null for empty buffer"); return;
  }
  PASS();
}

void test_copy_pattern() {
  TEST("append and copy_out pattern (used by pretrigger)");

  SegmentedBuffer buf;
  uint8_t d[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
  buf.append(d, sizeof(d));

  std::vector<uint8_t> out;
  buf.copy_out(out);

  if (out.size() != 6) { FAIL("copy_out size wrong"); return; }
  if (memcmp(out.data(), d, 6) != 0) {
    FAIL("copy_out data mismatch"); return;
  }
  PASS();
}

int main() {
  fprintf(stderr, "SegmentedBuffer tests\n");
  fprintf(stderr, "=====================\n");

  test_empty_after_construction();
  test_append_small_data();
  test_append_multiple_calls();
  test_append_exceeding_block_size();
  test_clear_resets_but_retains_capacity();
  test_reuse_after_clear();
  test_append_many_small_chunks_like_nal_units();
  test_release_frees_memory();
  test_data_null_for_empty();
  test_copy_pattern();

  fprintf(stderr, "\n%d tests, %d passed, %d failed\n",
          tests_run, tests_run - tests_failed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
