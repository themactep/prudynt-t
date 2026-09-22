#ifndef MsgChannel_hpp
#define MsgChannel_hpp

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include "stream/nalu_pool.hpp"

/* Implementation of the MsgChannel API, except that it keeps
 * the most recent bsize elements in the queue.
 */
template <class T> class MsgChannel {
public:
  // bsize:    element-count bound (buffer_size == 0 means unbounded count).
  // maxBytes: byte bound, 0 disables it.  A byte bound evicts whole frames
  //           like the count bound, but stops before removing the newest
  //           frame so one oversized IDR is still delivered.
  MsgChannel(unsigned int bsize, size_t maxBytes = 0)
      : buffer_size{bsize}, max_bytes{maxBytes} {
  }

  // SFINAE helper: detect frame-boundary fields at compile time.
  template <typename U, typename = void>
  struct has_frame_markers : std::false_type {};
  template <typename U>
  struct has_frame_markers<U, std::void_t<decltype(std::declval<U&>().is_frame_start),
                                          decltype(std::declval<U&>().is_frame_end)>>
      : std::true_type {};

  // Detect a std::vector<uint8_t> data member so its buffer can be recycled
  // into a NaluPool instead of being freed.
  template <typename U, typename = void>
  struct has_recyclable_data : std::false_type {};
  template <typename U>
  struct has_recyclable_data<
      U, std::void_t<decltype(std::declval<U&>().data),
                     std::enable_if_t<std::is_same_v<
                         std::remove_reference_t<decltype(std::declval<U&>().data)>,
                         std::vector<uint8_t>>>>> : std::true_type {};

  // Detect an element that reports its own payload size (H264NALUnit does).
  template <typename U, typename = void>
  struct has_size_method : std::false_type {};
  template <typename U>
  struct has_size_method<U,
                         std::void_t<decltype(std::declval<const U&>().size())>>
      : std::true_type {};

  void setPool(std::shared_ptr<NaluPool> p) {
    pool = std::move(p);
  }
  std::shared_ptr<NaluPool> getPool() const {
    return pool;
  }

  // Byte weight of an element, used by the byte bound.  Types carrying a
  // recyclable data vector report its size; the rest fall back to sizeof(T),
  // which only matters when a byte bound was requested for them.
  static size_t dataBytes(const T &elem) {
    if constexpr (has_size_method<T>::value)
      return elem.size();
    else if constexpr (has_recyclable_data<T>::value)
      return elem.data.size();
    else
      return sizeof(T);
  }

  // Write with frame-aware eviction (for types with is_frame_start/is_frame_end).
  // When buffer_size is exceeded, drop whole frames instead of individual NAL
  // units.  Dropping a mid-frame NAL corrupts the bitstream and causes decoder
  // glitches / micro-freezes until the next IDR.
  //
  // Returns false when the buffer was full and eviction occurred, true if the
  // write succeeded without eviction.
  bool write(T msg) {
    std::unique_lock<std::mutex> lck(cv_mtx);
    queued_bytes += dataBytes(msg);
    msg_buffer.push_front(std::move(msg));
    if (withinBudget()) {
      write_cv.notify_all();
      return true;
    }
    // Evict oldest complete frame if markers exist; otherwise drop the oldest
    // element.  A byte bound stops before the newest frame goes away, so a
    // single frame larger than the budget is still delivered.
    if constexpr (has_frame_markers<T>::value) {
      while (!withinBudget() && !msg_buffer.empty()) {
        if (max_bytes > 0 && oneFrameLeft())
          break; // keep the newest frame even if it exceeds the budget
        auto &back = msg_buffer.back();
        bool end_marker = back.is_frame_end;
        popBack();
        if (end_marker) break; // evicted one complete frame
        // Continue popping until we reach frame_end that closes this frame.
      }
    } else {
      while (msg_buffer.size() > buffer_size) {
        if (max_bytes > 0 && msg_buffer.size() <= 1)
          break;
        popBack();
      }
    }
    write_cv.notify_all();
    return false;
  }

  bool write_wait(T msg) {
    std::unique_lock<std::mutex> lck(cv_mtx);
    if (buffer_size == 0) {
      queued_bytes += dataBytes(msg);
      msg_buffer.push_front(std::move(msg));
      write_cv.notify_all();
      return true;
    }
    space_cv.wait(lck, [&] { return msg_buffer.size() < buffer_size; });
    queued_bytes += dataBytes(msg);
    msg_buffer.push_front(std::move(msg));
    write_cv.notify_all();
    return true;
  }

  bool read(T *out) {
    std::unique_lock<std::mutex> lck(cv_mtx);
    if (can_read()) {
      *out = msg_buffer.back();
      popBack();
      space_cv.notify_one();
      return true;
    }
    return false;
  }

  // Move the oldest element out instead of copying it.  read() copies the
  // payload and then recycles the original, which makes every frame allocate
  // a fresh heap buffer that the pool never sees; on a small device that churn
  // never gets returned to the OS.  Taking ownership avoids the copy entirely
  // and leaves the caller holding the buffer until it goes out of scope.
  bool read_move(T *out) {
    std::unique_lock<std::mutex> lck(cv_mtx);
    if (can_read()) {
      queued_bytes -= dataBytes(msg_buffer.back());
      *out = std::move(msg_buffer.back());
      msg_buffer.pop_back();
      space_cv.notify_one();
      return true;
    }
    return false;
  }

  T wait_read() {
    std::unique_lock<std::mutex> lck(cv_mtx);
    while (!can_read()) {
      write_cv.wait(lck);
    };
    T val = msg_buffer.back();
    popBack();
    space_cv.notify_one();
    return val;
  }

  // Return whatever is still queued to the pool.  Without this the last
  // backlog is freed to the allocator when the channel dies, so a session that
  // ends with a full queue hands the pool nothing back and every subsequent
  // session allocates from scratch.
  ~MsgChannel() {
    for (auto &elem : msg_buffer)
      recycle(elem);
  }

  void clear() {
    std::unique_lock<std::mutex> lck(cv_mtx);
    for (auto &elem : msg_buffer)
      recycle(elem);
    msg_buffer.clear();
    queued_bytes = 0;
    space_cv.notify_all();
  }

  // Keep small buffers for reuse; let oversized ones go so the memory
  // returns to the allocator instead of pinning in the pool.
  void release(T &elem) {
    recycle(elem);
  }

  size_t size() const {
    std::unique_lock<std::mutex> lck(cv_mtx);
    return msg_buffer.size();
  }

  unsigned int capacity() const {
    return buffer_size;
  }

private:
  bool can_read() {
    return !msg_buffer.empty();
  }

  bool withinBudget() const {
    if (msg_buffer.size() > buffer_size)
      return false;
    return max_bytes == 0 || queued_bytes <= max_bytes;
  }

  // True when only the newest frame is left in the queue.  Frame-aware
  // eviction uses this to stop before discarding the frame just written.
  bool oneFrameLeft() const {
    if constexpr (has_frame_markers<T>::value) {
      if (msg_buffer.size() <= 1)
        return true;
      return msg_buffer.front().frame_id == msg_buffer.back().frame_id;
    } else {
      return msg_buffer.size() <= 1;
    }
  }

  // Remove the oldest element and keep queued_bytes in step.
  void popBack() {
    queued_bytes -= dataBytes(msg_buffer.back());
    recycle(msg_buffer.back());
    msg_buffer.pop_back();
  }

  void recycle(T &elem) {
    if constexpr (has_recyclable_data<T>::value) {
      if (pool && !elem.data.empty())
        pool->returnBuf(std::move(elem.data));
    }
  }

  std::deque<T> msg_buffer;
  std::shared_ptr<NaluPool> pool;
  mutable std::mutex cv_mtx;
  std::condition_variable write_cv;
  std::condition_variable space_cv;
  unsigned int buffer_size;
  size_t max_bytes{0};
  size_t queued_bytes{0};
};

#endif
