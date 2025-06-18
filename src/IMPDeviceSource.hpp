#ifndef IMPDeviceSource_hpp
#define IMPDeviceSource_hpp

#include "FramedSource.hh"
#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>
#include "globals.hpp"

template <typename FrameType, typename Stream>
class IMPDeviceSource : public FramedSource
{
public:
    static IMPDeviceSource *createNew(UsageEnvironment &env, int encChn, std::shared_ptr<Stream> stream, const char *name);

    void on_data_available()
    {
        if (eventTriggerId != 0)
        {
            envir().taskScheduler().triggerEvent(eventTriggerId, this);
        }
    }
    IMPDeviceSource(UsageEnvironment &env, int encChn, std::shared_ptr<Stream> stream, const char *name);
    virtual ~IMPDeviceSource();

private:

    virtual void doGetNextFrame() override;
    static void deliverFrame0(void *clientData);
    static void afterGettingFrame0(void *clientData);
    void deliverFrame();
    void deinit();
    void generateMonotonicTimestamp();
    void initializeFrameDuration();
    bool shouldThrottleDelivery();

    int encChn;
    std::shared_ptr<Stream> stream;
    std::string name;   // for printing
    EventTriggerId eventTriggerId;

    // Monotonic timestamp management (NTP-shift resistant)
    std::chrono::steady_clock::time_point stream_start_time_;
    bool timestamp_initialized_;
    uint64_t frame_count_;
    double frame_duration_us_; // microseconds per frame

    // RTP flow control to prevent packet loss
    std::chrono::steady_clock::time_point last_delivery_time_;
    uint32_t consecutive_fast_deliveries_;
    static constexpr double MIN_DELIVERY_INTERVAL_US = 500.0; // Minimum 0.5ms between frames
};

#endif
