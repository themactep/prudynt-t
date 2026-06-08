// Test for AudioData inline buffer — standalone copy of the type.
// Build and run:
//   g++ -std=c++17 tests/test_audiodata.cpp -o /tmp/ad_test && /tmp/ad_test

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/time.h>
#include <vector>

// Standalone copy of AudioData (from globals.hpp)
struct alignas(8) AudioData {
  static constexpr size_t kMaxBytes = 4096;
  uint8_t buf[kMaxBytes];
  size_t len = 0;

  const uint8_t *data() const { return buf; }
  uint8_t *data() { return buf; }
  size_t size() const { return len; }
  bool empty() const { return len == 0; }
  void clear() { len = 0; }
  uint8_t *end() { return buf + len; }

  void insert(uint8_t *, const uint8_t *start, const uint8_t *end) {
    size_t count = static_cast<size_t>(end - start);
    if (count == 0) return;
    size_t avail = kMaxBytes - len;
    if (count > avail) count = avail;
    std::copy(start, start + count, buf + len);
    len += count;
  }

  void assign(const uint8_t *src, size_t count) {
    if (count > kMaxBytes) count = kMaxBytes;
    std::copy(src, src + count, buf);
    len = count;
  }
};

// Minimal AudioFrame for struct test
struct AudioFrame {
  AudioData data;
  struct timeval time;
};

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

void test_empty_after_construction() {
  TEST("starts empty with size 0");

  AudioData ad;
  if (!ad.empty()) { FAIL("should be empty"); return; }
  if (ad.size() != 0) { FAIL("size should be 0"); return; }
  PASS();
}

void test_assign_small_data() {
  TEST("assign small data");

  AudioData ad;
  uint8_t src[] = {0x01, 0x02, 0x03, 0x04};
  ad.assign(src, sizeof(src));

  if (ad.empty()) { FAIL("should not be empty"); return; }
  if (ad.size() != 4) { FAIL("size should be 4"); return; }
  if (memcmp(ad.data(), src, 4) != 0) { FAIL("data mismatch"); return; }
  PASS();
}

void test_insert_range() {
  TEST("insert range (AudioWorker pattern)");

  AudioData ad;
  uint8_t data[] = {0xAA, 0xBB, 0xCC};
  // Simulate the pattern used in AudioWorker:
  //   af.data.insert(af.data.end(), start, end)
  ad.insert(ad.end(), data, data + 3);

  if (ad.size() != 3) { FAIL("size should be 3"); return; }
  if (memcmp(ad.data(), data, 3) != 0) { FAIL("data mismatch"); return; }
  PASS();
}

void test_insert_multiple_calls() {
  TEST("multiple insert calls append sequentially");

  AudioData ad;
  uint8_t a[] = {0x01, 0x02};
  uint8_t b[] = {0x03, 0x04, 0x05};
  ad.insert(ad.end(), a, a + 2);
  ad.insert(ad.end(), b, b + 3);

  if (ad.size() != 5) { FAIL("size should be 5"); return; }
  uint8_t expected[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  if (memcmp(ad.data(), expected, 5) != 0) { FAIL("data mismatch"); return; }
  PASS();
}

void test_clear_resets() {
  TEST("clear resets size to 0");

  AudioData ad;
  uint8_t d[] = {0xDE, 0xAD};
  ad.assign(d, 2);
  ad.clear();

  if (ad.size() != 0) { FAIL("size should be 0 after clear"); return; }
  if (!ad.empty()) { FAIL("should be empty after clear"); return; }
  PASS();
}

void test_capacity_limit() {
  TEST("data truncated at kMaxBytes");

  AudioData ad;
  std::vector<uint8_t> big(AudioData::kMaxBytes + 100, 0xFF);
  ad.assign(big.data(), big.size());

  if (ad.size() != AudioData::kMaxBytes) {
    FAIL(("expected " + std::to_string(AudioData::kMaxBytes) + " bytes, got " +
          std::to_string(ad.size()))
             .c_str());
    return;
  }
  PASS();
}

void test_insert_capacity_limit() {
  TEST("insert truncated at kMaxBytes");

  AudioData ad;
  uint8_t fill = 0x42;
  std::vector<uint8_t> big(AudioData::kMaxBytes + 50, fill);
  // Append half
  ad.insert(ad.end(), big.data(), big.data() + AudioData::kMaxBytes);
  // Try to append more (will be truncated)
  ad.insert(ad.end(), big.data() + AudioData::kMaxBytes,
            big.data() + big.size());

  if (ad.size() != AudioData::kMaxBytes) {
    FAIL("should be capped at kMaxBytes after over-insert");
    return;
  }
  PASS();
}

void test_data_pointer_readonly() {
  TEST("data() returns const-correct pointer");

  AudioData ad;
  uint8_t d[] = {0xCA, 0xFE};
  ad.assign(d, 2);

  const uint8_t *ptr = ad.data();
  if (!ptr) { FAIL("data() should not be null"); return; }
  if (ptr[0] != 0xCA || ptr[1] != 0xFE) {
    FAIL("data content wrong"); return;
  }
  PASS();
}

void test_mutable_data_pointer() {
  TEST("non-const data() allows modification");

  AudioData ad;
  uint8_t d[] = {0x01, 0x02};
  ad.assign(d, 2);

  uint8_t *ptr = ad.data();
  ptr[0] = 0xFF;

  if (ad.data()[0] != 0xFF) { FAIL("mutate failed"); return; }
  PASS();
}

void test_audioframe_struct() {
  TEST("AudioFrame still works with new AudioData");

  AudioFrame af;
  (void)af.time; // just verify it compiles

  uint8_t pcm[] = {0x00, 0x01, 0x02, 0x03};
  af.data.assign(pcm, sizeof(pcm));

  if (af.data.size() != 4) { FAIL("AudioFrame data size wrong"); return; }
  PASS();
}

void test_end_pointer() {
  TEST("end() returns pointer past written data");

  AudioData ad;
  uint8_t d[] = {0x01, 0x02, 0x03};
  ad.assign(d, 3);

  uint8_t *end = ad.end();
  // Writing at end() should work
  end[0] = 0x04;
  // But size doesn't change
  if (ad.size() != 3) { FAIL("size unchanged after writing at end()"); return; }
  PASS();
}

int main() {
  fprintf(stderr, "AudioData inline buffer tests\n");
  fprintf(stderr, "============================\n");

  test_empty_after_construction();
  test_assign_small_data();
  test_insert_range();
  test_insert_multiple_calls();
  test_clear_resets();
  test_capacity_limit();
  test_insert_capacity_limit();
  test_data_pointer_readonly();
  test_mutable_data_pointer();
  test_audioframe_struct();
  test_end_pointer();

  fprintf(stderr, "\n%d tests, %d passed, %d failed\n",
          tests_run, tests_run - tests_failed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
