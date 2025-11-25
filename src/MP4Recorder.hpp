#pragma once

#include <memory>
#include <string>
#include <mutex>
#include <vector>

#include "MP4Muxer.hpp"

class MP4Recorder {
public:
    MP4Recorder();
    ~MP4Recorder();

    bool start(const std::string &path, const MP4Muxer::InitParams &params);
    void stop();

    void writeVideo(const uint8_t *data, size_t size, int64_t pts_ms, bool isKey);
    void writeAudio(const uint8_t *data, size_t size, int64_t pts_ms);

    bool isActive() const { return active_; }

private:
    void closeUnlocked();

    mutable std::mutex mutex_;
    std::unique_ptr<MP4Muxer> muxer_;
    int fd_ = -1;
    bool active_ = false;
};
