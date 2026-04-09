#ifndef GLOBALS_HPP
#define GLOBALS_HPP

#include "IMPAudio.hpp"
#include "IMPAudioOutput.hpp"
#include "IMPBackchannel.hpp"
#include "IMPEncoder.hpp"
#include "IMPFramesource.hpp"
#include "MP4Recorder.hpp"
#include "MsgChannel.hpp"
#ifdef PREBUFFER_ENABLED
#include "PreTriggerBuffer.hpp"
#endif
#include "liveMedia.hh"
#include "StreamCore.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define MSG_CHANNEL_SIZE 200
#define BACKCHANNEL_QUEUE_SIZE 200
#define AUDIO_OUTPUT_QUEUE_SIZE 64
#define NUM_AUDIO_CHANNELS 1
#define NUM_VIDEO_CHANNELS 2
#define NUM_JPEG_CHANNELS 2

using namespace std::chrono;

// Simple binary semaphore compatible with environments lacking
// std::binary_semaphore
class binary_semaphore_compat {
public:
  explicit binary_semaphore_compat(int initial = 0) : count(initial) {
  }

  void release() {
    std::lock_guard<std::mutex> lock(m);
    if (count == 0) {
      count = 1;
      cv.notify_one();
    }
  }

  void acquire() {
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return count > 0; });
    count = 0;
  }

private:
  std::mutex m;
  std::condition_variable cv;
  int count;
};

extern std::mutex mutex_main; // protects global_restart_rtsp and global_restart_video

struct AudioFrame {
  std::vector<uint8_t> data;
  struct timeval time;
  uint64_t duration_us{0};
};

struct H264NALUnit {
  std::vector<uint8_t> data;

  // Frame boundary tracking (detect incomplete frames)
  bool is_frame_start = false;      // First NAL unit of frame
  bool is_frame_end = false;        // Last NAL unit of frame
  uint32_t frame_id = 0;            // Unique per video frame
  uint32_t packet_index = 0;        // Position within frame (0-based)
  uint32_t packet_count = 0;        // Total NAL units in frame

  struct timeval time{0, 0};
  // Encoder timestamp in microseconds (from IMP encoder, monotonic)
  int64_t imp_ts = 0;
};

struct BackchannelFrame {
  std::vector<uint8_t> payload;
  IMPBackchannelFormat format;
  unsigned int clientSessionId;
  bool isShutdownSentinel{false};
};

/**
 * StreamCoreTraits specializations for H264NALUnit and AudioFrame.
 * These traits allow StreamCore to identify sync frames (keyframes/IDR).
 */
template <>
struct StreamCoreTraits<H264NALUnit>
{
    static bool is_sync(const H264NALUnit &nalu)
    {
        if (nalu.data.size() < 1)
            return false;
        // NAL unit type is in bits 0-4 of first byte (strip start code if present)
        size_t offset = 0;
        while (offset + 2 < nalu.data.size() && nalu.data[offset] == 0 && nalu.data[offset + 1] == 0)
        {
            if (nalu.data[offset + 2] == 1)
            {
                offset += 3;
                break;
            }
            offset++;
        }
        if (offset >= nalu.data.size())
            return false;
        uint8_t nalType = nalu.data[offset] & 0x1F;
        return nalType == 5; // IDR slice
    }
};

template <>
struct StreamCoreTraits<AudioFrame>
{
    static bool is_sync(const AudioFrame &)
    {
        return false; // Audio frames are not sync frames
    }
};

class VideoPrivacyMask;

/**
 * video_parameter_cache — ported from Prudynt-SE.
 *
 * A single mutex-protected struct that holds the most recent SPS/PPS/VPS NAL
 * units plus IDR timing metadata.  VideoWorker writes here whenever it sees
 * a new parameter-set NAL; RTSP::addSubsession() waits on the condition
 * variable instead of consuming live frames from msgChannel on startup.
 */
struct video_parameter_cache {
  std::mutex mutex;
  std::condition_variable cv;
  H264NALUnit sps;
  H264NALUnit pps;
  H264NALUnit vps;
  H264NALUnit latest_sync; // most recent IDR/keyframe NAL unit
  bool have_sps{false};
  bool have_pps{false};
  bool have_vps{false};
  bool have_latest_sync{false};
  uint64_t last_idr_us{0}; // hardware timestamp (IMP µs) of last IDR
  uint8_t profile_idc{0};
  uint8_t level_idc{0};
};

enum class AudioPlaybackJobType { PCM, CLEAR, STOP, WAIT, RECONFIGURE };

struct AudioPlaybackJob {
  AudioPlaybackJobType type{AudioPlaybackJobType::PCM};
  std::vector<int16_t> samples;
  /// Desired AO sample rate for RECONFIGURE jobs, or 0 to keep current.
  int sampleRate{0};
  bool hasVolume{false};
  int volume{0};
  bool hasGain{false};
  int gain{0};
  bool hasMute{false};
  bool mute{false};
  int wait_ms{0};
  bool flush_after_wait{false};
  int silence_ms{0};
  std::shared_ptr<std::promise<void>> completion;
};

struct jpeg_stream {
  int encChn;
  int streamChn;
  _stream *stream;
  std::atomic<bool> running; // set to false to make jpeg_grabber thread exit
  std::atomic<bool> active{false};
  pthread_t thread;
  IMPEncoder *imp_encoder;
  std::condition_variable should_grab_frames;
  binary_semaphore_compat is_activated{0};

  // In-memory snapshot buffer (JPEG bytes only), guarded by mutex_main when updated
  std::vector<unsigned char> snapshot_buf;
  // Per-request JPEG quality override (1..100, -1 = none)
  std::atomic<int> quality_override{-1};

  // Dynamic reconfiguration requests (applied by JPEGWorker)
  // Sequential frame counter for TRACE diagnostics (32-bit to avoid 64-bit atomics)
  std::atomic<uint32_t> frame_seq{0};
  std::atomic<int> req_width{-1};
  std::atomic<int> req_height{-1};
  std::atomic<int> req_fps{-1};
  std::atomic<bool> reconfig{false};

  steady_clock::time_point last_image;
  steady_clock::time_point last_subscriber;

  void request() {
    auto now = steady_clock::now();
    {
      std::unique_lock lck(mutex_main);
      last_subscriber = now;
    }
    // Wake JPEG worker if it's sleeping
    should_grab_frames.notify_one();
  }

  bool request_or_overrun() {
    return duration_cast<milliseconds>(steady_clock::now() - last_subscriber).count() < 1000;
  }

  jpeg_stream(int encChn, _stream *stream) : encChn(encChn), stream(stream), running(false), imp_encoder(nullptr) {
  }
};

struct audio_stream {
  int devId;
  int aiChn;
  int aeChn;
  bool running;
  bool active{false};
  pthread_t thread;
  IMPAudio *imp_audio;
  // StreamCore replaces msgChannel + audio_taps mechanism
  std::unique_ptr<StreamCore<AudioFrame>> audioCore;
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
  std::atomic<int> rtsp_client_count{0};
  
  // Timestamp normalization tracking (from Prudynt-SE)
  std::atomic<uint64_t> last_timestamp_us{0};
  std::atomic<uint64_t> timestamp_origin_raw{0};
  std::atomic<uint64_t> presentation_origin_us{0};

  audio_stream(int devId, int aiChn, int aeChn)
      : devId(devId), aiChn(aiChn), aeChn(aeChn), running(false), imp_audio(nullptr),
        audioCore(std::make_unique<StreamCore<AudioFrame>>(MSG_CHANNEL_SIZE)),
        onDataCallback{nullptr}, hasDataCallback{false} {
  }
};

struct video_stream {
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
  // StreamCore replaces msgChannel + video_taps mechanism
  std::unique_ptr<StreamCore<H264NALUnit>> videoCore;
  
  // Timestamp normalization tracking (from Prudynt-SE)
  std::atomic<uint64_t> timestamp_origin_raw{0};
  std::atomic<uint64_t> last_frame_timestamp_raw{0};
  std::atomic<uint64_t> last_timestamp_us{0};
  std::atomic<uint64_t> presentation_origin_us{0};
  std::function<void(void)> onDataCallback;
  bool run_for_jpeg;                 // see comment in audio_stream
  std::atomic<bool> hasDataCallback; // see comment in audio_stream
  std::atomic<bool> mp4_waiting_for_idr;
  std::atomic<int64_t> mp4_required_idr_ts_us;
  std::atomic<int64_t> mp4_last_idr_ts_us;
  std::atomic<uint64_t> mp4_last_idr_request_ms;
  std::atomic<int64_t> mp4_prebuffer_offset_ms;  // Offset for live frames when prebuffer is used
  std::atomic<bool> mp4_prebuffer_flushing;      // True while prebuffer frames are being written
  std::mutex onDataCallbackLock;     // protects onDataCallback from deallocation
  std::condition_variable should_grab_frames;
  binary_semaphore_compat is_activated{0};
  video_parameter_cache parameterCache;
  std::mutex privacy_mutex;
  std::shared_ptr<VideoPrivacyMask> privacy_mask;
  std::atomic<bool> privacy_requested{false};

#ifdef PREBUFFER_ENABLED
  // Pre-trigger buffer for MP4 recording
  std::unique_ptr<PreTriggerBuffer> prebuffer;
#endif

  video_stream(int encChn, _stream *stream, const char *name)
      : encChn(encChn), stream(stream), name(name), running(false), idr(false), idr_fix(0), imp_encoder(nullptr),
        imp_framesource(nullptr), videoCore(std::make_unique<StreamCore<H264NALUnit>>(MSG_CHANNEL_SIZE)),
        onDataCallback(nullptr), run_for_jpeg{false}, hasDataCallback{false}, mp4_waiting_for_idr{false},
        mp4_required_idr_ts_us{-1}, mp4_last_idr_ts_us{-1}, mp4_last_idr_request_ms{0}, mp4_prebuffer_offset_ms{0},
        mp4_prebuffer_flushing{false} {
  }
};

struct backchannel_stream {
  std::shared_ptr<MsgChannel<BackchannelFrame>> inputQueue;
  IMPBackchannel *imp_backchannel;
  bool running;
  pthread_t thread;
  std::mutex mutex;
  std::condition_variable should_grab_frames;
  std::atomic<unsigned int> is_sending{0};

  backchannel_stream()
      : inputQueue(std::make_shared<MsgChannel<BackchannelFrame>>(BACKCHANNEL_QUEUE_SIZE)), imp_backchannel(nullptr),
        running(false) {
  }
};

struct audio_output_stream {
  std::shared_ptr<MsgChannel<AudioPlaybackJob>> jobQueue;
  std::atomic<bool> running{false};
  pthread_t thread;
  std::unique_ptr<class IMPAudioOutput> imp_audio_output;
  std::mutex control_mutex;
  int current_volume{0};
  int current_gain{0};
  bool current_mute{false};
  /// Actual hardware sample rate after IMP_AO_GetPubAttr. May differ from
  /// the configured output_sample_rate on platforms where the CODEC clock
  /// is shared between AI and AO (T10/T20/T21).
  std::atomic<int> hardwareSampleRate{0};

  audio_output_stream() : jobQueue(std::make_shared<MsgChannel<AudioPlaybackJob>>(AUDIO_OUTPUT_QUEUE_SIZE)) {
  }
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
extern std::atomic<int> global_rtsp_clients;

extern std::shared_ptr<jpeg_stream> global_jpeg[NUM_JPEG_CHANNELS];
extern std::shared_ptr<audio_stream> global_audio[NUM_AUDIO_CHANNELS];
extern std::shared_ptr<video_stream> global_video[NUM_VIDEO_CHANNELS];
extern std::shared_ptr<backchannel_stream> global_backchannel;
extern std::shared_ptr<audio_output_stream> global_audio_output;

extern std::array<MP4Recorder, NUM_VIDEO_CHANNELS> global_mp4_recorders;
extern std::atomic<int> global_mp4_active_recorders;
extern std::atomic<bool> global_shutdown_requested;

struct DayNightHistorySample {
  int64_t time_now{0};
  int ev{0};
  int gb{0};
  int gr{0};
  int total_gain{0};
  int ae_luma{0};
  int awb_color_temp{0};
  int daynight_brightness{0};
  int total_gain_night_threshold{0};
  int total_gain_day_threshold{0};
  std::string daynight_mode{"unknown"};
};

class DayNightHistoryBuffer {
public:
  static constexpr size_t kMaxEntries = 300;

  void push(const DayNightHistorySample &sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_.size() >= kMaxEntries) {
      samples_.pop_front();
    }
    samples_.push_back(sample);
  }

  std::vector<DayNightHistorySample> snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<DayNightHistorySample>(samples_.begin(), samples_.end());
  }

private:
  mutable std::mutex mutex_;
  std::deque<DayNightHistorySample> samples_;
};

extern DayNightHistoryBuffer global_daynight_history;

// When true, video workers should keep polling/grabbing frames even if
// there is no RTSP/WS client attached. This is used by the MP4 recorder
// so that a START command over the control FIFO does not require an
// external streaming client.
extern std::atomic<bool> global_force_video_active;

#endif // GLOBALS_HPP
