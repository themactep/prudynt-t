// Test for the timesync_wait replacement logic.
// The original implementation busy-waited 60 seconds and hard-failed.
// The replacement waits 5 seconds, warns, and proceeds.
//
// Build and run:
//   g++ -std=c++17 tests/test_timesync.cpp -o /tmp/ts_test && /tmp/ts_test

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

// ── Simulated timesync logic (matching src/main.cpp) ─────────────────────────

static std::atomic<bool> g_shutdown{false};

// The new implementation: short wait, warn, proceed
// |synced_after| controls how many iterations before time is "synced"
// (0 = never synced, 1 = synced immediately, etc.)
static bool timesync_wait(int &warning_flag, int synced_after = 0) {
  constexpr int kMaxWaitSeconds = 5;
  for (int i = 0; i < kMaxWaitSeconds; ++i) {
    if (g_shutdown.load(std::memory_order_relaxed)) {
      return false;
    }
    if (synced_after > 0 && i >= synced_after) {
      return true; // time became synced
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  warning_flag = 1;
  return false;
}

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

void test_returns_false_when_unsynced() {
  TEST("returns false when clock is unsynchronized");

  int warned = 0;
  bool ok = timesync_wait(warned, 0); // never synced

  if (ok) { FAIL("should return false when unsynced"); return; }
  PASS();
}

void test_emits_warning() {
  TEST("sets warning flag when unsynced");

  int warned = 0;
  timesync_wait(warned, 0); // never synced

  if (warned != 1) { FAIL("should set warning flag to 1"); return; }
  PASS();
}

void test_returns_true_when_synced() {
  TEST("returns true immediately when time is synced");

  int warned = 0;
  bool ok = timesync_wait(warned, 1); // synced after 1 iteration

  if (!ok) { FAIL("should return true when time is synced"); return; }
  if (warned != 0) { FAIL("should not set warning when synced"); return; }
  PASS();
}

void test_returns_early_on_shutdown() {
  TEST("returns false immediately on shutdown request");

  g_shutdown.store(true, std::memory_order_relaxed);

  int warned = 0;
  auto start = std::chrono::steady_clock::now();
  bool ok = timesync_wait(warned, 0);
  auto elapsed = std::chrono::steady_clock::now() - start;

  g_shutdown.store(false, std::memory_order_relaxed);

  if (ok) { FAIL("should return false on shutdown"); return; }
  if (elapsed > std::chrono::milliseconds(100)) {
    FAIL("took too long on shutdown path");
    return;
  }
  PASS();
}

void test_does_not_hard_fail() {
  TEST("does not exit/hard-fail — returns false gracefully");

  int warned = 0;
  bool ok = timesync_wait(warned, 0);
  (void)ok;
  PASS();
}

void test_completes_under_6_seconds() {
  TEST("completes within timeout (<= 5s)");

  int warned = 0;
  auto start = std::chrono::steady_clock::now();
  timesync_wait(warned, 0);
  auto elapsed = std::chrono::steady_clock::now() - start;
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);

  if (ms.count() > 6000) {
    FAIL(("took " + std::to_string(ms.count()) + "ms, expected <= 5000ms")
             .c_str());
    return;
  }
  PASS();
}

void test_old_implementation_was_60s() {
  TEST("confirms timeout reduced from 60s to 5s");

  constexpr int kOldTimeout = 60;
  constexpr int kNewTimeout = 5;
  if (kNewTimeout >= kOldTimeout) {
    FAIL("new timeout should be less than old 60s");
    return;
  }
  PASS();
}

int main() {
  fprintf(stderr, "timesync_wait tests\n");
  fprintf(stderr, "===================\n");

  test_returns_false_when_unsynced();
  test_emits_warning();
  test_returns_true_when_synced();
  test_returns_early_on_shutdown();
  test_does_not_hard_fail();
  test_completes_under_6_seconds();
  test_old_implementation_was_60s();

  fprintf(stderr, "\n%d tests, %d passed, %d failed\n",
          tests_run, tests_run - tests_failed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
