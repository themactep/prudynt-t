#include "AdaptiveBitrate.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>

bool AdaptiveBitrateManager::initialize() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    if (initialized_.load()) {
        return true;
    }
    
    LOG_INFO("Initializing adaptive bitrate streaming manager");
    
    // Initialize default quality levels for all streams
    initializeDefaultQualityLevels();
    
    // Initialize statistics
    stats_ = {};
    stats_.last_update = std::chrono::steady_clock::now();
    
    initialized_.store(true);
    
    LOG_INFO("Adaptive bitrate manager initialized with " 
             << quality_levels_.size() << " stream configurations");
    return true;
}

void AdaptiveBitrateManager::initializeDefaultQualityLevels() {
    std::lock_guard<std::mutex> lock(quality_mutex_);
    
    // Create quality levels for stream 0 (main stream)
    quality_levels_[0] = {
        QualityPresets::ULTRA_LOW,
        QualityPresets::LOW,
        QualityPresets::MEDIUM,
        QualityPresets::HIGH,
        QualityPresets::ULTRA_HIGH
    };
    
    // Create quality levels for stream 1 (secondary stream)
    quality_levels_[1] = {
        {"Ultra Low", 150, 320, 240, 10, 35, 51, 1.5f, 0.15f, 800},
        {"Low", 300, 480, 270, 15, 30, 45, 1.3f, 0.10f, 600},
        {"Medium", 600, 640, 360, 20, 25, 40, 1.2f, 0.08f, 400},
        {"High", 1000, 854, 480, 25, 22, 35, 1.15f, 0.05f, 300}
    };
    
    LOG_DEBUG("Initialized default quality levels for streams 0 and 1");
}

void AdaptiveBitrateManager::registerClient(unsigned session_id, int stream_channel) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto session = std::make_unique<ClientSession>();
    session->session_id = session_id;
    session->stream_channel = stream_channel;
    session->session_start = std::chrono::steady_clock::now();
    session->last_adaptation = session->session_start;
    session->adaptation_count = 0;
    session->adaptation_in_progress = false;
    session->stability_score = 1.0f;
    
    // Initialize with default network conditions
    session->conditions = {};
    session->conditions.packet_loss_rate = 0.0f;
    session->conditions.rtt_ms = 100;
    session->conditions.jitter_ms = 10;
    session->conditions.bandwidth_bps = 5000000; // 5 Mbps default
    session->conditions.congestion_level = 0.0f;
    session->conditions.last_update = std::chrono::steady_clock::now();
    
    // Start with medium quality
    auto quality_levels = getQualityLevels(stream_channel);
    if (!quality_levels.empty()) {
        size_t medium_index = quality_levels.size() / 2;
        session->current_quality = quality_levels[medium_index];
        session->target_quality = session->current_quality;
    }
    
    client_sessions_[session_id] = std::move(session);
    stats_.active_sessions++;
    
    LOG_INFO("Registered adaptive bitrate client " << session_id 
             << " for stream " << stream_channel 
             << " with quality: " << AdaptiveUtils::qualityToString(client_sessions_[session_id]->current_quality));
}

void AdaptiveBitrateManager::unregisterClient(unsigned session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = client_sessions_.find(session_id);
    if (it != client_sessions_.end()) {
        LOG_INFO("Unregistered adaptive bitrate client " << session_id);
        client_sessions_.erase(it);
        stats_.active_sessions--;
    }
}

void AdaptiveBitrateManager::processRTCPFeedback(unsigned session_id,
                                               float packet_loss_rate,
                                               uint32_t rtt_ms,
                                               uint32_t jitter_ms,
                                               uint64_t bytes_received) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = client_sessions_.find(session_id);
    if (it == client_sessions_.end()) {
        return;
    }
    
    auto& session = *it->second;
    auto& conditions = session.conditions;
    
    // Update network conditions with smoothing
    const float alpha = 0.3f; // Smoothing factor
    conditions.packet_loss_rate = alpha * packet_loss_rate + (1 - alpha) * conditions.packet_loss_rate;
    conditions.rtt_ms = static_cast<uint32_t>(alpha * rtt_ms + (1 - alpha) * conditions.rtt_ms);
    conditions.jitter_ms = static_cast<uint32_t>(alpha * jitter_ms + (1 - alpha) * conditions.jitter_ms);
    conditions.last_update = std::chrono::steady_clock::now();
    
    // Update bandwidth estimate
    updateBandwidthEstimate(session, bytes_received);
    
    // Calculate congestion level
    conditions.congestion_level = std::min(1.0f, 
        conditions.packet_loss_rate * 5.0f + 
        std::min(1.0f, static_cast<float>(conditions.rtt_ms) / 500.0f) * 0.3f +
        std::min(1.0f, static_cast<float>(conditions.jitter_ms) / 100.0f) * 0.2f);
    
    // Update stability score
    session.stability_score = calculateStabilityScore(session);
    
    LOG_DEBUG("RTCP feedback for session " << session_id 
              << ": loss=" << (conditions.packet_loss_rate * 100) << "%, "
              << "rtt=" << conditions.rtt_ms << "ms, "
              << "jitter=" << conditions.jitter_ms << "ms, "
              << "bw=" << (conditions.bandwidth_bps / 1000) << "kbps, "
              << "congestion=" << (conditions.congestion_level * 100) << "%");
}

void AdaptiveBitrateManager::updateBandwidthEstimate(ClientSession& session, uint64_t bytes_received) {
    static std::unordered_map<unsigned, std::vector<std::pair<std::chrono::steady_clock::time_point, uint64_t>>> bandwidth_history;
    
    auto now = std::chrono::steady_clock::now();
    auto& history = bandwidth_history[session.session_id];
    
    // Add current measurement
    history.emplace_back(now, bytes_received);
    
    // Keep only recent measurements (last 30 seconds)
    auto cutoff = now - std::chrono::seconds(30);
    history.erase(std::remove_if(history.begin(), history.end(),
        [cutoff](const auto& entry) { return entry.first < cutoff; }), history.end());
    
    // Calculate bandwidth over the window
    if (history.size() >= 2) {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            history.back().first - history.front().first).count();
        
        if (duration > 0) {
            uint64_t total_bytes = 0;
            for (size_t i = 1; i < history.size(); ++i) {
                total_bytes += history[i].second;
            }
            
            // Convert to bits per second
            session.conditions.bandwidth_bps = (total_bytes * 8 * 1000) / duration;
        }
    }
}

bool AdaptiveBitrateManager::shouldAdaptQuality(unsigned session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = client_sessions_.find(session_id);
    if (it == client_sessions_.end() || !adaptation_enabled_.load()) {
        return false;
    }
    
    auto& session = *it->second;
    auto now = std::chrono::steady_clock::now();
    
    // Check if enough time has passed since last adaptation
    if (now - session.last_adaptation < adaptation_interval_) {
        return false;
    }
    
    // Check if adaptation is already in progress
    if (session.adaptation_in_progress) {
        return false;
    }
    
    // Check if conditions warrant adaptation
    return shouldUpgradeQuality(session) || shouldDowngradeQuality(session);
}

bool AdaptiveBitrateManager::shouldUpgradeQuality(const ClientSession& session) {
    const auto& conditions = session.conditions;
    
    // Check if network conditions are good enough for upgrade
    bool good_conditions = 
        conditions.packet_loss_rate < packet_loss_threshold_ * 0.5f &&
        conditions.rtt_ms < session.current_quality.max_rtt_ms * 0.8f &&
        conditions.congestion_level < 0.3f &&
        session.stability_score > stability_threshold_;
    
    if (!good_conditions) {
        return false;
    }
    
    // Check if we have enough bandwidth for next quality level
    auto next_quality = getNextHigherQuality(session.stream_channel, session.current_quality);
    if (next_quality.bitrate == session.current_quality.bitrate) {
        return false; // Already at highest quality
    }
    
    uint64_t required_bandwidth = AdaptiveUtils::estimateRequiredBandwidth(next_quality);
    return conditions.bandwidth_bps > required_bandwidth * bandwidth_margin_;
}

bool AdaptiveBitrateManager::shouldDowngradeQuality(const ClientSession& session) {
    const auto& conditions = session.conditions;
    
    // Check if network conditions require downgrade
    bool poor_conditions = 
        conditions.packet_loss_rate > packet_loss_threshold_ ||
        conditions.rtt_ms > session.current_quality.max_rtt_ms ||
        conditions.congestion_level > 0.7f;
    
    if (poor_conditions) {
        return true;
    }
    
    // Check if current bandwidth is insufficient
    uint64_t required_bandwidth = AdaptiveUtils::estimateRequiredBandwidth(session.current_quality);
    return conditions.bandwidth_bps < required_bandwidth * (2.0f - bandwidth_margin_);
}

QualityLevel AdaptiveBitrateManager::getTargetQuality(unsigned session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = client_sessions_.find(session_id);
    if (it == client_sessions_.end()) {
        return {};
    }
    
    auto& session = *it->second;
    QualityLevel target_quality = selectOptimalQuality(session);
    
    // Apply hysteresis to prevent oscillation
    if (AdaptiveUtils::isSignificantQualityChange(session.current_quality, target_quality)) {
        session.target_quality = target_quality;
        session.adaptation_in_progress = true;
        session.last_adaptation = std::chrono::steady_clock::now();
        session.adaptation_count++;
        
        // Update statistics
        if (target_quality.bitrate > session.current_quality.bitrate) {
            stats_.quality_upgrades++;
        } else {
            stats_.quality_downgrades++;
        }
        stats_.total_adaptations++;
        
        LOG_INFO("Quality adaptation for session " << session_id 
                 << ": " << AdaptiveUtils::qualityToString(session.current_quality)
                 << " -> " << AdaptiveUtils::qualityToString(target_quality));
    }
    
    return session.target_quality;
}

QualityLevel AdaptiveBitrateManager::selectOptimalQuality(const ClientSession& session) {
    auto quality_levels = getQualityLevels(session.stream_channel);
    if (quality_levels.empty()) {
        return session.current_quality;
    }
    
    const auto& conditions = session.conditions;
    
    // Find the best quality level that meets current network conditions
    QualityLevel best_quality = quality_levels[0]; // Start with lowest
    
    for (const auto& quality : quality_levels) {
        // Check if this quality level is feasible
        bool feasible = 
            conditions.packet_loss_rate <= quality.max_packet_loss &&
            conditions.rtt_ms <= quality.max_rtt_ms &&
            conditions.bandwidth_bps >= AdaptiveUtils::estimateRequiredBandwidth(quality) * quality.min_bandwidth_factor;
        
        if (feasible && quality.bitrate > best_quality.bitrate) {
            best_quality = quality;
        }
    }
    
    return best_quality;
}

float AdaptiveBitrateManager::calculateStabilityScore(const ClientSession& session) {
    auto now = std::chrono::steady_clock::now();
    auto session_duration = std::chrono::duration_cast<std::chrono::seconds>(
        now - session.session_start).count();
    
    if (session_duration < 10) {
        return 0.5f; // Not enough data
    }
    
    // Calculate stability based on adaptation frequency
    float adaptation_rate = static_cast<float>(session.adaptation_count) / session_duration;
    float stability = std::max(0.0f, 1.0f - adaptation_rate * 10.0f);
    
    // Factor in network condition stability
    const auto& conditions = session.conditions;
    float condition_stability = 1.0f - conditions.congestion_level;
    
    return (stability + condition_stability) / 2.0f;
}

std::vector<QualityLevel> AdaptiveBitrateManager::getQualityLevels(int stream_channel) const {
    std::lock_guard<std::mutex> lock(quality_mutex_);
    
    auto it = quality_levels_.find(stream_channel);
    if (it != quality_levels_.end()) {
        return it->second;
    }
    
    return {};
}

QualityLevel AdaptiveBitrateManager::getNextHigherQuality(int stream_channel, const QualityLevel& current) {
    auto levels = getQualityLevels(stream_channel);
    
    for (size_t i = 0; i < levels.size() - 1; ++i) {
        if (levels[i].bitrate == current.bitrate) {
            return levels[i + 1];
        }
    }
    
    return current; // Already at highest or not found
}

QualityLevel AdaptiveBitrateManager::getNextLowerQuality(int stream_channel, const QualityLevel& current) {
    auto levels = getQualityLevels(stream_channel);
    
    for (size_t i = 1; i < levels.size(); ++i) {
        if (levels[i].bitrate == current.bitrate) {
            return levels[i - 1];
        }
    }
    
    return current; // Already at lowest or not found
}

void AdaptiveBitrateManager::applyQualityChange(unsigned session_id, const QualityLevel& quality) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto it = client_sessions_.find(session_id);
    if (it != client_sessions_.end()) {
        it->second->current_quality = quality;
        it->second->adaptation_in_progress = false;

        LOG_INFO("Applied quality change for session " << session_id
                 << ": " << AdaptiveUtils::qualityToString(quality));
    }
}

AdaptationStats AdaptiveBitrateManager::getAdaptationStats() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    stats_.last_update = std::chrono::steady_clock::now();

    // Calculate average quality level
    if (!client_sessions_.empty()) {
        float total_quality = 0.0f;
        for (const auto& [id, session] : client_sessions_) {
            total_quality += AdaptiveUtils::calculateQualityScore(session->current_quality);
        }
        stats_.average_quality_level = total_quality / client_sessions_.size();
    }

    return stats_;
}

std::string AdaptiveBitrateManager::generateAdaptationReport() {
    auto stats = getAdaptationStats();

    std::ostringstream report;
    report << "\n=== Adaptive Bitrate Report ===\n";
    report << "Active Sessions: " << stats.active_sessions << "\n";
    report << "Total Adaptations: " << stats.total_adaptations << "\n";
    report << "Quality Upgrades: " << stats.quality_upgrades << "\n";
    report << "Quality Downgrades: " << stats.quality_downgrades << "\n";
    report << "Average Quality Level: " << std::fixed << std::setprecision(2)
           << (stats.average_quality_level * 100) << "%\n";

    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (const auto& [id, session] : client_sessions_) {
        report << "\nSession " << id << ":\n";
        report << "  Current Quality: " << AdaptiveUtils::qualityToString(session->current_quality) << "\n";
        report << "  Packet Loss: " << (session->conditions.packet_loss_rate * 100) << "%\n";
        report << "  RTT: " << session->conditions.rtt_ms << "ms\n";
        report << "  Bandwidth: " << (session->conditions.bandwidth_bps / 1000) << "kbps\n";
        report << "  Stability Score: " << (session->stability_score * 100) << "%\n";
        report << "  Adaptations: " << session->adaptation_count << "\n";
    }

    report << "===============================\n";
    return report.str();
}

void AdaptiveBitrateManager::enableAdaptation(bool enabled) {
    adaptation_enabled_.store(enabled);
    LOG_INFO("Adaptive bitrate " << (enabled ? "enabled" : "disabled"));
}

bool AdaptiveBitrateManager::isAdaptationEnabled() const {
    return adaptation_enabled_.load();
}

void AdaptiveBitrateManager::shutdown() {
    adaptation_enabled_.store(false);

    std::lock_guard<std::mutex> lock(sessions_mutex_);
    client_sessions_.clear();
    initialized_.store(false);

    LOG_INFO("Adaptive bitrate manager shutdown complete");
}

// AdaptiveUtils implementation
namespace AdaptiveUtils {

std::string qualityToString(const QualityLevel& quality) {
    std::ostringstream oss;
    oss << quality.name << " (" << quality.bitrate << "kbps, "
        << quality.width << "x" << quality.height << "@" << quality.fps << "fps)";
    return oss.str();
}

float calculateQualityScore(const QualityLevel& quality) {
    // Normalize quality score based on bitrate (0.0 to 1.0)
    // Assuming max bitrate of 8000 kbps for normalization
    return std::min(1.0f, static_cast<float>(quality.bitrate) / 8000.0f);
}

uint64_t estimateRequiredBandwidth(const QualityLevel& quality) {
    // Estimate required bandwidth with overhead
    // Base bandwidth + 20% overhead for protocol and network variations
    return static_cast<uint64_t>(quality.bitrate * 1000 * 1.2);
}

bool isSignificantQualityChange(const QualityLevel& from, const QualityLevel& to) {
    // Consider change significant if bitrate difference is > 15%
    float change_ratio = std::abs(static_cast<float>(to.bitrate - from.bitrate)) / from.bitrate;
    return change_ratio > AdaptiveBitrateManager::QUALITY_CHANGE_HYSTERESIS;
}

}
