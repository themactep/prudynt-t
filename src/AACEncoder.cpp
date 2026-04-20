#include "AACEncoder.hpp"

#if defined(USE_AAC) && USE_AAC
#include "Config.hpp"
#include "Logger.hpp"
#include <cstdint>

AACEncoder *AACEncoder::createNew(int sampleRate, int numChn) {
  return new AACEncoder(sampleRate, numChn);
}

AACEncoder::AACEncoder(int sampleRate, int numChn)
    : sampleRate(sampleRate), numChn(numChn) {
}

AACEncoder::~AACEncoder() {
  close();
}

int AACEncoder::open() {
  unsigned long outputBufferSize;
  handle = faacEncOpen(sampleRate, numChn, &inputSamples, &outputBufferSize);
  if (!handle) {
    LOG_ERROR("Failed to open FAAC encoder");
    return -1;
  }

  maxOutputBytes = outputBufferSize;
  LOG_INFO("FAAC maxOutputBytes=" << maxOutputBytes);

  faacEncConfigurationPtr config = faacEncGetCurrentConfiguration(handle);
  config->aacObjectType = LOW;
  config->bandWidth = sampleRate;
  config->bitRate = cfg->audio.input_bitrate * 1000;
  config->inputFormat = FAAC_INPUT_16BIT;
  config->mpegVersion = MPEG4;
  config->outputFormat = RAW_STREAM; // no need for ADTS headers

  // Disable to reduce CPU utilization
  config->allowMidside = 0;
  config->useTns = 0;

  if (!faacEncSetConfiguration(handle, config)) {
    LOG_ERROR("Failed to configure FAAC encoder");
    return -1;
  }

  LOG_INFO("FAAC expects " << inputSamples << " samples per frame");

  return 0;
}

int AACEncoder::close() {
  if (handle) {
    faacEncClose(handle);
  }
  handle = nullptr;
  return 0;
}

int AACEncoder::encode(IMPAudioFrame *data, unsigned char *outbuf,
                       int *outLen) {
  if (!handle) {
    LOG_ERROR("FAAC encoder not available");
    return -1;
  }

  const int totalInputSamples = data->len / static_cast<int>(sizeof(int16_t));
  if (totalInputSamples <= 0) {
    *outLen = 0;
    return 0;
  }

  if (static_cast<unsigned long>(totalInputSamples) != inputSamples) {
    LOG_WARN("FAAC sample mismatch: expected " << inputSamples << " got "
                                               << totalInputSamples);
  }

  // FAAC API takes int32_t samples even for FAAC_INPUT_16BIT. Provide a
  // sign-extended 32-bit PCM buffer to avoid reading past the 16-bit input.
  const int16_t *pcm16 = reinterpret_cast<const int16_t *>(data->virAddr);
  pcm32Buffer.resize(static_cast<size_t>(totalInputSamples));
  for (int i = 0; i < totalInputSamples; ++i) {
    pcm32Buffer[static_cast<size_t>(i)] = static_cast<int32_t>(pcm16[i]);
  }

  const int frameLen =
      faacEncEncode(handle, pcm32Buffer.data(), totalInputSamples,
                    reinterpret_cast<unsigned char *>(outbuf), maxOutputBytes);
  *outLen = frameLen;

  if (frameLen < 0) {
    LOG_ERROR("FAAC encoding failed: " << frameLen);
    close();
    if (open() == 0) {
      LOG_INFO("AAC encoder reinitialized");
    }
    return -1;
  }

  return 0;
}
#endif
