#ifndef AUDIO_WORKER_HPP
#define AUDIO_WORKER_HPP

#include "AudioReframer.hpp"
#include "IMPAudio.hpp"

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
  std::unique_ptr<AudioReframer> reframer;
  std::vector<int64_t> mp4_audio_samples;
  int mp4_audio_sample_rate = 0;
  std::unique_ptr<AudioTap> tap;

  // Wall-clock anchor for IMP hardware timestamp conversion.
  // The IMP timeStamp is a boot-relative µs counter that can wrap at 2^32 µs
  // (~71.6 min). We pin it to real wall clock time on the first frame and use
  // deltas from there, so fPresentationTime is always a valid Unix timeval and
  // RTCP sender reports carry correct NTP↔RTP mappings.
  bool hw_ts_initialized{false};
  int64_t hw_ts_base{0};        // IMP timestamp of the first frame (µs)
  struct timeval wall_ts_base{0, 0}; // gettimeofday() at the first frame
};

#endif // AUDIO_WORKER_HPP
