#include "TimestampManager.hpp"
#include "Logger.hpp"
#include "IMPSystem.hpp"
#include <time.h>
#include <map>
#include <cstdlib>
#include <chrono>
#include <imp_system.h>

#define MODULE "TIMESTAMP_MANAGER"

TimestampManager& TimestampManager::getInstance() {
    static TimestampManager instance;
    return instance;
}

int TimestampManager::initialize() {
    std::lock_guard<std::mutex> lock(mutex);

    if (initialized) {
        LOG_WARN("TimestampManager already initialized");
        return 0;
    }

    // No baseline needed - we use IMP_System_GetTimeStamp() directly
    // This ensures SINGLE SOURCE OF TRUTH with hardware timestamps
    initialized = true;

    LOG_INFO("TimestampManager initialized - using IMP hardware timestamps directly");
    return 0;
}

uint64_t TimestampManager::getTimestampNs() {
    if (!initialized) {
        LOG_ERROR("TimestampManager not initialized!");
        return 0;
    }

    // SINGLE SOURCE OF TRUTH: Use IMP hardware timestamp (already monotonic)
    // IMP_System_GetTimeStamp() returns microseconds since the baseline we set
    int64_t impTimestamp = IMP_System_GetTimeStamp();

    // Convert to nanoseconds for high precision
    return static_cast<uint64_t>(impTimestamp) * 1000ULL;
}

uint64_t TimestampManager::getTimestampUs() {
    if (!initialized) {
        LOG_ERROR("TimestampManager not initialized!");
        return 0;
    }

    // SINGLE SOURCE OF TRUTH: Use IMP hardware timestamp directly
    return static_cast<uint64_t>(IMP_System_GetTimeStamp());
}

void TimestampManager::getTimestamp(struct timeval* tv) {
    if (!tv) {
        LOG_ERROR("TimestampManager::getTimestamp: null timeval pointer");
        return;
    }

    if (!initialized) {
        LOG_ERROR("TimestampManager not initialized!");
        tv->tv_sec = 0;
        tv->tv_usec = 0;
        return;
    }

    // SINGLE SOURCE OF TRUTH: Use IMP hardware timestamp directly
    int64_t impTimestamp = IMP_System_GetTimeStamp();
    tv->tv_sec = impTimestamp / 1000000;
    tv->tv_usec = impTimestamp % 1000000;

    // TIMESTAMP DEBUG: Log TimestampManager output (improved rate limiting for sync debugging)
    static int log_count = 0;
    static auto last_log_time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto time_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time);

    // Log first 20 calls, then every 1 second, or if there's a significant timestamp jump
    static int64_t last_timestamp = 0;
    int64_t timestamp_diff = std::abs(impTimestamp - last_timestamp);
    bool significant_jump = (last_timestamp > 0 && timestamp_diff > 100000); // >100ms jump

    if (log_count < 20 || time_since_last.count() >= 1000 || significant_jump) {
        LOG_DEBUG("TIMESTAMP_MANAGER_SOURCE: impTimestamp=" << impTimestamp
                 << " tv_sec=" << tv->tv_sec << " tv_usec=" << tv->tv_usec
                 << " diff_from_last=" << (impTimestamp - last_timestamp));
        last_log_time = now;
        log_count++;
    }
    last_timestamp = impTimestamp;
}


