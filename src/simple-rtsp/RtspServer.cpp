// musl/uclibc may need this for MSG_NOSIGNAL, strncasecmp
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "RtspServer.hpp"
#include "RtpPacketizer.hpp"
#include "SdpGenerator.hpp"
#include "Logger.hpp"
#include "globals.hpp"
#include "IMPBackchannel.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

#undef MODULE
#define MODULE "SIMPLE-RTSP"

namespace simple_rtsp {

// ── Crash handler ──────────────────────────────────────────────────────────

static void crashHandler(int sig) {
    fprintf(stderr, "\n!!! CRASH signal %d !!!\n", sig);
    _exit(1);
}

__attribute__((constructor)) static void installCrashHandler() {
    signal(SIGSEGV, crashHandler);
    signal(SIGBUS, crashHandler);
}

// ═══════════════════════════════════════════════════════════════════════════
// Internal Session
// ═══════════════════════════════════════════════════════════════════════════

struct Session {
    int fd = -1;
    int sessionsIndex = -1;   // index in server's sessions_ vector
    char readBuf[RTSP_BUF_SIZE];
    int  readOff = 0;

    char sessionId[32]{};
    char videoSetupUrl[256]{};
    char audioSetupUrl[256]{};
    bool playing     = false;
    int  videoChn    = -1;   // encoder channel index, -1 = audio-only
    bool hasAudio    = false;
    bool audioOnly   = false; // true for /mic-style standalone audio
    bool backchannel = false; // receiving audio from client (talkback)
    bool backchannelActive = false; // this session incremented is_sending
    int  backchannelPayloadType = -1; // negotiated PT from ANNOUNCE

    // Transport
    bool    tcpInterleaved     = true;
    uint8_t videoInterleavedRtp  = 0;
    uint8_t videoInterleavedRtcp = 1;
    uint8_t audioInterleavedRtp  = 2;
    uint8_t audioInterleavedRtcp = 3;

    // UDP transport: server-side sockets and client target address
    int     videoRtpSock    = -1;
    int     videoRtcpSock   = -1;
    int     audioRtpSock    = -1;
    int     audioRtcpSock   = -1;
    // Server-side bound ports (reported in SETUP response as server_port)
    uint16_t videoServerRtpPort  = 0;
    uint16_t videoServerRtcpPort = 0;
    uint16_t audioServerRtpPort  = 0;
    uint16_t audioServerRtcpPort = 0;
    // Client-side ports (from SETUP request client_port, where we send to)
    uint16_t videoClientRtpPort  = 0;
    uint16_t videoClientRtcpPort = 0;
    uint16_t audioClientRtpPort  = 0;
    uint16_t audioClientRtcpPort = 0;
    // Backchannel: client-to-server RTP socket (we receive from client)
    int     backchannelRtpSock  = -1;
    uint16_t backchannelServerRtpPort = 0;
    uint16_t backchannelClientRtpPort = 0;
    // Backchannel TCP interleaved channels (separate from video/audio
    // so that backchannel SETUP does not corrupt video transport).
    uint8_t backchannelInterleavedRtp  = 0;
    uint8_t backchannelInterleavedRtcp = 1;
    sockaddr_in clientAddr  = {};
    socklen_t clientAddrLen = 0;

    // Video tap
    std::shared_ptr<MsgChannel<H264NALUnit>> videoTap;
    uint64_t videoTapId = 0;

    // Audio tap
    std::shared_ptr<MsgChannel<AudioFrame>> audioTap;
    uint64_t audioTapId = 0;

    // Subtitle (OSD text) — no tap, text is pulled from OSD each second
    bool hasSubtitles = false;
    bool subtitleTcp = false;
    char subtitleSetupUrl[256]{};
    uint8_t subtitleInterleavedRtp  = 6;
    uint8_t subtitleInterleavedRtcp = 7;
    // UDP subtitle transport
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

    bool    codecConfigSent    = false; // SPS/PPS prepended for this session
    bool    waitingForKeyframe = false; // drop non-IDR until first keyframe arrives

    // Audio uses the IMP driver's capture timestamp (microseconds).
    struct timeval startAnchor{0, 0};

    // Video uses encoder monotonic clock (imp_ts, microseconds).
    // Anchored to the first frame's imp_ts for the session so RTP
    // timestamps track the actual frame cadence.
    int64_t videoStartAnchorUs = -1;

    // Offset between imp_ts and CLOCK_MONOTONIC, computed once at
    // session start.  Adding this to imp_ts yields an approximate
    // CLOCK_MONOTONIC value, keeping the RTCP SR NTP anchor on the
    // same clock domain even when IMP_System_RebaseTimeStamp is not
    // in effect (e.g. on T31).
    int64_t videoTsToMonoOffset = 0;

    // Track last-sent SPS/PPS fingerprint to detect reconfiguration
    // (e.g. after day/night switch when fps changes and encoder re-emits
    // new codec config).
    uint32_t spsHash = 0;
    uint32_t ppsHash = 0;
    bool     spsChanged = false;  // set when we detect new SPS, cleared after prepend

    // Frame counter for RTP timestamp generation.  RTP timestamps now
    // use encoder imp_ts (real cadence) instead of a synthetic clock.
    // videoFrameCount is kept for diagnostics / SDP stats only.
    uint32_t videoFrameCount = 0;

    // Previous frame-start RTP timestamp, used for monotonicity guard.
    // Stored separately from videoRtp.timestamp because the RtpState
    // struct may be read from other contexts (RTCP SR, PLAY response)
    // that must not interfere with the monotonicity check.
    uint32_t lastFrameRtpTs = 0;
    bool     hasFrameRtpTs = false;

    // Audio monotonicity guard (simpler: audio frames are whole, no
    // fragmentation, so compare directly against audioRtp.timestamp).
    bool     hasAudioRtpTs = false;

    // RTCP SR clock mapping.  The wall-clock NTP time is captured ONCE
    // per session and paired with a CLOCK_MONOTONIC reference; all
    // subsequent SR NTP values are derived from the monotonic-domain
    // media timestamps (imp_ts / af.time, both rebased to
    // CLOCK_MONOTONIC by IMPSystem).  Using gettimeofday() directly in
    // each SR breaks the NTP<->RTP mapping whenever ntpd steps or slews
    // the system clock: receivers (ffmpeg/Frigate) recompute all PTS
    // from the SR pair and every stream jumps by the step size
    // ("Non-monotonic DTS" floods, watchdog restarts).
    uint64_t ntpAnchor = 0;        // NTP 32.32 (1900 epoch) at anchor
    int64_t  ntpAnchorMonoUs = -1; // CLOCK_MONOTONIC us at anchor
    int64_t  lastVideoTsUs = -1;   // monotonic us of last video frame start
    int64_t  lastAudioTsUs = -1;   // monotonic us of last audio frame

    time_t lastActivity = 0;

    bool hasValidSession() const { return sessionId[0] != '\0'; }

    // Multi-packet send queue.  Non-blocking sends — queued when EAGAIN.
    // Queue is never capped (memory is the only limit) so NALs are never dropped.
    std::deque<std::vector<uint8_t>> sendQueue;
    size_t sendQueueBytes = 0;

    // Deferred RTSP response (EAGAIN/partial send fallback).  Retried before
    // drain.  Must be large enough for DESCRIBE responses with SDP bodies.
    uint8_t pendingResp[RTSP_BUF_SIZE];
    size_t pendingRespLen = 0;
    size_t pendingRespOff = 0;  // bytes already sent from pendingResp
};

// ═══════════════════════════════════════════════════════════════════════════
// Static helper: non-blocking socket
// ═══════════════════════════════════════════════════════════════════════════

static bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// RtspServer implementation
// ═══════════════════════════════════════════════════════════════════════════

RtspServer::RtspServer() {
    // seed SSRCs
    srand(static_cast<unsigned>(time(nullptr)));
}

RtspServer::~RtspServer() {
    stop();
}

void RtspServer::setAuthCredentials(const std::string &user,
                                    const std::string &pass) {
    username_     = user;
    password_     = pass;
    authRequired_ = !user.empty();
}

void RtspServer::setSendBufferSize(int bytes) { sendBufSize_ = bytes; }
void RtspServer::setSendTimeout(int secs)      { sendTimeoutS_ = secs; }
void RtspServer::setStreamName(const std::string &n)  { streamName_ = n; }
void RtspServer::setStreamInfo(const std::string &i)  { streamInfo_ = i; }

void RtspServer::addVideoStream(int chn, const VideoStreamConfig &config,
                                std::shared_ptr<video_stream> state) {
    videoStreams_.push_back({chn, config, std::move(state)});
}

void RtspServer::addAudioStream(int chn, const AudioStreamConfig &config,
                                std::shared_ptr<audio_stream> state) {
    audioStreams_.push_back({chn, config, std::move(state)});
}

void RtspServer::addAudioOnlyStream(const AudioStreamConfig &config,
                                    std::shared_ptr<audio_stream> state) {
    audioOnlyStreams_.push_back({config, std::move(state)});
}

void RtspServer::enableBackchannel() {
    // Enumerate supported formats from IMPBackchannel
#define ADD_BC(EnumName, NameString, PayloadType, Frequency, MimeType) \
    backchannelFormats_.push_back({NameString, Frequency, PayloadType});
    X_FOREACH_BACKCHANNEL_FORMAT(ADD_BC)
#undef ADD_BC
    backchannelEnabled_ = true;
    LOG_INFO("Backchannel enabled: " << backchannelFormats_.size() << " codecs");
}

void RtspServer::addSubtitleStream(const SubtitleStreamConfig &config) {
    subtitleStreams_.push_back({config});
    LOG_INFO("Subtitle stream registered: " << config.codec
             << " pt=" << config.payloadType);
}

// ── Start / Stop ───────────────────────────────────────────────────────────

bool RtspServer::start(int port) {
    port_ = port > 0 ? port : 554;

    serverFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd_ < 0) {
        LOG_ERROR("socket() failed: " << strerror(errno));
        return false;
    }

    int one = 1;
    setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(serverFd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port_));

    if (bind(serverFd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind() failed: " << strerror(errno));
        close(serverFd_);
        serverFd_ = -1;
        return false;
    }

    if (listen(serverFd_, MAX_CLIENTS) < 0) {
        LOG_ERROR("listen() failed: " << strerror(errno));
        close(serverFd_);
        serverFd_ = -1;
        return false;
    }

    setNonBlocking(serverFd_);

    running_.store(true);
    eventThread_ = std::thread(&RtspServer::eventLoop, this);

    LOG_INFO("SimpleRTSP server started on port " << port_);
    return true;
}

void RtspServer::stop() {
    running_.store(false);
    if (eventThread_.joinable()) {
        eventThread_.join();
    }
    if (serverFd_ >= 0) {
        close(serverFd_);
        serverFd_ = -1;
    }
    // Clean up sessions (taps will be unregistered by destructors)
    cleanupAllSessions();
    LOG_INFO("SimpleRTSP server stopped");
}

void *RtspServer::run(void *arg) {
    auto *self = static_cast<RtspServer *>(arg);
    self->eventLoop();
    return nullptr;
}

// ── Event loop ─────────────────────────────────────────────────────────────

void RtspServer::eventLoop() {
    struct pollfd fds[MAX_CLIENTS + 1];
    Session *sessionMap[MAX_CLIENTS + 1];

    LOG_INFO("Event loop started");
    while (running_.load(std::memory_order_relaxed)) {
        int nfds = 0;

        // Server socket
        fds[nfds].fd     = serverFd_;
        fds[nfds].events = POLLIN;
        sessionMap[nfds]  = nullptr;
        nfds++;

        // Client sockets
        for (auto &s : sessions_) {
            if (s && s->fd >= 0) {
                fds[nfds].fd      = s->fd;
                fds[nfds].events  = POLLIN;
                sessionMap[nfds]   = s.get();
                nfds++;
            }
        }

        // Short timeout when streams are active so we can drain taps
        bool anyPlaying = false;
        for (auto &s : sessions_) {
            if (s && s->playing) { anyPlaying = true; break; }
        }
        int timeoutMs = anyPlaying ? 10 : 1000;

        int ret = poll(fds, nfds, timeoutMs);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("poll() error: " << strerror(errno));
            break;
        }

        // ── Accept new connections ──────────────────────────────────────
        if (fds[0].revents & POLLIN) {
            acceptClient();
        }

        // ── Handle client I/O ───────────────────────────────────────────
        for (int i = 1; i < nfds; i++) {
            if (fds[i].revents & (POLLIN | POLLERR | POLLHUP)) {
                Session *s = sessionMap[i];
                if (!s) continue;

                if (fds[i].revents & (POLLERR | POLLHUP)) {
                    LOG_INFO("POLLERR/POLLHUP on client fd=" << fds[i].fd
                             << " events=" << fds[i].revents);
                    closeClient(s->sessionsIndex);
                    continue;
                }

                handleRequest(s->sessionsIndex);
            }
        }

        // ── Drain taps for playing sessions ─────────────────────────────
        for (auto &s : sessions_) {
            if (!s || !s->playing) continue;
            if (s->videoChn < 0 && !s->audioOnly && !s->backchannel) continue;

            bool backpressure = false;

            // Retry any deferred RTSP response before RTP drains.
            if (s->pendingRespLen > 0) {
                ssize_t n = send(s->fd,
                                 s->pendingResp + s->pendingRespOff,
                                 s->pendingRespLen - s->pendingRespOff,
                                 MSG_NOSIGNAL);
                if (n > 0) {
                    s->pendingRespOff += static_cast<size_t>(n);
                    if (s->pendingRespOff >= s->pendingRespLen) {
                        s->pendingRespLen = 0;
                        s->pendingRespOff = 0;
                    }
                } else if (n < 0 && (errno == EPIPE || errno == ECONNRESET)) {
                    closeClient(s->sessionsIndex);
                    continue;
                }
                // EAGAIN — will retry next cycle (non-blocking poll loop)
            }

            // Drain send queue first — send as many queued packets as socket accepts.
            while (!s->sendQueue.empty()) {
                ssize_t n = send(s->fd,
                                 s->sendQueue.front().data(),
                                 s->sendQueue.front().size(),
                                 MSG_DONTWAIT | MSG_NOSIGNAL);
                if (n > 0 && static_cast<size_t>(n) >= s->sendQueue.front().size()) {
                    s->sendQueueBytes -= s->sendQueue.front().size();
                    s->sendQueue.pop_front();
                } else if (n > 0) {
                    s->sendQueue.front().erase(
                        s->sendQueue.front().begin(),
                        s->sendQueue.front().begin() + static_cast<ptrdiff_t>(n));
                    break; // socket full for now
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    closeClient(s->sessionsIndex);
                    continue; // socket broken
                } else {
                    break; // EAGAIN — socket full
                }
            }

            // Drain video tap — up to 30 NALs per cycle.  On backpressure
            // the NAL is dropped (destructive MsgChannel::read).
            if (s->videoTap) {
                H264NALUnit nal;
                int drained = 0;
                while (!backpressure && drained < 30 && s->videoTap->read(&nal)) {
                    if (!this->sendVideoNal(*s, nal)) {
                        backpressure = true;
                        break;
                    }
                    drained++;
                }
                if (drained > 0) {
                    LOG_DDEBUG("video drain " << drained << " NALs, ch="
                              << s->videoChn << " seq=" << s->videoRtp.seq);
                }
            }

            // Drain audio tap — skip if video hit backpressure
            if (!backpressure && s->audioTap) {
                AudioFrame af;
                int drained = 0;
                while (drained < 200 && s->audioTap->read(&af)) {
                    if (!this->sendAudioFrame(*s, af)) {
                        break;
                    }
                    drained++;
                }
                if (drained > 0)
                    LOG_DDEBUG("audio drain " << drained << " frames");
            }

            // ── Send subtitle update once per second ──────────────────
            if (s->hasSubtitles && s->videoChn >= 0 &&
                s->videoChn < NUM_VIDEO_CHANNELS &&
                global_video[s->videoChn] &&
                global_video[s->videoChn]->imp_encoder &&
                global_video[s->videoChn]->imp_encoder->osd) {
                time_t now = time(nullptr);
                if (now != s->lastSubtitleSent) {
                    s->lastSubtitleSent = now;
                    std::string text =
                        global_video[s->videoChn]->imp_encoder->osd
                            ->getPlaintextInfo();
                    if (!text.empty() && text != s->lastSubtitleText) {
                        s->lastSubtitleText = text;
                        this->sendSubtitleText(*s, text);
                    }
                }
            }

            // Drain orphaned main channels (always — keep encoder flowing)
            if (s->videoChn >= 0 && s->videoChn < NUM_VIDEO_CHANNELS &&
                global_video[s->videoChn] &&
                global_video[s->videoChn]->msgChannel) {
                H264NALUnit dummy;
                while (global_video[s->videoChn]->msgChannel->read(&dummy)) {}
            }
            if (global_audio[0] && global_audio[0]->msgChannel) {
                AudioFrame dummy;
                while (global_audio[0]->msgChannel->read(&dummy)) {}
            }

            // ── Receive backchannel RTP (client → camera audio) ───────
            if (s->backchannel && s->backchannelRtpSock >= 0 &&
                global_backchannel && global_backchannel->inputQueue) {
                uint8_t rtpBuf[2048];
                sockaddr_in fromAddr{};
                socklen_t fromLen = sizeof(fromAddr);
                ssize_t nr = recvfrom(s->backchannelRtpSock,
                                      rtpBuf, sizeof(rtpBuf), MSG_DONTWAIT,
                                      (sockaddr *)&fromAddr, &fromLen);
                while (nr >= 12) {
                    // Parse RTP header: skip 12-byte header, extract payload
                    uint8_t pt = rtpBuf[1] & 0x7F;
                    size_t payloadLen = static_cast<size_t>(nr) - 12;
                    if (payloadLen > 0) {
                        // Determine format from payload type
                        IMPBackchannelFormat fmt = IMPBackchannelFormat::UNKNOWN;
                        for (const auto &bc : backchannelFormats_) {
                            if (bc.payloadType == static_cast<int>(pt)) {
                                if (bc.codec == "PCMU")
                                    fmt = IMPBackchannelFormat::PCMU;
                                else if (bc.codec == "PCMA")
                                    fmt = IMPBackchannelFormat::PCMA;
                                else if (bc.codec == "mpeg4-generic")
                                    fmt = IMPBackchannelFormat::AAC;
                                else if (bc.codec == "OPUS")
                                    fmt = IMPBackchannelFormat::OPUS;
                                break;
                            }
                        }
                        if (fmt != IMPBackchannelFormat::UNKNOWN) {
                            // For AAC, strip AU-header-length + AU-header
                            // (RFC 3640: 2 bytes + 2 bytes per AU)
                            const uint8_t *payload = rtpBuf + 12;
                            if (fmt == IMPBackchannelFormat::AAC &&
                                payloadLen >= 4) {
                                uint16_t auHeaderLen =
                                    (static_cast<uint16_t>(payload[0]) << 8) |
                                    payload[1];
                                uint16_t auHeaderBytes = auHeaderLen / 8;
                                if (auHeaderBytes >= 2 &&
                                    payloadLen >= 4u + auHeaderBytes) {
                                    payload += 2 + auHeaderBytes;
                                    payloadLen -= 2 + auHeaderBytes;
                                }
                            }
                            if (payloadLen > 0 && payloadLen < 2048) {
                                BackchannelFrame frame;
                                frame.payload.assign(payload,
                                                     payload + payloadLen);
                                frame.format = fmt;
                                frame.clientSessionId =
                                    static_cast<unsigned>(s->sessionsIndex);
                                if (!global_backchannel->inputQueue->write(
                                        std::move(frame))) {
                                    LOG_DDEBUG("Backchannel queue full, "
                                               "dropping oldest frame");
                                }
                            }
                        }
                    }
                    // Check for more packets
                    nr = recvfrom(s->backchannelRtpSock,
                                  rtpBuf, sizeof(rtpBuf), MSG_DONTWAIT,
                                  (sockaddr *)&fromAddr, &fromLen);
                }
            }
        }

        // ── RTCP Sender Report every 5s ──────────────────────────────────
        static time_t lastRtcpSr = 0;
        time_t nowT = time(nullptr);
        if (nowT - lastRtcpSr >= 5) {
            lastRtcpSr = nowT;
            for (auto &ss : sessions_) {
                if (ss && ss->playing) sendRtcpSr(*ss);
            }
        }

        // ── Session timeouts ────────────────────────────────────────────

        // ── Session timeouts ────────────────────────────────────────────
        checkSessionTimeouts();
    }

    cleanupAllSessions();
    LOG_INFO("Event loop ended");
}

// ── Accept ─────────────────────────────────────────────────────────────────

void RtspServer::acceptClient() {
    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);
    int fd = accept(serverFd_, (struct sockaddr *)&addr, &addrLen);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            LOG_ERROR("accept() failed: " << strerror(errno));
        return;
    }

    // Find a free session slot
    int slot = -1;
    for (size_t i = 0; i < sessions_.size(); i++) {
        if (!sessions_[i] || sessions_[i]->fd < 0) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0 && sessions_.size() < MAX_CLIENTS) {
        slot = static_cast<int>(sessions_.size());
        sessions_.resize(slot + 1);
    }
    if (slot < 0) {
        LOG_WARN("Max clients reached, rejecting " << inet_ntoa(addr.sin_addr));
        close(fd);
        return;
    }

    setNonBlocking(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (!sessions_[slot])
        sessions_[slot] = std::make_unique<Session>();

    auto &s = sessions_[slot];
    s->fd = fd;
    s->sessionsIndex = slot;
    s->readOff = 0;
    s->playing = false;
    s->startAnchor = {0, 0};
    s->videoStartAnchorUs = -1;
    s->lastFrameRtpTs = 0;
    s->hasFrameRtpTs = false;
    s->hasAudioRtpTs = false;
    s->hasSubtitles = false;
    s->subtitleTcp = false;
    s->lastSubtitleSent = 0;
    s->lastSubtitleText.clear();
    s->subtitleSetupUrl[0] = '\0';
    s->videoChn = -1;
    s->hasAudio = false;
    s->audioOnly = false;
    s->backchannel = false;
    s->backchannelPayloadType = -1;
    s->backchannelInterleavedRtp = 0;
    s->backchannelInterleavedRtcp = 1;
    s->sessionId[0] = '\0';
    s->lastActivity = time(nullptr);
    s->videoRtp = RtpState{};
    s->audioRtp = RtpState{};
    s->videoRtp.ssrc = static_cast<uint32_t>(rand());
    s->audioRtp.ssrc = static_cast<uint32_t>(rand());
    s->videoRtp.seq = static_cast<uint16_t>(rand());
    s->audioRtp.seq = static_cast<uint16_t>(rand());
    s->subtitleRtp = RtpState{};
    s->subtitleRtp.timestamp = 90000;  // start at 1s
    s->subtitleRtp.ssrc = static_cast<uint32_t>(rand());
    s->subtitleRtp.seq = static_cast<uint16_t>(rand());
    s->videoRtp.timestamp = 0;
    s->audioRtp.timestamp = 0;
    s->pendingRespLen = 0;
    s->pendingRespOff = 0;

    // Reset RTCP SR / NTP anchor state so a reused Session slot
    // does not carry stale clock references from a prior connection.
    s->lastVideoTsUs = -1;
    s->lastAudioTsUs = -1;
    s->ntpAnchor = 0;
    s->ntpAnchorMonoUs = -1;

    s->clientAddr = addr;

    LOG_INFO("Client connected: " << inet_ntoa(addr.sin_addr) << ":"
            << ntohs(addr.sin_port));
}

// ── Close ──────────────────────────────────────────────────────────────────

void RtspServer::closeClient(int idx) {
    if (idx < 0 || idx >= static_cast<int>(sessions_.size())) return;
    auto &s = sessions_[idx];
    if (!s) return;
    LOG_INFO("Closing client session " << s->sessionId
             << " fd=" << s->fd
             << " playing=" << s->playing);

    // Decrement active player counts
    if (s->playing) {
        if (s->videoChn >= 0 && s->videoChn < NUM_VIDEO_CHANNELS) {
            activePlayers_[s->videoChn]--;
            if (activePlayers_[s->videoChn] <= 0) {
                activePlayers_[s->videoChn] = 0;
                if (global_video[s->videoChn]) {
                    global_video[s->videoChn]->hasDataCallback.store(
                        false, std::memory_order_relaxed);
                }
            }
        }
        if (s->hasAudio) {
            activeAudioPlayers_--;
            if (activeAudioPlayers_ <= 0) {
                activeAudioPlayers_ = 0;
                if (global_audio[0]) {
                    global_audio[0]->hasDataCallback.store(
                        false, std::memory_order_relaxed);
                }
            }
        }
    }

    // Unregister taps
    if (s->videoTapId != 0 && s->videoChn >= 0) {
        unregister_video_tap(s->videoChn, s->videoTapId);
        s->videoTapId = 0;
    }
    if (s->audioTapId != 0 && global_audio[0]) {
        unregister_audio_tap(0, s->audioTapId);
        s->audioTapId = 0;
    }
    s->videoTap.reset();
    s->audioTap.reset();

    // Close backchannel receive socket.
    // Only decrement is_sending if this session actually incremented it
    // (activated via PLAY or RECORD).  A session that only SETUP the
    // backchannel and disconnected must not drive the count negative,
    // which would permanently block future activations.
    if (s->backchannelActive && global_backchannel) {
        int prev = global_backchannel->is_sending.fetch_sub(1, std::memory_order_acq_rel);
        if (prev <= 1) {
            BackchannelFrame stop{};
            stop.isShutdownSentinel = true;
            global_backchannel->inputQueue->write(stop);
            // Wake the worker so it processes the sentinel even though
            // is_sending just dropped to 0.
            global_backchannel->should_grab_frames.notify_one();
        }
        s->backchannelActive = false;
    }
    s->backchannel = false;
    if (s->backchannelRtpSock >= 0) {
        close(s->backchannelRtpSock);
        s->backchannelRtpSock = -1;
    }

    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    // Close UDP sockets if allocated
    if (s->videoRtpSock   >= 0) { close(s->videoRtpSock);   s->videoRtpSock   = -1; }
    if (s->videoRtcpSock  >= 0) { close(s->videoRtcpSock);  s->videoRtcpSock  = -1; }
    if (s->audioRtpSock   >= 0) { close(s->audioRtpSock);   s->audioRtpSock   = -1; }
    if (s->audioRtcpSock  >= 0) { close(s->audioRtcpSock);  s->audioRtcpSock  = -1; }
    if (s->subtitleRtpSock   >= 0) { close(s->subtitleRtpSock);   s->subtitleRtpSock   = -1; }
    if (s->subtitleRtcpSock  >= 0) { close(s->subtitleRtcpSock);  s->subtitleRtcpSock  = -1; }
    s->hasSubtitles = false;
    s->sessionsIndex = -1;
    s->playing = false;
    s->sendQueue.clear();
    s->sendQueueBytes = 0;
    s->pendingRespLen = 0;
    s->pendingRespOff = 0;
}

void RtspServer::cleanupAllSessions() {
    for (size_t i = 0; i < sessions_.size(); i++)
        closeClient(static_cast<int>(i));
    sessions_.clear();
}

// ── Request handling ───────────────────────────────────────────────────────

void RtspServer::handleRequest(int idx) {
    auto &s = sessions_[idx];
    if (!s || s->fd < 0) return;

    char tmp[RTSP_BUF_SIZE];
    ssize_t n = recv(s->fd, tmp, sizeof(tmp) - 1, 0);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            LOG_DEBUG("Client disconnected (fd=" << s->fd << ")");
            closeClient(idx);
        }
        return;
    }
    s->lastActivity = time(nullptr);

    // Append to read buffer
    if (s->readOff + n >= RTSP_BUF_SIZE) {
        // Copy remote address immediately — inet_ntoa returns a static
        // buffer that the camera-IP lookup below will overwrite.
        char remoteIp[64];
        {
            const char *p = inet_ntoa(s->clientAddr.sin_addr);
            strncpy(remoteIp, p ? p : "0.0.0.0", sizeof(remoteIp) - 1);
        }
        int remotePort = ntohs(s->clientAddr.sin_port);

        // Dump the full accumulated buffer + new data for offline diagnosis.
        const char *dumpBase = cfg ? cfg->general.debug_dump_path : nullptr;
        bool dumped = false;
        if (dumpBase && dumpBase[0] != '\0') {
            char cameraIp[64] = "unknown";
            {
                sockaddr_in localAddr{};
                socklen_t addrLen = sizeof(localAddr);
                if (getsockname(s->fd,
                                reinterpret_cast<sockaddr *>(&localAddr),
                                &addrLen) == 0) {
                    const char *lip = inet_ntoa(localAddr.sin_addr);
                    if (lip) {
                        strncpy(cameraIp, lip, sizeof(cameraIp) - 1);
                    }
                }
            }

            char camDir[384];
            snprintf(camDir, sizeof(camDir), "%s/%s", dumpBase, cameraIp);
            ::mkdir(dumpBase, 0755);
            ::mkdir(camDir, 0755);

            if (access(camDir, W_OK) == 0) {
                char path[512];
                time_t now = time(nullptr);
                struct tm tm;
                localtime_r(&now, &tm);
                snprintf(path, sizeof(path),
                         "%s/overflow-%04d%02d%02d-%02d%02d%02d-%s-%d.bin",
                         camDir,
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                         tm.tm_hour, tm.tm_min, tm.tm_sec,
                         remoteIp, remotePort);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fwrite(s->readBuf, 1,
                           static_cast<size_t>(s->readOff), f);
                    fwrite(tmp, 1, static_cast<size_t>(n), f);
                    fclose(f);
                    dumped = true;
                }
            }
        }

        // Salvage the last complete RTSP request from the overflow so the
        // keepalive still gets handled.  Scan tmp (the most recent data)
        // backwards for \r\n\r\n and keep just that request.
        char *reqEnd = nullptr;
        for (char *p = tmp + n - 4; p >= tmp; p--) {
            if (p[0] == '\r' && p[1] == '\n' &&
                p[2] == '\r' && p[3] == '\n') {
                reqEnd = p + 4;
                break;
            }
        }
        if (reqEnd) {
            // Find the start of this request: byte after previous \r\n\r\n
            char *reqStart = tmp;
            for (char *p = reqEnd - 5; p >= tmp; p--) {
                if (p[0] == '\r' && p[1] == '\n' &&
                    p[2] == '\r' && p[3] == '\n') {
                    reqStart = p + 4;
                    break;
                }
            }
            size_t keepLen = static_cast<size_t>(reqEnd - reqStart);
            size_t trailing  = static_cast<size_t>(tmp + n - reqEnd);
            size_t totalKeep = keepLen + trailing;
            if (totalKeep > 0 && totalKeep < RTSP_BUF_SIZE) {
                memcpy(s->readBuf, reqStart, totalKeep);
                s->readOff = static_cast<int>(totalKeep);
                s->readBuf[s->readOff] = '\0';
            } else {
                s->readOff = 0;
            }
        } else {
            s->readOff = 0;
        }

        LOG_WARN("Request too large (" << s->readOff + n
                 << " bytes) from " << remoteIp << ":"
                 << remotePort << (dumped ? ", dumped" : "")
                 << (s->readOff > 0 ? ", salvaged last request" : ""));
    }
    memcpy(s->readBuf + s->readOff, tmp, static_cast<size_t>(n));
    s->readOff += static_cast<int>(n);
    s->readBuf[s->readOff] = '\0';

    // Strip all consecutive interleaved data frames prefixed with '$'
    while (s->readOff > 0 && s->readBuf[0] == '$') {
        if (s->readOff < 4) return; // incomplete header — wait for more
        uint16_t frameLen = (static_cast<uint8_t>(s->readBuf[2]) << 8)
                          |  static_cast<uint8_t>(s->readBuf[3]);
        size_t total = 4 + frameLen;
        if (static_cast<size_t>(s->readOff) < total) return; // incomplete

        // Capture backchannel audio from TCP interleaved frames
        if (s->backchannel && backchannelEnabled_ &&
            s->readBuf[1] == s->backchannelInterleavedRtp &&
            global_backchannel && global_backchannel->inputQueue &&
            frameLen >= 12) {
            const uint8_t *rtp = (const uint8_t *)s->readBuf + 4;
            uint8_t pt = rtp[1] & 0x7F;
            size_t payloadLen = frameLen - 12;
            IMPBackchannelFormat fmt = IMPBackchannelFormat::UNKNOWN;
            for (const auto &bc : backchannelFormats_) {
                if (bc.payloadType == static_cast<int>(pt)) {
                    if (bc.codec == "PCMU") fmt = IMPBackchannelFormat::PCMU;
                    else if (bc.codec == "PCMA") fmt = IMPBackchannelFormat::PCMA;
                    else if (bc.codec == "mpeg4-generic") fmt = IMPBackchannelFormat::AAC;
                    else if (bc.codec == "OPUS") fmt = IMPBackchannelFormat::OPUS;
                    break;
                }
            }
            if (fmt != IMPBackchannelFormat::UNKNOWN && payloadLen > 0) {
                BackchannelFrame frame;
                frame.payload.assign(rtp + 12, rtp + 12 + payloadLen);
                frame.format = fmt;
                frame.clientSessionId = static_cast<unsigned>(s->sessionsIndex);
                global_backchannel->inputQueue->write(std::move(frame));
            }
        }

        s->readOff -= static_cast<int>(total);
        memmove(s->readBuf, s->readBuf + total, static_cast<size_t>(s->readOff));
        s->readBuf[s->readOff] = '\0';
    }
    if (s->readOff == 0) return; // nothing left to parse

    // Check for complete request (ends with \r\n\r\n).
    // Use a NUL-safe scan — strstr() stops at the first 0x00 byte,
    // which breaks when buggy clients (LibVLC 2.0.3 / LIVE555) send
    // raw RTCP on the control socket (RTCP is full of NULs).
    char *end = nullptr;
    for (int i = 0; i <= s->readOff - 4; i++) {
        if (memcmp(s->readBuf + i, "\r\n\r\n", 4) == 0) {
            end = s->readBuf + i;
            break;
        }
    }
    if (!end) {
        for (int i = 0; i <= s->readOff - 2; i++) {
            if (memcmp(s->readBuf + i, "\n\n", 2) == 0) {
                end = s->readBuf + i;
                break;
            }
        }
        if (!end) return; // incomplete — wait for more data
    }

    // Find the start of this request: byte after the previous \r\n\r\n
    // (or the beginning of the buffer).  Strip any garbage that
    // accumulated before it so sscanf parses the right data.
    {
        char *reqStart = s->readBuf;
        for (char *p = end - 5; p >= s->readBuf; p--) {
            if (memcmp(p, "\r\n\r\n", 4) == 0) {
                reqStart = p + 4;
                break;
            }
        }
        if (reqStart > s->readBuf) {
            ptrdiff_t shift = reqStart - s->readBuf;
            s->readOff -= static_cast<int>(shift);
            memmove(s->readBuf, reqStart, static_cast<size_t>(s->readOff));
            s->readBuf[s->readOff] = '\0';
            end -= shift;
        }
    }

    // Parse request line
    char methodStr[64]{}, uri[256]{}, version[64]{};
    int parsed = sscanf(s->readBuf, "%63s %255s %63s", methodStr, uri, version);
    if (parsed < 2) {
        LOG_WARN("Malformed request: " << s->readBuf);
        sendResponse(*s, Status::BAD_REQUEST, 0, nullptr, nullptr);
        s->readOff = 0;
        return;
    }

    // Sanity check: method must not start with "RTSP/" — that's a server
    // response line, not a client request.  Some clients (ffmpeg) echo
    // response text when their state machine gets confused by backpressure.
    // Instead of closing (which truncates in-flight RTP data), just clear
    // the buffer and let the client recover on its next poll cycle.
    if (strncmp(methodStr, "RTSP/", 5) == 0) {
        LOG_WARN("Client sent RTSP response line — clearing buffer");
        s->readOff = 0;
        s->readBuf[0] = '\0';
        return;
    }

    Method method = parseMethod(methodStr);

    // Find headers — skip the request line and any blank lines that follow.
    char *headersStart = strstr(s->readBuf, "\r\n");
    if (!headersStart) headersStart = strstr(s->readBuf, "\n");
    if (headersStart) {
        if (*headersStart == '\r') headersStart++;
        if (*headersStart == '\n') headersStart++;
        // Skip any additional blank lines (some clients send \r\n\r\n
        // between method line and headers).
        while (*headersStart == '\r' || *headersStart == '\n')
            headersStart++;
    } else {
        headersStart = s->readBuf;
    }

    int cseq = parseCSeq(headersStart);
    if (cseq < 0) {
        // Dump first 200 bytes of request for debugging
        char preview[256];
        int plen = s->readOff < 200 ? s->readOff : 200;
        memcpy(preview, s->readBuf, plen);
        preview[plen] = '\0';
        LOG_WARN("Failed to parse CSeq. Request preview: " << preview);
        cseq = 0;
    }

    // Find request body (for ANNOUNCE, SET_PARAMETER).  Body starts
    // after the double-CRLF that terminates the headers.
    char *bodyStart = nullptr;
    {
        char *endHdrs = strstr(s->readBuf, "\r\n\r\n");
        if (!endHdrs) endHdrs = strstr(s->readBuf, "\n\n");
        if (endHdrs) {
            bodyStart = endHdrs;
            while (*bodyStart == '\r' || *bodyStart == '\n') bodyStart++;
            if (*bodyStart == '\0') bodyStart = nullptr;
        }
    }

    LOG_INFO("RTSP " << methodToString(method) << " " << uri
             << " CSeq=" << cseq);

    // ── Dispatch ────────────────────────────────────────────────────────
    switch (method) {
    case Method::OPTIONS:       handleOptions(idx, cseq);        break;
    case Method::DESCRIBE:      handleDescribe(idx, cseq, uri, headersStart);  break;
    case Method::SETUP:         handleSetup(idx, cseq, uri, headersStart); break;
    case Method::PLAY:          handlePlay(idx, cseq, uri, headersStart);  break;
    case Method::TEARDOWN:      handleTeardown(idx, cseq, headersStart);   break;
    case Method::PAUSE:
        // Pause is a no-op for live streams — acknowledge silently.
        sendResponse(*s, Status::OK, cseq, nullptr, nullptr);
        break;
    case Method::ANNOUNCE:
        handleAnnounce(idx, cseq, uri, headersStart, bodyStart);
        break;
    case Method::RECORD:
        handleRecord(idx, cseq, headersStart);
        break;
    case Method::GET_PARAMETER:
    case Method::SET_PARAMETER:
        LOG_DEBUG("RTSP " << methodToString(method));
        sendResponse(*s, Status::OK, cseq, nullptr, nullptr);
        break;
    default:
        LOG_WARN("Unsupported method (clearing buffer): " << methodStr);
        s->readOff = 0;
        return;
    }

    // Consume the processed request from the buffer.
    // Account for any body (e.g. ANNOUNCE SDP) specified by Content-Length.
    size_t consumed = static_cast<size_t>(end - s->readBuf) + 4; // past \r\n\r\n
    {
        const char *cl = stristr(s->readBuf, "Content-Length:");
        if (cl) {
            int bodyLen = 0;
            if (sscanf(cl, "Content-Length: %d", &bodyLen) == 1 && bodyLen > 0)
                consumed += static_cast<size_t>(bodyLen);
        }
    }
    if (consumed < static_cast<size_t>(s->readOff)) {
        memmove(s->readBuf, s->readBuf + consumed,
                static_cast<size_t>(s->readOff) - consumed);
        s->readOff -= static_cast<int>(consumed);
    } else {
        s->readOff = 0;
    }
}

// ── OPTIONS ────────────────────────────────────────────────────────────────

void RtspServer::handleOptions(int idx, int cseq) {
    auto &s = sessions_[idx];
    sendResponse(*s, Status::OK, cseq,
                 "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, "
                 "PAUSE, GET_PARAMETER, SET_PARAMETER\r\n",
                 nullptr);
}

// ── DESCRIBE ───────────────────────────────────────────────────────────────

void RtspServer::handleDescribe(int idx, int cseq, const char *uri,
                               const char *headers) {
    auto &s = sessions_[idx];

    // Check if client requested ONVIF backchannel (Section 5.3)
    bool clientWantsBackchannel =
        headers && stristr(headers, "Require:") &&
        stristr(headers, "www.onvif.org/ver20/backchannel");

    // ── Check audio-only endpoints first ──────────────────────────────
    for (size_t i = 0; i < audioOnlyStreams_.size(); i++) {
        const auto &acfg = audioOnlyStreams_[i].config;
        if (!acfg.endpoint.empty() && strstr(uri, acfg.endpoint.c_str())) {
            struct sockaddr_in localAddr;
            socklen_t len = sizeof(localAddr);
            char serverIp[64] = "0.0.0.0";
            if (getsockname(s->fd, (struct sockaddr *)&localAddr, &len) == 0)
                inet_ntop(AF_INET, &localAddr.sin_addr, serverIp, sizeof(serverIp));

            std::string sdp = generateAudioOnlySdp(acfg, serverIp, streamName_.c_str());
            char hdr[256];
            snprintf(hdr, sizeof(hdr),
                     "Content-Type: application/sdp\r\n"
                     "Content-Length: %zu\r\n", sdp.size());
            sendResponse(*s, Status::OK, cseq, hdr, sdp.c_str());
            return;
        }
    }

    // ── Backchannel probe (e.g. /backchannel) ────────────────────────
    if (strstr(uri, "backchannel")) {
        struct sockaddr_in localAddr;
        socklen_t len = sizeof(localAddr);
        char serverIp[64] = "0.0.0.0";
        if (getsockname(s->fd, (struct sockaddr *)&localAddr, &len) == 0)
            inet_ntop(AF_INET, &localAddr.sin_addr, serverIp, sizeof(serverIp));

        // Always return backchannel SDP — never fall through to video.
        // When disabled, the generator falls back to a basic PCMU track.
        std::vector<BackchannelConfig> fmts;
        if (backchannelEnabled_)
            fmts = backchannelFormats_;
        std::string sdp = generateBackchannelSdp(fmts,
                                                  serverIp, streamName_.c_str());
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
                 "Content-Type: application/sdp\r\n"
                 "Content-Length: %zu\r\n", sdp.size());
        sendResponse(*s, Status::OK, cseq, hdr, sdp.c_str());
        return;
    }

    // Find which stream this URI refers to
    // URI could be: rtsp://host:port/ch0  or just /ch0
    int videoIdx = -1;
    for (size_t i = 0; i < videoStreams_.size(); i++) {
        if (strstr(uri, videoStreams_[i].config.endpoint.c_str())) {
            videoIdx = static_cast<int>(i);
            break;
        }
    }
    if (videoIdx < 0 && !videoStreams_.empty())
        videoIdx = 0; // default to first stream

    if (videoIdx < 0) {
        sendResponse(*s, Status::NOT_FOUND, cseq, nullptr, nullptr);
        return;
    }

    // Update codec config from latest encoder state
    auto &ve = videoStreams_[static_cast<size_t>(videoIdx)];
    {
        auto &vs = ve.state;
        std::lock_guard<std::mutex> lock(vs->codec_config_mutex);
        if (vs->have_sps) {
            ve.config.sps = vs->latest_sps;
            ve.config.pps = vs->latest_pps;
            if (vs->have_vps) ve.config.vps = vs->latest_vps;
            ve.config.haveCodecConfig = true;
        }
    }

    const AudioStreamConfig *audioCfg = nullptr;
    AudioStreamConfig tmpAudio;
    if (!audioStreams_.empty()) {
        tmpAudio = audioStreams_[0].config;
        audioCfg = &tmpAudio;
    }

    // Get server IP from the socket
    struct sockaddr_in localAddr;
    socklen_t len = sizeof(localAddr);
    char serverIp[64] = "0.0.0.0";
    if (getsockname(s->fd, (struct sockaddr *)&localAddr, &len) == 0) {
        inet_ntop(AF_INET, &localAddr.sin_addr, serverIp, sizeof(serverIp));
    }

    const std::vector<BackchannelConfig> *bcfg =
        (backchannelEnabled_ && clientWantsBackchannel)
            ? &backchannelFormats_ : nullptr;
    const SubtitleStreamConfig *scfg =
        subtitleStreams_.empty() ? nullptr : &subtitleStreams_[0].config;
    std::string sdp = generateSdp(ve.config, audioCfg, serverIp, streamName_.c_str(),
                                  bcfg, scfg);

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "Content-Type: application/sdp\r\n"
             "Content-Length: %zu\r\n",
             sdp.size());

    sendResponse(*s, Status::OK, cseq, hdr, sdp.c_str());
}

// ── SETUP ──────────────────────────────────────────────────────────────────

void RtspServer::handleSetup(int idx, int cseq, const char *uri,
                             const char *headers) {
    auto &s = sessions_[idx];

    // ── Detect stream type from URI first ──────────────────────────────
    // Check audio-only endpoints (/mic, etc.)
    bool isAudioOnlyEndpoint = false;
    for (const auto &entry : audioOnlyStreams_) {
        if (!entry.config.endpoint.empty() && strstr(uri, entry.config.endpoint.c_str())) {
            isAudioOnlyEndpoint = true;
            s->audioOnly = true;
            break;
        }
    }

    bool isAudio = (strstr(uri, "track2") != nullptr);
    bool isVideo = (strstr(uri, "track1") != nullptr);
    bool isSubtitle = (strstr(uri, "track4") != nullptr) &&
                       !subtitleStreams_.empty();
    // Backchannel: client negotiated via ANNOUNCE, then SETUPs
    // with the SDP's control URL (typically "track0" or similar).
    bool isBackchannel = (strstr(uri, "track3") != nullptr ||
                          strstr(uri, "track0") != nullptr ||
                          strstr(uri, "backchannel") != nullptr) &&
                          backchannelEnabled_;

    if (isBackchannel && backchannelEnabled_) {
        handleBackchannelSetup(idx, cseq, uri, headers);
        return;
    }

    // Subtitle track setup
    if (isSubtitle) {
        handleSubtitleSetup(*s, headers, uri);
        char subHdr[256];
        if (s->subtitleTcp) {
            snprintf(subHdr, sizeof(subHdr),
                     "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n"
                     "Session: %s;timeout=65\r\n",
                     s->subtitleInterleavedRtp, s->subtitleInterleavedRtcp,
                     s->sessionId);
        } else {
            snprintf(subHdr, sizeof(subHdr),
                     "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
                     "Session: %s\r\n",
                     s->subtitleClientRtpPort, s->subtitleClientRtcpPort,
                     s->subtitleServerRtpPort, s->subtitleServerRtcpPort,
                     s->sessionId);
        }
        sendResponse(*s, Status::OK, cseq, subHdr, nullptr);
        return;
    }

    // Audio-only endpoints: track1 is audio
    if (isAudioOnlyEndpoint && isVideo) {
        isVideo = false;
        isAudio = true;
    }

    // Fallback: if no trackID, assume first is video (for legacy clients)
    if (!isAudio && !isVideo) {
        if (isAudioOnlyEndpoint) {
            isAudio = true;
        } else {
            isVideo = (s->videoChn < 0);
            isAudio = !isVideo && !audioStreams_.empty();
        }
    }

    // ── Parse transport ────────────────────────────────────────────────
    const char *t = stristr(headers, "Transport:");
    if (t && stristr(t, "RTP/AVP/TCP")) {
        s->tcpInterleaved = true;
        const char *il = strstr(t, "interleaved=");
        if (il) {
            int rtpCh, rtcpCh;
            if (sscanf(il, "interleaved=%d-%d", &rtpCh, &rtcpCh) == 2) {
                if (isAudio) {
                    s->audioInterleavedRtp  = static_cast<uint8_t>(rtpCh);
                    s->audioInterleavedRtcp = static_cast<uint8_t>(rtcpCh);
                } else {
                    s->videoInterleavedRtp  = static_cast<uint8_t>(rtpCh);
                    s->videoInterleavedRtcp = static_cast<uint8_t>(rtcpCh);
                }
            }
        }
    } else if (t && stristr(t, "RTP/AVP")) {
        // UDP transport: parse client_port and create server UDP sockets
        s->tcpInterleaved = false;

        // Save client address for sendto
        socklen_t alen = sizeof(s->clientAddr);
        if (getpeername(s->fd, (sockaddr *)&s->clientAddr, &alen) == 0)
            s->clientAddrLen = alen;

        // Parse client ports
        int clientRtpPort = 0, clientRtcpPort = 0;
        const char *cp = stristr(t, "client_port=");
        if (cp)
            sscanf(cp, "client_port=%d-%d", &clientRtpPort, &clientRtcpPort);

        // Save client target ports per stream
        uint16_t *outClientRtp  = isVideo ? &s->videoClientRtpPort  : &s->audioClientRtpPort;
        uint16_t *outClientRtcp = isVideo ? &s->videoClientRtcpPort : &s->audioClientRtcpPort;
        *outClientRtp  = static_cast<uint16_t>(clientRtpPort  > 0 ? clientRtpPort  : 5004);
        *outClientRtcp = static_cast<uint16_t>(clientRtcpPort > 0 ? clientRtcpPort : 5005);

        // Create and bind server UDP sockets (any available port)
        auto createUdpSocket = [](uint16_t &outPort) -> int {
            int sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock >= 0) {
                setNonBlocking(sock);
                sockaddr_in addr{};
                addr.sin_family      = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_ANY);
                addr.sin_port        = 0; // let kernel pick
                bind(sock, (sockaddr *)&addr, sizeof(addr));
                socklen_t slen = sizeof(addr);
                sockaddr_in bound{};
                getsockname(sock, (sockaddr *)&bound, &slen);
                outPort = ntohs(bound.sin_port);
            }
            return sock;
        };

        if (isVideo) {
            s->videoRtpSock   = createUdpSocket(s->videoServerRtpPort);
            s->videoRtcpSock  = createUdpSocket(s->videoServerRtcpPort);
        } else {
            s->audioRtpSock   = createUdpSocket(s->audioServerRtpPort);
            s->audioRtcpSock  = createUdpSocket(s->audioServerRtcpPort);
        }

        // Size the UDP RTP send buffers generously. A 1080p IDR frame is a
        // burst of ~80+ fragments (~120KB); the default Linux UDP send buffer
        // (~16-64KB) overflows under that burst, sendto() returns EAGAIN, and
        // the packetizer drops the rest of the NAL -> decoder desync. A 1MB
        // buffer holds a full IDR burst so fragments are never dropped locally.
        auto setUdpSendBuf = [](int sock, int size) {
            if (sock >= 0 && size > 0)
                setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
        };
        int videoUdpBuf = 1024 * 1024;
        int audioUdpBuf = 64 * 1024;
        if (isVideo) {
            setUdpSendBuf(s->videoRtpSock,  videoUdpBuf);
            setUdpSendBuf(s->videoRtcpSock, audioUdpBuf);
        } else {
            setUdpSendBuf(s->audioRtpSock,  audioUdpBuf);
            setUdpSendBuf(s->audioRtcpSock, audioUdpBuf);
        }
    }

    // ── Assign stream ──────────────────────────────────────────────────
    if (isVideo) {
        strncpy(s->videoSetupUrl, uri, sizeof(s->videoSetupUrl) - 1);
        int vIdx = -1;
        for (size_t i = 0; i < videoStreams_.size(); i++) {
            if (strstr(uri, videoStreams_[i].config.endpoint.c_str())) {
                vIdx = static_cast<int>(i);
                break;
            }
        }
        if (vIdx < 0 && !videoStreams_.empty()) vIdx = 0;

        if (vIdx >= 0) {
            s->videoChn = videoStreams_[static_cast<size_t>(vIdx)].chn;
            if (sendBufSize_ > 0) {
                setsockopt(s->fd, SOL_SOCKET, SO_SNDBUF,
                           &sendBufSize_, sizeof(sendBufSize_));
            }
            if (sendTimeoutS_ > 0) {
                struct timeval tv;
                tv.tv_sec  = sendTimeoutS_;
                tv.tv_usec = 0;
                setsockopt(s->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            }
        }
    } else if (isAudio) {
        strncpy(s->audioSetupUrl, uri, sizeof(s->audioSetupUrl) - 1);
        s->hasAudio = true;
    }

    // Generate session ID on first SETUP (if not already set)
    if (!s->hasValidSession()) {
        snprintf(s->sessionId, sizeof(s->sessionId), "%08X",
                 static_cast<unsigned>(time(nullptr)) ^
                 static_cast<unsigned>(rand()));
    }

    // Build transport response — per-track interleaved channels
    char hdr[512];
    if (s->tcpInterleaved) {
        int rtpCh  = isVideo ? s->videoInterleavedRtp  : s->audioInterleavedRtp;
        int rtcpCh = isVideo ? s->videoInterleavedRtcp : s->audioInterleavedRtcp;
        snprintf(hdr, sizeof(hdr),
                 "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n"
                 "Session: %s;timeout=65\r\n",
                 rtpCh, rtcpCh, s->sessionId);
    } else {
        // UDP: report server ports so client knows where to receive from
        if (isVideo) {
            snprintf(hdr, sizeof(hdr),
                     "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
                     "Session: %s\r\n",
                     s->videoClientRtpPort, s->videoClientRtcpPort,
                     s->videoServerRtpPort, s->videoServerRtcpPort,
                     s->sessionId);
        } else {
            snprintf(hdr, sizeof(hdr),
                     "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
                     "Session: %s\r\n",
                     s->audioClientRtpPort, s->audioClientRtcpPort,
                     s->audioServerRtpPort, s->audioServerRtcpPort,
                     s->sessionId);
        }
    }

    sendResponse(*s, Status::OK, cseq, hdr, nullptr);
}

// ── PLAY ───────────────────────────────────────────────────────────────────

void RtspServer::handlePlay(int idx, int cseq, const char *uri,
                            const char *headers) {
    auto &s = sessions_[idx];

    // Validate session
    const char *sh = stristr(headers, "Session:");
    if (!sh || !s->hasValidSession()) {
        sendResponse(*s, Status::SESSION_NOT_FOUND, cseq, nullptr, nullptr);
        return;
    }

    if (s->videoChn < 0 && !s->hasAudio && !s->audioOnly && !s->backchannel &&
        !s->hasSubtitles) {
        sendResponse(*s, Status::BAD_REQUEST, cseq, nullptr, nullptr);
        return;
    }

    // ── Set up video tap ────────────────────────────────────────────────
    if (s->videoChn >= 0 && !s->videoTap) {
        s->videoTap = std::make_shared<MsgChannel<H264NALUnit>>(MSG_CHANNEL_SIZE * 2);
        s->videoTapId = register_video_tap(
            s->videoChn,
            s->videoTap,
            []() {})
            .id;

        // Ensure video frames flow — set hasDataCallback
        if (s->videoChn < NUM_VIDEO_CHANNELS && global_video[s->videoChn]) {
            global_video[s->videoChn]->hasDataCallback.store(
                true, std::memory_order_relaxed);
            global_video[s->videoChn]->should_grab_frames.notify_one();
            // Request fresh IDR so client gets SPS/PPS immediately
            IMP_Encoder_RequestIDR(s->videoChn);
        }

        s->codecConfigSent = false;
        s->waitingForKeyframe = false;
        s->videoFrameCount = 0;
        s->videoStartAnchorUs = -1;
        s->lastFrameRtpTs = 0;
        s->hasFrameRtpTs = false;

        // Flush any NALs that accumulated in the tap between
        // registration and the first drain cycle.  Without this,
        // stale frames arrive in a burst and ffplay's jitter
        // buffer overflows on startup.
        s->videoTap->clear();
        s->hasAudioRtpTs = false;
        if (s->videoChn < NUM_VIDEO_CHANNELS)
            activePlayers_[s->videoChn]++;
    }

    // ── Set up audio tap ────────────────────────────────────────────────
    if (s->hasAudio && !s->audioTap && global_audio[0]) {
        s->audioTap = std::make_shared<MsgChannel<AudioFrame>>(MSG_CHANNEL_SIZE * 3);
        s->audioTapId = register_audio_tap(
            0, s->audioTap,
            []() {})
            .id;

        global_audio[0]->hasDataCallback.store(
            true, std::memory_order_relaxed);
        global_audio[0]->should_grab_frames.notify_one();

        activeAudioPlayers_++;
    }

    s->playing = true;

    LOG_INFO("PLAY started: ch=" << s->videoChn
             << " hasAudio=" << s->hasAudio
             << " hasSubtitles=" << s->hasSubtitles
             << " players=" << (s->videoChn >= 0 ? activePlayers_[s->videoChn] : 0));

    // ── Activate backchannel on PLAY (ONVIF Streaming Spec §5.3) ────
    // go2rtc sends PLAY to start the backchannel, not RECORD.
    // Guard per-session (not on the global count) so that multiple
    // concurrent backchannel sessions each pair one increment with one
    // decrement on close.
    if (s->backchannel && global_backchannel && !s->backchannelActive) {
        s->backchannelActive = true;
        global_backchannel->is_sending.fetch_add(1, std::memory_order_release);
        global_backchannel->should_grab_frames.notify_one();
        LOG_INFO("Backchannel activated via PLAY");
    }

    // ── Send RTSP response ─────────────────────────────────────────────
    char hdr[1024];
    // Single-stream RTP-Info: dual-stream RTP-Info blocks ffmpeg >=7
    // even when no audio data is sent.
    snprintf(hdr, sizeof(hdr),
             "Session: %s\r\n"
             "Range: npt=0.000-\r\n"
             "RTP-Info: url=%s;seq=%u;rtptime=%u\r\n",
             s->sessionId,
             s->videoSetupUrl[0] ? s->videoSetupUrl : uri,
             s->videoRtp.seq,
             s->videoRtp.timestamp);

    sendResponse(*s, Status::OK, cseq, hdr, nullptr);

    // Send initial RTCP SR immediately after the PLAY response.
    // The RTP-Info header above advertises rtptime=0; this SR pairs
    // that same RTP timestamp 0 with the current NTP wall-clock time
    // so receivers can compute PTS from the very first RTP packet.
    //
    // Burst 3 copies with a 5 ms gap between each.  For TCP interleaved
    // transport the first copy is sufficient (TCP ordering guarantees it
    // precedes RTP data).  For UDP transport the SR travels on a separate
    // socket from RTP data; the burst compensates for UDP's lack of
    // ordering and occasional packet loss.  Receivers that see duplicate
    // SRs will simply update their NTP→RTP mapping to the same values.
    sendRtcpSr(*s);
    usleep(5000);
    sendRtcpSr(*s);
    usleep(5000);
    sendRtcpSr(*s);
}

// ── TEARDOWN ───────────────────────────────────────────────────────────────

void RtspServer::handleTeardown(int idx, int cseq, const char *headers) {
    auto &s = sessions_[idx];
    const char *sh = stristr(headers, "Session:");
    (void)sh;

    sendResponse(*s, Status::OK, cseq, nullptr, nullptr);

    // Clean up this session
    closeClient(idx);
}

// ── ANNOUNCE (backchannel SDP from client) ─────────────────────────────────

void RtspServer::handleAnnounce(int idx, int cseq, const char *,
                                const char *, const char *body) {
    auto &s = sessions_[idx];
    if (!backchannelEnabled_) {
        sendResponse(*s, Status::NOT_FOUND, cseq, nullptr, nullptr);
        return;
    }
    // Parse the client's SDP to detect audio codec.  Look for
    // "m=audio" and extract the payload type, then match rtpmap.
    int pt = -1;
    if (body) {
        const char *m = stristr(body, "m=audio");
        if (m) {
            int rtpPort;
            int parsedPt;
            if (sscanf(m, "m=audio %d RTP/AVP %d", &rtpPort, &parsedPt) == 2)
                pt = parsedPt;
            else if (sscanf(m, "m=audio %d RTP/AVP %d", &rtpPort, &parsedPt) == 1) {
                // Some clients put PT on next line
                const char *a = stristr(body, "a=rtpmap:");
                if (a) sscanf(a, "a=rtpmap:%d", &parsedPt);
                pt = parsedPt;
            }
        }
    }
    if (pt < 0) {
        // Default to PCMU if we can't parse
        pt = 0;
    }
    // Validate against supported formats
    bool valid = false;
    for (const auto &bc : backchannelFormats_) {
        if (bc.payloadType == pt) { valid = true; break; }
    }
    if (!valid) {
        LOG_WARN("Backchannel: unsupported PT " << pt << ", falling back to PCMU");
        pt = 0;
    }
    s->backchannelPayloadType = pt;
    // Generate session ID for subsequent SETUP/RECORD
    if (!s->hasValidSession()) {
        snprintf(s->sessionId, sizeof(s->sessionId), "%08X",
                 static_cast<unsigned>(time(nullptr)) ^
                 static_cast<unsigned>(rand()));
    }
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "Session: %s\r\n", s->sessionId);
    LOG_INFO("Backchannel ANNOUNCE: PT=" << pt << " session=" << s->sessionId);
    sendResponse(*s, Status::OK, cseq, hdr, nullptr);
}

// ── RECORD (start backchannel) ──────────────────────────────────────────

void RtspServer::handleRecord(int idx, int cseq, const char *) {
    auto &s = sessions_[idx];
    if (!backchannelEnabled_ || (s->backchannelPayloadType < 0 && backchannelFormats_.empty())) {
        sendResponse(*s, Status::NOT_FOUND, cseq, nullptr, nullptr);
        return;
    }
    s->backchannel = true;
    s->playing = true;  // needed for event loop to drain this session
    // Notify BackchannelWorker that data may arrive (once per session;
    // repeated RECORD must not increment is_sending twice)
    if (global_backchannel && !s->backchannelActive) {
        s->backchannelActive = true;
        global_backchannel->is_sending.fetch_add(1, std::memory_order_release);
        global_backchannel->should_grab_frames.notify_one();
    }
    LOG_INFO("Backchannel RECORD started, PT=" << s->backchannelPayloadType);
    sendResponse(*s, Status::OK, cseq, nullptr, nullptr);
}

// ── Backchannel SETUP ──────────────────────────────────────────────────

void RtspServer::handleSubtitleSetup(Session &s, const char *headers,
                                     const char *uri) {
    // Extract video channel from URI (e.g. /ch0/track4 → chn=0)
    if (s.videoChn < 0 && uri) {
        const char *ch = strstr(uri, "/ch");
        if (ch) {
            int c = atoi(ch + 3);
            if (c >= 0 && c < NUM_VIDEO_CHANNELS)
                s.videoChn = c;
        }
    }
    // Generate session ID if not already set
    if (!s.hasValidSession()) {
        snprintf(s.sessionId, sizeof(s.sessionId), "%08X",
                 static_cast<unsigned>(time(nullptr)) ^
                 static_cast<unsigned>(rand()));
    }
    const char *t = stristr(headers, "Transport:");
    LOG_DEBUG("subtitle SETUP: transport=" << (t ? t : "(null)"));
    if (t && stristr(t, "RTP/AVP/TCP")) {
        s.subtitleTcp = true;
        const char *il = strstr(t, "interleaved=");
        if (il) {
            int rtpCh, rtcpCh;
            if (sscanf(il, "interleaved=%d-%d", &rtpCh, &rtcpCh) == 2) {
                s.subtitleInterleavedRtp  = static_cast<uint8_t>(rtpCh);
                s.subtitleInterleavedRtcp = static_cast<uint8_t>(rtcpCh);
                LOG_INFO("subtitle: TCP interleaved=" << rtpCh << "-" << rtcpCh);
            }
        }
    } else if (t && stristr(t, "RTP/AVP")) {
        // UDP: parse client ports and create server sockets
        int clientRtpPort = 0, clientRtcpPort = 0;
        const char *cp = stristr(t, "client_port=");
        if (cp)
            sscanf(cp, "client_port=%d-%d", &clientRtpPort, &clientRtcpPort);
        s.subtitleClientRtpPort  = static_cast<uint16_t>(clientRtpPort  > 0 ? clientRtpPort  : 5008);
        s.subtitleClientRtcpPort = static_cast<uint16_t>(clientRtcpPort > 0 ? clientRtcpPort : 5009);

        // Save client address for sendto — without this, subtitle
        // packets go to 0.0.0.0 and are silently dropped.
        socklen_t alen = sizeof(s.clientAddr);
        if (getpeername(s.fd, (sockaddr *)&s.clientAddr, &alen) == 0)
            s.clientAddrLen = alen;

        auto createUdpSocket = [](uint16_t &outPort) -> int {
            int sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock >= 0) {
                setNonBlocking(sock);
                sockaddr_in addr{};
                addr.sin_family      = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_ANY);
                addr.sin_port        = 0;
                bind(sock, (sockaddr *)&addr, sizeof(addr));
                socklen_t slen = sizeof(addr);
                sockaddr_in bound{};
                getsockname(sock, (sockaddr *)&bound, &slen);
                outPort = ntohs(bound.sin_port);
            }
            return sock;
        };
        s.subtitleRtpSock   = createUdpSocket(s.subtitleServerRtpPort);
        s.subtitleRtcpSock  = createUdpSocket(s.subtitleServerRtcpPort);
    }
    s.hasSubtitles = true;
}

void RtspServer::handleBackchannelSetup(int idx, int cseq,
                                        const char *uri, const char *headers) {
    auto &s = sessions_[idx];

    // If no ANNOUNCE was sent, pick first available codec
    if (s->backchannelPayloadType < 0 && !backchannelFormats_.empty())
        s->backchannelPayloadType = backchannelFormats_[0].payloadType;

    const char *t = stristr(headers, "Transport:");

    if (t && stristr(t, "RTP/AVP/TCP")) {
        s->tcpInterleaved = true;
        s->backchannel = true;  // allow PLAY to succeed before RECORD
        const char *il = strstr(t, "interleaved=");
        if (il) {
            int rtpCh, rtcpCh;
            if (sscanf(il, "interleaved=%d-%d", &rtpCh, &rtcpCh) == 2) {
                s->backchannelInterleavedRtp  = static_cast<uint8_t>(rtpCh);
                s->backchannelInterleavedRtcp = static_cast<uint8_t>(rtcpCh);
            }
        }
        // Generate session ID
        if (!s->hasValidSession()) {
            snprintf(s->sessionId, sizeof(s->sessionId), "%08X",
                     static_cast<unsigned>(time(nullptr)) ^
                     static_cast<unsigned>(rand()));
        }
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
                 "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n"
                 "Session: %s;timeout=65\r\n",
                 s->backchannelInterleavedRtp, s->backchannelInterleavedRtcp,
                 s->sessionId);
        sendResponse(*s, Status::OK, cseq, hdr, nullptr);
        return;
    }

    // UDP fallback
    // (TCP handled above, UDP below)

    // Parse client RTP port
    int clientRtpPort = 0, clientRtcpPort = 0;
    if (t) {
        const char *cp = stristr(t, "client_port=");
        if (cp)
            sscanf(cp, "client_port=%d-%d", &clientRtpPort, &clientRtcpPort);
    }
    s->backchannelClientRtpPort = static_cast<uint16_t>(clientRtpPort > 0 ? clientRtpPort : 5004);

    // Save client address for recvfrom identification
    socklen_t alen = sizeof(s->clientAddr);
    if (getpeername(s->fd, (sockaddr *)&s->clientAddr, &alen) != 0)
        s->clientAddrLen = 0;
    else
        s->clientAddrLen = alen;

    // Create UDP socket to receive backchannel RTP
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) {
        setNonBlocking(sock);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = 0;
        bind(sock, (sockaddr *)&addr, sizeof(addr));
        socklen_t slen = sizeof(addr);
        sockaddr_in bound{};
        getsockname(sock, (sockaddr *)&bound, &slen);
        s->backchannelServerRtpPort = ntohs(bound.sin_port);
        s->backchannelRtpSock = sock;
    }

    if (s->backchannelRtpSock < 0) {
        sendResponse(*s, Status::INTERNAL_ERROR, cseq, nullptr, nullptr);
        return;
    }

    // Mark the session as a backchannel receiver so that PLAY activates
    // it (ONVIF §5.3) and the event loop reads the UDP socket.  The TCP
    // interleaved branch above does the same; without this, UDP clients
    // that activate via PLAY were silently ignored.
    s->backchannel = true;

    // Generate session ID on first SETUP
    if (!s->hasValidSession()) {
        snprintf(s->sessionId, sizeof(s->sessionId), "%08X",
                 static_cast<unsigned>(time(nullptr)) ^
                 static_cast<unsigned>(rand()));
    }

    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
             "Session: %s\r\n",
             s->backchannelClientRtpPort,
             s->backchannelClientRtpPort + 1,
             s->backchannelServerRtpPort,
             s->backchannelServerRtpPort + 1,
             s->sessionId);
    sendResponse(*s, Status::OK, cseq, hdr, nullptr);
}

// ── Response sending ───────────────────────────────────────────────────────

void RtspServer::sendResponse(Session &s, Status status, int cseq,
                               const char *extraHeaders, const char *body) {
    char buf[RTSP_BUF_SIZE];
    int len;

    len = snprintf(buf, sizeof(buf),
                   "RTSP/1.0 %d %s\r\n"
                   "CSeq: %d\r\n"
                   "%s"
                   "\r\n"
                   "%s",
                   static_cast<int>(status), statusToString(status),
                   cseq,
                   extraHeaders ? extraHeaders : "",
                   body ? body : "");

    LOG_INFO("RESPONSE CSeq=" << cseq << " status=" << static_cast<int>(status)
             << " len=" << len);

    // Ensure the response ends with \r\n
    if (len < 2 || (len >= 2 && (buf[len-2] != '\r' || buf[len-1] != '\n'))) {
        if (static_cast<size_t>(len) + 2 < sizeof(buf)) {
            buf[len++] = '\r';
            buf[len++] = '\n';
        }
    }

    ssize_t sent = send(s.fd, buf, static_cast<size_t>(len), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent >= 0) {
        // Partial send — buffer the remainder for retry
        size_t written = static_cast<size_t>(sent);
        if (written < static_cast<size_t>(len)) {
            size_t remain = static_cast<size_t>(len) - written;
            size_t copy = remain;
            if (copy > sizeof(s.pendingResp)) copy = sizeof(s.pendingResp);
            memcpy(s.pendingResp, buf + written, copy);
            s.pendingRespLen = copy;
            s.pendingRespOff = 0;
        }
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Full send failed — buffer entire response for retry
        LOG_WARN("sendResponse EAGAIN — deferring");
        size_t copy = static_cast<size_t>(len);
        if (copy > sizeof(s.pendingResp)) copy = sizeof(s.pendingResp);
        memcpy(s.pendingResp, buf, copy);
        s.pendingRespLen = copy;
        s.pendingRespOff = 0;
    }
    // Other errors: silently drop (client will timeout and reconnect)
}

// ── RTP sending (video) ────────────────────────────────────────────────────

bool RtspServer::sendVideoNal(Session &s, const H264NALUnit &nal) {
    if (s.fd < 0) return false;
    if (nal.data.empty()) return false;

    // Strip start code if present.  Regular encoder NALs have no start code
    // (VideoWorker strips them), but injected SEI NALs include 4-byte start
    // codes.  Only strip if we see an exact match.
    const uint8_t *raw = nal.data.data();
    size_t rawLen = nal.data.size();
    size_t offset = 0;
    if (rawLen >= 4 && raw[0] == 0 && raw[1] == 0 && raw[2] == 0 && raw[3] == 1) {
        offset = 4;
    } else if (rawLen >= 3 && raw[0] == 0 && raw[1] == 0 && raw[2] == 1) {
        offset = 3;
    }
    if (offset >= rawLen) return false;

    const uint8_t *nalData = raw + offset;
    size_t nalLen = rawLen - offset;
    if (nalLen == 0) return false;

    // ── Determine codec from the registered stream ───────────────────
    bool isH265 = false;
    uint8_t pt = 96;
    for (auto &ve : videoStreams_) {
        if (ve.chn == s.videoChn) {
            isH265 = (ve.config.codec == "H265");
            break;
        }
    }

    // ── Timestamp ── encoder monotonic clock (90 kHz RTP) ────────────
    // Use imp_ts (microseconds from encoder, smoothed by VideoWorker)
    // anchored to the first picture-bearing frame of the session.
    // This produces RTP timestamps that match the actual frame cadence,
    // so a client's jitter buffer never starves when the encoder falls
    // below the configured framerate (e.g. 12 fps actual vs 30 fps
    // configured).  The old frame-counter approach always ticked at the
    // *configured* fps, causing mpv to consume buffered frames at 30
    // fps and then enter buffering.
    //
    // Anchoring is deferred past config-only NALs (SPS/PPS/VPS/SEI).
    // The IMP encoder may emit SPS+PPS as a separate frame-start
    // boundary before the IDR, and if the anchor is consumed on the
    // config NALs the entire initial burst shares ts=0, overflowing
    // the client jitter buffer and yielding "No video PTS" in mpv.
    if (nal.is_frame_start) {
        int64_t ts_us = nal.imp_ts;

        // Determine whether this NAL carries picture data (VCL).
        // SPS/PPS/VPS/SEI/AUD are non-VCL and should not anchor.
        bool isPictureNal = true;
        if (nalLen > 0) {
            if (isH265 && nalLen >= 2) {
                uint8_t t = (nalData[0] >> 1) & 0x3F;
                isPictureNal = (t <= 31);  // H.265 VCL: types 0-31
            } else {
                uint8_t t = nalData[0] & 0x1F;
                isPictureNal = (t == 1 || t == 5);  // H.264: non-IDR / IDR slice
            }
        }

        // Defer anchor when this is a config-only NAL and we haven't
        // anchored yet.  Advance the timestamp by a minimal amount
        // so the RTP clock stays monotonic across the config burst.
        if (s.videoStartAnchorUs < 0 && !isPictureNal) {
            if (s.hasFrameRtpTs) {
                s.videoRtp.timestamp = s.lastFrameRtpTs + 1;
            }
            s.lastFrameRtpTs = s.videoRtp.timestamp;
            s.hasFrameRtpTs = true;

            // Update RTCP SR clock reference but do not consume
            // the session anchor.
            struct timespec mono;
            clock_gettime(CLOCK_MONOTONIC, &mono);
            s.lastVideoTsUs = static_cast<int64_t>(mono.tv_sec) * 1000000LL +
                              static_cast<int64_t>(mono.tv_nsec) / 1000LL;
        } else {
            if (s.videoStartAnchorUs < 0) {
                s.videoStartAnchorUs = ts_us;
                // Compute imp_ts -> CLOCK_MONOTONIC offset once.
                // imp_ts may start from 0 (encoder init) while mono
                // starts from boot; the offset corrects the base so
                // ntpAt() receives an approximate monotonic value.
                struct timespec mono;
                clock_gettime(CLOCK_MONOTONIC, &mono);
                s.videoTsToMonoOffset =
                    static_cast<int64_t>(mono.tv_sec) * 1000000LL +
                    static_cast<int64_t>(mono.tv_nsec) / 1000LL
                    - ts_us;
            }
            int64_t rel_us = ts_us - s.videoStartAnchorUs;
            if (rel_us < 0) rel_us = 0;
            // 90 kHz RTP clock: multiply by 9, divide by 100 (90000/1000000)
            uint32_t new_ts = static_cast<uint32_t>((static_cast<uint64_t>(rel_us) * 9ULL) / 100ULL);
            // Guard against forward timestamp jumps (e.g. IMP encoder
            // timestamp domain transition from 0→real-time, which VideoWorker
            // cannot prevent when ts_last_frame_us is still 0).  Cap the step
            // to ~500 ms of video; larger jumps are clamped to a smooth
            // increment from the last RTP timestamp.
            if (s.hasFrameRtpTs) {
                static const uint32_t kMaxVideoRtpStep = 45000; // 500 ms at 90 kHz
                int64_t diff = static_cast<int64_t>(new_ts) - static_cast<int64_t>(s.lastFrameRtpTs);
                if (diff > kMaxVideoRtpStep) {
                    new_ts = s.lastFrameRtpTs + kMaxVideoRtpStep;
                } else if (diff <= 0) {
                    new_ts = s.lastFrameRtpTs + 1;
                }
            }
            s.lastFrameRtpTs = new_ts;
            s.hasFrameRtpTs = true;
            s.videoRtp.timestamp = new_ts;
            s.videoFrameCount++;

            // Capture CLOCK_MONOTONIC for RTCP SR NTP mapping.
            // Use real monotonic clock (not imp_ts + offset) because
            // imp_ts and CLOCK_MONOTONIC are different hardware clocks
            // on T31 and will drift apart over time, causing A-V desync.
            struct timespec mono;
            clock_gettime(CLOCK_MONOTONIC, &mono);
            s.lastVideoTsUs = static_cast<int64_t>(mono.tv_sec) * 1000000LL +
                              static_cast<int64_t>(mono.tv_nsec) / 1000LL;
        }
    }

    int clientIdx = s.sessionsIndex;
    uint8_t chan  = s.videoInterleavedRtp;
    auto output = [&, this, clientIdx, chan](const uint8_t *pkt, size_t len) -> bool {
        auto *sen = sessions_[clientIdx].get();
        if (!sen || sen->fd < 0) return false;

        if (!sen->tcpInterleaved) {
            // UDP: send to client via UDP socket
            if (sen->videoRtpSock < 0) return false;
            sockaddr_in target = sen->clientAddr;
            target.sin_port = htons(sen->videoClientRtpPort);
            ssize_t n = sendto(sen->videoRtpSock, pkt, len, MSG_DONTWAIT,
                               (sockaddr *)&target, sizeof(target));
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // Send buffer full: the IDR burst is outpacing the client's
                // drain rate. UDP has no flow control, so blindly firing the
                // rest of the burst overflows the client's receive buffer and
                // drops packets. Poll with a short timeout to pace to the
                // client's consumption rate.
                struct pollfd pfd;
                pfd.fd = sen->videoRtpSock;
                pfd.events = POLLOUT;
                if (poll(&pfd, 1, 250) > 0) {
                    n = sendto(sen->videoRtpSock, pkt, len, MSG_DONTWAIT,
                               (sockaddr *)&target, sizeof(target));
                }
            }
            // Always return true for UDP: a dropped datagram advances the
            // RTP sequence number (creating a gap the receiver can detect
            // and handle) rather than aborting the entire FU-A chain and
            // corrupting the rest of the frame.
            if (static_cast<size_t>(n) != len) {
                static int udp_drop_count = 0;
                if (udp_drop_count++ < 3)
                    LOG_WARN("UDP RTP send failed: dropped packet (ch="
                             << static_cast<int>(chan) << ")");
            }
            return true;
        }

        // TCP interleaved: non-blocking send with queue.
        // Packets that can't be sent immediately are queued for retry.
        // Always returns true — the drain loop retries queued packets.
        // Only returns false if client is disconnected.
        //
        // Ordering invariant: once any bytes are queued for a session, every
        // subsequent packet must be appended to the queue rather than sent
        // directly.  Otherwise a partially-written packet's queued tail would
        // be re-ordered AFTER the next packet(s) sent in the same drain loop
        // (the queue is only flushed at the start of the next iteration),
        // corrupting the interleaved bitstream.  The drain loop empties the
        // queue first each iteration, so direct sends resume once it drains.
        if (!sen->sendQueue.empty()) {
            uint8_t buf[1504];
            size_t total = len + 4;
            if (total > sizeof(buf)) {
                closeClient(sen->sessionsIndex);
                return false;
            }
            buf[0] = '$';
            buf[1] = chan;
            buf[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
            buf[3] = static_cast<uint8_t>(len & 0xFF);
            memcpy(buf + 4, pkt, len);
            if (sen->sendQueueBytes + total > 1024 * 1024) { // 1MB cap
                closeClient(sen->sessionsIndex);
                return false;
            }
            std::vector<uint8_t> pktBuf(buf, buf + total);
            sen->sendQueue.push_back(std::move(pktBuf));
            sen->sendQueueBytes += total;
            return true;
        }

        uint8_t buf[1504];
        size_t total = len + 4;
        buf[0] = '$';
        buf[1] = chan;
        buf[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        buf[3] = static_cast<uint8_t>(len & 0xFF);
        memcpy(buf + 4, pkt, len);

        ssize_t n = send(sen->fd, buf, total, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (static_cast<size_t>(n) == total) return true;

        // Partial or EAGAIN: enqueue the remainder.  From now on this
        // session's output appends to the queue (see guard above) until the
        // drain loop flushes it, preserving interleaved ordering.
        size_t sent = (n > 0) ? static_cast<size_t>(n) : 0;
        size_t remain = total - sent;
        if (sen->sendQueueBytes + remain > 1024 * 1024) { // 1MB cap
            // Queue full — client too slow, disconnect it
            closeClient(sen->sessionsIndex);
            return false;
        }
        std::vector<uint8_t> pktBuf(remain);
        memcpy(pktBuf.data(), buf + sent, remain);
        sen->sendQueue.push_back(std::move(pktBuf));
        sen->sendQueueBytes += remain;

        return true;
    };

    // ── Prepend SPS/PPS before first non-config NAL ───────────────────
    if (s.videoChn >= 0 &&
        s.videoChn < NUM_VIDEO_CHANNELS && global_video[s.videoChn]) {
        auto &vs = global_video[s.videoChn];
        std::lock_guard<std::mutex> lock(vs->codec_config_mutex);

        // Determine NAL type for this frame
        bool isSps = false, isPps = false, isVps = false;
        if (nalLen > 0) {
            if (isH265 && nalLen >= 2) {
                uint8_t t = (nalData[0] >> 1) & 0x3F;
                isVps = (t == 32); isSps = (t == 33); isPps = (t == 34);
            } else {
                uint8_t t = nalData[0] & 0x1F;
                isSps = (t == 7); isPps = (t == 8);
            }
        }

        // Detect SPS/PPS changes (e.g. after day/night reconfig) and
        // re-send config so the client decoder stays in sync.
        auto simpleHash = [](const uint8_t *d, size_t n) -> uint32_t {
            // FNV-1a 32-bit
            uint32_t h = 0x811C9DC5u;
            for (size_t i = 0; i < n; i++) {
                h ^= d[i];
                h *= 0x01000193u;
            }
            return h;
        };
        uint32_t curSpsHash = vs->have_sps ? simpleHash(vs->latest_sps.data(), vs->latest_sps.size()) : 0;
        uint32_t curPpsHash = vs->have_pps ? simpleHash(vs->latest_pps.data(), vs->latest_pps.size()) : 0;

        // Inline SPS/PPS: update our tracking so we know config changed.
        // Only flag as a reconfigure when we've already seen a full codec
        // config for this session.  On the initial connection spsHash and
        // ppsHash are both zero — that's the session learning the codec,
        // not a mid-stream reconfiguration that would invalidate the SDP.
        //
        // NOTE: we intentionally do NOT set codecConfigSent here.
        // The inline SPS/PPS are sent as standalone RTP NALs (below),
        // but the definitive delivery happens when they are prepended
        // before the first IDR frame.  Letting codecConfigSent remain
        // false until the prepend block runs ensures the client always
        // gets SPS/PPS both standalone and prepended on initial connect.
        if (isSps || isPps || isVps) {
            bool hadConfig = (s.spsHash != 0 || s.ppsHash != 0);
            if (hadConfig && (curSpsHash != s.spsHash || curPpsHash != s.ppsHash)) {
                s.spsChanged = true;
            }
            s.spsHash = curSpsHash;
            s.ppsHash = curPpsHash;
            // codecConfigSent intentionally not set here — see comment above
        }

        bool configChanged = (!s.codecConfigSent) || s.spsChanged;

        LOG_DDEBUG("ch" << s.videoChn << " codecConfig: have_sps=" << vs->have_sps
                 << " have_pps=" << vs->have_pps
                 << " sps_size=" << vs->latest_sps.size()
                 << " pps_size=" << vs->latest_pps.size()
                 << " configSent=" << s.codecConfigSent
                 << " configChanged=" << configChanged);

        if (!isSps && !isPps && !isVps && vs->have_sps && vs->have_pps && configChanged) {
            LOG_INFO("ch" << s.videoChn << " prepending SPS/PPS to frame"
                     << (s.spsChanged ? " (re-config detected)" : ""));
            if (isH265) {
                if (vs->have_vps && !vs->latest_vps.empty()) {
                    if (!packetizeH265(vs->latest_vps.data(), vs->latest_vps.size(),
                                       false, false, pt, s.videoRtp, output))
                        return false;
                }
                if (!packetizeH265(vs->latest_sps.data(), vs->latest_sps.size(),
                                    false, false, pt, s.videoRtp, output))
                    return false;
                if (!packetizeH265(vs->latest_pps.data(), vs->latest_pps.size(),
                                    false, false, pt, s.videoRtp, output))
                    return false;
            } else {
                if (!packetizeH264(vs->latest_sps.data(), vs->latest_sps.size(),
                                    false, false, pt, s.videoRtp, output))
                    return false;
                if (!packetizeH264(vs->latest_pps.data(), vs->latest_pps.size(),
                                    false, false, pt, s.videoRtp, output))
                    return false;
            }
            s.codecConfigSent = true;
            s.waitingForKeyframe = true;
            s.spsChanged = false;
        } else if (isSps || isPps || isVps) {
            // inline path already updated hashes above
            (void)0;
        }
    }

    // ── Keyframe gate: after codec config is sent on a new session,
    // drop non-keyframe NALs until the first IDR arrives.  This prevents
    // stale non-IDR frames (accumulated in the tap buffer before the
    // encoder flush) from reaching the client without reference pictures.
    if (s.waitingForKeyframe && !nal.is_keyframe)
        return true;
    if (nal.is_keyframe)
        s.waitingForKeyframe = false;

    // ── Packetize ──────────────────────────────────────────────────────
    bool ok;
    if (isH265) {
        ok = packetizeH265(nalData, nalLen,
                           nal.is_frame_start, nal.is_frame_end,
                           pt, s.videoRtp, output);
    } else {
        ok = packetizeH264(nalData, nalLen,
                           nal.is_frame_start, nal.is_frame_end,
                           pt, s.videoRtp, output);
    }

    return ok;
}

// ── RTP sending (audio) ────────────────────────────────────────────────────

bool RtspServer::sendAudioFrame(Session &s, const AudioFrame &af) {
    if (s.fd < 0 || af.data.empty()) return true;

    // Drop pre-session audio frames (captured before first video frame).
    // These accumulate in the tap during encoder startup and would flood the
    // stream with PTS=0 frames followed by a sudden jump to real PTS.
    if (s.startAnchor.tv_sec != 0 &&
        (af.time.tv_sec < s.startAnchor.tv_sec ||
         (af.time.tv_sec == s.startAnchor.tv_sec &&
          af.time.tv_usec < s.startAnchor.tv_usec))) {
        return true;
    }

    // Skip frames with zero timestamp: the IMP audio driver may produce
    // frames with timeStamp=0 for the first few captures after enable.
    // Using such a frame as the timestamp anchor causes a catastrophic
    // RTP timestamp jump (0 → millions of units) when the driver later
    // delivers frames with real timestamps, which triggers DTS
    // discontinuity errors and buffering resets in clients like mpv.
    if (af.time.tv_sec == 0 && af.time.tv_usec == 0) {
        return true;
    }

    // Read codec from audio config
    std::string codec = "AAC";
    int sampleRate = 16000;
    int channels = 1;
    uint8_t pt = 97;
    if (!audioStreams_.empty()) {
        codec = audioStreams_[0].config.codec;
        sampleRate = audioStreams_[0].config.sampleRate;
        channels = audioStreams_[0].config.channels;
        pt = static_cast<uint8_t>(audioStreams_[0].config.payloadType);
        // OPUS RTP clock must be 48000 regardless of hardware rate
        if (codec == "OPUS") sampleRate = 48000;
    }

    static int logOnce = 0;
    if (logOnce == 0) {
        LOG_INFO("Audio codec=" << codec << " rate=" << sampleRate
                 << " pt=" << static_cast<int>(pt)
                 << " len=" << af.data.size());
        logOnce = 1;
    }

    // RTP timestamp: relative to session-start anchor
    {
        int64_t dtUs = (static_cast<int64_t>(af.time.tv_sec) -
                        static_cast<int64_t>(s.startAnchor.tv_sec)) * 1000000LL
                     + (static_cast<int64_t>(af.time.tv_usec) -
                        static_cast<int64_t>(s.startAnchor.tv_usec));
        // Anchor on first audio frame's capture time if not already set.
        // Subtract a 1 ms guard band so any frames that were captured
        // slightly before the anchor frame still produce positive offsets
        // instead of backward DTS jumps.
        if (s.audioRtp.timestamp == 0 && s.startAnchor.tv_sec == 0 && s.startAnchor.tv_usec == 0) {
            s.startAnchor = af.time;
            if (s.startAnchor.tv_usec >= 1000) {
                s.startAnchor.tv_usec -= 1000;
            } else if (s.startAnchor.tv_sec > 0) {
                s.startAnchor.tv_sec -= 1;
                s.startAnchor.tv_usec += 1000000 - 1000;
            }
            dtUs = 1000;
        }
        if (dtUs < 0) dtUs = 0;
        uint32_t new_ts = static_cast<uint32_t>(
            dtUs * static_cast<int64_t>(sampleRate) / 1000000LL);
        // Guard against forward timestamp jumps (e.g. IMP driver timestamp
        // domain transition from 0→real-time).  Cap the step to 500 ms of
        // audio; larger jumps are treated as a discontinuity and the RTP
        // timestamp is clamped to a smooth increment from the last value.
        if (s.hasAudioRtpTs) {
            int32_t max_step = (sampleRate > 0) ? (sampleRate / 2) : 8000;
            int64_t diff = static_cast<int64_t>(new_ts) - static_cast<int64_t>(s.audioRtp.timestamp);
            if (diff > max_step) {
                new_ts = s.audioRtp.timestamp + static_cast<uint32_t>(max_step);
            } else if (diff <= 0) {
                new_ts = s.audioRtp.timestamp + 1;
            }
        }
        s.audioRtp.timestamp = new_ts;
        s.hasAudioRtpTs = true;

        // Use CLOCK_MONOTONIC for RTCP SR consistency — audio capture
        // timestamps (af.time) may not be rebased by the IMP driver to
        // the same clock domain as video imp_ts, so derive the RTCP SR
        // reference from the monotonic clock directly.
        struct timespec mono;
        clock_gettime(CLOCK_MONOTONIC, &mono);
        s.lastAudioTsUs = static_cast<int64_t>(mono.tv_sec) * 1000000LL +
                          static_cast<int64_t>(mono.tv_nsec) / 1000LL;
    }

    int clientIdx = s.sessionsIndex;
    uint8_t chan  = s.audioInterleavedRtp;
    auto output = [&, this, clientIdx, chan](const uint8_t *pkt, size_t len) -> bool {
        auto *sen = sessions_[clientIdx].get();
        if (!sen || sen->fd < 0) return false;
        if (!sen->tcpInterleaved) {
            // UDP: send to client via UDP socket
            if (sen->audioRtpSock < 0) return false;
            sockaddr_in target = sen->clientAddr;
            target.sin_port = htons(sen->audioClientRtpPort);
            ssize_t n = sendto(sen->audioRtpSock, pkt, len, MSG_DONTWAIT,
                               (sockaddr *)&target, sizeof(target));
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd pfd;
                pfd.fd = sen->audioRtpSock;
                pfd.events = POLLOUT;
                if (poll(&pfd, 1, 250) > 0) {
                    n = sendto(sen->audioRtpSock, pkt, len, MSG_DONTWAIT,
                               (sockaddr *)&target, sizeof(target));
                }
            }
            // Always return true for UDP: a dropped datagram creates a
            // recoverable sequence gap rather than aborting the stream.
            return true;
        }
        // TCP interleaved: non-blocking send with queue.
        // Same ordering invariant as the video path: once any bytes are
        // queued, append subsequent packets to the queue so a queued tail
        // is never re-ordered after later packets on the shared fd.
        if (!sen->sendQueue.empty()) {
            uint8_t buf[1504];
            size_t total = len + 4;
            if (total > sizeof(buf)) return false;
            buf[0] = '$';
            buf[1] = chan;
            buf[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
            buf[3] = static_cast<uint8_t>(len & 0xFF);
            memcpy(buf + 4, pkt, len);
            if (sen->sendQueueBytes + total > 1024 * 1024) {
                closeClient(sen->sessionsIndex);
                return false;
            }
            std::vector<uint8_t> pktBuf(buf, buf + total);
            sen->sendQueue.push_back(std::move(pktBuf));
            sen->sendQueueBytes += total;
            return true;
        }
        uint8_t buf[1504];
        size_t total = len + 4;
        buf[0] = '$';
        buf[1] = chan;
        buf[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        buf[3] = static_cast<uint8_t>(len & 0xFF);
        memcpy(buf + 4, pkt, len);
        ssize_t s = send(sen->fd, buf, total, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (s == static_cast<ssize_t>(total)) return true;
        // Partial or EAGAIN: enqueue for retry
        size_t sent = (s > 0) ? static_cast<size_t>(s) : 0;
        size_t remain = total - sent;
        if (sen->sendQueueBytes + remain > 1024 * 1024) {
            closeClient(sen->sessionsIndex);
            return false;
        }
        std::vector<uint8_t> pktBuf(remain);
        memcpy(pktBuf.data(), buf + sent, remain);
        sen->sendQueue.push_back(std::move(pktBuf));
        sen->sendQueueBytes += remain;
        return true;
    };

    if (codec == "AAC") {
        return packetizeAAC(af.data.data(), af.data.size(), 0, sampleRate,
                            pt, s.audioRtp, output);
    } else if (codec == "L16") {
        // MIPS is little-endian; RTP L16 payload requires big-endian
        std::vector<uint8_t> be(af.data.size());
        const uint16_t *src = reinterpret_cast<const uint16_t *>(af.data.data());
        uint16_t *dst = reinterpret_cast<uint16_t *>(be.data());
        size_t n = af.data.size() / 2;
        for (size_t i = 0; i < n; i++)
            dst[i] = (src[i] >> 8) | (src[i] << 8);
        int sampleBytes = channels * 2; // 16-bit per sample per channel
        return packetizeL16(be.data(), be.size(), sampleBytes,
                            pt, s.audioRtp, output);
    } else {
        // PCMU, PCMA, OPUS — raw payload
        return sendOne(af.data.data(), af.data.size(), pt,
                       /*marker*/ true, s.audioRtp, output);
    }
}

// ── TCP interleaved framing ────────────────────────────────────────────────

void RtspServer::sendInterleaved(int fd, uint8_t channel,
                                 const uint8_t *data, size_t len) {
    // Format: $ + channel(1) + length(2, big-endian) + data
    uint8_t hdr[4];
    hdr[0] = '$';
    hdr[1] = channel;
            hdr[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
            hdr[3] = static_cast<uint8_t>(len & 0xFF);

    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len  = 4;
    iov[1].iov_base = const_cast<uint8_t *>(data);
    iov[1].iov_len  = len;

    struct msghdr msg{};
    msg.msg_iov    = iov;
    msg.msg_iovlen = 2;
    sendmsg(fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
}

// ── Subtitle text RTP sender ───────────────────────────────────────────────

static void sendSubtitleRtp(Session &s,
                            const SubtitleStreamConfig &cfg,
                            const std::string &payload,
                            bool marker) {
    if (s.fd < 0) return;
    if (payload.empty() && !marker) return;

    uint8_t pt = static_cast<uint8_t>(cfg.payloadType);
    size_t payloadLen = payload.size();
    if (payloadLen > 1200) payloadLen = 1200;

    uint32_t rtptime = htonl(s.subtitleRtp.timestamp);
    // timestamp advanced by caller

    std::vector<uint8_t> rtp(12 + payloadLen);
    rtp[0] = 0x80;
    rtp[1] = (marker ? 0x80 : 0x00) | pt;
    rtp[2] = static_cast<uint8_t>((s.subtitleRtp.seq >> 8) & 0xFF);
    rtp[3] = static_cast<uint8_t>(s.subtitleRtp.seq & 0xFF);
    s.subtitleRtp.seq++;
    memcpy(&rtp[4], &rtptime, 4);
    uint32_t ssrc = htonl(s.subtitleRtp.ssrc);
    memcpy(&rtp[8], &ssrc, 4);
    if (payloadLen > 0)
        memcpy(&rtp[12], payload.data(), payloadLen);

    if (s.subtitleTcp) {
        uint8_t buf[1504];
        size_t total = rtp.size() + 4;
        buf[0] = '$';
        buf[1] = s.subtitleInterleavedRtp;
        buf[2] = static_cast<uint8_t>((rtp.size() >> 8) & 0xFF);
        buf[3] = static_cast<uint8_t>(rtp.size() & 0xFF);
        memcpy(buf + 4, rtp.data(), rtp.size());
        ssize_t sent = send(s.fd, buf, total, MSG_DONTWAIT | MSG_NOSIGNAL);
        static int sub_send_err = 0;
        if (sent < 0 && sub_send_err++ < 3)
            LOG_WARN("sendSubtitleRtp: send failed fd=" << s.fd << " err=" << errno);
    } else {
        if (s.subtitleRtpSock >= 0) {
            sockaddr_in target = s.clientAddr;
            target.sin_port = htons(s.subtitleClientRtpPort);
            ssize_t sent = sendto(s.subtitleRtpSock, rtp.data(), rtp.size(),
                                  MSG_DONTWAIT, (sockaddr *)&target,
                                  sizeof(target));
            static int sub_sendto_err = 0;
            if (sent < 0 && sub_sendto_err++ < 3)
                LOG_WARN("sendSubtitleRtp: sendto failed err=" << errno);
        }
    }
}

bool RtspServer::sendSubtitleText(Session &s, const std::string &text) {
    if (s.fd < 0 || text.empty()) return true;
    if (subtitleStreams_.empty()) return true;

    const auto &cfg = subtitleStreams_[0].config;

    LOG_DEBUG("subtitle ch=" << s.videoChn << " tcp=" << s.subtitleTcp
              << " ich=" << (int)s.subtitleInterleavedRtp
              << " len=" << text.size());

    // Format as single line: strip element labels, join values.
    // Collect values, then join with timestamp first if present.
    std::string ts, host, other;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find("\r\n", start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        size_t colon = line.find(':');
        if (colon != std::string::npos && colon + 1 < line.size()) {
            std::string val = line.substr(colon + 1);
            if (line.compare(0, colon, "clock") == 0)
                ts = val;
            else if (line.compare(0, colon, "host") == 0)
                host = val;
            else if (!val.empty())
                other += (other.empty() ? "" : " ") + val;
        }
        start = end + 2;
        if (start >= text.size()) break;
    }
    std::string doc = ts;
    if (!host.empty()) doc += (doc.empty() ? "" : " ") + host;
    if (!other.empty()) doc += (doc.empty() ? "" : " ") + other;

    sendSubtitleRtp(s, cfg, doc, true);         // M=1: text event
    s.subtitleRtp.timestamp += 90000;            // advance after send
    return true;
}

// ── RTCP Sender Report ──────────────────────────────────────────────────────

void RtspServer::sendRtcpSr(Session &s) {
    if (s.fd < 0) return;
    // Backchannel-only sessions have no RTP streams to report on.
    if (s.backchannel && s.videoChn < 0 && !s.hasAudio) return;

    // Establish the session NTP anchor once: wall-clock NTP paired with
    // CLOCK_MONOTONIC.  SR NTP values then advance on the monotonic
    // timeline so NTP daemon clock steps can never shift the mapping.
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    int64_t monoNowUs = static_cast<int64_t>(mono.tv_sec) * 1000000LL +
                        static_cast<int64_t>(mono.tv_nsec) / 1000LL;
    if (s.ntpAnchorMonoUs < 0) {
        s.ntpAnchor = simple_rtsp::ntpTimestamp();
        s.ntpAnchorMonoUs = monoNowUs;
    }
    // NTP 32.32 value at a given monotonic-domain microsecond instant.
    auto ntpAt = [&s](int64_t tsUs) -> uint64_t {
        int64_t d = tsUs - s.ntpAnchorMonoUs; // may be slightly negative
        int64_t whole = d / 1000000LL;
        int64_t rem = d - whole * 1000000LL;  // same sign as d
        int64_t off = whole * 4294967296LL + (rem * 4294967296LL) / 1000000LL;
        return s.ntpAnchor + static_cast<uint64_t>(off);
    };

    // Pair each stream's SR with the NTP time OF ITS LAST RTP TIMESTAMP
    // (not "now"): the RTP ts field below is the last packet's ts, and a
    // stale ts paired with a fresh NTP skews the mapping by up to one
    // frame interval per SR, wobbling receiver PTS backwards.
    uint64_t ntp = ntpAt(s.lastVideoTsUs >= 0 ? s.lastVideoTsUs : monoNowUs);
    uint32_t ntpMsw = htonl(static_cast<uint32_t>(ntp >> 32));
    uint32_t ntpLsw = htonl(static_cast<uint32_t>(ntp & 0xFFFFFFFF));
    uint64_t antp = ntpAt(s.lastAudioTsUs >= 0 ? s.lastAudioTsUs : monoNowUs);
    uint32_t antpMsw = htonl(static_cast<uint32_t>(antp >> 32));
    uint32_t antpLsw = htonl(static_cast<uint32_t>(antp & 0xFFFFFFFF));
    uint8_t rtcp[28] = {};
    rtcp[0] = 0x80; rtcp[1] = 200;
    rtcp[2] = 0; rtcp[3] = 6;

    uint32_t vsrc = htonl(s.videoRtp.ssrc);
    memcpy(rtcp + 4, &vsrc, 4);
    memcpy(rtcp + 8, &ntpMsw, 4);
    memcpy(rtcp + 12, &ntpLsw, 4);
    uint32_t vts = htonl(s.videoRtp.timestamp);
    memcpy(rtcp + 16, &vts, 4);
    rtcp[23] = 1; rtcp[27] = 1;

    if (!s.tcpInterleaved) {
        // UDP: send via UDP sockets
        sockaddr_in target = s.clientAddr;
        target.sin_port = htons(s.videoClientRtcpPort);
        sendto(s.videoRtcpSock, rtcp, sizeof(rtcp), MSG_DONTWAIT,
               (sockaddr *)&target, sizeof(target));
        if (s.hasAudio) {
            uint32_t asrc = htonl(s.audioRtp.ssrc);
            memcpy(rtcp + 4, &asrc, 4);
            memcpy(rtcp + 8, &antpMsw, 4);
            memcpy(rtcp + 12, &antpLsw, 4);
            uint32_t ats = htonl(s.audioRtp.timestamp);
            memcpy(rtcp + 16, &ats, 4);
            target.sin_port = htons(s.audioClientRtcpPort);
            sendto(s.audioRtcpSock, rtcp, sizeof(rtcp), MSG_DONTWAIT,
                   (sockaddr *)&target, sizeof(target));
        }
    } else {
        // TCP interleaved
        uint8_t vhdr[4] = { '$', s.videoInterleavedRtcp, 0, 28 };
        struct iovec viov[2];
        viov[0].iov_base = vhdr; viov[0].iov_len = 4;
        viov[1].iov_base = rtcp; viov[1].iov_len = 28;
        struct msghdr vmsg{};
        vmsg.msg_iov = viov; vmsg.msg_iovlen = 2;
        sendmsg(s.fd, &vmsg, MSG_DONTWAIT | MSG_NOSIGNAL);

        if (s.hasAudio) {
            uint32_t asrc = htonl(s.audioRtp.ssrc);
            memcpy(rtcp + 4, &asrc, 4);
            memcpy(rtcp + 8, &antpMsw, 4);
            memcpy(rtcp + 12, &antpLsw, 4);
            uint32_t ats = htonl(s.audioRtp.timestamp);
            memcpy(rtcp + 16, &ats, 4);
            uint8_t ahdr[4] = { '$', s.audioInterleavedRtcp, 0, 28 };
            struct iovec aiov[2];
            aiov[0].iov_base = ahdr; aiov[0].iov_len = 4;
            aiov[1].iov_base = rtcp; aiov[1].iov_len = 28;
            struct msghdr amsg{};
            amsg.msg_iov = aiov; amsg.msg_iovlen = 2;
            sendmsg(s.fd, &amsg, MSG_DONTWAIT | MSG_NOSIGNAL);
        }
    }
}

// ── Session timeouts ───────────────────────────────────────────────────────

void RtspServer::checkSessionTimeouts() {
    time_t now = time(nullptr);
    constexpr time_t kTimeout = 65; // slightly more than typical RTCP interval

    for (size_t i = 0; i < sessions_.size(); i++) {
        auto &s = sessions_[i];
        if (!s || s->fd < 0) continue;
        if (now - s->lastActivity > kTimeout) {
            LOG_DEBUG("Session " << s->sessionId << " timed out");
            closeClient(static_cast<int>(i));
        }
    }
}

} // namespace simple_rtsp
