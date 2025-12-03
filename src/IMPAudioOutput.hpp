#ifndef IMP_AUDIO_OUTPUT_HPP
#define IMP_AUDIO_OUTPUT_HPP

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>

class IMPAudioOutput
{
public:
    explicit IMPAudioOutput(int devId = 0, int channelId = 0);
    ~IMPAudioOutput();

    bool init();
    void deinit();

    bool setVolume(int volume);
    bool setGain(int gain);

    bool playSamples(const int16_t *samples, size_t sampleCount);
    bool flush();
    bool playSilence(int durationMs);
    void logLastBufferPreview(const std::string &context) const;

    int getVolume() const { return currentVolume; }
    int getGain() const { return currentGain; }

private:
    bool initialized;
    int devId;
    int channelId;
    int maxFrameBytes;
    int currentVolume;
    int currentGain;
    int configuredSampleRate;
    std::array<uint8_t, 64> bufferPreview{};
    size_t bufferPreviewLen{0};

    bool configureHardware();
    int samplerateFromConfig() const;
    void rememberPreview(const uint8_t *data, size_t length);
    int playbackSampleRate() const;
};

#endif // IMP_AUDIO_OUTPUT_HPP
