#pragma once

#include "ZeroCopyBuffer.hpp"
#include "globals.hpp"
#include "Config.hpp"
#include "IMPEncoder.hpp"
#include "Logger.hpp"
#include <thread>
#include <atomic>

#undef MODULE
#define MODULE "ZEROCOPY_VIDEO"

/**
 * Zero-Copy Video Worker
 * 
 * Optimized video processing pipeline that eliminates unnecessary memory copies:
 * - Direct buffer access from IMP encoder
 * - Zero-copy NAL unit creation
 * - Reference-counted buffer sharing
 * - Optimized message channel with move semantics
 */

class ZeroCopyVideoWorker {
public:
    ZeroCopyVideoWorker(int encoder_channel);
    ~ZeroCopyVideoWorker();
    
    // Start the zero-copy video processing
    void start();
    
    // Stop the video processing
    void stop();
    
    // Check if worker is running
    bool isRunning() const { return running_.load(); }
    
    // Get processing statistics
    struct Stats {
        uint64_t frames_processed;
        uint64_t bytes_processed;
        uint64_t zero_copy_count;
        uint64_t copy_count;
        double avg_frame_size;
        std::chrono::steady_clock::time_point last_update;
    };
    
    Stats getStats() const;

private:
    // Main processing loop
    void processingLoop();
    
    // Process a single encoder stream
    void processEncoderStream(const IMPEncoderStream& stream);
    
    // Create zero-copy NAL unit from encoder data
    ZeroCopyNALUnit createNALUnit(uint8_t* encoder_data, size_t size, size_t offset, 
                                  uint8_t nal_type, bool is_keyframe);
    
    // Check if we should process this frame
    bool shouldProcessFrame() const;
    
    // Update statistics
    void updateStats(size_t frame_size, bool was_zero_copy);
    
    int encoder_channel_;
    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> worker_thread_;
    
    // Statistics
    mutable std::mutex stats_mutex_;
    Stats stats_;
    
    // Configuration
    bool zero_copy_enabled_;
    size_t max_frame_size_;
    
    // Performance tracking
    std::chrono::steady_clock::time_point last_frame_time_;
    uint32_t consecutive_errors_;
    
    static constexpr uint32_t MAX_CONSECUTIVE_ERRORS = 10;
    static constexpr size_t DEFAULT_MAX_FRAME_SIZE = 1024 * 1024; // 1MB
};

/**
 * Zero-Copy Video Stream - enhanced version of video_stream
 */
struct ZeroCopyVideoStream {
    int encChn;
    _stream* stream;
    const char* name;
    bool running;
    pthread_t thread;
    bool idr;
    int idr_fix;
    bool active{false};
    
    IMPEncoder* imp_encoder;
    IMPFramesource* imp_framesource;
    
    // Zero-copy message channel
    std::shared_ptr<ZeroCopyMsgChannel<ZeroCopyNALUnit>> msgChannel;
    
    std::function<void(void)> onDataCallback;
    bool run_for_jpeg;
    std::atomic<bool> hasDataCallback;
    std::mutex onDataCallbackLock;
    std::condition_variable should_grab_frames;
    std::binary_semaphore is_activated{0};
    
    // Zero-copy specific
    std::unique_ptr<ZeroCopyVideoWorker> zero_copy_worker;
    bool zero_copy_enabled;
    
    ZeroCopyVideoStream(int encChn, _stream* stream, const char* name)
        : encChn(encChn), stream(stream), name(name), running(false), 
          idr(false), idr_fix(0), imp_encoder(nullptr), imp_framesource(nullptr),
          msgChannel(std::make_shared<ZeroCopyMsgChannel<ZeroCopyNALUnit>>(MSG_CHANNEL_SIZE)),
          onDataCallback(nullptr), run_for_jpeg{false}, hasDataCallback{false},
          zero_copy_enabled(true) {}
};

/**
 * Zero-Copy Integration Manager
 * 
 * Manages the transition between legacy and zero-copy systems
 */
class ZeroCopyIntegration {
public:
    static ZeroCopyIntegration& getInstance() {
        static ZeroCopyIntegration instance;
        return instance;
    }
    
    // Initialize zero-copy system
    bool initialize();
    
    // Enable/disable zero-copy for specific stream
    void enableZeroCopy(int stream_channel, bool enabled);
    
    // Check if zero-copy is enabled for stream
    bool isZeroCopyEnabled(int stream_channel) const;
    
    // Convert legacy video_stream to zero-copy version
    std::shared_ptr<ZeroCopyVideoStream> convertStream(video_stream* legacy_stream);
    
    // Get zero-copy statistics
    struct IntegrationStats {
        size_t zero_copy_streams;
        size_t legacy_streams;
        uint64_t total_zero_copy_frames;
        uint64_t total_legacy_frames;
        double zero_copy_efficiency;
    };
    
    IntegrationStats getStats() const;
    
    // Cleanup
    void shutdown();

private:
    ZeroCopyIntegration() = default;
    ~ZeroCopyIntegration() { shutdown(); }
    
    // Prevent copying
    ZeroCopyIntegration(const ZeroCopyIntegration&) = delete;
    ZeroCopyIntegration& operator=(const ZeroCopyIntegration&) = delete;
    
    std::unordered_map<int, bool> zero_copy_enabled_;
    std::unordered_map<int, std::shared_ptr<ZeroCopyVideoStream>> zero_copy_streams_;
    mutable std::mutex streams_mutex_;
    
    IntegrationStats stats_;
    std::atomic<bool> initialized_{false};
};

// Utility functions for zero-copy integration
namespace ZeroCopyIntegrationUtils {
    // Check if system supports zero-copy optimizations
    bool isZeroCopySupported();
    
    // Estimate memory savings from zero-copy
    size_t estimateMemorySavings(int stream_channel);
    
    // Benchmark zero-copy vs legacy performance
    struct BenchmarkResult {
        double zero_copy_fps;
        double legacy_fps;
        double memory_usage_reduction;
        double cpu_usage_reduction;
    };
    
    BenchmarkResult benchmarkPerformance(int stream_channel, size_t test_duration_seconds = 10);
    
    // Get optimal buffer configuration for zero-copy
    struct OptimalConfig {
        size_t buffer_count;
        size_t buffer_size;
        bool use_memory_mapping;
        bool enable_prefetch;
    };
    
    OptimalConfig getOptimalConfig(int stream_channel);
}
