#pragma once

#include "ZeroCopyMemoryAnalyzer.hpp"
#include <string>
#include <sstream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <iostream>

/**
 * Zero-Copy Memory Reporter
 * 
 * Generates comprehensive memory usage reports for monitoring and debugging
 */

class ZeroCopyMemoryReporter {
public:
    static ZeroCopyMemoryReporter& getInstance() {
        static ZeroCopyMemoryReporter instance;
        return instance;
    }
    
    // Report generation
    std::string generateSummaryReport() const;
    std::string generateDetailedReport() const;
    std::string generatePerformanceReport() const;
    std::string generateLeakReport() const;
    std::string generateOptimizationReport() const;
    
    // JSON format reports (for web interface)
    std::string generateJSONReport() const;
    
    // Console-friendly reports
    void printSummaryToConsole() const;
    void printDetailedToConsole() const;
    
    // File output
    bool saveReportToFile(const std::string& filename, const std::string& report) const;
    bool saveAllReportsToDirectory(const std::string& directory) const;
    
    // Comparison reports
    std::string generateComparisonReport(const ZeroCopyMemoryAnalyzer::MemoryStats& baseline) const;
    
    // Real-time monitoring
    void startPeriodicReporting(int interval_seconds = 60);
    void stopPeriodicReporting();

private:
    ZeroCopyMemoryReporter() = default;
    ~ZeroCopyMemoryReporter() { stopPeriodicReporting(); }
    
    // Prevent copying
    ZeroCopyMemoryReporter(const ZeroCopyMemoryReporter&) = delete;
    ZeroCopyMemoryReporter& operator=(const ZeroCopyMemoryReporter&) = delete;
    
    // Helper methods
    std::string formatBytes(size_t bytes) const;
    std::string formatPercentage(double ratio) const;
    std::string formatDuration(std::chrono::steady_clock::time_point start) const;
    std::string formatTimestamp(std::chrono::steady_clock::time_point time) const;
    
    // Periodic reporting
    std::atomic<bool> periodic_reporting_enabled_{false};
    std::unique_ptr<std::thread> reporting_thread_;
    void periodicReportingLoop(int interval_seconds);
};

/**
 * Memory Usage Dashboard
 * 
 * Real-time dashboard for monitoring zero-copy memory usage
 */
class ZeroCopyMemoryDashboard {
public:
    static ZeroCopyMemoryDashboard& getInstance() {
        static ZeroCopyMemoryDashboard instance;
        return instance;
    }
    
    // Dashboard data structure
    struct DashboardData {
        // Current metrics
        size_t total_memory_mb;
        size_t zero_copy_memory_mb;
        size_t legacy_memory_mb;
        double memory_savings_percent;
        double pool_hit_ratio_percent;
        
        // Performance metrics
        double allocations_per_second;
        double bytes_per_second;
        size_t active_buffers;
        size_t pool_buffers;
        
        // Health indicators
        bool memory_warning;
        bool memory_critical;
        size_t potential_leaks;
        double optimization_score; // 0-100
        
        // Trends (compared to previous measurement)
        double memory_trend_percent;
        double performance_trend_percent;
        
        std::chrono::steady_clock::time_point timestamp;
    };
    
    // Get current dashboard data
    DashboardData getCurrentData() const;
    
    // Update dashboard (call periodically)
    void updateDashboard();
    
    // Get formatted dashboard string
    std::string getFormattedDashboard() const;
    
    // Dashboard alerts
    struct Alert {
        std::string type;
        std::string message;
        std::chrono::steady_clock::time_point timestamp;
        bool acknowledged;
    };
    
    std::vector<Alert> getActiveAlerts() const;
    void acknowledgeAlert(size_t alert_index);
    void clearAllAlerts();

private:
    ZeroCopyMemoryDashboard() = default;
    ~ZeroCopyMemoryDashboard() = default;
    
    // Prevent copying
    ZeroCopyMemoryDashboard(const ZeroCopyMemoryDashboard&) = delete;
    ZeroCopyMemoryDashboard& operator=(const ZeroCopyMemoryDashboard&) = delete;
    
    DashboardData current_data_;
    DashboardData previous_data_;
    std::vector<Alert> active_alerts_;
    mutable std::mutex dashboard_mutex_;
    
    // Helper methods
    double calculateOptimizationScore(const ZeroCopyMemoryAnalyzer::MemoryStats& stats) const;
    void updateAlerts(const DashboardData& data);
    void addAlert(const std::string& type, const std::string& message);
};

/**
 * Memory Usage Benchmarker
 * 
 * Benchmarks memory usage and performance improvements
 */
class ZeroCopyMemoryBenchmarker {
public:
    static ZeroCopyMemoryBenchmarker& getInstance() {
        static ZeroCopyMemoryBenchmarker instance;
        return instance;
    }
    
    // Benchmark configuration
    struct BenchmarkConfig {
        size_t test_duration_seconds{60};
        size_t buffer_sizes[5]{1024, 4096, 16384, 65536, 262144};
        size_t allocations_per_size{1000};
        bool compare_with_legacy{true};
        bool measure_latency{true};
        bool measure_throughput{true};
    };
    
    // Benchmark results
    struct BenchmarkResults {
        // Memory efficiency
        double memory_savings_percent;
        double peak_memory_reduction_percent;
        
        // Performance metrics
        double allocation_speed_improvement_percent;
        double access_speed_improvement_percent;
        double overall_performance_improvement_percent;
        
        // Detailed metrics
        std::vector<double> allocation_latencies_us;
        std::vector<double> access_latencies_us;
        double throughput_mb_per_second;
        
        // Pool efficiency
        double pool_hit_ratio;
        double pool_efficiency_score;
        
        std::chrono::steady_clock::time_point benchmark_time;
        std::string benchmark_notes;
    };
    
    // Run benchmarks
    BenchmarkResults runFullBenchmark(const BenchmarkConfig& config);
    BenchmarkResults runFullBenchmark(); // Uses default config
    BenchmarkResults runMemoryBenchmark();
    BenchmarkResults runPerformanceBenchmark();
    BenchmarkResults runPoolEfficiencyBenchmark();
    
    // Compare results
    std::string compareResults(const BenchmarkResults& baseline, const BenchmarkResults& current) const;
    
    // Save/load benchmark results
    bool saveBenchmarkResults(const BenchmarkResults& results, const std::string& filename) const;
    BenchmarkResults loadBenchmarkResults(const std::string& filename) const;

private:
    ZeroCopyMemoryBenchmarker() = default;
    ~ZeroCopyMemoryBenchmarker() = default;
    
    // Prevent copying
    ZeroCopyMemoryBenchmarker(const ZeroCopyMemoryBenchmarker&) = delete;
    ZeroCopyMemoryBenchmarker& operator=(const ZeroCopyMemoryBenchmarker&) = delete;
    
    // Benchmark helpers
    double measureAllocationLatency(size_t buffer_size, size_t iterations);
    double measureAccessLatency(size_t buffer_size, size_t iterations);
    double measureThroughput(size_t total_bytes, std::chrono::duration<double> duration);
    
    // Legacy comparison helpers
    double benchmarkLegacyAllocation(size_t buffer_size, size_t iterations);
    double benchmarkLegacyAccess(size_t buffer_size, size_t iterations);
};

// Utility functions for memory analysis
namespace ZeroCopyMemoryUtils {
    // Convert memory stats to human-readable format
    std::string formatMemoryStats(const ZeroCopyMemoryAnalyzer::MemoryStats& stats);
    
    // Calculate memory efficiency score (0-100)
    double calculateEfficiencyScore(const ZeroCopyMemoryAnalyzer::MemoryStats& stats);
    
    // Detect memory usage patterns
    std::string detectUsagePattern(const std::vector<ZeroCopyMemoryAnalyzer::BufferUsageInfo>& usage);
    
    // Generate optimization recommendations
    std::vector<std::string> generateQuickRecommendations(const ZeroCopyMemoryAnalyzer::MemoryStats& stats);
    
    // Memory health check
    struct HealthCheck {
        bool memory_usage_healthy;
        bool pool_efficiency_healthy;
        bool no_leaks_detected;
        bool performance_acceptable;
        double overall_health_score; // 0-100
        std::vector<std::string> issues;
        std::vector<std::string> recommendations;
    };
    
    HealthCheck performHealthCheck();
}
