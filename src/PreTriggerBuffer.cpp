#include "PreTriggerBuffer.hpp"
#include "Logger.hpp"
#include <algorithm>

#undef MODULE
#define MODULE "PreTriggerBuffer"

PreTriggerBuffer::PreTriggerBuffer() 
    : write_index_(0), capacity_(0), max_memory_bytes_(0), 
      keyframe_only_(false), enabled_(false), memory_usage_(0), peak_memory_usage_(0) {
}

PreTriggerBuffer::~PreTriggerBuffer() {
    clear();
}

bool PreTriggerBuffer::init(int duration_seconds, int fps, int max_memory_mb, bool keyframe_only) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    if (duration_seconds <= 0 || fps <= 0 || max_memory_mb <= 0) {
        enabled_.store(false);
        return false;
    }
    
    // Calculate capacity: duration * fps, reduced for keyframe-only mode
    capacity_ = duration_seconds * fps;
    if (keyframe_only) {
        capacity_ = std::max(1, (int)(capacity_ / 8)); // Assume ~1 keyframe per 8 frames
    }
    
    max_memory_bytes_ = max_memory_mb * 1024 * 1024;
    keyframe_only_ = keyframe_only;
    
    // Pre-allocate buffer
    frames_.clear();
    frames_.resize(capacity_);
    write_index_ = 0;
    memory_usage_.store(0);
    enabled_.store(true);
    
    LOG_INFO("PreTriggerBuffer initialized: " << capacity_ << " frames, " 
             << max_memory_mb << "MB limit, keyframe_only=" << keyframe_only);
    
    return true;
}

void PreTriggerBuffer::addFrame(const uint8_t* data, size_t size, int64_t timestamp, bool is_keyframe) {
    if (!enabled_.load() || !data || size == 0) {
        return;
    }
    
    // Skip non-keyframes in keyframe-only mode
    if (keyframe_only_ && !is_keyframe) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    if (frames_.empty()) {
        return;
    }
    
    // Get current slot
    PreTriggerFrame& frame = frames_[write_index_];
    
    // Update memory usage (subtract old frame size)
    if (!frame.data.empty()) {
        memory_usage_.fetch_sub(frame.data.size());
    }
    
    // Store new frame
    frame.data.assign(data, data + size);
    frame.timestamp_us = timestamp;
    frame.is_keyframe = is_keyframe;
    frame.frame_size = size;
    
    // Update memory usage (add new frame size)
    memory_usage_.fetch_add(size);
    
    // Track peak memory usage
    size_t current_usage = memory_usage_.load();
    size_t peak = peak_memory_usage_.load();
    while (current_usage > peak && !peak_memory_usage_.compare_exchange_weak(peak, current_usage)) {
        // Retry if another thread updated peak_memory_usage_
    }
    
    // Advance write index (circular)
    write_index_ = (write_index_ + 1) % capacity_;
    
    // Enforce memory limit
    enforceMemoryLimit();
}

std::vector<PreTriggerFrame> PreTriggerBuffer::getFrames() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    std::vector<PreTriggerFrame> result;
    if (!enabled_.load() || frames_.empty()) {
        return result;
    }
    
    // Collect frames in chronological order (oldest first)
    size_t read_index = write_index_;
    for (size_t i = 0; i < capacity_; ++i) {
        const PreTriggerFrame& frame = frames_[read_index];
        if (!frame.data.empty()) {
            result.push_back(frame);
        }
        read_index = (read_index + 1) % capacity_;
    }
    
    // Sort by timestamp to ensure correct order
    std::sort(result.begin(), result.end(), 
              [](const PreTriggerFrame& a, const PreTriggerFrame& b) {
                  return a.timestamp_us < b.timestamp_us;
              });
    
    LOG_DEBUG("Retrieved " << result.size() << " prebuffer frames");
    return result;
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
    
    LOG_WARN("PreTriggerBuffer memory limit exceeded: " << current_usage 
             << " bytes > " << max_memory_bytes_ << " bytes");
    
    // If memory usage is significantly over limit, reduce buffer size
    if (current_usage > max_memory_bytes_ * 1.5) {
        reduceBufferSize();
        return;
    }
    
    // Remove oldest frames until under limit
    size_t removed_count = 0;
    size_t check_index = write_index_;
    
    while (current_usage > max_memory_bytes_ && removed_count < capacity_) {
        PreTriggerFrame& frame = frames_[check_index];
        if (!frame.data.empty()) {
            current_usage -= frame.data.size();
            memory_usage_.fetch_sub(frame.data.size());
            frame.data.clear();
            frame.frame_size = 0;
            removed_count++;
        }
        check_index = (check_index + 1) % capacity_;
    }
    
    if (removed_count > 0) {
        LOG_WARN("Removed " << removed_count << " frames to enforce memory limit");
    }
}

void PreTriggerBuffer::reduceBufferSize() {
    // This method is called with buffer_mutex_ already locked
    
    if (capacity_ <= 1) {
        return; // Cannot reduce further
    }
    
    size_t new_capacity = capacity_ / 2;
    LOG_WARN("PreTriggerBuffer reducing capacity from " << capacity_ << " to " << new_capacity);
    
    // Create new smaller buffer
    std::vector<PreTriggerFrame> new_frames(new_capacity);
    size_t copied = 0;
    size_t read_index = write_index_;
    
    // Copy most recent frames that fit in new capacity
    for (size_t i = 0; i < capacity_ && copied < new_capacity; ++i) {
        const PreTriggerFrame& frame = frames_[read_index];
        if (!frame.data.empty()) {
            new_frames[copied] = frame;
            copied++;
        }
        read_index = (read_index + 1) % capacity_;
    }
    
    // Update buffer state
    frames_ = std::move(new_frames);
    capacity_ = new_capacity;
    write_index_ = copied % capacity_;
    
    // Recalculate memory usage
    size_t new_usage = 0;
    for (const auto& frame : frames_) {
        if (!frame.data.empty()) {
            new_usage += frame.data.size();
        }
    }
    memory_usage_.store(new_usage);
    
    LOG_INFO("PreTriggerBuffer size reduced, new usage: " << new_usage << " bytes");
}