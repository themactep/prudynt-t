#ifndef MsgChannel_hpp
#define MsgChannel_hpp

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

/* Bounded FIFO message channel backed by a pre-allocated ring buffer.
 *
 * Unlike the old deque-based implementation, the ring buffer performs
 * zero dynamic allocations on the write() / read() hot paths — all
 * storage is allocated once at construction via reserve() and reused
 * via move-assignment.
 *
 * The write() path drops the oldest element when the buffer is full.
 * write_wait() blocks until space is available.
 * read() / wait_read() consume from the oldest element.
 */
template <class T> class MsgChannel {
public:
  explicit MsgChannel(unsigned int bsize) : buffer_size{bsize} {
    if (buffer_size > 0) {
      msg_buffer_.reserve(buffer_size);
      // Fill with default-constructed elements so the ring buffer
      // can move-assign into pre-existing slots.
      for (unsigned int i = 0; i < buffer_size; ++i) {
        msg_buffer_.emplace_back();
      }
    }
  }

  ~MsgChannel() { clear(); }

  // Non-copyable, non-movable
  MsgChannel(const MsgChannel &) = delete;
  MsgChannel &operator=(const MsgChannel &) = delete;

  /* Write |msg| into the buffer.  If the buffer is full the oldest
   * element is dropped.  Returns false when an element was dropped. */
  bool write(T msg) {
    std::unique_lock<std::mutex> lck(cv_mtx);

    if (count_ < buffer_size) {
      // Normal case: room available
      msg_buffer_[tail_] = std::move(msg);
      tail_ = (tail_ + 1) % buffer_size;
      ++count_;
      write_cv.notify_all();
      return true;
    }

    if (buffer_size == 0) {
      // Unbounded not supported with ring buffer (should not happen)
      return false;
    }

    // Full — overwrite the oldest element (advance head too)
    msg_buffer_[tail_] = std::move(msg);
    tail_ = (tail_ + 1) % buffer_size;
    head_ = tail_; // head follows tail when full (oldest slot was just replaced)
    write_cv.notify_all();
    return false;
  }

  /* Write |msg|, blocking until space is available (never drops). */
  bool write_wait(T msg) {
    std::unique_lock<std::mutex> lck(cv_mtx);
    if (buffer_size == 0) {
      // Degenerate case: unbounded — just push (but ring buffer can't grow)
      // Fall back: push into a temporary deque
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

  /* Try to read into |*out|.  Returns true on success. */
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

  /* Blocking read.  Returns the oldest element. */
  T wait_read() {
    std::unique_lock<std::mutex> lck(cv_mtx);
    while (count_ == 0) {
      write_cv.wait(lck);
    };
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
  unsigned int head_ = 0;       // read position (oldest)
  unsigned int tail_ = 0;       // write position (newest)
  unsigned int count_ = 0;      // number of elements in buffer

  // Fallback for the buffer_size == 0 degenerate case in write_wait
  std::deque<T> msg_buffer_deque_;
};

#endif
