#ifdef PREBUFFER_ENABLED

#include "PreTriggerBuffer.hpp"
#include "Logger.hpp"
#include <algorithm>

#undef MODULE
#define MODULE "PreTriggerBuffer"

PreTriggerBuffer::PreTriggerBuffer()
    : write_index_(0), capacity_(0), max_memory_bytes_(0), duration_us_(0),
      keyframe_only_(false), enabled_(false), memory_usage_(0),
      peak_memory_usage_(0) {
}

PreTriggerBuffer::~PreTriggerBuffer() {
  clear();
}

bool PreTriggerBuffer::init(int duration_seconds, int fps, int max_memory_mb,
                            bool keyframe_only) {
  std::lock_guard<std::mutex> lock(buffer_mutex_);

  if (duration_seconds <= 0 || fps <= 0 || max_memory_mb <= 0) {
    enabled_.store(false);
    return false;
  }

  // Store target duration in microseconds for time-based eviction
  duration_us_ = static_cast<int64_t>(duration_seconds) * 1000000LL;

  // Calculate max capacity: 2x expected frames to handle variable fps
  // This is just a ceiling - actual eviction is time-based
  capacity_ = duration_seconds * fps * 2;
  if (keyframe_only) {
    capacity_ =
        std::max(1, (int)(capacity_ / 8)); // Assume ~1 keyframe per 8 frames
  }

  max_memory_bytes_ = max_memory_mb * 1024 * 1024;
  keyframe_only_ = keyframe_only;

  // Pre-allocate buffer (use vector as dynamic list, not circular)
  frames_.clear();
  frames_.reserve(capacity_);
  write_index_ = 0;
  memory_usage_.store(0);
  enabled_.store(true);

  LOG_INFO("PreTriggerBuffer initialized: "
           << duration_seconds << "s duration, " << max_memory_mb
           << "MB limit, max_capacity=" << capacity_
           << ", keyframe_only=" << keyframe_only);

  return true;
}

void PreTriggerBuffer::addFrame(const uint8_t *data, size_t size,
                                int64_t timestamp_us, bool is_keyframe) {
  if (!enabled_.load() || !data || size == 0) {
    return;
  }

  // Skip non-keyframes in keyframe-only mode
  if (keyframe_only_ && !is_keyframe) {
    return;
  }

  std::lock_guard<std::mutex> lock(buffer_mutex_);

  // Create new frame
  PreTriggerFrame frame;
  frame.data.assign(data, data + size);
  frame.timestamp_us = timestamp_us;
  frame.is_keyframe = is_keyframe;
  frame.frame_size = size;

  // Add to buffer
  frames_.push_back(std::move(frame));

  // Update memory usage
  memory_usage_.fetch_add(size);

  // Track peak memory usage
  size_t current_usage = memory_usage_.load();
  size_t peak = peak_memory_usage_.load();
  while (current_usage > peak &&
         !peak_memory_usage_.compare_exchange_weak(peak, current_usage)) {
    // Retry if another thread updated peak_memory_usage_
  }

  // Enforce time limit (evict frames older than duration_us_)
  enforceTimeLimit(timestamp_us);

  // Enforce memory limit
  enforceMemoryLimit();

  // Enforce capacity limit (safety valve)
  while (frames_.size() > capacity_) {
    if (!frames_.empty()) {
      memory_usage_.fetch_sub(frames_.front().data.size());
      frames_.erase(frames_.begin());
    }
  }
}

std::vector<PreTriggerFrame> PreTriggerBuffer::getFrames() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);

  std::vector<PreTriggerFrame> result;
  if (!enabled_.load() || frames_.empty()) {
    return result;
  }

  // Frames are already in chronological order (oldest first)
  // Just copy them
  result = frames_;

  // Sort by timestamp to ensure correct order (safety check)
  std::sort(result.begin(), result.end(),
            [](const PreTriggerFrame &a, const PreTriggerFrame &b) {
              return a.timestamp_us < b.timestamp_us;
            });

  return result;
}

void PreTriggerBuffer::clearFrames() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);

  // Clear all frames
  frames_.clear();
  write_index_ = 0;
  memory_usage_.store(0);
  // Keep enabled_, capacity_, and duration_us_ unchanged so buffer continues
  // working
}

void PreTriggerBuffer::clear() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);

  frames_.clear();
  write_index_ = 0;
  capacity_ = 0;
  memory_usage_.store(0);
  peak_memory_usage_.store(0);
  enabled_.store(false);
}

size_t PreTriggerBuffer::getMemoryUsage() const {
  return memory_usage_.load();
}

void PreTriggerBuffer::enforceMemoryLimit() {
  // This method is called with buffer_mutex_ already locked

  size_t current_usage = memory_usage_.load();
  if (current_usage <= max_memory_bytes_) {
    return;
  }

  LOG_WARN("PreTriggerBuffer memory limit exceeded: "
           << current_usage << " bytes > " << max_memory_bytes_ << " bytes");

  // Remove oldest frames until under limit
  size_t removed_count = 0;

  while (current_usage > max_memory_bytes_ && !frames_.empty()) {
    current_usage -= frames_.front().data.size();
    memory_usage_.fetch_sub(frames_.front().data.size());
    frames_.erase(frames_.begin());
    removed_count++;
  }

  if (removed_count > 0) {
    LOG_WARN("Removed " << removed_count << " frames to enforce memory limit");
  }
}

void PreTriggerBuffer::enforceTimeLimit(int64_t newest_timestamp_us) {
  // This method is called with buffer_mutex_ already locked

  if (duration_us_ <= 0 || frames_.empty()) {
    return;
  }

  // Calculate cutoff timestamp
  int64_t cutoff_ts = newest_timestamp_us - duration_us_;

  // Remove frames older than cutoff
  while (!frames_.empty() && frames_.front().timestamp_us < cutoff_ts) {
    memory_usage_.fetch_sub(frames_.front().data.size());
    frames_.erase(frames_.begin());
  }
}

void PreTriggerBuffer::reduceBufferSize() {
  // This method is called with buffer_mutex_ already locked
  // With time-based eviction, we just reduce capacity_ for future growth limit

  if (capacity_ <= 1) {
    return; // Cannot reduce further
  }

  size_t new_capacity = capacity_ / 2;
  LOG_WARN("PreTriggerBuffer reducing max capacity from " << capacity_ << " to "
                                                          << new_capacity);
  capacity_ = new_capacity;

  // Trim frames if we have more than new capacity
  while (frames_.size() > capacity_ && !frames_.empty()) {
    memory_usage_.fetch_sub(frames_.front().data.size());
    frames_.erase(frames_.begin());
  }

  LOG_INFO(
      "PreTriggerBuffer capacity reduced, current frames: " << frames_.size());
}

#endif // PREBUFFER_ENABLED