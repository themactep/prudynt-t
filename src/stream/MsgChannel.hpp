#ifndef MsgChannel_hpp
#define MsgChannel_hpp

#include <atomic>
#include <condition_variable>
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
  MsgChannel(unsigned int bsize) : buffer_size{bsize} {
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

  void setPool(std::shared_ptr<NaluPool> p) {
    pool = std::move(p);
  }
  std::shared_ptr<NaluPool> getPool() const {
    return pool;
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
    msg_buffer.push_front(std::move(msg));
    if (msg_buffer.size() <= buffer_size) {
      write_cv.notify_all();
      return true;
    }
    // Evict oldest complete frame if markers exist; otherwise drop oldest NAL.
    if constexpr (has_frame_markers<T>::value) {
      while (msg_buffer.size() > buffer_size && !msg_buffer.empty()) {
        auto &back = msg_buffer.back();
        bool end_marker = back.is_frame_end;
        recycle(back);
        msg_buffer.pop_back();
        if (end_marker) break; // evicted one complete frame
        // Continue popping until we reach frame_end that closes this frame.
      }
    } else {
      recycle(msg_buffer.back());
      msg_buffer.pop_back();
    }
    write_cv.notify_all();
    return false;
  }

  bool write_wait(T msg) {
    std::unique_lock<std::mutex> lck(cv_mtx);
    if (buffer_size == 0) {
      msg_buffer.push_front(std::move(msg));
      write_cv.notify_all();
      return true;
    }
    space_cv.wait(lck, [&] { return msg_buffer.size() < buffer_size; });
    msg_buffer.push_front(std::move(msg));
    write_cv.notify_all();
    return true;
  }

  bool read(T *out) {
    std::unique_lock<std::mutex> lck(cv_mtx);
    if (can_read()) {
      *out = msg_buffer.back();
      recycle(msg_buffer.back());
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
    recycle(msg_buffer.back());
    msg_buffer.pop_back();
    space_cv.notify_one();
    return val;
  }

  void clear() {
    std::unique_lock<std::mutex> lck(cv_mtx);
    for (auto &elem : msg_buffer)
      recycle(elem);
    msg_buffer.clear();
    space_cv.notify_all();
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
};

#endif
