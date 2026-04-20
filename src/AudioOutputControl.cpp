#include "AudioOutputControl.hpp"

#include "AudioOutputWorker.hpp"
#include "Config.hpp"
#include "Logger.hpp"
#include "globals.hpp"
#include "imp_hal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
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
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(USE_AAC) && USE_AAC
#include <aaccommon.h>
#include <aacdec.h>
#endif

#if defined(USE_MP3) && USE_MP3
#ifndef ARDUINO
#define AUDIO_OUTPUT_CONTROL_DEFINED_ARDUINO
#define ARDUINO
#endif
#include <mp3dec.h>
#ifdef AUDIO_OUTPUT_CONTROL_DEFINED_ARDUINO
#undef ARDUINO
#undef AUDIO_OUTPUT_CONTROL_DEFINED_ARDUINO
#endif
#endif

#if defined(USE_FLAC) && USE_FLAC
#include <FLAC/stream_decoder.h>
#endif

#if defined(USE_OPUS) && USE_OPUS
#include <opus/opus.h>
#endif

#define MODULE "AudioOutputControl"

namespace {
constexpr const char *kFifoDir = "/run/prudynt";
constexpr const char *kFifoPath = "/run/prudynt/audio_out";
constexpr const char *kControlFifoPath = "/run/prudynt/audio_out_ctrl";
constexpr int kMaxLoopCount = 32;
constexpr int kMaxLoopDelayMs = 5000;

enum class AudioFileFormat { AUTO, PCM, WAV, AAC, OPUS, MP3, FLAC };

struct PlayCommandOptions {
  std::string path;
  AudioFileFormat format{AudioFileFormat::AUTO};
  int sampleRate{0};
  bool hasSampleRate{false};
  bool append{false};
  bool setVolume{false};
  int volume{0};
  bool setGain{false};
  int gain{0};
  int loopCount{1};
  int loopDelayMs{0};
};

const char *formatName(AudioFileFormat format) {
  switch (format) {
  case AudioFileFormat::AAC:
    return "aac";
  case AudioFileFormat::FLAC:
    return "flac";
  case AudioFileFormat::MP3:
    return "mp3";
  case AudioFileFormat::OPUS:
    return "opus";
  case AudioFileFormat::PCM:
    return "pcm";
  case AudioFileFormat::WAV:
    return "wav";
  default:
    return "auto";
  }
}

std::string trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string toLower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string toUpper(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return value;
}

bool parseInt(const std::string &token, int &value) {
  if (token.empty()) {
    return false;
  }
  char *end = nullptr;
  long parsed = std::strtol(token.c_str(), &end, 10);
  if (*end != '\0') {
    return false;
  }
  value = static_cast<int>(parsed);
  return true;
}

bool parseBool(const std::string &token, bool &value) {
  if (token.empty()) {
    return false;
  }
  std::string lower = toLower(token);
  if (lower == "true" || lower == "on" || lower == "yes") {
    value = true;
    return true;
  }
  if (lower == "false" || lower == "off" || lower == "no") {
    value = false;
    return true;
  }
  if (lower == "1") {
    value = true;
    return true;
  }
  if (lower == "0") {
    value = false;
    return true;
  }
  int parsed = 0;
  if (parseInt(token, parsed)) {
    value = (parsed != 0);
    return true;
  }
  return false;
}

int clampVolume(int value) {
  // SDK IMP_AO_SetVol accepts -30 (mute) through 120 (+30 dB).
  return std::clamp(value, -30, 120);
}

int clampGain(int value) {
  return std::clamp(value, 0, 31);
}

int defaultSampleRate() {
  if (!cfg) {
    return 16000;
  }
  int sr = cfg->audio.output_sample_rate;
  return (sr > 0) ? sr : 16000;
}

/// Return the sample rate that the AO hardware is actually running at.
/// On platforms where the CODEC clock is shared between AI and AO
/// (T10/T20/T21), the hardware rate may differ from the configured
/// output_sample_rate.  Falls back to defaultSampleRate() when the
/// hardware rate has not been published yet.
int effectiveOutputSampleRate() {
  if (global_audio_output) {
    int hw =
        global_audio_output->hardwareSampleRate.load(std::memory_order_acquire);
    if (hw > 0) {
      return hw;
    }
  }
  return defaultSampleRate();
}

int clampLoopCount(int value) {
  if (value < 1) {
    return 1;
  }
  return std::min(value, kMaxLoopCount);
}

int clampLoopDelay(int value) {
  if (value < 0) {
    return 0;
  }
  return std::min(value, kMaxLoopDelayMs);
}

bool ensureFifo(const char *path) {
  if (mkdir(kFifoDir, 0775) < 0 && errno != EEXIST) {
    LOG_ERROR("AudioOutputControl: mkdir failed for " << kFifoDir << ": "
                                                      << strerror(errno));
    return false;
  }

  ::unlink(path);
  if (mkfifo(path, 0666) < 0) {
    LOG_ERROR("AudioOutputControl: mkfifo failed for " << path << ": "
                                                       << strerror(errno));
    return false;
  }
  return true;
}

struct FifoListenerConfig {
  const char *path;
  const char *label;
  bool allowPlayCommands;
};

void fifoListenerLoop(const FifoListenerConfig &config);

bool readEntirePcm(const std::string &path, std::vector<int16_t> &samples) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    LOG_ERROR("AudioOutputControl: failed to open PCM file '" << path << "'");
    return false;
  }

  file.seekg(0, std::ios::end);
  std::streampos endPos = file.tellg();
  if (endPos <= 0) {
    LOG_WARN("AudioOutputControl: PCM file '" << path << "' is empty");
    return false;
  }
  const size_t totalBytes = static_cast<size_t>(endPos);
  file.seekg(0, std::ios::beg);

  samples.resize(totalBytes / sizeof(int16_t));
  file.read(reinterpret_cast<char *>(samples.data()),
            static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));
  if (!file) {
    auto bytesRead = static_cast<size_t>(file.gcount());
    samples.resize(bytesRead / sizeof(int16_t));
    LOG_WARN("AudioOutputControl: partial read for PCM file '"
             << path << "', truncating to " << samples.size() << " samples");
  }

  if (totalBytes % sizeof(int16_t) != 0) {
    LOG_WARN("AudioOutputControl: PCM file '"
             << path << "' has trailing bytes; ignoring remainder");
  }

  return !samples.empty();
}

struct WavPayload {
  int sampleRate{0};
  uint16_t channels{0};
  uint16_t bitsPerSample{0};
  std::vector<int16_t> samples;
};

bool readWavFile(const std::string &path, WavPayload &payload) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    LOG_ERROR("AudioOutputControl: failed to open WAV file '" << path << "'");
    return false;
  }

  auto readChunkId = [&](char (&buffer)[4]) -> bool {
    return static_cast<bool>(file.read(buffer, 4));
  };

  char chunkId[4];
  if (!readChunkId(chunkId) || std::strncmp(chunkId, "RIFF", 4) != 0) {
    LOG_ERROR("AudioOutputControl: '" << path << "' is not a RIFF file");
    return false;
  }

  uint32_t riffSize = 0;
  file.read(reinterpret_cast<char *>(&riffSize), 4);
  (void)riffSize;

  if (!readChunkId(chunkId) || std::strncmp(chunkId, "WAVE", 4) != 0) {
    LOG_ERROR("AudioOutputControl: '" << path << "' is not a WAVE file");
    return false;
  }

  bool fmtFound = false;
  bool dataFound = false;
  std::streampos dataOffset{};
  uint32_t dataSize = 0;
  uint16_t audioFormat = 0;

  while (file && (!fmtFound || !dataFound)) {
    if (!readChunkId(chunkId)) {
      break;
    }
    uint32_t chunkSize = 0;
    if (!file.read(reinterpret_cast<char *>(&chunkSize), 4)) {
      break;
    }

    if (std::strncmp(chunkId, "fmt ", 4) == 0) {
      fmtFound = true;
      file.read(reinterpret_cast<char *>(&audioFormat), 2);
      file.read(reinterpret_cast<char *>(&payload.channels), 2);
      file.read(reinterpret_cast<char *>(&payload.sampleRate), 4);
      uint32_t byteRate = 0;
      uint16_t blockAlign = 0;
      file.read(reinterpret_cast<char *>(&byteRate), 4);
      file.read(reinterpret_cast<char *>(&blockAlign), 2);
      file.read(reinterpret_cast<char *>(&payload.bitsPerSample), 2);

      if (chunkSize > 16) {
        file.seekg(chunkSize - 16, std::ios::cur);
      }
    } else if (std::strncmp(chunkId, "data", 4) == 0) {
      dataFound = true;
      dataSize = chunkSize;
      dataOffset = file.tellg();
      file.seekg(chunkSize, std::ios::cur);
    } else {
      file.seekg(chunkSize, std::ios::cur);
    }

    if (chunkSize % 2 == 1) {
      file.seekg(1, std::ios::cur);
    }
  }

  if (!fmtFound || !dataFound) {
    LOG_ERROR("AudioOutputControl: WAV file '" << path
                                               << "' missing required chunks");
    return false;
  }

  if (audioFormat != 1 || payload.bitsPerSample != 16 ||
      payload.channels != 1) {
    LOG_ERROR("AudioOutputControl: WAV file '" << path
                                               << "' must be 16-bit mono PCM");
    return false;
  }

  payload.samples.resize(dataSize / sizeof(int16_t));
  file.clear();
  file.seekg(dataOffset, std::ios::beg);
  file.read(reinterpret_cast<char *>(payload.samples.data()), dataSize);
  if (!file) {
    auto bytesRead = static_cast<size_t>(file.gcount());
    payload.samples.resize(bytesRead / sizeof(int16_t));
    LOG_WARN("AudioOutputControl: partial read for WAV file '" << path << "'");
  }

  return !payload.samples.empty();
}

#if defined(USE_AAC) && USE_AAC
struct AdtsHeader {
  int frameLength{0};
  int sampleRate{0};
  int channelCount{0};
  bool hasCrc{false};
  int headerSize{0};
  int profile{0};
};

bool parseAdtsHeader(const uint8_t *data, size_t size, AdtsHeader &header) {
  if (size < 7) {
    return false;
  }

  if (data[0] != 0xFF || (data[1] & 0xF0) != 0xF0) {
    return false;
  }

  bool protectionAbsent = (data[1] & 0x01) != 0;
  header.profile = ((data[2] & 0xC0) >> 6) + 1; // 1=Main, 2=LC, etc

  static constexpr int kSamplingRates[] = {96000, 88200, 64000, 48000, 44100,
                                           32000, 24000, 22050, 16000, 12000,
                                           11025, 8000,  7350};
  int sampleRateIndex = (data[2] & 0x3C) >> 2;
  if (sampleRateIndex < 0 ||
      sampleRateIndex >= static_cast<int>(std::size(kSamplingRates))) {
    return false;
  }
  header.sampleRate = kSamplingRates[sampleRateIndex];

  header.channelCount = ((data[2] & 0x01) << 2) | ((data[3] & 0xC0) >> 6);
  header.hasCrc = !protectionAbsent;
  header.headerSize = header.hasCrc ? 9 : 7;

  header.frameLength =
      ((data[3] & 0x03) << 11) | (data[4] << 3) | ((data[5] & 0xE0) >> 5);

  if (header.frameLength < header.headerSize) {
    return false;
  }

  return true;
}

bool configureDecoderForHeader(HAACDecoder decoder, const AdtsHeader &header) {
  if (!decoder || header.sampleRate <= 0) {
    return false;
  }

  AACFrameInfo frameInfo{};
  frameInfo.nChans = (header.channelCount > 0) ? header.channelCount : 1;
  frameInfo.sampRateCore = header.sampleRate;
  frameInfo.profile = AAC_PROFILE_LC;

  int ret = AACSetRawBlockParams(decoder, 0, &frameInfo);
  if (ret != ERR_AAC_NONE) {
    LOG_ERROR("AudioOutputControl: AACSetRawBlockParams failed: " << ret);
    return false;
  }
  return true;
}

bool decodeAacFile(const std::string &path, std::vector<int16_t> &samples,
                   int &sampleRate) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    LOG_ERROR("AudioOutputControl: failed to open AAC file '" << path << "'");
    return false;
  }

  std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
  if (buffer.empty()) {
    LOG_WARN("AudioOutputControl: AAC file '" << path << "' is empty");
    return false;
  }

  HAACDecoder decoder = AACInitDecoder();
  if (!decoder) {
    LOG_ERROR("AudioOutputControl: failed to initialize AAC decoder");
    return false;
  }
  struct DecoderGuard {
    HAACDecoder handle;
    explicit DecoderGuard(HAACDecoder h) : handle(h) {
    }
    ~DecoderGuard() {
      if (handle) {
        AACFreeDecoder(handle);
      }
    }
  } decoderGuard(decoder);

  bool decoderConfigured = false;
  size_t offset = 0;
  constexpr size_t kMaxDecodeSamples = 8192;
  std::array<int16_t, kMaxDecodeSamples> decodeBuffer{};
  bool decodedAny = false;

  while (offset + 7 <= buffer.size()) {
    AdtsHeader header;
    if (!parseAdtsHeader(buffer.data() + offset, buffer.size() - offset,
                         header)) {
      ++offset; // attempt to resync on next byte
      continue;
    }

    if (offset + static_cast<size_t>(header.frameLength) > buffer.size()) {
      LOG_WARN("AudioOutputControl: truncated AAC frame near offset "
               << offset);
      break;
    }

    if (!decoderConfigured) {
      if (!configureDecoderForHeader(decoder, header)) {
        return false;
      }
      decoderConfigured = true;
    }

    int payloadSize = header.frameLength - header.headerSize;
    if (payloadSize <= 0) {
      offset += header.frameLength;
      continue;
    }

    unsigned char *framePtr =
        const_cast<unsigned char *>(buffer.data() + offset + header.headerSize);
    int bytesLeft = payloadSize;

    int ret = AACDecode(decoder, &framePtr, &bytesLeft, decodeBuffer.data());
    if (ret != 0 && ret != ERR_AAC_INDATA_UNDERFLOW) {
      LOG_ERROR("AudioOutputControl: AACDecode failed with "
                << ret << " at offset " << offset);
      return false;
    }

    AACFrameInfo frameInfo{};
    AACGetLastFrameInfo(decoder, &frameInfo);

    if (frameInfo.outputSamps <= 0) {
      offset += header.frameLength;
      continue;
    }

    if (frameInfo.outputSamps > static_cast<int>(kMaxDecodeSamples)) {
      LOG_ERROR(
          "AudioOutputControl: decoded AAC frame exceeds buffer capacity");
      return false;
    }

    if (frameInfo.nChans <= 0) {
      LOG_WARN("AudioOutputControl: decoded AAC frame reports zero channels");
      offset += header.frameLength;
      continue;
    }

    if (sampleRate == 0) {
      if (frameInfo.sampRateOut > 0) {
        sampleRate = frameInfo.sampRateOut;
      } else if (header.sampleRate > 0) {
        sampleRate = header.sampleRate;
      }
    }

    const int channels = frameInfo.nChans;
    if (channels == 1) {
      samples.insert(samples.end(), decodeBuffer.begin(),
                     decodeBuffer.begin() + frameInfo.outputSamps);
    } else {
      const int frames = frameInfo.outputSamps / channels;
      for (int i = 0; i < frames; ++i) {
        int sum = 0;
        for (int ch = 0; ch < channels; ++ch) {
          sum += decodeBuffer[static_cast<size_t>(i) * channels + ch];
        }
        samples.push_back(static_cast<int16_t>(sum / channels));
      }
    }

    decodedAny = true;
    offset += header.frameLength;
  }

  if (!decodedAny) {
    LOG_WARN("AudioOutputControl: no decodable AAC frames found in '" << path
                                                                      << "'");
    return false;
  }

  if (sampleRate == 0) {
    sampleRate = defaultSampleRate();
  }

  return true;
}
#else
bool decodeAacFile(const std::string &path, std::vector<int16_t> & /*samples*/,
                   int & /*sampleRate*/) {
  LOG_ERROR("AudioOutputControl: AAC support is disabled at build time ("
            << path << ")");
  return false;
}
#endif

#if defined(USE_MP3) && USE_MP3
bool decodeMp3File(const std::string &path, std::vector<int16_t> &samples,
                   int &sampleRate) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    LOG_ERROR("AudioOutputControl: failed to open MP3 file '" << path << "'");
    return false;
  }

  std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
  if (buffer.empty()) {
    LOG_WARN("AudioOutputControl: MP3 file '" << path << "' is empty");
    return false;
  }

  HMP3Decoder decoder = MP3InitDecoder();
  if (!decoder) {
    LOG_ERROR("AudioOutputControl: failed to initialize MP3 decoder");
    return false;
  }
  struct Mp3DecoderGuard {
    HMP3Decoder handle;
    explicit Mp3DecoderGuard(HMP3Decoder h) : handle(h) {
    }
    ~Mp3DecoderGuard() {
      if (handle) {
        MP3FreeDecoder(handle);
      }
    }
  } decoderGuard(decoder);

  unsigned char *readPtr = buffer.data();
  int bytesLeft = static_cast<int>(buffer.size());
  std::array<int16_t, MAX_NCHAN * MAX_NSAMP * MAX_NGRAN> decodeBuffer{};
  bool decodedAny = false;

  while (bytesLeft > 0) {
    int offset = MP3FindSyncWord(readPtr, bytesLeft);
    if (offset < 0) {
      break;
    }
    readPtr += offset;
    bytesLeft -= offset;
    if (bytesLeft <= 0) {
      break;
    }

    unsigned char *framePtr = readPtr;
    int frameBytesLeft = bytesLeft;
    int err =
        MP3Decode(decoder, &framePtr, &frameBytesLeft, decodeBuffer.data(), 0);
    if (err == ERR_MP3_INDATA_UNDERFLOW) {
      break;
    }
    if (err != ERR_MP3_NONE) {
      LOG_WARN("AudioOutputControl: MP3Decode error "
               << err << " for '" << path << "', attempting resync");
      if (bytesLeft <= 1) {
        break;
      }
      ++readPtr;
      --bytesLeft;
      continue;
    }

    MP3FrameInfo frameInfo{};
    MP3GetLastFrameInfo(decoder, &frameInfo);
    if (frameInfo.samprate > 0 && sampleRate == 0) {
      sampleRate = frameInfo.samprate;
    }

    int channels = std::max(frameInfo.nChans, 1);
    size_t outputSamples =
        static_cast<size_t>(std::max(frameInfo.outputSamps, 0));
    if (outputSamples == 0) {
      readPtr = framePtr;
      bytesLeft = frameBytesLeft;
      continue;
    }

    size_t limitedSamples = std::min(outputSamples, decodeBuffer.size());
    size_t frames =
        (channels > 0) ? (limitedSamples / static_cast<size_t>(channels)) : 0;
    if (frames == 0) {
      readPtr = framePtr;
      bytesLeft = frameBytesLeft;
      continue;
    }

    samples.reserve(samples.size() + frames);
    if (channels == 1) {
      samples.insert(samples.end(), decodeBuffer.begin(),
                     decodeBuffer.begin() + limitedSamples);
    } else {
      for (size_t i = 0; i < frames; ++i) {
        int32_t sum = 0;
        for (int ch = 0; ch < channels; ++ch) {
          size_t idx =
              i * static_cast<size_t>(channels) + static_cast<size_t>(ch);
          sum += decodeBuffer[idx];
        }
        sum /= channels;
        if (sum > INT16_MAX) {
          sum = INT16_MAX;
        } else if (sum < INT16_MIN) {
          sum = INT16_MIN;
        }
        samples.push_back(static_cast<int16_t>(sum));
      }
    }

    decodedAny = true;
    readPtr = framePtr;
    bytesLeft = frameBytesLeft;
  }

  if (!decodedAny) {
    LOG_WARN("AudioOutputControl: no decodable MP3 frames found in '" << path
                                                                      << "'");
    return false;
  }

  if (sampleRate == 0) {
    sampleRate = defaultSampleRate();
  }

  return true;
}
#else
bool decodeMp3File(const std::string &path, std::vector<int16_t> & /*samples*/,
                   int & /*sampleRate*/) {
  LOG_ERROR("AudioOutputControl: MP3 support is disabled at build time ("
            << path << ")");
  return false;
}
#endif

AudioFileFormat inferFormatFromExtension(const std::string &path) {
  auto stripQuery = [](std::string value) {
    auto hash = value.find('#');
    if (hash != std::string::npos) {
      value = value.substr(0, hash);
    }
    auto query = value.find('?');
    if (query != std::string::npos) {
      value = value.substr(0, query);
    }
    return value;
  };

  std::string trimmed = stripQuery(path);
  auto slash = trimmed.find_last_of('/');
  if (slash != std::string::npos && slash + 1 < trimmed.size()) {
    trimmed = trimmed.substr(slash + 1);
  }

  auto dot = trimmed.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= trimmed.size()) {
    return AudioFileFormat::PCM;
  }

  std::string ext = toLower(trimmed.substr(dot));
  if (ext == ".aac" || ext == ".adts") {
    return AudioFileFormat::AAC;
  }
  if (ext == ".flac") {
    return AudioFileFormat::FLAC;
  }
  if (ext == ".mp3" || ext == ".mp2" || ext == ".mpeg") {
    return AudioFileFormat::MP3;
  }
  if (ext == ".opus" || ext == ".oga" || ext == ".ogg") {
    return AudioFileFormat::OPUS;
  }
  if (ext == ".wav" || ext == ".wave") {
    return AudioFileFormat::WAV;
  }
  return AudioFileFormat::PCM;
}

bool fileLooksLikeWav(std::ifstream &file) {
  std::array<char, 12> header{};
  file.read(header.data(), static_cast<std::streamsize>(header.size()));
  if (file.gcount() < static_cast<std::streamsize>(header.size())) {
    return false;
  }
  return std::memcmp(header.data(), "RIFF", 4) == 0 &&
         std::memcmp(header.data() + 8, "WAVE", 4) == 0;
}

#if defined(USE_AAC) && USE_AAC
bool fileLooksLikeAac(std::ifstream &file) {
  std::array<uint8_t, 7> header{};
  file.read(reinterpret_cast<char *>(header.data()),
            static_cast<std::streamsize>(header.size()));
  if (file.gcount() < static_cast<std::streamsize>(header.size())) {
    return false;
  }
  AdtsHeader adts{};
  return parseAdtsHeader(header.data(), header.size(), adts);
}
#else
bool fileLooksLikeAac(std::ifstream & /*file*/) {
  return false;
}
#endif

#if defined(USE_OPUS) && USE_OPUS
bool fileLooksLikeOpus(std::ifstream &file) {
  char capture[4];
  if (!file.read(capture, sizeof(capture))) {
    return false;
  }
  if (std::strncmp(capture, "OggS", 4) != 0) {
    return false;
  }

  std::array<unsigned char, 23> header{};
  if (!file.read(reinterpret_cast<char *>(header.data()),
                 static_cast<std::streamsize>(header.size()))) {
    return false;
  }

  uint8_t version = header[0];
  if (version != 0) {
    return false;
  }

  uint8_t headerType = header[1];
  if ((headerType & 0x02) == 0) {
    return false;
  }

  uint8_t pageSegments = header[22];
  std::vector<uint8_t> lacing(pageSegments);
  if (pageSegments > 0 &&
      !file.read(reinterpret_cast<char *>(lacing.data()),
                 static_cast<std::streamsize>(pageSegments))) {
    return false;
  }

  size_t payloadSize = 0;
  for (uint8_t segLen : lacing) {
    payloadSize += segLen;
  }

  std::vector<uint8_t> payload(payloadSize);
  if (payloadSize > 0 &&
      !file.read(reinterpret_cast<char *>(payload.data()),
                 static_cast<std::streamsize>(payload.size()))) {
    return false;
  }

  std::vector<uint8_t> packet;
  packet.reserve(256);
  size_t payloadOffset = 0;
  for (uint8_t segLen : lacing) {
    if (payloadOffset + segLen > payload.size()) {
      return false;
    }
    packet.insert(packet.end(),
                  payload.begin() + static_cast<std::ptrdiff_t>(payloadOffset),
                  payload.begin() +
                      static_cast<std::ptrdiff_t>(payloadOffset + segLen));
    payloadOffset += segLen;
    if (segLen < 255) {
      break;
    }
  }

  return packet.size() >= 8 && std::memcmp(packet.data(), "OpusHead", 8) == 0;
}
#else
bool fileLooksLikeOpus(std::ifstream & /*file*/) {
  return false;
}
#endif

#if defined(USE_MP3) && USE_MP3
bool fileLooksLikeMp3(std::ifstream &file) {
  std::array<char, 3> signature{};
  file.read(signature.data(), static_cast<std::streamsize>(signature.size()));
  if (file.gcount() < static_cast<std::streamsize>(signature.size())) {
    return false;
  }
  if (std::memcmp(signature.data(), "ID3", 3) == 0) {
    return true;
  }

  file.clear();
  file.seekg(0, std::ios::beg);
  std::array<unsigned char, 2048> buffer{};
  file.read(reinterpret_cast<char *>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
  std::streamsize readBytes = file.gcount();
  if (readBytes <= 0) {
    return false;
  }
  return MP3FindSyncWord(buffer.data(), static_cast<int>(readBytes)) >= 0;
}
#else
bool fileLooksLikeMp3(std::ifstream & /*file*/) {
  return false;
}
#endif

#if defined(USE_FLAC) && USE_FLAC
bool fileLooksLikeFlac(std::ifstream &file) {
  std::array<char, 4> marker{};
  file.read(marker.data(), static_cast<std::streamsize>(marker.size()));
  if (file.gcount() < static_cast<std::streamsize>(marker.size())) {
    return false;
  }
  return std::memcmp(marker.data(), "fLaC", 4) == 0;
}
#else
bool fileLooksLikeFlac(std::ifstream & /*file*/) {
  return false;
}
#endif

AudioFileFormat detectFormatFromContent(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return AudioFileFormat::PCM;
  }

  auto resetStream = [&file]() {
    file.clear();
    file.seekg(0, std::ios::beg);
  };

  if (fileLooksLikeAac(file)) {
    return AudioFileFormat::AAC;
  }

  resetStream();
  if (fileLooksLikeFlac(file)) {
    return AudioFileFormat::FLAC;
  }

  resetStream();
  if (fileLooksLikeMp3(file)) {
    return AudioFileFormat::MP3;
  }

  resetStream();
  if (fileLooksLikeOpus(file)) {
    return AudioFileFormat::OPUS;
  }

  resetStream();
  if (fileLooksLikeWav(file)) {
    return AudioFileFormat::WAV;
  }

  return AudioFileFormat::PCM;
}

bool isUrl(const std::string &value) {
  auto pos = value.find("://");
  if (pos == std::string::npos) {
    return false;
  }
  std::string scheme = toLower(value.substr(0, pos));
  return scheme == "http" || scheme == "https";
}

bool bufferLooksLikeWav(const std::vector<uint8_t> &buffer) {
  return buffer.size() >= 12 && std::memcmp(buffer.data(), "RIFF", 4) == 0 &&
         std::memcmp(buffer.data() + 8, "WAVE", 4) == 0;
}

#if defined(USE_AAC) && USE_AAC
bool bufferLooksLikeAac(const std::vector<uint8_t> &buffer) {
  if (buffer.size() < 7) {
    return false;
  }
  AdtsHeader header{};
  return parseAdtsHeader(buffer.data(), buffer.size(), header);
}
#else
bool bufferLooksLikeAac(const std::vector<uint8_t> & /*buffer*/) {
  return false;
}
#endif

#if defined(USE_OPUS) && USE_OPUS
bool bufferLooksLikeOpus(const std::vector<uint8_t> &buffer) {
  if (buffer.size() < 36) {
    return false;
  }
  if (std::memcmp(buffer.data(), "OggS", 4) != 0) {
    return false;
  }
  // Skip capture pattern, header and first lacing table entry (at least 1 byte)
  for (size_t idx = 26; idx + 8 <= buffer.size(); ++idx) {
    if (std::memcmp(buffer.data() + idx, "OpusHead", 8) == 0) {
      return true;
    }
  }
  return false;
}
#else
bool bufferLooksLikeOpus(const std::vector<uint8_t> & /*buffer*/) {
  return false;
}
#endif

#if defined(USE_FLAC) && USE_FLAC
bool bufferLooksLikeFlac(const std::vector<uint8_t> &buffer) {
  return buffer.size() >= 4 && std::memcmp(buffer.data(), "fLaC", 4) == 0;
}
#else
bool bufferLooksLikeFlac(const std::vector<uint8_t> & /*buffer*/) {
  return false;
}
#endif

#if defined(USE_MP3) && USE_MP3
bool bufferLooksLikeMp3(const std::vector<uint8_t> &buffer) {
  if (buffer.size() >= 3 && std::memcmp(buffer.data(), "ID3", 3) == 0) {
    return true;
  }
  if (buffer.empty()) {
    return false;
  }
  const unsigned char *ptr = buffer.data();
  int sync = MP3FindSyncWord(const_cast<unsigned char *>(ptr),
                             static_cast<int>(buffer.size()));
  return sync >= 0;
}
#else
bool bufferLooksLikeMp3(const std::vector<uint8_t> & /*buffer*/) {
  return false;
}
#endif

std::optional<AudioFileFormat>
detectFormatFromBuffer(const std::vector<uint8_t> &buffer) {
  if (bufferLooksLikeAac(buffer)) {
    return AudioFileFormat::AAC;
  }
  if (bufferLooksLikeFlac(buffer)) {
    return AudioFileFormat::FLAC;
  }
  if (bufferLooksLikeMp3(buffer)) {
    return AudioFileFormat::MP3;
  }
  if (bufferLooksLikeOpus(buffer)) {
    return AudioFileFormat::OPUS;
  }
  if (bufferLooksLikeWav(buffer)) {
    return AudioFileFormat::WAV;
  }
  return std::nullopt;
}

bool ensureCurlInitialized();
void stopActiveStream(bool waitForStop, const char *reason);
bool startStreamingPlayback(const PlayCommandOptions &options);

uint16_t readLe16(const unsigned char *data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLe32(const unsigned char *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

#if defined(USE_FLAC) && USE_FLAC
const char *flacInitStatusName(FLAC__StreamDecoderInitStatus status) {
  switch (status) {
  case FLAC__STREAM_DECODER_INIT_STATUS_OK:
    return "ok";
  case FLAC__STREAM_DECODER_INIT_STATUS_UNSUPPORTED_CONTAINER:
    return "unsupported container";
  case FLAC__STREAM_DECODER_INIT_STATUS_INVALID_CALLBACKS:
    return "invalid callbacks";
  case FLAC__STREAM_DECODER_INIT_STATUS_MEMORY_ALLOCATION_ERROR:
    return "memory allocation error";
  case FLAC__STREAM_DECODER_INIT_STATUS_ERROR_OPENING_FILE:
    return "error opening file";
  case FLAC__STREAM_DECODER_INIT_STATUS_ALREADY_INITIALIZED:
    return "already initialized";
  default:
    return "unknown";
  }
}

const char *flacErrorStatusName(FLAC__StreamDecoderErrorStatus status) {
  switch (status) {
  case FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC:
    return "lost sync";
  case FLAC__STREAM_DECODER_ERROR_STATUS_BAD_HEADER:
    return "bad header";
  case FLAC__STREAM_DECODER_ERROR_STATUS_FRAME_CRC_MISMATCH:
    return "frame crc mismatch";
  case FLAC__STREAM_DECODER_ERROR_STATUS_UNPARSEABLE_STREAM:
    return "unparseable stream";
  default:
    return "unknown";
  }
}

struct FlacDecodeContext {
  std::vector<int16_t> *samples{nullptr};
  bool decodedAny{false};
  int sampleRate{0};
  std::ifstream *file{nullptr};
};

FLAC__StreamDecoderWriteStatus
flacWriteCallback(const FLAC__StreamDecoder * /*decoder*/,
                  const FLAC__Frame *frame, const FLAC__int32 *const buffer[],
                  void *clientData) {
  auto *ctx = static_cast<FlacDecodeContext *>(clientData);
  if (!ctx || !ctx->samples || !frame || !buffer) {
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  const unsigned channels = std::max<uint32_t>(frame->header.channels, 1);
  const unsigned blockSize = frame->header.blocksize;
  if (ctx->sampleRate == 0 && frame->header.sample_rate > 0) {
    ctx->sampleRate = static_cast<int>(frame->header.sample_rate);
  }

  ctx->samples->reserve(ctx->samples->size() + blockSize);
  for (unsigned i = 0; i < blockSize; ++i) {
    int64_t sum = 0;
    for (unsigned ch = 0; ch < channels; ++ch) {
      sum += buffer[ch][i];
    }
    sum /= static_cast<int64_t>(channels);
    if (sum > INT16_MAX) {
      sum = INT16_MAX;
    } else if (sum < INT16_MIN) {
      sum = INT16_MIN;
    }
    ctx->samples->push_back(static_cast<int16_t>(sum));
  }

  ctx->decodedAny = true;
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void flacMetadataCallback(const FLAC__StreamDecoder * /*decoder*/,
                          const FLAC__StreamMetadata *metadata,
                          void *clientData) {
  auto *ctx = static_cast<FlacDecodeContext *>(clientData);
  if (!ctx || !metadata) {
    return;
  }
  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO &&
      ctx->sampleRate == 0 && metadata->data.stream_info.sample_rate > 0) {
    ctx->sampleRate = static_cast<int>(metadata->data.stream_info.sample_rate);
  }
}

void flacErrorCallback(const FLAC__StreamDecoder * /*decoder*/,
                       FLAC__StreamDecoderErrorStatus status,
                       void * /*clientData*/) {
  LOG_WARN("AudioOutputControl: FLAC decoder error: "
           << flacErrorStatusName(status));
}

FLAC__StreamDecoderReadStatus
flacReadCallback(const FLAC__StreamDecoder * /*decoder*/, FLAC__byte buffer[],
                 size_t *bytes, void *clientData) {
  auto *ctx = static_cast<FlacDecodeContext *>(clientData);
  if (!ctx || !ctx->file || !bytes) {
    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
  }
  if (*bytes == 0) {
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
  }

  ctx->file->read(reinterpret_cast<char *>(buffer),
                  static_cast<std::streamsize>(*bytes));
  std::streamsize readBytes = ctx->file->gcount();
  if (readBytes <= 0) {
    if (ctx->file->eof()) {
      *bytes = 0;
      return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    ctx->file->clear();
    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
  }

  *bytes = static_cast<size_t>(readBytes);
  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderSeekStatus
flacSeekCallback(const FLAC__StreamDecoder * /*decoder*/,
                 FLAC__uint64 absoluteByteOffset, void *clientData) {
  auto *ctx = static_cast<FlacDecodeContext *>(clientData);
  if (!ctx || !ctx->file) {
    return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
  }

  ctx->file->clear();
  ctx->file->seekg(static_cast<std::streamoff>(absoluteByteOffset),
                   std::ios::beg);
  if (!(*ctx->file)) {
    ctx->file->clear();
    return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
  }
  return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

FLAC__StreamDecoderTellStatus
flacTellCallback(const FLAC__StreamDecoder * /*decoder*/,
                 FLAC__uint64 *absoluteByteOffset, void *clientData) {
  auto *ctx = static_cast<FlacDecodeContext *>(clientData);
  if (!ctx || !ctx->file || !absoluteByteOffset) {
    return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
  }
  auto pos = ctx->file->tellg();
  if (pos < 0) {
    return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
  }
  *absoluteByteOffset = static_cast<FLAC__uint64>(pos);
  return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderLengthStatus
flacLengthCallback(const FLAC__StreamDecoder * /*decoder*/,
                   FLAC__uint64 *streamLength, void *clientData) {
  auto *ctx = static_cast<FlacDecodeContext *>(clientData);
  if (!ctx || !ctx->file || !streamLength) {
    return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
  }

  auto current = ctx->file->tellg();
  ctx->file->seekg(0, std::ios::end);
  auto end = ctx->file->tellg();
  if (end < 0) {
    ctx->file->clear();
    ctx->file->seekg(current, std::ios::beg);
    return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
  }
  *streamLength = static_cast<FLAC__uint64>(end);
  ctx->file->seekg(current, std::ios::beg);
  return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

FLAC__bool flacEofCallback(const FLAC__StreamDecoder * /*decoder*/,
                           void *clientData) {
  auto *ctx = static_cast<FlacDecodeContext *>(clientData);
  if (!ctx || !ctx->file) {
    return true;
  }
  return ctx->file->eof();
}

bool decodeFlacFile(const std::string &path, std::vector<int16_t> &samples,
                    int &sampleRate) {
  FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
  if (!decoder) {
    LOG_ERROR("AudioOutputControl: failed to create FLAC decoder");
    return false;
  }
  struct FlacDecoderGuard {
    FLAC__StreamDecoder *handle;
    bool initialized{false};
    explicit FlacDecoderGuard(FLAC__StreamDecoder *h) : handle(h) {
    }
    ~FlacDecoderGuard() {
      if (handle) {
        if (initialized) {
          FLAC__stream_decoder_finish(handle);
        }
        FLAC__stream_decoder_delete(handle);
      }
    }
  } decoderGuard(decoder);

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    LOG_ERROR("AudioOutputControl: failed to open FLAC file '" << path << "'");
    return false;
  }

  FlacDecodeContext ctx{&samples};
  ctx.file = &file;
  auto initStatus = FLAC__stream_decoder_init_stream(
      decoder, flacReadCallback, flacSeekCallback, flacTellCallback,
      flacLengthCallback, flacEofCallback, flacWriteCallback,
      flacMetadataCallback, flacErrorCallback, &ctx);
  if (initStatus != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
    LOG_ERROR("AudioOutputControl: FLAC init failed for '"
              << path << "': " << flacInitStatusName(initStatus));
    return false;
  }
  decoderGuard.initialized = true;

  if (!FLAC__stream_decoder_process_until_end_of_stream(decoder)) {
    LOG_ERROR("AudioOutputControl: FLAC decoding aborted for '" << path << "'");
    return false;
  }

  if (!ctx.decodedAny) {
    LOG_WARN("AudioOutputControl: no FLAC audio frames decoded from '" << path
                                                                       << "'");
    return false;
  }

  if (ctx.sampleRate > 0) {
    sampleRate = ctx.sampleRate;
  } else if (sampleRate == 0) {
    sampleRate = defaultSampleRate();
  }

  return true;
}
#else
bool decodeFlacFile(const std::string &path, std::vector<int16_t> & /*samples*/,
                    int & /*sampleRate*/) {
  LOG_ERROR("AudioOutputControl: FLAC support is disabled at build time ("
            << path << ")");
  return false;
}
#endif

#if defined(USE_OPUS) && USE_OPUS
bool decodeOpusFile(const std::string &path, std::vector<int16_t> &samples,
                    int &sampleRate) {
  constexpr int kOpusSampleRate = 48000;
  constexpr int kMaxOpusChannels = 2;
  constexpr int kMaxFrameSize = 5760; // 120 ms at 48 kHz

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    LOG_ERROR("AudioOutputControl: failed to open Opus file '" << path << "'");
    return false;
  }

  std::vector<uint8_t> currentPacket;
  bool haveStreamSerial = false;
  uint32_t streamSerial = 0;
  int packetIndex = 0;
  OpusDecoder *decoder = nullptr;
  int opusChannels = 0;
  int samplesToDiscard = 0;
  bool decodedAny = false;
  std::array<opus_int16, kMaxFrameSize * kMaxOpusChannels> decodeBuffer{};

  auto destroyDecoder = [&]() {
    if (decoder) {
      opus_decoder_destroy(decoder);
      decoder = nullptr;
    }
  };

  auto processPacket = [&](const std::vector<uint8_t> &packet) -> bool {
    if (packet.empty()) {
      return true;
    }

    if (packetIndex == 0) {
      if (packet.size() < 19 ||
          std::memcmp(packet.data(), "OpusHead", 8) != 0) {
        LOG_ERROR("AudioOutputControl: invalid Opus ID header in '" << path
                                                                    << "'");
        return false;
      }
      opusChannels = packet[9];
      if (opusChannels <= 0 || opusChannels > kMaxOpusChannels) {
        LOG_ERROR("AudioOutputControl: unsupported Opus channel count "
                  << opusChannels << " in '" << path << "'");
        return false;
      }

      uint16_t preSkip = readLe16(packet.data() + 10);
      samplesToDiscard = preSkip;

      int opusError = 0;
      decoder = opus_decoder_create(kOpusSampleRate, opusChannels, &opusError);
      if (opusError != OPUS_OK || !decoder) {
        LOG_ERROR("AudioOutputControl: failed to create Opus decoder: "
                  << opus_strerror(opusError));
        return false;
      }

      sampleRate = kOpusSampleRate;
    } else if (packetIndex == 1) {
      if (packet.size() < 8 || std::memcmp(packet.data(), "OpusTags", 8) != 0) {
        LOG_WARN("AudioOutputControl: unexpected Opus comment packet in '"
                 << path << "'");
      }
    } else {
      if (!decoder) {
        LOG_ERROR("AudioOutputControl: decoder not initialized before audio "
                  "packets in '"
                  << path << "'");
        return false;
      }

      int decodedSamples = opus_decode(decoder, packet.data(),
                                       static_cast<opus_int32>(packet.size()),
                                       decodeBuffer.data(), kMaxFrameSize, 0);
      if (decodedSamples < 0) {
        LOG_ERROR("AudioOutputControl: opus_decode failed: "
                  << opus_strerror(decodedSamples));
        return false;
      }

      if (decodedSamples > 0) {
        int frames = decodedSamples;
        for (int i = 0; i < frames; ++i) {
          if (samplesToDiscard > 0) {
            --samplesToDiscard;
            continue;
          }

          int sum = 0;
          for (int ch = 0; ch < opusChannels; ++ch) {
            sum += decodeBuffer[static_cast<size_t>(i) * opusChannels + ch];
          }
          samples.push_back(static_cast<int16_t>(sum / opusChannels));
        }

        decodedAny = true;
      }
    }

    ++packetIndex;
    return true;
  };

  while (file) {
    char capture[4];
    file.read(capture, sizeof(capture));
    if (!file) {
      break;
    }
    if (std::strncmp(capture, "OggS", 4) != 0) {
      LOG_ERROR("AudioOutputControl: invalid Ogg capture pattern in '" << path
                                                                       << "'");
      destroyDecoder();
      return false;
    }

    std::array<unsigned char, 23> header{};
    if (!file.read(reinterpret_cast<char *>(header.data()), header.size())) {
      LOG_ERROR("AudioOutputControl: truncated Ogg page header in '" << path
                                                                     << "'");
      destroyDecoder();
      return false;
    }

    uint8_t version = header[0];
    if (version != 0) {
      LOG_ERROR("AudioOutputControl: unsupported Ogg version in '" << path
                                                                   << "'");
      destroyDecoder();
      return false;
    }

    uint8_t headerType = header[1];
    (void)headerType;
    uint32_t serial = readLe32(header.data() + 10);
    uint8_t pageSegments = header[22];

    if (!haveStreamSerial) {
      if ((headerType & 0x02) == 0) {
        LOG_ERROR("AudioOutputControl: first Ogg page missing BOS flag in '"
                  << path << "'");
        destroyDecoder();
        return false;
      }
      streamSerial = serial;
      haveStreamSerial = true;
    } else if (serial != streamSerial) {
      LOG_ERROR(
          "AudioOutputControl: multiple logical streams not supported in '"
          << path << "'");
      destroyDecoder();
      return false;
    }

    std::vector<uint8_t> lacing(pageSegments);
    if (pageSegments > 0 &&
        !file.read(reinterpret_cast<char *>(lacing.data()), pageSegments)) {
      LOG_ERROR("AudioOutputControl: truncated lacing table in '" << path
                                                                  << "'");
      destroyDecoder();
      return false;
    }

    size_t payloadSize = 0;
    for (uint8_t segLen : lacing) {
      payloadSize += segLen;
    }

    std::vector<uint8_t> payload(payloadSize);
    if (payloadSize > 0 &&
        !file.read(reinterpret_cast<char *>(payload.data()), payloadSize)) {
      LOG_ERROR("AudioOutputControl: truncated page payload in '" << path
                                                                  << "'");
      destroyDecoder();
      return false;
    }

    size_t payloadOffset = 0;
    for (uint8_t segLen : lacing) {
      if (payloadOffset + segLen > payload.size()) {
        LOG_ERROR("AudioOutputControl: invalid segment length in '" << path
                                                                    << "'");
        destroyDecoder();
        return false;
      }

      currentPacket.insert(
          currentPacket.end(),
          payload.begin() + static_cast<std::ptrdiff_t>(payloadOffset),
          payload.begin() +
              static_cast<std::ptrdiff_t>(payloadOffset + segLen));
      payloadOffset += segLen;

      if (segLen < 255) {
        if (!processPacket(currentPacket)) {
          destroyDecoder();
          return false;
        }
        currentPacket.clear();
      }
    }
  }

  if (!currentPacket.empty()) {
    LOG_WARN("AudioOutputControl: leftover partial Opus packet ignored for '"
             << path << "'");
  }

  destroyDecoder();

  if (!decodedAny) {
    LOG_WARN("AudioOutputControl: no audio payload decoded from Opus file '"
             << path << "'");
    return false;
  }

  if (sampleRate == 0) {
    sampleRate = kOpusSampleRate;
  }

  return true;
}
#else
bool decodeOpusFile(const std::string &path, std::vector<int16_t> & /*samples*/,
                    int & /*sampleRate*/) {
  LOG_ERROR("AudioOutputControl: Opus support is disabled at build time ("
            << path << ")");
  return false;
}
#endif

std::vector<int16_t> resampleLinear(const std::vector<int16_t> &input,
                                    int inputRate, int outputRate) {
  if (inputRate == outputRate || input.empty()) {
    return input;
  }

  double ratio =
      static_cast<double>(outputRate) / static_cast<double>(inputRate);
  size_t outputSize = static_cast<size_t>(
      std::max(1.0, std::round(static_cast<double>(input.size()) * ratio)));
  std::vector<int16_t> output(outputSize);
  for (size_t i = 0; i < outputSize; ++i) {
    double inputPos = static_cast<double>(i) / ratio;
    size_t baseIndex = static_cast<size_t>(inputPos);
    if (baseIndex >= input.size()) {
      baseIndex = input.size() - 1;
    }
    size_t nextIndex = std::min(baseIndex + 1, input.size() - 1);
    double factor = inputPos - static_cast<double>(baseIndex);
    double interpolated =
        static_cast<double>(input[baseIndex]) * (1.0 - factor) +
        static_cast<double>(input[nextIndex]) * factor;
    if (interpolated > INT16_MAX) {
      interpolated = INT16_MAX;
    } else if (interpolated < INT16_MIN) {
      interpolated = INT16_MIN;
    }
    output[i] = static_cast<int16_t>(interpolated);
  }
  return output;
}

constexpr size_t kMaxStreamingSniffBytes = 65536;
constexpr auto kStreamingTailSilence = std::chrono::milliseconds(40);

class StreamingLinearResampler {
public:
  StreamingLinearResampler(int inputRate, int outputRate) {
    reset(inputRate, outputRate);
  }

  void reset(int inputRate, int outputRate) {
    inputRate_ = inputRate;
    outputRate_ = outputRate;
    if (inputRate_ <= 0 || outputRate_ <= 0) {
      increment_ = 1.0;
    } else {
      increment_ =
          static_cast<double>(inputRate_) / static_cast<double>(outputRate_);
    }
    nextOutputTime_ = 0.0;
    processedSamples_ = 0;
    hasLastSample_ = false;
    lastSample_ = 0;
  }

  void process(const int16_t *samples, size_t count,
               std::vector<int16_t> &output) {
    if (!samples || count == 0 || inputRate_ <= 0 || outputRate_ <= 0) {
      return;
    }
    if (inputRate_ == outputRate_) {
      output.insert(output.end(), samples, samples + count);
      processedSamples_ += static_cast<int64_t>(count);
      if (count > 0) {
        lastSample_ = samples[count - 1];
        hasLastSample_ = true;
      }
      return;
    }

    for (size_t i = 0; i < count; ++i) {
      const int16_t current = samples[i];
      if (!hasLastSample_) {
        lastSample_ = current;
        hasLastSample_ = true;
        processedSamples_ = 1;
        while (nextOutputTime_ < static_cast<double>(processedSamples_)) {
          output.push_back(current);
          nextOutputTime_ += increment_;
        }
        continue;
      }

      double segmentStart = static_cast<double>(processedSamples_ - 1);
      double segmentEnd = static_cast<double>(processedSamples_);
      while (nextOutputTime_ <= segmentEnd) {
        double frac =
            (segmentEnd - segmentStart) > 0.0
                ? (nextOutputTime_ - segmentStart) / (segmentEnd - segmentStart)
                : 0.0;
        double interpolated =
            static_cast<double>(lastSample_) +
            (static_cast<double>(current) - static_cast<double>(lastSample_)) *
                frac;
        if (interpolated > INT16_MAX) {
          interpolated = INT16_MAX;
        } else if (interpolated < INT16_MIN) {
          interpolated = INT16_MIN;
        }
        output.push_back(static_cast<int16_t>(interpolated));
        nextOutputTime_ += increment_;
      }

      lastSample_ = current;
      ++processedSamples_;
    }
  }

  void flush(std::vector<int16_t> &output) {
    if (!hasLastSample_ || inputRate_ <= 0 || outputRate_ <= 0) {
      return;
    }
    if (inputRate_ == outputRate_) {
      output.push_back(lastSample_);
      return;
    }

    double segmentEnd = static_cast<double>(processedSamples_);
    while (nextOutputTime_ <= segmentEnd) {
      output.push_back(lastSample_);
      nextOutputTime_ += increment_;
    }
  }

private:
  int inputRate_{0};
  int outputRate_{0};
  double increment_{1.0};
  double nextOutputTime_{0.0};
  int64_t processedSamples_{0};
  bool hasLastSample_{false};
  int16_t lastSample_{0};
};

class AudioStreamSink {
public:
  AudioStreamSink(const PlayCommandOptions &options,
                  std::atomic<bool> &stopFlag)
      : options_(options), stopRequested_(stopFlag),
        targetRate_(effectiveOutputSampleRate()) {
  }

  bool prepare() {
    if (!cfg || !cfg->audio.output_enabled) {
      LOG_WARN("AudioOutputControl: STREAM ignored because audio output is "
               "disabled");
      return false;
    }
    if (!global_audio_output) {
      LOG_ERROR("AudioOutputControl: STREAM ignored because audio output "
                "stream is not available");
      return false;
    }

    pendingVolume_ = options_.setVolume;
    pendingGain_ = options_.setGain;

    if (!options_.append) {
      AudioOutputWorker::clearQueue(true);
    }

    if ((pendingVolume_ || pendingGain_) &&
        AudioOutputWorker::applyVolumeGain(pendingVolume_, options_.volume,
                                           pendingGain_, options_.gain)) {
      pendingVolume_ = false;
      pendingGain_ = false;
    }

    return true;
  }

  bool pushSamples(const int16_t *samples, size_t count, int sourceRate) {
    if (!samples || count == 0) {
      return true;
    }
    if (shouldStop()) {
      return false;
    }

    int actualSourceRate = (sourceRate > 0) ? sourceRate : targetRate_;

    // On first push with a known source rate that differs from the current
    // AO rate, try to reconfigure AO to avoid resampling artefacts on
    // platforms where the DAC may not support the configured rate.
    // Skip on platforms with a shared AI/AO CODEC clock (T10/T20/T21/T30):
    // changing the AO clock also shifts the AI capture rate, causing the
    // microphone stream to play back at the wrong pitch/speed.
    if (!reconfigureAttempted_ && actualSourceRate != targetRate_ &&
        !options_.append && !hal::caps().has_shared_audio_clock) {
      reconfigureAttempted_ = true;
      if (AudioOutputWorker::reconfigureRate(actualSourceRate)) {
        targetRate_ = effectiveOutputSampleRate();
        reconfigured_ = true;
        resampler_.reset();
        currentSourceRate_ = 0;
        LOG_DEBUG("AudioStreamSink: reconfigured AO to " << targetRate_
                                                         << " Hz");
      }
    }

    convertBuffer_.clear();

    if (actualSourceRate != targetRate_) {
      if (!resampler_ || currentSourceRate_ != actualSourceRate) {
        resampler_ = std::make_unique<StreamingLinearResampler>(
            actualSourceRate, targetRate_);
        currentSourceRate_ = actualSourceRate;
      }
      resampler_->process(samples, count, convertBuffer_);
    } else {
      convertBuffer_.insert(convertBuffer_.end(), samples, samples + count);
    }

    if (convertBuffer_.empty()) {
      return true;
    }

    std::vector<int16_t> chunk;
    chunk.swap(convertBuffer_);
    return enqueueChunk(std::move(chunk));
  }

  bool pushSamples(const std::vector<int16_t> &samples, int sourceRate) {
    if (samples.empty()) {
      return true;
    }
    return pushSamples(samples.data(), samples.size(), sourceRate);
  }

  bool finish() {
    if (resampler_) {
      convertBuffer_.clear();
      resampler_->flush(convertBuffer_);
      if (!convertBuffer_.empty()) {
        std::vector<int16_t> tail;
        tail.swap(convertBuffer_);
        if (!enqueueChunk(std::move(tail))) {
          return false;
        }
      }
    }

    if (pendingVolume_ || pendingGain_) {
      AudioOutputWorker::enqueuePcm(std::vector<int16_t>{}, pendingVolume_,
                                    options_.volume, pendingGain_,
                                    options_.gain);
      pendingVolume_ = false;
      pendingGain_ = false;
    }

    if (!options_.append) {
      AudioOutputWorker::waitForPlaybackCompletion(std::chrono::milliseconds(0),
                                                   true, kStreamingTailSilence);

      // Restore AO to the configured default rate after streaming ends.
      if (reconfigured_) {
        int defaultRate = defaultSampleRate();
        if (effectiveOutputSampleRate() != defaultRate) {
          AudioOutputWorker::reconfigureRate(defaultRate);
        }
      }
    }

    return true;
  }

  bool shouldStop() const {
    return stopRequested_.load(std::memory_order_relaxed) ||
           global_shutdown_requested.load(std::memory_order_relaxed);
  }

private:
  bool enqueueChunk(std::vector<int16_t> &&chunk) {
    if (chunk.empty()) {
      return true;
    }
    if (shouldStop()) {
      return false;
    }
    if (!AudioOutputWorker::enqueuePcmBlocking(std::move(chunk), pendingVolume_,
                                               options_.volume, pendingGain_,
                                               options_.gain)) {
      return false;
    }
    pendingVolume_ = false;
    pendingGain_ = false;
    return true;
  }

  const PlayCommandOptions &options_;
  std::atomic<bool> &stopRequested_;
  int targetRate_{0};
  bool pendingVolume_{false};
  bool pendingGain_{false};
  bool reconfigureAttempted_{false};
  bool reconfigured_{false};
  std::unique_ptr<StreamingLinearResampler> resampler_;
  int currentSourceRate_{0};
  std::vector<int16_t> convertBuffer_;
};

class StreamingDecoder {
public:
  explicit StreamingDecoder(AudioStreamSink &sink) : sink_(sink) {
  }
  virtual ~StreamingDecoder() = default;

  virtual bool feed(const uint8_t *data, size_t size, bool endOfStream) = 0;
  virtual bool finalize() = 0;

protected:
  AudioStreamSink &sink_;
};

class StreamingPcmDecoder : public StreamingDecoder {
public:
  StreamingPcmDecoder(AudioStreamSink &sink, int sampleRate)
      : StreamingDecoder(sink) {
    sampleRate_ = (sampleRate > 0) ? sampleRate : defaultSampleRate();
  }

  bool feed(const uint8_t *data, size_t size, bool endOfStream) override {
    if (data && size > 0) {
      buffer_.insert(buffer_.end(), data, data + size);
    }

    if (!drainAvailable()) {
      return false;
    }

    if (endOfStream && !buffer_.empty()) {
      LOG_WARN("AudioOutputControl: PCM stream contained trailing partial "
               "sample, dropping byte");
      buffer_.clear();
      readOffset_ = 0;
    }

    return true;
  }

  bool finalize() override {
    return feed(nullptr, 0, true);
  }

private:
  static constexpr size_t kMaxSamplesPerChunk = 2048;

  bool drainAvailable() {
    constexpr size_t kSampleBytes = sizeof(int16_t);
    bool ok = true;
    while (ok) {
      size_t bytesAvailable = buffer_.size() - readOffset_;
      if (bytesAvailable < kSampleBytes) {
        break;
      }
      size_t samplesAvailable = bytesAvailable / kSampleBytes;
      size_t emitSamples = std::min(samplesAvailable, kMaxSamplesPerChunk);
      decode_.resize(emitSamples);
      const uint8_t *ptr = buffer_.data() + readOffset_;
      for (size_t i = 0; i < emitSamples; ++i) {
        int16_t sample =
            static_cast<int16_t>(ptr[0] | (static_cast<int16_t>(ptr[1]) << 8));
        decode_[i] = sample;
        ptr += kSampleBytes;
      }
      readOffset_ += emitSamples * kSampleBytes;
      ok = sink_.pushSamples(decode_, sampleRate_);
    }

    if (readOffset_ > 0) {
      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<std::ptrdiff_t>(readOffset_));
      readOffset_ = 0;
    }
    return ok;
  }

  int sampleRate_{0};
  std::vector<uint8_t> buffer_;
  size_t readOffset_{0};
  std::vector<int16_t> decode_;
};

#if defined(USE_AAC) && USE_AAC
class StreamingAacDecoder : public StreamingDecoder {
public:
  explicit StreamingAacDecoder(AudioStreamSink &sink) : StreamingDecoder(sink) {
    decoder_ = AACInitDecoder();
    if (!decoder_) {
      LOG_ERROR("AudioOutputControl: failed to initialize AAC decoder");
    }
  }

  ~StreamingAacDecoder() override {
    if (decoder_) {
      AACFreeDecoder(decoder_);
      decoder_ = nullptr;
    }
  }

  bool feed(const uint8_t *data, size_t size, bool endOfStream) override {
    if (data && size > 0) {
      buffer_.insert(buffer_.end(), data, data + size);
    }

    if (!decoder_) {
      return false;
    }

    if (!drainFrames(endOfStream)) {
      return false;
    }

    if (endOfStream && !buffer_.empty()) {
      LOG_WARN("AudioOutputControl: leftover AAC bytes after end-of-stream, "
               "dropping "
               << buffer_.size() << " byte(s)");
      buffer_.clear();
    }

    return true;
  }

  bool finalize() override {
    return feed(nullptr, 0, true);
  }

private:
  bool drainFrames(bool allowTruncate) {
    constexpr size_t kHeaderSize = 7;
    bool progress = true;
    while (progress) {
      progress = false;
      if (buffer_.size() < kHeaderSize) {
        break;
      }

      AdtsHeader header{};
      if (!parseAdtsHeader(buffer_.data(), buffer_.size(), header)) {
        buffer_.erase(buffer_.begin());
        continue;
      }

      if (buffer_.size() < static_cast<size_t>(header.frameLength)) {
        if (allowTruncate) {
          buffer_.clear();
        }
        break;
      }

      if (!configured_ && !configureDecoderForHeader(decoder_, header)) {
        return false;
      }
      configured_ = true;

      const size_t payloadOffset = header.headerSize;
      if (header.frameLength <= header.headerSize) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + header.frameLength);
        continue;
      }

      unsigned char *framePtr = buffer_.data() + payloadOffset;
      int bytesLeft = header.frameLength - header.headerSize;
      int decodeStatus =
          AACDecode(decoder_, &framePtr, &bytesLeft, decodeBuffer_.data());
      if (decodeStatus != 0 && decodeStatus != ERR_AAC_INDATA_UNDERFLOW) {
        LOG_WARN("AudioOutputControl: AACDecode error " << decodeStatus
                                                        << ", resyncing");
        buffer_.erase(buffer_.begin());
        continue;
      }

      AACFrameInfo info{};
      AACGetLastFrameInfo(decoder_, &info);
      if (info.outputSamps <= 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + header.frameLength);
        continue;
      }

      if (sampleRate_ == 0) {
        sampleRate_ =
            (info.sampRateOut > 0) ? info.sampRateOut : header.sampleRate;
        if (sampleRate_ <= 0) {
          sampleRate_ = defaultSampleRate();
        }
      }

      decoded_.clear();
      const int channels = std::max(info.nChans, 1);
      if (channels == 1) {
        decoded_.insert(decoded_.end(), decodeBuffer_.begin(),
                        decodeBuffer_.begin() + info.outputSamps);
      } else {
        const int frames = info.outputSamps / channels;
        decoded_.reserve(frames);
        for (int i = 0; i < frames; ++i) {
          int sum = 0;
          for (int ch = 0; ch < channels; ++ch) {
            sum += decodeBuffer_[static_cast<size_t>(i) * channels + ch];
          }
          decoded_.push_back(static_cast<int16_t>(sum / channels));
        }
      }

      if (!decoded_.empty() && !sink_.pushSamples(decoded_, sampleRate_)) {
        return false;
      }

      buffer_.erase(buffer_.begin(), buffer_.begin() + header.frameLength);
      progress = true;
    }
    return true;
  }

  HAACDecoder decoder_{nullptr};
  bool configured_{false};
  int sampleRate_{0};
  std::vector<uint8_t> buffer_;
  std::vector<int16_t> decoded_;
  static constexpr size_t kMaxAacSamples = 8192;
  std::array<int16_t, kMaxAacSamples> decodeBuffer_{};
};
#endif

#if defined(USE_MP3) && USE_MP3
class StreamingMp3Decoder : public StreamingDecoder {
public:
  explicit StreamingMp3Decoder(AudioStreamSink &sink)
      : StreamingDecoder(sink), decoder_(MP3InitDecoder()) {
    if (!decoder_) {
      LOG_ERROR("AudioOutputControl: failed to initialize MP3 decoder");
    }
  }

  ~StreamingMp3Decoder() override {
    if (decoder_) {
      MP3FreeDecoder(decoder_);
      decoder_ = nullptr;
    }
  }

  bool feed(const uint8_t *data, size_t size, bool endOfStream) override {
    if (data && size > 0) {
      buffer_.insert(buffer_.end(), data, data + size);
    }
    if (!decoder_) {
      return false;
    }

    bool progress = true;
    while (progress && !sink_.shouldStop()) {
      progress = false;
      if (buffer_.size() - readOffset_ < 4) {
        break;
      }

      int bytesLeft = static_cast<int>(buffer_.size() - readOffset_);
      unsigned char *searchPtr = buffer_.data() + readOffset_;
      int offset = MP3FindSyncWord(searchPtr, bytesLeft);
      if (offset < 0) {
        if (endOfStream) {
          buffer_.clear();
          readOffset_ = 0;
        }
        break;
      }
      readOffset_ += static_cast<size_t>(offset);

      unsigned char *framePtr = buffer_.data() + readOffset_;
      int frameBytesLeft = static_cast<int>(buffer_.size() - readOffset_);
      int err = MP3Decode(decoder_, &framePtr, &frameBytesLeft,
                          frameBuffer_.data(), 0);
      if (err == ERR_MP3_INDATA_UNDERFLOW) {
        break;
      }
      if (err != ERR_MP3_NONE) {
        LOG_WARN("AudioOutputControl: MP3Decode error " << err
                                                        << ", resyncing");
        ++readOffset_;
        continue;
      }

      MP3FrameInfo frameInfo{};
      MP3GetLastFrameInfo(decoder_, &frameInfo);
      if (frameInfo.outputSamps <= 0) {
        readOffset_ = static_cast<size_t>(framePtr - buffer_.data());
        progress = true;
        continue;
      }

      if (sampleRate_ == 0 && frameInfo.samprate > 0) {
        sampleRate_ = frameInfo.samprate;
      }

      int channels = std::max(frameInfo.nChans, 1);
      size_t totalSamples =
          static_cast<size_t>(std::max(frameInfo.outputSamps, 0));
      decoded_.clear();
      if (channels == 1) {
        decoded_.insert(decoded_.end(), frameBuffer_.begin(),
                        frameBuffer_.begin() + totalSamples);
      } else {
        decoded_.reserve(totalSamples / channels);
        for (size_t i = 0; i + static_cast<size_t>(channels) <= totalSamples;
             i += static_cast<size_t>(channels)) {
          int32_t sum = 0;
          for (int ch = 0; ch < channels; ++ch) {
            sum += frameBuffer_[i + static_cast<size_t>(ch)];
          }
          decoded_.push_back(static_cast<int16_t>(sum / channels));
        }
      }

      if (!decoded_.empty()) {
        if (!sink_.pushSamples(decoded_, sampleRate_)) {
          return false;
        }
        decoded_.clear();
      }

      readOffset_ = static_cast<size_t>(framePtr - buffer_.data());
      progress = true;
    }

    if (readOffset_ > 0 && readOffset_ <= buffer_.size()) {
      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<std::ptrdiff_t>(readOffset_));
      readOffset_ = 0;
    }

    if (endOfStream) {
      buffer_.clear();
      readOffset_ = 0;
    }

    return true;
  }

  bool finalize() override {
    return feed(nullptr, 0, true);
  }

private:
  HMP3Decoder decoder_{nullptr};
  std::vector<uint8_t> buffer_;
  std::vector<int16_t> decoded_;
  std::array<int16_t, MAX_NCHAN * MAX_NSAMP * MAX_NGRAN> frameBuffer_{};
  size_t readOffset_{0};
  int sampleRate_{0};
};

#endif

#if defined(USE_OPUS) && USE_OPUS
class StreamingOpusDecoder : public StreamingDecoder {
public:
  explicit StreamingOpusDecoder(AudioStreamSink &sink)
      : StreamingDecoder(sink) {
  }

  ~StreamingOpusDecoder() override {
    destroyDecoder();
  }

  bool feed(const uint8_t *data, size_t size, bool endOfStream) override {
    if (data && size > 0) {
      buffer_.insert(buffer_.end(), data, data + size);
    }

    if (!drainPages(endOfStream)) {
      return false;
    }

    if (endOfStream && !currentPacket_.empty()) {
      LOG_WARN("AudioOutputControl: dropping partial Opus packet");
      currentPacket_.clear();
    }
    if (endOfStream && !buffer_.empty()) {
      LOG_WARN("AudioOutputControl: leftover Opus bytes after end-of-stream");
      buffer_.clear();
    }

    return true;
  }

  bool finalize() override {
    return feed(nullptr, 0, true);
  }

private:
  static constexpr int kOpusSampleRate = 48000;
  static constexpr int kMaxOpusChannels = 2;
  static constexpr int kMaxFrameSize = 5760; // 120 ms @ 48 kHz

  void destroyDecoder() {
    if (decoder_) {
      opus_decoder_destroy(decoder_);
      decoder_ = nullptr;
    }
  }

  bool drainPages(bool allowTruncate) {
    constexpr size_t kOggHeaderBytes = 27;
    while (!sink_.shouldStop()) {
      if (buffer_.size() < kOggHeaderBytes) {
        break;
      }

      if (std::memcmp(buffer_.data(), "OggS", 4) != 0) {
        buffer_.erase(buffer_.begin());
        continue;
      }

      uint8_t version = buffer_[4];
      if (version != 0) {
        buffer_.erase(buffer_.begin());
        continue;
      }

      uint8_t headerType = buffer_[5];
      uint32_t serial = readLe32(buffer_.data() + 14);
      uint8_t pageSegments = buffer_[26];
      size_t headerSize = kOggHeaderBytes + pageSegments;
      if (buffer_.size() < headerSize) {
        break;
      }

      size_t payloadSize = 0;
      const uint8_t *lacing = buffer_.data() + kOggHeaderBytes;
      for (size_t i = 0; i < pageSegments; ++i) {
        payloadSize += lacing[i];
      }

      size_t totalSize = headerSize + payloadSize;
      if (buffer_.size() < totalSize) {
        break;
      }

      if (!haveStreamSerial_) {
        if ((headerType & 0x02) == 0) {
          LOG_WARN("AudioOutputControl: skipping Opus page without BOS flag");
          buffer_.erase(buffer_.begin(),
                        buffer_.begin() +
                            static_cast<std::ptrdiff_t>(totalSize));
          continue;
        }
        haveStreamSerial_ = true;
        streamSerial_ = serial;
      } else if (serial != streamSerial_) {
        LOG_WARN("AudioOutputControl: ignoring unexpected Opus stream serial");
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(totalSize));
        continue;
      }

      const uint8_t *payload = buffer_.data() + headerSize;
      size_t payloadOffset = 0;
      bool pageValid = true;
      for (size_t i = 0; i < pageSegments; ++i) {
        size_t segLen = lacing[i];
        if (payloadOffset + segLen > payloadSize) {
          LOG_WARN("AudioOutputControl: invalid Opus lacing length");
          pageValid = false;
          break;
        }

        currentPacket_.insert(currentPacket_.end(), payload + payloadOffset,
                              payload + payloadOffset + segLen);
        payloadOffset += segLen;

        if (segLen < 255) {
          if (!processPacket(currentPacket_)) {
            return false;
          }
          currentPacket_.clear();
        }
      }

      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<std::ptrdiff_t>(totalSize));
      if (!pageValid) {
        currentPacket_.clear();
        continue;
      }
    }

    if (allowTruncate && !buffer_.empty()) {
      LOG_WARN("AudioOutputControl: truncated Opus stream at EOF");
      buffer_.clear();
      currentPacket_.clear();
    }
    return true;
  }

  bool processPacket(const std::vector<uint8_t> &packet) {
    if (packet.empty()) {
      ++packetIndex_;
      return true;
    }

    if (packetIndex_ == 0) {
      if (packet.size() < 19 ||
          std::memcmp(packet.data(), "OpusHead", 8) != 0) {
        LOG_ERROR("AudioOutputControl: Opus stream missing ID header");
        return false;
      }

      opusChannels_ = packet[9];
      if (opusChannels_ <= 0 || opusChannels_ > kMaxOpusChannels) {
        LOG_ERROR("AudioOutputControl: unsupported Opus channel count "
                  << opusChannels_);
        return false;
      }

      uint16_t preSkip = readLe16(packet.data() + 10);
      samplesToDiscard_ = preSkip;

      int opusError = 0;
      decoder_ =
          opus_decoder_create(kOpusSampleRate, opusChannels_, &opusError);
      if (!decoder_ || opusError != OPUS_OK) {
        LOG_ERROR("AudioOutputControl: opus_decoder_create failed: "
                  << opus_strerror(opusError));
        decoder_ = nullptr;
        return false;
      }
      sampleRate_ = kOpusSampleRate;
      ++packetIndex_;
      return true;
    }

    if (packetIndex_ == 1 && packet.size() >= 8 &&
        std::memcmp(packet.data(), "OpusTags", 8) == 0) {
      ++packetIndex_;
      return true;
    }

    if (!decoder_) {
      LOG_ERROR("AudioOutputControl: Opus decoder not initialized");
      return false;
    }

    int decodedSamples = opus_decode(decoder_, packet.data(),
                                     static_cast<opus_int32>(packet.size()),
                                     decodeBuffer_.data(), kMaxFrameSize, 0);
    if (decodedSamples < 0) {
      LOG_WARN("AudioOutputControl: opus_decode failed: "
               << opus_strerror(decodedSamples));
      ++packetIndex_;
      return true;
    }

    if (decodedSamples > 0) {
      const int channels = std::max(opusChannels_, 1);
      downmix_.clear();
      downmix_.reserve(decodedSamples);
      for (int i = 0; i < decodedSamples; ++i) {
        if (samplesToDiscard_ > 0) {
          --samplesToDiscard_;
          continue;
        }
        int sum = 0;
        for (int ch = 0; ch < channels; ++ch) {
          sum += decodeBuffer_[static_cast<size_t>(i) * channels + ch];
        }
        downmix_.push_back(static_cast<int16_t>(sum / channels));
      }

      if (!downmix_.empty() && !sink_.pushSamples(downmix_, sampleRate_)) {
        return false;
      }
    }

    ++packetIndex_;
    return true;
  }

  std::vector<uint8_t> buffer_;
  std::vector<uint8_t> currentPacket_;
  OpusDecoder *decoder_{nullptr};
  bool haveStreamSerial_{false};
  uint32_t streamSerial_{0};
  int packetIndex_{0};
  int opusChannels_{0};
  int samplesToDiscard_{0};
  int sampleRate_{0};
  std::array<opus_int16, kMaxFrameSize * kMaxOpusChannels> decodeBuffer_{};
  std::vector<int16_t> downmix_;
};

#endif

std::unique_ptr<StreamingDecoder>
createStreamingDecoder(AudioFileFormat format, AudioStreamSink &sink,
                       const PlayCommandOptions &options) {
  switch (format) {
#if defined(USE_AAC) && USE_AAC
  case AudioFileFormat::AAC:
    return std::make_unique<StreamingAacDecoder>(sink);
#endif
#if defined(USE_MP3) && USE_MP3
  case AudioFileFormat::MP3:
    return std::make_unique<StreamingMp3Decoder>(sink);
#endif
#if defined(USE_OPUS) && USE_OPUS
  case AudioFileFormat::OPUS:
    return std::make_unique<StreamingOpusDecoder>(sink);
#endif
  case AudioFileFormat::PCM: {
    int streamRate = options.hasSampleRate ? options.sampleRate : 0;
    if (streamRate <= 0) {
      LOG_WARN("AudioOutputControl: PCM stream sample rate unknown, falling "
               "back to default");
    }
    return std::make_unique<StreamingPcmDecoder>(sink, streamRate);
  }
  default:
    LOG_ERROR("AudioOutputControl: streaming for format '"
              << formatName(format) << "' is not supported yet");
    return nullptr;
  }
}

class StreamingSession : public std::enable_shared_from_this<StreamingSession> {
public:
  explicit StreamingSession(PlayCommandOptions options)
      : options_(std::move(options)), stopRequested_(false) {
    resolvedFormat_ = options_.format;
    if (resolvedFormat_ == AudioFileFormat::AUTO) {
      AudioFileFormat guess = inferFormatFromExtension(options_.path);
      if (guess != AudioFileFormat::PCM) {
        resolvedFormat_ = guess;
      }
    }
  }

  ~StreamingSession() {
    requestStop();
    if (worker_.joinable()) {
      if (std::this_thread::get_id() == worker_.get_id()) {
        worker_.detach();
      } else {
        worker_.join();
      }
    }
  }

  void start() {
    auto self = shared_from_this();
    worker_ = std::thread([self]() { self->run(); });
  }

  void requestStop() {
    stopRequested_.store(true, std::memory_order_relaxed);
  }

  void join() {
    if (worker_.joinable() && std::this_thread::get_id() != worker_.get_id()) {
      worker_.join();
    }
  }

private:
  void run() {
    sink_ = std::make_unique<AudioStreamSink>(options_, stopRequested_);
    if (!sink_->prepare()) {
      return;
    }

    if (options_.loopCount > 1 || options_.loopDelayMs > 0) {
      LOG_WARN("AudioOutputControl: streaming ignores loop/delay settings");
    }

    if (!ensureCurlInitialized()) {
      LOG_ERROR("AudioOutputControl: curl initialization failed");
      return;
    }

    CURL *handle = curl_easy_init();
    if (!handle) {
      LOG_ERROR("AudioOutputControl: unable to allocate curl handle");
      return;
    }

    curl_easy_setopt(handle, CURLOPT_URL, options_.path.c_str());
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 256L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "prudynt-audio/1.0");
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION,
                     &StreamingSession::writeCallback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION,
                     &StreamingSession::progressCallback);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, this);

    CURLcode res = curl_easy_perform(handle);
    if (res != CURLE_OK && res != CURLE_ABORTED_BY_CALLBACK) {
      LOG_ERROR("AudioOutputControl: stream download failed: "
                << curl_easy_strerror(res));
    }
    curl_easy_cleanup(handle);

    if (decoder_) {
      decoder_->finalize();
    }
    sink_->finish();
  }

  bool handleData(const uint8_t *data, size_t size, bool endOfStream) {
    if (stopRequested_.load(std::memory_order_relaxed)) {
      return false;
    }

    if (decoder_) {
      return decoder_->feed(data, size, endOfStream);
    }

    if (data && size > 0) {
      sniffBuffer_.insert(sniffBuffer_.end(), data, data + size);
    }

    if (resolvedFormat_ == AudioFileFormat::AUTO) {
      auto detected = detectFormatFromBuffer(sniffBuffer_);
      if (detected.has_value()) {
        resolvedFormat_ = *detected;
        LOG_INFO("AudioOutputControl: detected streaming format '"
                 << formatName(resolvedFormat_) << "'");
      } else if (sniffBuffer_.size() >= kMaxStreamingSniffBytes) {
        LOG_ERROR("AudioOutputControl: unable to detect stream format");
        return false;
      }
    }

    if (resolvedFormat_ == AudioFileFormat::AUTO) {
      return true;
    }

    if (!decoder_) {
      decoder_ = createStreamingDecoder(resolvedFormat_, *sink_, options_);
      if (!decoder_) {
        return false;
      }
      if (!sniffBuffer_.empty()) {
        std::vector<uint8_t> pending;
        pending.swap(sniffBuffer_);
        return decoder_->feed(pending.data(), pending.size(), endOfStream);
      }
    }

    return true;
  }

  static size_t writeCallback(char *ptr, size_t size, size_t nmemb,
                              void *userdata) {
    auto *session = static_cast<StreamingSession *>(userdata);
    if (!session) {
      return 0;
    }
    size_t total = size * nmemb;
    if (total == 0) {
      return 0;
    }
    if (!session->handleData(reinterpret_cast<const uint8_t *>(ptr), total,
                             false)) {
      session->requestStop();
      return 0;
    }
    return total;
  }

  static int progressCallback(void *clientp, curl_off_t, curl_off_t, curl_off_t,
                              curl_off_t) {
    auto *session = static_cast<StreamingSession *>(clientp);
    if (!session) {
      return 0;
    }
    return session->stopRequested_.load(std::memory_order_relaxed) ? 1 : 0;
  }

  PlayCommandOptions options_;
  std::atomic<bool> stopRequested_;
  std::unique_ptr<AudioStreamSink> sink_;
  std::unique_ptr<StreamingDecoder> decoder_;
  AudioFileFormat resolvedFormat_{AudioFileFormat::AUTO};
  std::vector<uint8_t> sniffBuffer_;
  std::thread worker_;
};

std::once_flag gCurlInitFlag;
std::atomic<bool> gCurlReady{false};
std::mutex gStreamMutex;
std::shared_ptr<StreamingSession> gActiveStream;

bool ensureCurlInitialized() {
  std::call_once(gCurlInitFlag, []() {
    CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    gCurlReady.store(res == CURLE_OK, std::memory_order_relaxed);
    if (res != CURLE_OK) {
      LOG_ERROR("AudioOutputControl: curl_global_init failed: "
                << curl_easy_strerror(res));
    }
  });
  return gCurlReady.load(std::memory_order_relaxed);
}

bool startStreamingPlayback(const PlayCommandOptions &options) {
  if (!isUrl(options.path)) {
    return false;
  }

  stopActiveStream(true, "restart");

  auto session = std::make_shared<StreamingSession>(options);
  {
    std::lock_guard<std::mutex> lock(gStreamMutex);
    gActiveStream = session;
  }
  session->start();
  LOG_INFO("AudioOutputControl: started streaming from '" << options.path
                                                          << "'");
  return true;
}

void stopActiveStream(bool waitForStop, const char *reason) {
  std::shared_ptr<StreamingSession> session;
  {
    std::lock_guard<std::mutex> lock(gStreamMutex);
    session = gActiveStream;
    gActiveStream.reset();
  }
  if (!session) {
    return;
  }
  if (reason) {
    LOG_INFO("AudioOutputControl: stopping stream (" << reason << ")");
  }
  session->requestStop();
  if (waitForStop) {
    session->join();
  }
}

void enqueueSamples(const std::vector<int16_t> &samples, bool setVolume,
                    int volume, bool setGain, int gain) {
  if (samples.empty()) {
    AudioOutputWorker::enqueuePcm(std::vector<int16_t>{}, setVolume, volume,
                                  setGain, gain);
    return;
  }

  int targetRate = effectiveOutputSampleRate();
  size_t chunk = static_cast<size_t>(std::max(targetRate / 50, 1));
  bool firstChunk = true;
  for (size_t offset = 0; offset < samples.size(); offset += chunk) {
    size_t remaining = std::min(chunk, samples.size() - offset);
    auto beginIt = samples.begin() + static_cast<std::ptrdiff_t>(offset);
    auto endIt =
        samples.begin() + static_cast<std::ptrdiff_t>(offset + remaining);
    std::vector<int16_t> block(beginIt, endIt);
    if (!AudioOutputWorker::enqueuePcmBlocking(std::move(block),
                                               firstChunk && setVolume, volume,
                                               firstChunk && setGain, gain)) {
      LOG_ERROR("AudioOutputControl: failed to enqueue PCM chunk");
      break;
    }
    firstChunk = false;
  }

  if (firstChunk && (setVolume || setGain)) {
    AudioOutputWorker::enqueuePcm(std::vector<int16_t>{}, setVolume, volume,
                                  setGain, gain);
  }
}

void handlePlay(const PlayCommandOptions &options) {
  if (!cfg || !cfg->audio.output_enabled) {
    LOG_WARN(
        "AudioOutputControl: PLAY ignored because audio output is disabled");
    return;
  }
  if (!global_audio_output) {
    LOG_ERROR("AudioOutputControl: PLAY ignored because audio output stream is "
              "not available");
    return;
  }

  AudioFileFormat format = options.format;
  if (format == AudioFileFormat::AUTO) {
    format = inferFormatFromExtension(options.path);
    if (format == AudioFileFormat::PCM) {
      AudioFileFormat detected = detectFormatFromContent(options.path);
      if (detected != AudioFileFormat::PCM) {
        LOG_DEBUG("AudioOutputControl: inferred format '"
                  << formatName(detected) << "' for '" << options.path
                  << "' by inspecting content");
        format = detected;
      }
    }
  }

  if (isUrl(options.path)) {
    PlayCommandOptions streamingOptions = options;
    streamingOptions.format = format;
    if (!startStreamingPlayback(streamingOptions)) {
      LOG_ERROR("AudioOutputControl: failed to start streaming playback for '"
                << options.path << "'");
    }
    return;
  }

  stopActiveStream(true, "file playback");

  std::vector<int16_t> samples;
  int sourceRate = options.hasSampleRate ? options.sampleRate : 0;

  std::string rateStr = options.hasSampleRate
                            ? std::to_string(options.sampleRate)
                            : std::string("(default)");
  std::string volStr = options.setVolume ? std::to_string(options.volume)
                                         : std::string("(unchanged)");
  std::string gainStr = options.setGain ? std::to_string(options.gain)
                                        : std::string("(unchanged)");
  int loopCount = clampLoopCount(options.loopCount);
  int loopDelayMs = clampLoopDelay(options.loopDelayMs);

  LOG_DEBUG("AudioOutputControl: PLAY requested (path='"
            << options.path << "', format=" << formatName(format)
            << ", append=" << (options.append ? 1 : 0) << ", loop=" << loopCount
            << ", delay=" << loopDelayMs << "ms"
            << ", rate=" << rateStr << ", vol=" << volStr
            << ", gain=" << gainStr << ")");

  if (format == AudioFileFormat::AAC) {
    if (!decodeAacFile(options.path, samples, sourceRate)) {
      return;
    }
  } else if (format == AudioFileFormat::FLAC) {
    if (!decodeFlacFile(options.path, samples, sourceRate)) {
      return;
    }
  } else if (format == AudioFileFormat::MP3) {
    if (!decodeMp3File(options.path, samples, sourceRate)) {
      return;
    }
  } else if (format == AudioFileFormat::OPUS) {
    if (!decodeOpusFile(options.path, samples, sourceRate)) {
      return;
    }
  } else if (format == AudioFileFormat::WAV) {
    WavPayload payload;
    if (!readWavFile(options.path, payload)) {
      return;
    }
    samples = std::move(payload.samples);
    sourceRate = payload.sampleRate;
  } else {
    if (!readEntirePcm(options.path, samples)) {
      return;
    }
    if (!options.hasSampleRate) {
      sourceRate = defaultSampleRate();
    }
  }

  if (samples.empty()) {
    LOG_WARN("AudioOutputControl: PLAY command produced zero samples");
    return;
  }

  int targetRate = effectiveOutputSampleRate();

  // If the source and AO rates differ, try to reconfigure AO to the source
  // rate instead of resampling.  On some platforms (T10/T20/T21) the hardware
  // DAC may not run at the configured rate, so resampling to that rate
  // produces audio played at the wrong speed.  Reconfiguring to the source
  // rate avoids resampling entirely and lets the hardware run at a rate it
  // actually supports.
  // Skip on platforms with a shared AI/AO CODEC clock (T10/T20/T21/T30):
  // changing the AO clock also shifts the AI capture rate, causing the
  // microphone stream to play back at the wrong pitch/speed.
  bool reconfigured = false;
  if (sourceRate > 0 && sourceRate != targetRate && !options.append &&
      !hal::caps().has_shared_audio_clock) {
    if (AudioOutputWorker::reconfigureRate(sourceRate)) {
      targetRate = effectiveOutputSampleRate();
      reconfigured = true;
      LOG_DEBUG("AudioOutputControl: reconfigured AO to "
                << targetRate << " Hz to match source");
    } else {
      LOG_DEBUG("AudioOutputControl: AO reconfigure to "
                << sourceRate << " Hz failed, falling back to resampling");
    }
  }

  if (sourceRate != targetRate) {
    samples = resampleLinear(samples, sourceRate, targetRate);
  }

  const size_t samplesPerLoop = samples.size();
  const size_t totalSamples = samplesPerLoop * static_cast<size_t>(loopCount);
  auto enqueueStart = std::chrono::steady_clock::now();

  if (!options.append) {
    AudioOutputWorker::clearQueue(true);
  }

  bool pendingVolume = options.setVolume;
  bool pendingGain = options.setGain;
  if ((pendingVolume || pendingGain) &&
      AudioOutputWorker::applyVolumeGain(pendingVolume, options.volume,
                                         pendingGain, options.gain)) {
    pendingVolume = false;
    pendingGain = false;
  }

  LOG_DEBUG("AudioOutputControl: queuing "
            << totalSamples << " samples (" << loopCount << " loop(s), src="
            << sourceRate << " Hz -> dst=" << targetRate << " Hz)");

  for (int loopIdx = 0; loopIdx < loopCount; ++loopIdx) {
    enqueueSamples(samples, pendingVolume, options.volume, pendingGain,
                   options.gain);

    pendingVolume = false;
    pendingGain = false;

    if (loopDelayMs > 0 && loopIdx + 1 < loopCount) {
      std::this_thread::sleep_for(std::chrono::milliseconds(loopDelayMs));
    }
  }

  if (!options.append) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - enqueueStart);
    double expectedMsExact =
        (targetRate > 0) ? (static_cast<double>(totalSamples) * 1000.0) /
                               static_cast<double>(targetRate)
                         : 0.0;
    int expectedMs = static_cast<int>(std::ceil(expectedMsExact));
    int remainingMs = expectedMs - static_cast<int>(elapsed.count());
    if (remainingMs < 0) {
      remainingMs = 0;
    }
    remainingMs += 20; // small guard so flush happens after the tail

    constexpr auto kTailSilence = std::chrono::milliseconds(40);
    if (!AudioOutputWorker::waitForPlaybackCompletion(
            std::chrono::milliseconds(remainingMs), true, kTailSilence)) {
      LOG_WARN("AudioOutputControl: failed to wait for playback completion; "
               "audio queue may still be busy");
    }

    // Restore AO to the configured default rate so that backchannel
    // and other audio paths get the expected sample rate.
    if (reconfigured) {
      int defaultRate = defaultSampleRate();
      if (effectiveOutputSampleRate() != defaultRate) {
        AudioOutputWorker::reconfigureRate(defaultRate);
        LOG_DEBUG("AudioOutputControl: restored AO to "
                  << effectiveOutputSampleRate() << " Hz");
      }
    }
  }
}

void applyVolumeChange(int volume) {
  int clamped = clampVolume(volume);
  if (!AudioOutputWorker::applyVolumeGain(true, clamped, false, 0)) {
    AudioOutputWorker::enqueuePcm(std::vector<int16_t>{}, true, clamped, false,
                                  0);
  }
}

void applyGainChange(int gain) {
  int clamped = clampGain(gain);
  if (!AudioOutputWorker::applyVolumeGain(false, 0, true, clamped)) {
    AudioOutputWorker::enqueuePcm(std::vector<int16_t>{}, false, 0, true,
                                  clamped);
  }
}

void handleSetCommand(std::istringstream &iss) {
  bool volumeSet = false;
  bool gainSet = false;
  int volume = 0;
  int gain = 0;
  std::string token;
  while (iss >> token) {
    std::string key;
    std::string value = token;
    auto eq = token.find('=');
    if (eq != std::string::npos) {
      key = toLower(token.substr(0, eq));
      value = token.substr(eq + 1);
    } else {
      key.clear();
    }

    if (key == "volume" || key == "vol") {
      int parsed = 0;
      if (parseInt(value, parsed)) {
        volume = clampVolume(parsed);
        volumeSet = true;
      }
    } else if (key == "gain") {
      int parsed = 0;
      if (parseInt(value, parsed)) {
        gain = clampGain(parsed);
        gainSet = true;
      }
    }
  }

  if (volumeSet || gainSet) {
    if (!AudioOutputWorker::applyVolumeGain(volumeSet, volume, gainSet, gain)) {
      AudioOutputWorker::enqueuePcm(std::vector<int16_t>{}, volumeSet, volume,
                                    gainSet, gain);
    }
  } else {
    LOG_WARN("AudioOutputControl: SET command missing assignments");
  }
}

void handleCommand(const std::string &line, bool allowPlayback) {
  std::istringstream iss(line);
  std::string op;
  iss >> op;
  if (op.empty()) {
    return;
  }
  op = toUpper(op);

  LOG_DEBUG("AudioOutputControl: received command '" << line << "'");

  if (op == "PLAY") {
    if (!allowPlayback) {
      LOG_WARN("AudioOutputControl: PLAY command ignored on control FIFO");
      return;
    }
    PlayCommandOptions options;
    std::string token;
    while (iss >> token) {
      auto eq = token.find('=');
      if (eq == std::string::npos) {
        LOG_WARN("AudioOutputControl: ignoring PLAY argument without key '"
                 << token << "'. Use key=value syntax.");
        continue;
      }

      std::string key = toLower(token.substr(0, eq));
      std::string value = token.substr(eq + 1);

      if (key.empty()) {
        LOG_WARN("AudioOutputControl: ignoring PLAY argument with empty key '"
                 << token << "'");
        continue;
      }

      if (value.empty()) {
        LOG_WARN("AudioOutputControl: ignoring PLAY argument '"
                 << token << "' with empty value");
        continue;
      }

      if (key == "path" || key == "url") {
        options.path = value;
      } else if (key == "vol" || key == "volume") {
        int parsed = 0;
        if (parseInt(value, parsed)) {
          options.volume = clampVolume(parsed);
          options.setVolume = true;
        }
      } else if (key == "gain") {
        int parsed = 0;
        if (parseInt(value, parsed)) {
          options.gain = clampGain(parsed);
          options.setGain = true;
        }
      } else if (key == "rate" || key == "samplerate") {
        int parsed = 0;
        if (parseInt(value, parsed) && parsed > 0) {
          options.sampleRate = parsed;
          options.hasSampleRate = true;
        }
      } else if (key == "append") {
        int parsed = 0;
        if (parseInt(value, parsed)) {
          options.append = (parsed != 0);
        }
      } else if (key == "loop" || key == "loops" || key == "repeat") {
        int parsed = 0;
        if (parseInt(value, parsed)) {
          int clamped = clampLoopCount(parsed);
          if (clamped != parsed) {
            LOG_WARN("AudioOutputControl: loop count "
                     << parsed << " adjusted to " << clamped
                     << " (max=" << kMaxLoopCount << ")");
          }
          options.loopCount = clamped;
        }
      } else if (key == "delay" || key == "loopdelay") {
        int parsed = 0;
        if (parseInt(value, parsed)) {
          int clamped = clampLoopDelay(parsed);
          if (clamped != parsed) {
            LOG_WARN("AudioOutputControl: loop delay "
                     << parsed << " adjusted to " << clamped
                     << " ms (max=" << kMaxLoopDelayMs << ")");
          }
          options.loopDelayMs = clamped;
        }
      } else if (key == "format" || key == "fmt") {
        std::string lowerValue = toLower(value);
        if (lowerValue == "aac") {
          options.format = AudioFileFormat::AAC;
        } else if (lowerValue == "flac") {
          options.format = AudioFileFormat::FLAC;
        } else if (lowerValue == "mp3" || lowerValue == "mpeg" ||
                   lowerValue == "mp2") {
          options.format = AudioFileFormat::MP3;
        } else if (lowerValue == "opus") {
          options.format = AudioFileFormat::OPUS;
        } else if (lowerValue == "pcm") {
          options.format = AudioFileFormat::PCM;
        } else if (lowerValue == "wav" || lowerValue == "wave") {
          options.format = AudioFileFormat::WAV;
        }
      }
    }

    if (options.path.empty()) {
      LOG_WARN("AudioOutputControl: PLAY command missing required path=/url= "
               "argument");
      return;
    }

    handlePlay(options);
  } else if (op == "STOP") {
    LOG_DEBUG("AudioOutputControl: STOP requested");
    stopActiveStream(true, "stop command");
    if (!AudioOutputWorker::clearQueue(true)) {
      LOG_WARN("AudioOutputControl: STOP command ignored; audio output queue "
               "not available");
    }
  } else if (op == "MUTE") {
    std::string token;
    if (!(iss >> token)) {
      LOG_WARN("AudioOutputControl: MUTE command missing argument");
      return;
    }
    auto eq = token.find('=');
    std::string value =
        (eq == std::string::npos) ? token : token.substr(eq + 1);
    bool mute = false;
    if (!parseBool(value, mute)) {
      LOG_WARN("AudioOutputControl: invalid MUTE value '" << value << "'");
      return;
    }
    LOG_DEBUG("AudioOutputControl: MUTE=" << (mute ? 1 : 0));
    if (!AudioOutputWorker::applyMute(mute)) {
      AudioOutputWorker::enqueuePcm(std::vector<int16_t>{}, false, 0, false, 0,
                                    true, mute);
    }
  } else if (op == "VOLUME") {
    std::string value;
    if (!(iss >> value)) {
      LOG_WARN("AudioOutputControl: VOLUME command missing argument");
      return;
    }
    int parsed = 0;
    if (!parseInt(value, parsed)) {
      LOG_WARN("AudioOutputControl: invalid VOLUME value '" << value << "'");
      return;
    }
    LOG_DEBUG("AudioOutputControl: VOLUME=" << parsed);
    applyVolumeChange(parsed);
  } else if (op == "GAIN") {
    std::string value;
    if (!(iss >> value)) {
      LOG_WARN("AudioOutputControl: GAIN command missing argument");
      return;
    }
    int parsed = 0;
    if (!parseInt(value, parsed)) {
      LOG_WARN("AudioOutputControl: invalid GAIN value '" << value << "'");
      return;
    }
    LOG_DEBUG("AudioOutputControl: GAIN=" << parsed);
    applyGainChange(parsed);
  } else if (op == "SET") {
    LOG_DEBUG("AudioOutputControl: SET command received");
    handleSetCommand(iss);
  } else {
    LOG_WARN("AudioOutputControl: unrecognized command '" << op << "'");
  }
}

void fifoListenerLoop(const FifoListenerConfig &config) {
  LOG_INFO("AudioOutputControl: listening on " << config.label << " FIFO "
                                               << config.path);
  while (!global_shutdown_requested.load(std::memory_order_relaxed)) {
    int fd = ::open(config.path, O_RDONLY);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == ENOENT) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      LOG_ERROR("AudioOutputControl: open() failed for FIFO '"
                << config.path << "': " << strerror(errno));
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    while (!global_shutdown_requested.load(std::memory_order_relaxed)) {
      char buffer[512];
      ssize_t bytes = ::read(fd, buffer, sizeof(buffer));
      if (bytes <= 0) {
        break;
      }

      std::string chunk(buffer, static_cast<size_t>(bytes));
      std::istringstream lines(chunk);
      std::string line;
      while (std::getline(lines, line)) {
        auto cleaned = trim(line);
        if (!cleaned.empty()) {
          handleCommand(cleaned, config.allowPlayCommands);
        }
      }
    }

    ::close(fd);
  }
}
} // namespace

void AudioOutputControl::run() {
  if (!cfg) {
    LOG_ERROR("AudioOutputControl: configuration unavailable; not starting "
              "FIFO thread");
    return;
  }

  if (!cfg->audio.output_enabled) {
    LOG_INFO("AudioOutputControl: audio output disabled, skipping FIFO setup");
    return;
  }

  if (!ensureFifo(kFifoPath) || !ensureFifo(kControlFifoPath)) {
    return;
  }

  FifoListenerConfig controlCfg{kControlFifoPath, "control", false};
  std::thread controlThread([controlCfg]() { fifoListenerLoop(controlCfg); });
  controlThread.detach();

  FifoListenerConfig playbackCfg{kFifoPath, "playback", true};
  fifoListenerLoop(playbackCfg);

  stopActiveStream(true, "shutdown");

  ::unlink(kFifoPath);
  ::unlink(kControlFifoPath);
  LOG_INFO("AudioOutputControl: shutting down and removing FIFOs");
}
