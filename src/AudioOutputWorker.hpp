#ifndef AUDIO_OUTPUT_WORKER_HPP
#define AUDIO_OUTPUT_WORKER_HPP

#include <vector>
#include <cstdint>

#include "globals.hpp"

class AudioOutputWorker
{
public:
    AudioOutputWorker() = default;
    ~AudioOutputWorker() = default;

    static void *thread_entry(void *arg);

    static bool enqueuePcm(std::vector<int16_t> &&samples,
                           bool applyVolume = false,
                           int volume = 0,
                           bool applyGain = false,
                           int gain = 0);

    static bool enqueuePcmBlocking(std::vector<int16_t> &&samples,
                                   bool applyVolume = false,
                                   int volume = 0,
                                   bool applyGain = false,
                                   int gain = 0);

    static bool applyVolumeGain(bool applyVolume,
                                int volume,
                                bool applyGain,
                                int gain);

    static bool clearQueue();

    static void signalShutdown();

private:
    void run();
};

#endif // AUDIO_OUTPUT_WORKER_HPP
