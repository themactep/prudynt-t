#include "MemoryMonitor.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <cstring>

bool MemoryMonitor::initialize(bool enable_allocation_tracking) {
    std::lock_guard<std::mutex> lock(allocations_mutex_);
    
    if (initialized_.load()) {
        return true;
    }
    
    LOG_INFO("Initializing memory monitor");
    
    allocation_tracking_enabled_.store(enable_allocation_tracking);
    
    // Initialize statistics
    stats_ = {};
    stats_.last_update = std::chrono::steady_clock::now();
    
    // Take initial snapshot
    SystemMemorySnapshot initial_snapshot;
    if (readSystemMemory(initial_snapshot)) {
        std::lock_guard<std::mutex> history_lock(history_mutex_);
        memory_history_.push_back(initial_snapshot);
    }
    
    last_analysis_ = std::chrono::steady_clock::now();
    initialized_.store(true);
    
    LOG_INFO("Memory monitor initialized (allocation tracking: " 
             << (enable_allocation_tracking ? "enabled" : "disabled") << ")");
    return true;
}

void MemoryMonitor::startMonitoring() {
    if (!initialized_.load()) {
        LOG_ERROR("Memory monitor not initialized");
        return;
    }
    
    if (monitoring_enabled_.load()) {
        LOG_WARN("Memory monitoring already started");
        return;
    }
    
    should_stop_.store(false);
    monitoring_enabled_.store(true);
    monitoring_thread_ = std::make_unique<std::thread>(&MemoryMonitor::monitoringLoop, this);
    
    LOG_INFO("Memory monitoring started");
}

void MemoryMonitor::stopMonitoring() {
    if (!monitoring_enabled_.load()) {
        return;
    }
    
    should_stop_.store(true);
    monitoring_enabled_.store(false);
    
    if (monitoring_thread_ && monitoring_thread_->joinable()) {
        monitoring_thread_->join();
    }
    monitoring_thread_.reset();
    
    LOG_INFO("Memory monitoring stopped");
}

void MemoryMonitor::monitoringLoop() {
    LOG_DEBUG("Memory monitoring loop started");
    
    while (!should_stop_.load()) {
        try {
            // Take memory snapshot
            SystemMemorySnapshot snapshot;
            if (readSystemMemory(snapshot)) {
                std::lock_guard<std::mutex> lock(history_mutex_);
                memory_history_.push_back(snapshot);
                
                // Limit history size
                if (memory_history_.size() > max_history_size_) {
                    memory_history_.erase(memory_history_.begin());
                }
            }
            
            // Periodic analysis
            auto now = std::chrono::steady_clock::now();
            if (now - last_analysis_ > std::chrono::minutes(5)) {
                analyzeMemoryTrends();
                if (allocation_tracking_enabled_.load()) {
                    detectMemoryLeaks();
                }
                last_analysis_ = now;
                analysis_count_++;
            }
            
            // Sleep until next snapshot
            std::this_thread::sleep_for(snapshot_interval_);
            
        } catch (const std::exception& e) {
            LOG_ERROR("Error in memory monitoring loop: " << e.what());
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }
    
    LOG_DEBUG("Memory monitoring loop stopped");
}

bool MemoryMonitor::readSystemMemory(SystemMemorySnapshot& snapshot) {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) {
        return false;
    }
    
    snapshot.timestamp = std::chrono::steady_clock::now();
    
    std::string line;
    while (std::getline(meminfo, line)) {
        std::istringstream iss(line);
        std::string key;
        size_t value;
        std::string unit;
        
        if (iss >> key >> value >> unit) {
            // Convert kB to bytes
            value *= 1024;
            
            if (key == "MemTotal:") {
                snapshot.total_memory = value;
            } else if (key == "MemFree:") {
                snapshot.free_memory = value;
            } else if (key == "MemAvailable:") {
                snapshot.available_memory = value;
            } else if (key == "Buffers:") {
                snapshot.buffers = value;
            } else if (key == "Cached:") {
                snapshot.cached = value;
            }
        }
    }
    
    // If MemAvailable not available, estimate it
    if (snapshot.available_memory == 0) {
        snapshot.available_memory = snapshot.free_memory + snapshot.buffers + snapshot.cached;
    }
    
    // Read process memory
    readProcessMemory(snapshot);
    
    // Read CPU usage
    snapshot.cpu_usage = readCpuUsage();
    
    return true;
}

bool MemoryMonitor::readProcessMemory(SystemMemorySnapshot& snapshot) {
    std::ifstream status("/proc/self/status");
    if (!status.is_open()) {
        return false;
    }
    
    std::string line;
    while (std::getline(status, line)) {
        std::istringstream iss(line);
        std::string key;
        size_t value;
        std::string unit;
        
        if (iss >> key >> value >> unit) {
            // Convert kB to bytes
            value *= 1024;
            
            if (key == "VmRSS:") {
                snapshot.process_rss = value;
            } else if (key == "VmSize:") {
                snapshot.process_vms = value;
            }
        }
    }
    
    return true;
}

float MemoryMonitor::readCpuUsage() {
    static long long last_total = 0, last_idle = 0;
    
    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) {
        return 0.0f;
    }
    
    std::string line;
    std::getline(stat, line);
    
    std::istringstream iss(line);
    std::string cpu;
    long long user, nice, system, idle, iowait, irq, softirq, steal;
    
    if (!(iss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal)) {
        return 0.0f;
    }
    
    long long total = user + nice + system + idle + iowait + irq + softirq + steal;
    long long total_diff = total - last_total;
    long long idle_diff = idle - last_idle;
    
    float cpu_usage = 0.0f;
    if (total_diff > 0) {
        cpu_usage = 100.0f * (total_diff - idle_diff) / total_diff;
    }
    
    last_total = total;
    last_idle = idle;
    
    return cpu_usage;
}

void MemoryMonitor::trackAllocation(void* ptr, size_t size, const std::string& source) {
    if (!allocation_tracking_enabled_.load() || !ptr) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(allocations_mutex_);
    
    // Limit tracked allocations to prevent excessive memory usage
    if (tracked_allocations_.size() >= MAX_TRACKED_ALLOCATIONS) {
        LOG_WARN("Maximum tracked allocations reached, skipping tracking");
        return;
    }
    
    MemoryAllocation allocation;
    allocation.address = ptr;
    allocation.size = size;
    allocation.allocated_time = std::chrono::steady_clock::now();
    allocation.source_info = source;
    allocation.thread_id = std::this_thread::get_id();
    
    tracked_allocations_[ptr] = allocation;
    
    // Update statistics
    stats_.current_usage += size;
    stats_.total_allocated += size;
    stats_.allocation_count++;
    
    if (stats_.current_usage > stats_.peak_usage) {
        stats_.peak_usage = stats_.current_usage;
    }
    
    stats_.last_update = std::chrono::steady_clock::now();
}

void MemoryMonitor::trackDeallocation(void* ptr) {
    if (!allocation_tracking_enabled_.load() || !ptr) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(allocations_mutex_);
    
    auto it = tracked_allocations_.find(ptr);
    if (it != tracked_allocations_.end()) {
        stats_.current_usage -= it->second.size;
        stats_.total_freed += it->second.size;
        stats_.free_count++;
        tracked_allocations_.erase(it);
    }
    
    stats_.last_update = std::chrono::steady_clock::now();
}

MemoryStats MemoryMonitor::getMemoryStats() {
    std::lock_guard<std::mutex> lock(allocations_mutex_);
    auto stats = stats_;
    stats.last_update = std::chrono::steady_clock::now();
    return stats;
}

SystemMemorySnapshot MemoryMonitor::getCurrentSnapshot() {
    SystemMemorySnapshot snapshot;
    readSystemMemory(snapshot);
    return snapshot;
}

std::vector<SystemMemorySnapshot> MemoryMonitor::getHistoricalSnapshots(size_t count) {
    std::lock_guard<std::mutex> lock(history_mutex_);
    
    if (count >= memory_history_.size()) {
        return memory_history_;
    }
    
    return std::vector<SystemMemorySnapshot>(
        memory_history_.end() - count, memory_history_.end());
}

void MemoryMonitor::analyzeMemoryTrends() {
    auto snapshots = getHistoricalSnapshots(20); // Last 20 snapshots
    if (snapshots.size() < 5) {
        return; // Not enough data
    }
    
    auto pattern = MemoryUtils::detectPattern(snapshots);
    float growth_rate = MemoryUtils::calculateGrowthRate(snapshots);
    
    if (pattern == MemoryUtils::MemoryPattern::GROWING && growth_rate > 1024) { // 1KB/s growth
        LOG_WARN("Memory usage growing at " << MemoryUtils::formatBytes(growth_rate) << "/s - "
                 << MemoryUtils::getPatternDescription(pattern));
    }
    
    // Check for high memory pressure
    if (isMemoryPressureHigh()) {
        LOG_WARN("High memory pressure detected");
        logMemoryStatus();
    }
}

bool MemoryMonitor::isMemoryPressureHigh() {
    auto snapshot = getCurrentSnapshot();
    float pressure = 1.0f - (static_cast<float>(snapshot.available_memory) / snapshot.total_memory);
    return pressure > HIGH_PRESSURE_THRESHOLD;
}

LeakReport MemoryMonitor::detectLeaks() {
    std::lock_guard<std::mutex> lock(allocations_mutex_);

    LeakReport report;
    report.report_time = std::chrono::steady_clock::now();
    report.total_leaked_bytes = 0;

    auto now = std::chrono::steady_clock::now();

    for (const auto& [ptr, allocation] : tracked_allocations_) {
        auto age = now - allocation.allocated_time;
        if (age > leak_threshold_ && allocation.size >= MIN_LEAK_SIZE) {
            report.suspected_leaks.push_back(allocation);
            report.total_leaked_bytes += allocation.size;
        }
    }

    stats_.leak_count = report.suspected_leaks.size();
    report.analysis = analyzeLeakPattern(report.suspected_leaks);

    if (!report.suspected_leaks.empty()) {
        LOG_WARN("Detected " << report.suspected_leaks.size() << " potential memory leaks ("
                 << MemoryUtils::formatBytes(report.total_leaked_bytes) << " total)");
    }

    return report;
}

std::string MemoryMonitor::analyzeLeakPattern(const std::vector<MemoryAllocation>& leaks) {
    if (leaks.empty()) {
        return "No leaks detected";
    }

    std::unordered_map<std::string, size_t> source_counts;
    std::unordered_map<size_t, size_t> size_counts;

    for (const auto& leak : leaks) {
        source_counts[leak.source_info]++;
        size_counts[leak.size]++;
    }

    std::ostringstream analysis;
    analysis << "Leak analysis: ";

    // Find most common source
    auto max_source = std::max_element(source_counts.begin(), source_counts.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    if (max_source != source_counts.end() && max_source->second > 1) {
        analysis << "Most common source: " << max_source->first
                 << " (" << max_source->second << " leaks). ";
    }

    // Find most common size
    auto max_size = std::max_element(size_counts.begin(), size_counts.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    if (max_size != size_counts.end() && max_size->second > 1) {
        analysis << "Most common size: " << MemoryUtils::formatBytes(max_size->first)
                 << " (" << max_size->second << " allocations).";
    }

    return analysis.str();
}

std::string MemoryMonitor::generateMemoryReport() {
    auto stats = getMemoryStats();
    auto snapshot = getCurrentSnapshot();
    auto pattern = MemoryUtils::detectPattern(getHistoricalSnapshots(20));

    std::ostringstream report;
    report << "\n=== Memory Monitor Report ===\n";
    report << "System Memory:\n";
    report << "  Total: " << MemoryUtils::formatBytes(snapshot.total_memory) << "\n";
    report << "  Available: " << MemoryUtils::formatBytes(snapshot.available_memory) << "\n";
    report << "  Process RSS: " << MemoryUtils::formatBytes(snapshot.process_rss) << "\n";
    report << "  Process VMS: " << MemoryUtils::formatBytes(snapshot.process_vms) << "\n";
    report << "  CPU Usage: " << std::fixed << std::setprecision(1) << snapshot.cpu_usage << "%\n";

    if (allocation_tracking_enabled_.load()) {
        report << "\nAllocation Tracking:\n";
        report << "  Current Usage: " << MemoryUtils::formatBytes(stats.current_usage) << "\n";
        report << "  Peak Usage: " << MemoryUtils::formatBytes(stats.peak_usage) << "\n";
        report << "  Total Allocated: " << MemoryUtils::formatBytes(stats.total_allocated) << "\n";
        report << "  Total Freed: " << MemoryUtils::formatBytes(stats.total_freed) << "\n";
        report << "  Allocations: " << stats.allocation_count << "\n";
        report << "  Deallocations: " << stats.free_count << "\n";
        report << "  Potential Leaks: " << stats.leak_count << "\n";
    }

    report << "\nMemory Pattern: " << MemoryUtils::getPatternDescription(pattern) << "\n";
    report << "Growth Rate: " << MemoryUtils::formatBytes(getMemoryGrowthRate()) << "/s\n";
    report << "Analysis Count: " << analysis_count_ << "\n";
    report << "==============================\n";

    return report.str();
}

void MemoryMonitor::logMemoryStatus() {
    LOG_INFO(generateMemoryReport());
}

float MemoryMonitor::getMemoryGrowthRate() {
    return MemoryUtils::calculateGrowthRate(getHistoricalSnapshots(10));
}

void MemoryMonitor::shutdown() {
    stopMonitoring();

    std::lock_guard<std::mutex> lock(allocations_mutex_);
    tracked_allocations_.clear();
    memory_history_.clear();
    initialized_.store(false);

    LOG_INFO("Memory monitor shutdown complete");
}

// MemoryUtils implementation
namespace MemoryUtils {

std::string formatBytes(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        unit++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << size << " " << units[unit];
    return oss.str();
}

float calculateGrowthRate(const std::vector<SystemMemorySnapshot>& snapshots, size_t window_size) {
    if (snapshots.size() < 2) {
        return 0.0f;
    }

    size_t start_idx = snapshots.size() > window_size ? snapshots.size() - window_size : 0;
    const auto& start = snapshots[start_idx];
    const auto& end = snapshots.back();

    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        end.timestamp - start.timestamp).count();

    if (duration <= 0) {
        return 0.0f;
    }

    long long memory_diff = static_cast<long long>(end.process_rss) - static_cast<long long>(start.process_rss);
    return static_cast<float>(memory_diff) / duration;
}

MemoryPattern detectPattern(const std::vector<SystemMemorySnapshot>& snapshots) {
    if (snapshots.size() < 5) {
        return MemoryPattern::UNKNOWN;
    }

    std::vector<size_t> memory_values;
    for (const auto& snapshot : snapshots) {
        memory_values.push_back(snapshot.process_rss);
    }

    // Calculate trend
    float sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
    for (size_t i = 0; i < memory_values.size(); ++i) {
        sum_x += i;
        sum_y += memory_values[i];
        sum_xy += i * memory_values[i];
        sum_x2 += i * i;
    }

    float n = static_cast<float>(memory_values.size());
    float slope = (n * sum_xy - sum_x * sum_y) / (n * sum_x2 - sum_x * sum_x);

    // Determine pattern based on slope
    if (std::abs(slope) < 1024) { // Less than 1KB change per sample
        return MemoryPattern::STABLE;
    } else if (slope > 0) {
        return MemoryPattern::GROWING;
    } else {
        return MemoryPattern::DECLINING;
    }
}

std::string getPatternDescription(MemoryPattern pattern) {
    switch (pattern) {
        case MemoryPattern::STABLE: return "Stable";
        case MemoryPattern::GROWING: return "Growing";
        case MemoryPattern::DECLINING: return "Declining";
        case MemoryPattern::OSCILLATING: return "Oscillating";
        case MemoryPattern::UNKNOWN: return "Unknown";
        default: return "Unknown";
    }
}

}
