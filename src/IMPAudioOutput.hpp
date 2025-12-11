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
  int samplerateFromConfig() const;
  int playbackSampleRate() const;
};

#endif // IMP_AUDIO_OUTPUT_HPP
