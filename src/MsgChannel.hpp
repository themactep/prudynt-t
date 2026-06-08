#ifndef MsgChannel_hpp
#define MsgChannel_hpp

#include <atomic>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

/* Bounded FIFO message channel backed by a ring buffer of optional<T>.
 *
 * Unlike the old deque-based implementation, the ring buffer performs
 * zero dynamic allocations on the write() / read() hot paths — slots
 * are lazily constructed via optional::emplace() on first write and
 * reset on read.
 *
 * The write() path drops the oldest element when the buffer is full.
 * write_wait() blocks until space is available.
 * read() / wait_read() consume from the oldest element.
 */
template <class T> class MsgChannel {
public:
  explicit MsgChannel(unsigned int bsize) : buffer_size{bsize} {
    if (buffer_size > 0) {
      msg_buffer_.resize(buffer_size);
      // All slots are default-constructed std::nullopt — no T is
      // constructed until the first write() to that slot.
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
      msg_buffer_[tail_].emplace(std::move(msg));
      tail_ = (tail_ + 1) % buffer_size;
      ++count_;
      write_cv.notify_all();
      return true;
    }

    if (buffer_size == 0) {
      return false;
    }

    // Full — overwrite the oldest element (advance head too)
    msg_buffer_[tail_].emplace(std::move(msg));
    tail_ = (tail_ + 1) % buffer_size;
    head_ = tail_;
    write_cv.notify_all();
    return false;
  }

  /* Write |msg|, blocking until space is available (never drops). */
  bool write_wait(T msg) {
    std::unique_lock<std::mutex> lck(cv_mtx);
    if (buffer_size == 0) {
      msg_buffer_deque_.push_front(std::move(msg));
      write_cv.notify_all();
      return true;
    }
    space_cv.wait(lck, [&] { return count_ < buffer_size; });
    msg_buffer_[tail_].emplace(std::move(msg));
    tail_ = (tail_ + 1) % buffer_size;
    ++count_;
    write_cv.notify_all();
    return true;
  }

  /* Try to read into |*out|.  Returns true on success. */
  bool read(T *out) {
    std::unique_lock<std::mutex> lck(cv_mtx);
    if (count_ > 0) {
      *out = std::move(*msg_buffer_[head_]);
      msg_buffer_[head_].reset();
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
    T val = std::move(*msg_buffer_[head_]);
    msg_buffer_[head_].reset();
    head_ = (head_ + 1) % buffer_size;
    --count_;
    space_cv.notify_one();
    return val;
  }

  void clear() {
    std::unique_lock<std::mutex> lck(cv_mtx);
    for (auto &slot : msg_buffer_) {
      slot.reset();
    }
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
  std::vector<std::optional<T>> msg_buffer_;
  mutable std::mutex cv_mtx;
  std::condition_variable write_cv;
  std::condition_variable space_cv;
  unsigned int buffer_size;
  unsigned int head_ = 0;
  unsigned int tail_ = 0;
  unsigned int count_ = 0;

  // Fallback for the buffer_size == 0 degenerate case in write_wait
  std::deque<T> msg_buffer_deque_;
};

#endif
