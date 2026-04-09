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
  while (stream->msgChannel->read(&nal)) {
    if (nal.data.size() > fMaxSize) {
      fFrameSize = fMaxSize;
      fNumTruncatedBytes = nal.data.size() - fMaxSize;
    } else {
      fFrameSize = nal.data.size();
    }

    /* Use monotonic counter-based timestamps for AAC to produce perfectly
       steady +1024 RTP PTS increments and avoid gettimeofday jitter. */
    if constexpr (std::is_same_v<FrameType, AudioFrame>) {
      auto *imp_audio = global_audio[encChn]->imp_audio;
      int sampleRate = 16000;
      if (cfg && cfg->audio.input_sample_rate > 0) {
        sampleRate = cfg->audio.input_sample_rate;
      }
      if (imp_audio) {
        if (imp_audio->sample_rate > 0) {
          sampleRate = imp_audio->sample_rate;
        }
      }

      // Always use current wallclock. This keeps fPresentationTime in the
      // same clock domain as live555's presetNextTimestamp(), so NTP steps
      // and stale ring-buffer frames never cause RTP timestamp jumps.
      gettimeofday(&fPresentationTime, NULL);

      int64_t pts_us = tv_to_us(fPresentationTime);
      int rate_for_tick = sampleRate > 0 ? sampleRate : 16000;
      // AAC: 1024 samples/frame; non-AAC: ~40ms frames
      int64_t min_audio_step_us = (1024LL * 1000000LL + rate_for_tick - 1) / rate_for_tick;
      if (min_audio_step_us < 1) {
        min_audio_step_us = 1;
      }
      if (audioLastPtsUs >= 0) {
        int64_t delta_pts_us = pts_us - audioLastPtsUs;
        if (delta_pts_us <= 0) {
          pts_us = audioLastPtsUs + min_audio_step_us;
          fPresentationTime = us_to_tv(pts_us);
        }
      }
      audioLastPtsUs = pts_us;
    } else {
      // Always use current wallclock for video too.
      gettimeofday(&fPresentationTime, NULL);

      int64_t pts_us = tv_to_us(fPresentationTime);
      int64_t min_video_step_us = 12;
      if (stream && stream->stream && stream->stream->fps > 0) {
        min_video_step_us = 1000000LL / stream->stream->fps;
        if (min_video_step_us < 12) {
          min_video_step_us = 12;
        }
      }
      if (videoLastPtsUs >= 0) {
        int64_t delta_pts_us = pts_us - videoLastPtsUs;
        if (delta_pts_us <= 0) {
          pts_us = videoLastPtsUs + min_video_step_us;
          fPresentationTime = us_to_tv(pts_us);
        } else if (delta_pts_us > 2000000LL) {
          // Large forward jump (new RTSP session or discontinuity) — re-anchor
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
  }
  fFrameSize = 0;
}
