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

  if (failures == 0)
    std::printf("PASS: MsgChannel count/byte bounds and eviction accounting\n");
  return failures ? 1 : 0;
}
