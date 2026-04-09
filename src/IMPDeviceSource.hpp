#ifndef IMPDeviceSource_hpp
#define IMPDeviceSource_hpp

#include "FramedSource.hh"
#include "globals.hpp"
#include <condition_variable>
#include <mutex>
#include <queue>

template <typename FrameType, typename Stream> class IMPDeviceSource : public FramedSource {
public:
  static IMPDeviceSource *createNew(UsageEnvironment &env, int encChn, std::shared_ptr<Stream> stream,
                                    const char *name);

  void on_data_available() {
    if (eventTriggerId != 0) {
      envir().taskScheduler().triggerEvent(eventTriggerId, this);
    }
  }
  IMPDeviceSource(UsageEnvironment &env, int encChn, std::shared_ptr<Stream> stream, const char *name);
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

  // StreamCore cursor for reading frames (replaces msgChannel)
  typename StreamCore<FrameType>::Cursor cursor;

  // Presentation time normalization (from Prudynt-SE)
  uint64_t presentationAnchorUs{0};      // Anchor point for timestamp normalization
  uint64_t lastSourceFrameUs{0};         // Last raw source timestamp
  uint64_t lastPresentationFrameUs{0};   // Last normalized presentation timestamp
  
  // Helper for timestamp normalization
  uint64_t normalizePresentationTimeUs(uint64_t sourceFrameUs, uint64_t durationUs);
};

#endif
