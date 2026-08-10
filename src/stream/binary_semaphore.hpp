#ifndef BINARY_SEMAPHORE_HPP
#define BINARY_SEMAPHORE_HPP

#include <condition_variable>
#include <mutex>

// Simple binary semaphore compatible with environments lacking
// std::binary_semaphore (pre-C++20 or limited toolchains).
class binary_semaphore_compat {
public:
  explicit binary_semaphore_compat(int initial = 0) : count(initial) {}

  void release() {
    std::lock_guard<std::mutex> lock(m);
    if (count == 0) {
      count = 1;
      cv.notify_one();
    }
  }

  void acquire() {
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return count > 0; });
    count = 0;
  }

private:
  std::mutex m;
  std::condition_variable cv;
  int count;
};

#endif
