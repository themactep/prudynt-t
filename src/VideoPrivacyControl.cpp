#include "VideoPrivacyControl.hpp"

#include "Logger.hpp"
#include "VideoPrivacyMask.hpp"
#include "globals.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define MODULE "VideoPrivacyControl"

namespace {
constexpr const char *kFifoDir = "/run/prudynt";
constexpr const char *kFifoPath = "/run/prudynt/video_ctrl";

std::string trim(const std::string &value) {
  auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
  if (first == value.end()) {
    return {};
  }
  auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); });
  return std::string(first, last.base());
}

bool parseInt(const std::string &token, int &value) {
  if (token.empty()) {
    return false;
  }
  char *end = nullptr;
  long parsed = std::strtol(token.c_str(), &end, 10);
  if (!end || *end != '\0') {
    return false;
  }
  value = static_cast<int>(parsed);
  return true;
}

bool parseBool(const std::string &token, bool &value) {
  std::string lower = token;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower == "true" || lower == "on" || lower == "yes" || lower == "1") {
    value = true;
    return true;
  }
  if (lower == "false" || lower == "off" || lower == "no" || lower == "0") {
    value = false;
    return true;
  }
  int numeric = 0;
  if (parseInt(token, numeric)) {
    value = (numeric != 0);
    return true;
  }
  return false;
}

void ensureFifo() {
  if (mkdir(kFifoDir, 0775) < 0 && errno != EEXIST) {
    LOG_ERROR("VideoPrivacyControl: mkdir failed for " << kFifoDir << ": " << strerror(errno));
    return;
  }
  ::unlink(kFifoPath);
  if (mkfifo(kFifoPath, 0660) < 0) {
    LOG_ERROR("VideoPrivacyControl: mkfifo failed for " << kFifoPath << ": " << strerror(errno));
    return;
  }
}

bool applyPrivacy(int channel, bool enabled) {
  if (channel < 0 || channel >= NUM_VIDEO_CHANNELS) {
    LOG_WARN("VideoPrivacyControl: channel " << channel << " out of range");
    return false;
  }
  auto stream = global_video[channel];
  if (!stream) {
    LOG_WARN("VideoPrivacyControl: channel " << channel << " is unavailable");
    return false;
  }
  stream->privacy_requested.store(enabled, std::memory_order_relaxed);

  std::shared_ptr<VideoPrivacyMask> mask;
  {
    std::lock_guard<std::mutex> lock(stream->privacy_mutex);
    mask = stream->privacy_mask;
  }

  if (mask && mask->isReady()) {
    if (!mask->setEnabled(enabled)) {
      LOG_WARN("VideoPrivacyControl: failed to set privacy " << enabled << " on channel " << channel);
      return false;
    }
    LOG_INFO("VideoPrivacyControl: channel " << channel << (enabled ? " muted" : " restored"));
    return true;
  }

  LOG_INFO("VideoPrivacyControl: channel " << channel << " privacy=" << (enabled ? 1 : 0) << " pending worker init");
  return true;
}

void handleCommand(const std::string &line) {
  std::string trimmed = trim(line);
  if (trimmed.empty()) {
    return;
  }

  std::istringstream iss(trimmed);
  std::string verb;
  iss >> verb;
  std::transform(verb.begin(), verb.end(), verb.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

  if (verb != "PRIVACY") {
    LOG_DEBUG("VideoPrivacyControl: ignoring verb " << verb);
    return;
  }

  int channel = 0;
  bool channel_set = false;
  bool apply_all = false;
  bool value = false;
  bool value_set = false;

  std::string token;
  while (iss >> token) {
    auto eq = token.find('=');
    std::string key;
    std::string val;
    if (eq == std::string::npos) {
      key.clear();
      val = token;
    } else {
      key = token.substr(0, eq);
      val = token.substr(eq + 1);
      std::transform(key.begin(), key.end(), key.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    if (key == "ch" || key == "channel") {
      std::string val_lower = val;
      std::transform(val_lower.begin(), val_lower.end(), val_lower.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

      if (val_lower == "all" || val_lower == "both" || val_lower == "*") {
        apply_all = true;
        channel_set = true;
      } else if (parseInt(val, channel)) {
        channel_set = true;
      } else {
        LOG_WARN("VideoPrivacyControl: invalid channel token '" << token << "'");
      }
      continue;
    }

    if (key == "value" || key == "state" || key.empty()) {
      if (parseBool(val, value)) {
        value_set = true;
      } else {
        LOG_WARN("VideoPrivacyControl: invalid value token '" << token << "'");
      }
      continue;
    }
  }

  if (!value_set) {
    LOG_WARN("VideoPrivacyControl: PRIVACY missing value");
    return;
  }

  if (!channel_set) {
    apply_all = true;
  }

  if (apply_all) {
    for (int ch = 0; ch < NUM_VIDEO_CHANNELS; ++ch) {
      applyPrivacy(ch, value);
    }
    return;
  }

  applyPrivacy(channel, value);
}

void fifoLoop() {
  ensureFifo();
  LOG_INFO("VideoPrivacyControl: FIFO loop starting at " << kFifoPath);
  while (!global_shutdown_requested.load(std::memory_order_relaxed)) {
    int fd = open(kFifoPath, O_RDONLY);
    if (fd < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    std::string buffer;
    buffer.reserve(256);
    char chunk[256];

    while (!global_shutdown_requested.load(std::memory_order_relaxed)) {
      ssize_t bytes = read(fd, chunk, sizeof(chunk));
      if (bytes <= 0) {
        break;
      }
      buffer.append(chunk, static_cast<size_t>(bytes));
      size_t pos = 0;
      while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        handleCommand(line);
      }
    }

    close(fd);
  }
}

} // namespace

void VideoPrivacyControl::run() {
  fifoLoop();
}
