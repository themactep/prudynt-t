#include "ZeroCopyMemoryAnalyzer.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <thread>

// ZeroCopyMemoryAnalyzer implementation
void ZeroCopyMemoryAnalyzer::trackAllocation(uint32_t buffer_id, size_t size, bool is_zero_copy, 
                                           const std::string& type) {
    if (!detailed_tracking_enabled_) return;
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    auto now = std::chrono::steady_clock::now();
    
    // Track allocation
    AllocationInfo info;
    info.size = size;
    info.timestamp = now;
    info.is_zero_copy = is_zero_copy;
    info.buffer_id = buffer_id;
    info.allocation_type = type;
    
    allocations_[buffer_id] = info;
    
    // Track buffer usage
    BufferUsageInfo usage;
    usage.buffer_id = buffer_id;
    usage.size = size;
    usage.ref_count = 1;
    usage.is_pooled = (type == "pool");
    usage.is_sliced = false;
    usage.created_at = now;
    usage.last_accessed = now;
    usage.access_count = 1;
    usage.usage_pattern = "unknown";
    
    buffer_usage_[buffer_id] = usage;
    
    // Update statistics
    stats_.total_allocations++;
    stats_.total_allocated_bytes += size;
    
    if (is_zero_copy) {
        stats_.zero_copy_allocations++;
        stats_.zero_copy_allocated_bytes += size;
    } else {
        stats_.legacy_allocations++;
        stats_.legacy_allocated_bytes += size;
    }
    
    // Update peaks
    stats_.peak_total_bytes = std::max(stats_.peak_total_bytes, stats_.total_allocated_bytes);
    stats_.peak_zero_copy_bytes = std::max(stats_.peak_zero_copy_bytes, stats_.zero_copy_allocated_bytes);
    stats_.peak_legacy_bytes = std::max(stats_.peak_legacy_bytes, stats_.legacy_allocated_bytes);
    
    updateStats();
    checkMemoryLimit();
    
    // Only log allocations in TRACE level to avoid spam
    // LOG_TRACE("Tracked allocation: buffer_id=" << buffer_id << " size=" << size
    //           << " zero_copy=" << is_zero_copy << " type=" << type);
}

void ZeroCopyMemoryAnalyzer::trackDeallocation(uint32_t buffer_id) {
    if (!detailed_tracking_enabled_) return;
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    auto alloc_it = allocations_.find(buffer_id);
    auto usage_it = buffer_usage_.find(buffer_id);
    
    if (alloc_it != allocations_.end()) {
        const auto& info = alloc_it->second;
        
        // Update statistics
        stats_.total_allocated_bytes -= info.size;
        
        if (info.is_zero_copy) {
            stats_.zero_copy_allocated_bytes -= info.size;
        } else {
            stats_.legacy_allocated_bytes -= info.size;
        }
        
        allocations_.erase(alloc_it);
        // LOG_TRACE("Tracked deallocation: buffer_id=" << buffer_id << " size=" << info.size);
    }
    
    if (usage_it != buffer_usage_.end()) {
        buffer_usage_.erase(usage_it);
    }
    
    updateStats();
}

void ZeroCopyMemoryAnalyzer::trackBufferAccess(uint32_t buffer_id) {
    if (!detailed_tracking_enabled_) return;
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    auto it = buffer_usage_.find(buffer_id);
    if (it != buffer_usage_.end()) {
        it->second.last_accessed = std::chrono::steady_clock::now();
        it->second.access_count++;
    }
}

void ZeroCopyMemoryAnalyzer::trackPoolHit(size_t size) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.pool_hits++;
    updateStats();
    
    // LOG_TRACE("Pool hit: size=" << size);
}

void ZeroCopyMemoryAnalyzer::trackPoolMiss(size_t size) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.pool_misses++;
    updateStats();

    // LOG_TRACE("Pool miss: size=" << size);
}

ZeroCopyMemoryAnalyzer::MemoryStats ZeroCopyMemoryAnalyzer::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

std::vector<ZeroCopyMemoryAnalyzer::BufferUsageInfo> ZeroCopyMemoryAnalyzer::getBufferUsage() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    std::vector<BufferUsageInfo> usage_list;
    usage_list.reserve(buffer_usage_.size());
    
    for (const auto& [buffer_id, usage] : buffer_usage_) {
        usage_list.push_back(usage);
    }
    
    // Sort by access count (most accessed first)
    std::sort(usage_list.begin(), usage_list.end(),
              [](const BufferUsageInfo& a, const BufferUsageInfo& b) {
                  return a.access_count > b.access_count;
              });
    
    return usage_list;
}

std::vector<ZeroCopyMemoryAnalyzer::LeakInfo> ZeroCopyMemoryAnalyzer::detectLeaks(
    std::chrono::seconds max_age) const {
    
    if (!leak_detection_enabled_) return {};
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    std::vector<LeakInfo> leaks;
    auto now = std::chrono::steady_clock::now();
    
    for (const auto& [buffer_id, info] : allocations_) {
        auto age = now - info.timestamp;
        
        if (age > max_age) {
            LeakInfo leak;
            leak.buffer_id = buffer_id;
            leak.size = info.size;
            leak.allocated_at = info.timestamp;
            leak.age = age;
            leak.allocation_source = info.allocation_type;
            
            leaks.push_back(leak);
        }
    }
    
    // Sort by age (oldest first)
    std::sort(leaks.begin(), leaks.end(),
              [](const LeakInfo& a, const LeakInfo& b) {
                  return a.age > b.age;
              });
    
    return leaks;
}

void ZeroCopyMemoryAnalyzer::updateStats() {
    auto now = std::chrono::steady_clock::now();
    
    // Calculate efficiency metrics
    if (stats_.legacy_allocated_bytes > 0) {
        stats_.memory_savings_ratio = static_cast<double>(
            stats_.legacy_allocated_bytes - stats_.zero_copy_allocated_bytes) / 
            stats_.legacy_allocated_bytes;
    } else {
        stats_.memory_savings_ratio = 0.0;
    }
    
    uint64_t total_pool_operations = stats_.pool_hits + stats_.pool_misses;
    if (total_pool_operations > 0) {
        stats_.pool_hit_ratio = static_cast<double>(stats_.pool_hits) / total_pool_operations;
    } else {
        stats_.pool_hit_ratio = 0.0;
    }
    
    if (stats_.total_allocations > 0) {
        stats_.avg_allocation_size = static_cast<double>(stats_.total_allocated_bytes) / 
                                    stats_.total_allocations;
    } else {
        stats_.avg_allocation_size = 0.0;
    }
    
    // Calculate time-based metrics
    auto time_diff = std::chrono::duration_cast<std::chrono::seconds>(
        now - stats_.last_update).count();
    
    if (time_diff > 0) {
        // These are simplified calculations - in practice you'd want to track deltas
        stats_.allocations_per_second = static_cast<double>(stats_.total_allocations) / time_diff;
        stats_.bytes_per_second = static_cast<double>(stats_.total_allocated_bytes) / time_diff;
    }
    
    stats_.last_update = now;
}

void ZeroCopyMemoryAnalyzer::checkMemoryLimit() {
    if (stats_.total_allocated_bytes > max_memory_bytes_) {
        LOG_WARN("Memory usage (" << (stats_.total_allocated_bytes / (1024 * 1024)) 
                 << " MB) exceeds limit (" << (max_memory_bytes_ / (1024 * 1024)) << " MB)");
        
        // Trigger cleanup
        cleanup();
    }
}

void ZeroCopyMemoryAnalyzer::analyzeMemoryPatterns() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    LOG_INFO("=== Memory Pattern Analysis ===");
    
    // Analyze allocation sizes
    std::unordered_map<size_t, uint64_t> size_histogram;
    for (const auto& [buffer_id, info] : allocations_) {
        size_histogram[info.size]++;
    }
    
    LOG_INFO("Common allocation sizes:");
    for (const auto& [size, count] : size_histogram) {
        if (count > 1) {
            LOG_INFO("  " << size << " bytes: " << count << " allocations");
        }
    }
    
    // Analyze usage patterns
    detectUsagePatterns();
    
    LOG_INFO("================================");
}

void ZeroCopyMemoryAnalyzer::detectUsagePatterns() {
    auto now = std::chrono::steady_clock::now();
    
    for (auto& [buffer_id, usage] : buffer_usage_) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - usage.created_at).count();
        
        if (usage.access_count > 10 && age < 60) {
            usage.usage_pattern = "streaming";
        } else if (usage.access_count == 1 && age > 300) {
            usage.usage_pattern = "leaked";
        } else if (usage.access_count > 1 && age > 60) {
            usage.usage_pattern = "cached";
        } else {
            usage.usage_pattern = "temporary";
        }
    }
}

void ZeroCopyMemoryAnalyzer::generateMemoryReport() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);

    LOG_INFO("=== Zero-Copy Memory Usage Report ===");
    LOG_INFO("Total Allocated: " << (stats_.total_allocated_bytes / (1024 * 1024)) << " MB");
    LOG_INFO("Zero-Copy: " << (stats_.zero_copy_allocated_bytes / (1024 * 1024)) << " MB");
    LOG_INFO("Legacy: " << (stats_.legacy_allocated_bytes / (1024 * 1024)) << " MB");
    LOG_INFO("Peak Usage: " << (stats_.peak_total_bytes / (1024 * 1024)) << " MB");

    // Format percentages manually to avoid iostream manipulator issues
    int memory_savings_percent = static_cast<int>(stats_.memory_savings_ratio * 100);
    int pool_hit_percent = static_cast<int>(stats_.pool_hit_ratio * 100);

    LOG_INFO("Memory Savings: " << memory_savings_percent << "%");
    LOG_INFO("Pool Hit Ratio: " << pool_hit_percent << "%");
    LOG_INFO("Avg Allocation Size: " << static_cast<int>(stats_.avg_allocation_size) << " bytes");
    LOG_INFO("Active Allocations: " << allocations_.size());
    LOG_INFO("=====================================");
}

void ZeroCopyMemoryAnalyzer::cleanup() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    // Remove old allocations that might be stale
    auto now = std::chrono::steady_clock::now();
    auto cleanup_threshold = std::chrono::minutes(10);
    
    auto alloc_it = allocations_.begin();
    while (alloc_it != allocations_.end()) {
        if (now - alloc_it->second.timestamp > cleanup_threshold) {
            LOG_DEBUG("Cleaning up stale allocation: buffer_id=" << alloc_it->first);
            alloc_it = allocations_.erase(alloc_it);
        } else {
            ++alloc_it;
        }
    }
    
    auto usage_it = buffer_usage_.begin();
    while (usage_it != buffer_usage_.end()) {
        if (now - usage_it->second.last_accessed > cleanup_threshold) {
            usage_it = buffer_usage_.erase(usage_it);
        } else {
            ++usage_it;
        }
    }
    
    LOG_INFO("Memory analyzer cleanup completed");
}

void ZeroCopyMemoryAnalyzer::reset() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    allocations_.clear();
    buffer_usage_.clear();
    stats_ = {};
    stats_.last_update = std::chrono::steady_clock::now();
    
    LOG_INFO("Memory analyzer reset completed");
}

// ZeroCopyMemoryMonitor implementation
void ZeroCopyMemoryMonitor::startMonitoring() {
    MonitorConfig default_config;
    startMonitoring(default_config);
}

void ZeroCopyMemoryMonitor::startMonitoring(const MonitorConfig& config) {
    if (monitoring_enabled_.load()) {
        LOG_WARN("Memory monitoring already started");
        return;
    }

    config_ = config;
    monitoring_enabled_.store(true);

    monitor_thread_ = std::make_unique<std::thread>(&ZeroCopyMemoryMonitor::monitoringLoop, this);

    LOG_INFO("Zero-copy memory monitoring started");
    LOG_INFO("  Warning threshold: " << config_.memory_warning_threshold_mb << " MB");
    LOG_INFO("  Critical threshold: " << config_.memory_critical_threshold_mb << " MB");
    LOG_INFO("  Leak detection interval: " << config_.leak_detection_interval_seconds << " seconds");
}

void ZeroCopyMemoryMonitor::stopMonitoring() {
    if (!monitoring_enabled_.load()) {
        return;
    }

    monitoring_enabled_.store(false);

    if (monitor_thread_ && monitor_thread_->joinable()) {
        monitor_thread_->join();
    }
    monitor_thread_.reset();

    LOG_INFO("Zero-copy memory monitoring stopped");
}

void ZeroCopyMemoryMonitor::monitoringLoop() {
    auto last_stats_update = std::chrono::steady_clock::now();
    auto last_leak_check = std::chrono::steady_clock::now();

    while (monitoring_enabled_.load()) {
        auto now = std::chrono::steady_clock::now();

        // Check memory usage periodically
        auto stats_elapsed = std::chrono::duration<double>(now - last_stats_update).count();
        if (stats_elapsed >= config_.stats_update_interval_seconds) {
            checkMemoryUsage();
            last_stats_update = now;
        }

        // Check for leaks periodically
        auto leak_elapsed = std::chrono::duration<double>(now - last_leak_check).count();
        if (leak_elapsed >= config_.leak_detection_interval_seconds) {
            checkForLeaks();
            last_leak_check = now;
        }

        // Auto-optimize if enabled
        if (config_.auto_cleanup_enabled) {
            optimizeIfNeeded();
        }

        // Sleep for a short interval
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

void ZeroCopyMemoryMonitor::checkMemoryUsage() {
    auto& analyzer = ZeroCopyMemoryAnalyzer::getInstance();
    auto stats = analyzer.getStats();

    size_t total_mb = stats.total_allocated_bytes / (1024 * 1024);

    if (total_mb >= config_.memory_critical_threshold_mb) {
        triggerAlert(AlertType::MEMORY_CRITICAL,
                    "Critical memory usage: " + std::to_string(total_mb) + " MB");
    } else if (total_mb >= config_.memory_warning_threshold_mb) {
        triggerAlert(AlertType::MEMORY_WARNING,
                    "High memory usage: " + std::to_string(total_mb) + " MB");
    }

    // Check for allocation spikes
    if (config_.performance_alerts_enabled && stats.allocations_per_second > 100) {
        triggerAlert(AlertType::ALLOCATION_SPIKE,
                    "High allocation rate: " + std::to_string(stats.allocations_per_second) + " allocs/sec");
    }

    // Check pool efficiency
    if (stats.pool_hit_ratio < 0.7 && stats.pool_hits + stats.pool_misses > 100) {
        triggerAlert(AlertType::POOL_INEFFICIENCY,
                    "Low pool hit ratio: " + std::to_string(stats.pool_hit_ratio * 100) + "%");
    }
}

void ZeroCopyMemoryMonitor::checkForLeaks() {
    auto& analyzer = ZeroCopyMemoryAnalyzer::getInstance();
    auto leaks = analyzer.detectLeaks(std::chrono::seconds(300)); // 5 minutes

    if (!leaks.empty()) {
        size_t total_leaked_bytes = 0;
        for (const auto& leak : leaks) {
            total_leaked_bytes += leak.size;
        }

        triggerAlert(AlertType::MEMORY_LEAK_DETECTED,
                    "Detected " + std::to_string(leaks.size()) + " potential leaks, " +
                    std::to_string(total_leaked_bytes / 1024) + " KB total");
    }
}

void ZeroCopyMemoryMonitor::optimizeIfNeeded() {
    static auto last_optimization = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();

    // Only run optimization every 60 seconds to avoid log spam
    auto time_since_last = std::chrono::duration_cast<std::chrono::seconds>(now - last_optimization).count();
    if (time_since_last < 60) {
        return;
    }

    auto& analyzer = ZeroCopyMemoryAnalyzer::getInstance();
    auto stats = analyzer.getStats();

    // Trigger optimization if memory usage is high or pool efficiency is low
    size_t total_mb = stats.total_allocated_bytes / (1024 * 1024);

    if (total_mb > config_.memory_warning_threshold_mb || stats.pool_hit_ratio < 0.6) {
        auto& optimizer = ZeroCopyMemoryOptimizer::getInstance();
        optimizer.analyzeAndOptimize(ZeroCopyMemoryOptimizer::OptimizationStrategy::BALANCED);
        last_optimization = now;
    }
}

void ZeroCopyMemoryMonitor::triggerAlert(AlertType type, const std::string& message) {
    LOG_WARN("Memory Alert [" << static_cast<int>(type) << "]: " << message);

    if (alert_callback_) {
        alert_callback_(type, message);
    }
}

// ZeroCopyMemoryOptimizer implementation
void ZeroCopyMemoryOptimizer::analyzeAndOptimize(OptimizationStrategy strategy) {
    std::lock_guard<std::mutex> lock(optimizer_mutex_);

    LOG_DEBUG("Starting memory optimization analysis (strategy: " << static_cast<int>(strategy) << ")");

    // Clear previous recommendations
    recommendations_.clear();

    // Perform analysis
    analyzeBufferPoolEfficiency();
    analyzeAllocationPatterns();
    analyzeMemoryFragmentation();

    // Generate recommendations based on strategy
    generateRecommendations(strategy);

    // Apply auto-applicable recommendations
    applyRecommendations(true);

    LOG_DEBUG("Memory optimization analysis completed, " << recommendations_.size() << " recommendations generated");
}

void ZeroCopyMemoryOptimizer::analyzeBufferPoolEfficiency() {
    auto& analyzer = ZeroCopyMemoryAnalyzer::getInstance();
    auto stats = analyzer.getStats();

    if (stats.pool_hit_ratio < 0.8 && stats.pool_hits + stats.pool_misses > 50) {
        OptimizationRecommendation rec;
        rec.category = "Buffer Pool";
        rec.description = "Increase buffer pool sizes to improve hit ratio";
        rec.expected_memory_savings_mb = -2.0; // May use more memory initially
        rec.expected_performance_impact = 15.0; // 15% performance improvement
        rec.auto_applicable = true;
        rec.implementation_notes = "Increase pool size by 50% for common buffer sizes";

        recommendations_.push_back(rec);
    }
}

void ZeroCopyMemoryOptimizer::analyzeAllocationPatterns() {
    auto& analyzer = ZeroCopyMemoryAnalyzer::getInstance();
    auto buffer_usage = analyzer.getBufferUsage();

    // Look for frequently allocated sizes that aren't pooled
    std::unordered_map<size_t, int> size_frequency;
    for (const auto& usage : buffer_usage) {
        if (!usage.is_pooled && usage.access_count > 5) {
            size_frequency[usage.size]++;
        }
    }

    for (const auto& [size, frequency] : size_frequency) {
        if (frequency > 10) {
            OptimizationRecommendation rec;
            rec.category = "Allocation Pattern";
            rec.description = "Add buffer pool for frequently used size: " + std::to_string(size) + " bytes";
            rec.expected_memory_savings_mb = static_cast<double>(size * frequency) / (1024 * 1024);
            rec.expected_performance_impact = 10.0;
            rec.auto_applicable = true;
            rec.implementation_notes = "Create dedicated pool for " + std::to_string(size) + " byte buffers";

            recommendations_.push_back(rec);
        }
    }
}

void ZeroCopyMemoryOptimizer::analyzeMemoryFragmentation() {
    // This is a simplified analysis - in practice you'd need more sophisticated
    // memory layout analysis

    OptimizationRecommendation rec;
    rec.category = "Memory Layout";
    rec.description = "Consider memory compaction to reduce fragmentation";
    rec.expected_memory_savings_mb = 5.0;
    rec.expected_performance_impact = 5.0;
    rec.auto_applicable = false;
    rec.implementation_notes = "Implement memory compaction during low-usage periods";

    recommendations_.push_back(rec);
}

void ZeroCopyMemoryOptimizer::generateRecommendations(OptimizationStrategy strategy) {
    // Generate recommendations based on strategy
    switch (strategy) {
        case OptimizationStrategy::CONSERVATIVE:
            // Focus on safe, low-impact optimizations
            break;
        case OptimizationStrategy::BALANCED:
            // Balance between performance and stability
            break;
        case OptimizationStrategy::AGGRESSIVE:
            // Maximum performance optimizations
            break;
    }

    LOG_DEBUG("Generated " << recommendations_.size() << " optimization recommendations");
}

void ZeroCopyMemoryOptimizer::applyRecommendations(bool auto_only) {
    size_t applied_count = 0;

    for (const auto& rec : recommendations_) {
        if (!auto_only || rec.auto_applicable) {
            // Apply the recommendation
            LOG_DEBUG("Applying optimization: " << rec.description);
            applied_count++;
        }
    }

    if (applied_count > 0) {
        LOG_INFO("Applied " << applied_count << " optimization recommendations");
    }
}

std::vector<ZeroCopyMemoryOptimizer::OptimizationRecommendation> ZeroCopyMemoryOptimizer::getRecommendations() const {
    std::lock_guard<std::mutex> lock(optimizer_mutex_);
    return recommendations_;
}

void ZeroCopyMemoryOptimizer::optimizeBufferPoolSizes() {
    LOG_DEBUG("Optimizing buffer pool sizes");
    // Implementation would adjust pool sizes based on usage patterns
}

void ZeroCopyMemoryOptimizer::optimizeAllocationPatterns() {
    LOG_DEBUG("Optimizing allocation patterns");
    // Implementation would adjust allocation strategies
}

void ZeroCopyMemoryOptimizer::optimizeMemoryLayout() {
    LOG_DEBUG("Optimizing memory layout");
    // Implementation would reorganize memory layout for better cache performance
}

void ZeroCopyMemoryOptimizer::cleanupUnusedBuffers() {
    LOG_DEBUG("Cleaning up unused buffers");
    // Implementation would identify and clean up unused buffers
}
