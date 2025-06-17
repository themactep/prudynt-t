#include "ZeroCopyBuffer.hpp"
#include <algorithm>
#include <cstdlib>

std::atomic<uint32_t> ZeroCopyBuffer::next_buffer_id_{1};

ZeroCopyBuffer::ZeroCopyBuffer(uint8_t* data, size_t size, bool owns_memory)
    : data_(data), size_(size), owns_memory_(owns_memory), buffer_id_(next_buffer_id_++) {
    LOG_DEBUG("Created ZeroCopyBuffer " << buffer_id_ << " size=" << size << " owns=" << owns_memory);
}

ZeroCopyBuffer::~ZeroCopyBuffer() {
    if (owns_memory_ && data_) {
        std::free(data_);
        LOG_DEBUG("Freed ZeroCopyBuffer " << buffer_id_ << " size=" << size_);
    }
}

std::shared_ptr<ZeroCopyBuffer> ZeroCopyBuffer::create(uint8_t* data, size_t size, bool take_ownership) {
    if (!data || size == 0) {
        LOG_ERROR("Invalid parameters for ZeroCopyBuffer::create");
        return nullptr;
    }
    
    return std::shared_ptr<ZeroCopyBuffer>(new ZeroCopyBuffer(data, size, take_ownership));
}

std::shared_ptr<ZeroCopyBuffer> ZeroCopyBuffer::create(size_t size) {
    if (size == 0) {
        LOG_ERROR("Cannot create zero-size buffer");
        return nullptr;
    }
    
    // Use aligned allocation for better performance
    uint8_t* data = static_cast<uint8_t*>(std::aligned_alloc(32, size));
    if (!data) {
        LOG_ERROR("Failed to allocate " << size << " bytes for ZeroCopyBuffer");
        return nullptr;
    }
    
    return std::shared_ptr<ZeroCopyBuffer>(new ZeroCopyBuffer(data, size, true));
}

std::shared_ptr<ZeroCopyBuffer> ZeroCopyBuffer::createFromEncoder(uint8_t* encoder_data, size_t size, size_t offset) {
    if (!encoder_data || size <= offset) {
        LOG_ERROR("Invalid encoder data parameters");
        return nullptr;
    }
    
    // Create buffer that points directly to encoder data (after offset)
    // Note: This assumes encoder data remains valid during buffer lifetime
    uint8_t* data_ptr = encoder_data + offset;
    size_t data_size = size - offset;
    
    return std::shared_ptr<ZeroCopyBuffer>(new ZeroCopyBuffer(data_ptr, data_size, false));
}

std::shared_ptr<ZeroCopyBuffer> ZeroCopyBuffer::slice(size_t offset, size_t length) const {
    if (offset >= size_ || offset + length > size_) {
        LOG_ERROR("Invalid slice parameters: offset=" << offset << " length=" << length << " size=" << size_);
        return nullptr;
    }
    
    auto sliced = std::shared_ptr<ZeroCopyBuffer>(new ZeroCopyBuffer(data_ + offset, length, false));
    sliced->parent_buffer_ = std::const_pointer_cast<ZeroCopyBuffer>(shared_from_this());
    
    return sliced;
}

void ZeroCopyBuffer::copyTo(uint8_t* dest, size_t dest_size) const {
    if (!dest || dest_size == 0) {
        LOG_ERROR("Invalid destination for copyTo");
        return;
    }
    
    size_t copy_size = std::min(size_, dest_size);
    std::memcpy(dest, data_, copy_size);
    
    if (copy_size < size_) {
        LOG_WARN("Buffer truncated in copyTo: " << copy_size << "/" << size_ << " bytes copied");
    }
}

void ZeroCopyNALUnit::analyzeNALType() {
    if (!buffer || buffer->size() == 0) {
        return;
    }
    
    const uint8_t* data = buffer->data();
    nal_type = ZeroCopyUtils::analyzeNALType(data, buffer->size());
    is_keyframe = ZeroCopyUtils::isKeyframe(nal_type);
}

ZeroCopyNALUnit ZeroCopyNALUnit::fromLegacy(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return ZeroCopyNALUnit();
    }
    
    // Create new buffer and copy data
    auto buffer = ZeroCopyBuffer::create(data.size());
    if (!buffer) {
        return ZeroCopyNALUnit();
    }
    
    std::memcpy(buffer->mutableData(), data.data(), data.size());
    return ZeroCopyNALUnit(buffer);
}

std::shared_ptr<ZeroCopyBuffer> ZeroCopyBufferPool::getBuffer(size_t size) {
    if (size > MAX_BUFFER_SIZE) {
        LOG_WARN("Requested buffer size " << size << " exceeds maximum " << MAX_BUFFER_SIZE);
        return ZeroCopyBuffer::create(size);
    }
    
    std::lock_guard<std::mutex> global_lock(global_mutex_);
    auto& pool_entry = buffer_pools_[size];
    
    std::lock_guard<std::mutex> pool_lock(pool_entry.mutex);
    
    if (!pool_entry.buffers.empty()) {
        auto buffer = pool_entry.buffers.back();
        pool_entry.buffers.pop_back();
        reuse_count_++;
        
        LOG_DEBUG("Reused buffer from pool, size=" << size << " remaining=" << pool_entry.buffers.size());
        return buffer;
    }
    
    // No buffer available, create new one
    auto buffer = ZeroCopyBuffer::create(size);
    if (buffer) {
        allocation_count_++;
        total_buffers_++;
    }
    
    LOG_DEBUG("Created new buffer, size=" << size);
    return buffer;
}

void ZeroCopyBufferPool::returnBuffer(std::shared_ptr<ZeroCopyBuffer> buffer) {
    if (!buffer || !buffer->isValid()) {
        return;
    }
    
    size_t size = buffer->size();
    if (size > MAX_BUFFER_SIZE) {
        return; // Don't pool very large buffers
    }
    
    // Only pool buffers with single reference (about to be destroyed)
    if (buffer.use_count() > 1) {
        return;
    }
    
    std::lock_guard<std::mutex> global_lock(global_mutex_);
    auto& pool_entry = buffer_pools_[size];
    
    std::lock_guard<std::mutex> pool_lock(pool_entry.mutex);
    
    if (pool_entry.buffers.size() < MAX_POOL_SIZE_PER_BUCKET) {
        pool_entry.buffers.push_back(buffer);
        LOG_DEBUG("Returned buffer to pool, size=" << size << " pool_size=" << pool_entry.buffers.size());
    }
}

ZeroCopyBufferPool::PoolStats ZeroCopyBufferPool::getStats() const {
    std::lock_guard<std::mutex> lock(global_mutex_);
    
    PoolStats stats;
    stats.total_buffers = total_buffers_.load();
    stats.reuse_count = reuse_count_.load();
    stats.allocation_count = allocation_count_.load();
    stats.available_buffers = 0;
    stats.allocated_bytes = 0;
    
    for (const auto& [size, pool_entry] : buffer_pools_) {
        std::lock_guard<std::mutex> pool_lock(pool_entry.mutex);
        stats.available_buffers += pool_entry.buffers.size();
        stats.allocated_bytes += size * pool_entry.buffers.size();
    }
    
    return stats;
}

void ZeroCopyBufferPool::cleanup() {
    std::lock_guard<std::mutex> lock(global_mutex_);
    
    size_t cleaned_buffers = 0;
    for (auto& [size, pool_entry] : buffer_pools_) {
        std::lock_guard<std::mutex> pool_lock(pool_entry.mutex);
        
        // Keep only half of the buffers in each pool
        size_t keep_count = pool_entry.buffers.size() / 2;
        if (pool_entry.buffers.size() > keep_count) {
            cleaned_buffers += pool_entry.buffers.size() - keep_count;
            pool_entry.buffers.resize(keep_count);
        }
    }
    
    if (cleaned_buffers > 0) {
        LOG_INFO("Cleaned up " << cleaned_buffers << " unused buffers from pool");
    }
}

// ZeroCopyUtils implementation
namespace ZeroCopyUtils {

ZeroCopyNALUnit convertLegacyNALUnit(const std::vector<uint8_t>& data) {
    return ZeroCopyNALUnit::fromLegacy(data);
}

ZeroCopyNALUnit createFromEncoderData(uint8_t* encoder_data, size_t size, size_t offset) {
    auto buffer = ZeroCopyBuffer::createFromEncoder(encoder_data, size, offset);
    if (!buffer) {
        return ZeroCopyNALUnit();
    }
    
    ZeroCopyNALUnit nal_unit(buffer);
    gettimeofday(&nal_unit.timestamp, nullptr);
    return nal_unit;
}

uint8_t analyzeNALType(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        return 0;
    }
    
    // H.264 NAL unit type is in the lower 5 bits of the first byte
    // H.265 NAL unit type is in bits 1-6 of the first byte
    
    // For now, assume H.264 (most common)
    return data[0] & 0x1F;
}

bool isKeyframe(uint8_t nal_type, bool is_h265) {
    if (is_h265) {
        // H.265 keyframe types
        return (nal_type >= 16 && nal_type <= 23); // IDR and CRA frames
    } else {
        // H.264 keyframe types
        return (nal_type == 5); // IDR frame
    }
}

}
