#include "audio/codec/AACEncoder.hpp"

#if defined(USE_AAC) && USE_AAC
#include "config/Config.hpp"
#include "util/Logger.hpp"
#include <cstdint>
#include <cstring>
#include <ctime>

AACEncoder *AACEncoder::createNew(int sampleRate, int numChn) {
  return new AACEncoder(sampleRate, numChn);
}

AACEncoder::AACEncoder(int sampleRate, int numChn)
    : sampleRate(sampleRate), numChn(numChn) {}

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

  params.sample_rate = sampleRate;
  params.num_channels = numChn;
  params.mpeg_version = FAAC_MPEG4;
  params.object_type = FAAC_OBJ_LOW;
  params.input_format = FAAC_INPUT_16BIT;
  params.output_format = FAAC_STREAM_RAW;
  params.bit_rate = cfg->audio.mic_bitrate_kbps() * 1000;
  params.bandwidth = 0;
  params.use_tns = false;
  params.joint_mode = FAAC_JOINT_NONE;

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

  inputSamples = info.frame_samples;
  maxOutputBytes = info.max_output_bytes;
  lastFramePtsUs = 0;
  nextOutTsMs = 0;
  outTsRunning = false;

  // FIFO: capacity = 3x frame size so we can accumulate across HAL frames
  int fifoCapacity = (int)inputSamples * numChn * 3;
  fifo = std::make_unique<SampleFifo>(numChn);
  fifo->setCapacity(fifoCapacity);

  // Heap-allocated output buffer for FAAC (avoids stack overflow on MIPS)
  delete[] encOutBuf;
  encOutBuf = new uint8_t[maxOutputBytes + 4096];

  // Capture the real AudioSpecificConfig from FAAC
  {
    const uint8_t *ascPtr = nullptr;
    uint32_t aLen = 0;
    if (faac_encoder_asc(handle, &ascPtr, &aLen) == FAAC_OK &&
        ascPtr && aLen > 0 && aLen <= sizeof(asc)) {
      memcpy(asc, ascPtr, aLen);
      ascLen = aLen;
    }
  }

  LOG_INFO("FAAC encoder: " << sampleRate << "Hz"
           << " maxOutputBytes=" << maxOutputBytes
           << " inputSamples=" << inputSamples);

  return 0;
}

int AACEncoder::close() {
  if (handle) {
    faac_encoder_close(&handle);
  }
  handle = nullptr;
  delete[] encOutBuf;
  encOutBuf = nullptr;
  return 0;
}

int AACEncoder::encode(IMPAudioFrame *data, unsigned char *outbuf,
                       int *outLen) {
  if (!handle) {
    LOG_ERROR("FAAC encoder not available");
    return -1;
  }

  const auto frameSamples = (data->len / sizeof(int16_t)) / numChn;
  *outLen = 0;

  fifo->push(reinterpret_cast<const int16_t *>(data->virAddr),
            (int)frameSamples * numChn);

  if (!outTsRunning) {
    if (data->timeStamp != 0) {
      nextOutTsMs = (uint32_t)(data->timeStamp / 1000);
    } else {
      // HAL timestamps unavailable (e.g. T10) --- use monotonic wall clock
      struct timespec mono;
      clock_gettime(CLOCK_MONOTONIC, &mono);
      nextOutTsMs = (uint32_t)(mono.tv_sec * 1000 + mono.tv_nsec / 1000000);
    }
    outTsRunning = true;
  }

  uint32_t frameDur = (uint32_t)inputSamples * 1000 / sampleRate;
  int frameTotal = (int)inputSamples * numChn;

  // Drain the FIFO in exact frame-sized chunks
  int16_t frameBuf[2048]; // max HE-AAC: 2048 * numChn (numChn <= 2)
  while (fifo->available() >= frameTotal) {
    fifo->read(frameBuf, frameTotal);

    uint32_t bytesWritten = 0;
    uint32_t outCap = (uint32_t)(maxOutputBytes + 4096);
    faac_status st = faac_encoder_encode(
        handle, frameBuf, (uint32_t)frameTotal,
        encOutBuf, outCap, &bytesWritten);

    if (st != FAAC_OK) {
      LOG_ERROR("FAAC encoding failed: " << faac_strerror(st));
      close();
      if (open() == 0)
        LOG_INFO("AAC encoder reinitialized");
      return -1;
    }

    if (bytesWritten > 0) {
      memcpy(outbuf + *outLen, encOutBuf, bytesWritten);
      *outLen += static_cast<int>(bytesWritten);

      lastFramePtsUs = (int64_t)nextOutTsMs * 1000;
      nextOutTsMs += frameDur;

      // Drift correction: snap encoder clock to HAL when it drifts > 4 frames
      if (data->timeStamp != 0) {
        uint32_t impTsMs = (uint32_t)(data->timeStamp / 1000);
        int32_t err = (int32_t)impTsMs - (int32_t)nextOutTsMs;
        if (err > (int32_t)frameDur * 4 || err < -(int32_t)frameDur * 4)
          nextOutTsMs = impTsMs;
        else if (err > 1)
          nextOutTsMs++;
        else if (err < -1)
          nextOutTsMs--;
      }
    }
  }

  return 0;
}
#endif
