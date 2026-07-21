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
  faac_params params;
  faac_status st = faac_params_init(&params);
  if (st != FAAC_OK) {
    LOG_ERROR("faac_params_init failed: " << faac_strerror(st));
    return -1;
  }

  params.sample_rate   = sampleRate;
  params.num_channels  = numChn;
  params.object_type   = FAAC_OBJ_LOW;
  params.mpeg_version  = FAAC_MPEG4;
  params.input_format  = FAAC_INPUT_16BIT;
  params.output_format = FAAC_STREAM_RAW;
  params.bit_rate      = cfg->audio.input_bitrate * 1000;
  params.bandwidth     = sampleRate;
  params.use_tns       = false;
  params.joint_mode    = FAAC_JOINT_NONE;

  st = faac_encoder_open(&params, &handle);
  if (st != FAAC_OK) {
    LOG_ERROR("faac_encoder_open failed: " << faac_strerror(st));
    handle = nullptr;
    return -1;
  }

  faac_encoder_info info;
  info.struct_size = sizeof(info);
  st = faac_encoder_get_info(handle, &info);
  if (st != FAAC_OK) {
    LOG_ERROR("faac_encoder_get_info failed: " << faac_strerror(st));
    faac_encoder_close(&handle);
    return -1;
  }

  inputSamples  = info.frame_samples;
  maxOutputBytes = info.max_output_bytes;

  LOG_INFO("FAAC maxOutputBytes=" << maxOutputBytes
           << " inputSamples=" << inputSamples);

  return 0;
}

int AACEncoder::close() {
  if (handle) {
    faac_encoder_close(&handle);
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

  const auto frameSamples = (data->len / sizeof(int16_t)) / numChn;
  if (frameSamples != inputSamples) {
    LOG_WARN("FAAC sample mismatch: expected " << inputSamples << " got "
                                               << frameSamples);
  }

  uint32_t bytesWritten = 0;
  faac_status st = faac_encoder_encode(
      handle, data->virAddr, frameSamples * numChn,
      outbuf, maxOutputBytes, &bytesWritten);
  *outLen = static_cast<int>(bytesWritten);

  if (st != FAAC_OK) {
    LOG_ERROR("FAAC encoding failed: " << faac_strerror(st));
    close();
    if (open() == 0) {
      LOG_INFO("AAC encoder reinitialized");
    }
    return -1;
  }

  return 0;
}
#endif
