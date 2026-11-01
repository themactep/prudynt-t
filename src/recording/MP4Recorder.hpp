#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "recording/MP4Muxer.hpp"

class MP4Recorder {
public:
  MP4Recorder();
  ~MP4Recorder();

  bool start(const std::string &path, const MP4Muxer::InitParams &params);
  void stop();

  void writeVideo(const uint8_t *data, size_t size, int64_t pts_ms, bool isKey);
  void writeAudio(const uint8_t *data, size_t size, int64_t pts_ms);

  bool isActive() const {
    return active_.load(std::memory_order_acquire);
  }

  // True after a write failure ended the segment early (abortUnlocked), until
  // stop() or the next start(). The recorder is no longer active then, but the
  // caller still owns a started segment: its state file, its slot in the
  // active-recorder count, its stop timer.
  bool hasAborted() const {
    return aborted_.load(std::memory_order_acquire);
  }

  // Bumped on every start(). AudioWorker keeps its own per-channel clock and
  // resets it when this changes, because a looped segment restarts faster than
  // one audio frame and the inactive window is never observed.
  uint64_t generation() const {
    return generation_.load(std::memory_order_acquire);
  }

private:
  void closeUnlocked();
  void abortUnlocked();

  mutable std::mutex mutex_;
  std::unique_ptr<MP4Muxer> muxer_;
  std::atomic<uint64_t> generation_{0};
  int fd_ = -1;
  std::atomic<bool> active_{false};
  std::atomic<bool> aborted_{false};
};
