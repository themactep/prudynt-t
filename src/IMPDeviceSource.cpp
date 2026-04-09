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
uint64_t IMPDeviceSource<FrameType, Stream>::normalizePresentationTimeUs(uint64_t sourceFrameUs,
                                                                          uint64_t durationUs) {
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
        presentationAnchorUs =
            sourceFrameUs > fallbackDurationUs ? sourceFrameUs - fallbackDurationUs : 0;
      } else {
        // AAC demuxers commonly derive stream start_time as first_pts - frame_duration.
        // Anchor audio one frame earlier so the first emitted packet lands at +duration,
        // which yields a clean zero start_time instead of -0.064 on 16 kHz AAC.
        presentationAnchorUs =
            sourceFrameUs > fallbackDurationUs ? sourceFrameUs - fallbackDurationUs : 0;
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

    // Timestamps are already normalized by VideoWorker/AudioWorker, but we need to
    // normalize AGAIN in IMPDeviceSource (like Prudynt-SE does) to establish a fresh
    // anchor point per RTSP session/subscriber
    uint64_t source_frame_us = static_cast<uint64_t>(nal.time.tv_sec) * 1000000ULL +
                               static_cast<uint64_t>(nal.time.tv_usec);
    
    uint64_t duration_us = 0;
    if (lastSourceFrameUs != 0 && source_frame_us > lastSourceFrameUs) {
      duration_us = source_frame_us - lastSourceFrameUs;
    } else if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
      const int fps = (stream && stream->stream && stream->stream->fps > 0)
          ? stream->stream->fps : 25;
      duration_us = 1000000ULL / static_cast<uint64_t>(fps);
    } else {
      // AAC: 1024 samples at 16kHz = 64ms
      auto *imp_audio = global_audio[encChn]->imp_audio;
      int sampleRate = imp_audio ? imp_audio->sample_rate : 16000;
      if (sampleRate <= 0) sampleRate = 16000;
      duration_us = (1024ULL * 1000000ULL) / static_cast<uint64_t>(sampleRate);
    }
    
    const uint64_t presentation_us = normalizePresentationTimeUs(source_frame_us, duration_us);
    fPresentationTime = us_to_tv(presentation_us);
    fDurationInMicroseconds = duration_us;

    // Debug: Log first few timestamps
    static int log_count = 0;
    if (log_count < 5) {
      if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
        LOG_DEBUG("Video: source=" << source_frame_us << " anchor=" << presentationAnchorUs 
                  << " presentation=" << presentation_us << " duration=" << duration_us);
      } else {
        LOG_DEBUG("Audio: source=" << source_frame_us << " anchor=" << presentationAnchorUs 
                  << " presentation=" << presentation_us << " duration=" << duration_us);
      }
      log_count++;
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
