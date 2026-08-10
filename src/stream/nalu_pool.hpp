#ifndef NALU_POOL_HPP
#define NALU_POOL_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// NALU buffer pool --- avoids per-NAL heap allocations in the hot video path.
// Buffers are reused across frames: borrow() returns an empty vector with
// pre-allocated capacity, returnBuf() puts it back for the next borrow().
class NaluPool {
public:
  explicit NaluPool(size_t maxSz = 64) : maxPoolSize(maxSz) {}

  std::vector<uint8_t> borrow(size_t hint = 0) {
    std::lock_guard<std::mutex> lock(mtx);
    if (!pool.empty()) {
      auto v = std::move(pool.back());
      pool.pop_back();
      if (hint > 0 && v.capacity() < hint)
        v.reserve(hint);
      return v;
    }
    std::vector<uint8_t> v;
    if (hint > 0)
      v.reserve(hint);
    return v;
  }

  void returnBuf(std::vector<uint8_t> &&v) {
    std::lock_guard<std::mutex> lock(mtx);
    if (pool.size() < maxPoolSize) {
      v.clear();
      pool.push_back(std::move(v));
    }
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mtx);
    return pool.size();
  }

private:
  mutable std::mutex mtx;
  std::vector<std::vector<uint8_t>> pool;
  size_t maxPoolSize;
};

#endif
