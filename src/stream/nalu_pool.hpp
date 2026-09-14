#ifndef NALU_POOL_HPP
#define NALU_POOL_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// NALU buffer pool --- avoids per-NAL heap allocations in the hot video path.
// Buffers are reused across frames: borrow() returns an empty vector with
// pre-allocated capacity, returnBuf() puts it back for the next borrow().
//
// Retention is bounded by total capacity rather than by buffer count.  A
// count-only bound lets a handful of IDR-sized buffers pin megabytes: clear()
// keeps the allocation, so the pool's footprint is the sum of the largest NALs
// ever seen.  On a 32 MB device that is enough to exhaust memory, so the pool
// keeps its budget and drops anything that would exceed it.
class NaluPool {
public:
  // maxSz:     buffers kept at most.
  // maxBytes: total capacity kept at most, in bytes.
  //
  // The budget must comfortably hold the working set (the main channel plus
  // one tap per client, several buffers deep at 1080p) or every keyframe
  // allocation misses the pool and lands on the heap.
  explicit NaluPool(size_t maxSz = 64, size_t maxBytes = 4 * 1024 * 1024)
      : maxPoolSize(maxSz), maxPoolBytes(maxBytes) {}

  std::vector<uint8_t> borrow(size_t hint = 0) {
    std::lock_guard<std::mutex> lock(mtx);
    if (!pool.empty()) {
      auto v = std::move(pool.back());
      pool.pop_back();
      size_t cap = v.capacity();
      pooled_bytes = cap < pooled_bytes ? pooled_bytes - cap : 0;
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
    size_t cap = v.capacity();
    std::lock_guard<std::mutex> lock(mtx);
    // Drop anything that would push the pool past its budget so the pool can
    // never pin more than maxPoolBytes.
    if (pool.size() >= maxPoolSize || pooled_bytes + cap > maxPoolBytes)
      return;
    v.clear();
    pooled_bytes += cap;
    pool.push_back(std::move(v));
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mtx);
    return pool.size();
  }

private:
  mutable std::mutex mtx;
  std::vector<std::vector<uint8_t>> pool;
  size_t maxPoolSize;
  size_t maxPoolBytes;
  size_t pooled_bytes{0};
};

#endif
