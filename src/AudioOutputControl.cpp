#include "AudioOutputControl.hpp"

#include "AudioOutputWorker.hpp"
#include "Config.hpp"
#include "Logger.hpp"
#include "globals.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <aaccommon.h>
#include <aacdec.h>

#define MODULE "AudioOutputControl"

namespace
{
    constexpr const char *kFifoDir = "/run/prudynt";
    constexpr const char *kFifoPath = "/run/prudynt/audio_out";

    enum class AudioFileFormat
    {
        AUTO,
        PCM,
        WAV,
        AAC
    };

    struct PlayCommandOptions
    {
        std::string path;
        AudioFileFormat format{AudioFileFormat::AUTO};
        int sampleRate{0};
        bool hasSampleRate{false};
        bool append{false};
        bool setVolume{false};
        int volume{0};
        bool setGain{false};
        int gain{0};
    };

    const char *formatName(AudioFileFormat format)
    {
        switch (format)
        {
        case AudioFileFormat::PCM:
            return "pcm";
        case AudioFileFormat::WAV:
            return "wav";
        case AudioFileFormat::AAC:
            return "aac";
        default:
            return "auto";
        }
    }

    std::string trim(const std::string &value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return {};
        }
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::string toLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    std::string toUpper(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return value;
    }

    bool parseInt(const std::string &token, int &value)
    {
        if (token.empty())
        {
            return false;
        }
        char *end = nullptr;
        long parsed = std::strtol(token.c_str(), &end, 10);
        if (*end != '\0')
        {
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    }

    int clampVolume(int value)
    {
        return std::clamp(value, 0, 100);
    }

    int clampGain(int value)
    {
        return std::clamp(value, 0, 31);
    }

    int defaultSampleRate()
    {
        if (!cfg)
        {
            return 16000;
        }
        int sr = cfg->audio.output_sample_rate;
        return (sr > 0) ? sr : 16000;
    }

    bool ensureFifo()
    {
        if (mkdir(kFifoDir, 0775) < 0 && errno != EEXIST)
        {
            LOG_ERROR("AudioOutputControl: mkdir failed for " << kFifoDir << ": " << strerror(errno));
            return false;
        }

        ::unlink(kFifoPath);
        if (mkfifo(kFifoPath, 0666) < 0)
        {
            LOG_ERROR("AudioOutputControl: mkfifo failed for " << kFifoPath << ": " << strerror(errno));
            return false;
        }
        return true;
    }

    bool readEntirePcm(const std::string &path, std::vector<int16_t> &samples)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            LOG_ERROR("AudioOutputControl: failed to open PCM file '" << path << "'");
            return false;
        }

        file.seekg(0, std::ios::end);
        std::streampos endPos = file.tellg();
        if (endPos <= 0)
        {
            LOG_WARN("AudioOutputControl: PCM file '" << path << "' is empty");
            return false;
        }
        const size_t totalBytes = static_cast<size_t>(endPos);
        file.seekg(0, std::ios::beg);

        samples.resize(totalBytes / sizeof(int16_t));
        file.read(reinterpret_cast<char *>(samples.data()), static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));
        if (!file)
        {
            auto bytesRead = static_cast<size_t>(file.gcount());
            samples.resize(bytesRead / sizeof(int16_t));
            LOG_WARN("AudioOutputControl: partial read for PCM file '" << path << "', truncating to "
                                                                         << samples.size() << " samples");
        }

        if (totalBytes % sizeof(int16_t) != 0)
        {
            LOG_WARN("AudioOutputControl: PCM file '" << path << "' has trailing bytes; ignoring remainder");
        }

        return !samples.empty();
    }

    struct WavPayload
    {
        int sampleRate{0};
        uint16_t channels{0};
        uint16_t bitsPerSample{0};
        std::vector<int16_t> samples;
    };

    bool readWavFile(const std::string &path, WavPayload &payload)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            LOG_ERROR("AudioOutputControl: failed to open WAV file '" << path << "'");
            return false;
        }

        auto readChunkId = [&](char (&buffer)[4]) -> bool {
            return static_cast<bool>(file.read(buffer, 4));
        };

        char chunkId[4];
        if (!readChunkId(chunkId) || std::strncmp(chunkId, "RIFF", 4) != 0)
        {
            LOG_ERROR("AudioOutputControl: '" << path << "' is not a RIFF file");
            return false;
        }

        uint32_t riffSize = 0;
        file.read(reinterpret_cast<char *>(&riffSize), 4);
        (void) riffSize;

        if (!readChunkId(chunkId) || std::strncmp(chunkId, "WAVE", 4) != 0)
        {
            LOG_ERROR("AudioOutputControl: '" << path << "' is not a WAVE file");
            return false;
        }

        bool fmtFound = false;
        bool dataFound = false;
        std::streampos dataOffset{};
        uint32_t dataSize = 0;
        uint16_t audioFormat = 0;

        while (file && (!fmtFound || !dataFound))
        {
            if (!readChunkId(chunkId))
            {
                break;
            }
            uint32_t chunkSize = 0;
            if (!file.read(reinterpret_cast<char *>(&chunkSize), 4))
            {
                break;
            }

            if (std::strncmp(chunkId, "fmt ", 4) == 0)
            {
                fmtFound = true;
                file.read(reinterpret_cast<char *>(&audioFormat), 2);
                file.read(reinterpret_cast<char *>(&payload.channels), 2);
                file.read(reinterpret_cast<char *>(&payload.sampleRate), 4);
                uint32_t byteRate = 0;
                uint16_t blockAlign = 0;
                file.read(reinterpret_cast<char *>(&byteRate), 4);
                file.read(reinterpret_cast<char *>(&blockAlign), 2);
                file.read(reinterpret_cast<char *>(&payload.bitsPerSample), 2);

                if (chunkSize > 16)
                {
                    file.seekg(chunkSize - 16, std::ios::cur);
                }
            }
            else if (std::strncmp(chunkId, "data", 4) == 0)
            {
                dataFound = true;
                dataSize = chunkSize;
                dataOffset = file.tellg();
                file.seekg(chunkSize, std::ios::cur);
            }
            else
            {
                file.seekg(chunkSize, std::ios::cur);
            }

            if (chunkSize % 2 == 1)
            {
                file.seekg(1, std::ios::cur);
            }
        }

        if (!fmtFound || !dataFound)
        {
            LOG_ERROR("AudioOutputControl: WAV file '" << path << "' missing required chunks");
            return false;
        }

        if (audioFormat != 1 || payload.bitsPerSample != 16 || payload.channels != 1)
        {
            LOG_ERROR("AudioOutputControl: WAV file '" << path << "' must be 16-bit mono PCM");
            return false;
        }

        payload.samples.resize(dataSize / sizeof(int16_t));
        file.clear();
        file.seekg(dataOffset, std::ios::beg);
        file.read(reinterpret_cast<char *>(payload.samples.data()), dataSize);
        if (!file)
        {
            auto bytesRead = static_cast<size_t>(file.gcount());
            payload.samples.resize(bytesRead / sizeof(int16_t));
            LOG_WARN("AudioOutputControl: partial read for WAV file '" << path << "'");
        }

        return !payload.samples.empty();
    }

    struct AdtsHeader
    {
        int frameLength{0};
        int sampleRate{0};
        int channelCount{0};
        bool hasCrc{false};
        int headerSize{0};
        int profile{0};
    };

    bool parseAdtsHeader(const uint8_t *data, size_t size, AdtsHeader &header)
    {
        if (size < 7)
        {
            return false;
        }

        if (data[0] != 0xFF || (data[1] & 0xF0) != 0xF0)
        {
            return false;
        }

        bool protectionAbsent = (data[1] & 0x01) != 0;
        header.profile = ((data[2] & 0xC0) >> 6) + 1; // 1=Main, 2=LC, etc

        static constexpr int kSamplingRates[] = {
            96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
            16000, 12000, 11025, 8000, 7350
        };
        int sampleRateIndex = (data[2] & 0x3C) >> 2;
        if (sampleRateIndex < 0 || sampleRateIndex >= static_cast<int>(std::size(kSamplingRates)))
        {
            return false;
        }
        header.sampleRate = kSamplingRates[sampleRateIndex];

        header.channelCount = ((data[2] & 0x01) << 2) | ((data[3] & 0xC0) >> 6);
        header.hasCrc = !protectionAbsent;
        header.headerSize = header.hasCrc ? 9 : 7;

        header.frameLength = ((data[3] & 0x03) << 11)
                             | (data[4] << 3)
                             | ((data[5] & 0xE0) >> 5);

        if (header.frameLength < header.headerSize)
        {
            return false;
        }

        return true;
    }

    bool configureDecoderForHeader(HAACDecoder decoder, const AdtsHeader &header)
    {
        if (!decoder || header.sampleRate <= 0)
        {
            return false;
        }

        AACFrameInfo frameInfo{};
        frameInfo.nChans = (header.channelCount > 0) ? header.channelCount : 1;
        frameInfo.sampRateCore = header.sampleRate;
        frameInfo.profile = AAC_PROFILE_LC;

        int ret = AACSetRawBlockParams(decoder, 0, &frameInfo);
        if (ret != ERR_AAC_NONE)
        {
            LOG_ERROR("AudioOutputControl: AACSetRawBlockParams failed: " << ret);
            return false;
        }
        return true;
    }

    bool decodeAacFile(const std::string &path, std::vector<int16_t> &samples, int &sampleRate)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            LOG_ERROR("AudioOutputControl: failed to open AAC file '" << path << "'");
            return false;
        }

        std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (buffer.empty())
        {
            LOG_WARN("AudioOutputControl: AAC file '" << path << "' is empty");
            return false;
        }

        HAACDecoder decoder = AACInitDecoder();
        if (!decoder)
        {
            LOG_ERROR("AudioOutputControl: failed to initialize AAC decoder");
            return false;
        }
        struct DecoderGuard
        {
            HAACDecoder handle;
            explicit DecoderGuard(HAACDecoder h) : handle(h) {}
            ~DecoderGuard()
            {
                if (handle)
                {
                    AACFreeDecoder(handle);
                }
            }
        } decoderGuard(decoder);

        bool decoderConfigured = false;
        size_t offset = 0;
        constexpr size_t kMaxDecodeSamples = 8192;
        std::array<int16_t, kMaxDecodeSamples> decodeBuffer{};
        bool decodedAny = false;

        while (offset + 7 <= buffer.size())
        {
            AdtsHeader header;
            if (!parseAdtsHeader(buffer.data() + offset, buffer.size() - offset, header))
            {
                ++offset; // attempt to resync on next byte
                continue;
            }

            if (offset + static_cast<size_t>(header.frameLength) > buffer.size())
            {
                LOG_WARN("AudioOutputControl: truncated AAC frame near offset " << offset);
                break;
            }

            if (!decoderConfigured)
            {
                if (!configureDecoderForHeader(decoder, header))
                {
                    return false;
                }
                decoderConfigured = true;
            }

            int payloadSize = header.frameLength - header.headerSize;
            if (payloadSize <= 0)
            {
                offset += header.frameLength;
                continue;
            }

            unsigned char *framePtr = const_cast<unsigned char *>(buffer.data() + offset + header.headerSize);
            int bytesLeft = payloadSize;

            int ret = AACDecode(decoder, &framePtr, &bytesLeft, decodeBuffer.data());
            if (ret != 0 && ret != ERR_AAC_INDATA_UNDERFLOW)
            {
                LOG_ERROR("AudioOutputControl: AACDecode failed with " << ret << " at offset " << offset);
                return false;
            }

            AACFrameInfo frameInfo{};
            AACGetLastFrameInfo(decoder, &frameInfo);

            if (frameInfo.outputSamps <= 0)
            {
                offset += header.frameLength;
                continue;
            }

            if (frameInfo.outputSamps > static_cast<int>(kMaxDecodeSamples))
            {
                LOG_ERROR("AudioOutputControl: decoded AAC frame exceeds buffer capacity");
                return false;
            }

            if (frameInfo.nChans <= 0)
            {
                LOG_WARN("AudioOutputControl: decoded AAC frame reports zero channels");
                offset += header.frameLength;
                continue;
            }

            if (sampleRate == 0)
            {
                if (frameInfo.sampRateOut > 0)
                {
                    sampleRate = frameInfo.sampRateOut;
                }
                else if (header.sampleRate > 0)
                {
                    sampleRate = header.sampleRate;
                }
            }

            const int channels = frameInfo.nChans;
            if (channels == 1)
            {
                samples.insert(samples.end(),
                               decodeBuffer.begin(),
                               decodeBuffer.begin() + frameInfo.outputSamps);
            }
            else
            {
                const int frames = frameInfo.outputSamps / channels;
                for (int i = 0; i < frames; ++i)
                {
                    int sum = 0;
                    for (int ch = 0; ch < channels; ++ch)
                    {
                        sum += decodeBuffer[static_cast<size_t>(i) * channels + ch];
                    }
                    samples.push_back(static_cast<int16_t>(sum / channels));
                }
            }

            decodedAny = true;
            offset += header.frameLength;
        }

        if (!decodedAny)
        {
            LOG_WARN("AudioOutputControl: no decodable AAC frames found in '" << path << "'");
            return false;
        }

        if (sampleRate == 0)
        {
            sampleRate = defaultSampleRate();
        }

        return true;
    }

    std::vector<int16_t> resampleLinear(const std::vector<int16_t> &input, int inputRate, int outputRate)
    {
        if (inputRate == outputRate || input.empty())
        {
            return input;
        }

        double ratio = static_cast<double>(outputRate) / static_cast<double>(inputRate);
        size_t outputSize = static_cast<size_t>(std::max(1.0,
                                                         std::round(static_cast<double>(input.size()) * ratio)));
        std::vector<int16_t> output(outputSize);
        for (size_t i = 0; i < outputSize; ++i)
        {
            double inputPos = static_cast<double>(i) / ratio;
            size_t baseIndex = static_cast<size_t>(inputPos);
            if (baseIndex >= input.size())
            {
                baseIndex = input.size() - 1;
            }
            size_t nextIndex = std::min(baseIndex + 1, input.size() - 1);
            double factor = inputPos - static_cast<double>(baseIndex);
            double interpolated = static_cast<double>(input[baseIndex]) * (1.0 - factor)
                                  + static_cast<double>(input[nextIndex]) * factor;
            if (interpolated > INT16_MAX)
            {
                interpolated = INT16_MAX;
            }
            else if (interpolated < INT16_MIN)
            {
                interpolated = INT16_MIN;
            }
            output[i] = static_cast<int16_t>(interpolated);
        }
        return output;
    }

    void enqueueSamples(const std::vector<int16_t> &samples,
                        bool setVolume,
                        int volume,
                        bool setGain,
                        int gain)
    {
        if (samples.empty())
        {
            AudioOutputWorker::enqueuePcm(std::vector<int16_t>{}, setVolume, volume, setGain, gain);
            return;
        }

        int targetRate = defaultSampleRate();
        size_t chunk = static_cast<size_t>(std::max(targetRate / 50, 1));
        bool firstChunk = true;
        for (size_t offset = 0; offset < samples.size(); offset += chunk)
        {
            size_t remaining = std::min(chunk, samples.size() - offset);
            auto beginIt = samples.begin() + static_cast<std::ptrdiff_t>(offset);
            auto endIt = samples.begin() + static_cast<std::ptrdiff_t>(offset + remaining);
            std::vector<int16_t> block(beginIt, endIt);
            if (!AudioOutputWorker::enqueuePcmBlocking(std::move(block),
                                                       firstChunk && setVolume,
                                                       volume,
                                                       firstChunk && setGain,
                                                       gain))
            {
                LOG_ERROR("AudioOutputControl: failed to enqueue PCM chunk");
                break;
            }
            firstChunk = false;
        }

        if (firstChunk && (setVolume || setGain))
        {
            AudioOutputWorker::enqueuePcm(std::vector<int16_t>{}, setVolume, volume, setGain, gain);
        }
    }

    void handlePlay(const PlayCommandOptions &options)
    {
        if (!cfg || !cfg->audio.output_enabled)
        {
            LOG_WARN("AudioOutputControl: PLAY ignored because audio output is disabled");
            return;
        }
        if (!global_audio_output)
        {
            LOG_ERROR("AudioOutputControl: PLAY ignored because audio output stream is not available");
            return;
        }

        AudioFileFormat format = options.format;
        if (format == AudioFileFormat::AUTO)
        {
            std::filesystem::path path(options.path);
            std::string ext = toLower(path.extension().string());
            if (ext == ".wav" || ext == ".wave")
            {
                format = AudioFileFormat::WAV;
            }
            else if (ext == ".aac" || ext == ".adts")
            {
                format = AudioFileFormat::AAC;
            }
            else
            {
                format = AudioFileFormat::PCM;
            }
        }

        std::vector<int16_t> samples;
        int sourceRate = options.hasSampleRate ? options.sampleRate : 0;

        std::string rateStr = options.hasSampleRate ? std::to_string(options.sampleRate) : std::string("(default)");
        std::string volStr = options.setVolume ? std::to_string(options.volume) : std::string("(unchanged)");
        std::string gainStr = options.setGain ? std::to_string(options.gain) : std::string("(unchanged)");

        LOG_INFO("AudioOutputControl: PLAY requested (path='" << options.path
             << "', format=" << formatName(format)
             << ", append=" << (options.append ? 1 : 0)
             << ", rate=" << rateStr
             << ", vol=" << volStr
             << ", gain=" << gainStr << ")");

        if (format == AudioFileFormat::WAV)
        {
            WavPayload payload;
            if (!readWavFile(options.path, payload))
            {
                return;
            }
            samples = std::move(payload.samples);
            sourceRate = payload.sampleRate;
        }
        else if (format == AudioFileFormat::AAC)
        {
            if (!decodeAacFile(options.path, samples, sourceRate))
            {
                return;
            }
        }
        else
        {
            if (!readEntirePcm(options.path, samples))
            {
                return;
            }
            if (!options.hasSampleRate)
            {
                sourceRate = defaultSampleRate();
            }
        }

        if (samples.empty())
        {
            LOG_WARN("AudioOutputControl: PLAY command produced zero samples");
            return;
        }

        int targetRate = defaultSampleRate();
        if (sourceRate != targetRate)
        {
            samples = resampleLinear(samples, sourceRate, targetRate);
        }

        if (!options.append)
        {
            AudioOutputWorker::clearQueue();
        }

        bool pendingVolume = options.setVolume;
        bool pendingGain = options.setGain;
        if ((pendingVolume || pendingGain)
            && AudioOutputWorker::applyVolumeGain(pendingVolume, options.volume, pendingGain, options.gain))
        {
            pendingVolume = false;
            pendingGain = false;
        }

        LOG_INFO("AudioOutputControl: queuing " << samples.size() << " samples (src=" << sourceRate
             << " Hz -> dst=" << targetRate << " Hz)");

        enqueueSamples(samples,
                       pendingVolume,
                       options.volume,
                       pendingGain,
                       options.gain);
    }

    void applyVolumeChange(int volume)
    {
        int clamped = clampVolume(volume);
        if (!AudioOutputWorker::applyVolumeGain(true, clamped, false, 0))
        {
            AudioOutputWorker::enqueuePcm(std::vector<int16_t>{}, true, clamped, false, 0);
        }
    }

    void applyGainChange(int gain)
    {
        int clamped = clampGain(gain);
        if (!AudioOutputWorker::applyVolumeGain(false, 0, true, clamped))
        {
            AudioOutputWorker::enqueuePcm(std::vector<int16_t>{}, false, 0, true, clamped);
        }
    }

    void handleSetCommand(std::istringstream &iss)
    {
        bool volumeSet = false;
        bool gainSet = false;
        int volume = 0;
        int gain = 0;
        std::string token;
        while (iss >> token)
        {
            std::string key;
            std::string value = token;
            auto eq = token.find('=');
            if (eq != std::string::npos)
            {
                key = toLower(token.substr(0, eq));
                value = token.substr(eq + 1);
            }
            else
            {
                key.clear();
            }

            if (key == "volume" || key == "vol")
            {
                int parsed = 0;
                if (parseInt(value, parsed))
                {
                    volume = clampVolume(parsed);
                    volumeSet = true;
                }
            }
            else if (key == "gain")
            {
                int parsed = 0;
                if (parseInt(value, parsed))
                {
                    gain = clampGain(parsed);
                    gainSet = true;
                }
            }
        }

        if (volumeSet || gainSet)
        {
            if (!AudioOutputWorker::applyVolumeGain(volumeSet, volume, gainSet, gain))
            {
                AudioOutputWorker::enqueuePcm(std::vector<int16_t>{}, volumeSet, volume, gainSet, gain);
            }
        }
        else
        {
            LOG_WARN("AudioOutputControl: SET command missing assignments");
        }
    }

    void handleCommand(const std::string &line)
    {
        std::istringstream iss(line);
        std::string op;
        iss >> op;
        if (op.empty())
        {
            return;
        }
        op = toUpper(op);

        LOG_INFO("AudioOutputControl: received command '" << line << "'");

        if (op == "PLAY")
        {
            PlayCommandOptions options;
            std::string token;
            while (iss >> token)
            {
                std::string key;
                std::string value = token;
                auto eq = token.find('=');
                if (eq != std::string::npos)
                {
                    key = toLower(token.substr(0, eq));
                    value = token.substr(eq + 1);
                }
                else
                {
                    key.clear();
                }

                if (options.path.empty() && key.empty())
                {
                    options.path = value;
                    continue;
                }

                if (key.empty())
                {
                    continue;
                }

                if (key == "path")
                {
                    options.path = value;
                }
                else if (key == "vol" || key == "volume")
                {
                    int parsed = 0;
                    if (parseInt(value, parsed))
                    {
                        options.volume = clampVolume(parsed);
                        options.setVolume = true;
                    }
                }
                else if (key == "gain")
                {
                    int parsed = 0;
                    if (parseInt(value, parsed))
                    {
                        options.gain = clampGain(parsed);
                        options.setGain = true;
                    }
                }
                else if (key == "rate" || key == "samplerate")
                {
                    int parsed = 0;
                    if (parseInt(value, parsed) && parsed > 0)
                    {
                        options.sampleRate = parsed;
                        options.hasSampleRate = true;
                    }
                }
                else if (key == "append")
                {
                    int parsed = 0;
                    if (parseInt(value, parsed))
                    {
                        options.append = (parsed != 0);
                    }
                }
                else if (key == "format" || key == "fmt")
                {
                    std::string lowerValue = toLower(value);
                    if (lowerValue == "wav" || lowerValue == "wave")
                    {
                        options.format = AudioFileFormat::WAV;
                    }
                    else if (lowerValue == "pcm")
                    {
                        options.format = AudioFileFormat::PCM;
                    }
                    else if (lowerValue == "aac")
                    {
                        options.format = AudioFileFormat::AAC;
                    }
                }
            }

            if (options.path.empty())
            {
                LOG_WARN("AudioOutputControl: PLAY command missing path argument");
                return;
            }

            handlePlay(options);
        }
        else if (op == "STOP")
        {
            LOG_INFO("AudioOutputControl: STOP requested");
            if (!AudioOutputWorker::clearQueue())
            {
                LOG_WARN("AudioOutputControl: STOP command ignored; audio output queue not available");
            }
        }
        else if (op == "VOLUME")
        {
            std::string value;
            if (!(iss >> value))
            {
                LOG_WARN("AudioOutputControl: VOLUME command missing argument");
                return;
            }
            int parsed = 0;
            if (!parseInt(value, parsed))
            {
                LOG_WARN("AudioOutputControl: invalid VOLUME value '" << value << "'");
                return;
            }
            LOG_INFO("AudioOutputControl: VOLUME=" << parsed);
            applyVolumeChange(parsed);
        }
        else if (op == "GAIN")
        {
            std::string value;
            if (!(iss >> value))
            {
                LOG_WARN("AudioOutputControl: GAIN command missing argument");
                return;
            }
            int parsed = 0;
            if (!parseInt(value, parsed))
            {
                LOG_WARN("AudioOutputControl: invalid GAIN value '" << value << "'");
                return;
            }
            LOG_INFO("AudioOutputControl: GAIN=" << parsed);
            applyGainChange(parsed);
        }
        else if (op == "SET")
        {
            LOG_INFO("AudioOutputControl: SET command received");
            handleSetCommand(iss);
        }
        else
        {
            LOG_WARN("AudioOutputControl: unrecognized command '" << op << "'");
        }
    }
}

void AudioOutputControl::run()
{
    if (!cfg)
    {
        LOG_ERROR("AudioOutputControl: configuration unavailable; not starting FIFO thread");
        return;
    }

    if (!cfg->audio.output_enabled)
    {
        LOG_INFO("AudioOutputControl: audio output disabled, skipping FIFO setup");
        return;
    }

    if (!ensureFifo())
    {
        return;
    }

    LOG_INFO("AudioOutputControl: listening on FIFO " << kFifoPath);

    while (!global_shutdown_requested.load(std::memory_order_relaxed))
    {
        int fd = ::open(kFifoPath, O_RDONLY);
        if (fd < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_ERROR("AudioOutputControl: open() failed for FIFO: " << strerror(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        while (!global_shutdown_requested.load(std::memory_order_relaxed))
        {
            char buffer[512];
            ssize_t bytes = ::read(fd, buffer, sizeof(buffer));
            if (bytes <= 0)
            {
                break;
            }

            std::string chunk(buffer, static_cast<size_t>(bytes));
            std::istringstream lines(chunk);
            std::string line;
            while (std::getline(lines, line))
            {
                auto cleaned = trim(line);
                if (!cleaned.empty())
                {
                    handleCommand(cleaned);
                }
            }
        }

        ::close(fd);
    }

    ::unlink(kFifoPath);
    LOG_INFO("AudioOutputControl: shutting down and removing FIFO");
}
