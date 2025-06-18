#include "ZeroCopyIMPDeviceSource.hpp"
#include "GroupsockHelper.hh"
#include "globals.hpp"
#include "IMPDeviceSource.hpp"

// ZeroCopyDeviceSourceBase implementation
ZeroCopyDeviceSourceBase::ZeroCopyDeviceSourceBase(UsageEnvironment& env, int encChn, const char* name)
    : FramedSource(env), encChn(encChn), name(name), eventTriggerId(0),
      zero_copy_preferred_(true), consecutive_zero_copy_failures_(0) {

    // Initialize statistics
    delivery_stats_ = {};
    delivery_stats_.last_update = std::chrono::steady_clock::now();

    eventTriggerId = envir().taskScheduler().createEventTrigger(deliverFrame0);

    LOG_DEBUG("ZeroCopyDeviceSourceBase " << name << " constructed, encoder channel:" << encChn);
}

ZeroCopyDeviceSourceBase::~ZeroCopyDeviceSourceBase() {
    // Don't call pure virtual function from destructor
    // Subclasses should handle cleanup in their own destructors

    if (eventTriggerId != 0) {
        envir().taskScheduler().deleteEventTrigger(eventTriggerId);
        eventTriggerId = 0;
    }

    LOG_DEBUG("ZeroCopyDeviceSourceBase " << name << " destructed, encoder channel:" << encChn);
}

void ZeroCopyDeviceSourceBase::doGetNextFrame() {
    deliverFrame();
}

void ZeroCopyDeviceSourceBase::doStopGettingFrames() {
    // Stop getting frames
    FramedSource::doStopGettingFrames();
}

void ZeroCopyDeviceSourceBase::deliverFrame0(void* clientData) {
    static_cast<ZeroCopyDeviceSourceBase*>(clientData)->deliverFrame();
}

void ZeroCopyDeviceSourceBase::deliverFrame() {
    if (!isCurrentlyAwaitingData()) {
        return;
    }

    // Try to read next frame
    if (readNextFrame()) {
        delivery_stats_.zero_copy_deliveries++;
        consecutive_zero_copy_failures_ = 0;
        return; // readNextFrame() already called afterGetting()
    }

    // No frame available - schedule retry
    consecutive_zero_copy_failures_++;
    if (consecutive_zero_copy_failures_ > MAX_ZERO_COPY_FAILURES) {
        zero_copy_preferred_ = false;
        LOG_WARN("Disabling zero-copy for channel " << encChn << " due to failures");
    }

    // No frame available - follow correct Live555 pattern
    // Don't call afterGetting() or schedule retry - just wait for onDataAvailable() callback
    // This prevents race condition and follows Live555 event-driven architecture
}

// ZeroCopyVideoSource implementation
ZeroCopyVideoSource* ZeroCopyVideoSource::createNew(UsageEnvironment& env, int encChn,
                                                   std::shared_ptr<ZeroCopyVideoStream> stream,
                                                   const char* name) {
    return new ZeroCopyVideoSource(env, encChn, stream, name);
}

ZeroCopyVideoSource::ZeroCopyVideoSource(UsageEnvironment& env, int encChn,
                                       std::shared_ptr<ZeroCopyVideoStream> stream, const char* name)
    : ZeroCopyDeviceSourceBase(env, encChn, name), stream(stream) {

    // Initialize video-specific statistics
    video_stats_ = {};
    video_stats_.last_update = std::chrono::steady_clock::now();

    // Setup callbacks
    setupCallbacks();

    // Register with performance monitor
    ZeroCopyPerformanceMonitor::getInstance().registerSource(encChn, this);

    LOG_INFO("Created ZeroCopyVideoSource for channel " << encChn);
}

ZeroCopyVideoSource::~ZeroCopyVideoSource() {
    cleanupCallbacks();
}

void ZeroCopyVideoSource::setupCallbacks() {
    std::lock_guard<std::mutex> lock_stream{mutex_main};
    std::lock_guard<std::mutex> lock_callback{stream->onDataCallbackLock};

    stream->onDataCallback = [this]() { this->onDataAvailable(); };
    stream->hasDataCallback = true;
    stream->should_grab_frames.notify_one();
}

void ZeroCopyVideoSource::cleanupCallbacks() {
    if (stream) {
        std::lock_guard<std::mutex> lock_stream{mutex_main};
        std::lock_guard<std::mutex> lock_callback{stream->onDataCallbackLock};

        stream->hasDataCallback = false;
        stream->onDataCallback = nullptr;
    }

    // Unregister from performance monitor
    ZeroCopyPerformanceMonitor::getInstance().unregisterSource(encChn);
}

bool ZeroCopyVideoSource::readNextFrame() {
    ZeroCopyNALUnit nal_unit;

    if (!stream->msgChannel->read(&nal_unit)) {
        return false; // No data available
    }

    if (!nal_unit.isValid()) {
        LOG_WARN("Invalid zero-copy NAL unit received for channel " << encChn);
        return false;
    }

    // Update video statistics
    updateVideoStats(nal_unit, true);

    // Check if frame fits in destination buffer
    size_t frame_size = nal_unit.size();
    if (frame_size > fMaxSize) {
        fFrameSize = fMaxSize;
        fNumTruncatedBytes = frame_size - fMaxSize;
        LOG_WARN("Frame truncated: " << frame_size << " -> " << fMaxSize << " bytes");
    } else {
        fFrameSize = frame_size;
        fNumTruncatedBytes = 0;
    }

    // Set presentation time
    fPresentationTime = nal_unit.timestamp;

    // Zero-copy: direct pointer access
    const uint8_t* src_data = nal_unit.data();
    if (src_data && fFrameSize > 0) {
        // Copy data to LiveMedia555 buffer (unfortunately required by LiveMedia555 API)
        std::memcpy(fTo, src_data, fFrameSize);

        // Update statistics
        delivery_stats_.total_bytes_delivered += fFrameSize;
        delivery_stats_.last_update = std::chrono::steady_clock::now();

        LOG_DEBUG("Delivered zero-copy frame: " << fFrameSize << " bytes, channel " << encChn);

        FramedSource::afterGetting(this);
        return true;
    }

    return false;
}

void ZeroCopyVideoSource::updateVideoStats(const ZeroCopyNALUnit& nal_unit, bool was_zero_copy) {
    std::lock_guard<std::mutex> lock(video_stats_mutex_);

    video_stats_.frames_delivered++;
    video_stats_.bytes_delivered += nal_unit.size();

    if (nal_unit.is_keyframe) {
        video_stats_.keyframes_delivered++;
    }

    // Update average frame size
    video_stats_.avg_frame_size = static_cast<double>(video_stats_.bytes_delivered) /
                                  video_stats_.frames_delivered;

    // Update zero-copy ratio
    video_stats_.zero_copy_ratio = static_cast<double>(delivery_stats_.zero_copy_deliveries) /
                                   (delivery_stats_.zero_copy_deliveries + delivery_stats_.legacy_deliveries);

    video_stats_.last_update = std::chrono::steady_clock::now();
}

ZeroCopyVideoSource::VideoDeliveryStats ZeroCopyVideoSource::getVideoStats() const {
    std::lock_guard<std::mutex> lock(video_stats_mutex_);
    return video_stats_;
}



// ZeroCopySourceFactory implementation
FramedSource* ZeroCopySourceFactory::createVideoSource(UsageEnvironment& env, int encChn, const char* name) {
    // Check if zero-copy is available and enabled
    if (isZeroCopyAvailable(encChn)) {
        auto& integration = ZeroCopyIntegration::getInstance();
        auto zero_copy_stream = integration.convertStream(global_video[encChn].get());

        if (zero_copy_stream) {
            LOG_INFO("Creating zero-copy video source for channel " << encChn);
            return ZeroCopyVideoSource::createNew(env, encChn, zero_copy_stream, name);
        }
    }

    // Fallback to legacy source
    LOG_INFO("Creating legacy video source for channel " << encChn);
    return IMPDeviceSource<H264NALUnit, video_stream>::createNew(env, encChn, global_video[encChn], name);
}

FramedSource* ZeroCopySourceFactory::createAudioSource(UsageEnvironment& env, int encChn, const char* name) {
    // Audio sources use legacy implementation for now
    return IMPDeviceSource<AudioFrame, audio_stream>::createNew(env, encChn, global_audio[encChn], name);
}

bool ZeroCopySourceFactory::isZeroCopyAvailable(int encChn) {
    if (encChn < 0 || encChn >= NUM_VIDEO_CHANNELS) {
        return false;
    }
    
    auto& integration = ZeroCopyIntegration::getInstance();
    return integration.isZeroCopyEnabled(encChn);
}

ZeroCopySourceFactory::SourceConfig ZeroCopySourceFactory::getOptimalConfig(int encChn) {
    SourceConfig config;
    config.use_zero_copy = isZeroCopyAvailable(encChn);
    config.buffer_size = estimateOptimalBufferSize(encChn);
    config.max_frame_size = 1024 * 1024; // 1MB default
    config.enable_prefetch = true;
    
    return config;
}

bool ZeroCopySourceFactory::checkZeroCopySupport(int encChn) {
    // Check platform support
#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
    return true; // These platforms support zero-copy
#else
    return false; // Older platforms may not support zero-copy reliably
#endif
}

size_t ZeroCopySourceFactory::estimateOptimalBufferSize(int encChn) {
    // Estimate based on stream configuration
    if (encChn >= 0 && encChn < NUM_VIDEO_CHANNELS && global_video[encChn]) {
        auto stream = global_video[encChn]->stream;
        if (stream) {
            // Estimate based on resolution and bitrate
            size_t estimated_size = (stream->width * stream->height * stream->bitrate) / (8 * stream->fps);
            return std::max(estimated_size, static_cast<size_t>(64 * 1024)); // Minimum 64KB
        }
    }
    
    return 256 * 1024; // Default 256KB
}

// ZeroCopyPerformanceMonitor implementation
void ZeroCopyPerformanceMonitor::registerSource(int encChn, ZeroCopyVideoSource* source) {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    monitored_sources_[encChn] = source;
    
    LOG_DEBUG("Registered zero-copy source for monitoring: channel " << encChn);
}

void ZeroCopyPerformanceMonitor::unregisterSource(int encChn) {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    monitored_sources_.erase(encChn);
    
    LOG_DEBUG("Unregistered zero-copy source from monitoring: channel " << encChn);
}

ZeroCopyPerformanceMonitor::PerformanceReport ZeroCopyPerformanceMonitor::generateReport() const {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    
    PerformanceReport report;
    report.active_sources = monitored_sources_.size();
    report.overall_zero_copy_ratio = 0.0;
    report.total_bytes_saved = 0;
    report.avg_delivery_latency_ms = 0.0;
    
    if (monitored_sources_.empty()) {
        return report;
    }
    
    double total_ratio = 0.0;
    for (const auto& [channel, source] : monitored_sources_) {
        if (source) {
            auto stats = source->getVideoStats();
            double ratio = stats.zero_copy_ratio;
            total_ratio += ratio;
            
            report.per_channel_ratios.emplace_back(channel, ratio);
            
            // Estimate bytes saved (assuming 50% reduction with zero-copy)
            report.total_bytes_saved += static_cast<uint64_t>(stats.bytes_delivered * ratio * 0.5);
        }
    }
    
    report.overall_zero_copy_ratio = total_ratio / monitored_sources_.size();
    
    return report;
}

void ZeroCopyPerformanceMonitor::logPerformanceSummary() const {
    auto report = generateReport();
    
    LOG_INFO("=== Zero-Copy Performance Summary ===");
    LOG_INFO("Active Sources: " << report.active_sources);
    LOG_INFO("Overall Zero-Copy Ratio: " << (report.overall_zero_copy_ratio * 100) << "%");
    LOG_INFO("Estimated Bytes Saved: " << (report.total_bytes_saved / (1024 * 1024)) << " MB");
    
    for (const auto& [channel, ratio] : report.per_channel_ratios) {
        LOG_INFO("Channel " << channel << " Zero-Copy Ratio: " << (ratio * 100) << "%");
    }
    LOG_INFO("=====================================");
}
