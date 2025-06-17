#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <chrono>
#include "Logger.hpp"

#define MODULE "BUFFERPOOL"

/**
 * Dynamic Buffer Pool Manager for Prudynt-T
 * 
 * Features:
 * - Dynamic buffer allocation based on memory pressure
 * - Shared buffer pools between streams
 * - Memory usage monitoring
 * - Automatic buffer count adjustment
 * - Low-memory device optimization
 */

struct BufferStats {
    size_t total_allocated;
    size_t total_used;
    size_t peak_usage;
    size_t allocation_count;
    size_t deallocation_count;
    std::chrono::steady_clock::time_point last_update;
};

struct MemoryInfo {
    size_t total_memory;
    size_t available_memory;
    size_t used_memory;
    float memory_pressure;  // 0.0 to 1.0
};

class BufferPool {
public:
    static BufferPool& getInstance() {
        static BufferPool instance;
        return instance;
    }

    // Initialize buffer pool with system memory info
    bool initialize();
    
    // Allocate buffer for specific stream
    void* allocateBuffer(const std::string& stream_name, size_t size);
    
    // Release buffer back to pool
    void releaseBuffer(const std::string& stream_name, void* buffer);
    
    // Get optimal buffer count for stream based on current conditions
    int getOptimalBufferCount(const std::string& stream_name, int requested_count);
    
    // Monitor memory pressure and adjust allocations
    void updateMemoryPressure();
    
    // Get buffer statistics
    BufferStats getStats(const std::string& stream_name = "");
    
    // Get current memory information
    MemoryInfo getMemoryInfo();
    
    // Cleanup and shutdown
    void shutdown();

private:
    BufferPool() = default;
    ~BufferPool() { shutdown(); }
    
    // Prevent copying
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    struct BufferBlock {
        void* data;
        size_t size;
        bool in_use;
        std::chrono::steady_clock::time_point allocated_time;
        std::chrono::steady_clock::time_point last_used;
    };

    struct StreamPool {
        std::vector<std::unique_ptr<BufferBlock>> buffers;
        size_t max_buffers;
        size_t buffer_size;
        BufferStats stats;
        std::mutex mutex;
    };

    // Read system memory information
    bool readMemoryInfo();
    
    // Calculate optimal buffer count based on memory pressure
    int calculateOptimalBufferCount(const std::string& stream_name, int requested);
    
    // Cleanup unused buffers
    void cleanupUnusedBuffers();
    
    // Check if we're on a low-memory device
    bool isLowMemoryDevice() const;

    std::unordered_map<std::string, std::unique_ptr<StreamPool>> stream_pools_;
    std::mutex global_mutex_;
    
    MemoryInfo memory_info_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> low_memory_mode_{false};
    
    // Configuration
    static constexpr size_t MIN_BUFFERS = 1;
    static constexpr size_t MAX_BUFFERS = 8;
    static constexpr size_t LOW_MEMORY_THRESHOLD = 64 * 1024 * 1024; // 64MB
    static constexpr float HIGH_PRESSURE_THRESHOLD = 0.85f;
    static constexpr float LOW_PRESSURE_THRESHOLD = 0.60f;
    
    // Buffer cleanup interval
    static constexpr auto CLEANUP_INTERVAL = std::chrono::minutes(5);
    std::chrono::steady_clock::time_point last_cleanup_;
};

/**
 * RAII Buffer wrapper for automatic cleanup
 */
class ManagedBuffer {
public:
    ManagedBuffer(const std::string& stream_name, size_t size)
        : stream_name_(stream_name), buffer_(nullptr) {
        buffer_ = BufferPool::getInstance().allocateBuffer(stream_name, size);
    }
    
    ~ManagedBuffer() {
        if (buffer_) {
            BufferPool::getInstance().releaseBuffer(stream_name_, buffer_);
        }
    }
    
    // Move constructor
    ManagedBuffer(ManagedBuffer&& other) noexcept
        : stream_name_(std::move(other.stream_name_)), buffer_(other.buffer_) {
        other.buffer_ = nullptr;
    }
    
    // Move assignment
    ManagedBuffer& operator=(ManagedBuffer&& other) noexcept {
        if (this != &other) {
            if (buffer_) {
                BufferPool::getInstance().releaseBuffer(stream_name_, buffer_);
            }
            stream_name_ = std::move(other.stream_name_);
            buffer_ = other.buffer_;
            other.buffer_ = nullptr;
        }
        return *this;
    }
    
    // Prevent copying
    ManagedBuffer(const ManagedBuffer&) = delete;
    ManagedBuffer& operator=(const ManagedBuffer&) = delete;
    
    void* get() const { return buffer_; }
    bool valid() const { return buffer_ != nullptr; }

private:
    std::string stream_name_;
    void* buffer_;
};

// Helper macros for easy buffer management
#define ALLOCATE_MANAGED_BUFFER(stream_name, size) \
    ManagedBuffer(stream_name, size)

#define GET_OPTIMAL_BUFFER_COUNT(stream_name, requested) \
    BufferPool::getInstance().getOptimalBufferCount(stream_name, requested)
