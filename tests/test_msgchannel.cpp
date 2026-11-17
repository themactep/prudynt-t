// Host test for MsgChannel count/byte bounds and eviction accounting.
// Build and run:
//   c++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/test_msgchannel.cpp -o /tmp/test_msgchannel
//   /tmp/test_msgchannel

#include "stream/MsgChannel.hpp"

#include <cstdio>

namespace {

int failures = 0;

void check(bool cond, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: tests/test_msgchannel.cpp:%d\n", line);
    ++failures;
  }
}

#define CHECK(cond) check((cond), __LINE__)

// Frame-like element with the markers and the stream timestamp the age bound
// uses (H264NALUnit has the same fields).
struct TsFrame {
  std::vector<uint8_t> data;
  bool is_frame_start = false;
  bool is_frame_end = false;
  uint32_t frame_id = 0;
  int64_t imp_ts = 0;
  size_t size() const { return data.size(); }
};

TsFrame make_frame(uint32_t id, int64_t ts) {
  TsFrame f;
  f.data.assign(1, 0);
  f.is_frame_start = true;
  f.is_frame_end = true;
  f.frame_id = id;
  f.imp_ts = ts;
  return f;
}

} // namespace

int main() {
  constexpr size_t kInt = sizeof(int);

  // Byte bound with no frame markers (the AudioFrame path).  Evict the oldest
  // elements until the queue fits, keeping at least the newest.
  {
    MsgChannel<int> ch(100, 2 * kInt);
    CHECK(ch.write(1));
    CHECK(ch.write(2));
    CHECK(ch.evictions() == 0);
    CHECK(!ch.write(3)); // over the byte budget -> evicts
    CHECK(ch.evictions() == 1);
    CHECK(ch.size() == 2);
    CHECK(!ch.write(4));
    CHECK(ch.evictions() == 2);
    CHECK(ch.size() == 2);
  }

  // A single element larger than the byte budget is kept, and it must not be
  // reported as an eviction (or fMP4 readers resync for no reason).
  {
    MsgChannel<int> ch(100, 1); // budget below one int
    ch.write(7);
    CHECK(ch.size() == 1);
    CHECK(ch.evictions() == 0);
  }

  // Count bound still applies without a byte bound.
  {
    MsgChannel<int> ch(2, 0);
    CHECK(ch.write(1));
    CHECK(ch.write(2));
    CHECK(!ch.write(3));
    CHECK(ch.size() == 2);
    CHECK(ch.evictions() == 1);
  }

  // Age bound: the oldest frame is evicted once the queue spans more than the
  // limit; the newest frame stays.
  {
    MsgChannel<TsFrame> ch(100, 0, 2'000'000);
    CHECK(ch.write(make_frame(1, 0)));
    CHECK(ch.write(make_frame(2, 1'000'000)));
    CHECK(ch.write(make_frame(3, 2'000'000)));
    CHECK(ch.evictions() == 0);
    CHECK(!ch.write(make_frame(4, 2'500'000)));
    CHECK(ch.evictions() == 1);
    CHECK(ch.size() == 3);
  }

  // A single frame is kept (nothing older to drop), and types without a stream
  // timestamp ignore the age bound.
  {
    MsgChannel<TsFrame> ch(100, 0, 2'000'000);
    CHECK(ch.write(make_frame(1, 0)));
    CHECK(ch.evictions() == 0);
    CHECK(ch.size() == 1);

    MsgChannel<int> no_ts(100, 0, 2'000'000);
    CHECK(no_ts.write(1));
    CHECK(no_ts.write(2));
    CHECK(no_ts.evictions() == 0);
  }

  if (failures == 0)
    std::printf(
        "PASS: MsgChannel count/byte/age bounds and eviction accounting\n");
  return failures ? 1 : 0;
}
