#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

struct PreTriggerFrame {
    std::vector<uint8_t> data;     // Complete frame (all NAL units)
    int64_t timestamp_us;          // IMP timestamp
    bool is_keyframe;              // IDR frame flag
    size_t frame_size;             // Total bytes
};

class PreTriggerBuffer {
public:
    PreTriggerBuffer();
    ~PreTriggerBuffer();
    
    // Initialize buffer with capacity based on config
    bool init(int duration_seconds, int fps, int max_memory_mb, bool keyframe_only);
    
    // Add frame to circular buffer
    void addFrame(const uint8_t* data, size_t size, int64_t timestamp, bool is_keyframe);
    
    // Get all buffered frames for recording (oldest first)
    std::vector<PreTriggerFrame> getFrames();
    
    // Clear buffer and reset
    void clear();
    
    // Get current memory usage in bytes
    size_t getMemoryUsage() const;
    
    // Check if buffer is enabled
    bool isEnabled() const { return enabled_.load(); }
    
private:
    std::vector<PreTriggerFrame> frames_;
    size_t write_index_;
    size_t capacity_;
    size_t max_memory_bytes_;
    bool keyframe_only_;
    std::atomic<bool> enabled_;
    mutable std::mutex buffer_mutex_;
    // Add memory usage tracking per buffer
    std::atomic<size_t> memory_usage_;
    std::atomic<size_t> peak_memory_usage_;
    
    void enforceMemoryLimit();
    void reduceBufferSize();
};