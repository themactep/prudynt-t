#ifndef MsgChannel_hpp
#define MsgChannel_hpp

#include <atomic>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

/* Implementation of the MsgChannel API, except that it keeps
 * the most recent bsize elements in the queue.
 */
template <class T> class MsgChannel {
public:
  MsgChannel(unsigned int bsize) : buffer_size{bsize} {
  }

  bool write(T msg) {
    std::unique_lock<std::mutex> lck(cv_mtx);
    msg_buffer.push_front(std::move(msg));
    if (msg_buffer.size() > buffer_size) {
      msg_buffer.pop_back();
      write_cv.notify_all(); // wake wait_read() callers (e.g. RTSP SPS/PPS init)
      return false;
    }
    write_cv.notify_all();
    return true;
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
