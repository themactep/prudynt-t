#include "Opus.hpp"

#if defined(USE_OPUS) && USE_OPUS
#include "Config.hpp"
#include "Logger.hpp"
#include "RTSPStatus.hpp"
#include <atomic>

namespace {
std::atomic<uint32_t> g_opus_mismatch_count{0};

bool is_valid_opus_frame_size(int sample_rate, int samples_per_channel) {
  if (sample_rate <= 0 || samples_per_channel <= 0) {
    return false;
  }

  // Opus supports 2.5/5/10/20/40/60 ms frame durations.
  static constexpr int kFrameDurationTenthsMs[] = {25, 50, 100, 200, 400, 600};
  for (int duration_tenths_ms : kFrameDurationTenthsMs) {
    int expected = static_cast<int>(
        (static_cast<int64_t>(sample_rate) * duration_tenths_ms) / 10000);
    if (samples_per_channel == expected) {
      return true;
    }
  }

  return false;
}
} // namespace

Opus *Opus::createNew(int sampleRate, int numChn) {
  return new Opus(sampleRate, numChn);
}

Opus::~Opus() {
  close();
}

int Opus::open() {
  int opusError;

  encoder = opus_encoder_create(sampleRate, numChn, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &opusError);
  if (opusError != OPUS_OK) {
    LOG_ERROR("Failed to create Opus encoder: " << opus_strerror(opusError));
    return -1;
  }

  // Configure encoder for maximum quality at the configured bitrate
  int bitrate = cfg->audio.input_bitrate * 1000; // bps
  opusError = opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate));
  if (opusError != OPUS_OK) {
    LOG_ERROR("Failed to set bitrate (" << bitrate << ") for Opus encoder: " << opus_strerror(opusError));
  }

  opusError = opus_encoder_ctl(encoder, OPUS_GET_BITRATE(&bitrate));
  if (opusError != OPUS_OK) {
    LOG_ERROR("Failed to get bitrate from Opus encoder: " << opus_strerror(opusError));
    return -1;
  }

  LOG_INFO("Encoder bitrate: " << bitrate);
  g_opus_mismatch_count.store(0, std::memory_order_relaxed);
  RTSPStatus::writeCustomParameter("audio0", "opus_mismatch_count", "0");

  return 0;
}

int Opus::close() {
  if (encoder) {
    opus_encoder_destroy(encoder);
  }
  encoder = nullptr;
  return 0;
}

int Opus::encode(IMPAudioFrame *data, unsigned char *outbuf, int *outLen) {
  const int samples_per_channel = (data->len / static_cast<int>(sizeof(int16_t))) / numChn;
  if (!is_valid_opus_frame_size(sampleRate, samples_per_channel)) {
    uint32_t mismatch_count = ++g_opus_mismatch_count;
    RTSPStatus::writeCustomParameter("audio0", "opus_mismatch_count",
                                     std::to_string(mismatch_count));
    if (mismatch_count <= 10 || (mismatch_count % 100) == 0) {
      LOG_WARN("Opus frame size mismatch: got " << samples_per_channel
                                                << " samples/ch at " << sampleRate
                                                << " Hz, dropping frame");
    }
    return -1;
  }

  opus_int32 bytesEncoded =
      opus_encode(encoder, reinterpret_cast<const opus_int16 *>(data->virAddr), samples_per_channel,
                  reinterpret_cast<unsigned char *>(outbuf), 1024);

  if (bytesEncoded < 0) {
    LOG_WARN("Opus encoding failed with error code: " << bytesEncoded);
    return -1;
  }

  *outLen = bytesEncoded;

  return 0;
}
#endif
