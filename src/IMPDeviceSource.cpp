#include "IMPDeviceSource.hpp"
#include "GroupsockHelper.hh"
#include "Logger.hpp"
#include <algorithm>
#include <cstring>
#include <type_traits>
#include <unordered_map>

#define MODULE "IMPDeviceSource"

static inline struct timeval us_to_tv(uint64_t us) {
  struct timeval tv;
  tv.tv_sec = us / 1000000ULL;
  tv.tv_usec = us % 1000000ULL;
  return tv;
}

namespace {
std::mutex gSessionVideoReadyMutex;
std::unordered_map<unsigned, bool> gSessionVideoReady;

bool is_session_video_ready(unsigned clientSessionId) {
  if (clientSessionId == 0)
    return true;

  std::lock_guard<std::mutex> lock(gSessionVideoReadyMutex);
  const auto it = gSessionVideoReady.find(clientSessionId);
  return it != gSessionVideoReady.end() && it->second;
}

void set_session_video_ready(unsigned clientSessionId, bool ready) {
  if (clientSessionId == 0)
    return;

  std::lock_guard<std::mutex> lock(gSessionVideoReadyMutex);
  gSessionVideoReady[clientSessionId] = ready;
}

void clear_session_video_ready(unsigned clientSessionId) {
  if (clientSessionId == 0)
    return;

  std::lock_guard<std::mutex> lock(gSessionVideoReadyMutex);
  gSessionVideoReady.erase(clientSessionId);
}
} // namespace

// explicit instantiation
template class IMPDeviceSource<H264NALUnit, video_stream>;
template class IMPDeviceSource<AudioFrame, audio_stream>;

template <typename FrameType, typename Stream>
IMPDeviceSource<FrameType, Stream> *
IMPDeviceSource<FrameType, Stream>::createNew(UsageEnvironment &env, int encChn,
                                              std::shared_ptr<Stream> stream,
                                              const char *name,
                                              bool eagerActivate,
                                              unsigned clientSessionId) {
  return new IMPDeviceSource<FrameType, Stream>(env, encChn, stream, name,
                                                eagerActivate, clientSessionId);
}

template <typename FrameType, typename Stream>
IMPDeviceSource<FrameType, Stream>::IMPDeviceSource(
    UsageEnvironment &env, int encChn, std::shared_ptr<Stream> stream,
    const char *name, bool eagerActivate, unsigned clientSessionId)
    : FramedSource(env), encChn(encChn), clientSessionId(clientSessionId),
      stream{stream}, name{name}, eventTriggerId(0),
      eagerActivate(eagerActivate) {
  eventTriggerId = envir().taskScheduler().createEventTrigger(deliverFrame0);
  if (eagerActivate) {
    setCaptureEnabled(true);
  }
  LOG_DEBUG("IMPDeviceSource "
            << name << " constructed, encoder channel:" << encChn
            << " session=" << clientSessionId << " eager=" << eagerActivate);
}

template <typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::deinit() {
  setCaptureEnabled(false);
  if (eventTriggerId != 0) {
    envir().taskScheduler().deleteEventTrigger(eventTriggerId);
    eventTriggerId = 0;
  }
  if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
    clear_session_video_ready(clientSessionId);
  }
  LOG_DEBUG("IMPDeviceSource " << name << " deinit complete, encoder channel:"
                               << encChn << " session=" << clientSessionId);
}

template <typename FrameType, typename Stream>
IMPDeviceSource<FrameType, Stream>::~IMPDeviceSource() {
  deinit();
}

template <typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::doGetNextFrame() {
  if (!captureEnabled) {
    setCaptureEnabled(true);
  }
  deliverFrame();
}

template <typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::doStopGettingFrames() {
  FramedSource::doStopGettingFrames();
  if (!eagerActivate && captureEnabled) {
    setCaptureEnabled(false);
  }
}

template <typename FrameType, typename Stream>
uint64_t IMPDeviceSource<FrameType, Stream>::normalizePresentationTimeUs(
    uint64_t sourceFrameUs, uint64_t durationUs) {
  const uint64_t fallbackDurationUs = std::max<uint64_t>(durationUs, 1);

  // If source frame hasn't changed, return same presentation time
  if (sourceFrameUs != 0 && lastSourceFrameUs != 0 &&
      sourceFrameUs == lastSourceFrameUs && lastPresentationFrameUs != 0) {
    return lastPresentationFrameUs;
  }

  uint64_t normalizedUs = 0;
  if (sourceFrameUs != 0) {
    // Establish anchor on first frame
    if (presentationAnchorUs == 0) {
      if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
        presentationAnchorUs = sourceFrameUs > fallbackDurationUs
                                   ? sourceFrameUs - fallbackDurationUs
                                   : 0;
      } else {
        // AAC demuxers commonly derive stream start_time as first_pts -
        // frame_duration. Anchor audio one frame earlier so the first emitted
        // packet lands at +duration, which yields a clean zero start_time
        // instead of -0.064 on 16 kHz AAC.
        presentationAnchorUs = sourceFrameUs > fallbackDurationUs
                                   ? sourceFrameUs - fallbackDurationUs
                                   : 0;
      }
    }

    if (sourceFrameUs >= presentationAnchorUs) {
      normalizedUs = sourceFrameUs - presentationAnchorUs;
    }
  }

  // Monotonicity check
  if (lastPresentationFrameUs != 0 && normalizedUs <= lastPresentationFrameUs) {
    normalizedUs = lastPresentationFrameUs + fallbackDurationUs;
  }

  lastSourceFrameUs = sourceFrameUs;
  lastPresentationFrameUs = normalizedUs;
  return normalizedUs;
}

template <typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::deliverFrame0(void *clientData) {
  ((IMPDeviceSource<FrameType, Stream> *)clientData)->deliverFrame();
}

template <typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::setCaptureEnabled(bool enabled) {
  bool request_idr = false;

  {
    std::lock_guard lock_stream{mutex_main};
    std::lock_guard lock_callback{stream->onDataCallbackLock};

    if (captureEnabled == enabled) {
      return;
    }

    if (enabled) {
      presentationAnchorUs = 0;
      lastSourceFrameUs = 0;
      lastPresentationFrameUs = 0;

      if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
        set_session_video_ready(clientSessionId, false);
        cursor.emplace(stream->videoCore->registerSubscriber(
            [this]() { this->on_data_available(); },
            StreamStartPolicy::LatestSync));
        stream->hasDataCallback.store(stream->videoCore->subscriberCount() > 0,
                                      std::memory_order_relaxed);
        request_idr = true;
      } else {
        cursor.emplace(stream->audioCore->registerSubscriber(
            [this]() { this->on_data_available(); },
            StreamStartPolicy::LiveEdge));
        stream->hasDataCallback.store(stream->audioCore->subscriberCount() > 0,
                                      std::memory_order_relaxed);
      }

      // StreamCore subscriber callbacks drive delivery; this flag callback is
      // only used by worker run-state logic and must never reference a
      // per-session object that might be destroyed while other subscribers stay
      // active.
      stream->onDataCallback = []() {};
      stream->should_grab_frames.notify_one();
    } else {
      if (cursor.has_value()) {
        if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
          stream->videoCore->unregisterSubscriber(*cursor);
          clear_session_video_ready(clientSessionId);
          stream->hasDataCallback.store(stream->videoCore->subscriberCount() >
                                            0,
                                        std::memory_order_relaxed);
        } else {
          stream->audioCore->unregisterSubscriber(*cursor);
          stream->hasDataCallback.store(stream->audioCore->subscriberCount() >
                                            0,
                                        std::memory_order_relaxed);
        }
        cursor.reset();
      }

      if (!stream->hasDataCallback.load(std::memory_order_relaxed)) {
        stream->onDataCallback = nullptr;
      }
    }

    captureEnabled = enabled;
  }

  if (request_idr) {
    IMP_Encoder_RequestIDR(encChn);
  }
}

template <typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::deliverFrame() {
  if (!isCurrentlyAwaitingData()) {
    return;
  }

  if constexpr (std::is_same_v<FrameType, AudioFrame>) {
    if (!is_session_video_ready(clientSessionId)) {
      return;
    }
  }

  if (!cursor.has_value()) {
    fFrameSize = 0;
    return;
  }

  FrameType nal;
  // Use StreamCore cursor instead of msgChannel
  bool hasFrame;
  if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
    hasFrame = stream->videoCore->read(*cursor, &nal);
  } else {
    hasFrame = stream->audioCore->read(*cursor, &nal);
  }

  while (hasFrame) {
    if (nal.data.empty()) {
      if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
        hasFrame = stream->videoCore->read(*cursor, &nal);
      } else {
        hasFrame = stream->audioCore->read(*cursor, &nal);
      }
      continue;
    }

    if (nal.data.size() > fMaxSize) {
      fFrameSize = fMaxSize;
      fNumTruncatedBytes = nal.data.size() - fMaxSize;
    } else {
      fFrameSize = nal.data.size();
    }

    // Timestamps are already normalized by VideoWorker/AudioWorker, but we need
    // to normalize AGAIN in IMPDeviceSource (like Prudynt-SE does) to establish
    // a fresh anchor point per RTSP session/subscriber
    uint64_t source_frame_us =
        static_cast<uint64_t>(nal.time.tv_sec) * 1000000ULL +
        static_cast<uint64_t>(nal.time.tv_usec);

    uint64_t duration_hint_us = 0;
    if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
      const int fps = (stream && stream->stream && stream->stream->fps > 0)
                          ? stream->stream->fps
                          : 25;
      duration_hint_us = 1000000ULL / static_cast<uint64_t>(fps);
    } else {
      duration_hint_us = nal.duration_us;
      if (duration_hint_us == 0) {
        // AAC: 1024 samples at 16kHz = 64ms
        auto *imp_audio = global_audio[encChn]->imp_audio;
        int sampleRate = imp_audio ? imp_audio->sample_rate : 16000;
        if (sampleRate <= 0)
          sampleRate = 16000;
        duration_hint_us =
            (1024ULL * 1000000ULL) / static_cast<uint64_t>(sampleRate);
      }
    }

    uint64_t duration_us = duration_hint_us > 0 ? duration_hint_us : 1;
    if (lastSourceFrameUs != 0) {
      if constexpr (std::is_same_v<FrameType, AudioFrame>) {
        if (duration_hint_us > 0) {
          if (source_frame_us > lastSourceFrameUs) {
            const uint64_t raw_delta = source_frame_us - lastSourceFrameUs;
            if (raw_delta > duration_hint_us * 2) {
              static unsigned audio_jump_logs = 0;
              if (audio_jump_logs < 16) {
                LOG_DEBUG(
                    "IMPDeviceSource audio timestamp jump: source_delta_us="
                    << raw_delta << " expected_us=" << duration_hint_us
                    << " (using expected cadence)");
                ++audio_jump_logs;
              }
            }
          }
          // Keep audio cadence stable per RTP packet duration, even if source
          // timestamps jump due queue skips or producer resets.
          source_frame_us = lastSourceFrameUs + duration_hint_us;
          duration_us = duration_hint_us;
        }
      } else if (source_frame_us > lastSourceFrameUs) {
        duration_us = source_frame_us - lastSourceFrameUs;
      }
    }

    const uint64_t presentation_us =
        normalizePresentationTimeUs(source_frame_us, duration_us);
    fPresentationTime = us_to_tv(presentation_us);
    fDurationInMicroseconds = duration_us;

    if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
      set_session_video_ready(clientSessionId, true);
    }

    memcpy(fTo, nal.data.data(), fFrameSize);

    if (fFrameSize > 0) {
      FramedSource::afterGetting(this);
      return;
    }

    // Try to read next frame
    if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
      hasFrame = stream->videoCore->read(*cursor, &nal);
    } else {
      hasFrame = stream->audioCore->read(*cursor, &nal);
    }
  }
  fFrameSize = 0;
}
