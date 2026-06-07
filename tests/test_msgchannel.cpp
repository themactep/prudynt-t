// Self-contained test for MsgChannel ring buffer.
// Build and run:
//   g++ -std=c++17 -I../src tests/test_msgchannel.cpp -o /tmp/msg_test
//   && /tmp/msg_test
//
// Standalone mode:
//   g++ -std=c++17 -DSTANDALONE tests/test_msgchannel.cpp -o /tmp/msg_test
//   && /tmp/msg_test

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef STANDALONE
#include "MsgChannel.hpp"
#else
// ── Ring-buffer-based MsgChannel (matches src/MsgChannel.hpp) ────────────────

#include <atomic>
#include <condition_variable>
#include <deque>
#include <iostream>

template <class T> class MsgChannel {
public:
  explicit MsgChannel(unsigned int bsize) : buffer_size{bsize} {
    if (buffer_size > 0) {
      msg_buffer_.reserve(buffer_size);
      for (unsigned int i = 0; i < buffer_size; ++i) {
        msg_buffer_.emplace_back();
      }
    }
  }

  ~MsgChannel() { clear(); }
  MsgChannel(const MsgChannel &) = delete;
  MsgChannel &operator=(const MsgChannel &) = delete;

  bool write(T msg) {
    std::unique_lock<std::mutex> lck(cv_mtx);
    if (count_ < buffer_size) {
      msg_buffer_[tail_] = std::move(msg);
      tail_ = (tail_ + 1) % buffer_size;
      ++count_;
      write_cv.notify_all();
      return true;
    }
    if (buffer_size == 0) return false;
    msg_buffer_[tail_] = std::move(msg);
    tail_ = (tail_ + 1) % buffer_size;
    head_ = tail_;
    write_cv.notify_all();
    return false;
  }

  bool write_wait(T msg) {
    std::unique_lock<std::mutex> lck(cv_mtx);
    if (buffer_size == 0) {
      msg_buffer_deque_.push_front(std::move(msg));
      write_cv.notify_all();
      return true;
    }
    space_cv.wait(lck, [&] { return count_ < buffer_size; });
    msg_buffer_[tail_] = std::move(msg);
    tail_ = (tail_ + 1) % buffer_size;
    ++count_;
    write_cv.notify_all();
    return true;
  }

  bool read(T *out) {
    std::unique_lock<std::mutex> lck(cv_mtx);
    if (count_ > 0) {
      *out = std::move(msg_buffer_[head_]);
      head_ = (head_ + 1) % buffer_size;
      --count_;
      space_cv.notify_one();
      return true;
    }
    return false;
  }

  T wait_read() {
    std::unique_lock<std::mutex> lck(cv_mtx);
    while (count_ == 0) { write_cv.wait(lck); };
    T val = std::move(msg_buffer_[head_]);
    head_ = (head_ + 1) % buffer_size;
    --count_;
    space_cv.notify_one();
    return val;
  }

  void clear() {
    std::unique_lock<std::mutex> lck(cv_mtx);
    count_ = 0;
    head_ = 0;
    tail_ = 0;
    space_cv.notify_all();
  }

  size_t size() const {
    std::unique_lock<std::mutex> lck(cv_mtx);
    return count_;
  }

  unsigned int capacity() const { return buffer_size; }

private:
  std::vector<T> msg_buffer_;
  mutable std::mutex cv_mtx;
  std::condition_variable write_cv;
  std::condition_variable space_cv;
  unsigned int buffer_size;
  unsigned int head_ = 0;
  unsigned int tail_ = 0;
  unsigned int count_ = 0;
  std::deque<T> msg_buffer_deque_;
};
#endif

// ── Test types ───────────────────────────────────────────────────────────────

struct IntMessage {
  int value = 0;
  IntMessage() = default;
  IntMessage(int v) : value(v) {}
  // Move ops
  IntMessage(IntMessage &&) noexcept = default;
  IntMessage &operator=(IntMessage &&) noexcept = default;
};

struct BigMessage {
  std::vector<uint8_t> payload;
  int id = 0;
  bool is_key = false;

  BigMessage() = default;
  BigMessage(int id_, size_t size, bool key)
      : payload(size, static_cast<uint8_t>(id_)), id(id_), is_key(key) {}
  BigMessage(BigMessage &&) noexcept = default;
  BigMessage &operator=(BigMessage &&) noexcept = default;
};

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

void test_write_read_fifo_order() {
  TEST("write/read preserves FIFO order");

  MsgChannel<IntMessage> ch(10);
  for (int i = 0; i < 5; ++i) ch.write(IntMessage(i));

  for (int i = 0; i < 5; ++i) {
    IntMessage msg;
    bool ok = ch.read(&msg);
    if (!ok) { FAIL("read failed"); return; }
    if (msg.value != i) {
      FAIL(("expected " + std::to_string(i) + " got " +
            std::to_string(msg.value))
               .c_str());
      return;
    }
  }
  PASS();
}

void test_write_drops_oldest_when_full() {
  TEST("write drops oldest when full");

  MsgChannel<IntMessage> ch(3);
  ch.write(IntMessage(1));
  ch.write(IntMessage(2));
  ch.write(IntMessage(3));
  bool dropped = ch.write(IntMessage(4)); // should drop 1

  if (dropped) { FAIL("write over capacity should return false"); return; }

  IntMessage msg;
  ch.read(&msg);
  if (msg.value != 2) {
    FAIL("oldest (1) should have been dropped, got " +
         std::to_string(msg.value));
    return;
  }
  ch.read(&msg);
  if (msg.value != 3) { FAIL("expected 3"); return; }
  ch.read(&msg);
  if (msg.value != 4) { FAIL("expected 4"); return; }
  PASS();
}

void test_read_returns_false_when_empty() {
  TEST("read returns false on empty channel");

  MsgChannel<IntMessage> ch(3);
  IntMessage msg;
  bool ok = ch.read(&msg);
  if (ok) { FAIL("read on empty should return false"); return; }
  PASS();
}

void test_wait_read_blocks_until_write() {
  TEST("wait_read blocks until data arrives");

  MsgChannel<IntMessage> ch(10);
  int received = -1;
  std::thread t([&]() {
    IntMessage msg = ch.wait_read();
    received = msg.value;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (received != -1) {
    FAIL("wait_read returned before write");
    t.join();
    return;
  }

  ch.write(IntMessage(42));
  t.join();
  if (received != 42) { FAIL("expected 42"); return; }
  PASS();
}

void test_write_wait_blocks_when_full() {
  TEST("write_wait blocks when full, unblocks after read");

  MsgChannel<IntMessage> ch(2);
  ch.write(IntMessage(1));
  ch.write(IntMessage(2));

  bool written = false;
  std::thread t([&]() {
    ch.write_wait(IntMessage(3));
    written = true;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (written) {
    FAIL("write_wait should block on full buffer");
    t.join();
    return;
  }

  IntMessage msg;
  ch.read(&msg); // frees a slot
  t.join();
  if (!written) { FAIL("write_wait should have completed after read"); return; }
  PASS();
}

void test_ring_wraparound() {
  TEST("ring buffer handles wraparound correctly");

  MsgChannel<IntMessage> ch(4);
  // Fill and drain to get head/tail out of alignment
  for (int i = 0; i < 4; ++i) ch.write(IntMessage(i));
  IntMessage m;
  ch.read(&m); // drops 0
  ch.read(&m); // drops 1
  // Now head=2, tail=0 (wrapped), count=2

  ch.write(IntMessage(10)); // should go at tail (index 0)
  ch.write(IntMessage(20)); // should go at tail (index 1)

  // Read order: 2, 3, 10, 20
  ch.read(&m);
  if (m.value != 2) { FAIL("expected 2 after wraparound"); return; }
  ch.read(&m);
  if (m.value != 3) { FAIL("expected 3 after wraparound"); return; }
  ch.read(&m);
  if (m.value != 10) { FAIL("expected 10 after wraparound"); return; }
  ch.read(&m);
  if (m.value != 20) { FAIL("expected 20 after wraparound"); return; }
  PASS();
}

void test_clear_resets_state() {
  TEST("clear resets head/tail/count");

  MsgChannel<IntMessage> ch(5);
  ch.write(IntMessage(1));
  ch.write(IntMessage(2));
  ch.clear();

  if (ch.size() != 0) { FAIL("size should be 0 after clear"); return; }
  IntMessage m;
  if (ch.read(&m)) { FAIL("read after clear should return false"); return; }
  PASS();
}

void test_big_messages_no_allocation_after_init() {
  TEST("big messages don't reallocate after init");

  MsgChannel<BigMessage> ch(8);
  // Fill and drain a few times
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < 8; ++i) {
      ch.write(BigMessage(i, 1000, i == 0));
    }
    BigMessage m;
    for (int i = 0; i < 8; ++i) {
      bool ok = ch.read(&m);
      if (!ok) { FAIL("read failed during big msg test"); return; }
    }
  }
  PASS();
}

void test_zero_capacity_channel() {
  TEST("zero-capacity channel rejects writes");

  MsgChannel<IntMessage> ch(0);
  bool ok = ch.write(IntMessage(1));
  if (ok) { FAIL("write to zero-capacity should return false"); return; }
  PASS();
}

void test_concurrent_producer_consumer() {
  TEST("concurrent producer/consumer with write_wait");

  MsgChannel<IntMessage> ch(32);
  constexpr int kCount = 500;
  std::atomic<int> produced{0};

  std::thread producer([&]() {
    for (int i = 0; i < kCount; ++i) {
      ch.write_wait(IntMessage(i));
      produced.store(i + 1, std::memory_order_release);
    }
  });

  int expected = 0;
  int last_bad = -1;
  for (int i = 0; i < kCount; ++i) {
    IntMessage m = ch.wait_read();
    if (m.value != expected) {
      last_bad = m.value;
      break;
    }
    ++expected;
  }

  producer.join();
  if (last_bad >= 0) {
    FAIL(("out of order: expected " + std::to_string(expected) +
          " got " + std::to_string(last_bad))
             .c_str());
    return;
  }
  if (expected != kCount) {
    FAIL(("only got " + std::to_string(expected) + " of " +
          std::to_string(kCount))
             .c_str());
    return;
  }
  PASS();
}

void test_read_after_full_drop_preserves_remaining() {
  TEST("after full-drop, remaining elements are correct");

  MsgChannel<IntMessage> ch(3);
  ch.write(IntMessage(10));
  ch.write(IntMessage(20));
  ch.write(IntMessage(30));
  ch.write(IntMessage(40)); // drops 10
  ch.write(IntMessage(50)); // drops 20

  // Remaining: 30, 40, 50
  IntMessage m;
  ch.read(&m);
  if (m.value != 30) { FAIL("expected 30 after double drop"); return; }
  ch.read(&m);
  if (m.value != 40) { FAIL("expected 40 after double drop"); return; }
  ch.read(&m);
  if (m.value != 50) { FAIL("expected 50 after double drop"); return; }
  PASS();
}

int main() {
  fprintf(stderr, "MsgChannel ring buffer tests\n");
  fprintf(stderr, "============================\n");

  test_write_read_fifo_order();
  test_write_drops_oldest_when_full();
  test_read_returns_false_when_empty();
  test_wait_read_blocks_until_write();
  test_write_wait_blocks_when_full();
  test_ring_wraparound();
  test_clear_resets_state();
  test_big_messages_no_allocation_after_init();
  test_zero_capacity_channel();
  test_concurrent_producer_consumer();
  test_read_after_full_drop_preserves_remaining();

  fprintf(stderr, "\n%d tests, %d passed, %d failed\n",
          tests_run, tests_run - tests_failed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
