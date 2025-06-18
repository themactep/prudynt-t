#pragma once

#include <memory>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <deque>
#include <condition_variable>
#include "Logger.hpp"
#include "BufferPool.hpp"

#undef MODULE
#define MODULE "ZEROCOPY"

/**
 * Zero-Copy Buffer System for Prudynt-T
 * 
 * Features:
 * - Reference-counted buffers to eliminate unnecessary copies
 * - Memory pool integration for efficient buffer reuse
 * - Direct pointer access for high-performance streaming
 * - RAII buffer management with automatic cleanup
 * - Thread-safe reference counting
 * - Integration with existing BufferPool system
 */

class ZeroCopyBuffer : public std::enable_shared_from_this<ZeroCopyBuffer> {
public:
    // Create buffer from existing data (takes ownership)
    static std::shared_ptr<ZeroCopyBuffer> create(uint8_t* data, size_t size, bool take_ownership = true);
    
    // Create buffer with allocated memory
    static std::shared_ptr<ZeroCopyBuffer> create(size_t size);
    
    // Create buffer from encoder stream data (zero-copy from IMP)
    static std::shared_ptr<ZeroCopyBuffer> createFromEncoder(uint8_t* encoder_data, size_t size, size_t offset = 4);
    
    // Get raw data pointer (read-only)
    const uint8_t* data() const {
        // Track buffer access for memory analysis
        const_cast<ZeroCopyBuffer*>(this)->trackAccess();
        return data_;
    }

    // Get mutable data pointer (use with caution)
    uint8_t* mutableData() {
        trackAccess();
        return data_;
    }
    
    // Get buffer size
    size_t size() const { return size_; }
    
    // Get buffer ID for debugging/monitoring
    uint32_t getId() const { return buffer_id_; }
    
    // Create a view/slice of this buffer (shares same memory)
    std::shared_ptr<ZeroCopyBuffer> slice(size_t offset, size_t length) const;
    
    // Copy data to another buffer (only when necessary)
    void copyTo(uint8_t* dest, size_t dest_size) const;
    
    // Check if buffer is valid
    bool isValid() const { return data_ != nullptr && size_ > 0; }

public:
    ~ZeroCopyBuffer();

private:
    ZeroCopyBuffer(uint8_t* data, size_t size, bool owns_memory);

    // Internal method to track buffer access
    void trackAccess();
    
    // Prevent copying (use shared_ptr instead)
    ZeroCopyBuffer(const ZeroCopyBuffer&) = delete;
    ZeroCopyBuffer& operator=(const ZeroCopyBuffer&) = delete;

    uint8_t* data_;
    size_t size_;
    bool owns_memory_;
    uint32_t buffer_id_;
    
    // For sliced buffers
    std::shared_ptr<ZeroCopyBuffer> parent_buffer_;
    
    static std::atomic<uint32_t> next_buffer_id_;
};

/**
 * Zero-Copy NAL Unit - replaces H264NALUnit with zero-copy semantics
 */
struct ZeroCopyNALUnit {
    std::shared_ptr<ZeroCopyBuffer> buffer;
    struct timeval timestamp;
    uint8_t nal_type;
    bool is_keyframe;
    
    ZeroCopyNALUnit() : nal_type(0), is_keyframe(false) {
        timestamp.tv_sec = 0;
        timestamp.tv_usec = 0;
    }
    
    ZeroCopyNALUnit(std::shared_ptr<ZeroCopyBuffer> buf) 
        : buffer(std::move(buf)), nal_type(0), is_keyframe(false) {
        timestamp.tv_sec = 0;
        timestamp.tv_usec = 0;
        if (buffer && buffer->size() > 0) {
            analyzeNALType();
        }
    }
    
    // Get data pointer
    const uint8_t* data() const {
        return buffer ? buffer->data() : nullptr;
    }
    
    // Get data size
    size_t size() const {
        return buffer ? buffer->size() : 0;
    }
    
    // Check if valid
    bool isValid() const {
        return buffer && buffer->isValid();
    }
    
    // Analyze NAL type from data
    void analyzeNALType();
    
    // Create from legacy H264NALUnit (for compatibility)
    static ZeroCopyNALUnit fromLegacy(const std::vector<uint8_t>& data);
};

/**
 * Zero-Copy Message Channel - optimized version of MsgChannel
 */
template <typename T>
class ZeroCopyMsgChannel {
public:
    ZeroCopyMsgChannel(unsigned int buffer_size) : buffer_size_(buffer_size) {}
    
    bool write(T&& msg) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Move the message to avoid copying
        msg_buffer_.emplace_front(std::move(msg));
        
        if (msg_buffer_.size() > buffer_size_) {
            msg_buffer_.pop_back();
            return false;
        }
        
        condition_.notify_all();
        return true;
    }
    
    bool read(T* out) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (msg_buffer_.empty()) {
            return false;
        }
        
        // Move to avoid copying
        *out = std::move(msg_buffer_.back());
        msg_buffer_.pop_back();
        return true;
    }
    
    T waitRead() {
        std::unique_lock<std::mutex> lock(mutex_);
        
        while (msg_buffer_.empty()) {
            condition_.wait(lock);
        }
        
        // Move to avoid copying
        T result = std::move(msg_buffer_.back());
        msg_buffer_.pop_back();
        return result;
    }
    
    size_t size() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return msg_buffer_.size();
    }
    
    bool empty() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return msg_buffer_.empty();
    }

private:
    std::deque<T> msg_buffer_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    unsigned int buffer_size_;
};

/**
 * Zero-Copy Buffer Pool Manager
 */
class ZeroCopyBufferPool {
public:
    static ZeroCopyBufferPool& getInstance() {
        static ZeroCopyBufferPool instance;
        return instance;
    }
    
    // Get buffer from pool or create new one
    std::shared_ptr<ZeroCopyBuffer> getBuffer(size_t size);
    
    // Return buffer to pool for reuse
    void returnBuffer(std::shared_ptr<ZeroCopyBuffer> buffer);
    
    // Get pool statistics
    struct PoolStats {
        size_t total_buffers;
        size_t available_buffers;
        size_t allocated_bytes;
        size_t reuse_count;
        size_t allocation_count;
    };
    
    PoolStats getStats() const;
    
    // Cleanup unused buffers
    void cleanup();

private:
    ZeroCopyBufferPool() = default;
    ~ZeroCopyBufferPool() = default;
    
    // Prevent copying
    ZeroCopyBufferPool(const ZeroCopyBufferPool&) = delete;
    ZeroCopyBufferPool& operator=(const ZeroCopyBufferPool&) = delete;
    
    struct BufferPoolEntry {
        std::vector<std::shared_ptr<ZeroCopyBuffer>> buffers;
        mutable std::mutex mutex;
    };
    
    // Pool organized by buffer size
    std::unordered_map<size_t, BufferPoolEntry> buffer_pools_;
    mutable std::mutex global_mutex_;
    
    // Statistics
    mutable std::atomic<size_t> total_buffers_{0};
    mutable std::atomic<size_t> reuse_count_{0};
    mutable std::atomic<size_t> allocation_count_{0};
    
    // Configuration
    static constexpr size_t MAX_POOL_SIZE_PER_BUCKET = 10;
    static constexpr size_t MAX_BUFFER_SIZE = 2 * 1024 * 1024; // 2MB
};

// Utility functions
namespace ZeroCopyUtils {
    // Convert legacy H264NALUnit to ZeroCopyNALUnit
    ZeroCopyNALUnit convertLegacyNALUnit(const std::vector<uint8_t>& data);
    
    // Create NAL unit from encoder data
    ZeroCopyNALUnit createFromEncoderData(uint8_t* encoder_data, size_t size, size_t offset = 4);
    
    // Analyze NAL unit type
    uint8_t analyzeNALType(const uint8_t* data, size_t size);
    
    // Check if NAL unit is keyframe
    bool isKeyframe(uint8_t nal_type, bool is_h265 = false);
}
