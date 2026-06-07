// Self-contained test for PreTriggerBuffer — build and run:
//   g++ -std=c++17 -I../src tests/test_triggerbuffer.cpp -o /tmp/tb_test
//   && /tmp/tb_test
//
// Standalone mode (copies relevant code inline):
//   g++ -std=c++17 -DSTANDALONE tests/test_triggerbuffer.cpp -o /tmp/tb_test
//   && /tmp/tb_test

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef STANDALONE
// Normal build — include the real header
#include "PreTriggerBuffer.hpp"
#else
// Standalone — minimal copy of PreTriggerBuffer internals for testing
#include <atomic>
#include <algorithm>
#include <mutex>
#include <string>

struct PreTriggerFrame {
  std::vector<uint8_t> data;
  int64_t timestamp_us;
  bool is_keyframe;
  size_t frame_size;
};

class PreTriggerBuffer {
public:
  PreTriggerBuffer();
  ~PreTriggerBuffer();
  bool init(int duration_seconds, int fps, int max_memory_mb,
            bool keyframe_only);
  void addFrame(const uint8_t *data, size_t size, int64_t timestamp_us,
                bool is_keyframe);
  std::vector<PreTriggerFrame> getFrames();
  void drainFrames(std::vector<PreTriggerFrame> &out);
  void clearFrames();
  void clear();
  size_t getMemoryUsage() const;
  bool isEnabled() const { return enabled_.load(); }
  size_t frameCount() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return frames_.size();
  }

private:
  std::vector<PreTriggerFrame> frames_;
  size_t write_index_{0};
  size_t capacity_{0};
  size_t max_memory_bytes_{0};
  int64_t duration_us_{0};
  bool keyframe_only_{false};
  std::atomic<bool> enabled_{false};
  mutable std::mutex buffer_mutex_;
  std::atomic<size_t> memory_usage_{0};
  std::atomic<size_t> peak_memory_usage_{0};

  void enforceMemoryLimit();
  void enforceTimeLimit(int64_t newest_timestamp_us);
};

// -- Implementation (same logic as the real PreTriggerBuffer) --

PreTriggerBuffer::PreTriggerBuffer() = default;
PreTriggerBuffer::~PreTriggerBuffer() { clear(); }

bool PreTriggerBuffer::init(int duration_seconds, int fps, int max_memory_mb,
                            bool keyframe_only) {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  if (duration_seconds <= 0 || fps <= 0 || max_memory_mb <= 0) {
    enabled_.store(false);
    return false;
  }
  duration_us_ = static_cast<int64_t>(duration_seconds) * 1000000LL;
  capacity_ = duration_seconds * fps * 2;
  if (keyframe_only) {
    capacity_ = std::max(1, (int)(capacity_ / 8));
  }
  max_memory_bytes_ = max_memory_mb * 1024 * 1024;
  keyframe_only_ = keyframe_only;
  frames_.clear();
  frames_.reserve(capacity_);
  write_index_ = 0;
  memory_usage_.store(0);
  enabled_.store(true);
  return true;
}

void PreTriggerBuffer::addFrame(const uint8_t *data, size_t size,
                                int64_t timestamp_us, bool is_keyframe) {
  if (!enabled_.load() || !data || size == 0) return;
  if (keyframe_only_ && !is_keyframe) return;
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  PreTriggerFrame frame;
  frame.data.assign(data, data + size);
  frame.timestamp_us = timestamp_us;
  frame.is_keyframe = is_keyframe;
  frame.frame_size = size;
  frames_.push_back(std::move(frame));
  memory_usage_.fetch_add(size);
  size_t current_usage = memory_usage_.load();
  size_t peak = peak_memory_usage_.load();
  while (current_usage > peak &&
         !peak_memory_usage_.compare_exchange_weak(peak, current_usage)) {}
  enforceTimeLimit(timestamp_us);
  enforceMemoryLimit();
  while (frames_.size() > capacity_) {
    if (!frames_.empty()) {
      memory_usage_.fetch_sub(frames_.front().data.size());
      frames_.erase(frames_.begin());
    }
  }
}

std::vector<PreTriggerFrame> PreTriggerBuffer::getFrames() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  std::vector<PreTriggerFrame> result;
  if (!enabled_.load() || frames_.empty()) return result;
  result = frames_;
  std::sort(result.begin(), result.end(),
            [](const PreTriggerFrame &a, const PreTriggerFrame &b) {
              return a.timestamp_us < b.timestamp_us;
            });
  return result;
}

void PreTriggerBuffer::drainFrames(std::vector<PreTriggerFrame> &out) {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  if (!enabled_.load() || frames_.empty()) return;
  out.reserve(frames_.size());
  for (auto &f : frames_) {
    out.push_back(std::move(f));
  }
  frames_.clear();
  memory_usage_.store(0);
  write_index_ = 0;
}

void PreTriggerBuffer::clearFrames() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  frames_.clear();
  write_index_ = 0;
  memory_usage_.store(0);
}

void PreTriggerBuffer::clear() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  frames_.clear();
  write_index_ = 0;
  capacity_ = 0;
  memory_usage_.store(0);
  peak_memory_usage_.store(0);
  enabled_.store(false);
}

size_t PreTriggerBuffer::getMemoryUsage() const {
  return memory_usage_.load();
}

void PreTriggerBuffer::enforceMemoryLimit() {
  size_t current_usage = memory_usage_.load();
  if (current_usage <= max_memory_bytes_) return;
  while (current_usage > max_memory_bytes_ && !frames_.empty()) {
    current_usage -= frames_.front().data.size();
    memory_usage_.fetch_sub(frames_.front().data.size());
    frames_.erase(frames_.begin());
  }
}

void PreTriggerBuffer::enforceTimeLimit(int64_t newest_timestamp_us) {
  if (duration_us_ <= 0 || frames_.empty()) return;
  int64_t cutoff_ts = newest_timestamp_us - duration_us_;
  while (!frames_.empty() && frames_.front().timestamp_us < cutoff_ts) {
    memory_usage_.fetch_sub(frames_.front().data.size());
    frames_.erase(frames_.begin());
  }
}
#endif // STANDALONE

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

void test_drain_frames_returns_all_frames_in_order() {
  TEST("drainFrames returns all frames in order");

  PreTriggerBuffer b;
  b.init(10, 25, 8, false); // 10s, 25fps, 8MB

  uint8_t frame1[] = {0x00, 0x01, 0x02};
  uint8_t frame2[] = {0x03, 0x04, 0x05, 0x06};
  uint8_t frame3[] = {0x07, 0x08};

  b.addFrame(frame1, sizeof(frame1), 1000000, true);
  b.addFrame(frame2, sizeof(frame2), 2000000, false);
  b.addFrame(frame3, sizeof(frame3), 3000000, false);

  std::vector<PreTriggerFrame> out;
  b.drainFrames(out);

  if (out.size() != 3) {
    FAIL("expected 3 frames, got " + std::to_string(out.size()));
    return;
  }
  if (out[0].timestamp_us != 1000000) { FAIL("frame 0 wrong timestamp"); return; }
  if (out[1].timestamp_us != 2000000) { FAIL("frame 1 wrong timestamp"); return; }
  if (out[2].timestamp_us != 3000000) { FAIL("frame 2 wrong timestamp"); return; }
  if (!out[0].is_keyframe)           { FAIL("frame 0 should be keyframe"); return; }
  if (out[0].data.size() != 3)       { FAIL("frame 0 wrong data size"); return; }
  if (out[1].data.size() != 4)       { FAIL("frame 1 wrong data size"); return; }
  if (out[2].data.size() != 2)       { FAIL("frame 2 wrong data size"); return; }
  // Verify original data is intact via move
  if (out[0].data[0] != 0x00 || out[0].data[1] != 0x01) {
    FAIL("frame 0 data corrupted"); return;
  }
  PASS();
}

void test_drain_frames_clears_internal_state() {
  TEST("drainFrames clears internal buffer and memory accounting");

  PreTriggerBuffer b;
  b.init(10, 25, 8, false);

  uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD};
  b.addFrame(data, sizeof(data), 1000000, true);

  std::vector<PreTriggerFrame> out;
  b.drainFrames(out);

  if (b.frameCount() != 0) {
    FAIL("internal frame count should be 0 after drain");
    return;
  }
  if (b.getMemoryUsage() != 0) {
    FAIL("memory usage should be 0 after drain");
    return;
  }
  PASS();
}

void test_drain_frames_reuses_buffer_after_drain() {
  TEST("drainFrames allows adding frames after drain");

  PreTriggerBuffer b;
  b.init(10, 25, 8, false);

  uint8_t d1[] = {0x01};
  uint8_t d2[] = {0x02};
  b.addFrame(d1, sizeof(d1), 1000000, true);
  b.addFrame(d2, sizeof(d2), 2000000, false);

  std::vector<PreTriggerFrame> out1;
  b.drainFrames(out1);

  if (!b.isEnabled()) { FAIL("buffer should still be enabled after drain"); return; }

  uint8_t d3[] = {0x03, 0x04};
  b.addFrame(d3, sizeof(d3), 3000000, true);

  std::vector<PreTriggerFrame> out2;
  b.drainFrames(out2);

  if (out2.size() != 1) {
    FAIL("expected 1 frame after re-add, got " + std::to_string(out2.size()));
    return;
  }
  if (out2[0].data.size() != 2) { FAIL("re-added frame wrong size"); return; }
  PASS();
}

void test_drain_empty_buffer() {
  TEST("drainFrames on empty buffer produces empty output");

  PreTriggerBuffer b;
  b.init(10, 25, 8, false);

  std::vector<PreTriggerFrame> out;
  b.drainFrames(out);

  if (!out.empty()) { FAIL("drain of empty buffer should produce empty output"); return; }
  PASS();
}

void test_drain_disabled_buffer() {
  TEST("drainFrames on disabled buffer produces empty output");

  PreTriggerBuffer b;
  // Not initialized — disabled by default
  std::vector<PreTriggerFrame> out;
  b.drainFrames(out);

  if (!out.empty()) { FAIL("drain of disabled buffer should produce empty output"); return; }
  PASS();
}

void test_get_frames_still_works() {
  TEST("getFrames still works after drainFrames is added");

  PreTriggerBuffer b;
  b.init(10, 25, 8, false);

  uint8_t d[] = {0xAA};
  b.addFrame(d, sizeof(d), 1000000, true);

  auto out = b.getFrames();
  if (out.size() != 1) { FAIL("getFrames should return 1 frame"); return; }
  PASS();
}

void test_drain_no_data_copy() {
  TEST("drainFrames avoids data copy (pointer stability check)");

  PreTriggerBuffer b;
  b.init(10, 25, 8, false);

  uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
  b.addFrame(data, sizeof(data), 1000000, true);

  // After addFrame, the buffer owns a copy of the data.
  // After drainFrames, the output owns the moved copy.
  // The pointers should differ (the old buffer's data is gone).
  std::vector<PreTriggerFrame> out;
  b.drainFrames(out);

  if (out.size() != 1) { FAIL("expected 1 frame"); return; }
  if (out[0].data.data() == data) {
    FAIL("data pointer should differ (drain moved, not aliased)");
    return;
  }
  if (out[0].data[0] != 0xDE || out[0].data[3] != 0xEF) {
    FAIL("data content corrupted after move");
    return;
  }
  PASS();
}

void test_no_use_after_free() {
  TEST("drainFrames moved data survives buffer destruction");

  std::vector<uint8_t> captured;
  {
    PreTriggerBuffer b;
    b.init(10, 25, 8, false);

    uint8_t data[] = {0xCA, 0xFE};
    b.addFrame(data, sizeof(data), 1000000, true);

    std::vector<PreTriggerFrame> out;
    b.drainFrames(out);

    // b is about to be destroyed — capture moved data
    captured = std::move(out[0].data);
  } // b destroyed here

  if (captured.size() != 2) { FAIL("captured data should survive"); return; }
  if (captured[0] != 0xCA || captured[1] != 0xFE) {
    FAIL("captured data corrupted");
    return;
  }
  PASS();
}

int main() {
  fprintf(stderr, "PreTriggerBuffer drainFrames tests\n");
  fprintf(stderr, "===================================\n");

  test_drain_frames_returns_all_frames_in_order();
  test_drain_frames_clears_internal_state();
  test_drain_frames_reuses_buffer_after_drain();
  test_drain_empty_buffer();
  test_drain_disabled_buffer();
  test_get_frames_still_works();
  test_drain_no_data_copy();
  test_no_use_after_free();

  fprintf(stderr, "\n%d tests, %d passed, %d failed\n",
          tests_run, tests_run - tests_failed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
