#ifndef BACKCHANNEL_PROCESSOR_HPP
#define BACKCHANNEL_PROCESSOR_HPP

// Processes audio frames, decodes them, handles session management (who is
// "current"), resamples, and forwards PCM data to the audio output queue.

#include "audio/IMPBackchannel.hpp"
#include "stream/globals.hpp"

#include <cstdint>
#include <vector>

class BackchannelWorker {
public:
  BackchannelWorker();
  ~BackchannelWorker();

  static void *thread_entry(void *arg);
  static void signalShutdown();

private:
  void run();

  std::vector<int16_t> resampleLinear(const std::vector<int16_t> &input_pcm,
                                      int input_rate, int output_rate);

  bool processFrame(const BackchannelFrame &frame);
  bool decodeFrame(const uint8_t *payload, size_t payloadSize,
                   IMPBackchannelFormat format,
                   std::vector<int16_t> &outPcmBuffer);

  unsigned int currentSessionId;

  BackchannelWorker(const BackchannelWorker &) = delete;
  BackchannelWorker &operator=(const BackchannelWorker &) = delete;
};

#endif // BACKCHANNEL_PROCESSOR_HPP
