#include "IMPDeviceSource.hpp"
#include "GroupsockHelper.hh"
#include <iostream>
#include <type_traits>

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
  if (stream->msgChannel->read(&nal)) {
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
      if (imp_audio && imp_audio->format == IMPAudioFormat::AAC) {
        if (audioFirstFrame) {
          gettimeofday(&audioStartTime, NULL);
          audioFrameCount = 0;
          audioFirstFrame = false;
        }
        int sampleRate = (imp_audio->sample_rate > 0) ? imp_audio->sample_rate : 16000;
        constexpr uint64_t kSamplesPerFrame = 1024;
        uint64_t usec_offset = (audioFrameCount * kSamplesPerFrame * 1000000ULL) / static_cast<uint64_t>(sampleRate);
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
    } else {
      // Video: use encoder timestamps for stable, jitter-free PTS.
      // The IMP encoder provides a monotonic microsecond timestamp per NAL.
      // We anchor it to wall-clock at the first frame, then derive all
      // subsequent presentation times from the encoder's own clock.
      if (videoFirstFrame) {
        gettimeofday(&videoBaseTime, NULL);
        videoFirstImpTs = nal.imp_ts;
        videoFirstFrame = false;
      }
      int64_t delta_us = nal.imp_ts - videoFirstImpTs;
      // Guard against timestamp wrap/reset (negative delta) or
      // impossible forward jumps (> 2s in one frame = encoder restart).
      if (delta_us < 0 || (delta_us > 0 && videoLastDelta >= 0 &&
          (delta_us - videoLastDelta) > 2000000LL)) {
        gettimeofday(&videoBaseTime, NULL);
        videoFirstImpTs = nal.imp_ts;
        delta_us = 0;
      }
      videoLastDelta = delta_us;
      fPresentationTime.tv_sec  = videoBaseTime.tv_sec  + static_cast<time_t>(delta_us / 1000000LL);
      fPresentationTime.tv_usec = videoBaseTime.tv_usec + static_cast<suseconds_t>(delta_us % 1000000LL);
      if (fPresentationTime.tv_usec >= 1000000) {
        fPresentationTime.tv_sec++;
        fPresentationTime.tv_usec -= 1000000;
      }
    }

    memcpy(fTo, &nal.data[0], fFrameSize);

    if (fFrameSize > 0) {
      FramedSource::afterGetting(this);
    }
  } else {
    fFrameSize = 0;
  }
}
