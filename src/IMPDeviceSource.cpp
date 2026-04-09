#include "IMPDeviceSource.hpp"
#include "GroupsockHelper.hh"
#include <cstring>
#include <iostream>
#include <type_traits>
#include "Logger.hpp"

#define MODULE "IMPDeviceSource"

static inline int64_t tv_to_us(const struct timeval &tv) {
  return static_cast<int64_t>(tv.tv_sec) * 1000000LL + static_cast<int64_t>(tv.tv_usec);
}

static inline struct timeval us_to_tv(int64_t us) {
  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(us / 1000000LL);
  tv.tv_usec = static_cast<suseconds_t>(us % 1000000LL);
  if (tv.tv_usec < 0) {
    tv.tv_sec -= 1;
    tv.tv_usec += 1000000;
  }
  return tv;
}

static inline bool is_plausible_wallclock_tv(const struct timeval &tv) {
  if (tv.tv_usec < 0 || tv.tv_usec >= 1000000) {
    return false;
  }
  constexpr time_t kMinUnixTime = 946684800; // 2000-01-01
  return tv.tv_sec >= kMinUnixTime;
}

// explicit instantiation
template class IMPDeviceSource<H264NALUnit, video_stream>;
template class IMPDeviceSource<AudioFrame, audio_stream>;

template <typename FrameType, typename Stream>
IMPDeviceSource<FrameType, Stream> *IMPDeviceSource<FrameType, Stream>::createNew(UsageEnvironment &env, int encChn,
                                                                                  std::shared_ptr<Stream> stream,
                                                                                  const char *name) {
  return new IMPDeviceSource<FrameType, Stream>(env, encChn, stream, name);
}

template <typename FrameType, typename Stream>
IMPDeviceSource<FrameType, Stream>::IMPDeviceSource(UsageEnvironment &env, int encChn, std::shared_ptr<Stream> stream,
                                                    const char *name)
    : FramedSource(env), encChn(encChn), stream{stream}, name{name}, eventTriggerId(0) {
  std::lock_guard lock_stream{mutex_main};
  std::lock_guard lock_callback{stream->onDataCallbackLock};
  
  // Register cursor with StreamCore (replaces msgChannel)
  if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
    cursor = stream->videoCore->registerSubscriber(
        [this]() { this->on_data_available(); },
        StreamStartPolicy::LatestSync);
  } else {
    cursor = stream->audioCore->registerSubscriber(
        [this]() { this->on_data_available(); },
        StreamStartPolicy::LiveEdge);
  }
  
  stream->onDataCallback = [this]() { this->on_data_available(); };
  stream->hasDataCallback = true;

  eventTriggerId = envir().taskScheduler().createEventTrigger(deliverFrame0);
  stream->should_grab_frames.notify_one();
  LOG_DEBUG("IMPDeviceSource " << name << " constructed, encoder channel:" << encChn);
}

template <typename FrameType, typename Stream> void IMPDeviceSource<FrameType, Stream>::deinit() {
  std::lock_guard lock_stream{mutex_main};
  LOG_DEBUG("IMPDeviceSource " << name << " deinit begin, encoder channel:" << encChn
            << " eventTriggerId=" << eventTriggerId
            << " stream_addr=" << reinterpret_cast<uintptr_t>(stream.get()));
  std::lock_guard lock_callback{stream->onDataCallbackLock};
  
  // Unregister cursor from StreamCore
  if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
    stream->videoCore->unregisterSubscriber(cursor);
  } else {
    stream->audioCore->unregisterSubscriber(cursor);
  }
  
  envir().taskScheduler().deleteEventTrigger(eventTriggerId);
  stream->hasDataCallback = false;
  stream->onDataCallback = nullptr;
  LOG_DEBUG("IMPDeviceSource " << name << " deinit complete, encoder channel:" << encChn);
}

template <typename FrameType, typename Stream> IMPDeviceSource<FrameType, Stream>::~IMPDeviceSource() {
  deinit();
}

template <typename FrameType, typename Stream> void IMPDeviceSource<FrameType, Stream>::doGetNextFrame() {
  deliverFrame();
}

template <typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::deliverFrame0(void *clientData) {
  ((IMPDeviceSource<FrameType, Stream> *)clientData)->deliverFrame();
}

template <typename FrameType, typename Stream> void IMPDeviceSource<FrameType, Stream>::deliverFrame() {
  if (!isCurrentlyAwaitingData()) {
    return;
  }

  FrameType nal;
  // Use StreamCore cursor instead of msgChannel
  bool hasFrame;
  if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
    hasFrame = stream->videoCore->read(cursor, &nal);
  } else {
    hasFrame = stream->audioCore->read(cursor, &nal);
  }
  
  while (hasFrame) {
    if (nal.data.size() > fMaxSize) {
      fFrameSize = fMaxSize;
      fNumTruncatedBytes = nal.data.size() - fMaxSize;
    } else {
      fFrameSize = nal.data.size();
    }

    // Use the hardware timestamp from TimestampManager that was captured
    // when the frame was encoded (stored in nal.time by VideoWorker/AudioWorker)
    fPresentationTime = nal.time;
    
    // Monotonicity check - ensure timestamps never go backwards
    int64_t pts_us = tv_to_us(fPresentationTime);
    
    if constexpr (std::is_same_v<FrameType, AudioFrame>) {
      // For audio, also check for reasonable minimum step (1024 samples at 16kHz ~= 64ms)
      if (audioLastPtsUs >= 0 && pts_us <= audioLastPtsUs) {
        LOG_WARN("Audio timestamp went backwards or stalled: %" PRId64 " -> %" PRId64, audioLastPtsUs, pts_us);
        // Force forward progress
        auto *imp_audio = global_audio[encChn]->imp_audio;
        int sampleRate = imp_audio ? imp_audio->sample_rate : 16000;
        if (sampleRate <= 0) sampleRate = 16000;
        int64_t min_audio_step_us = (1024LL * 1000000LL) / sampleRate;
        pts_us = audioLastPtsUs + min_audio_step_us;
        fPresentationTime = us_to_tv(pts_us);
      }
      audioLastPtsUs = pts_us;
    } else {
      // For video, check monotonicity and detect large jumps (session restarts)
      if (videoLastPtsUs >= 0) {
        int64_t delta_pts_us = pts_us - videoLastPtsUs;
        if (delta_pts_us <= 0) {
          LOG_WARN("Video timestamp went backwards or stalled: %" PRId64 " -> %" PRId64, videoLastPtsUs, pts_us);
          // Force forward progress (assume 30fps)
          int64_t min_video_step_us = stream && stream->stream && stream->stream->fps > 0 
            ? 1000000LL / stream->stream->fps : 33333;
          pts_us = videoLastPtsUs + min_video_step_us;
          fPresentationTime = us_to_tv(pts_us);
        } else if (delta_pts_us > 2000000LL) {
          // Large forward jump (>2s) indicates new session or discontinuity
          LOG_DEBUG("Video timestamp jump detected: %" PRId64 "us, re-anchoring", delta_pts_us);
          videoFirstFrame = true;
          videoLastDelta = -1;
        }
      }
      videoLastPtsUs = pts_us;
    }

    memcpy(fTo, &nal.data[0], fFrameSize);

    if (fFrameSize > 0) {
      FramedSource::afterGetting(this);
      return;
    }
    
    // Try to read next frame
    if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
      hasFrame = stream->videoCore->read(cursor, &nal);
    } else {
      hasFrame = stream->audioCore->read(cursor, &nal);
    }
  }
  fFrameSize = 0;
}
