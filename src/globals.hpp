#ifndef GLOBALS_HPP
#define GLOBALS_HPP

#include <memory>
#include <functional>
#include <atomic>
#include <vector>
#include <array>
#include <mutex>
#include <condition_variable>
#include "liveMedia.hh"

#include "MsgChannel.hpp"
#include "IMPAudio.hpp"
#include "IMPAudioOutput.hpp"
#include "IMPEncoder.hpp"
#include "IMPFramesource.hpp"
#include "IMPBackchannel.hpp"
#include "MP4Recorder.hpp"

#define MSG_CHANNEL_SIZE 20
#define BACKCHANNEL_QUEUE_SIZE 200
#define AUDIO_OUTPUT_QUEUE_SIZE 64
#define NUM_AUDIO_CHANNELS 1
#define NUM_VIDEO_CHANNELS 2

using namespace std::chrono;

// Simple binary semaphore compatible with environments lacking std::binary_semaphore
class binary_semaphore_compat {
public:
    explicit binary_semaphore_compat(int initial = 0) : count(initial) {}

    void release() {
        std::lock_guard<std::mutex> lock(m);
        if (count == 0) {
            count = 1;
            cv.notify_one();
        }
    }

    void acquire() {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&]{ return count > 0; });
        count = 0;
    }

private:
    std::mutex m;
    std::condition_variable cv;
    int count;
};

extern std::mutex mutex_main; // protects global_restart_rtsp and global_restart_video

struct AudioFrame
{
    std::vector<uint8_t> data;
    struct timeval time;
};

struct H264NALUnit
{
    std::vector<uint8_t> data;
    /* timestamp fix, can be removed if solved
    struct timeval time;
    int64_t imp_ts;
    */
};

struct BackchannelFrame
{
    std::vector<uint8_t> payload;
    IMPBackchannelFormat format;
    unsigned int clientSessionId;
    bool isShutdownSentinel{false};
};

enum class AudioPlaybackJobType
{
    PCM,
    CLEAR,
    STOP
};

struct AudioPlaybackJob
{
    AudioPlaybackJobType type{AudioPlaybackJobType::PCM};
    std::vector<int16_t> samples;
    bool hasVolume{false};
    int volume{0};
    bool hasGain{false};
    int gain{0};
};

struct jpeg_stream
{
    int encChn;
    int streamChn;
    _stream *stream;
    std::atomic<bool> running; // set to false to make jpeg_grabber thread exit
    std::atomic<bool> active{false};
    pthread_t thread;
    IMPEncoder *imp_encoder;
    std::condition_variable should_grab_frames;
    binary_semaphore_compat is_activated{0};

    steady_clock::time_point last_image;
    steady_clock::time_point last_subscriber;

    void request()
    {
        auto now = steady_clock::now();
        std::unique_lock lck(mutex_main);
        last_subscriber = now;
    }

    bool request_or_overrun() {
        return duration_cast<milliseconds>(steady_clock::now() - last_subscriber).count() < 1000;
    }

    jpeg_stream(int encChn, _stream *stream)
        : encChn(encChn), stream(stream), running(false), imp_encoder(nullptr) {}
};

struct audio_stream
{
    int devId;
    int aiChn;
    int aeChn;
    bool running;
    bool active{false};
    pthread_t thread;
    IMPAudio *imp_audio;
    std::shared_ptr<MsgChannel<AudioFrame>> msgChannel;
    std::function<void(void)> onDataCallback;
    /* Check whether onDataCallback is not null in a data race free manner.
     * Use only for optimizations, i.e., to skip work if no data callback
     * is registered right now.
     */
    std::atomic<bool> hasDataCallback;
    std::mutex onDataCallbackLock; // protects onDataCallback from deallocation
    std::condition_variable should_grab_frames;
    binary_semaphore_compat is_activated{0};

    StreamReplicator *streamReplicator = nullptr;

    audio_stream(int devId, int aiChn, int aeChn)
        : devId(devId), aiChn(aiChn), aeChn(aeChn), running(false), imp_audio(nullptr),
          msgChannel(std::make_shared<MsgChannel<AudioFrame>>(30)),
          onDataCallback{nullptr}, hasDataCallback{false} {}
};

struct video_stream
{
    int encChn;
    _stream *stream;
    const char *name;
    bool running;
    pthread_t thread;
    bool idr;
    int idr_fix;
    bool active{false};
    IMPEncoder *imp_encoder;
    IMPFramesource *imp_framesource;
    std::shared_ptr<MsgChannel<H264NALUnit>> msgChannel;
    std::function<void(void)> onDataCallback;
    bool run_for_jpeg;                 // see comment in audio_stream
    std::atomic<bool> hasDataCallback; // see comment in audio_stream
    std::atomic<bool> mp4_waiting_for_idr;
    std::atomic<int64_t> mp4_required_idr_ts;
    std::atomic<int64_t> mp4_last_idr_ts;
    std::atomic<uint64_t> mp4_last_idr_request_ms;
    std::mutex onDataCallbackLock;     // protects onDataCallback from deallocation
    std::condition_variable should_grab_frames;
    binary_semaphore_compat is_activated{0};
    std::mutex codec_config_mutex;
    std::vector<uint8_t> latest_sps;
    std::vector<uint8_t> latest_pps;
    bool have_sps;
    bool have_pps;

    video_stream(int encChn, _stream *stream, const char *name)
        : encChn(encChn), stream(stream), name(name), running(false), idr(false), idr_fix(0),
          imp_encoder(nullptr), imp_framesource(nullptr),
          msgChannel(std::make_shared<MsgChannel<H264NALUnit>>(MSG_CHANNEL_SIZE)),
                    onDataCallback(nullptr), run_for_jpeg{false}, hasDataCallback{false},
                    mp4_waiting_for_idr{false}, mp4_required_idr_ts{-1}, mp4_last_idr_ts{-1},
                    mp4_last_idr_request_ms{0},
          have_sps(false), have_pps(false) {}
};

struct backchannel_stream
{
    std::shared_ptr<MsgChannel<BackchannelFrame>> inputQueue;
    IMPBackchannel* imp_backchannel;
    bool running;
    pthread_t thread;
    std::mutex mutex;
    std::condition_variable should_grab_frames;
    std::atomic<unsigned int> is_sending{0};

    backchannel_stream()
        : inputQueue(std::make_shared<MsgChannel<BackchannelFrame>>(BACKCHANNEL_QUEUE_SIZE)),
        imp_backchannel(nullptr),
        running(false) {}
};

    struct audio_output_stream
    {
        std::shared_ptr<MsgChannel<AudioPlaybackJob>> jobQueue;
        std::atomic<bool> running{false};
        pthread_t thread;
        std::unique_ptr<class IMPAudioOutput> imp_audio_output;
        std::mutex control_mutex;
        int current_volume{0};
        int current_gain{0};

        audio_output_stream()
        : jobQueue(std::make_shared<MsgChannel<AudioPlaybackJob>>(AUDIO_OUTPUT_QUEUE_SIZE)) {}
    };


extern std::condition_variable global_cv_worker_restart;

extern bool global_restart;
extern bool global_restart_rtsp;
extern bool global_restart_video;
extern bool global_restart_audio;

extern bool global_osd_thread_signal;
extern bool global_main_thread_signal;
extern bool global_motion_thread_signal;
extern std::atomic<char> global_rtsp_thread_signal;

extern std::shared_ptr<jpeg_stream> global_jpeg[NUM_VIDEO_CHANNELS];
extern std::shared_ptr<audio_stream> global_audio[NUM_AUDIO_CHANNELS];
extern std::shared_ptr<video_stream> global_video[NUM_VIDEO_CHANNELS];
extern std::shared_ptr<backchannel_stream> global_backchannel;
extern std::shared_ptr<audio_output_stream> global_audio_output;

extern std::array<MP4Recorder, NUM_VIDEO_CHANNELS> global_mp4_recorders;
extern std::atomic<int> global_mp4_active_recorders;
extern std::atomic<bool> global_shutdown_requested;

// When true, video workers should keep polling/grabbing frames even if
// there is no RTSP/WS client attached. This is used by the MP4 recorder
// so that a START command over the control FIFO does not require an
// external streaming client.
extern std::atomic<bool> global_force_video_active;

#endif // GLOBALS_HPP
