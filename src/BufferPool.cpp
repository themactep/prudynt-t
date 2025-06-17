#include "BufferPool.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <algorithm>

bool BufferPool::initialize() {
    std::lock_guard<std::mutex> lock(global_mutex_);
    
    if (initialized_.load()) {
        return true;
    }
    
    LOG_INFO("Initializing dynamic buffer pool manager");
    
    // Read initial memory information
    if (!readMemoryInfo()) {
        LOG_ERROR("Failed to read system memory information");
        return false;
    }
    
    // Check if we're on a low-memory device
    low_memory_mode_.store(isLowMemoryDevice());
    
    if (low_memory_mode_.load()) {
        LOG_INFO("Low memory device detected (" << memory_info_.total_memory / (1024*1024) 
                 << "MB), enabling conservative buffer allocation");
    }
    
    last_cleanup_ = std::chrono::steady_clock::now();
    initialized_.store(true);
    
    LOG_INFO("Buffer pool initialized successfully");
    return true;
}

bool BufferPool::readMemoryInfo() {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) {
        LOG_ERROR("Cannot open /proc/meminfo");
        return false;
    }
    
    std::string line;
    size_t mem_total = 0, mem_available = 0, mem_free = 0, buffers = 0, cached = 0;
    
    while (std::getline(meminfo, line)) {
        std::istringstream iss(line);
        std::string key;
        size_t value;
        std::string unit;
        
        if (iss >> key >> value >> unit) {
            // Convert kB to bytes
            value *= 1024;
            
            if (key == "MemTotal:") {
                mem_total = value;
            } else if (key == "MemAvailable:") {
                mem_available = value;
            } else if (key == "MemFree:") {
                mem_free = value;
            } else if (key == "Buffers:") {
                buffers = value;
            } else if (key == "Cached:") {
                cached = value;
            }
        }
    }
    
    memory_info_.total_memory = mem_total;
    
    // If MemAvailable is not available (older kernels), calculate it
    if (mem_available == 0) {
        mem_available = mem_free + buffers + cached;
    }
    
    memory_info_.available_memory = mem_available;
    memory_info_.used_memory = mem_total - mem_available;
    memory_info_.memory_pressure = static_cast<float>(memory_info_.used_memory) / mem_total;
    
    LOG_DEBUG("Memory info: Total=" << mem_total/(1024*1024) << "MB, "
              << "Available=" << mem_available/(1024*1024) << "MB, "
              << "Pressure=" << (memory_info_.memory_pressure * 100) << "%");
    
    return true;
}

bool BufferPool::isLowMemoryDevice() const {
    return memory_info_.total_memory <= LOW_MEMORY_THRESHOLD;
}

void* BufferPool::allocateBuffer(const std::string& stream_name, size_t size) {
    if (!initialized_.load()) {
        if (!const_cast<BufferPool*>(this)->initialize()) {
            return nullptr;
        }
    }
    
    std::lock_guard<std::mutex> lock(global_mutex_);
    
    // Get or create stream pool
    auto& pool = stream_pools_[stream_name];
    if (!pool) {
        pool = std::make_unique<StreamPool>();
        pool->buffer_size = size;
        pool->max_buffers = low_memory_mode_.load() ? MIN_BUFFERS : MAX_BUFFERS;
    }
    
    std::lock_guard<std::mutex> pool_lock(pool->mutex);
    
    // Try to find an unused buffer
    for (auto& buffer : pool->buffers) {
        if (!buffer->in_use && buffer->size >= size) {
            buffer->in_use = true;
            buffer->last_used = std::chrono::steady_clock::now();
            pool->stats.total_used++;
            return buffer->data;
        }
    }
    
    // Check if we can allocate a new buffer
    if (pool->buffers.size() >= pool->max_buffers) {
        LOG_WARN("Buffer pool for " << stream_name << " is full, cannot allocate new buffer");
        return nullptr;
    }
    
    // Update memory pressure before allocation
    const_cast<BufferPool*>(this)->updateMemoryPressure();
    
    // Check memory pressure before allocating
    if (memory_info_.memory_pressure > HIGH_PRESSURE_THRESHOLD) {
        LOG_WARN("High memory pressure (" << (memory_info_.memory_pressure * 100) 
                 << "%), refusing buffer allocation for " << stream_name);
        return nullptr;
    }
    
    // Allocate new buffer
    void* data = std::aligned_alloc(32, size); // 32-byte aligned for SIMD
    if (!data) {
        LOG_ERROR("Failed to allocate " << size << " bytes for " << stream_name);
        return nullptr;
    }
    
    auto buffer = std::make_unique<BufferBlock>();
    buffer->data = data;
    buffer->size = size;
    buffer->in_use = true;
    buffer->allocated_time = std::chrono::steady_clock::now();
    buffer->last_used = buffer->allocated_time;
    
    pool->buffers.push_back(std::move(buffer));
    pool->stats.total_allocated += size;
    pool->stats.allocation_count++;
    pool->stats.total_used++;
    
    if (pool->stats.total_used > pool->stats.peak_usage) {
        pool->stats.peak_usage = pool->stats.total_used;
    }
    
    LOG_DEBUG("Allocated " << size << " bytes for " << stream_name 
              << " (pool size: " << pool->buffers.size() << ")");
    
    return data;
}

void BufferPool::releaseBuffer(const std::string& stream_name, void* buffer) {
    if (!buffer || !initialized_.load()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(global_mutex_);
    
    auto pool_it = stream_pools_.find(stream_name);
    if (pool_it == stream_pools_.end()) {
        LOG_ERROR("Stream pool not found for " << stream_name);
        return;
    }
    
    auto& pool = pool_it->second;
    std::lock_guard<std::mutex> pool_lock(pool->mutex);
    
    // Find and release the buffer
    for (auto& buf : pool->buffers) {
        if (buf->data == buffer && buf->in_use) {
            buf->in_use = false;
            buf->last_used = std::chrono::steady_clock::now();
            pool->stats.total_used--;
            pool->stats.deallocation_count++;
            
            LOG_DEBUG("Released buffer for " << stream_name 
                      << " (active: " << pool->stats.total_used << ")");
            return;
        }
    }
    
    LOG_WARN("Buffer not found in pool for " << stream_name);
}

int BufferPool::getOptimalBufferCount(const std::string& stream_name, int requested_count) {
    if (!initialized_.load()) {
        return requested_count;
    }
    
    updateMemoryPressure();
    
    return calculateOptimalBufferCount(stream_name, requested_count);
}

int BufferPool::calculateOptimalBufferCount(const std::string& stream_name, int requested) {
    // Start with requested count
    int optimal = requested;
    
    // Adjust based on memory pressure
    if (memory_info_.memory_pressure > HIGH_PRESSURE_THRESHOLD) {
        optimal = MIN_BUFFERS;
        LOG_DEBUG("High memory pressure, reducing " << stream_name 
                  << " buffers to " << optimal);
    } else if (memory_info_.memory_pressure < LOW_PRESSURE_THRESHOLD && !low_memory_mode_.load()) {
        optimal = std::min(requested + 1, static_cast<int>(MAX_BUFFERS));
        LOG_DEBUG("Low memory pressure, allowing " << stream_name 
                  << " buffers: " << optimal);
    }
    
    // Always respect minimum
    optimal = std::max(optimal, static_cast<int>(MIN_BUFFERS));
    
    return optimal;
}

void BufferPool::updateMemoryPressure() {
    readMemoryInfo();
    
    // Trigger cleanup if needed
    auto now = std::chrono::steady_clock::now();
    if (now - last_cleanup_ > CLEANUP_INTERVAL) {
        cleanupUnusedBuffers();
        last_cleanup_ = now;
    }
}

void BufferPool::cleanupUnusedBuffers() {
    std::lock_guard<std::mutex> lock(global_mutex_);
    
    auto now = std::chrono::steady_clock::now();
    size_t total_freed = 0;
    
    for (auto& [stream_name, pool] : stream_pools_) {
        std::lock_guard<std::mutex> pool_lock(pool->mutex);
        
        auto it = pool->buffers.begin();
        while (it != pool->buffers.end()) {
            auto& buffer = *it;
            
            // Remove buffers unused for more than 5 minutes
            if (!buffer->in_use && 
                (now - buffer->last_used) > std::chrono::minutes(5)) {
                
                std::free(buffer->data);
                pool->stats.total_allocated -= buffer->size;
                total_freed += buffer->size;
                it = pool->buffers.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    if (total_freed > 0) {
        LOG_INFO("Cleaned up " << total_freed << " bytes of unused buffers");
    }
}

BufferStats BufferPool::getStats(const std::string& stream_name) {
    std::lock_guard<std::mutex> lock(global_mutex_);
    
    if (stream_name.empty()) {
        // Return global stats
        BufferStats global_stats = {};
        for (const auto& [name, pool] : stream_pools_) {
            std::lock_guard<std::mutex> pool_lock(pool->mutex);
            global_stats.total_allocated += pool->stats.total_allocated;
            global_stats.total_used += pool->stats.total_used;
            global_stats.peak_usage += pool->stats.peak_usage;
            global_stats.allocation_count += pool->stats.allocation_count;
            global_stats.deallocation_count += pool->stats.deallocation_count;
        }
        global_stats.last_update = std::chrono::steady_clock::now();
        return global_stats;
    }
    
    auto pool_it = stream_pools_.find(stream_name);
    if (pool_it != stream_pools_.end()) {
        std::lock_guard<std::mutex> pool_lock(pool_it->second->mutex);
        auto stats = pool_it->second->stats;
        stats.last_update = std::chrono::steady_clock::now();
        return stats;
    }
    
    return {};
}

MemoryInfo BufferPool::getMemoryInfo() {
    updateMemoryPressure();
    return memory_info_;
}

void BufferPool::shutdown() {
    if (!initialized_.load()) {
        return;
    }
    
    LOG_INFO("Shutting down buffer pool");
    
    std::lock_guard<std::mutex> lock(global_mutex_);
    
    size_t total_freed = 0;
    for (auto& [stream_name, pool] : stream_pools_) {
        std::lock_guard<std::mutex> pool_lock(pool->mutex);
        
        for (auto& buffer : pool->buffers) {
            if (buffer->data) {
                std::free(buffer->data);
                total_freed += buffer->size;
            }
        }
        pool->buffers.clear();
    }
    
    stream_pools_.clear();
    initialized_.store(false);
    
    LOG_INFO("Buffer pool shutdown complete, freed " << total_freed << " bytes");
}
