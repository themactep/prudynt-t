#ifndef IMP_AUDIO_OUTPUT_HPP
#define IMP_AUDIO_OUTPUT_HPP

#include <cstddef>
#include <cstdint>
#include <string>

class IMPAudioOutput {
public:
  explicit IMPAudioOutput(int devId = 0, int channelId = 0);
  ~IMPAudioOutput();

  bool init();
  void deinit();

  /// Reconfigure AO to a different sample rate (deinit + reinit).
  /// Preserves current volume/gain/mute settings.
  bool reconfigure(int newRateHz);

  bool setVolume(int volume);
  bool setGain(int gain);
  bool setMute(bool mute);

  bool playSamples(const int16_t *samples, size_t sampleCount);
  bool flush();
  bool playSilence(int durationMs);

  int getVolume() const {
    return currentVolume;
  }
  int getGain() const {
    return currentGain;
  }
  /// Return the actual sample rate that the hardware is running at.
  /// May differ from the configured rate on shared-CODEC platforms.
  int getPlaybackSampleRate() const {
    return playbackSampleRate();
  }

private:
  bool initialized;
  int devId;
  int channelId;
  int maxFrameBytes;
  int currentVolume;
  int currentGain;
  bool currentMute;
  int configuredSampleRate;

  bool configureHardware();
  bool configureHardwareAtRate(int sampleRate);
  int samplerateFromConfig() const;
  int playbackSampleRate() const;
};

#endif // IMP_AUDIO_OUTPUT_HPP
