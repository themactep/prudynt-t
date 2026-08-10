#ifndef AUDIO_OUTPUT_WORKER_HPP
#define AUDIO_OUTPUT_WORKER_HPP

#include <chrono>
#include <cstdint>
#include <vector>

#include "stream/globals.hpp"

class AudioOutputWorker {
public:
  AudioOutputWorker() = default;
  ~AudioOutputWorker() = default;

  static void *thread_entry(void *arg);

  static bool enqueuePcm(std::vector<int16_t> &&samples,
                         bool applyVolume = false, int volume = 0,
                         bool applyGain = false, int gain = 0,
                         bool applyMute = false, bool mute = false);

  static bool enqueuePcmBlocking(std::vector<int16_t> &&samples,
                                 bool applyVolume = false, int volume = 0,
                                 bool applyGain = false, int gain = 0,
                                 bool applyMute = false, bool mute = false);

  static bool applyVolumeGain(bool applyVolume, int volume, bool applyGain,
                              int gain);

  static bool applyMute(bool mute);

  static bool clearQueue(bool waitForFlush = false);

  /// Reconfigure AO hardware to the given sample rate.  Blocks until
  /// the worker thread has completed the reconfiguration.
  /// Returns true if AO is now running at newRateHz.
  static bool reconfigureRate(int newRateHz);

  static bool waitForPlaybackCompletion(
      std::chrono::milliseconds waitDuration, bool flushAfterWait,
      std::chrono::milliseconds silencePadding = std::chrono::milliseconds(0));

  static void signalShutdown();

private:
  void run(struct StartHelper *sh = nullptr);
};

#endif // AUDIO_OUTPUT_WORKER_HPP
