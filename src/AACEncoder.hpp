#ifndef AAC_ENCODER_HPP
#define AAC_ENCODER_HPP

#include "IMPAudio.hpp"

#if defined(USE_AAC) && USE_AAC
#include <faac.h>
#include <cstdint>
#include <cstring>
#include <memory>

// Generic sample FIFO: accumulates interleaved int16 samples and feeds
// exactly frameSamples per encode call, regardless of HAL chunk size.
// Codec-agnostic --- works for AAC-LC (1024), HE-AAC (2048), Opus (960), etc.
class SampleFifo {
public:
  explicit SampleFifo(int channels) : numChn(channels) {}
  ~SampleFifo() { delete[] buf; }

  // Non-copyable
  SampleFifo(const SampleFifo &) = delete;
  SampleFifo &operator=(const SampleFifo &) = delete;

  void setCapacity(int totalSamples) {
    delete[] buf;
    cap = totalSamples;
    fill = 0;
    head = 0;
    buf = new int16_t[totalSamples];
  }

  // Push interleaved samples. Returns number of samples actually written.
  int push(const int16_t *src, int count) {
    int space = cap - fill;
    int n = count < space ? count : space;
    for (int i = 0; i < n; i++)
      buf[(head + fill + i) % cap] = src[i];
    fill += n;
    return n;
  }

  // Read exactly `count` interleaved samples from the front.
  // Caller must ensure available() >= count.
  void read(int16_t *dst, int count) {
    for (int i = 0; i < count; i++)
      dst[i] = buf[(head + i) % cap];
    head = (head + count) % cap;
    fill -= count;
  }

  int available() const { return fill; }
  int channels() const { return numChn; }

private:
  int16_t *buf = nullptr;
  int cap = 0;
  int fill = 0;
  int head = 0;
  int numChn;
};

class AACEncoder : public IMPAudioEncoder {
public:
  static AACEncoder *createNew(int sampleRate, int numChn);

  AACEncoder(int sampleRate, int numChn);
  virtual ~AACEncoder();

  int open() override;
  int encode(IMPAudioFrame *data, unsigned char *outbuf, int *outLen) override;
  int close() override;

  unsigned long getMaxOutputBytes() const {
    return maxOutputBytes;
  }
  int getFrameSamples() const {
    return static_cast<int>(inputSamples);
  }
  int64_t getLastFramePtsUs() const {
    return lastFramePtsUs;
  }
  const uint8_t *getAsc() const {
    return asc;
  }
  uint32_t getAscLen() const {
    return ascLen;
  }
  void setInputRate(int rate) {
    if (rate > 0)
      sampleRate = rate;
  }

private:
  faac_encoder *handle = nullptr;
  unsigned long inputSamples;
  unsigned long maxOutputBytes = 0;
  int sampleRate;
  int numChn;
  int64_t lastFramePtsUs = 0;
  uint8_t asc[8] = {};
  uint32_t ascLen = 0;

  // Sample FIFO --- accumulates PCM, feeds encoder in exact frame-sized chunks
  std::unique_ptr<SampleFifo> fifo;

  // Heap-allocated output buffer (avoids MIPS stack overflow from 16KB on stack)
  uint8_t *encOutBuf = nullptr;

  // Smooth output timestamp
  uint32_t nextOutTsMs = 0;
  bool outTsRunning = false;
};
#endif

#endif // AAC_ENCODER_HPP
