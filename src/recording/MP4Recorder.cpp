#include "recording/MP4Recorder.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "util/Logger.hpp"
#include "recording/MP4MuxerFactory.hpp"

#undef MODULE
#define MODULE "MP4Recorder"

namespace {

/* Write all of data to fd, looping over short writes. Returns 0 on
 * success or -errno on the first failure. A soft NFS mount turns a
 * stalled write into EIO; callers must not ignore it. */
int write_all(int fd, const uint8_t *data, size_t len)
{
  size_t off = 0;
  while (off < len) {
    ssize_t w = ::write(fd, data + off, len - off);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return -errno;
    }
    off += static_cast<size_t>(w);
  }
  return 0;
}

/* Same as write_all(), but at an absolute offset. Used to patch the durations
 * into the moov written at the start of the segment. */
int pwrite_all(int fd, const uint8_t *data, size_t len, off_t off) {
  size_t done = 0;
  while (done < len) {
    ssize_t w =
        ::pwrite(fd, data + done, len - done, off + static_cast<off_t>(done));
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return -errno;
    }
    done += static_cast<size_t>(w);
  }
  return 0;
}

} // namespace

MP4Recorder::MP4Recorder() = default;

MP4Recorder::~MP4Recorder() {
  std::lock_guard<std::mutex> lock(mutex_);
  closeUnlocked();
}

bool MP4Recorder::start(const std::string &path,
                        const MP4Muxer::InitParams &params) {
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
    int rc = write_all(fd_, initSeg.data(), initSeg.size());
    if (rc < 0) {
      LOG_ERROR("failed to write MP4 init segment: " << std::strerror(-rc));
      DestroyMP4Muxer(muxer_.release());
      ::close(fd_);
      fd_ = -1;
      return false;
    }
  }

  generation_.fetch_add(1, std::memory_order_release);
  active_.store(true, std::memory_order_release);
  return true;
}

void MP4Recorder::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  closeUnlocked();
}

void MP4Recorder::writeVideo(const uint8_t *data, size_t size, int64_t pts_ms,
                             bool isKey) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_ || !muxer_ || fd_ < 0)
    return;

  std::vector<uint8_t> frag = muxer_->muxVideo(data, size, pts_ms, isKey);
  if (!frag.empty()) {
    int rc = write_all(fd_, frag.data(), frag.size());
    if (rc < 0) {
      LOG_ERROR("failed to write video fragment: " << std::strerror(-rc));
      abortUnlocked();
      return;
    }
  }
}

void MP4Recorder::writeAudio(const uint8_t *data, size_t size, int64_t pts_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_ || !muxer_ || fd_ < 0)
    return;

  std::vector<uint8_t> frag = muxer_->muxAudio(data, size, pts_ms);
  if (!frag.empty()) {
    int rc = write_all(fd_, frag.data(), frag.size());
    if (rc < 0) {
      LOG_ERROR("failed to write audio fragment: " << std::strerror(-rc));
      abortUnlocked();
      return;
    }
  }
}

void MP4Recorder::closeUnlocked() {
  if (muxer_ && fd_ >= 0) {
    std::vector<uint8_t> initSeg = muxer_->getFinalInitSegment();
    if (!initSeg.empty()) {
      int rc = pwrite_all(fd_, initSeg.data(), initSeg.size(), 0);
      if (rc < 0) {
        LOG_ERROR("failed to patch MP4 header: " << std::strerror(-rc));
      }
    }
  }
  if (muxer_) {
    muxer_->close();
    DestroyMP4Muxer(muxer_.release());
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  active_.store(false, std::memory_order_release);
  aborted_.store(false, std::memory_order_release);
}

/* Stop the segment after a write failure. The muxer is left for
 * stop()/closeUnlocked() to destroy, so the loop sees an inactive
 * recorder and starts a fresh segment instead of writing into a file
 * that already has a hole in it. aborted_ stays set until then so the
 * control socket can still tell an aborted segment from one that was
 * never started and clean up after it (state file, active count). */
void MP4Recorder::abortUnlocked() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  active_.store(false, std::memory_order_release);
  aborted_.store(true, std::memory_order_release);
}
