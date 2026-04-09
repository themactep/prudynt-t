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

    // Timestamps are already normalized by VideoWorker/AudioWorker
    // Just use them directly (like Prudynt-SE does)
    fPresentationTime = nal.time;
    
    // Calculate duration from timestamp delta or use defaults
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
    
    fDurationInMicroseconds = duration_us;
    lastSourceFrameUs = source_frame_us;

    // Debug: Log first few timestamps
    static int log_count = 0;
    if (log_count < 5) {
      if constexpr (std::is_same_v<FrameType, H264NALUnit>) {
        LOG_DEBUG("Video PTS: " << fPresentationTime.tv_sec << "." << fPresentationTime.tv_usec 
                  << " duration=" << duration_us << "us");
      } else {
        LOG_DEBUG("Audio PTS: " << fPresentationTime.tv_sec << "." << fPresentationTime.tv_usec 
                  << " duration=" << duration_us << "us");
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
