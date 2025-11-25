#include "MP4Recorder.hpp"

#include <fcntl.h>
#include <unistd.h>

#include "Logger.hpp"
#include "MP4MuxerFactory.hpp"

#undef MODULE
#define MODULE "MP4Recorder"

MP4Recorder::MP4Recorder() = default;

MP4Recorder::~MP4Recorder() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeUnlocked();
}

bool MP4Recorder::start(const std::string &path, const MP4Muxer::InitParams &params) {
    std::lock_guard<std::mutex> lock(mutex_);

    closeUnlocked();

    fd_ = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd_ < 0) {
        LOG_ERROR("failed to open MP4 file '" << path << "'");
        return false;
    }

    muxer_.reset(CreateMP4Muxer());
    if (!muxer_) {
        LOG_ERROR("no MP4 muxer backend available");
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    if (!muxer_->init(params)) {
        LOG_ERROR("MP4 muxer init failed");
        DestroyMP4Muxer(muxer_.release());
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    std::vector<uint8_t> initSeg = muxer_->getInitSegment();
    if (!initSeg.empty()) {
        ssize_t w = ::write(fd_, initSeg.data(), initSeg.size());
        (void)w;
    }

    active_ = true;
    return true;
}

void MP4Recorder::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeUnlocked();
}

void MP4Recorder::writeVideo(const uint8_t *data, size_t size, int64_t pts_ms, bool isKey) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || !muxer_ || fd_ < 0) return;

    std::vector<uint8_t> frag = muxer_->muxVideo(data, size, pts_ms, isKey);
    if (!frag.empty()) {
        ssize_t w = ::write(fd_, frag.data(), frag.size());
        (void)w;
    }
}

void MP4Recorder::writeAudio(const uint8_t *data, size_t size, int64_t pts_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || !muxer_ || fd_ < 0) return;

    std::vector<uint8_t> frag = muxer_->muxAudio(data, size, pts_ms);
    if (!frag.empty()) {
        ssize_t w = ::write(fd_, frag.data(), frag.size());
        (void)w;
    }
}

void MP4Recorder::closeUnlocked() {
    if (muxer_) {
        muxer_->close();
        DestroyMP4Muxer(muxer_.release());
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    active_ = false;
}
