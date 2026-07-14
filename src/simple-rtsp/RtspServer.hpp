#pragma once

#include "RtspTypes.hpp"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Need NUM_VIDEO_CHANNELS, video_stream, audio_stream from globals.hpp
#include "globals.hpp"

namespace simple_rtsp {

struct Session;

class RtspServer {
public:
    RtspServer();
    ~RtspServer();

    // ── Configuration (must be called before start()) ───────────────────

    void setAuthCredentials(const std::string &user, const std::string &pass);
    void setSendBufferSize(int bytes);
    void setSendTimeout(int seconds);
    void setStreamName(const std::string &name);
    void setStreamInfo(const std::string &info);

    // ── Stream registration ─────────────────────────────────────────────

    // Register a video stream.  The server will create taps on the
    // video_state's msgChannel per client.
    // `chn` = encoder channel (0 or 1).
    void addVideoStream(int chn, const VideoStreamConfig &config,
                        std::shared_ptr<video_stream> videoState);

    // Register an audio stream (multiplexed with video).
    void addAudioStream(int chn, const AudioStreamConfig &config,
                        std::shared_ptr<audio_stream> audioState);

    // Register a standalone audio-only endpoint (e.g. /mic, no video).
    void addAudioOnlyStream(const AudioStreamConfig &config,
                            std::shared_ptr<audio_stream> audioState);

    // ── Lifecycle ───────────────────────────────────────────────────────

    bool start(int port);
    void stop();

    bool isRunning() const { return running_.load(std::memory_order_relaxed); }

    // ── Thread entry (compatible with existing main.cpp) ─────────────────
    static void *run(void *arg);

private:
    void eventLoop();

    // Client connection handling
    void acceptClient();
    void closeClient(int idx);

    // RTSP request dispatch
    void handleRequest(int clientIdx);

    // Per-method handlers – return the CSeq from the request
    void handleOptions(int clientIdx, int cseq);
    void handleDescribe(int clientIdx, int cseq, const char *uri);
    void handleSetup(int clientIdx, int cseq, const char *uri,
                     const char *headers);
    void handlePlay(int clientIdx, int cseq, const char *uri,
                    const char *headers);
    void handleTeardown(int clientIdx, int cseq, const char *headers);

    // Socket helpers
    void sendResponse(Session &s, Status status, int cseq,
                      const char *extraHeaders, const char *body);
    void sendInterleaved(int fd, uint8_t channel,
                         const uint8_t *data, size_t len);

    // RTP streaming helpers
    bool sendVideoNal(Session &s, const H264NALUnit &nal);
    bool sendAudioFrame(Session &s, const AudioFrame &af);

    // RTCP
    void sendRtcpSr(Session &s);

    // Timers & cleanup
    void checkSessionTimeouts();
    void cleanupAllSessions();

    // ── State ───────────────────────────────────────────────────────────

    int serverFd_ = -1;
    int port_      = 554;

    std::string streamName_ = "thingino prudynt";
    std::string streamInfo_ = "stream0";
    std::string username_;
    std::string password_;
    bool authRequired_ = false;

    int sendBufSize_  = 65536;
    int sendTimeoutS_ = 5;

    std::atomic<bool> running_{false};
    std::thread eventThread_;

    // Registered streams
    struct VideoEntry {
        int chn;
        VideoStreamConfig config;
        std::shared_ptr<video_stream> state;
    };
    std::vector<VideoEntry> videoStreams_;

    struct AudioEntry {
        int chn;
        AudioStreamConfig config;
        std::shared_ptr<audio_stream> state;
    };
    std::vector<AudioEntry> audioStreams_;

    // Standalone audio-only endpoints (e.g. /mic)
    struct AudioOnlyEntry {
        AudioStreamConfig config;
        std::shared_ptr<audio_stream> state;
    };
    std::vector<AudioOnlyEntry> audioOnlyStreams_;

    // Session objects (one per connected client)
    std::vector<std::unique_ptr<Session>> sessions_;

    // How many sessions are actively streaming per channel
    int activePlayers_[NUM_VIDEO_CHANNELS] = {};
    int activeAudioPlayers_ = 0;
};

} // namespace simple_rtsp
