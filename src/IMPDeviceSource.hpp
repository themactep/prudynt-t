#ifndef IMPDeviceSource_hpp
#define IMPDeviceSource_hpp

#include "FramedSource.hh"
#include "globals.hpp"
#include <condition_variable>
#include <mutex>
#include <queue>

template <typename FrameType, typename Stream>
class IMPDeviceSource : public FramedSource {
public:
  static IMPDeviceSource *createNew(UsageEnvironment &env, int encChn,
                                    std::shared_ptr<Stream> stream,
                                    const char *name);

  void on_data_available() {
    if (eventTriggerId != 0) {
      envir().taskScheduler().triggerEvent(eventTriggerId, this);
    }
  }
  IMPDeviceSource(UsageEnvironment &env, int encChn,
                  std::shared_ptr<Stream> stream, const char *name);
  virtual ~IMPDeviceSource();

private:
  virtual void doGetNextFrame() override;
  static void deliverFrame0(void *clientData);
  void deliverFrame();
  void deinit();
  int encChn;
  std::shared_ptr<Stream> stream;
  std::string name; // for printing
  EventTriggerId eventTriggerId;

  // Monotonic counter for audio PTS (avoids gettimeofday jitter for AAC)
  bool audioFirstFrame{true};
  struct timeval audioStartTime{};
  uint64_t audioFrameCount{0};
  int audioClockSampleRate{0};
  int64_t audioLastPtsUs{-1};

  // Video encoder timestamp tracking (avoids gettimeofday jitter)
  bool videoFirstFrame{true};
  int64_t videoFirstImpTs{0};
  int64_t videoLastDelta{-1};
  int64_t videoLastPtsUs{-1};
  struct timeval videoBaseTime{};
};

#endif
