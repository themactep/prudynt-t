#pragma once

#include "network/rtsp/RtpPacketizer.hpp"
#include "network/rtsp/RtspTypes.hpp"
#include "stream/globals.hpp"

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace simple_rtsp {

struct Session {
    int fd = -1;
    int sessionsIndex = -1;
    char readBuf[RTSP_BUF_SIZE];
    int  readOff = 0;

    char sessionId[32]{};
    char videoSetupUrl[256]{};
    char audioSetupUrl[256]{};
    bool playing     = false;
    int  videoChn    = -1;
    bool hasAudio    = false;
    bool audioOnly   = false;
    bool backchannel = false;
    bool backchannelActive = false;
    int  backchannelPayloadType = -1;

    // Transport
    bool    tcpInterleaved     = true;
    uint8_t videoInterleavedRtp  = 0;
    uint8_t videoInterleavedRtcp = 1;
    uint8_t audioInterleavedRtp  = 2;
    uint8_t audioInterleavedRtcp = 3;

    // UDP transport
    int     videoRtpSock    = -1;
    int     videoRtcpSock   = -1;
    int     audioRtpSock    = -1;
    int     audioRtcpSock   = -1;
    uint16_t videoServerRtpPort  = 0;
    uint16_t videoServerRtcpPort = 0;
    uint16_t audioServerRtpPort  = 0;
    uint16_t audioServerRtcpPort = 0;
    uint16_t videoClientRtpPort  = 0;
    uint16_t videoClientRtcpPort = 0;
    uint16_t audioClientRtpPort  = 0;
    uint16_t audioClientRtcpPort = 0;
    int     backchannelRtpSock  = -1;
    uint16_t backchannelServerRtpPort = 0;
    uint16_t backchannelClientRtpPort = 0;
    uint8_t backchannelInterleavedRtp  = 0;
    uint8_t backchannelInterleavedRtcp = 1;
    sockaddr_in clientAddr  = {};
    socklen_t clientAddrLen = 0;

    // Taps
    std::shared_ptr<MsgChannel<H264NALUnit>> videoTap;
    uint64_t videoTapId = 0;
    std::shared_ptr<MsgChannel<AudioFrame>> audioTap;
    uint64_t audioTapId = 0;

    // Subtitle (OSD text)
    bool hasSubtitles = false;
    bool subtitleTcp = false;
    char subtitleSetupUrl[256]{};
    uint8_t subtitleInterleavedRtp  = 6;
    uint8_t subtitleInterleavedRtcp = 7;
    int     subtitleRtpSock   = -1;
    int     subtitleRtcpSock  = -1;
    uint16_t subtitleServerRtpPort  = 0;
    uint16_t subtitleServerRtcpPort = 0;
    uint16_t subtitleClientRtpPort  = 0;
    uint16_t subtitleClientRtcpPort = 0;
    time_t  lastSubtitleSent = 0;
    std::string lastSubtitleText;

    // RTP state
    RtpState videoRtp;
    RtpState audioRtp;
    RtpState subtitleRtp;

    bool    codecConfigSent    = false;
    bool    waitingForKeyframe = false;

    // Timestamps
    struct timeval startAnchor{0, 0};
    int64_t videoStartAnchorUs = -1;
    int64_t videoTsToMonoOffset = 0;
    uint32_t spsHash = 0;
    uint32_t ppsHash = 0;
    bool     spsChanged = false;
    uint32_t videoFrameCount = 0;
    uint32_t lastFrameRtpTs = 0;
    bool     hasFrameRtpTs = false;
    bool     hasAudioRtpTs = false;
    uint64_t ntpAnchor = 0;
    int64_t  ntpAnchorMonoUs = -1;
    int64_t  lastVideoTsUs = -1;
    int64_t  lastAudioTsUs = -1;

    time_t lastActivity = 0;
    bool   authenticated = false;

    bool hasValidSession() const { return sessionId[0] != '\0'; }

    // Send queue (non-blocking, unbounded)
    std::deque<std::vector<uint8_t>> sendQueue;
    size_t sendQueueBytes = 0;

    // Deferred RTSP response (EAGAIN/partial send fallback)
    uint8_t pendingResp[RTSP_BUF_SIZE];
    size_t pendingRespLen = 0;
    size_t pendingRespOff = 0;
};

} // namespace simple_rtsp
