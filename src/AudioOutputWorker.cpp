#include "AudioOutputWorker.hpp"

#include "Config.hpp"
#include "IMPAudioOutput.hpp"
#include "Logger.hpp"

#include <thread>

#define MODULE "AudioOutputWorker"

void *AudioOutputWorker::thread_entry(void * /*arg*/) {
  AudioOutputWorker worker;
  worker.run();
  return nullptr;
}

bool AudioOutputWorker::enqueuePcm(std::vector<int16_t> &&samples, bool applyVolume, int volume, bool applyGain,
                                   int gain, bool applyMute, bool mute) {
  if (!global_audio_output || !global_audio_output->jobQueue) {
    LOG_ERROR("Audio output queue is not initialized");
    return false;
  }

  AudioPlaybackJob job;
  job.type = AudioPlaybackJobType::PCM;
  job.samples = std::move(samples);
  job.hasVolume = applyVolume;
  job.volume = volume;
  job.hasGain = applyGain;
  job.gain = gain;
  job.hasMute = applyMute;
  job.mute = mute;

  bool enqueued = global_audio_output->jobQueue->write(std::move(job));
  if (!enqueued) {
    LOG_WARN("Audio output queue full; dropped oldest PCM chunk to enqueue new "
             "data");
  }

  return true;
}

bool AudioOutputWorker::enqueuePcmBlocking(std::vector<int16_t> &&samples, bool applyVolume, int volume, bool applyGain,
                                           int gain, bool applyMute, bool mute) {
  if (!global_audio_output || !global_audio_output->jobQueue) {
    LOG_ERROR("Audio output queue is not initialized");
    return false;
  }

  AudioPlaybackJob job;
  job.type = AudioPlaybackJobType::PCM;
  job.samples = std::move(samples);
  job.hasVolume = applyVolume;
  job.volume = volume;
  job.hasGain = applyGain;
  job.gain = gain;
  job.hasMute = applyMute;
  job.mute = mute;

  global_audio_output->jobQueue->write_wait(std::move(job));
  return true;
}

bool AudioOutputWorker::applyVolumeGain(bool applyVolume, int volume, bool applyGain, int gain) {
  if (!applyVolume && !applyGain) {
    return true;
  }

  if (!global_audio_output) {
    LOG_DEBUG("Audio output stream not available for volume/gain update");
    return false;
  }

  std::lock_guard<std::mutex> lock(global_audio_output->control_mutex);
  if (!global_audio_output->imp_audio_output) {
    LOG_DEBUG("IMP audio output not ready; deferring volume/gain change");
    return false;
  }

  if (applyVolume) {
    global_audio_output->imp_audio_output->setVolume(volume);
    global_audio_output->current_volume = volume;
  }
  if (applyGain) {
    global_audio_output->imp_audio_output->setGain(gain);
    global_audio_output->current_gain = gain;
  }

  return true;
}

bool AudioOutputWorker::applyMute(bool mute) {
  if (!global_audio_output) {
    LOG_DEBUG("Audio output stream not available for mute update");
    return false;
  }

  std::lock_guard<std::mutex> lock(global_audio_output->control_mutex);
  if (!global_audio_output->imp_audio_output) {
    LOG_DEBUG("IMP audio output not ready; deferring mute change");
    return false;
  }

  if (!global_audio_output->imp_audio_output->setMute(mute)) {
    return false;
  }

  global_audio_output->current_mute = mute;
  return true;
}

bool AudioOutputWorker::clearQueue(bool waitForFlush) {
  if (!global_audio_output || !global_audio_output->jobQueue) {
    return false;
  }

  global_audio_output->jobQueue->clear();

  AudioPlaybackJob job;
  job.type = AudioPlaybackJobType::CLEAR;
  std::shared_ptr<std::promise<void>> completion;
  if (waitForFlush) {
    completion = std::make_shared<std::promise<void>>();
    job.completion = completion;
  }

  bool enqueued = global_audio_output->jobQueue->write(std::move(job));
  if (!enqueued) {
    LOG_WARN("Audio output queue full while enqueuing clear command; dropped "
             "oldest chunk");
  }

  if (waitForFlush && completion) {
    completion->get_future().wait();
  }

  return true;
}

bool AudioOutputWorker::waitForPlaybackCompletion(std::chrono::milliseconds waitDuration, bool flushAfterWait,
                                                  std::chrono::milliseconds silencePadding) {
  if (!global_audio_output || !global_audio_output->jobQueue) {
    return false;
  }

  AudioPlaybackJob job;
  job.type = AudioPlaybackJobType::WAIT;
  job.wait_ms = static_cast<int>(waitDuration.count());
  job.flush_after_wait = flushAfterWait;
  job.silence_ms = static_cast<int>(silencePadding.count());
  auto completion = std::make_shared<std::promise<void>>();
  job.completion = completion;

  global_audio_output->jobQueue->write_wait(std::move(job));
  completion->get_future().wait();
  return true;
}

void AudioOutputWorker::signalShutdown() {
  if (!global_audio_output || !global_audio_output->jobQueue) {
    return;
  }

  clearQueue(true);

  AudioPlaybackJob job;
  job.type = AudioPlaybackJobType::STOP;

  bool enqueued = global_audio_output->jobQueue->write(std::move(job));
  if (!enqueued) {
    LOG_WARN("Audio output queue was full while enqueuing shutdown sentinel");
  }
}

void AudioOutputWorker::run() {
  if (!global_audio_output) {
    LOG_ERROR("Audio output stream not initialized");
    return;
  }

  global_audio_output->running = true;
  global_audio_output->current_volume = cfg->audio.output_vol;
  global_audio_output->current_gain = cfg->audio.output_gain;

  global_audio_output->imp_audio_output = std::make_unique<IMPAudioOutput>(0, 0);
  if (!global_audio_output->imp_audio_output->init()) {
    LOG_ERROR("Failed to initialize IMP audio output");
    global_audio_output->imp_audio_output.reset();
    global_audio_output->running = false;
    return;
  }

  while (global_audio_output->running) {
    AudioPlaybackJob job = global_audio_output->jobQueue->wait_read();
    if (job.type == AudioPlaybackJobType::STOP) {
      if (global_audio_output->imp_audio_output) {
        global_audio_output->imp_audio_output->flush();
      }
      break;
    }
    if (job.type == AudioPlaybackJobType::CLEAR) {
      if (global_audio_output->imp_audio_output) {
        global_audio_output->imp_audio_output->flush();
      }
      if (job.completion) {
        job.completion->set_value();
      }
      continue;
    }
    if (job.type == AudioPlaybackJobType::WAIT) {
      if (job.wait_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(job.wait_ms));
      }
      if (job.silence_ms > 0 && global_audio_output->imp_audio_output) {
        if (!global_audio_output->imp_audio_output->playSilence(job.silence_ms)) {
          LOG_WARN("AudioOutputWorker: failed to inject tail silence");
        }
      }
      if (job.flush_after_wait && global_audio_output->imp_audio_output) {
        global_audio_output->imp_audio_output->flush();
      }
      if (job.completion) {
        job.completion->set_value();
      }
      continue;
    }

    if (job.hasVolume || job.hasGain || job.hasMute) {
      std::lock_guard<std::mutex> lock(global_audio_output->control_mutex);
      if (job.hasVolume) {
        global_audio_output->imp_audio_output->setVolume(job.volume);
        global_audio_output->current_volume = job.volume;
      }
      if (job.hasGain) {
        global_audio_output->imp_audio_output->setGain(job.gain);
        global_audio_output->current_gain = job.gain;
      }
      if (job.hasMute) {
        if (!global_audio_output->imp_audio_output->setMute(job.mute)) {
          LOG_WARN("AudioOutputWorker: failed to set mute=" << job.mute);
        } else {
          global_audio_output->current_mute = job.mute;
        }
      }
    }

    if (!job.samples.empty()) {
      if (!global_audio_output->imp_audio_output->playSamples(job.samples.data(), job.samples.size())) {
        LOG_WARN("Failed to play PCM chunk (" << job.samples.size() << " samples)");
      }
    }
  }

  global_audio_output->imp_audio_output.reset();
  global_audio_output->running = false;
}
