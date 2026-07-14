// musl/uclibc may need this for MSG_NOSIGNAL, strncasecmp
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "RtspServer.hpp"
#include "RtpPacketizer.hpp"
#include "SdpGenerator.hpp"
#include "Logger.hpp"
#include "globals.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
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
    int  videoChn    = -1;   // encoder channel index, -1 = unset
    bool hasAudio    = false;

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
    sockaddr_in clientAddr  = {};
    socklen_t clientAddrLen = 0;

    // Video tap
    std::shared_ptr<MsgChannel<H264NALUnit>> videoTap;
    uint64_t videoTapId = 0;

    // Audio tap
    std::shared_ptr<MsgChannel<AudioFrame>> audioTap;
    uint64_t audioTapId = 0;

    // RTP state
    RtpState videoRtp;
    RtpState audioRtp;

    bool    codecConfigSent    = false; // SPS/PPS prepended for this session
    bool    waitingForKeyframe = false; // drop non-IDR until first keyframe arrives
    bool    sendInitialRtcpSr  = false; // Send RTCP SR after first frame

    // Audio uses wall-clock start (separate because audio NALs lack imp_ts)
    struct timeval startAnchor{0, 0};

    // Video uses encoder monotonic clock (imp_ts, microseconds).
    // Anchored to the first frame's imp_ts for the session so RTP
    // timestamps track the actual frame cadence.
    int64_t videoStartAnchorUs = -1;

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
            if (!s || !s->playing || s->videoChn < 0) continue;

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
                    LOG_DEBUG("video drain " << drained << " NALs, ch="
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
                    LOG_DEBUG("audio drain " << drained << " frames");
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
    s->videoChn = -1;
    s->hasAudio = false;
    s->sessionId[0] = '\0';
    s->lastActivity = time(nullptr);
    s->videoRtp = RtpState{};
    s->audioRtp = RtpState{};
    s->videoRtp.ssrc = static_cast<uint32_t>(rand());
    s->audioRtp.ssrc = static_cast<uint32_t>(rand());
    s->videoRtp.seq = static_cast<uint16_t>(rand());
    s->audioRtp.seq = static_cast<uint16_t>(rand());
    s->videoRtp.timestamp = 0;
    s->audioRtp.timestamp = 0;
    s->pendingRespLen = 0;
    s->pendingRespOff = 0;

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

    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    // Close UDP sockets if allocated
    if (s->videoRtpSock   >= 0) { close(s->videoRtpSock);   s->videoRtpSock   = -1; }
    if (s->videoRtcpSock  >= 0) { close(s->videoRtcpSock);  s->videoRtcpSock  = -1; }
    if (s->audioRtpSock   >= 0) { close(s->audioRtpSock);   s->audioRtpSock   = -1; }
    if (s->audioRtcpSock  >= 0) { close(s->audioRtcpSock);  s->audioRtcpSock  = -1; }
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
        LOG_WARN("Request too large, resetting buffer");
        s->readOff = 0;
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
        s->readOff -= static_cast<int>(total);
        memmove(s->readBuf, s->readBuf + total, static_cast<size_t>(s->readOff));
        s->readBuf[s->readOff] = '\0';
    }
    if (s->readOff == 0) return; // nothing left to parse

    // Check for complete request (ends with \r\n\r\n)
    char *end = strstr(s->readBuf, "\r\n\r\n");
    if (!end) {
        end = strstr(s->readBuf, "\n\n");
        if (!end) return; // incomplete — wait for more data
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

    LOG_INFO("RTSP " << methodToString(method) << " " << uri
             << " CSeq=" << cseq);

    // ── Dispatch ────────────────────────────────────────────────────────
    switch (method) {
    case Method::OPTIONS:       handleOptions(idx, cseq);        break;
    case Method::DESCRIBE:      handleDescribe(idx, cseq, uri);  break;
    case Method::SETUP:         handleSetup(idx, cseq, uri, headersStart); break;
    case Method::PLAY:          handlePlay(idx, cseq, uri, headersStart);  break;
    case Method::TEARDOWN:      handleTeardown(idx, cseq, headersStart);   break;
    case Method::GET_PARAMETER:
    case Method::SET_PARAMETER:
    case Method::PAUSE:
    case Method::ANNOUNCE:
    case Method::RECORD:
        LOG_DEBUG("RTSP " << methodToString(method));
        sendResponse(*s, Status::OK, cseq, nullptr, nullptr);
        break;
    default:
        LOG_WARN("Unsupported method (clearing buffer): " << methodStr);
        s->readOff = 0;
        return;
    }

    // Consume the processed request from the buffer
    size_t consumed = static_cast<size_t>(end - s->readBuf) + 4; // past \r\n\r\n
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

void RtspServer::handleDescribe(int idx, int cseq, const char *uri) {
    auto &s = sessions_[idx];

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

    std::string sdp = generateSdp(ve.config, audioCfg, serverIp);

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
    bool isAudio = (strstr(uri, "track2") != nullptr);
    bool isVideo = (strstr(uri, "track1") != nullptr);
    // Fallback: if no trackID, assume first is video (for legacy clients)
    if (!isAudio && !isVideo) {
        isVideo = (s->videoChn < 0); // first SETUP without trackID = video
        isAudio = !isVideo && !audioStreams_.empty();
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

    if (s->videoChn < 0 && !s->hasAudio) {
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
             << " players=" << activePlayers_[s->videoChn]);

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

    // Defer RTCP SR until the first video frame arrives so the timestamp
    // and sequence number match actual RTP packets, preventing FFmpeg's
    // "dropping old packet received too late" jitter buffer issue.
    s->sendInitialRtcpSr = true;
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

    // ── Timestamp ── encoder monotonic clock (90 kHz RTP) ────────────
    // Use imp_ts (microseconds from encoder, smoothed by VideoWorker)
    // anchored to the first frame of the session.  This produces RTP
    // timestamps that match the actual frame cadence, so a client's
    // jitter buffer never starves when the encoder falls below the
    // configured framerate (e.g. 12 fps actual vs 30 fps configured).
    // The old frame-counter approach always ticked at the *configured*
    // fps, causing mpv to consume buffered frames at 30 fps and then
    // enter buffering.
    if (nal.is_frame_start) {
        int64_t ts_us = nal.imp_ts;
        if (s.videoStartAnchorUs < 0) {
            s.videoStartAnchorUs = ts_us;
        }
        int64_t rel_us = ts_us - s.videoStartAnchorUs;
        if (rel_us < 0) rel_us = 0;
        // 90 kHz RTP clock: multiply by 9, divide by 100 (90000/1000000)
        uint32_t new_ts = static_cast<uint32_t>((static_cast<uint64_t>(rel_us) * 9ULL) / 100ULL);
        // RTP timestamps must be strictly increasing (RFC 3550).
        // Compare against the previous frame-start timestamp, not
        // videoRtp.timestamp (which may have been read by other paths).
        if (s.hasFrameRtpTs && new_ts <= s.lastFrameRtpTs) {
            new_ts = s.lastFrameRtpTs + 1;
        }
        s.lastFrameRtpTs = new_ts;
        s.hasFrameRtpTs = true;
        s.videoRtp.timestamp = new_ts;
        s.videoFrameCount++;
    }

    // ── Determine codec from the registered stream ───────────────────
    bool isH265 = false;
    uint8_t pt = 96;
    for (auto &ve : videoStreams_) {
        if (ve.chn == s.videoChn) {
            isH265 = (ve.config.codec == "H265");
            break;
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
                // drops packets -> "RTP: missed N packets" -> decoder desync.
                // The socket is non-blocking, so a flags=0 "retry" would also
                // fail immediately. Instead poll() until it is writable
                // (bounded) and retry; this paces us to the client's
                // consumption rate, exactly like TCP flow control would, and
                // never drops a packet we could have sent.
                struct pollfd pfd;
                pfd.fd = sen->videoRtpSock;
                pfd.events = POLLOUT;
                if (poll(&pfd, 1, 250) > 0) {
                    n = sendto(sen->videoRtpSock, pkt, len, MSG_DONTWAIT,
                               (sockaddr *)&target, sizeof(target));
                }
            }
            return static_cast<size_t>(n) == len;
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

    // Send deferred initial RTCP SR after the first video frame so the
    // timestamp and sequence number match actual RTP packets.
    if (ok && s.sendInitialRtcpSr && nal.is_frame_start) {
        sendRtcpSr(s);
        s.sendInitialRtcpSr = false;
    }

    return ok;
}

// ── RTP sending (audio) ────────────────────────────────────────────────────

bool RtspServer::sendAudioFrame(Session &s, const AudioFrame &af) {
    if (s.fd < 0 || af.data.empty()) return true;
    if (af.data.size() > 1400) {
        LOG_ERROR("Audio frame too large: " << af.data.size() << " bytes, dropping");
        return true;
    }

    // Drop pre-session audio frames (captured before first video frame).
    // These accumulate in the tap during encoder startup and would flood the
    // stream with PTS=0 frames followed by a sudden jump to real PTS.
    if (s.startAnchor.tv_sec != 0 &&
        (af.time.tv_sec < s.startAnchor.tv_sec ||
         (af.time.tv_sec == s.startAnchor.tv_sec &&
          af.time.tv_usec < s.startAnchor.tv_usec))) {
        return true;
    }

    // Read codec from audio config
    std::string codec = "AAC";
    int sampleRate = 16000;
    uint8_t pt = 97;
    if (!audioStreams_.empty()) {
        codec = audioStreams_[0].config.codec;
        sampleRate = audioStreams_[0].config.sampleRate;
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
        // Anchor on first audio frame's capture time if not already set
        if (s.audioRtp.timestamp == 0 && s.startAnchor.tv_sec == 0 && s.startAnchor.tv_usec == 0) {
            s.startAnchor = af.time;
            dtUs = 0;
        }
        if (dtUs < 0) dtUs = 0;
        s.audioRtp.timestamp = static_cast<uint32_t>(
            dtUs * static_cast<int64_t>(sampleRate) / 1000000LL);
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
            return static_cast<size_t>(n) == len;
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
        return sendOne(be.data(), be.size(), pt,
                       /*marker*/ true, s.audioRtp, output);
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

// ── RTCP Sender Report ──────────────────────────────────────────────────────

void RtspServer::sendRtcpSr(Session &s) {
    if (s.fd < 0) return;
    uint64_t ntp = simple_rtsp::ntpTimestamp();
    uint32_t ntpMsw = htonl(static_cast<uint32_t>(ntp >> 32));
    uint32_t ntpLsw = htonl(static_cast<uint32_t>(ntp & 0xFFFFFFFF));
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
