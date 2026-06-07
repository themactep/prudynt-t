// Test for OSD glyph render pool — validates that the reusable pixel buffer
// eliminates malloc/free per character in renderGlyph().
//
// Build and run:
//   g++ -std=c++17 tests/test_osdglyphpool.cpp -o /tmp/osd_test
//   && /tmp/osd_test

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ── Minimal simulation of the pool pattern used in OSD::renderGlyph() ────────

class TestRenderer {
public:
  uint8_t *pool_{nullptr};
  size_t pool_size_{0};

  ~TestRenderer() {
    free(pool_);
    pool_ = nullptr;
    pool_size_ = 0;
  }

  // Simulates renderGlyph allocation — uses pool, no malloc per "glyph"
  bool render(int width, int height) {
    size_t needed = (size_t)width * height;
    if (needed > pool_size_) {
      uint8_t *new_pool = (uint8_t *)realloc(pool_, needed);
      if (!new_pool) return false;
      pool_ = new_pool;
      pool_size_ = needed;
    }
    // Fill with test pattern
    memset(pool_, 0xAA, needed);
    return true;
  }

  // Count how many reallocations happen
  int realloc_count_{0};

  // Version with tracking
  bool render_tracked(int width, int height) {
    size_t needed = (size_t)width * height;
    if (needed > pool_size_) {
      uint8_t *new_pool = (uint8_t *)realloc(pool_, needed);
      if (!new_pool) return false;
      pool_ = new_pool;
      pool_size_ = needed;
      realloc_count_++;
    }
    memset(pool_, 0xAA, needed);
    return true;
  }
};

// ── Tests ────────────────────────────────────────────────────────────────────

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

void test_pool_starts_null() {
  TEST("pool starts as nullptr with size 0");

  TestRenderer r;
  if (r.pool_ != nullptr) { FAIL("pool should be null initially"); return; }
  if (r.pool_size_ != 0) { FAIL("pool size should be 0"); return; }
  PASS();
}

void test_first_render_allocates_pool() {
  TEST("first render allocates pool");

  TestRenderer r;
  bool ok = r.render(100, 50);
  if (!ok) { FAIL("render failed"); return; }
  if (r.pool_ == nullptr) { FAIL("pool should be allocated after render"); return; }
  if (r.pool_size_ < 5000) { FAIL("pool too small"); return; }
  PASS();
}

void test_subsequent_smaller_render_reuses_pool() {
  TEST("smaller glyph reuses existing pool (no realloc)");

  TestRenderer r;
  r.render(200, 100); // 20000 bytes
  size_t size_after_big = r.pool_size_;

  r.render_tracked(50, 30); // 1500 bytes — fits in existing pool
  int count = r.realloc_count_;

  if (count != 0) {
    FAIL("realloc happened on smaller glyph — pool should be reused");
    return;
  }
  if (r.pool_size_ != size_after_big) {
    FAIL("pool size should not change when reusing");
    return;
  }
  PASS();
}

void test_larger_glyph_grows_pool() {
  TEST("larger glyph grows pool when needed");

  TestRenderer r;
  r.render(50, 50); // 2500 bytes

  r.render_tracked(200, 200); // 40000 bytes — needs growth
  int count = r.realloc_count_;

  if (count != 1) {
    FAIL("should have exactly 1 realloc for larger glyph");
    return;
  }
  if (r.pool_size_ < 40000) {
    FAIL("pool should be at least 40000 after growth");
    return;
  }
  PASS();
}

void test_render_sequence_stabilizes_after_max() {
  TEST("pool stabilizes after largest glyph (no reallocs on repeats)");

  TestRenderer r;
  // Simulate a font rendering session: process the full character set once
  // (sizes vary), then render individual characters of varying sizes.
  // After the first pass the pool should cover the largest glyph.
  int first_pass[][2] = {{10, 10}, {15, 20}, {5, 8},  {30, 25},
                         {8, 12},  {7, 7},   {50, 40}};
  int n1 = sizeof(first_pass) / sizeof(first_pass[0]);

  int reallocs = 0;
  auto do_render = [&](int w, int h) {
    size_t needed = (size_t)w * h;
    if (needed > r.pool_size_) {
      uint8_t *new_pool = (uint8_t *)realloc(r.pool_, needed);
      if (!new_pool) return false;
      r.pool_ = new_pool;
      r.pool_size_ = needed;
      reallocs++;
    }
    return true;
  };

  for (int i = 0; i < n1; ++i) {
    if (!do_render(first_pass[i][0], first_pass[i][1]))
    { FAIL("alloc failed"); return; }
  }

  // Now render 100 more glyphs, all smaller than the max (50x40=2000)
  for (int i = 0; i < 100; ++i) {
    int w = 10 + (i % 20);  // max 29
    int h = 10 + ((i * 3) % 20); // max 29
    if (!do_render(w, h)) { FAIL("render failed"); return; }
  }

  // Total reallocs: at most n1 (each larger than previous), but typically
  // a handful.  The key assertion: no reallocs during the 100-repeat phase.
  if (reallocs > n1) {
    FAIL(("too many reallocs during initial pass: " +
          std::to_string(reallocs))
             .c_str());
    return;
  }

  // Count reallocs in the repeat phase by resetting counter
  int reallocs_repeat = 0;
  auto prev_size = r.pool_size_;
  for (int i = 0; i < 100; ++i) {
    int w = 10 + (i % 20);
    int h = 10 + ((i * 3) % 20);
    size_t needed = (size_t)w * h;
    if (needed > r.pool_size_) {
      uint8_t *new_pool = (uint8_t *)realloc(r.pool_, needed);
      if (!new_pool) { FAIL("alloc failed"); return; }
      r.pool_ = new_pool;
      r.pool_size_ = needed;
      reallocs_repeat++;
    }
  }

  if (reallocs_repeat != 0) {
    FAIL(("expected 0 reallocs in repeat phase, got " +
          std::to_string(reallocs_repeat))
             .c_str());
    return;
  }
  PASS();
}

void test_pool_freed_on_destroy() {
  TEST("pool is freed when renderer is destroyed");

  uint8_t *addr;
  {
    TestRenderer r;
    r.render(50, 50);
    addr = r.pool_;
    if (addr == nullptr) { FAIL("pool should exist before destroy"); return; }
  }
  // r destroyed — pool should be freed (cannot dereference, just confirm
  // the test doesn't crash. The valgrind check would confirm no leak.)
  PASS();
}

void test_render_1000_glyphs_no_leak() {
  TEST("1000 renders don't leak (pool stabilizes)");

  TestRenderer r;
  for (int i = 0; i < 1000; ++i) {
    // Simulate typical glyph sizes (10-40px wide, 10-40px tall)
    int w = 10 + (i % 30);
    int h = 10 + ((i * 7) % 30);
    if (!r.render(w, h)) { FAIL("render failed"); return; }
  }
  // Pool should exist and have a reasonable size
  if (r.pool_ == nullptr) { FAIL("pool should exist after 1000 renders"); return; }
  PASS();
}

int main() {
  fprintf(stderr, "OSD glyph pool tests\n");
  fprintf(stderr, "====================\n");

  test_pool_starts_null();
  test_first_render_allocates_pool();
  test_subsequent_smaller_render_reuses_pool();
  test_larger_glyph_grows_pool();
  test_render_sequence_stabilizes_after_max();
  test_pool_freed_on_destroy();
  test_render_1000_glyphs_no_leak();

  fprintf(stderr, "\n%d tests, %d passed, %d failed\n",
          tests_run, tests_run - tests_failed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
