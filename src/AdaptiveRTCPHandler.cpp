#include "AdaptiveRTCPHandler.hpp"
#include <cstring>
#include <algorithm>

void AdaptiveRTCPHandler::registerSession(unsigned session_id, int stream_channel) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    
    RTCPSessionData session_data;
    session_data.session_id = session_id;
    session_data.stream_channel = stream_channel;
    session_data.last_report = std::chrono::steady_clock::now();
    session_data.last_packets_sent = 0;
    session_data.last_bytes_sent = 0;
    session_data.cumulative_packets_lost = 0;
    session_data.last_sequence_number = 0;
    
    session_data_[session_id] = session_data;
    
    // Register with adaptive bitrate manager
    AdaptiveBitrateManager::getInstance().registerClient(session_id, stream_channel);
    
    LOG_DEBUG("Registered RTCP session " << session_id << " for stream " << stream_channel);
}

void AdaptiveRTCPHandler::unregisterSession(unsigned session_id) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    
    auto it = session_data_.find(session_id);
    if (it != session_data_.end()) {
        session_data_.erase(it);
        AdaptiveBitrateManager::getInstance().unregisterClient(session_id);
        LOG_DEBUG("Unregistered RTCP session " << session_id);
    }
}

void AdaptiveRTCPHandler::rtcpCallback(void* clientData, unsigned sessionId,
                                     unsigned char* rtcpData, unsigned rtcpDataSize) {
    // Static callback function for LiveMedia555 compatibility
    AdaptiveRTCPHandler& handler = getInstance();
    
    if (rtcpData && rtcpDataSize > 0) {
        uint8_t packet_type;
        uint32_t ssrc;
        
        if (handler.parseRTCPPacket(rtcpData, rtcpDataSize, packet_type, ssrc)) {
            switch (packet_type) {
                case RTCPPacketTypes::RECEIVER_REPORT:
                    handler.processReceiverReport(sessionId, rtcpData, rtcpDataSize);
                    break;
                case RTCPPacketTypes::SENDER_REPORT:
                    handler.processSenderReport(sessionId, rtcpData, rtcpDataSize);
                    break;
                default:
                    // Other RTCP packet types not handled for adaptation
                    break;
            }
        }
    }
}

bool AdaptiveRTCPHandler::parseRTCPPacket(unsigned char* data, unsigned size,
                                        uint8_t& packet_type, uint32_t& ssrc) {
    if (!RTCPUtils::validateRTCPHeader(data, size)) {
        return false;
    }
    
    // RTCP header format:
    // 0                   1                   2                   3
    // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |V=2|P|    RC   |   PT=SR=200   |             length            |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                         SSRC of sender                        |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    
    packet_type = data[1];
    ssrc = RTCPUtils::extractUint32(data, 4);
    
    return true;
}

void AdaptiveRTCPHandler::processReceiverReport(unsigned session_id,
                                              unsigned char* rtcpData,
                                              unsigned rtcpDataSize) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    
    auto it = session_data_.find(session_id);
    if (it == session_data_.end()) {
        return;
    }
    
    auto& session = it->second;
    
    // Extract receiver report data
    uint32_t packets_lost, highest_seq, jitter, lsr, dlsr;
    if (!extractReceiverReportData(rtcpData, rtcpDataSize, 
                                  packets_lost, highest_seq, jitter, lsr, dlsr)) {
        LOG_WARN("Failed to parse receiver report for session " << session_id);
        return;
    }
    
    // Calculate network metrics
    float packet_loss_rate = calculatePacketLossRate(session, packets_lost);
    uint32_t rtt_ms = calculateRTT(lsr, dlsr);
    uint32_t jitter_ms = convertJitterToMs(jitter);
    
    // Estimate bytes received (simplified calculation)
    uint64_t bytes_received = (highest_seq - session.last_sequence_number) * 1400; // Assume ~1400 bytes per packet
    
    // Update session data
    session.last_report = std::chrono::steady_clock::now();
    session.cumulative_packets_lost = packets_lost;
    session.last_sequence_number = highest_seq;
    
    // Send feedback to adaptive bitrate manager
    AdaptiveBitrateManager::getInstance().processRTCPFeedback(
        session_id, packet_loss_rate, rtt_ms, jitter_ms, bytes_received);
    
    LOG_DEBUG("RTCP RR for session " << session_id 
              << ": loss=" << (packet_loss_rate * 100) << "%, "
              << "rtt=" << rtt_ms << "ms, "
              << "jitter=" << jitter_ms << "ms");
    
    // Check if adaptation is needed
    checkAdaptation(session_id);
}

void AdaptiveRTCPHandler::processSenderReport(unsigned session_id,
                                            unsigned char* rtcpData,
                                            unsigned rtcpDataSize) {
    // Sender reports contain information about what we're sending
    // Can be used to track our own transmission statistics
    std::lock_guard<std::mutex> lock(data_mutex_);
    
    auto it = session_data_.find(session_id);
    if (it == session_data_.end()) {
        return;
    }
    
    // For now, just update the last report time
    it->second.last_report = std::chrono::steady_clock::now();
}

bool AdaptiveRTCPHandler::extractReceiverReportData(unsigned char* data, unsigned size,
                                                   uint32_t& packets_lost,
                                                   uint32_t& highest_seq_received,
                                                   uint32_t& jitter,
                                                   uint32_t& lsr,
                                                   uint32_t& dlsr) {
    // Receiver Report format (after RTCP header):
    // 0                   1                   2                   3
    // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                 SSRC_1 (SSRC of first source)                 |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // | fraction lost |       cumulative number of packets lost      |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |           extended highest sequence number received           |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                      interarrival jitter                     |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                         last SR (LSR)                        |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                   delay since last SR (DLSR)                 |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    
    if (size < 32) { // Minimum size for RR with one report block
        return false;
    }
    
    // Skip RTCP header (8 bytes) and SSRC (4 bytes)
    size_t offset = 12;
    
    // Extract cumulative packets lost (24 bits, skip fraction lost)
    packets_lost = RTCPUtils::extractUint32(data, offset) & 0x00FFFFFF;
    offset += 4;
    
    // Extract extended highest sequence number
    highest_seq_received = RTCPUtils::extractUint32(data, offset);
    offset += 4;
    
    // Extract interarrival jitter
    jitter = RTCPUtils::extractUint32(data, offset);
    offset += 4;
    
    // Extract last SR timestamp
    lsr = RTCPUtils::extractUint32(data, offset);
    offset += 4;
    
    // Extract delay since last SR
    dlsr = RTCPUtils::extractUint32(data, offset);
    
    return true;
}

float AdaptiveRTCPHandler::calculatePacketLossRate(const RTCPSessionData& session, uint32_t packets_lost) {
    if (session.last_sequence_number == 0) {
        return 0.0f; // First report
    }
    
    uint32_t packets_sent = session.last_sequence_number;
    if (packets_sent == 0) {
        return 0.0f;
    }
    
    return RTCPUtils::calculateLossPercentage(packets_sent, packets_lost) / 100.0f;
}

uint32_t AdaptiveRTCPHandler::calculateRTT(uint32_t lsr, uint32_t dlsr) {
    if (lsr == 0 || dlsr == 0) {
        return 100; // Default RTT if no data
    }
    
    // Convert NTP timestamp format to milliseconds
    // This is a simplified calculation
    uint32_t rtt_ntp = dlsr; // DLSR is already in NTP timestamp format
    return RTCPUtils::ntpToMilliseconds(rtt_ntp);
}

uint32_t AdaptiveRTCPHandler::convertJitterToMs(uint32_t jitter_units, uint32_t sample_rate) {
    // Convert jitter from timestamp units to milliseconds
    // For video, sample rate is typically 90000 Hz
    if (sample_rate == 0) {
        return 0;
    }
    
    return (jitter_units * 1000) / sample_rate;
}

void AdaptiveRTCPHandler::checkAdaptation(unsigned session_id) {
    // Check if quality adaptation is needed
    if (AdaptiveBitrateManager::getInstance().shouldAdaptQuality(session_id)) {
        auto target_quality = AdaptiveBitrateManager::getInstance().getTargetQuality(session_id);
        
        // Here we would trigger the encoder to change quality
        // This requires integration with the encoder system
        LOG_INFO("Quality adaptation recommended for session " << session_id 
                 << ": " << AdaptiveUtils::qualityToString(target_quality));
        
        // Apply the quality change
        AdaptiveBitrateManager::getInstance().applyQualityChange(session_id, target_quality);
    }
}

RTCPSessionData* AdaptiveRTCPHandler::getSessionData(unsigned session_id) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    
    auto it = session_data_.find(session_id);
    return (it != session_data_.end()) ? &it->second : nullptr;
}

// RTCPUtils implementation
namespace RTCPUtils {

uint32_t extractUint32(const unsigned char* data, size_t offset) {
    return (static_cast<uint32_t>(data[offset]) << 24) |
           (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) |
           static_cast<uint32_t>(data[offset + 3]);
}

uint16_t extractUint16(const unsigned char* data, size_t offset) {
    return (static_cast<uint16_t>(data[offset]) << 8) |
           static_cast<uint16_t>(data[offset + 1]);
}

uint32_t ntpToMilliseconds(uint32_t ntp_timestamp) {
    // Convert NTP timestamp fraction to milliseconds
    // NTP timestamp is in 1/65536 second units
    return (ntp_timestamp * 1000) / 65536;
}

bool validateRTCPHeader(const unsigned char* data, unsigned size) {
    if (size < 8) {
        return false;
    }
    
    // Check version (should be 2)
    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2) {
        return false;
    }
    
    // Check packet type (should be valid RTCP type)
    uint8_t packet_type = data[1];
    return (packet_type >= 200 && packet_type <= 204);
}

float calculateLossPercentage(uint32_t packets_sent, uint32_t packets_lost) {
    if (packets_sent == 0) {
        return 0.0f;
    }
    
    return (static_cast<float>(packets_lost) / packets_sent) * 100.0f;
}

}
