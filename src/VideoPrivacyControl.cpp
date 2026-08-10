#include "VideoPrivacyControl.hpp"

#include "IMPEncoder.hpp"
#include "Logger.hpp"
#include "globals.hpp"
#include "imp_hal.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
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
constexpr const char *kPrivacyStatePath = "/run/prudynt/privacy.active";

void write_privacy_state_file() {
  int fd = ::open(kPrivacyStatePath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) {
    LOG_WARN("VideoPrivacyControl: failed to create state file "
             << kPrivacyStatePath);
    return;
  }
  const char *payload = "privacy=true\n";
  ssize_t ignored = ::write(fd, payload, strlen(payload));
  (void)ignored;
  ::close(fd);
}

void remove_privacy_state_file() {
  ::unlink(kPrivacyStatePath);
}

std::string trim(const std::string &value) {
  auto first =
      std::find_if_not(value.begin(), value.end(),
                       [](unsigned char c) { return std::isspace(c); });
  if (first == value.end()) {
    return {};
  }
  auto last = std::find_if_not(value.rbegin(), value.rend(),
                               [](unsigned char c) { return std::isspace(c); });
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
  std::transform(
      lower.begin(), lower.end(), lower.begin(),
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
    LOG_ERROR("VideoPrivacyControl: mkdir failed for " << kFifoDir << ": "
                                                       << strerror(errno));
    return;
  }
  ::unlink(kFifoPath);
  if (mkfifo(kFifoPath, 0660) < 0) {
    LOG_ERROR("VideoPrivacyControl: mkfifo failed for " << kFifoPath << ": "
                                                        << strerror(errno));
    return;
  }
}

void applyPrivacyToAllChannels(bool enabled) {
  for (int ch = 0; ch < NUM_VIDEO_CHANNELS; ++ch) {
    if (!global_video[ch]) continue;
    auto &vs = global_video[ch];

    if (enabled) {
      // -- Enable: create hardware OSD cover -------------------------
      vs->privacy_requested.store(true, std::memory_order_release);

      // Flush any buffered frames
      H264NALUnit dummy;
      if (vs->msgChannel) {
        while (vs->msgChannel->read(&dummy)) {}
      }
      {
        std::lock_guard<std::mutex> lock(vs->tap_mutex);
        for (auto &tap : vs->video_taps) {
          if (auto queue = tap.queue.lock()) {
            while (queue->read(&dummy)) {}
          }
        }
      }

      // Get stream dimensions
      int sw = vs->stream ? vs->stream->width : 1920;
      int sh = vs->stream ? vs->stream->height : 1080;
      if (sw <= 0) sw = 1920;
      if (sh <= 0) sh = 1080;

      // Create full-frame black cover region
      int encGrp = vs->encChn;  // OSD group = encoder channel
      IMPOSDRgnAttr rgnAttr{};
      rgnAttr.type = OSD_REG_COVER;
      rgnAttr.rect.p0.x = 0;
      rgnAttr.rect.p0.y = 0;
      rgnAttr.rect.p1.x = sw - 1;
      rgnAttr.rect.p1.y = sh - 1;
      rgnAttr.fmt = PIX_FMT_BGRA;
      rgnAttr.data.coverData.color = hal::osd::black_cover_color();

      LOG_DEBUG("VideoPrivacyControl: creating OSD cover for ch" << ch
                << " encGrp=" << encGrp
                << " fmt=" << (int)rgnAttr.fmt
                << " size=" << sw << "x" << sh);

      IMPRgnHandle handle = IMP_OSD_CreateRgn(&rgnAttr);
      if (handle == INVHANDLE) {
        LOG_ERROR("VideoPrivacyControl: IMP_OSD_CreateRgn failed for ch"
                  << ch << " encGrp=" << encGrp);
        continue;
      }
      LOG_DEBUG("VideoPrivacyControl: IMP_OSD_CreateRgn ok handle="
                << (int)handle << " ch" << ch);

      IMPOSDGrpRgnAttr grpAttr{};
      grpAttr.show = 1;
      int ret = IMP_OSD_RegisterRgn(handle, encGrp, &grpAttr);
      if (ret != 0) {
        LOG_ERROR("VideoPrivacyControl: IMP_OSD_RegisterRgn failed for ch"
                  << ch << " encGrp=" << encGrp << " ret=" << ret);
        IMP_OSD_DestroyRgn(handle);
        continue;
      }

      // Pin the cover's layer explicitly via SetGrpRgnAttr --- RegisterRgn does
      // not reliably apply it. Higher layer = nearer the front, so keeping the
      // cover one below the burned-in OSD timestamp (layer 2) lets the
      // timestamp composite on top while the cover still fully obscures the
      // video (it is opaque and full-frame, so no scene leaks). Only the
      // relative order matters --- these are the only two drawn regions.
      grpAttr.layer = 1;
      ret = IMP_OSD_SetGrpRgnAttr(handle, encGrp, &grpAttr);
      if (ret != 0) {
        LOG_WARN("VideoPrivacyControl: IMP_OSD_SetGrpRgnAttr failed for ch"
                 << ch << " encGrp=" << encGrp << " ret=" << ret);
      }

      ret = IMP_OSD_Start(encGrp);
      if (ret != 0) {
        LOG_WARN("VideoPrivacyControl: IMP_OSD_Start(" << encGrp
                << ") = " << ret);
      }

      vs->privacy_osd_handle = static_cast<int>(handle);
      // Request IDR so the cover appears in the next keyframe
      if (vs->running) {
        ret = IMP_Encoder_RequestIDR(ch);
        if (ret != 0) {
          LOG_WARN("VideoPrivacyControl: IMP_Encoder_RequestIDR(ch" << ch
                   << ") = " << ret);
        }
      }

      LOG_INFO("VideoPrivacyControl: OSD cover enabled on ch" << ch
               << " (" << sw << "x" << sh << ")");
    } else {
      // -- Disable: destroy OSD cover --------------------------------
      vs->privacy_requested.store(false, std::memory_order_release);

      if (vs->privacy_osd_handle >= 0) {
        int encGrp = vs->encChn;
        int ret;
        ret = IMP_OSD_ShowRgn((IMPRgnHandle)(intptr_t)vs->privacy_osd_handle,
                        encGrp, 0);
        if (ret != 0) {
          LOG_WARN("VideoPrivacyControl: IMP_OSD_ShowRgn(hide) ch" << ch
                   << " ret=" << ret);
        }
        ret = IMP_OSD_UnRegisterRgn((IMPRgnHandle)(intptr_t)vs->privacy_osd_handle,
                              encGrp);
        if (ret != 0) {
          LOG_WARN("VideoPrivacyControl: IMP_OSD_UnRegisterRgn ch" << ch
                   << " ret=" << ret);
        }
        IMP_OSD_DestroyRgn((IMPRgnHandle)(intptr_t)vs->privacy_osd_handle);
        vs->privacy_osd_handle = -1;
      }

      if (vs->running) {
        int ret = IMP_Encoder_RequestIDR(ch);
        if (ret != 0) {
          LOG_WARN("VideoPrivacyControl: IMP_Encoder_RequestIDR(ch" << ch
                   << ") = " << ret);
        }
      }

      LOG_INFO("VideoPrivacyControl: OSD cover disabled on ch" << ch);
    }
  }

  if (enabled) {
    write_privacy_state_file();
    LOG_INFO("VideoPrivacyControl: privacy enabled on all channels");
  } else {
    remove_privacy_state_file();
    LOG_INFO("VideoPrivacyControl: privacy disabled on all channels");
  }
}

void handleCommand(const std::string &line) {
  std::string trimmed = trim(line);
  if (trimmed.empty()) {
    return;
  }

  std::istringstream iss(trimmed);
  std::string verb;
  iss >> verb;
  std::transform(verb.begin(), verb.end(), verb.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });

  if (verb != "PRIVACY") {
    LOG_DEBUG("VideoPrivacyControl: ignoring verb " << verb);
    return;
  }

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
      std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
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

  // Always apply privacy to all channels simultaneously for security
  LOG_INFO("VideoPrivacyControl: handleCommand applying privacy="
           << (value ? "on" : "off"));
  applyPrivacyToAllChannels(value);

  // Persist state to config if save_state is enabled
  if (cfg && cfg->privacy.save_state) {
    cfg->privacy.enabled = value;
    cfg->updateConfig();
    LOG_INFO("VideoPrivacyControl: persisted privacy.enabled = "
             << (value ? "true" : "false"));
  }
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

void VideoPrivacyControl::applyStartupState() {
  LOG_INFO("VideoPrivacyControl::applyStartupState() ENTRY"
           << " cfg=" << (cfg ? "ptr" : "NULL")
           << " privacy.enabled="
           << (cfg ? (cfg->privacy.enabled ? "true" : "false") : "n/a"));

  if (!cfg) {
    LOG_WARN("VideoPrivacyControl::applyStartupState() cfg is NULL, skipping");
    return;
  }
  if (!cfg->privacy.enabled) {
    LOG_INFO("VideoPrivacyControl::applyStartupState() privacy.enabled=false, skipping");
    return;
  }

  LOG_INFO("VideoPrivacyControl: applying persisted privacy on startup");

  // Write to the FIFO so the privacy thread handles it (same path as the
  // shell 'privacy on' command).  Direct IMP_OSD calls from main would race
  // with encoder group init in the video worker threads.
  //
  // The FIFO thread may still be blocked on open(O_RDONLY) waiting for a
  // writer --- retry non-blocking until the channel is established.
  const char *cmd = "PRIVACY value=on\n";
  for (int attempt = 0; attempt < 50; ++attempt) {
    LOG_DEBUG("VideoPrivacyControl::applyStartupState attempt " << (attempt + 1));
    int fd = open(kFifoPath, O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
      LOG_INFO("VideoPrivacyControl: writing startup privacy command");
      ssize_t n = write(fd, cmd, strlen(cmd));
      LOG_INFO("VideoPrivacyControl: wrote " << n << " bytes to FIFO");
      close(fd);
      return;
    }
    LOG_DEBUG("VideoPrivacyControl::applyStartupState open errno=" << errno
              << " (" << strerror(errno) << ")");
    if (errno != ENXIO) {
      LOG_WARN("VideoPrivacyControl: cannot open FIFO " << kFifoPath
               << " for startup privacy: " << strerror(errno));
      return;
    }
    usleep(100000);  // 100 ms
  }
  LOG_WARN("VideoPrivacyControl: timed out waiting for FIFO reader");
}

void VideoPrivacyControl::run() {
  fifoLoop();
}
