#pragma once

#include <memory>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <thread>
#include <functional>
#include "Logger.hpp"

#undef MODULE
#define MODULE "ZEROCOPY_MEMORY"

/**
 * Zero-Copy Memory Analyzer
 * 
 * Comprehensive memory usage analysis and optimization for the zero-copy system.
 * Tracks memory allocations, buffer usage, and provides detailed statistics.
 */

class ZeroCopyMemoryAnalyzer {
public:
    static ZeroCopyMemoryAnalyzer& getInstance() {
        static ZeroCopyMemoryAnalyzer instance;
        return instance;
    }
    
    // Memory allocation tracking
    struct AllocationInfo {
        size_t size;
        std::chrono::steady_clock::time_point timestamp;
        bool is_zero_copy;
        uint32_t buffer_id;
        std::string allocation_type; // "buffer", "nal_unit", "pool", etc.
    };
    
    // Memory usage statistics
    struct MemoryStats {
        // Current usage
        size_t total_allocated_bytes;
        size_t zero_copy_allocated_bytes;
        size_t legacy_allocated_bytes;
        size_t buffer_pool_bytes;
        
        // Peak usage
        size_t peak_total_bytes;
        size_t peak_zero_copy_bytes;
        size_t peak_legacy_bytes;
        
        // Allocation counts
        uint64_t total_allocations;
        uint64_t zero_copy_allocations;
        uint64_t legacy_allocations;
        uint64_t pool_hits;
        uint64_t pool_misses;
        
        // Efficiency metrics
        double memory_savings_ratio; // (legacy_bytes - zero_copy_bytes) / legacy_bytes
        double pool_hit_ratio;       // pool_hits / (pool_hits + pool_misses)
        double avg_allocation_size;
        
        // Time-based metrics
        std::chrono::steady_clock::time_point last_update;
        double allocations_per_second;
        double bytes_per_second;
    };
    
    // Buffer usage analysis
    struct BufferUsageInfo {
        uint32_t buffer_id;
        size_t size;
        size_t ref_count;
        bool is_pooled;
        bool is_sliced;
        std::chrono::steady_clock::time_point created_at;
        std::chrono::steady_clock::time_point last_accessed;
        uint64_t access_count;
        std::string usage_pattern; // "streaming", "temporary", "cached", etc.
    };
    
    // Memory leak detection
    struct LeakInfo {
        uint32_t buffer_id;
        size_t size;
        std::chrono::steady_clock::time_point allocated_at;
        std::chrono::duration<double> age;
        std::string allocation_source;
    };
    
    // Public interface
    void trackAllocation(uint32_t buffer_id, size_t size, bool is_zero_copy, 
                        const std::string& type = "buffer");
    void trackDeallocation(uint32_t buffer_id);
    void trackBufferAccess(uint32_t buffer_id);
    void trackPoolHit(size_t size);
    void trackPoolMiss(size_t size);
    
    MemoryStats getStats() const;
    std::vector<BufferUsageInfo> getBufferUsage() const;
    std::vector<LeakInfo> detectLeaks(std::chrono::seconds max_age = std::chrono::seconds(300)) const;
    
    // Analysis and optimization
    void analyzeMemoryPatterns();
    void optimizeBufferPool();
    void generateMemoryReport() const;
    
    // Configuration
    void setMemoryLimit(size_t max_bytes) { max_memory_bytes_ = max_bytes; }
    void setLeakDetectionEnabled(bool enabled) { leak_detection_enabled_ = enabled; }
    void setDetailedTracking(bool enabled) { detailed_tracking_enabled_ = enabled; }
    
    // Cleanup and maintenance
    void cleanup();
    void reset();

private:
    ZeroCopyMemoryAnalyzer() = default;
    ~ZeroCopyMemoryAnalyzer() = default;
    
    // Prevent copying
    ZeroCopyMemoryAnalyzer(const ZeroCopyMemoryAnalyzer&) = delete;
    ZeroCopyMemoryAnalyzer& operator=(const ZeroCopyMemoryAnalyzer&) = delete;
    
    // Internal tracking
    mutable std::mutex stats_mutex_;
    std::unordered_map<uint32_t, AllocationInfo> allocations_;
    std::unordered_map<uint32_t, BufferUsageInfo> buffer_usage_;
    
    // Statistics
    MemoryStats stats_;
    
    // Configuration
    size_t max_memory_bytes_{100 * 1024 * 1024}; // 100MB default limit
    bool leak_detection_enabled_{true};
    bool detailed_tracking_enabled_{true};
    
    // Internal helpers
    void updateStats();
    void checkMemoryLimit();
    void detectUsagePatterns();
    
    // Constants
    static constexpr size_t CLEANUP_INTERVAL_SECONDS = 60;
    static constexpr size_t MAX_TRACKED_ALLOCATIONS = 10000;
};

/**
 * Memory Usage Monitor
 * 
 * Real-time monitoring of memory usage with alerts and automatic optimization
 */
class ZeroCopyMemoryMonitor {
public:
    static ZeroCopyMemoryMonitor& getInstance() {
        static ZeroCopyMemoryMonitor instance;
        return instance;
    }
    
    // Monitoring configuration
    struct MonitorConfig {
        size_t memory_warning_threshold_mb{50};
        size_t memory_critical_threshold_mb{80};
        double leak_detection_interval_seconds{60.0};
        double stats_update_interval_seconds{5.0};
        bool auto_cleanup_enabled{true};
        bool performance_alerts_enabled{true};
    };
    
    // Alert types
    enum class AlertType {
        MEMORY_WARNING,
        MEMORY_CRITICAL,
        MEMORY_LEAK_DETECTED,
        POOL_INEFFICIENCY,
        ALLOCATION_SPIKE,
        PERFORMANCE_DEGRADATION
    };
    
    // Alert callback
    using AlertCallback = std::function<void(AlertType, const std::string&)>;
    
    void startMonitoring(const MonitorConfig& config);
    void startMonitoring(); // Uses default config
    void stopMonitoring();
    void setAlertCallback(AlertCallback callback) { alert_callback_ = callback; }
    
    // Manual monitoring operations
    void checkMemoryUsage();
    void checkForLeaks();
    void optimizeIfNeeded();
    
    bool isMonitoring() const { return monitoring_enabled_.load(); }
    MonitorConfig getConfig() const { return config_; }

private:
    ZeroCopyMemoryMonitor() = default;
    ~ZeroCopyMemoryMonitor() { stopMonitoring(); }
    
    // Prevent copying
    ZeroCopyMemoryMonitor(const ZeroCopyMemoryMonitor&) = delete;
    ZeroCopyMemoryMonitor& operator=(const ZeroCopyMemoryMonitor&) = delete;
    
    void monitoringLoop();
    void processAlerts();
    void triggerAlert(AlertType type, const std::string& message);
    
    std::atomic<bool> monitoring_enabled_{false};
    std::unique_ptr<std::thread> monitor_thread_;
    MonitorConfig config_;
    AlertCallback alert_callback_;
    
    mutable std::mutex monitor_mutex_;
    std::chrono::steady_clock::time_point last_stats_update_;
    std::chrono::steady_clock::time_point last_leak_check_;
};

/**
 * Memory Optimization Engine
 * 
 * Automatic memory optimization based on usage patterns and performance metrics
 */
class ZeroCopyMemoryOptimizer {
public:
    static ZeroCopyMemoryOptimizer& getInstance() {
        static ZeroCopyMemoryOptimizer instance;
        return instance;
    }
    
    // Optimization strategies
    enum class OptimizationStrategy {
        CONSERVATIVE,  // Minimal changes, focus on stability
        BALANCED,      // Balance between performance and memory usage
        AGGRESSIVE     // Maximum performance, higher memory usage
    };
    
    // Optimization recommendations
    struct OptimizationRecommendation {
        std::string category;
        std::string description;
        double expected_memory_savings_mb;
        double expected_performance_impact;
        bool auto_applicable;
        std::string implementation_notes;
    };
    
    void analyzeAndOptimize(OptimizationStrategy strategy = OptimizationStrategy::BALANCED);
    std::vector<OptimizationRecommendation> getRecommendations() const;
    void applyRecommendations(bool auto_only = true);
    
    // Specific optimizations
    void optimizeBufferPoolSizes();
    void optimizeAllocationPatterns();
    void optimizeMemoryLayout();
    void cleanupUnusedBuffers();

private:
    ZeroCopyMemoryOptimizer() = default;
    ~ZeroCopyMemoryOptimizer() = default;
    
    // Prevent copying
    ZeroCopyMemoryOptimizer(const ZeroCopyMemoryOptimizer&) = delete;
    ZeroCopyMemoryOptimizer& operator=(const ZeroCopyMemoryOptimizer&) = delete;
    
    std::vector<OptimizationRecommendation> recommendations_;
    mutable std::mutex optimizer_mutex_;
    
    // Analysis helpers
    void analyzeBufferPoolEfficiency();
    void analyzeAllocationPatterns();
    void analyzeMemoryFragmentation();
    void generateRecommendations(OptimizationStrategy strategy);
};

// Utility macros for memory tracking
#define ZEROCOPY_TRACK_ALLOCATION(buffer_id, size, is_zero_copy, type) \
    ZeroCopyMemoryAnalyzer::getInstance().trackAllocation(buffer_id, size, is_zero_copy, type)

#define ZEROCOPY_TRACK_DEALLOCATION(buffer_id) \
    ZeroCopyMemoryAnalyzer::getInstance().trackDeallocation(buffer_id)

#define ZEROCOPY_TRACK_ACCESS(buffer_id) \
    ZeroCopyMemoryAnalyzer::getInstance().trackBufferAccess(buffer_id)

#define ZEROCOPY_TRACK_POOL_HIT(size) \
    ZeroCopyMemoryAnalyzer::getInstance().trackPoolHit(size)

#define ZEROCOPY_TRACK_POOL_MISS(size) \
    ZeroCopyMemoryAnalyzer::getInstance().trackPoolMiss(size)
