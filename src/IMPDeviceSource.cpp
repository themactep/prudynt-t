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
  std::lock_guard lock_callback{stream->onDataCallbackLock};
  envir().taskScheduler().deleteEventTrigger(eventTriggerId);
  stream->hasDataCallback = false;
  stream->onDataCallback = nullptr;
  LOG_DEBUG("IMPDeviceSource " << name << " destructed, encoder channel:" << encChn);
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
      bool use_aac_clock = false;
      int sampleRate = 16000;
      if (cfg && cfg->audio.input_sample_rate > 0) {
        sampleRate = cfg->audio.input_sample_rate;
      }
      if (imp_audio) {
        use_aac_clock = (imp_audio->format == IMPAudioFormat::AAC);
        if (imp_audio->sample_rate > 0) {
          sampleRate = imp_audio->sample_rate;
        }
      } else if (cfg && cfg->audio.input_format && std::strcmp(cfg->audio.input_format, "AAC") == 0) {
        // During transient audio restarts, keep AAC timing behavior stable.
        use_aac_clock = true;
      }

      bool has_audio_time = is_plausible_wallclock_tv(nal.time);
      if (has_audio_time) {
        fPresentationTime = nal.time;
      } else if (use_aac_clock) {
        if (audioFirstFrame) {
          gettimeofday(&audioStartTime, NULL);
          audioFrameCount = 0;
          audioClockSampleRate = sampleRate;
          audioFirstFrame = false;
        } else if (audioClockSampleRate <= 0 && sampleRate > 0) {
          audioClockSampleRate = sampleRate;
        }
        int effectiveSampleRate = (audioClockSampleRate > 0) ? audioClockSampleRate : sampleRate;
        if (effectiveSampleRate <= 0) {
          effectiveSampleRate = 16000;
        }
        constexpr uint64_t kSamplesPerFrame = 1024;
        uint64_t usec_offset =
            (audioFrameCount * kSamplesPerFrame * 1000000ULL) / static_cast<uint64_t>(effectiveSampleRate);
        fPresentationTime.tv_sec  = audioStartTime.tv_sec  + static_cast<time_t>(usec_offset / 1000000ULL);
        fPresentationTime.tv_usec = audioStartTime.tv_usec + static_cast<suseconds_t>(usec_offset % 1000000ULL);
        if (fPresentationTime.tv_usec >= 1000000) {
          fPresentationTime.tv_sec++;
          fPresentationTime.tv_usec -= 1000000;
        }
        audioFrameCount++;
      } else {
        gettimeofday(&fPresentationTime, NULL);
      }

      int64_t pts_us = tv_to_us(fPresentationTime);
      int rate_for_tick = sampleRate > 0 ? sampleRate : 16000;
      int64_t min_audio_step_us;
      if (use_aac_clock) {
        min_audio_step_us = (1024LL * 1000000LL + rate_for_tick - 1) / rate_for_tick;
      } else {
        // Non-AAC (Opus, PCM, etc.): use actual frame duration.
        // IMP audio uses 40ms frames (e.g., Opus at 48kHz = 1920 samples/frame).
        int samples_per_frame = static_cast<int>(rate_for_tick * 0.040);
        min_audio_step_us = (static_cast<int64_t>(samples_per_frame) * 1000000LL + rate_for_tick - 1) / rate_for_tick;
      }
      if (min_audio_step_us < 1) {
        min_audio_step_us = 1;
      }
      if (audioLastPtsUs >= 0) {
        int64_t delta_pts_us = pts_us - audioLastPtsUs;
        int64_t max_forward_jump_us = 1000000LL;
        if (use_aac_clock && min_audio_step_us > 0) {
          int64_t aac_jump_limit = min_audio_step_us * 120;
          if (aac_jump_limit > max_forward_jump_us) {
            max_forward_jump_us = aac_jump_limit;
          }
        }
        if (delta_pts_us <= 0) {
          pts_us = audioLastPtsUs + min_audio_step_us;
          fPresentationTime = us_to_tv(pts_us);
        } else if (delta_pts_us > max_forward_jump_us) {
          // Large forward jump (new RTSP session or discontinuity) — re-anchor
        }
      }
      audioLastPtsUs = pts_us;
    } else {
      // All NALs in the same video frame (access unit) MUST share one RTP timestamp.
      // Reuse the presentation time computed for the first NAL of this frame.
      if (nal.frame_id != 0 && nal.frame_id == videoLastFrameId) {
        fPresentationTime = videoFramePresentationTime;
      } else {
        bool vid_plausible = is_plausible_wallclock_tv(nal.time);
        if (vid_plausible) {
          fPresentationTime = nal.time;
        } else {
          // Video fallback path for sources that don't provide explicit timeval.
          int64_t nominal_step_us = 33333;
          if (stream && stream->stream && stream->stream->fps > 0) {
            nominal_step_us = 1000000LL / stream->stream->fps;
          }
          if (nominal_step_us < 12) {
            nominal_step_us = 12;
          }
          if (videoFirstFrame) {
            gettimeofday(&videoBaseTime, NULL);
            videoFirstImpTs = nal.imp_ts;
            videoFirstFrame = false;
          }

          int64_t delta_us = nal.imp_ts - videoFirstImpTs;

          bool needs_reanchor = delta_us < 0;
          if (!needs_reanchor && videoLastDelta >= 0) {
            int64_t step = delta_us - videoLastDelta;
            needs_reanchor = (step > 2000000LL) || (step < -500000LL);
          }
          if (needs_reanchor) {
            int64_t target_delta = (videoLastDelta >= 0) ? (videoLastDelta + nominal_step_us) : 0;
            videoFirstImpTs = nal.imp_ts - target_delta;
            delta_us = target_delta;
          } else if (videoLastDelta >= 0 && delta_us <= videoLastDelta) {
            delta_us = videoLastDelta + nominal_step_us;
          }
          videoLastDelta = delta_us;
          fPresentationTime.tv_sec  = videoBaseTime.tv_sec  + static_cast<time_t>(delta_us / 1000000LL);
          fPresentationTime.tv_usec = videoBaseTime.tv_usec + static_cast<suseconds_t>(delta_us % 1000000LL);
          if (fPresentationTime.tv_usec >= 1000000) {
            fPresentationTime.tv_sec++;
            fPresentationTime.tv_usec -= 1000000;
          }
        }

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

        videoLastFrameId = nal.frame_id;
        videoFramePresentationTime = fPresentationTime;
      }
    }

    memcpy(fTo, &nal.data[0], fFrameSize);

    if (fFrameSize > 0) {
      FramedSource::afterGetting(this);
      return;
    }
  }
  fFrameSize = 0;
}
