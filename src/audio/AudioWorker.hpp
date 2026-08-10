#ifndef AUDIO_WORKER_HPP
#define AUDIO_WORKER_HPP

#include "audio/IMPAudio.hpp"

#include <memory>
#include <vector>

class AudioTap;

class AudioWorker {
public:
  explicit AudioWorker(int encChn);
  ~AudioWorker();

  static void *thread_entry(void *arg);

private:
  void run();
  void process_audio_frame(IMPAudioFrame &frame);
  void process_frame(IMPAudioFrame &frame);
  void publishTapFrame(const IMPAudioFrame &frame);

  int encChn;
  std::vector<int64_t> mp4_audio_samples;
  int mp4_audio_sample_rate = 0;
  std::unique_ptr<AudioTap> tap;
  std::vector<uint8_t> directEncBuf;
};

#endif // AUDIO_WORKER_HPP
