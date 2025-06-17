#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include <fstream>
#include "Logger.hpp"

#undef MODULE
#define MODULE "MEMMONITOR"

/**
 * Memory Leak Detection and Monitoring System
 * 
 * Features:
 * - Real-time memory usage tracking
 * - Leak detection with stack traces
 * - Memory growth pattern analysis
 * - Integration with BufferPool
 * - Automatic cleanup recommendations
 * - Performance impact monitoring
 */

struct MemoryAllocation {
    void* address;
    size_t size;
    std::chrono::steady_clock::time_point allocated_time;
    std::string source_info;  // File:line or function name
    std::thread::id thread_id;
};

struct MemoryStats {
    size_t current_usage;
    size_t peak_usage;
    size_t total_allocated;
    size_t total_freed;
    size_t allocation_count;
    size_t free_count;
    size_t leak_count;
    std::chrono::steady_clock::time_point last_update;
};

struct SystemMemorySnapshot {
    size_t total_memory;
    size_t free_memory;
    size_t available_memory;
    size_t buffers;
    size_t cached;
    size_t process_rss;      // Resident Set Size
    size_t process_vms;      // Virtual Memory Size
    float cpu_usage;
    std::chrono::steady_clock::time_point timestamp;
};

struct LeakReport {
    std::vector<MemoryAllocation> suspected_leaks;
    size_t total_leaked_bytes;
    std::chrono::steady_clock::time_point report_time;
    std::string analysis;
};

class MemoryMonitor {
public:
    static MemoryMonitor& getInstance() {
        static MemoryMonitor instance;
        return instance;
    }

    // Initialize memory monitoring
    bool initialize(bool enable_allocation_tracking = false);
    
    // Start/stop monitoring thread
    void startMonitoring();
    void stopMonitoring();
    
    // Manual allocation tracking (for custom allocators)
    void trackAllocation(void* ptr, size_t size, const std::string& source = "");
    void trackDeallocation(void* ptr);
    
    // Memory analysis
    MemoryStats getMemoryStats();
    SystemMemorySnapshot getCurrentSnapshot();
    std::vector<SystemMemorySnapshot> getHistoricalSnapshots(size_t count = 100);
    
    // Leak detection
    LeakReport detectLeaks();
    std::vector<MemoryAllocation> getLongLivedAllocations(std::chrono::minutes age_threshold = std::chrono::minutes(10));
    
    // Memory pressure analysis
    bool isMemoryPressureHigh();
    float getMemoryGrowthRate(); // bytes per second
    
    // Reporting
    std::string generateMemoryReport();
    void logMemoryStatus();
    
    // Configuration
    void setLeakDetectionThreshold(std::chrono::minutes threshold);
    void setSnapshotInterval(std::chrono::seconds interval);
    void setMaxHistorySize(size_t max_snapshots);
    
    // Cleanup
    void shutdown();

private:
    MemoryMonitor() = default;
    ~MemoryMonitor() { shutdown(); }
    
    // Prevent copying
    MemoryMonitor(const MemoryMonitor&) = delete;
    MemoryMonitor& operator=(const MemoryMonitor&) = delete;

    // Monitoring thread function
    void monitoringLoop();
    
    // System memory reading
    bool readSystemMemory(SystemMemorySnapshot& snapshot);
    bool readProcessMemory(SystemMemorySnapshot& snapshot);
    float readCpuUsage();
    
    // Analysis functions
    void analyzeMemoryTrends();
    void detectMemoryLeaks();
    std::string analyzeLeakPattern(const std::vector<MemoryAllocation>& leaks);
    
    // Data structures
    std::unordered_map<void*, MemoryAllocation> tracked_allocations_;
    std::vector<SystemMemorySnapshot> memory_history_;
    std::mutex allocations_mutex_;
    std::mutex history_mutex_;
    
    // Statistics
    MemoryStats stats_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> monitoring_enabled_{false};
    std::atomic<bool> allocation_tracking_enabled_{false};
    
    // Monitoring thread
    std::unique_ptr<std::thread> monitoring_thread_;
    std::atomic<bool> should_stop_{false};
    
    // Configuration
    std::chrono::minutes leak_threshold_{std::chrono::minutes(10)};
    std::chrono::seconds snapshot_interval_{std::chrono::seconds(30)};
    size_t max_history_size_{200}; // ~100 minutes at 30s intervals
    
    // Performance tracking
    std::chrono::steady_clock::time_point last_analysis_;
    size_t analysis_count_{0};
    
    // Constants
    static constexpr float HIGH_PRESSURE_THRESHOLD = 0.90f;
    static constexpr size_t MIN_LEAK_SIZE = 1024; // 1KB minimum to report as leak
    static constexpr size_t MAX_TRACKED_ALLOCATIONS = 10000;
};

// Optional allocation tracking macros (can be disabled for performance)
#ifdef ENABLE_MEMORY_TRACKING
#define TRACK_MALLOC(size) MemoryMonitor::getInstance().trackAllocation(malloc(size), size, __FILE__ ":" + std::to_string(__LINE__))
#define TRACK_FREE(ptr) do { MemoryMonitor::getInstance().trackDeallocation(ptr); free(ptr); } while(0)
#define TRACK_NEW(type) ([&]() { auto ptr = new type; MemoryMonitor::getInstance().trackAllocation(ptr, sizeof(type), __FILE__ ":" + std::to_string(__LINE__)); return ptr; })()
#define TRACK_DELETE(ptr) do { MemoryMonitor::getInstance().trackDeallocation(ptr); delete ptr; } while(0)
#else
#define TRACK_MALLOC(size) malloc(size)
#define TRACK_FREE(ptr) free(ptr)
#define TRACK_NEW(type) new type
#define TRACK_DELETE(ptr) delete ptr
#endif

// Memory monitoring utilities
namespace MemoryUtils {
    // Format bytes in human-readable format
    std::string formatBytes(size_t bytes);

    // Calculate memory growth rate
    float calculateGrowthRate(const std::vector<SystemMemorySnapshot>& snapshots, size_t window_size = 10);

    // Detect memory usage patterns
    enum class MemoryPattern {
        STABLE,
        GROWING,
        DECLINING,
        OSCILLATING,
        UNKNOWN
    };

    MemoryPattern detectPattern(const std::vector<SystemMemorySnapshot>& snapshots);

    // Get pattern description
    std::string getPatternDescription(MemoryPattern pattern);
}
