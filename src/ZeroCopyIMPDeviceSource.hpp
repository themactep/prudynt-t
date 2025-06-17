#pragma once

#include "FramedSource.hh"
#include "ZeroCopyBuffer.hpp"
#include "ZeroCopyVideoWorker.hpp"
#include "Logger.hpp"
#include <mutex>
#include <condition_variable>

#undef MODULE
#define MODULE "ZEROCOPY_SOURCE"

/**
 * Zero-Copy IMP Device Source
 * 
 * Optimized version of IMPDeviceSource that eliminates memory copies
 * by directly using zero-copy buffers from the video pipeline.
 */

/**
 * Base class for zero-copy device sources
 */
class ZeroCopyDeviceSourceBase : public FramedSource {
public:
    void onDataAvailable() {
        if (eventTriggerId != 0) {
            envir().taskScheduler().triggerEvent(eventTriggerId, this);
        }
    }

protected:
    ZeroCopyDeviceSourceBase(UsageEnvironment& env, int encChn, const char* name);
    virtual ~ZeroCopyDeviceSourceBase();

    // FramedSource overrides
    virtual void doGetNextFrame() override;
    virtual void doStopGettingFrames() override;

    // Pure virtual methods for subclasses
    virtual bool readNextFrame() = 0;
    virtual void setupCallbacks() = 0;
    virtual void cleanupCallbacks() = 0;

private:
    static void deliverFrame0(void* clientData);
    void deliverFrame();

protected:
    int encChn;
    const char* name;
    EventTriggerId eventTriggerId;

    // Performance tracking
    struct DeliveryStats {
        uint64_t zero_copy_deliveries;
        uint64_t legacy_deliveries;
        uint64_t total_bytes_delivered;
        std::chrono::steady_clock::time_point last_update;
    };

    DeliveryStats delivery_stats_;
    mutable std::mutex stats_mutex_;

    // Optimization state
    bool zero_copy_preferred_;
    size_t consecutive_zero_copy_failures_;

    static constexpr size_t MAX_ZERO_COPY_FAILURES = 5;
};

/**
 * Specialized zero-copy source for video streams
 */
class ZeroCopyVideoSource : public ZeroCopyDeviceSourceBase {
public:
    static ZeroCopyVideoSource* createNew(UsageEnvironment& env, int encChn,
                                         std::shared_ptr<ZeroCopyVideoStream> stream,
                                         const char* name);

    // Get delivery statistics
    struct VideoDeliveryStats {
        uint64_t frames_delivered;
        uint64_t keyframes_delivered;
        uint64_t bytes_delivered;
        double avg_frame_size;
        double zero_copy_ratio;
        std::chrono::steady_clock::time_point last_update;
    };

    VideoDeliveryStats getVideoStats() const;

protected:
    ZeroCopyVideoSource(UsageEnvironment& env, int encChn,
                       std::shared_ptr<ZeroCopyVideoStream> stream, const char* name);
    virtual ~ZeroCopyVideoSource();

    // Implement pure virtual methods
    virtual bool readNextFrame() override;
    virtual void setupCallbacks() override;
    virtual void cleanupCallbacks() override;

private:
    // Video-specific delivery optimization
    bool deliverVideoFrame();
    void updateVideoStats(const ZeroCopyNALUnit& nal_unit, bool was_zero_copy);

    std::shared_ptr<ZeroCopyVideoStream> stream;
    VideoDeliveryStats video_stats_;
    mutable std::mutex video_stats_mutex_;
};

/**
 * Zero-Copy Source Factory
 * 
 * Creates appropriate source types based on stream configuration
 */
class ZeroCopySourceFactory {
public:
    // Create zero-copy source for video stream
    static FramedSource* createVideoSource(UsageEnvironment& env, int encChn,
                                          const char* name = "video");
    
    // Create zero-copy source for audio stream  
    static FramedSource* createAudioSource(UsageEnvironment& env, int encChn,
                                          const char* name = "audio");
    
    // Check if zero-copy is available for stream
    static bool isZeroCopyAvailable(int encChn);
    
    // Get optimal source configuration
    struct SourceConfig {
        bool use_zero_copy;
        size_t buffer_size;
        size_t max_frame_size;
        bool enable_prefetch;
    };
    
    static SourceConfig getOptimalConfig(int encChn);

private:
    // Helper methods
    static bool checkZeroCopySupport(int encChn);
    static size_t estimateOptimalBufferSize(int encChn);
};

/**
 * Zero-Copy Performance Monitor
 * 
 * Monitors and reports zero-copy performance metrics
 */
class ZeroCopyPerformanceMonitor {
public:
    static ZeroCopyPerformanceMonitor& getInstance() {
        static ZeroCopyPerformanceMonitor instance;
        return instance;
    }
    
    // Register a zero-copy source for monitoring
    void registerSource(int encChn, ZeroCopyVideoSource* source);
    
    // Unregister source
    void unregisterSource(int encChn);
    
    // Get performance report
    struct PerformanceReport {
        size_t active_sources;
        double overall_zero_copy_ratio;
        uint64_t total_bytes_saved;
        double avg_delivery_latency_ms;
        std::vector<std::pair<int, double>> per_channel_ratios;
    };
    
    PerformanceReport generateReport() const;
    
    // Log performance summary
    void logPerformanceSummary() const;

private:
    ZeroCopyPerformanceMonitor() = default;
    ~ZeroCopyPerformanceMonitor() = default;
    
    // Prevent copying
    ZeroCopyPerformanceMonitor(const ZeroCopyPerformanceMonitor&) = delete;
    ZeroCopyPerformanceMonitor& operator=(const ZeroCopyPerformanceMonitor&) = delete;
    
    std::unordered_map<int, ZeroCopyVideoSource*> monitored_sources_;
    mutable std::mutex sources_mutex_;
};


