#pragma once

#include "liveMedia.hh"
#include "AdaptiveBitrate.hpp"
#include "Logger.hpp"
#include <chrono>
#include <unordered_map>

#undef MODULE
#define MODULE "ADAPTIVE_RTCP"

/**
 * RTCP Handler for Adaptive Bitrate Streaming
 * 
 * Processes RTCP Receiver Reports to extract network condition information
 * and feeds it to the AdaptiveBitrateManager for quality adaptation decisions.
 */

struct RTCPSessionData {
    unsigned session_id;
    int stream_channel;
    std::chrono::steady_clock::time_point last_report;
    uint32_t last_packets_sent;
    uint32_t last_bytes_sent;
    uint32_t cumulative_packets_lost;
    uint32_t last_sequence_number;
};

class AdaptiveRTCPHandler {
public:
    static AdaptiveRTCPHandler& getInstance() {
        static AdaptiveRTCPHandler instance;
        return instance;
    }

    // Register a session for RTCP monitoring
    void registerSession(unsigned session_id, int stream_channel);
    
    // Unregister a session
    void unregisterSession(unsigned session_id);
    
    // RTCP callback function (static for C compatibility)
    static void rtcpCallback(void* clientData, unsigned sessionId, 
                           unsigned char* rtcpData, unsigned rtcpDataSize);
    
    // Process RTCP Receiver Report
    void processReceiverReport(unsigned session_id, 
                             unsigned char* rtcpData, 
                             unsigned rtcpDataSize);
    
    // Process RTCP Sender Report
    void processSenderReport(unsigned session_id,
                           unsigned char* rtcpData,
                           unsigned rtcpDataSize);
    
    // Trigger quality adaptation check
    void checkAdaptation(unsigned session_id);
    
    // Get session statistics
    RTCPSessionData* getSessionData(unsigned session_id);

private:
    AdaptiveRTCPHandler() = default;
    ~AdaptiveRTCPHandler() = default;
    
    // Prevent copying
    AdaptiveRTCPHandler(const AdaptiveRTCPHandler&) = delete;
    AdaptiveRTCPHandler& operator=(const AdaptiveRTCPHandler&) = delete;

    // Parse RTCP packet
    bool parseRTCPPacket(unsigned char* data, unsigned size,
                        uint8_t& packet_type, uint32_t& ssrc);
    
    // Extract receiver report data
    bool extractReceiverReportData(unsigned char* data, unsigned size,
                                 uint32_t& packets_lost,
                                 uint32_t& highest_seq_received,
                                 uint32_t& jitter,
                                 uint32_t& lsr,
                                 uint32_t& dlsr);
    
    // Calculate network metrics
    float calculatePacketLossRate(const RTCPSessionData& session, uint32_t packets_lost);
    uint32_t calculateRTT(uint32_t lsr, uint32_t dlsr);
    uint32_t convertJitterToMs(uint32_t jitter_units, uint32_t sample_rate = 90000);
    
    // Session data storage
    std::unordered_map<unsigned, RTCPSessionData> session_data_;
    std::mutex data_mutex_;
};

// RTCP packet type constants
namespace RTCPPacketTypes {
    constexpr uint8_t SENDER_REPORT = 200;
    constexpr uint8_t RECEIVER_REPORT = 201;
    constexpr uint8_t SOURCE_DESCRIPTION = 202;
    constexpr uint8_t BYE = 203;
    constexpr uint8_t APP = 204;
}

// Utility functions for RTCP parsing
namespace RTCPUtils {
    // Extract 32-bit value from network byte order
    uint32_t extractUint32(const unsigned char* data, size_t offset);
    
    // Extract 16-bit value from network byte order
    uint16_t extractUint16(const unsigned char* data, size_t offset);
    
    // Convert NTP timestamp to milliseconds
    uint32_t ntpToMilliseconds(uint32_t ntp_timestamp);
    
    // Validate RTCP packet header
    bool validateRTCPHeader(const unsigned char* data, unsigned size);
    
    // Calculate packet loss percentage
    float calculateLossPercentage(uint32_t packets_sent, uint32_t packets_lost);
}
