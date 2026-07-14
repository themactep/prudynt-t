#ifndef MsgChannel_hpp
#define MsgChannel_hpp

#include <atomic>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <type_traits>

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
        msg_buffer.pop_back();
        if (end_marker) break; // evicted one complete frame
        // Continue popping until we reach frame_end that closes this frame.
      }
    } else {
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
    msg_buffer.pop_back();
    space_cv.notify_one();
    return val;
  }

  void clear() {
    std::unique_lock<std::mutex> lck(cv_mtx);
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

  std::deque<T> msg_buffer;
  mutable std::mutex cv_mtx;
  std::condition_variable write_cv;
  std::condition_variable space_cv;
  unsigned int buffer_size;
};

#endif
