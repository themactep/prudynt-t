#include "IMPDeviceSource.hpp"
#include <iostream>
#include "GroupsockHelper.hh"

// explicit instantiation
template class IMPDeviceSource<H264NALUnit, video_stream>;
template class IMPDeviceSource<AudioFrame, audio_stream>;

template<typename FrameType, typename Stream>
IMPDeviceSource<FrameType, Stream> *IMPDeviceSource<FrameType, Stream>::createNew(UsageEnvironment &env, int encChn, std::shared_ptr<Stream> stream, const char *name)
{
    return new IMPDeviceSource<FrameType, Stream>(env, encChn, stream, name);
}

template<typename FrameType, typename Stream>
IMPDeviceSource<FrameType, Stream>::IMPDeviceSource(UsageEnvironment &env, int encChn, std::shared_ptr<Stream> stream, const char *name)
    : FramedSource(env), encChn(encChn), stream{stream}, name{name}, eventTriggerId(0),
      timestamp_initialized_(false), frame_count_(0), frame_duration_us_(40000.0), // Default 25fps = 40ms = 40000us
      consecutive_fast_deliveries_(0)
{
    std::lock_guard lock_stream {mutex_main};
    std::lock_guard lock_callback {stream->onDataCallbackLock};
    stream->onDataCallback = [this]()
    { this->on_data_available(); };
    stream->hasDataCallback = true;

    // Initialize frame duration based on stream configuration
    initializeFrameDuration();

    eventTriggerId = envir().taskScheduler().createEventTrigger(deliverFrame0);
    stream->should_grab_frames.notify_one();
    LOG_DEBUG("IMPDeviceSource " << name << " constructed, encoder channel:" << encChn);
}

template<typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::deinit()
{
    std::lock_guard lock_stream {mutex_main};
    std::lock_guard lock_callback {stream->onDataCallbackLock};
    envir().taskScheduler().deleteEventTrigger(eventTriggerId);
    stream->hasDataCallback = false;
    stream->onDataCallback = nullptr;
    LOG_DEBUG("IMPDeviceSource " << name << " destructed, encoder channel:" << encChn);
}

template<typename FrameType, typename Stream>
IMPDeviceSource<FrameType, Stream>::~IMPDeviceSource()
{
    deinit();
}

template<typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::doGetNextFrame()
{
    deliverFrame();
}

template <typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::deliverFrame0(void *clientData)
{
    ((IMPDeviceSource<FrameType, Stream> *)clientData)->deliverFrame();
}

template<typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::afterGettingFrame0(void *clientData)
{
    ((IMPDeviceSource *)clientData)->FramedSource::afterGetting((IMPDeviceSource *)clientData);
}

template <typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::deliverFrame()
{
    static int frame_count = 0;
    static auto last_debug = std::chrono::steady_clock::now();
    frame_count++;

    auto now = std::chrono::steady_clock::now();
    auto time_since_debug = std::chrono::duration_cast<std::chrono::seconds>(now - last_debug).count();

    // Debug output every 5 seconds
    if (time_since_debug >= 5) {
        LOG_INFO("Frame delivery stats for " << name << " (ch" << encChn << "): "
                 << frame_count << " calls in " << time_since_debug << "s");
        frame_count = 0;
        last_debug = now;
    }

    if (!isCurrentlyAwaitingData()) {
        LOG_DEBUG("Not awaiting data for " << name << " (ch" << encChn << ")");
        return;
    }

    FrameType nal;
    if (stream->msgChannel->read(&nal))
    {
        LOG_DEBUG("Read frame for " << name << " (ch" << encChn << "): " << nal.data.size() << " bytes");

        if (nal.data.size() > fMaxSize)
        {
            fFrameSize = fMaxSize;
            fNumTruncatedBytes = nal.data.size() - fMaxSize;
            LOG_WARN("Frame truncated for " << name << " (ch" << encChn << "): "
                     << nal.data.size() << " -> " << fMaxSize << " bytes");
        }
        else
        {
            fFrameSize = nal.data.size();
        }

        // Generate monotonic RTP timestamps (NTP-shift resistant)
        generateMonotonicTimestamp();

        memcpy(fTo, &nal.data[0], fFrameSize);

        LOG_DEBUG("Calling afterGetting() for " << name << " (ch" << encChn << ") with " << fFrameSize << " bytes");

        // Implement RTP flow control to prevent packet loss
        if (shouldThrottleDelivery()) {
            LOG_DEBUG("Throttling frame delivery for " << name << " (ch" << encChn << ") to prevent RTP overflow");
            // Schedule delayed delivery instead of immediate
            envir().taskScheduler().scheduleDelayedTask(2000, // 2ms delay
                (TaskFunc*)afterGettingFrame0, this);
        } else {
            FramedSource::afterGetting(this);
        }
    }
    else
    {
        // No frame available - implement watchdog to prevent permanent freeze
        static int no_frame_count = 0;
        static auto last_no_frame_debug = std::chrono::steady_clock::now();
        no_frame_count++;

        auto time_since_no_frame = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_no_frame_debug).count();

        // Log no-frame events every second
        if (time_since_no_frame >= 1000) {
            LOG_WARN("No frames available for " << name << " (ch" << encChn << "): "
                     << no_frame_count << " attempts in " << time_since_no_frame << "ms");
            no_frame_count = 0;
            last_no_frame_debug = now;
        }

        // Schedule a retry after a short delay to ensure we don't get stuck
        static auto last_retry = std::chrono::steady_clock::now();
        auto time_since_retry = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_retry).count();

        // Only schedule retry if it's been more than 100ms since last retry
        if (time_since_retry > 100) {
            LOG_DEBUG("Scheduling retry for " << name << " (ch" << encChn << ") after " << time_since_retry << "ms");
            envir().taskScheduler().scheduleDelayedTask(10000, // 10ms delay
                (TaskFunc*)deliverFrame0, this);
            last_retry = now;
        }
    }
}

template<typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::generateMonotonicTimestamp() {
    if (!timestamp_initialized_) {
        // Initialize monotonic stream start time (immune to NTP shifts)
        stream_start_time_ = std::chrono::steady_clock::now();
        timestamp_initialized_ = true;
        frame_count_ = 0;

        // Set initial presentation time to current system time
        // This gives Live555 a reasonable starting point
        gettimeofday(&fPresentationTime, NULL);

        LOG_INFO("Initialized monotonic timestamps for " << name << " (ch" << encChn
                 << ") - immune to NTP time shifts");
    } else {
        // Calculate elapsed time since stream start using monotonic clock
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - stream_start_time_);
        uint64_t elapsed_us = elapsed.count();

        // Generate frame-rate based timestamp
        frame_count_++;
        uint64_t expected_us = static_cast<uint64_t>(frame_count_ * frame_duration_us_);

        // Use the more consistent of the two (frame-rate based vs elapsed time)
        // This handles frame rate variations while maintaining smooth timing
        uint64_t timestamp_us = std::min(expected_us, elapsed_us + static_cast<uint64_t>(frame_duration_us_));

        // Convert to timeval format for Live555
        fPresentationTime.tv_sec = timestamp_us / 1000000;
        fPresentationTime.tv_usec = timestamp_us % 1000000;

        // Add to initial base time to maintain absolute timestamps
        struct timeval initial_time;
        gettimeofday(&initial_time, NULL);
        fPresentationTime.tv_sec += (initial_time.tv_sec - timestamp_us / 1000000);

        // Periodic debug output
        if (frame_count_ % 250 == 0) { // Every ~10 seconds at 25fps
            LOG_DEBUG("Monotonic timestamp for frame " << frame_count_ << ": "
                     << fPresentationTime.tv_sec << "." << fPresentationTime.tv_usec
                     << " (elapsed: " << elapsed_us << "us, expected: " << expected_us << "us)");
        }
    }
}

// Template specializations for frame duration initialization
template<typename FrameType, typename Stream>
bool IMPDeviceSource<FrameType, Stream>::shouldThrottleDelivery() {
    auto now = std::chrono::steady_clock::now();

    if (last_delivery_time_.time_since_epoch().count() == 0) {
        // First delivery
        last_delivery_time_ = now;
        consecutive_fast_deliveries_ = 0;
        return false;
    }

    auto time_since_last = std::chrono::duration_cast<std::chrono::microseconds>(now - last_delivery_time_);
    double interval_us = time_since_last.count();

    // Check if we're delivering too fast
    if (interval_us < MIN_DELIVERY_INTERVAL_US) {
        consecutive_fast_deliveries_++;

        // Throttle if we've had multiple fast deliveries in a row
        if (consecutive_fast_deliveries_ > 1) { // Even more aggressive throttling
            LOG_WARN("THROTTLING delivery for " << name << " - interval: " << interval_us
                     << "us (min: " << MIN_DELIVERY_INTERVAL_US << "us), consecutive: " << consecutive_fast_deliveries_);
            return true;
        }
    } else {
        consecutive_fast_deliveries_ = 0;
    }

    last_delivery_time_ = now;
    return false;
}

template<>
void IMPDeviceSource<H264NALUnit, video_stream>::initializeFrameDuration() {
    if (stream && stream->stream && stream->stream->fps > 0) {
        frame_duration_us_ = 1000000.0 / stream->stream->fps;
        LOG_DEBUG("Video frame duration set to " << frame_duration_us_ << "us for " << stream->stream->fps << "fps");
    } else {
        frame_duration_us_ = 40000.0; // Default 25fps
        LOG_DEBUG("Using default video frame duration: 40000us (25fps)");
    }
}

template<>
void IMPDeviceSource<AudioFrame, audio_stream>::initializeFrameDuration() {
    // Audio frames are typically much more frequent
    frame_duration_us_ = 20000.0; // 50fps equivalent for audio frames
    LOG_DEBUG("Audio frame duration set to " << frame_duration_us_ << "us");
}

// Template instantiations are already done elsewhere, don't duplicate them
