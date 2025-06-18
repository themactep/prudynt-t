#include "ZeroCopyVideoWorker.hpp"
#include "WorkerUtils.hpp"
#include <chrono>

ZeroCopyVideoWorker::ZeroCopyVideoWorker(int encoder_channel)
    : encoder_channel_(encoder_channel), zero_copy_enabled_(true), 
      max_frame_size_(DEFAULT_MAX_FRAME_SIZE), consecutive_errors_(0) {
    
    // Initialize statistics
    stats_ = {};
    stats_.last_update = std::chrono::steady_clock::now();
    last_frame_time_ = stats_.last_update;
    
    LOG_INFO("Created ZeroCopyVideoWorker for channel " << encoder_channel_);
}

ZeroCopyVideoWorker::~ZeroCopyVideoWorker() {
    stop();
    LOG_INFO("Destroyed ZeroCopyVideoWorker for channel " << encoder_channel_);
}

void ZeroCopyVideoWorker::start() {
    if (running_.load()) {
        LOG_WARN("ZeroCopyVideoWorker already running for channel " << encoder_channel_);
        return;
    }
    
    running_.store(true);
    worker_thread_ = std::make_unique<std::thread>(&ZeroCopyVideoWorker::processingLoop, this);
    
    LOG_INFO("Started ZeroCopyVideoWorker for channel " << encoder_channel_);
}

void ZeroCopyVideoWorker::stop() {
    if (!running_.load()) {
        return;
    }
    
    running_.store(false);
    
    if (worker_thread_ && worker_thread_->joinable()) {
        worker_thread_->join();
    }
    worker_thread_.reset();
    
    LOG_INFO("Stopped ZeroCopyVideoWorker for channel " << encoder_channel_);
}

void ZeroCopyVideoWorker::processingLoop() {
    LOG_DEBUG("Zero-copy processing loop started for channel " << encoder_channel_);
    
    while (running_.load()) {
        try {
            if (!shouldProcessFrame()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            
            // Poll for new encoder data
            if (IMP_Encoder_PollingStream(encoder_channel_, cfg->general.imp_polling_timeout) == 0) {
                IMPEncoderStream stream;
                if (IMP_Encoder_GetStream(encoder_channel_, &stream, GET_STREAM_BLOCKING) == 0) {
                    processEncoderStream(stream);
                    
                    // Release the stream back to encoder
                    IMP_Encoder_ReleaseStream(encoder_channel_, &stream);
                    consecutive_errors_ = 0;
                } else {
                    LOG_ERROR("IMP_Encoder_GetStream failed for channel " << encoder_channel_);
                    consecutive_errors_++;
                }
            }
            
            // Check for too many consecutive errors
            if (consecutive_errors_ > MAX_CONSECUTIVE_ERRORS) {
                LOG_ERROR("Too many consecutive errors (" << consecutive_errors_ 
                         << "), stopping zero-copy worker for channel " << encoder_channel_);
                break;
            }
            
        } catch (const std::exception& e) {
            LOG_ERROR("Exception in zero-copy processing loop: " << e.what());
            consecutive_errors_++;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    LOG_DEBUG("Zero-copy processing loop ended for channel " << encoder_channel_);
}

void ZeroCopyVideoWorker::processEncoderStream(const IMPEncoderStream& stream) {
    auto now = std::chrono::steady_clock::now();
    
    for (uint32_t i = 0; i < stream.packCount; i++) {
        // Get encoder data pointer and size
#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
        uint8_t* start = (uint8_t*)stream.virAddr + stream.pack[i].offset;
        size_t length = stream.pack[i].length;
        uint8_t nal_type = stream.pack[i].nalType.h264NalType;
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30)
        uint8_t* start = (uint8_t*)stream.pack[i].virAddr;
        size_t length = stream.pack[i].length;
        uint8_t nal_type = 0; // Need to extract from data
#endif
        
        if (!start || length == 0) {
            LOG_WARN("Invalid encoder data for channel " << encoder_channel_ << " pack " << i);
            continue;
        }
        
        // Check frame size limits
        if (length > max_frame_size_) {
            LOG_WARN("Frame size " << length << " exceeds maximum " << max_frame_size_ 
                     << " for channel " << encoder_channel_);
            updateStats(length, false);
            continue;
        }
        
        // Determine if this is a keyframe
        bool is_keyframe = false;
#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
        is_keyframe = (stream.pack[i].nalType.h264NalType == 7 || 
                      stream.pack[i].nalType.h264NalType == 8 || 
                      stream.pack[i].nalType.h264NalType == 5 ||
                      stream.pack[i].nalType.h265NalType == 32);
#else
        // Extract NAL type from data for older platforms
        if (length > 4) {
            nal_type = ZeroCopyUtils::analyzeNALType(start + 4, length - 4);
            is_keyframe = ZeroCopyUtils::isKeyframe(nal_type);
        }
#endif
        
        // Create zero-copy NAL unit
        ZeroCopyNALUnit nal_unit = createNALUnit(start, length, 4, nal_type, is_keyframe);
        if (!nal_unit.isValid()) {
            LOG_ERROR("Failed to create zero-copy NAL unit for channel " << encoder_channel_);
            updateStats(length, false);
            continue;
        }
        
        // Set timestamp
        nal_unit.timestamp.tv_sec = now.time_since_epoch().count() / 1000000000;
        nal_unit.timestamp.tv_usec = (now.time_since_epoch().count() % 1000000000) / 1000;
        
        // Send to message channel (zero-copy)
        auto zero_copy_stream = ZeroCopyIntegration::getInstance().convertStream(global_video[encoder_channel_].get());
        if (zero_copy_stream && zero_copy_stream->msgChannel) {
            if (zero_copy_stream->msgChannel->write(std::move(nal_unit))) {
                updateStats(length, true);
                
                // Notify callback
                std::unique_lock<std::mutex> lock(zero_copy_stream->onDataCallbackLock);
                if (zero_copy_stream->onDataCallback) {
                    zero_copy_stream->onDataCallback();
                }
            } else {
                LOG_WARN("Zero-copy message channel full for channel " << encoder_channel_);
                updateStats(length, false);
            }
        }
    }
    
    last_frame_time_ = now;
}

ZeroCopyNALUnit ZeroCopyVideoWorker::createNALUnit(uint8_t* encoder_data, size_t size, size_t offset,
                                                   uint8_t nal_type, bool is_keyframe) {
    if (!encoder_data || size <= offset) {
        return ZeroCopyNALUnit();
    }
    
    ZeroCopyNALUnit nal_unit;
    
    if (zero_copy_enabled_) {
        // Try zero-copy approach first
        nal_unit.buffer = ZeroCopyBuffer::createFromEncoder(encoder_data, size, offset);
        if (nal_unit.buffer) {
            nal_unit.nal_type = nal_type;
            nal_unit.is_keyframe = is_keyframe;
            return nal_unit;
        }
    }
    
    // Fallback to copy approach
    nal_unit.buffer = ZeroCopyBuffer::create(size - offset);
    if (nal_unit.buffer) {
        std::memcpy(nal_unit.buffer->mutableData(), encoder_data + offset, size - offset);
        nal_unit.nal_type = nal_type;
        nal_unit.is_keyframe = is_keyframe;
    }
    
    return nal_unit;
}

bool ZeroCopyVideoWorker::shouldProcessFrame() const {
    // Check if we have active clients
    if (!global_video[encoder_channel_]->hasDataCallback.load()) {
        return false;
    }
    
    // Check if we need to process for JPEG
    bool run_for_jpeg = (encoder_channel_ == global_jpeg[0]->streamChn && 
                        global_video[encoder_channel_]->run_for_jpeg);
    
    return global_video[encoder_channel_]->hasDataCallback.load() || run_for_jpeg;
}

void ZeroCopyVideoWorker::updateStats(size_t frame_size, bool was_zero_copy) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    stats_.frames_processed++;
    stats_.bytes_processed += frame_size;
    
    if (was_zero_copy) {
        stats_.zero_copy_count++;
    } else {
        stats_.copy_count++;
    }
    
    // Update average frame size
    stats_.avg_frame_size = static_cast<double>(stats_.bytes_processed) / stats_.frames_processed;
    stats_.last_update = std::chrono::steady_clock::now();
}

ZeroCopyVideoWorker::Stats ZeroCopyVideoWorker::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

// ZeroCopyIntegration implementation
bool ZeroCopyIntegration::initialize() {
    if (initialized_.load()) {
        return true;
    }
    
    LOG_INFO("Initializing zero-copy integration system");

    // Initialize zero-copy buffer pool
    ZeroCopyBufferPool::getInstance();

    // Re-enable zero-copy now that RTP packetization is fixed
    for (int i = 0; i < NUM_VIDEO_CHANNELS; i++) {
        zero_copy_enabled_[i] = true;
    }

    LOG_INFO("Zero-copy re-enabled with proper RTP flow control");
    
    initialized_.store(true);
    LOG_INFO("Zero-copy integration system initialized");
    return true;
}

void ZeroCopyIntegration::enableZeroCopy(int stream_channel, bool enabled) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    zero_copy_enabled_[stream_channel] = enabled;
    
    LOG_INFO("Zero-copy " << (enabled ? "enabled" : "disabled") 
             << " for stream channel " << stream_channel);
}

bool ZeroCopyIntegration::isZeroCopyEnabled(int stream_channel) const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = zero_copy_enabled_.find(stream_channel);
    return (it != zero_copy_enabled_.end()) ? it->second : false;
}

std::shared_ptr<ZeroCopyVideoStream> ZeroCopyIntegration::convertStream(video_stream* legacy_stream) {
    if (!legacy_stream) {
        return nullptr;
    }
    
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    int channel = legacy_stream->encChn;
    auto it = zero_copy_streams_.find(channel);
    
    if (it != zero_copy_streams_.end()) {
        return it->second;
    }
    
    // Create new zero-copy stream
    auto zero_copy_stream = std::make_shared<ZeroCopyVideoStream>(
        legacy_stream->encChn, legacy_stream->stream, legacy_stream->name);
    
    // Copy relevant state
    zero_copy_stream->running = legacy_stream->running;
    zero_copy_stream->idr = legacy_stream->idr;
    zero_copy_stream->idr_fix = legacy_stream->idr_fix;
    zero_copy_stream->active = legacy_stream->active;
    zero_copy_stream->hasDataCallback = legacy_stream->hasDataCallback.load();
    zero_copy_stream->run_for_jpeg = legacy_stream->run_for_jpeg;
    
    zero_copy_streams_[channel] = zero_copy_stream;
    
    LOG_INFO("Converted legacy stream to zero-copy for channel " << channel);
    return zero_copy_stream;
}

ZeroCopyIntegration::IntegrationStats ZeroCopyIntegration::getStats() const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    IntegrationStats stats;
    stats.zero_copy_streams = zero_copy_streams_.size();
    stats.legacy_streams = NUM_VIDEO_CHANNELS - stats.zero_copy_streams;
    stats.total_zero_copy_frames = 0;
    stats.total_legacy_frames = 0;
    
    // Collect statistics from zero-copy workers
    for (const auto& [channel, stream] : zero_copy_streams_) {
        if (stream->zero_copy_worker) {
            auto worker_stats = stream->zero_copy_worker->getStats();
            stats.total_zero_copy_frames += worker_stats.zero_copy_count;
            stats.total_legacy_frames += worker_stats.copy_count;
        }
    }
    
    // Calculate efficiency
    uint64_t total_frames = stats.total_zero_copy_frames + stats.total_legacy_frames;
    stats.zero_copy_efficiency = total_frames > 0 ? 
        static_cast<double>(stats.total_zero_copy_frames) / total_frames : 0.0;
    
    return stats;
}

void ZeroCopyIntegration::shutdown() {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    // Stop all zero-copy workers
    for (auto& [channel, stream] : zero_copy_streams_) {
        if (stream->zero_copy_worker) {
            stream->zero_copy_worker->stop();
        }
    }
    
    zero_copy_streams_.clear();
    zero_copy_enabled_.clear();
    initialized_.store(false);
    
    LOG_INFO("Zero-copy integration system shutdown complete");
}
