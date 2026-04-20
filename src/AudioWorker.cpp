#include "AudioWorker.hpp"

#include "Config.hpp"
#include "Logger.hpp"
#include "RTSPStatus.hpp"
#include "TimestampManager.hpp"
#include "WorkerUtils.hpp"
#include "globals.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define MODULE "AudioWorker"

static uint64_t pick_audio_anchor_us(uint64_t frame_duration_us) {
  uint64_t anchor_us = frame_duration_us;

  for (int video_ch = 0; video_ch < NUM_VIDEO_CHANNELS; ++video_ch) {
    if (!global_video[video_ch]) {
      continue;
    }
    uint64_t video_ts = global_video[video_ch]->last_timestamp_us.load(std::memory_order_relaxed);
    if (video_ts > anchor_us) {
      anchor_us = video_ts;
    }
  }

  return anchor_us;
}

constexpr uint64_t kAudioStatusPublishIntervalUs = 1000000ULL;

uint64_t monotonic_now_us() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

class AudioTap {
public:
  AudioTap() = default;

  ~AudioTap() {
    shutdown();
  }

  void configure(bool enabled, const std::string &path, int sampleRate, int bitwidth, int channels) {
    if (fifoPath != path) {
      closeWriter();
      fifoReady = false;
    }

    fifoPath = path;
    enabledTap = enabled && !fifoPath.empty();
    sampleRateHz = (sampleRate > 0) ? sampleRate : 16000;
    bitwidthBits = (bitwidth > 0) ? bitwidth : 16;
    channelCount = std::max(1, channels);
    backpressureWarned = false;

    if (!enabledTap) {
      shutdown();
      return;
    }

    if (!fifoReady && !createFifo()) {
      enabledTap = false;
      return;
    }

    LOG_INFO("AudioTap: ready at " << fifoPath << " (" << bitwidthBits << "-bit " << sampleRateHz << " Hz, "
                                   << channelCount << " ch)");
  }

  bool hasActiveReader() const {
    return enabledTap && fd >= 0;
  }

  bool wantsCapture() const {
    return enabledTap;
  }

  void publish(const uint8_t *data, size_t length) {
    if (!enabledTap || !data || length == 0) {
      return;
    }

    if (!fifoReady && !createFifo()) {
      return;
    }

    if (fd < 0 && !openWriter()) {
      return;
    }

    size_t offset = 0;
    while (offset < length) {
      ssize_t written = ::write(fd, data + offset, length - offset);
      if (written > 0) {
        offset += static_cast<size_t>(written);
        backpressureWarned = false;
        continue;
      }

      if (written < 0 && errno == EINTR) {
        continue;
      }

      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (!backpressureWarned) {
          LOG_WARN("AudioTap: FIFO full, dropping PCM samples");
          backpressureWarned = true;
        }
        break;
      }

      if (written < 0 && (errno == EPIPE || errno == ENXIO)) {
        LOG_INFO("AudioTap: reader disconnected");
      } else if (written < 0) {
        LOG_WARN("AudioTap: write failed: " << strerror(errno));
      }

      closeWriter();
      break;
    }
  }

  void shutdown() {
    closeWriter();
    if (fifoReady && !fifoPath.empty()) {
      ::unlink(fifoPath.c_str());
    }
    fifoReady = false;
    backpressureWarned = false;
  }

private:
  bool createFifo() {
    if (fifoPath.empty()) {
      return false;
    }

    auto slash = fifoPath.find_last_of('/');
    if (slash != std::string::npos) {
      std::string dir = fifoPath.substr(0, slash);
      if (!dir.empty()) {
        if (::mkdir(dir.c_str(), 0775) < 0 && errno != EEXIST) {
          LOG_ERROR("AudioTap: mkdir failed for " << dir << ": " << strerror(errno));
          return false;
        }
      }
    }

    if (::unlink(fifoPath.c_str()) < 0 && errno != ENOENT) {
      LOG_WARN("AudioTap: unlink failed for " << fifoPath << ": " << strerror(errno));
    }
    if (::mkfifo(fifoPath.c_str(), 0660) < 0) {
      if (errno != EEXIST) {
        LOG_ERROR("AudioTap: mkfifo failed for " << fifoPath << ": " << strerror(errno));
        return false;
      }
    }

    fifoReady = true;
    return true;
  }

  bool openWriter() {
    fd = ::open(fifoPath.c_str(), O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
      if (errno != ENXIO) {
        LOG_WARN("AudioTap: open failed for " << fifoPath << ": " << strerror(errno));
      }
      return false;
    }
    return true;
  }

  void closeWriter() {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }

  std::string fifoPath;
  bool enabledTap{false};
  bool fifoReady{false};
  int fd{-1};
  int sampleRateHz{0};
  int bitwidthBits{0};
  int channelCount{0};
  bool backpressureWarned{false};
};

AudioWorker::AudioWorker(int chn)
    : encChn(chn), mp4_audio_samples(NUM_VIDEO_CHANNELS, 0), mp4_audio_sample_rate(0),
      tap(std::make_unique<AudioTap>()) {
  LOG_DEBUG("AudioWorker created for channel " << encChn);
}

AudioWorker::~AudioWorker() {
  LOG_DEBUG("AudioWorker destroyed for channel " << encChn);
}

void AudioWorker::process_audio_frame(IMPAudioFrame &frame) {
  AudioFrame af;
  
  // Calculate frame duration
  int sample_size_bytes = frame.bitwidth / 8;
  int channels = (frame.soundmode == AUDIO_SOUND_MODE_MONO) ? 1 : 2;
  if (channels <= 0) {
    channels = global_audio[encChn]->imp_audio ? global_audio[encChn]->imp_audio->outChnCnt : 1;
  }
  if (channels <= 0) {
    channels = 1;
  }
  int64_t frame_samples = 0;
  if (sample_size_bytes > 0) {
    frame_samples = frame.len / (sample_size_bytes * channels);
  }
  
  int sampleRate = global_audio[encChn]->imp_audio ? global_audio[encChn]->imp_audio->sample_rate : 16000;
  if (sampleRate <= 0) sampleRate = 16000;
  
  uint64_t frame_duration_us = frame_samples > 0 
      ? (frame_samples * 1000000ULL) / sampleRate 
      : 64000ULL; // Default ~64ms for AAC
  af.duration_us = frame_duration_us;
  
  // Normalize audio timestamp (similar to Prudynt-SE)
  uint64_t presentation_origin_us =
      global_audio[encChn]->presentation_origin_us.load(std::memory_order_relaxed);
  if (presentation_origin_us == 0) {
    const uint64_t fallback_origin_us = pick_audio_anchor_us(frame_duration_us);
    uint64_t expected = 0;
    if (global_audio[encChn]->presentation_origin_us.compare_exchange_strong(
            expected, fallback_origin_us, std::memory_order_relaxed)) {
      presentation_origin_us = fallback_origin_us;
    } else {
      presentation_origin_us = expected;
    }
  }

  uint64_t last_ts = global_audio[encChn]->last_timestamp_us.load(std::memory_order_relaxed);
  uint64_t ts_us = last_ts;
  if (ts_us == 0) {
    ts_us = presentation_origin_us;
  } else {
    ts_us += frame_duration_us;
  }

  // Monotonicity check
  if (last_ts != 0 && ts_us <= last_ts) {
    ts_us = last_ts + frame_duration_us;
  }
  
  global_audio[encChn]->last_timestamp_us.store(ts_us, std::memory_order_relaxed);

  af.time.tv_sec = ts_us / 1000000ULL;
  af.time.tv_usec = ts_us % 1000000ULL;

  uint8_t *start = (uint8_t *)frame.virAddr;
  uint8_t *end = start + frame.len;

  IMPAudioStream stream;
  bool got_stream = false;
  if (global_audio[encChn]->imp_audio->directEncode) {
    // Direct encoding — bypass IMP_AENC to avoid its heap corruption bug
    int outLen = 0;
    if (IMPAudio::encodeDirect(&frame, directEncBuf.data(), &outLen) == 0 && outLen > 0) {
      start = directEncBuf.data();
      end = start + outLen;
    }
  } else if (global_audio[encChn]->imp_audio->format != IMPAudioFormat::PCM) {
    // IMP_AENC path for built-in codecs (G711A, G711U, G726)
    if (IMP_AENC_SendFrame(global_audio[encChn]->aeChn, &frame) != 0) {
      LOG_ERROR("IMP_AENC_SendFrame(" << global_audio[encChn]->devId << ", " << global_audio[encChn]->aeChn
                                      << ") failed");
    } else if (IMP_AENC_PollingStream(global_audio[encChn]->aeChn, cfg->general.imp_polling_timeout_ms) != 0) {
      LOG_ERROR("IMP_AENC_PollingStream(" << global_audio[encChn]->devId << ", " << global_audio[encChn]->aeChn
                                          << ") failed");
    } else if (IMP_AENC_GetStream(global_audio[encChn]->aeChn, &stream, IMPBlock::BLOCK) != 0) {
      LOG_ERROR("IMP_AENC_GetStream(" << global_audio[encChn]->devId << ", " << global_audio[encChn]->aeChn
                                      << ") failed");
    } else {
      got_stream = true;
      start = (uint8_t *)stream.stream;
      end = start + stream.len;
    }
  }

  if (end > start) {
    af.data.insert(af.data.end(), start, end);
  }

  size_t frame_len = (end > start) ? static_cast<size_t>(end - start) : 0;
  if (frame_samples <= 0) {
    frame_samples = 1024;
  }

  bool any_recorder_active = false;
  for (int ch = 0; ch < NUM_VIDEO_CHANNELS; ++ch) {
    if (static_cast<size_t>(ch) >= mp4_audio_samples.size()) {
      break;
    }
    auto &recorder = global_mp4_recorders[ch];
    bool recorder_active = recorder.isActive();
    if (!recorder_active) {
      mp4_audio_samples[ch] = 0;
      continue;
    }

    any_recorder_active = true;

    if (mp4_audio_sample_rate <= 0) {
      if (global_audio[encChn]->imp_audio) {
        mp4_audio_sample_rate = global_audio[encChn]->imp_audio->sample_rate;
      }
      if (mp4_audio_sample_rate <= 0) {
        mp4_audio_sample_rate = cfg->audio.input_sample_rate;
      }
    }

    int64_t pts_ms = 0;
    if (mp4_audio_sample_rate > 0) {
      pts_ms = (mp4_audio_samples[ch] * 1000) / mp4_audio_sample_rate;
    }

    if (frame_len > 0) {
      recorder.writeAudio(start, frame_len, pts_ms);
    }
    mp4_audio_samples[ch] += frame_samples;
  }

  if (!any_recorder_active) {
    mp4_audio_sample_rate = 0;
  }

  if (global_audio[encChn]->audioCore && !af.data.empty()) {
    const uint64_t now_us = monotonic_now_us();
    if (status_last_publish_us == 0 ||
        (now_us - status_last_publish_us) >= kAudioStatusPublishIntervalUs) {
      const uint64_t samples_per_channel =
          frame_samples > 0 ? static_cast<uint64_t>(frame_samples) : 1024ULL;
      int warn_frames = cfg ? std::max(cfg->audio.buffer_warn_frames, 1) : 1;
      int cap_frames = cfg ? std::max(cfg->audio.buffer_cap_frames, warn_frames + 1) : 2;
      if (cap_frames < warn_frames) {
        cap_frames = warn_frames;
      }

      const uint64_t ring_depth = global_audio[encChn]->audioCore->depth();
      const uint64_t client_count = global_audio[encChn]->audioCore->subscriberCount();
      const uint64_t drop_count = global_audio[encChn]->audioCore->producerDropCount();
      const uint64_t buffer_level = ring_depth * samples_per_channel;
      const uint64_t warn_samples = static_cast<uint64_t>(warn_frames) * samples_per_channel;
      const uint64_t cap_samples = static_cast<uint64_t>(cap_frames) * samples_per_channel;

      const std::string stream_name = "audio" + std::to_string(encChn);
      RTSPStatus::writeCustomParameter(stream_name, "buffer_warn_samples_per_channel",
                                       std::to_string(warn_samples));
      RTSPStatus::writeCustomParameter(stream_name, "buffer_cap_samples_per_channel",
                                       std::to_string(cap_samples));
      RTSPStatus::writeCustomParameter(stream_name, "buffer_level_samples_per_channel",
                                       std::to_string(buffer_level));
      RTSPStatus::writeCustomParameter(stream_name, "buffer_drop_count",
                                       std::to_string(drop_count));
      RTSPStatus::writeCustomParameter(stream_name, "ring_depth",
                                       std::to_string(ring_depth));
      RTSPStatus::writeCustomParameter(stream_name, "client_count",
                                       std::to_string(client_count));
      RTSPStatus::writeCustomParameter(stream_name, "drop_count",
                                       std::to_string(drop_count));
      status_last_publish_us = now_us;
    }
  }

  if (!af.data.empty() && global_audio[encChn]->hasDataCallback) {
    // Publish to StreamCore (replaces msgChannel + tap mechanism)
    try {
      global_audio[encChn]->audioCore->publish(af);
      
      // Notify live555 callback if registered
      std::unique_lock<std::mutex> lock_stream{global_audio[encChn]->onDataCallbackLock};
      if (global_audio[encChn]->onDataCallback)
        global_audio[encChn]->onDataCallback();
    } catch (const std::exception& e) {
      LOG_ERROR("audio encChn:" << encChn << " - Failed to publish: " << e.what());
    }
  }

  if (got_stream &&
      IMP_AENC_ReleaseStream(global_audio[encChn]->aeChn, &stream) < 0) {
    LOG_ERROR("IMP_AENC_ReleaseStream(" << global_audio[encChn]->devId << ", " << global_audio[encChn]->aeChn
                                        << ", &stream) failed");
  }
}

void AudioWorker::publishTapFrame(const IMPAudioFrame &frame) {
  if (!tap || frame.virAddr == nullptr || frame.len <= 0) {
    return;
  }

  tap->publish(reinterpret_cast<uint8_t *>(frame.virAddr), static_cast<size_t>(frame.len));
}

void AudioWorker::process_frame(IMPAudioFrame &frame) {
  if (global_audio[encChn]->imp_audio->outChnCnt == 2 && frame.soundmode == AUDIO_SOUND_MODE_MONO) {
    size_t sample_size = frame.bitwidth / 8;
    size_t num_samples = frame.len / sample_size;
    size_t stereo_size = frame.len * 2;
    uint8_t *stereo_buffer = new uint8_t[stereo_size];

    for (size_t i = 0; i < num_samples; i++) {
      uint8_t *mono_sample = ((uint8_t *)frame.virAddr) + (i * sample_size);
      uint8_t *stereo_left = stereo_buffer + (i * sample_size * 2);
      uint8_t *stereo_right = stereo_left + sample_size;
      memcpy(stereo_left, mono_sample, sample_size);
      memcpy(stereo_right, mono_sample, sample_size);
    }

    IMPAudioFrame stereo_frame = frame;
    stereo_frame.virAddr = (uint32_t *)stereo_buffer;
    stereo_frame.len = stereo_size;
    stereo_frame.soundmode = AUDIO_SOUND_MODE_STEREO;

    publishTapFrame(stereo_frame);
    process_audio_frame(stereo_frame);
    delete[] stereo_buffer;
  } else {
    publishTapFrame(frame);
    process_audio_frame(frame);
  }
}

void AudioWorker::run() {
  LOG_DEBUG("Start audio processing run loop for channel " << encChn);

  if (tap && cfg) {
    const char *path = cfg->audio.tap_path ? cfg->audio.tap_path : "";
    int sampleRate = cfg->audio.input_sample_rate;
    int bitwidth = 16; // Prudynt captures 16-bit PCM frames
    int channels = 1;
    if (global_audio[encChn]->imp_audio) {
      if (global_audio[encChn]->imp_audio->sample_rate > 0) {
        sampleRate = global_audio[encChn]->imp_audio->sample_rate;
      }
      if (global_audio[encChn]->imp_audio->outChnCnt > 0) {
        channels = global_audio[encChn]->imp_audio->outChnCnt;
      }
    }
    tap->configure(cfg->audio.tap_enabled && cfg->audio.input_enabled, path, sampleRate, bitwidth, channels);
  }

  // Initialize AudioReframer only if needed, store in member variable
  if (global_audio[encChn]->imp_audio->format == IMPAudioFormat::AAC) {
    reframer = std::make_unique<AudioReframer>(global_audio[encChn]->imp_audio->sample_rate,
                                               /* inputSamplesPerFrame */
                                               global_audio[encChn]->imp_audio->sample_rate * 0.040,
                                               /* outputSamplesPerFrame */ 1024);
    LOG_DEBUG("AudioReframer created for channel " << encChn);
  } else {
    LOG_DEBUG("AudioReframer not needed or imp_audio not ready for channel " << encChn);
  }

  // Pre-allocate output buffer for direct encoding (AAC/OPUS bypass IMP_AENC)
  if (global_audio[encChn]->imp_audio->directEncode) {
    directEncBuf.resize(8192);
    LOG_DEBUG("Direct encode buffer allocated (8192 bytes) for channel " << encChn);
  }

  auto reset_timeline_state = [&]() {
    global_audio[encChn]->last_timestamp_us.store(0, std::memory_order_relaxed);
    global_audio[encChn]->timestamp_origin_raw.store(0, std::memory_order_relaxed);
    global_audio[encChn]->presentation_origin_us.store(0, std::memory_order_relaxed);

    if (global_audio[encChn]->imp_audio &&
        global_audio[encChn]->imp_audio->format == IMPAudioFormat::AAC) {
      reframer = std::make_unique<AudioReframer>(global_audio[encChn]->imp_audio->sample_rate,
                                                 /* inputSamplesPerFrame */
                                                 global_audio[encChn]->imp_audio->sample_rate * 0.040,
                                                 /* outputSamplesPerFrame */ 1024);
    }
  };
  bool had_audio_clients = false;

  while (global_audio[encChn]->running) {
    bool recorder_needs_audio = (global_mp4_active_recorders.load(std::memory_order_relaxed) > 0);
    bool audio_clients_active = global_audio[encChn]->hasDataCallback.load(std::memory_order_relaxed);
    if (audio_clients_active && !had_audio_clients) {
      reset_timeline_state();
    }
    had_audio_clients = audio_clients_active;
    bool tap_requests_audio = tap && tap->wantsCapture();
    // Keep AI/AENC running for active consumers even when mic is logically
    // "disabled", so runtime toggles can be implemented as mute/unmute without
    // breaking AAC framing/timestamps in existing RTSP sessions.
    bool should_capture_audio = (audio_clients_active || recorder_needs_audio || tap_requests_audio);

    if (should_capture_audio) {
      if (IMP_AI_PollingFrame(global_audio[encChn]->devId, global_audio[encChn]->aiChn,
                              cfg->general.imp_polling_timeout_ms) == 0) {
        IMPAudioFrame frame;
        if (IMP_AI_GetFrame(global_audio[encChn]->devId, global_audio[encChn]->aiChn, &frame, IMPBlock::BLOCK) != 0) {
          LOG_ERROR("IMP_AI_GetFrame(" << global_audio[encChn]->devId << ", " << global_audio[encChn]->aiChn
                                       << ") failed");
          continue; // avoid using an uninitialized frame
        }

        if (reframer) {
          reframer->addFrame(reinterpret_cast<uint8_t *>(frame.virAddr), frame.timeStamp);
          while (reframer->hasMoreFrames()) {
            size_t frameLen = 1024 * sizeof(uint16_t) * global_audio[encChn]->imp_audio->outChnCnt;
            std::vector<uint8_t> frameData(frameLen, 0);
            int64_t audio_ts_us;
            reframer->getReframedFrame(frameData.data(), audio_ts_us);
            IMPAudioFrame reframed = {.bitwidth = frame.bitwidth,
                                      .soundmode = frame.soundmode,
                                      .virAddr = reinterpret_cast<uint32_t *>(frameData.data()),
                                      .phyAddr = frame.phyAddr,
                                      .timeStamp = audio_ts_us,
                                      .seq = frame.seq,
                                      .len = static_cast<int>(frameLen)};
            process_frame(reframed);
          }
        } else {
          process_frame(frame);
        }

        if (IMP_AI_ReleaseFrame(global_audio[encChn]->devId, global_audio[encChn]->aiChn, &frame) < 0) {
          LOG_ERROR("IMP_AI_ReleaseFrame(" << global_audio[encChn]->devId << ", " << global_audio[encChn]->aiChn
                                           << ", &frame) failed");
        }
      } else {
        LOG_DEBUG(global_audio[encChn]->devId << ", " << global_audio[encChn]->aiChn << " POLLING TIMEOUT");
      }
    } else if (!global_restart) {
      std::unique_lock<std::mutex> lock_stream{mutex_main};
      global_audio[encChn]->active = false;
      LOG_DDEBUG("AUDIO LOCK");

      /* Since the audio stream is permanently in use by the stream replicator,
       * we send the audio grabber and encoder to standby when no video is
       * requested.
       */
      while (!global_restart_audio) {
        bool recorder_needed_now = (global_mp4_active_recorders.load(std::memory_order_relaxed) > 0);
        bool audio_clients_active_now = global_audio[encChn]->hasDataCallback.load(std::memory_order_relaxed);
        bool video_clients_active_now = global_video[0]->hasDataCallback.load(std::memory_order_relaxed) ||
                                        global_video[1]->hasDataCallback.load(std::memory_order_relaxed) ||
                                        global_force_video_active.load(std::memory_order_relaxed) ||
                                        recorder_needed_now;
        // Resume if audio clients are active (audio-only streams like /mic)
        if (audio_clients_active_now) {
          break;
        }
        // Resume if video clients need audio
        if (global_audio[encChn]->onDataCallback != nullptr && video_clients_active_now) {
          break;
        }
        if (recorder_needed_now) {
          break;
        }
        global_audio[encChn]->should_grab_frames.wait(lock_stream);
      }
      global_audio[encChn]->active = true;
      LOG_DDEBUG("AUDIO UNLOCK");
    } else {
      /* to prevent clogging on startup or while restarting the threads
       * we wait for 250ms
       */
      usleep(250 * 1000);
    }
  }
}

void *AudioWorker::thread_entry(void *arg) {
  StartHelper *sh = static_cast<StartHelper *>(arg);
  int encChn = sh->encChn;
  bool start_signal_sent = false;

  LOG_DEBUG("Start audio_grabber thread for device " << global_audio[encChn]->devId << " and channel "
                                                     << global_audio[encChn]->aiChn << " and encoder "
                                                     << global_audio[encChn]->aeChn);

  try {
    global_audio[encChn]->imp_audio =
        IMPAudio::createNew(global_audio[encChn]->devId, global_audio[encChn]->aiChn, global_audio[encChn]->aeChn);

    // inform main that initialization is complete
    sh->has_started.release();
    start_signal_sent = true;

    /* 'active' indicates, the thread is activly polling and grabbing images
     * 'running' describes the runlevel of the thread, if this value is set to
     * false the thread exits and cleanup all ressources
     */
    global_audio[encChn]->active = true;
    global_audio[encChn]->running = true;

    AudioWorker worker(encChn);
    worker.run();
  } catch (const std::exception &e) {
    LOG_ERROR("AudioWorker init/run failed: " << e.what());
  } catch (...) {
    LOG_ERROR("AudioWorker init/run failed with unknown exception");
  }

  if (!start_signal_sent) {
    sh->has_started.release();
  }

  global_audio[encChn]->running = false;
  global_audio[encChn]->active = false;

  if (global_audio[encChn]->imp_audio) {
    delete global_audio[encChn]->imp_audio;
    global_audio[encChn]->imp_audio = nullptr;
  }

  return 0;
}
