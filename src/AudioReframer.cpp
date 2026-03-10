#include "AudioReframer.hpp"
#include <algorithm>
#include <stdexcept>

AudioReframer::AudioReframer(unsigned int inputSampleRate, unsigned int inputSamplesPerFrame,
                             unsigned int outputSamplesPerFrame)
    : inputSampleRate(inputSampleRate), inputSamplesPerFrame(inputSamplesPerFrame),
      outputSamplesPerFrame(outputSamplesPerFrame), currentTimestamp(0), timestampRemainder(0), samplesAccumulated(0),
      buffer(2 * std::max(inputSamplesPerFrame, outputSamplesPerFrame) * sizeof(uint16_t)) {
  if (inputSamplesPerFrame == 0 || outputSamplesPerFrame == 0) {
    throw std::invalid_argument("Number of samples per frame must be greater than zero.");
  }
}

void AudioReframer::addFrame(const uint8_t *frameData, int64_t timestamp) {
  if (frameData == nullptr) {
    throw std::invalid_argument("Frame data cannot be null.");
  }

  size_t inputFrameSize = inputSamplesPerFrame * sizeof(uint16_t);
  buffer.push(frameData, inputFrameSize);

  if (samplesAccumulated == 0) {
    currentTimestamp = timestamp; // Initialize timestamp with the first frame
    timestampRemainder = 0;
  }

  samplesAccumulated += inputSamplesPerFrame;
}

void AudioReframer::getReframedFrame(uint8_t *frameData, int64_t &timestamp) {
  if (!hasMoreFrames()) {
    throw std::runtime_error("Insufficient samples to generate a reframed output.");
  }

  if (frameData == nullptr) {
    throw std::invalid_argument("Output frame cannot be null.");
  }

  size_t outputFrameSize = outputSamplesPerFrame * sizeof(uint16_t);
  buffer.fetch(frameData, outputFrameSize);
  samplesAccumulated -= outputSamplesPerFrame;

  timestamp = currentTimestamp;
  if (inputSampleRate > 0) {
    // Timestamp is in microseconds; preserve fractional precision between frames.
    int64_t numer = static_cast<int64_t>(outputSamplesPerFrame) * 1000000LL + timestampRemainder;
    int64_t step_us = numer / static_cast<int64_t>(inputSampleRate);
    timestampRemainder = numer % static_cast<int64_t>(inputSampleRate);
    if (step_us < 1) {
      step_us = 1;
    }
    currentTimestamp += step_us;
  } // else: keep currentTimestamp stable if misconfigured
}

bool AudioReframer::hasMoreFrames() const {
  return samplesAccumulated >= outputSamplesPerFrame;
}
