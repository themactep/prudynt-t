#include "MP4ControlSocket.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "Config.hpp"
#include "Logger.hpp"
#include "MP4Muxer.hpp"
#include "globals.hpp"

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>

namespace {
namespace fs = std::filesystem;
constexpr const char *FIFO_DIR = "/run/prudynt";
constexpr const char *FIFO_PATH = "/run/prudynt/mp4ctl";

std::string channel_state_path(int channel) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%s/mp4ctl-ch%d.active", FIFO_DIR, channel);
  return std::string(buffer);
}

void write_channel_state_file(int channel, const std::string &record_path, int duration_seconds) {
  if (channel < 0 || channel >= NUM_VIDEO_CHANNELS) {
    return;
  }
  auto state_path = channel_state_path(channel);
  int fd = ::open(state_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) {
    LOG_WARN("MP4ControlSocket: failed to update state file " << state_path);
    return;
  }
  std::string payload = "path=" + record_path + "\n";
  if (duration_seconds > 0) {
    payload += "duration=" + std::to_string(duration_seconds) + "\n";
  }
  ssize_t ignored = ::write(fd, payload.c_str(), payload.size());
  (void)ignored;
  ::close(fd);
}

void remove_channel_state_file(int channel) {
  if (channel < 0 || channel >= NUM_VIDEO_CHANNELS) {
    return;
  }
  auto state_path = channel_state_path(channel);
  ::unlink(state_path.c_str());
}

struct StartCommandOptions {
  std::string path;
  std::string mount;
  std::string directory;
  std::string nameTemplate;
  int durationSeconds{0};
  int channel{0};
  bool loop{false};
  bool durationProvided{false};
  bool channelProvided{false};
  bool mountProvided{false};
  bool directoryProvided{false};
  bool nameTemplateProvided{false};
};

struct RecordingLoopParams {
  std::string mount;
  std::string directory;
  std::string nameTemplate;
  int durationSeconds{0};
  int channel{0};
};

struct LoopState {
  RecordingLoopParams params;
  std::atomic<bool> stopRequested{false};
};

std::array<std::shared_ptr<LoopState>, NUM_VIDEO_CHANNELS> loop_states;
std::array<std::thread, NUM_VIDEO_CHANNELS> loop_threads;
std::mutex loop_state_mutex;

std::chrono::system_clock::time_point round_up_to_minute(std::chrono::system_clock::time_point tp) {
  auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(tp);
  auto epoch_seconds = seconds.time_since_epoch();
  auto remainder = epoch_seconds.count() % 60;
  if (remainder == 0) {
    return seconds;
  }
  return seconds + std::chrono::seconds(60 - remainder);
}

bool sleep_until_time(std::chrono::system_clock::time_point target, std::atomic<bool> *stop_flag = nullptr) {
  while (true) {
    if (stop_flag && stop_flag->load(std::memory_order_relaxed)) {
      return false;
    }
    auto now = std::chrono::system_clock::now();
    if (now >= target) {
      return true;
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(target - now);
    if (remaining > std::chrono::milliseconds(250)) {
      remaining = std::chrono::milliseconds(250);
    }
    std::this_thread::sleep_for(remaining);
  }
}

struct MountEntry {
  fs::path mountPoint;
  std::string fsType;
  bool readWrite{false};
  bool valid{false};
  size_t matchLength{0};
};

std::string decode_mount_token(const std::string &token) {
  std::string out;
  out.reserve(token.size());
  for (size_t i = 0; i < token.size(); ++i) {
    char c = token[i];
    if (c == '\\' && i + 3 < token.size() && std::isdigit(static_cast<unsigned char>(token[i + 1])) &&
        std::isdigit(static_cast<unsigned char>(token[i + 2])) &&
        std::isdigit(static_cast<unsigned char>(token[i + 3]))) {
      int value = (token[i + 1] - '0') * 64 + (token[i + 2] - '0') * 8 + (token[i + 3] - '0');
      out.push_back(static_cast<char>(value));
      i += 3;
      continue;
    }
    out.push_back(c);
  }
  return out;
}

bool path_has_prefix(const fs::path &path, const fs::path &prefix) {
  auto path_str = path.generic_string();
  auto prefix_str = prefix.generic_string();
  if (prefix_str.empty()) {
    return false;
  }
  if (prefix_str == "/") {
    return true;
  }
  if (path_str.size() < prefix_str.size()) {
    return false;
  }
  if (path_str.compare(0, prefix_str.size(), prefix_str) != 0) {
    return false;
  }
  if (path_str.size() == prefix_str.size()) {
    return true;
  }
  if (prefix_str.back() == '/') {
    return true;
  }
  return path_str[prefix_str.size()] == '/';
}

bool mount_options_include_rw(const std::string &options) {
  std::stringstream ss(options);
  std::string opt;
  while (std::getline(ss, opt, ',')) {
    if (opt == "rw") {
      return true;
    }
    if (opt == "ro") {
      return false;
    }
  }
  return false;
}

MountEntry find_mount_for_path(const fs::path &input) {
  MountEntry best;
  fs::path normalized = input;
  if (!normalized.is_absolute()) {
    normalized = fs::absolute(normalized);
  }
  normalized = normalized.lexically_normal();

  std::ifstream mounts("/proc/mounts");
  if (!mounts) {
    return best;
  }

  std::string line;
  while (std::getline(mounts, line)) {
    if (line.empty()) {
      continue;
    }
    std::istringstream iss(line);
    std::string device;
    std::string mount_point;
    std::string fs_type;
    std::string options;
    if (!(iss >> device >> mount_point >> fs_type >> options)) {
      continue;
    }
    fs::path mp(decode_mount_token(mount_point));
    mp = mp.lexically_normal();
    if (mp.empty()) {
      continue;
    }
    if (!path_has_prefix(normalized, mp)) {
      continue;
    }
    size_t depth = mp.generic_string().size();
    if (!best.valid || depth > best.matchLength) {
      best.mountPoint = mp;
      best.fsType = fs_type;
      best.readWrite = mount_options_include_rw(options);
      best.valid = true;
      best.matchLength = depth;
    }
  }
  return best;
}

bool validate_mount_entry(const MountEntry &entry, std::string &reason) {
  if (!entry.valid) {
    reason = "path is not located on an active mount";
    return false;
  }
  if (!entry.readWrite) {
    reason = "target filesystem is read-only";
    return false;
  }
  if (entry.fsType == "overlay") {
    reason = "overlay filesystem is not allowed for recordings";
    return false;
  }
  return true;
}

bool validate_path_mount(const fs::path &path, std::string &reason) {
  auto entry = find_mount_for_path(path);
  return validate_mount_entry(entry, reason);
}

bool validate_mount_point_path(fs::path mount_path, std::string &reason) {
  if (mount_path.empty()) {
    reason = "mount path is empty";
    return false;
  }
  if (!mount_path.is_absolute()) {
    mount_path = fs::absolute(mount_path);
  }
  mount_path = mount_path.lexically_normal();
  auto entry = find_mount_for_path(mount_path);
  if (!entry.valid || entry.mountPoint != mount_path) {
    reason = "mount point '" + mount_path.string() + "' is not active";
    return false;
  }
  return validate_mount_entry(entry, reason);
}

bool start_recording(const std::string &path, int target_channel);
void stop_recording(int channel);

std::mutex stop_timer_mutex;
std::array<std::thread, NUM_VIDEO_CHANNELS> stop_timer_threads;
std::array<std::atomic<bool>, NUM_VIDEO_CHANNELS> stop_timer_cancel;
struct StopTimerInitializer {
  StopTimerInitializer() {
    for (auto &flag : stop_timer_cancel) {
      flag.store(false, std::memory_order_relaxed);
    }
  }
} stop_timer_initializer;

std::shared_ptr<video_stream> wait_for_video_channel(int channel, std::chrono::milliseconds timeout) {
  if (channel < 0 || channel >= NUM_VIDEO_CHANNELS) {
    return nullptr;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto video = global_video[channel];
    if (video) {
      return video;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return global_video[channel];
}

bool snapshot_codec_config(std::vector<uint8_t> &sps, std::vector<uint8_t> &pps, int channel = -1) {
  auto snapshot_from_channel = [&](int ch) -> bool {
    if (ch < 0 || ch >= NUM_VIDEO_CHANNELS) {
      return false;
    }
    auto &vs = global_video[ch];
    if (!vs) {
      return false;
    }
    std::lock_guard<std::mutex> lock(vs->codec_config_mutex);
    if (vs->have_sps && vs->have_pps && !vs->latest_sps.empty() && !vs->latest_pps.empty()) {
      sps = vs->latest_sps;
      pps = vs->latest_pps;
      return true;
    }
    return false;
  };

  if (channel >= 0) {
    return snapshot_from_channel(channel);
  }

  for (int ch = 0; ch < NUM_VIDEO_CHANNELS; ++ch) {
    if (snapshot_from_channel(ch)) {
      return true;
    }
  }
  return false;
}

bool wait_for_codec_config(std::vector<uint8_t> &sps, std::vector<uint8_t> &pps, std::chrono::milliseconds timeout,
                           int channel = -1) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (snapshot_codec_config(sps, pps, channel)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

bool build_avcc(const std::vector<uint8_t> &sps, const std::vector<uint8_t> &pps, std::vector<uint8_t> &avcC) {
  if (sps.size() < 4 || pps.empty()) {
    return false;
  }

  avcC.clear();
  avcC.reserve(11 + sps.size() + pps.size());
  avcC.push_back(0x01);
  avcC.push_back(sps[1]);
  avcC.push_back(sps[2]);
  avcC.push_back(sps[3]);
  avcC.push_back(0xFF);
  avcC.push_back(0xE1);
  uint16_t sps_len = static_cast<uint16_t>(sps.size());
  avcC.push_back(static_cast<uint8_t>((sps_len >> 8) & 0xFF));
  avcC.push_back(static_cast<uint8_t>(sps_len & 0xFF));
  avcC.insert(avcC.end(), sps.begin(), sps.end());

  avcC.push_back(0x01);
  uint16_t pps_len = static_cast<uint16_t>(pps.size());
  avcC.push_back(static_cast<uint8_t>((pps_len >> 8) & 0xFF));
  avcC.push_back(static_cast<uint8_t>(pps_len & 0xFF));
  avcC.insert(avcC.end(), pps.begin(), pps.end());
  return true;
}

bool build_aac_config(std::vector<uint8_t> &aacConfig) {
  if (!cfg || !cfg->audio.input_enabled || std::strcmp(cfg->audio.input_format, "AAC") != 0) {
    return false;
  }

  static constexpr int sample_rate_table[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                              22050, 16000, 12000, 11025, 8000,  7350};
  int sample_rate = cfg->audio.input_sample_rate;
  int sample_rate_index = -1;
  for (int i = 0; i < static_cast<int>(sizeof(sample_rate_table) / sizeof(sample_rate_table[0])); ++i) {
    if (sample_rate_table[i] == sample_rate) {
      sample_rate_index = i;
      break;
    }
  }

#if defined(LIB_AUDIO_PROCESSING)
  int channels = cfg->audio.force_stereo ? 2 : 1;
#else
  int channels = 1;
#endif
  if (channels < 1 || channels > 7) {
    LOG_ERROR("MP4ControlSocket: unsupported channel count for AAC "
              "AudioSpecificConfig");
    return false;
  }

  uint8_t audioObjectType = 2; // AAC LC
  auto append_bits = [&](uint32_t value, int bits, uint8_t &current_byte, int &bit_count, std::vector<uint8_t> &out) {
    for (int i = bits - 1; i >= 0; --i) {
      current_byte = static_cast<uint8_t>((current_byte << 1) | ((value >> i) & 0x01));
      bit_count++;
      if (bit_count == 8) {
        out.push_back(current_byte);
        bit_count = 0;
        current_byte = 0;
      }
    }
  };
  auto finalize_bits = [&](uint8_t &current_byte, int &bit_count, std::vector<uint8_t> &out) {
    if (bit_count > 0) {
      current_byte <<= (8 - bit_count);
      out.push_back(current_byte);
      current_byte = 0;
      bit_count = 0;
    }
  };

  aacConfig.clear();
  uint8_t current_byte = 0;
  int bit_count = 0;
  append_bits(audioObjectType, 5, current_byte, bit_count, aacConfig);
  if (sample_rate_index >= 0) {
    append_bits(static_cast<uint32_t>(sample_rate_index), 4, current_byte, bit_count, aacConfig);
  } else {
    append_bits(0x0F, 4, current_byte, bit_count, aacConfig);
    append_bits(static_cast<uint32_t>(sample_rate), 24, current_byte, bit_count, aacConfig);
  }
  append_bits(static_cast<uint32_t>(channels), 4, current_byte, bit_count, aacConfig);
  finalize_bits(current_byte, bit_count, aacConfig);
  return true;
}

bool start_recording(const std::string &path, int target_channel) {
  if (!cfg) {
    return false;
  }

  if (target_channel < 0 || target_channel >= NUM_VIDEO_CHANNELS) {
    LOG_ERROR("MP4ControlSocket: invalid channel " << target_channel);
    return false;
  }

  auto video = wait_for_video_channel(target_channel, std::chrono::milliseconds(2000));
  if (!video) {
    LOG_ERROR("MP4ControlSocket: video channel " << target_channel << " not available (timed out waiting)");
    return false;
  }

  auto &recorder = global_mp4_recorders[target_channel];
  if (recorder.isActive()) {
    LOG_WARN("MP4ControlSocket: recorder already active on channel " << target_channel);
    return false;
  }

  auto disable_force_if_idle = []() {
    if (global_mp4_active_recorders.load(std::memory_order_relaxed) == 0) {
      global_force_video_active.store(false, std::memory_order_relaxed);
    }
  };
  auto reset_wait_state = [&]() { video->mp4_waiting_for_idr.store(false, std::memory_order_relaxed); };

  global_force_video_active.store(true, std::memory_order_relaxed);
  for (int i = 0; i < NUM_VIDEO_CHANNELS; ++i) {
    auto worker = global_video[i];
    if (!worker) {
      continue;
    }
    worker->should_grab_frames.notify_one();
    bool is_target = (i == target_channel);
    worker->mp4_waiting_for_idr.store(is_target, std::memory_order_relaxed);
    if (is_target) {
      int64_t last_idr = worker->mp4_last_idr_ts.load(std::memory_order_relaxed);
      worker->mp4_required_idr_ts.store(last_idr, std::memory_order_relaxed);
      auto now_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count();
      worker->mp4_last_idr_request_ms.store(static_cast<uint64_t>(now_ms), std::memory_order_relaxed);
      IMP_Encoder_RequestIDR(worker->encChn);
    }
  }

  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
  if (!wait_for_codec_config(sps, pps, std::chrono::milliseconds(1500), target_channel)) {
    LOG_ERROR("MP4ControlSocket: timed out waiting for SPS/PPS before START");
    reset_wait_state();
    disable_force_if_idle();
    return false;
  }

  const _stream *stream_cfg = nullptr;
  if (target_channel == 0) {
    stream_cfg = &cfg->stream0;
  } else if (target_channel == 1) {
    stream_cfg = &cfg->stream1;
  }

  MP4Muxer::InitParams init{};
  if (stream_cfg) {
    init.width = stream_cfg->width;
    init.height = stream_cfg->height;
    init.fps = stream_cfg->fps;
  } else {
    init.width = cfg->stream0.width;
    init.height = cfg->stream0.height;
    init.fps = cfg->stream0.fps;
  }
  init.sampleRate = 0;
  init.channels = 0;
  if (cfg->audio.input_enabled) {
#if defined(LIB_AUDIO_PROCESSING)
    init.channels = cfg->audio.force_stereo ? 2 : 1;
#else
    init.channels = 1;
#endif
    init.sampleRate = cfg->audio.input_sample_rate;
    if (std::strcmp(cfg->audio.input_format, "AAC") == 0) {
      if (!build_aac_config(init.aacConfig)) {
        init.channels = 0;
        init.sampleRate = 0;
      }
    } else {
      // PCM/OPUS/G711 currently unsupported in MP4 recorder
      init.channels = 0;
      init.sampleRate = 0;
    }
  }

  if (!build_avcc(sps, pps, init.avcC)) {
    LOG_ERROR("MP4ControlSocket: failed to build avcC from SPS/PPS");
    reset_wait_state();
    disable_force_if_idle();
    return false;
  }

  bool ok = recorder.start(path.c_str(), init);
  if (ok) {
    global_mp4_active_recorders.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO("MP4ControlSocket: recorder started with avcC payload size=" << init.avcC.size() << " on channel "
                                                                          << target_channel);
  } else {
    reset_wait_state();
    disable_force_if_idle();
  }
  return ok;
}

void stop_recording(int channel) {
  if (channel < 0 || channel >= NUM_VIDEO_CHANNELS) {
    return;
  }

  auto &recorder = global_mp4_recorders[channel];
  if (!recorder.isActive()) {
    return;
  }

  recorder.stop();
  if (global_video[channel]) {
    global_video[channel]->mp4_waiting_for_idr.store(false);
  }
  remove_channel_state_file(channel);

  int remaining = global_mp4_active_recorders.fetch_sub(1, std::memory_order_relaxed) - 1;
  if (remaining <= 0) {
    global_mp4_active_recorders.store(0, std::memory_order_relaxed);
    global_force_video_active.store(false, std::memory_order_relaxed);
  }
  LOG_INFO("MP4ControlSocket: recorder stopped for channel " << channel);
}

void stop_all_recordings() {
  for (int ch = 0; ch < NUM_VIDEO_CHANNELS; ++ch) {
    stop_recording(ch);
  }
}

void cancel_stop_timer(int channel) {
  if (channel < 0 || channel >= NUM_VIDEO_CHANNELS) {
    return;
  }

  std::thread to_join;
  {
    std::lock_guard<std::mutex> lock(stop_timer_mutex);
    if (stop_timer_threads[channel].joinable()) {
      stop_timer_cancel[channel].store(true, std::memory_order_relaxed);
      to_join = std::move(stop_timer_threads[channel]);
    }
  }
  if (to_join.joinable()) {
    to_join.join();
  }
  stop_timer_cancel[channel].store(false, std::memory_order_relaxed);
}

void cancel_all_stop_timers() {
  for (int ch = 0; ch < NUM_VIDEO_CHANNELS; ++ch) {
    cancel_stop_timer(ch);
  }
}

void schedule_stop_timer(int channel, int duration_seconds) {
  if (channel < 0 || channel >= NUM_VIDEO_CHANNELS || duration_seconds <= 0) {
    return;
  }

  cancel_stop_timer(channel);
  {
    std::lock_guard<std::mutex> lock(stop_timer_mutex);
    stop_timer_cancel[channel].store(false, std::memory_order_relaxed);
    stop_timer_threads[channel] = std::thread([channel, duration_seconds]() {
      LOG_INFO("MP4ControlSocket: auto-stop timer scheduled for " << duration_seconds << " seconds on channel "
                                                                  << channel);
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_seconds);
      while (!stop_timer_cancel[channel].load(std::memory_order_relaxed) &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (!stop_timer_cancel[channel].load(std::memory_order_relaxed)) {
        LOG_INFO("MP4ControlSocket: auto-stop timer elapsed for channel " << channel);
        stop_recording(channel);
      }
    });
  }
}

bool parse_int_token(const std::string &token, int &value) {
  if (token.empty()) {
    return false;
  }
  char *end = nullptr;
  long v = std::strtol(token.c_str(), &end, 10);
  if (*end != '\0') {
    return false;
  }
  value = static_cast<int>(v);
  return true;
}

bool parse_bool_token(const std::string &token, bool &value) {
  if (token.empty()) {
    return false;
  }
  if (token == "1" || token == "true" || token == "TRUE" || token == "yes" || token == "on") {
    value = true;
    return true;
  }
  if (token == "0" || token == "false" || token == "FALSE" || token == "no" || token == "off") {
    value = false;
    return true;
  }
  return false;
}

std::string default_string(const char *value, const char *fallback) {
  if (value && *value) {
    return value;
  }
  return fallback;
}

int default_recorder_duration() {
  if (cfg && cfg->recorder.duration > 0) {
    return cfg->recorder.duration;
  }
  return 60;
}

int default_recorder_channel() {
  if (cfg) {
    return cfg->recorder.channel;
  }
  return 0;
}

std::string default_recorder_mount() {
  if (cfg) {
    return default_string(cfg->recorder.mount, "/mnt/mmc");
  }
  return "/mnt/mmc";
}

std::string default_recorder_directory() {
  if (cfg) {
    return default_string(cfg->recorder.device_path, "%hostname");
  }
  return "%hostname";
}

std::string default_recorder_template() {
  if (cfg) {
    return default_string(cfg->recorder.filename, "%Y/%m/%d/%H-%M-%S");
  }
  return "%Y/%m/%d/%H-%M-%S";
}

RecordingLoopParams build_loop_params(const StartCommandOptions &options, bool *ok_out = nullptr) {
  bool ok = true;
  RecordingLoopParams params;
  params.channel = options.channel;
  if (params.channel < 0 || params.channel >= NUM_VIDEO_CHANNELS) {
    params.channel = default_recorder_channel();
  }
  params.durationSeconds =
      (options.durationProvided && options.durationSeconds > 0) ? options.durationSeconds : default_recorder_duration();
  params.mount = options.mountProvided ? options.mount : default_recorder_mount();
  params.directory = options.directoryProvided ? options.directory : default_recorder_directory();
  params.nameTemplate = options.nameTemplateProvided ? options.nameTemplate : default_recorder_template();
  if (params.durationSeconds <= 0) {
    ok = false;
  }
  if (params.mount.empty()) {
    ok = false;
  }
  if (params.nameTemplate.empty()) {
    ok = false;
  }
  if (ok_out) {
    *ok_out = ok;
  }
  return params;
}

bool ensure_parent_directory(const fs::path &path) {
  auto parent = path.parent_path();
  if (parent.empty()) {
    return true;
  }
  std::error_code ec;
  if (fs::exists(parent, ec)) {
    return !ec;
  }
  fs::create_directories(parent, ec);
  if (ec) {
    LOG_WARN("MP4ControlSocket: failed to create directories for " << parent << " error=" << ec.message());
    return false;
  }
  return true;
}

std::string format_segment_name(const std::string &templ,
                                std::chrono::system_clock::time_point reference = std::chrono::system_clock::now()) {
  auto tt = std::chrono::system_clock::to_time_t(reference);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &tt);
#else
  localtime_r(&tt, &tm_buf);
#endif
  char buffer[256];
  if (std::strftime(buffer, sizeof(buffer), templ.c_str(), &tm_buf) == 0) {
    return "segment";
  }
  return buffer;
}

std::string build_loop_target_path(const RecordingLoopParams &params,
                                   std::chrono::system_clock::time_point reference = std::chrono::system_clock::now()) {
  std::string reason;
  if (!validate_mount_point_path(fs::path(params.mount), reason)) {
    LOG_ERROR("MP4ControlSocket: refusing to record on mount '" << params.mount << "': " << reason);
    return {};
  }
  fs::path root(params.mount);
  if (!params.directory.empty()) {
    root /= params.directory;
  }
  std::string name = format_segment_name(params.nameTemplate, reference);
  if (name.size() < 4 || name.substr(name.size() - 4) != ".mp4") {
    name += ".mp4";
  }
  fs::path fullPath = root / name;
  if (!ensure_parent_directory(fullPath)) {
    return {};
  }
  return fullPath.string();
}

bool wait_until_channel_idle(int channel, std::atomic<bool> *stop_flag = nullptr) {
  if (channel < 0 || channel >= NUM_VIDEO_CHANNELS) {
    return false;
  }
  while (global_mp4_recorders[channel].isActive()) {
    if (stop_flag && stop_flag->load(std::memory_order_relaxed)) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return true;
}

bool wait_for_recording_completion(int channel, std::atomic<bool> *stop_flag = nullptr) {
  if (channel < 0 || channel >= NUM_VIDEO_CHANNELS) {
    return false;
  }
  while (global_mp4_recorders[channel].isActive()) {
    if (stop_flag && stop_flag->load(std::memory_order_relaxed)) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return true;
}

bool begin_segment(const std::string &path, int channel, int duration_seconds) {
  if (path.empty()) {
    LOG_ERROR("MP4ControlSocket: cannot START without a target path");
    return false;
  }
  std::string reason;
  if (!validate_path_mount(fs::path(path), reason)) {
    LOG_ERROR("MP4ControlSocket: refusing to record to '" << path << "': " << reason);
    return false;
  }
  bool ok = start_recording(path, channel);
  if (!ok) {
    return false;
  }
  write_channel_state_file(channel, path, duration_seconds);
  if (duration_seconds > 0) {
    schedule_stop_timer(channel, duration_seconds);
  }
  return true;
}

void stop_loop_locked(int channel) {
  if (channel < 0 || channel >= NUM_VIDEO_CHANNELS) {
    return;
  }
  if (!loop_states[channel]) {
    return;
  }
  loop_states[channel]->stopRequested.store(true, std::memory_order_relaxed);
  if (loop_threads[channel].joinable()) {
    loop_threads[channel].join();
  }
  loop_states[channel].reset();
}

void stop_loop_for_channel(int channel) {
  std::lock_guard<std::mutex> lock(loop_state_mutex);
  stop_loop_locked(channel);
}

void stop_all_loops() {
  std::lock_guard<std::mutex> lock(loop_state_mutex);
  for (int ch = 0; ch < NUM_VIDEO_CHANNELS; ++ch) {
    stop_loop_locked(ch);
  }
}

void loop_worker(int channel, std::shared_ptr<LoopState> state) {
  if (!state) {
    return;
  }

  constexpr auto kMinute = std::chrono::seconds(60);
  constexpr auto kShortClipThreshold = std::chrono::seconds(15);

  bool first_segment_pending = true;
  auto next_start = std::chrono::system_clock::now();

  while (!state->stopRequested.load(std::memory_order_relaxed)) {
    int segment_duration_seconds = state->params.durationSeconds;
    std::chrono::system_clock::time_point segment_start;

    if (first_segment_pending) {
      segment_start = std::chrono::system_clock::now();
      auto boundary = round_up_to_minute(segment_start);
      if (boundary <= segment_start) {
        boundary += kMinute;
      }
      auto span = std::chrono::duration_cast<std::chrono::seconds>(boundary - segment_start);
      if (span < kShortClipThreshold) {
        boundary += kMinute;
        span = std::chrono::duration_cast<std::chrono::seconds>(boundary - segment_start);
      }
      segment_duration_seconds = static_cast<int>(span.count());
      if (segment_duration_seconds <= 0) {
        segment_duration_seconds = state->params.durationSeconds;
      }
      next_start = boundary;
    } else {
      auto scheduled_start = next_start;
      auto now = std::chrono::system_clock::now();
      if (scheduled_start > now) {
        if (!sleep_until_time(scheduled_start, &state->stopRequested)) {
          break;
        }
      }
      segment_start = scheduled_start;
      segment_duration_seconds = state->params.durationSeconds;
    }

    if (!wait_until_channel_idle(channel, &state->stopRequested)) {
      break;
    }

    auto target = build_loop_target_path(state->params, segment_start);
    if (target.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }
    if (!begin_segment(target, channel, segment_duration_seconds)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }
    if (!first_segment_pending) {
      // Advance next_start only after segment successfully started
      next_start = segment_start + segment_duration_seconds;
      next_start = round_up_to_minute(next_start);
    }
    first_segment_pending = false;

    if (!wait_for_recording_completion(channel, &state->stopRequested)) {
      break;
    }
  }
  cancel_stop_timer(channel);
  stop_recording(channel);
}

void start_loop_for_channel(const RecordingLoopParams &params) {
  if (params.channel < 0 || params.channel >= NUM_VIDEO_CHANNELS) {
    LOG_ERROR("MP4ControlSocket: invalid loop channel " << params.channel);
    return;
  }
  if (params.durationSeconds <= 0) {
    LOG_ERROR("MP4ControlSocket: loop duration must be > 0");
    return;
  }
  std::lock_guard<std::mutex> lock(loop_state_mutex);
  stop_loop_locked(params.channel);
  auto state = std::make_shared<LoopState>();
  state->params = params;
  loop_states[params.channel] = state;
  loop_threads[params.channel] = std::thread(loop_worker, params.channel, state);
  LOG_INFO("MP4ControlSocket: loop recorder enabled on channel " << params.channel);
}

bool handle_loop_start(const StartCommandOptions &options) {
  bool ok = false;
  auto params = build_loop_params(options, &ok);
  if (!ok) {
    LOG_ERROR("MP4ControlSocket: invalid loop START request "
              "(mount/duration/name required)");
    return false;
  }
  std::string reason;
  if (!validate_mount_point_path(fs::path(params.mount), reason)) {
    LOG_ERROR("MP4ControlSocket: loop START rejected for mount '" << params.mount << "': " << reason);
    return false;
  }
  if ((params.durationSeconds % 60) != 0) {
    LOG_WARN("MP4ControlSocket: loop duration " << params.durationSeconds
                                                << "s is not a multiple of 60s; minute alignment may add padding");
  }
  start_loop_for_channel(params);
  return true;
}
} // namespace

void MP4ControlSocket::run() {
  LOG_INFO("MP4ControlSocket: run() entered");

  // Create FIFO if it does not exist
  if (mkdir(FIFO_DIR, 0775) < 0 && errno != EEXIST) {
    LOG_ERROR("MP4ControlSocket: mkdir failed for " << FIFO_DIR);
    return;
  }

  unlink(FIFO_PATH);
  if (mkfifo(FIFO_PATH, 0666) < 0) {
    LOG_ERROR("MP4ControlSocket: mkfifo failed for " << FIFO_PATH);
    return;
  }

  LOG_INFO("MP4ControlSocket: listening on FIFO " << FIFO_PATH);
  LOG_INFO("MP4ControlSocket: FIFO setup completed, entering accept/read loop");

  char buf[512];
  while (true) {
    int fd = open(FIFO_PATH, O_RDONLY);
    if (fd < 0) {
      LOG_ERROR("MP4ControlSocket: open() failed for FIFO, exiting loop");
      break;
    }

    LOG_INFO("MP4ControlSocket: FIFO opened, waiting for commands");

    while (true) {
      std::memset(buf, 0, sizeof(buf));
      ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
      if (n <= 0) {
        LOG_INFO("MP4ControlSocket: read() returned " << n << ", closing FIFO and reopening");
        break;
      }

      std::string cmd(buf, static_cast<size_t>(n));
      LOG_INFO("MP4ControlSocket: raw command buffer ('" << cmd << "') length=" << n);
      std::istringstream iss(cmd);
      std::string op;
      iss >> op;
      LOG_INFO("MP4ControlSocket: parsed op='" << op << "'");

      if (op == "START") {
        StartCommandOptions options;
        options.channel = default_recorder_channel();
        std::vector<std::string> positional;
        std::string token;
        while (iss >> token) {
          auto eq_pos = token.find('=');
          if (eq_pos == std::string::npos) {
            positional.push_back(token);
            continue;
          }
          std::string key = token.substr(0, eq_pos);
          std::string value = token.substr(eq_pos + 1);
          if (key.empty()) {
            continue;
          }
          int parsed_value = 0;
          if (key == "dur" || key == "duration") {
            if (parse_int_token(value, parsed_value)) {
              options.durationSeconds = parsed_value;
              options.durationProvided = true;
            } else {
              LOG_WARN("MP4ControlSocket: invalid duration token '" << token << "'");
            }
            continue;
          }
          if (key == "ch" || key == "channel") {
            if (parse_int_token(value, parsed_value)) {
              options.channel = parsed_value;
              options.channelProvided = true;
            } else {
              LOG_WARN("MP4ControlSocket: invalid channel token '" << token << "'");
            }
            continue;
          }
          if (key == "loop") {
            if (!parse_bool_token(value, options.loop)) {
              options.loop = (value != "0");
            }
            continue;
          }
          if (key == "mount") {
            options.mount = value;
            options.mountProvided = true;
            continue;
          }
          if (key == "dir" || key == "directory") {
            options.directory = value;
            options.directoryProvided = true;
            continue;
          }
          if (key == "name" || key == "template") {
            options.nameTemplate = value;
            options.nameTemplateProvided = true;
            continue;
          }
          if (key == "path") {
            options.path = value;
            continue;
          }
          LOG_WARN("MP4ControlSocket: unrecognized key '" << key << "' in START command");
        }

        if (!positional.empty() && options.path.empty()) {
          options.path = positional[0];
        }
        if (positional.size() > 1 && options.durationSeconds == 0) {
          int parsed_value = 0;
          if (parse_int_token(positional[1], parsed_value)) {
            options.durationSeconds = parsed_value;
            options.durationProvided = true;
          }
        }
        if (positional.size() > 2) {
          int parsed_value = 0;
          if (parse_int_token(positional[2], parsed_value)) {
            options.channel = parsed_value;
            options.channelProvided = true;
          }
        }

        if (options.loop) {
          handle_loop_start(options);
          continue;
        }

        int channel = options.channel;
        if (channel < 0 || channel >= NUM_VIDEO_CHANNELS) {
          channel = default_recorder_channel();
        }

        std::string path = options.path;
        if (path.empty() && (!options.mount.empty() || !options.directory.empty() || !options.nameTemplate.empty())) {
          bool single_ok = false;
          auto params = build_loop_params(options, &single_ok);
          if (single_ok) {
            path = build_loop_target_path(params);
            channel = params.channel;
            if (options.durationSeconds == 0) {
              options.durationSeconds = params.durationSeconds;
            }
          }
        }

        int duration_seconds = options.durationSeconds;
        if (duration_seconds < 0) {
          duration_seconds = 0;
        }

        LOG_INFO("MP4ControlSocket: START requested, path='" << path << "' duration=" << duration_seconds
                                                             << "s channel=" << channel);

        stop_loop_for_channel(channel);

        bool ok = begin_segment(path, channel, duration_seconds);
        if (!ok) {
          LOG_ERROR("MP4ControlSocket: START failed for path '" << path << "'");
        }
      } else if (op == "STOP") {
        int stop_channel = -1;
        std::string token;
        while (iss >> token) {
          std::string key;
          std::string value = token;
          auto eq_pos = token.find('=');
          if (eq_pos != std::string::npos) {
            key = token.substr(0, eq_pos);
            value = token.substr(eq_pos + 1);
          }
          int parsed_value = 0;
          if (!parse_int_token(value, parsed_value)) {
            LOG_WARN("MP4ControlSocket: ignoring non-numeric token '" << token << "' in STOP command");
            continue;
          }
          if (!key.empty() && key != "ch" && key != "channel") {
            LOG_WARN("MP4ControlSocket: unrecognized key '" << key << "' in STOP command");
            continue;
          }
          stop_channel = parsed_value;
          break;
        }

        if (stop_channel >= 0) {
          LOG_INFO("MP4ControlSocket: STOP requested for channel " << stop_channel);
          stop_loop_for_channel(stop_channel);
          cancel_stop_timer(stop_channel);
          stop_recording(stop_channel);
        } else {
          LOG_INFO("MP4ControlSocket: STOP requested for all channels");
          stop_all_loops();
          cancel_all_stop_timers();
          stop_all_recordings();
        }
      } else {
        LOG_ERROR("MP4ControlSocket: unknown command '" << op << "'");
      }
    }

    ::close(fd);
  }

  stop_all_loops();
  cancel_all_stop_timers();
  stop_all_recordings();
  ::unlink(FIFO_PATH);
}
