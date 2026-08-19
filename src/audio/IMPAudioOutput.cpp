#include "audio/IMPAudioOutput.hpp"

#include "config/Config.hpp"
#include "util/Logger.hpp"

#include <algorithm>
#include <cstdint>
#include <imp/imp_audio.h>
#include <vector>

#define MODULE "IMPAudioOutput"

namespace {
constexpr int kFrameDurationMs = 20;

auto toImpSampleRate(int sampleRate) {
  switch (sampleRate) {
  case 8000:
    return AUDIO_SAMPLE_RATE_8000;
  case 16000:
    return AUDIO_SAMPLE_RATE_16000;
  case 24000:
    return AUDIO_SAMPLE_RATE_24000;
#ifdef AUDIO_SAMPLE_RATE_32000
  case 32000:
    return AUDIO_SAMPLE_RATE_32000;
#endif
  case 44100:
    return AUDIO_SAMPLE_RATE_44100;
  case 48000:
    return AUDIO_SAMPLE_RATE_48000;
  default:
    LOG_WARN("Unsupported AO sample rate " << sampleRate
                                           << ", falling back to 16000Hz");
    return AUDIO_SAMPLE_RATE_16000;
  }
}
} // namespace

IMPAudioOutput::IMPAudioOutput(int devId_, int channelId_)
    : initialized(false), devId(devId_), channelId(channelId_),
      maxFrameBytes(0), currentVolume(0), currentGain(0), currentMute(false),
      configuredSampleRate(0) {
}

IMPAudioOutput::~IMPAudioOutput() {
  deinit();
}

bool IMPAudioOutput::init() {
  if (initialized) {
    return true;
  }

  if (!configureHardware()) {
    return false;
  }

  currentVolume = cfg->audio.output_vol;
  currentGain = cfg->audio.output_gain;

  setVolume(currentVolume);
  setGain(currentGain);
  setMute(currentMute);

  initialized = true;
  return true;
}

void IMPAudioOutput::deinit() {
  if (!initialized) {
    return;
  }

  if (IMP_AO_FlushChnBuf(devId, channelId) != 0) {
    LOG_WARN("IMP_AO_FlushChnBuf failed for channel " << channelId);
  }

  if (IMP_AO_DisableChn(devId, channelId) != 0) {
    LOG_WARN("IMP_AO_DisableChn failed for channel " << channelId);
  }

  if (IMP_AO_Disable(devId) != 0) {
    LOG_WARN("IMP_AO_Disable failed for device " << devId);
  }

  initialized = false;
}

bool IMPAudioOutput::reconfigure(int newRateHz) {
  if (newRateHz <= 0) {
    return false;
  }
  if (initialized && newRateHz == configuredSampleRate) {
    return true; // already at this rate
  }

  int savedVolume = currentVolume;
  int savedGain = currentGain;
  bool savedMute = currentMute;

  deinit();

  if (!configureHardwareAtRate(newRateHz)) {
    LOG_ERROR("AO reconfigure to " << newRateHz << " Hz failed");
    return false;
  }

  setVolume(savedVolume);
  setGain(savedGain);
  setMute(savedMute);

  initialized = true;
  LOG_INFO("AO reconfigured to " << configuredSampleRate << " Hz");
  return true;
}

bool IMPAudioOutput::configureHardwareAtRate(int sampleRate) {
  auto aoSampleRate = toImpSampleRate(sampleRate);

  IMPAudioIOAttr attr{};
  attr.samplerate = aoSampleRate;
  attr.bitwidth = AUDIO_BIT_WIDTH_16;
  attr.soundmode = AUDIO_SOUND_MODE_MONO;
  attr.frmNum = 20;
  attr.numPerFrm = std::max(sampleRate / (1000 / kFrameDurationMs), 1);
  attr.chnCnt = 1;

  maxFrameBytes = attr.numPerFrm * static_cast<int>(sizeof(int16_t));

  if (IMP_AO_SetPubAttr(devId, &attr) != 0) {
    LOG_ERROR("IMP_AO_SetPubAttr failed");
    return false;
  }

  if (IMP_AO_GetPubAttr(devId, &attr) != 0) {
    LOG_ERROR("IMP_AO_GetPubAttr failed");
    return false;
  }

  int actualRate = static_cast<int>(attr.samplerate);
  if (actualRate > 0 && actualRate != sampleRate) {
    LOG_WARN("AO sample rate adjusted by hardware: requested "
             << sampleRate << " Hz, got " << actualRate
             << " Hz (shared CODEC clock?)");
    int newNumPerFrm = std::max(actualRate / (1000 / kFrameDurationMs), 1);
    maxFrameBytes = newNumPerFrm * static_cast<int>(sizeof(int16_t));
  }

  if (IMP_AO_Enable(devId) != 0) {
    LOG_ERROR("IMP_AO_Enable failed");
    return false;
  }

  if (IMP_AO_EnableChn(devId, channelId) != 0) {
    LOG_ERROR("IMP_AO_EnableChn failed for channel " << channelId);
    IMP_AO_Disable(devId);
    return false;
  }

  configuredSampleRate = (actualRate > 0) ? actualRate : sampleRate;
  LOG_INFO("AO initialised: device=" << devId << " channel=" << channelId
                                     << " rate=" << configuredSampleRate
                                     << " Hz"
                                     << " maxFrameBytes=" << maxFrameBytes);
  return true;
}

bool IMPAudioOutput::configureHardware() {
  return configureHardwareAtRate(samplerateFromConfig());
}

bool IMPAudioOutput::setVolume(int volume) {
  currentVolume = volume;
  if (IMP_AO_SetVol(devId, channelId, volume) != 0) {
    LOG_WARN("IMP_AO_SetVol failed (volume=" << volume << ")");
    return false;
  }
  return true;
}

bool IMPAudioOutput::setGain(int gain) {
  currentGain = gain;
  if (IMP_AO_SetGain(devId, channelId, gain) != 0) {
    LOG_WARN("IMP_AO_SetGain failed (gain=" << gain << ")");
    return false;
  }
  return true;
}

bool IMPAudioOutput::setMute(bool mute) {
  currentMute = mute;
  if (IMP_AO_SetVolMute(devId, channelId, mute ? 1 : 0) != 0) {
    LOG_WARN("IMP_AO_SetVolMute failed (mute=" << mute << ")");
    return false;
  }
  return true;
}

bool IMPAudioOutput::playSamples(const int16_t *samples, size_t sampleCount) {
  if (!initialized || samples == nullptr || sampleCount == 0) {
    return false;
  }

  const uint8_t *bytePtr = reinterpret_cast<const uint8_t *>(samples);
  size_t remainingBytes = sampleCount * sizeof(int16_t);

  while (remainingBytes > 0) {
    const size_t chunk =
        (maxFrameBytes > 0)
            ? std::min(static_cast<size_t>(maxFrameBytes), remainingBytes)
            : remainingBytes;
    IMPAudioFrame frame{};
    frame.virAddr =
        reinterpret_cast<uint32_t *>(const_cast<uint8_t *>(bytePtr));
    frame.len = static_cast<unsigned int>(chunk);

    if (IMP_AO_SendFrame(devId, channelId, &frame, BLOCK) != 0) {
      LOG_ERROR("IMP_AO_SendFrame failed (len=" << frame.len << ")");
      return false;
    }

    bytePtr += chunk;
    remainingBytes -= chunk;
  }

  return true;
}

bool IMPAudioOutput::flush() {
  if (!initialized) {
    return false;
  }

  if (IMP_AO_FlushChnBuf(devId, channelId) != 0) {
    LOG_WARN("IMP_AO_FlushChnBuf failed during flush request for channel "
             << channelId);
    return false;
  }
  return true;
}

int IMPAudioOutput::samplerateFromConfig() const {
  int requestedRate = cfg->audio.output_sample_rate();
  if (requestedRate <= 0) {
    requestedRate = 16000;
  }
  return requestedRate;
}

int IMPAudioOutput::playbackSampleRate() const {
  if (configuredSampleRate > 0) {
    return configuredSampleRate;
  }
  return samplerateFromConfig();
}

bool IMPAudioOutput::playSilence(int durationMs) {
  if (!initialized || durationMs <= 0) {
    return true;
  }

  const int rate = playbackSampleRate();
  if (rate <= 0) {
    return false;
  }

  size_t samples =
      static_cast<size_t>(static_cast<int64_t>(rate) * durationMs / 1000);
  if (samples == 0) {
    samples = std::max(rate / 50, 1); // default to ~20ms of silence
  }

  std::vector<int16_t> zeros(samples, 0);
  return playSamples(zeros.data(), zeros.size());
}
