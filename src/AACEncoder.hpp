#ifndef AAC_ENCODER_HPP
#define AAC_ENCODER_HPP

#include "IMPAudio.hpp"

#if defined(USE_AAC) && USE_AAC
#include <faac.h>

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

private:
  faac_encoder *handle = nullptr;
  unsigned long inputSamples;
  unsigned long maxOutputBytes = 0;
  int sampleRate;
  int numChn;
};
#endif

#endif // AAC_ENCODER_HPP
