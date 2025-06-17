#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include "Logger.hpp"
#include "Config.hpp"

#undef MODULE
#define MODULE "ADAPTIVE_BITRATE"

/**
 * Adaptive Bitrate Streaming Manager
 * 
 * Features:
 * - Real-time network condition monitoring via RTCP feedback
 * - Dynamic quality adjustment based on client capabilities
 * - Multiple quality levels with smooth transitions
 * - Packet loss and jitter detection
 * - Bandwidth estimation and congestion control
 * - Per-client adaptation for multi-client scenarios
 */

struct NetworkConditions {
    float packet_loss_rate;        // 0.0 to 1.0
    uint32_t rtt_ms;              // Round-trip time in milliseconds
    uint32_t jitter_ms;           // Jitter in milliseconds
    uint64_t bandwidth_bps;       // Estimated bandwidth in bits per second
    float congestion_level;       // 0.0 to 1.0 (0 = no congestion)
    std::chrono::steady_clock::time_point last_update;
};

struct QualityLevel {
    std::string name;
    int bitrate;                  // Target bitrate in kbps
    int width;                    // Video width
    int height;                   // Video height
    int fps;                      // Frame rate
    int min_qp;                   // Minimum QP
    int max_qp;                   // Maximum QP
    float min_bandwidth_factor;   // Minimum bandwidth as factor of bitrate
    float max_packet_loss;        // Maximum acceptable packet loss
    uint32_t max_rtt_ms;         // Maximum acceptable RTT
};

struct ClientSession {
    unsigned session_id;
    int stream_channel;
    NetworkConditions conditions;
    QualityLevel current_quality;
    QualityLevel target_quality;
    std::chrono::steady_clock::time_point last_adaptation;
    std::chrono::steady_clock::time_point session_start;
    uint32_t adaptation_count;
    bool adaptation_in_progress;
    float stability_score;        // 0.0 to 1.0 (higher = more stable)
};

struct AdaptationStats {
    uint32_t total_adaptations;
    uint32_t quality_upgrades;
    uint32_t quality_downgrades;
    uint32_t active_sessions;
    float average_quality_level;
    std::chrono::steady_clock::time_point last_update;
};

class AdaptiveBitrateManager {
public:
    static AdaptiveBitrateManager& getInstance() {
        static AdaptiveBitrateManager instance;
        return instance;
    }

    // Initialize adaptive bitrate system
    bool initialize();
    
    // Client session management
    void registerClient(unsigned session_id, int stream_channel);
    void unregisterClient(unsigned session_id);
    
    // RTCP feedback processing
    void processRTCPFeedback(unsigned session_id, 
                           float packet_loss_rate,
                           uint32_t rtt_ms,
                           uint32_t jitter_ms,
                           uint64_t bytes_received);
    
    // Quality level management
    void defineQualityLevels(int stream_channel, const std::vector<QualityLevel>& levels);
    std::vector<QualityLevel> getQualityLevels(int stream_channel) const;
    
    // Adaptation control
    bool shouldAdaptQuality(unsigned session_id);
    QualityLevel getTargetQuality(unsigned session_id);
    void applyQualityChange(unsigned session_id, const QualityLevel& quality);
    
    // Network monitoring
    NetworkConditions getNetworkConditions(unsigned session_id);
    float estimateBandwidth(unsigned session_id);
    
    // Statistics and monitoring
    AdaptationStats getAdaptationStats();
    std::string generateAdaptationReport();
    
    // Configuration
    void setAdaptationInterval(std::chrono::seconds interval);
    void setStabilityThreshold(float threshold);
    void setPacketLossThreshold(float threshold);
    void setBandwidthMargin(float margin);
    
    // Control
    void enableAdaptation(bool enabled);
    bool isAdaptationEnabled() const;
    void shutdown();

private:
    AdaptiveBitrateManager() = default;
    ~AdaptiveBitrateManager() { shutdown(); }
    
    // Prevent copying
    AdaptiveBitrateManager(const AdaptiveBitrateManager&) = delete;
    AdaptiveBitrateManager& operator=(const AdaptiveBitrateManager&) = delete;

    // Adaptation logic
    QualityLevel selectOptimalQuality(const ClientSession& session);
    bool shouldUpgradeQuality(const ClientSession& session);
    bool shouldDowngradeQuality(const ClientSession& session);
    float calculateStabilityScore(const ClientSession& session);
    
    // Bandwidth estimation
    void updateBandwidthEstimate(ClientSession& session, uint64_t bytes_received);
    uint64_t estimateAvailableBandwidth(const NetworkConditions& conditions);
    
    // Quality level utilities
    QualityLevel findBestQualityForBandwidth(int stream_channel, uint64_t bandwidth_bps);
    QualityLevel getNextLowerQuality(int stream_channel, const QualityLevel& current);
    QualityLevel getNextHigherQuality(int stream_channel, const QualityLevel& current);
    
    // Default quality levels
    void initializeDefaultQualityLevels();
    std::vector<QualityLevel> createDefaultQualityLevels(int stream_channel);

    // Data structures
    std::unordered_map<unsigned, std::unique_ptr<ClientSession>> client_sessions_;
    std::unordered_map<int, std::vector<QualityLevel>> quality_levels_; // per stream channel
    std::mutex sessions_mutex_;
    mutable std::mutex quality_mutex_;
    
    // Statistics
    AdaptationStats stats_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> adaptation_enabled_{true};
    
    // Configuration
    std::chrono::seconds adaptation_interval_{std::chrono::seconds(5)};
    float stability_threshold_{0.7f};
    float packet_loss_threshold_{0.05f}; // 5%
    float bandwidth_margin_{1.2f}; // 20% margin
    
    // Monitoring thread
    std::unique_ptr<std::thread> monitoring_thread_;
    std::atomic<bool> should_stop_{false};
    
public:
    // Constants
    static constexpr uint32_t MIN_ADAPTATION_INTERVAL_MS = 2000;
    static constexpr uint32_t MAX_RTT_MS = 1000;
    static constexpr float MAX_PACKET_LOSS = 0.20f; // 20%
    static constexpr uint32_t BANDWIDTH_WINDOW_SIZE = 10;
    static constexpr float QUALITY_CHANGE_HYSTERESIS = 0.15f; // 15% hysteresis
};

// Quality level presets
namespace QualityPresets {
    // Ultra Low (for very poor connections)
    const QualityLevel ULTRA_LOW = {
        "Ultra Low", 200, 320, 240, 10, 35, 51, 1.5f, 0.15f, 800
    };
    
    // Low (for poor connections)
    const QualityLevel LOW = {
        "Low", 500, 640, 360, 15, 30, 45, 1.3f, 0.10f, 600
    };
    
    // Medium (for average connections)
    const QualityLevel MEDIUM = {
        "Medium", 1000, 854, 480, 20, 25, 40, 1.2f, 0.08f, 400
    };
    
    // High (for good connections)
    const QualityLevel HIGH = {
        "High", 2000, 1280, 720, 25, 22, 35, 1.15f, 0.05f, 300
    };
    
    // Ultra High (for excellent connections)
    const QualityLevel ULTRA_HIGH = {
        "Ultra High", 4000, 1920, 1080, 30, 18, 30, 1.1f, 0.02f, 200
    };
}

// Utility functions
namespace AdaptiveUtils {
    // Convert quality level to string
    std::string qualityToString(const QualityLevel& quality);
    
    // Calculate quality score (0.0 to 1.0)
    float calculateQualityScore(const QualityLevel& quality);
    
    // Estimate required bandwidth for quality level
    uint64_t estimateRequiredBandwidth(const QualityLevel& quality);
    
    // Check if quality change is significant
    bool isSignificantQualityChange(const QualityLevel& from, const QualityLevel& to);
}
