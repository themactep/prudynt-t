#include "audio/playback/AudioOutputControl.hpp"
#include "audio/AudioOutputWorker.hpp"
#include "audio/AudioWorker.hpp"
#include "audio/BackchannelWorker.hpp"
#include "config/Config.hpp"
#include "core/crash_handler.hpp"
#include "audio/IMPBackchannel.hpp"
#include "isp/IMPSystem.hpp"
#include "isp/ImagingControl.hpp"
#include "video/JPEGWorker.hpp"
#include "util/Logger.hpp"
#include "recording/MP4ControlSocket.hpp"
#include "isp/Motion.hpp"
#include "network/RTSP.hpp"
#include "video/VideoPrivacyControl.hpp"
#include "video/VideoWorker.hpp"
#include "isp/imp_hal.hpp"
#if defined(WEBSOCKET_ENABLED)
#include "network/WS.hpp"
#endif
#include "network/HTTPMJPEG.hpp"
#include "network/IPCServer.hpp"
#include "util/WorkerUtils.hpp"
#include "stream/globals.hpp"
#include "version.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <filesystem>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

using namespace std::chrono;

std::mutex mutex_main;
std::condition_variable global_cv_worker_restart;

bool startup = true;
bool global_restart = false;

bool global_restart_rtsp = false;
bool global_restart_video = false;
bool global_restart_audio = false;
bool global_main_thread_signal = false;
bool global_motion_thread_signal = false;
std::atomic<char> global_rtsp_thread_signal{1};
std::atomic<int> global_rtsp_clients{0};

std::shared_ptr<jpeg_stream> global_jpeg[NUM_JPEG_CHANNELS] = {nullptr};
std::shared_ptr<video_stream> global_video[NUM_VIDEO_CHANNELS] = {nullptr};
std::shared_ptr<audio_stream> global_audio[NUM_AUDIO_CHANNELS] = {nullptr};
std::shared_ptr<backchannel_stream> global_backchannel = nullptr;
std::shared_ptr<audio_output_stream> global_audio_output = nullptr;

std::shared_ptr<CFG> cfg = nullptr;

#if defined(WEBSOCKET_ENABLED)
WS ws;
#endif
HTTPMJPEG http_mjpeg;
IPCServer ipc_server;
RTSP rtsp;
Motion motion;
IMPSystem *imp_system = nullptr;

constexpr const char *kPrudyntRunDir = "/run/prudynt";
constexpr const char *kPrudyntLockPath = "/run/prudynt/prudynt.lock";
int instance_lock_fd = -1;

bool acquire_instance_lock() {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(kPrudyntRunDir, ec);
  if (ec) {
    LOG_WARN("Failed to create run directory " << kPrudyntRunDir << ": "
                                               << ec.message());
  }

  instance_lock_fd = ::open(kPrudyntLockPath, O_RDWR | O_CREAT, 0644);
  if (instance_lock_fd < 0) {
    LOG_ERROR("Unable to open instance lock file " << kPrudyntLockPath << ": "
                                                   << strerror(errno));
    instance_lock_fd = -1;
    return false;
  }

  if (::flock(instance_lock_fd, LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK) {
      char buf[64] = {0};
      ssize_t n = ::pread(instance_lock_fd, buf, sizeof(buf) - 1, 0);
      std::string pid_info;
      if (n > 0) {
        pid_info.assign(buf, buf + n);
        pid_info.erase(std::remove(pid_info.begin(), pid_info.end(), '\n'),
                       pid_info.end());
      }
      if (!pid_info.empty()) {
        LOG_ERROR("Another Prudynt instance appears to be running (pid "
                  << pid_info << "). Exiting.");
      } else {
        LOG_ERROR("Another Prudynt instance appears to be running. Exiting.");
      }
    } else {
      LOG_ERROR("Unable to lock instance file: " << strerror(errno));
    }
    ::close(instance_lock_fd);
    instance_lock_fd = -1;
    return false;
  }

  if (ftruncate(instance_lock_fd, 0) != 0) {
    LOG_WARN("Failed to truncate instance lock file: " << strerror(errno));
  }
  char pid_buf[32];
  int len =
      snprintf(pid_buf, sizeof(pid_buf), "%d\n", static_cast<int>(getpid()));
  if (len > 0) {
    if (::write(instance_lock_fd, pid_buf, len) < 0) {
      LOG_WARN(
          "Failed to write pid to instance lock file: " << strerror(errno));
    } else {
      ::fsync(instance_lock_fd);
    }
  }
  return true;
}

void release_instance_lock() {
  if (instance_lock_fd >= 0) {
    ::flock(instance_lock_fd, LOCK_UN);
    ::close(instance_lock_fd);
    instance_lock_fd = -1;
  }
}

struct InstanceLockGuard {
  bool held = false;
  bool acquire() {
    held = acquire_instance_lock();
    return held;
  }
  ~InstanceLockGuard() {
    if (held) {
      release_instance_lock();
    }
  }
};

namespace {
sigset_t shutdown_signal_set;

void *shutdown_signal_thread(void *arg) {
  sigset_t local_set = *static_cast<sigset_t *>(arg);
  int received_signal = 0;
  while (sigwait(&local_set, &received_signal) == 0) {
    LOG_INFO("main: received signal " << received_signal
                                      << ", initiating shutdown");
    global_shutdown_requested.store(true, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(mutex_main);
      global_restart_rtsp = true;
      global_restart_video = true;
      global_restart_audio = true;
    }
    global_cv_worker_restart.notify_all();
    break;
  }
  return nullptr;
}

void recover_stale_imp_state() {
#if defined(PLATFORM_T23)
  LOG_WARN("Startup recovery: skipped aggressive pre-init cleanup on T23");
  return;
#else
  const char *force_recover = std::getenv("PRUDYNT_FORCE_RECOVER");
  if (!force_recover || force_recover[0] == '\0' ||
      strcmp(force_recover, "1") != 0) {
    LOG_DEBUG(
        "Startup recovery: disabled (set PRUDYNT_FORCE_RECOVER=1 to enable)");
    return;
  }

  LOG_WARN("Startup recovery: attempting to clean stale IMP state from "
           "previous crash");

  for (int ch = 0; ch < 4; ++ch) {
    IMP_Encoder_StopRecvPic(ch);
    IMP_Encoder_UnRegisterChn(ch);
    IMP_Encoder_DestroyChn(ch);
  }
  for (int grp = 0; grp < 4; ++grp) {
    IMP_Encoder_DestroyGroup(grp);
  }

  for (int ch = 0; ch < 4; ++ch) {
    IMP_FrameSource_DisableChn(ch);
    IMP_FrameSource_DestroyChn(ch);
  }

  for (int grp = 0; grp < 4; ++grp) {
    IMP_OSD_Stop(grp);
    IMP_OSD_DestroyGroup(grp);
  }

  for (int ch = 0; ch < 4; ++ch) {
    IMP_ADEC_DestroyChn(ch);
  }

  IMP_AO_DisableChn(0, 0);
  IMP_AO_Disable(0);
  IMP_AI_DisableChn(0, 0);
  IMP_AI_Disable(0);
  IMP_AI_DisableChn(1, 0);
  IMP_AI_Disable(1);

  IMP_System_Exit();
  IMP_ISP_DisableTuning();
  IMP_ISP_Close();

  LOG_WARN("Startup recovery: stale IMP cleanup pass completed");
#endif
}
} // namespace

bool timesync_wait() {
  // I don't really have a better way to do this than
  // a no-earlier-than time. The most common sync failure
  // is time() == 0
  int timeout_s = 0;
  while (time(NULL) < 1647489843) {
    if (global_shutdown_requested.load(std::memory_order_relaxed)) {
      return false;
    }
    std::this_thread::sleep_for(seconds(1));
    ++timeout_s;
    if (timeout_s == 60)
      return false;
  }
  return true;
}

void start_video(int encChn) {
  if (global_shutdown_requested.load(std::memory_order_relaxed))
    return;

  StartHelper sh{encChn};
  int ret = pthread_create(&global_video[encChn]->thread, nullptr,
                           VideoWorker::thread_entry, static_cast<void *>(&sh));
  LOG_DEBUG_OR_ERROR(ret, "create video[" << encChn << "] thread");

  // wait for initialization done
  sh.has_started.acquire();
}

// -- TEMP DIAGNOSTIC: GOT corruption watchdog ----------------------------
// Two SIGSEGV-at-PC=0 crashes showed the static binary's GOT slot for
// std::condition_variable::notify_all() reading as 0 at runtime while the
// on-disk image holds a valid address. This snapshots the .got section at
// startup and polls for divergence, logging address/old/new of any word
// that changes. The GOT of a static executable is never legitimately
// written at runtime, so any hit is the corrupter. Remove once found.
static void start_got_watchdog() {
  uintptr_t got_addr = 0;
  size_t got_size = 0;

  // Locate .got in our own ELF image
  FILE *f = fopen("/proc/self/exe", "rb");
  if (!f) {
    LOG_WARN("got-watchdog: cannot open /proc/self/exe");
    return;
  }
  Elf32_Ehdr eh;
  Elf32_Shdr sh;
  std::vector<char> shstr;
  if (fread(&eh, sizeof(eh), 1, f) == 1 && eh.e_type != ET_EXEC) {
    // PIE/dynamic build: section vaddrs are unrelocated (dereferencing
    // sh_addr faults), and the dynamic linker legitimately writes GOT
    // slots anyway. The watchdog is only meaningful for static ET_EXEC.
    LOG_WARN("got-watchdog: binary is not a static executable, skipping");
    fclose(f);
    return;
  }
  if (fseek(f, 0, SEEK_SET) == 0 && fread(&eh, sizeof(eh), 1, f) == 1 &&
      fseek(f, eh.e_shoff + (long)eh.e_shstrndx * eh.e_shentsize, SEEK_SET) ==
          0 &&
      fread(&sh, sizeof(sh), 1, f) == 1) {
    shstr.resize(sh.sh_size);
    if (fseek(f, sh.sh_offset, SEEK_SET) == 0 &&
        fread(shstr.data(), 1, sh.sh_size, f) == sh.sh_size) {
      for (int i = 0; i < eh.e_shnum; i++) {
        if (fseek(f, eh.e_shoff + (long)i * eh.e_shentsize, SEEK_SET) != 0 ||
            fread(&sh, sizeof(sh), 1, f) != 1) {
          break;
        }
        if (sh.sh_name < shstr.size() &&
            strcmp(&shstr[sh.sh_name], ".got") == 0) {
          got_addr = sh.sh_addr;
          got_size = sh.sh_size;
          break;
        }
      }
    }
  }
  fclose(f);

  if (!got_addr || !got_size) {
    LOG_WARN("got-watchdog: .got section not found");
    return;
  }

  char buf[96];
  snprintf(buf, sizeof(buf), "got-watchdog: monitoring 0x%08lx..0x%08lx",
           (unsigned long)got_addr, (unsigned long)(got_addr + got_size));
  LOG_INFO(buf);

  std::thread([got_addr, got_size]() {
    const volatile uint32_t *got = (const volatile uint32_t *)got_addr;
    const size_t n = got_size / sizeof(uint32_t);
    std::vector<uint32_t> snap(n);
    for (size_t i = 0; i < n; i++) {
      snap[i] = got[i];
    }
    int reported = 0;
    while (reported < 32) {
      for (size_t i = 0; i < n; i++) {
        uint32_t v = got[i];
        if (v != snap[i]) {
          char msg[128];
          snprintf(msg, sizeof(msg),
                   "got-watchdog: GOT[%u] @0x%08lx changed 0x%08x -> 0x%08x",
                   (unsigned)i, (unsigned long)(got_addr + i * 4), snap[i], v);
          LOG_ERROR(msg);
          snap[i] = v;
          reported++;
        }
      }
      usleep(2000);
    }
    LOG_ERROR("got-watchdog: too many changes, watchdog stopped");
  }).detach();
}

int main(int argc, const char *argv[]) {
  if (Logger::init("INFO")) {
    LOG_ERROR("Logger initialization failed.");
    return 1;
  }

  LOG_INFO("PRUDYNT-T Video Daemon: " << FULL_VERSION_STRING);
  LOG_INFO("Starting Prudynt Video Server.");
  LOG_INFO(
      "Configuration bootstrap deferred until after early startup recovery");

  // TEMP DIAGNOSTIC: probe notify_all right away. Two SIGSEGV-at-PC=0
  // crashes jumped through a zeroed GOT slot for this exact function on the
  // first-ever call; if the slot is dead from process start this crashes
  // here with a clean RA in main() instead of in the video worker.
  {
    std::condition_variable diag_cv;
    diag_cv.notify_all();
    LOG_INFO("diag: notify_all probe at startup OK");
  }
  start_got_watchdog();

  InstanceLockGuard instance_lock;

#if defined(WEBSOCKET_ENABLED)
  pthread_t ws_thread;
#endif
  pthread_t osd_thread;
  pthread_t rtsp_thread;
  pthread_t motion_thread;
  pthread_t backchannel_thread;
  pthread_t audio_output_thread;
  pthread_t signal_thread;
  /* daynight_thread removed - photosensing delegated to daynightd */
  bool signal_thread_started = false;

  bool http_mjpeg_started = false;

  if (!instance_lock.acquire()) {
    LOG_ERROR("Prudynt is already running. Exiting.");
    return 1;
  }

  install_crash_handler();

  sigemptyset(&shutdown_signal_set);
  sigaddset(&shutdown_signal_set, SIGINT);
  sigaddset(&shutdown_signal_set, SIGTERM);
  sigaddset(&shutdown_signal_set, SIGHUP);
  sigaddset(&shutdown_signal_set, SIGPIPE);
  int sigmask_ret = pthread_sigmask(SIG_BLOCK, &shutdown_signal_set, nullptr);
  if (sigmask_ret != 0) {
    LOG_ERROR("Failed to block shutdown signals, pthread_sigmask returned "
              << sigmask_ret);
    return 1;
  }

  if (pthread_create(&signal_thread, nullptr, shutdown_signal_thread,
                     &shutdown_signal_set) != 0) {
    LOG_ERROR("Failed to create shutdown signal watcher thread");
    return 1;
  }
  signal_thread_started = true;

  auto join_signal_thread = [&](bool force_signal) {
    if (!signal_thread_started) {
      return;
    }
    if (force_signal) {
      pthread_kill(signal_thread, SIGTERM);
    }
    int ret = pthread_join(signal_thread, nullptr);
    LOG_DEBUG_OR_ERROR(ret, "join shutdown signal thread");
    signal_thread_started = false;
  };

  recover_stale_imp_state();

  cfg = std::make_shared<CFG>();

  // Refuse to start with a corrupted config - bare defaults are only
  // acceptable when there is no config file at all.
  if (cfg->config_corrupted) {
    LOG_ERROR("Configuration file is corrupted.  Restore a valid"
              " /etc/prudynt.json or remove it to start with defaults.");
    join_signal_thread(true);
    return 1;
  }

  Logger::setLevel(cfg->general.loglevel);

#if defined(WEBSOCKET_ENABLED)
  LOG_INFO("WebSocket module compiled; runtime state: "
           << (cfg->websocket.enabled ? "enabled" : "disabled"));
#else
  LOG_INFO("WebSocket module not compiled into this build.");
#endif
  LOG_INFO("HTTP server is "
           << ((cfg->http.enabled &&
                (cfg->http.mjpeg_enabled || cfg->http.api_enabled))
                   ? "enabled"
                   : "disabled"));
  LOG_INFO("Motion module is "
           << (cfg->motion.enabled ? "enabled" : "disabled"));

  if (!timesync_wait()) {
    if (global_shutdown_requested.load(std::memory_order_relaxed)) {
      LOG_INFO("Shutdown requested before time synchronization completed.");
      join_signal_thread(false);
      return 0;
    }
    LOG_ERROR("Time is not synchronized.");
    join_signal_thread(true);
    return 1;
  }

  if (!imp_system) {
    imp_system = IMPSystem::createNew();
  }

  bool mic_is_digital = cfg && cfg->audio.mic_is_digital;
  int audio_input_device_id = mic_is_digital ? 0 : 1;
  const char *cpu_info = (cfg->sysinfo.cpu && cfg->sysinfo.cpu[0] != '\0')
                             ? cfg->sysinfo.cpu
                             : "unknown";
  LOG_INFO("Audio input: selected AI device "
           << audio_input_device_id << " ("
           << (mic_is_digital ? "digital" : "analog") << " mic, CPU "
           << cpu_info << ")");

  // Start Unix domain socket control server for MP4 recording
  std::thread(MP4ControlSocket::run).detach();
  std::thread(AudioOutputControl::run).detach();
  std::thread(VideoPrivacyControl::run).detach();
  ImagingControl::start();

  global_video[0] = std::make_shared<video_stream>(0, &cfg->stream0, "stream0");
  global_video[1] = std::make_shared<video_stream>(1, &cfg->stream1, "stream1");
  global_jpeg[0] = std::make_shared<jpeg_stream>(2, &cfg->stream2);
  global_jpeg[1] = std::make_shared<jpeg_stream>(3, &cfg->stream3);

  global_audio[0] = std::make_shared<audio_stream>(audio_input_device_id, 0, 0);
  global_audio[0]->msgChannel =
      std::make_shared<MsgChannel<AudioFrame>>(MSG_CHANNEL_SIZE);
  global_backchannel = std::make_shared<backchannel_stream>();
  global_audio_output = std::make_shared<audio_output_stream>();


#if defined(WEBSOCKET_ENABLED)
  pthread_create(&ws_thread, nullptr, WS::run, &ws);
#endif

  if (cfg->http.enabled && (cfg->http.mjpeg_enabled || cfg->http.api_enabled)) {
    http_mjpeg.start(cfg->http.port, cfg->http.mjpeg_enabled,
                     cfg->http.api_enabled, cfg->http.auth_required,
                     cfg->http.username, cfg->http.password);
    http_mjpeg_started = true;
  }

  ipc_server.start();

  while (!global_shutdown_requested.load(std::memory_order_relaxed)) {
    global_restart = true;
    // Start audio INPUT first so that the CODEC clock is configured
    // before the audio output thread initialises.  On platforms with a
    // shared AI/AO CODEC clock (T10/T20/T21) the AO's
    // IMP_AO_GetPubAttr will then return the actual running rate.
    if (cfg->audio.input_enabled && (global_restart_audio || startup)) {
      StartHelper sh{0};
      int ret =
          pthread_create(&global_audio[0]->thread, nullptr,
                         AudioWorker::thread_entry, static_cast<void *>(&sh));
      LOG_DEBUG_OR_ERROR(ret, "create audio thread");
      // wait for initialization done
      sh.has_started.acquire();
    }

    if (cfg->audio.output_enabled && (global_restart_audio || startup)) {
      StartHelper ao_sh{0};
      int ret = pthread_create(&audio_output_thread, nullptr,
                               AudioOutputWorker::thread_entry,
                               static_cast<void *>(&ao_sh));
      LOG_DEBUG_OR_ERROR(ret, "create audio output thread");
      // Wait for AO hardware init to complete before starting video.
      // The IMP SDK shares internal state between AO and encoder
      // subsystems; concurrent init causes memory corruption.
      ao_sh.has_started.acquire();
    }

    if (cfg->audio.output_enabled && (global_restart_audio || startup)) {
      StartHelper bc_sh{0};
      int ret = pthread_create(&backchannel_thread, nullptr,
                               BackchannelWorker::thread_entry,
                               static_cast<void *>(&bc_sh));
      LOG_DEBUG_OR_ERROR(ret, "create backchannel thread");
      // Wait for ADEC hardware init to complete before starting video.
      // The IMP SDK shares internal state between ADEC and encoder
      // subsystems; concurrent init causes memory corruption -> SIGILL.
      bc_sh.has_started.acquire();
    }

    if (global_restart_video || startup) {
      if (cfg->stream0.enabled) {
        start_video(0);
      }

      if (cfg->stream1.enabled) {
        start_video(1);
      }

      if (cfg->stream2.enabled &&
          (cfg->stream2.jpeg_idle_fps > 0 || cfg->stream2.jpeg_refresh > 0)) {
        StartHelper sh{2};
        int ret =
            pthread_create(&global_jpeg[0]->thread, nullptr,
                           JPEGWorker::thread_entry, static_cast<void *>(&sh));
        LOG_DEBUG_OR_ERROR(ret, "create jpeg thread");
        // wait for initialization done
        sh.has_started.acquire();
      }

      if (cfg->stream3.enabled &&
          (cfg->stream3.jpeg_idle_fps > 0 || cfg->stream3.jpeg_refresh > 0)) {
        StartHelper sh{3};
        int ret =
            pthread_create(&global_jpeg[1]->thread, nullptr,
                           JPEGWorker::thread_entry, static_cast<void *>(&sh));
        LOG_DEBUG_OR_ERROR(ret, "create jpeg thread 2");
        sh.has_started.acquire();
      }

      if (cfg->osd.sei.enabled || cfg->osd.burnin.enabled) {
        int ret = pthread_create(&osd_thread, nullptr, OSD::thread_entry, NULL);
        LOG_DEBUG_OR_ERROR(ret, "create osd thread");
      }

      if (cfg->motion.enabled) {
        int ret = pthread_create(&motion_thread, nullptr, Motion::run, &motion);
        LOG_DEBUG_OR_ERROR(ret, "create motion thread");
      }

      /* daynight photosensing thread removed - use daynightd daemon */
    }

    // Apply persisted privacy state on first startup (before RTSP goes live)
    if (startup) {
      LOG_INFO("main: calling VideoPrivacyControl::applyStartupState()");
      VideoPrivacyControl::applyStartupState();
    }

    // start rtsp server
    if (global_rtsp_thread_signal != 0 && (global_restart_rtsp || startup)) {
      int ret = pthread_create(&rtsp_thread, nullptr, RTSP::run, &rtsp);
      LOG_DEBUG_OR_ERROR(ret, "create rtsp thread");
    }

    LOG_DEBUG("main thread is going to sleep");
    std::unique_lock lck(mutex_main);

    startup = false;
    global_restart = false;
    global_restart_video = false;
    global_restart_audio = false;
    global_restart_rtsp = false;

    while (!global_restart_rtsp && !global_restart_video &&
           !global_restart_audio &&
           !global_shutdown_requested.load(std::memory_order_relaxed))
      global_cv_worker_restart.wait(lck);

    bool shutting_down =
        global_shutdown_requested.load(std::memory_order_relaxed);
    if (shutting_down) {
      global_restart_rtsp = true;
      global_restart_video = true;
      global_restart_audio = true;
    }
    lck.unlock();

    global_restart = true;

    if (global_restart_rtsp) {
      // stop rtsp thread
      if (global_rtsp_thread_signal == 0) {
        global_rtsp_thread_signal = 1;
        int ret = pthread_join(rtsp_thread, NULL);
        LOG_DEBUG_OR_ERROR(ret, "join rtsp thread");
      }
    }

    // Stop video/encoder BEFORE audio --- the encoder pipeline must be
    // healthy when we call StopRecvPic/DestroyChn.  Audio teardown (AI)
    // shares hardware resources with the encoder; tearing down audio
    // first corrupts encoder state and causes subsequent IMP calls to
    // block indefinitely.
    if (global_restart_video) {
      // stop motion thread
      if (global_motion_thread_signal) {
        global_motion_thread_signal = false;
        int ret = pthread_join(motion_thread, NULL);
        LOG_DEBUG_OR_ERROR(ret, "join motion thread");
      }

      // stop osd thread
      if (global_osd_thread_signal) {
        global_osd_thread_signal = false;
        int ret = pthread_join(osd_thread, NULL);
        LOG_DEBUG_OR_ERROR(ret, "join osd thread");
      }

      // stop stream0
      if (global_video[0]->imp_encoder) {
        global_video[0]->running = false;
        global_video[0]->should_grab_frames.notify_one();
        LOG_DEBUG("waiting for stream0 thread to exit...");
        int ret = pthread_join(global_video[0]->thread, NULL);
        LOG_DEBUG("join stream0 done, ret=" << ret);
      }

      // stop stream1
      if (global_video[1]->imp_encoder) {
        global_video[1]->running = false;
        global_video[1]->should_grab_frames.notify_one();
        LOG_DEBUG("waiting for stream1 thread to exit...");
        int ret = pthread_join(global_video[1]->thread, NULL);
        LOG_DEBUG("join stream1 done, ret=" << ret);
      }

      // stop jpeg (after video --- safe to disable framesource now)
      if (global_jpeg[0]->imp_encoder) {
        global_jpeg[0]->running = false;
        global_jpeg[0]->should_grab_frames.notify_one();
        int ret = pthread_join(global_jpeg[0]->thread, NULL);
        LOG_DEBUG_OR_ERROR(ret, "join jpeg thread");
      }

      if (global_jpeg[1]->imp_encoder) {
        global_jpeg[1]->running = false;
        global_jpeg[1]->should_grab_frames.notify_one();
        LOG_DEBUG("waiting for jpeg[1] thread to exit...");
        int ret = pthread_join(global_jpeg[1]->thread, NULL);
        LOG_DEBUG("join jpeg[1] done, ret=" << ret);
      }
    }

    // stop audio (after video --- encoder pipeline is now clean)
    if (global_audio[0]->imp_audio && global_restart_audio) {
      global_audio[0]->running = false;
      global_audio[0]->should_grab_frames.notify_one();
      LOG_DEBUG("waiting for audio thread to exit...");
      int ret = pthread_join(global_audio[0]->thread, NULL);
      LOG_DEBUG("join audio done, ret=" << ret);
    }

    if (global_audio_output && global_audio_output->running &&
        global_restart_audio) {
      AudioOutputWorker::signalShutdown();
      int ret = pthread_join(audio_output_thread, NULL);
      LOG_DEBUG_OR_ERROR(ret, "join audio output thread");
    }

    // stop backchannel
    if (global_backchannel->imp_backchannel && global_restart_audio) {
      global_backchannel->running = false;
      BackchannelWorker::signalShutdown();
      global_backchannel->should_grab_frames.notify_one();
      int ret = pthread_join(backchannel_thread, NULL);
      LOG_DEBUG_OR_ERROR(ret, "join backchannel thread");
    }

    if (global_shutdown_requested.load(std::memory_order_relaxed)) {
      break;
    }
  }

  join_signal_thread(false);

  /* daynight thread join removed - photosensing delegated to daynightd */

#if defined(WEBSOCKET_ENABLED)
  if (cfg->websocket.enabled) {
    ws.stop();
    int ret = pthread_join(ws_thread, nullptr);
    LOG_DEBUG_OR_ERROR(ret, "join websocket thread");
  }
#endif

  if (http_mjpeg_started) {
    http_mjpeg.stop();
  }

  ipc_server.stop();

  ImagingControl::stop();

  if (imp_system) {
    delete imp_system;
    imp_system = nullptr;
  }

  // Clean up runtime state so stale files (privacy.active etc.) don't
  // survive across restarts and mislead status probes.
  {
    std::error_code ec;
    std::filesystem::remove_all(kPrudyntRunDir, ec);
    if (ec) {
      LOG_WARN("Failed to remove " << kPrudyntRunDir << ": " << ec.message());
    }
  }

  return 0;
}
