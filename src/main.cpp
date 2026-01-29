#include "AudioOutputControl.hpp"
#include "AudioOutputWorker.hpp"
#include "AudioWorker.hpp"
#include "BackchannelWorker.hpp"
#include "Config.hpp"
#include "ConfigWatcher.hpp"
#include "DayNightWorker.hpp"
#include "IMPBackchannel.hpp"
#include "IMPSystem.hpp"
#include "ImagingControl.hpp"
#include "JPEGWorker.hpp"
#include "Logger.hpp"
#include "MP4ControlSocket.hpp"
#include "Motion.hpp"
#include "RTSP.hpp"
#include "VideoPrivacyControl.hpp"
#include "VideoWorker.hpp"
#if defined(WEBSOCKET_ENABLED)
#include "WS.hpp"
#endif
#include "WorkerUtils.hpp"
#include "globals.hpp"
#include "HTTPMJPEG.hpp"
#include "IPCServer.hpp"
#include "version.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <thread>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>

// execinfo.h is only available on glibc, not uclibc/musl
#if !defined(LIBC_UCLIBC) && !defined(LIBC_MUSL)
#include <execinfo.h>
#define HAS_BACKTRACE 1
#else
#define HAS_BACKTRACE 0
#endif

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

std::shared_ptr<CFG> cfg = std::make_shared<CFG>();

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
    LOG_WARN("Failed to create run directory " << kPrudyntRunDir << ": " << ec.message());
  }

  instance_lock_fd = ::open(kPrudyntLockPath, O_RDWR | O_CREAT, 0644);
  if (instance_lock_fd < 0) {
    LOG_ERROR("Unable to open instance lock file " << kPrudyntLockPath << ": " << strerror(errno));
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
        pid_info.erase(std::remove(pid_info.begin(), pid_info.end(), '\n'), pid_info.end());
      }
      if (!pid_info.empty()) {
        LOG_ERROR("Another Prudynt instance appears to be running (pid " << pid_info << "). Exiting.");
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
  int len = snprintf(pid_buf, sizeof(pid_buf), "%d\n", static_cast<int>(getpid()));
  if (len > 0) {
    if (::write(instance_lock_fd, pid_buf, len) < 0) {
      LOG_WARN("Failed to write pid to instance lock file: " << strerror(errno));
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

// Helper to write strings safely in signal handler
static void safe_write(int fd, const char* str) {
  write(fd, str, strlen(str));
}

// Helper to write hex value safely
static void safe_write_hex(int fd, unsigned long val) {
  char buf[20];
  char* p = buf + sizeof(buf) - 1;
  *p = '\0';
  if (val == 0) {
    *(--p) = '0';
  } else {
    while (val > 0 && p > buf) {
      unsigned int digit = val & 0xf;
      *(--p) = digit < 10 ? '0' + digit : 'a' + (digit - 10);
      val >>= 4;
    }
  }
  safe_write(fd, p);
}

// Enhanced crash handler with diagnostics
void crash_signal_handler_extended(int sig, siginfo_t *info, void *context) {
  constexpr const char *kCrashReportPath = "/tmp/prudynt_crash.log";

  // Open crash report file
  int crash_fd = open(kCrashReportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (crash_fd < 0) {
    crash_fd = STDERR_FILENO;
  }

  // Get timestamp
  time_t now = time(nullptr);
  char timebuf[64];
  struct tm tm_info;
  localtime_r(&now, &tm_info);
  strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_info);

  // Write crash header
  safe_write(crash_fd, "\n=== PRUDYNT CRASH REPORT ===\n");
  safe_write(crash_fd, "Timestamp: ");
  safe_write(crash_fd, timebuf);
  safe_write(crash_fd, "\nVersion: " FULL_VERSION_STRING "\n");
  safe_write(crash_fd, "PID: ");
  safe_write_hex(crash_fd, getpid());
  safe_write(crash_fd, "\n\n");

  // Signal information
  safe_write(crash_fd, "Signal: ");
  const char *signame = "UNKNOWN";
  switch (sig) {
    case SIGSEGV: signame = "SIGSEGV (Segmentation fault)"; break;
    case SIGABRT: signame = "SIGABRT (Abort)"; break;
    case SIGILL:  signame = "SIGILL (Illegal instruction)"; break;
    case SIGFPE:  signame = "SIGFPE (Floating point exception)"; break;
    case SIGBUS:  signame = "SIGBUS (Bus error)"; break;
  }
  safe_write(crash_fd, signame);
  safe_write(crash_fd, "\n");

  // Signal details
  if (info) {
    safe_write(crash_fd, "Signal code: ");
    safe_write_hex(crash_fd, info->si_code);

    if (sig == SIGILL) {
      safe_write(crash_fd, " (");
      switch (info->si_code) {
        case ILL_ILLOPC: safe_write(crash_fd, "illegal opcode"); break;
        case ILL_ILLOPN: safe_write(crash_fd, "illegal operand"); break;
        case ILL_ILLADR: safe_write(crash_fd, "illegal addressing mode"); break;
        case ILL_ILLTRP: safe_write(crash_fd, "illegal trap"); break;
        case ILL_PRVOPC: safe_write(crash_fd, "privileged opcode"); break;
        case ILL_PRVREG: safe_write(crash_fd, "privileged register"); break;
        case ILL_COPROC: safe_write(crash_fd, "coprocessor error"); break;
        case ILL_BADSTK: safe_write(crash_fd, "internal stack error"); break;
        default: safe_write(crash_fd, "unknown"); break;
      }
      safe_write(crash_fd, ")");
    } else if (sig == SIGSEGV) {
      safe_write(crash_fd, " (");
      switch (info->si_code) {
        case SEGV_MAPERR: safe_write(crash_fd, "address not mapped"); break;
        case SEGV_ACCERR: safe_write(crash_fd, "invalid permissions"); break;
        default: safe_write(crash_fd, "unknown"); break;
      }
      safe_write(crash_fd, ")");
    }
    safe_write(crash_fd, "\n");

    safe_write(crash_fd, "Fault address: 0x");
    safe_write_hex(crash_fd, (unsigned long)info->si_addr);
    safe_write(crash_fd, "\n");
  }

  // Program counter and registers (MIPS-specific)
  if (context) {
    ucontext_t *uc = (ucontext_t *)context;
    safe_write(crash_fd, "\nRegisters:\n");

#if defined(__mips__)
    // MIPS register access
    safe_write(crash_fd, "PC (Instruction Address): 0x");
    safe_write_hex(crash_fd, uc->uc_mcontext.pc);
    safe_write(crash_fd, "\n");

    safe_write(crash_fd, "SP (Stack Pointer): 0x");
    safe_write_hex(crash_fd, uc->uc_mcontext.gregs[29]);
    safe_write(crash_fd, "\n");

    safe_write(crash_fd, "RA (Return Address): 0x");
    safe_write_hex(crash_fd, uc->uc_mcontext.gregs[31]);
    safe_write(crash_fd, "\n");
#elif defined(__arm__)
    safe_write(crash_fd, "PC: 0x");
    safe_write_hex(crash_fd, uc->uc_mcontext.arm_pc);
    safe_write(crash_fd, "\n");
    safe_write(crash_fd, "SP: 0x");
    safe_write_hex(crash_fd, uc->uc_mcontext.arm_sp);
    safe_write(crash_fd, "\n");
    safe_write(crash_fd, "LR: 0x");
    safe_write_hex(crash_fd, uc->uc_mcontext.arm_lr);
    safe_write(crash_fd, "\n");
#else
    safe_write(crash_fd, "(register dump not available for this architecture)\n");
#endif
  }

  // Backtrace
#if HAS_BACKTRACE
  safe_write(crash_fd, "\nBacktrace:\n");
  void *backtrace_buffer[64];
  int frame_count = backtrace(backtrace_buffer, 64);

  // backtrace_symbols is not async-signal-safe, but we're crashing anyway
  // and we need the information. We'll use backtrace_symbols_fd which writes directly.
  backtrace_symbols_fd(backtrace_buffer, frame_count, crash_fd);

  // Try to resolve symbols using dladdr (also not async-signal-safe, but informative)
  safe_write(crash_fd, "\nDetailed backtrace:\n");
  for (int i = 0; i < frame_count; i++) {
    Dl_info dlinfo;
    if (dladdr(backtrace_buffer[i], &dlinfo)) {
      safe_write(crash_fd, "#");
      safe_write_hex(crash_fd, i);
      safe_write(crash_fd, " 0x");
      safe_write_hex(crash_fd, (unsigned long)backtrace_buffer[i]);
      safe_write(crash_fd, " in ");
      if (dlinfo.dli_sname) {
        safe_write(crash_fd, dlinfo.dli_sname);
      } else {
        safe_write(crash_fd, "???");
      }
      safe_write(crash_fd, " from ");
      if (dlinfo.dli_fname) {
        safe_write(crash_fd, dlinfo.dli_fname);
      } else {
        safe_write(crash_fd, "???");
      }
      safe_write(crash_fd, "\n");
    }
  }
#else
  safe_write(crash_fd, "\nBacktrace: (not available - execinfo.h not found)\n");
  safe_write(crash_fd, "To get a backtrace, enable core dumps with 'ulimit -c unlimited'\n");
  safe_write(crash_fd, "and use 'gdb /usr/bin/prudynt core' to analyze the crash.\n");
#endif
  // Also write to stderr
  if (crash_fd != STDERR_FILENO) {
    safe_write(STDERR_FILENO, "\nPrudynt crashed! Crash report saved to ");
    safe_write(STDERR_FILENO, kCrashReportPath);
    safe_write(STDERR_FILENO, "\n");
    safe_write(STDERR_FILENO, "Signal: ");
    safe_write(STDERR_FILENO, signame);
    if (info) {
      safe_write(STDERR_FILENO, " at address 0x");
      safe_write_hex(STDERR_FILENO, (unsigned long)info->si_addr);
    }
    safe_write(STDERR_FILENO, "\n");
    close(crash_fd);
  }

  // Release the instance lock
  release_instance_lock();

  // Re-raise the signal with default handler to generate core dump if enabled
  signal(sig, SIG_DFL);
  raise(sig);
}

void *shutdown_signal_thread(void *arg) {
  sigset_t local_set = *static_cast<sigset_t *>(arg);
  int received_signal = 0;
  while (sigwait(&local_set, &received_signal) == 0) {
    LOG_INFO("main: received signal " << received_signal << ", initiating shutdown");
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
} // namespace

bool timesync_wait() {
  // I don't really have a better way to do this than
  // a no-earlier-than time. The most common sync failure
  // is time() == 0
  int timeout = 0;
  while (time(NULL) < 1647489843) {
    if (global_shutdown_requested.load(std::memory_order_relaxed)) {
      return false;
    }
    std::this_thread::sleep_for(seconds(1));
    ++timeout;
    if (timeout == 60)
      return false;
  }
  return true;
}

void start_video(int encChn) {
  StartHelper sh{encChn};
  int ret = pthread_create(&global_video[encChn]->thread, nullptr, VideoWorker::thread_entry, static_cast<void *>(&sh));
  LOG_DEBUG_OR_ERROR(ret, "create video[" << encChn << "] thread");

  // wait for initialization done
  sh.has_started.acquire();
}

int main(int argc, const char *argv[]) {
  LOG_INFO("PRUDYNT-T Video Daemon: " << FULL_VERSION_STRING);

  InstanceLockGuard instance_lock;

  pthread_t cw_thread;
#if defined(WEBSOCKET_ENABLED)
  pthread_t ws_thread;
#endif
  pthread_t osd_thread;
  pthread_t rtsp_thread;
  pthread_t motion_thread;
  pthread_t backchannel_thread;
  pthread_t audio_output_thread;
  pthread_t signal_thread;
  pthread_t daynight_thread;
  bool daynight_thread_started = false;
  bool signal_thread_started = false;

  bool http_mjpeg_started = false;

  if (Logger::init(cfg->general.loglevel)) {
    LOG_ERROR("Logger initialization failed.");
    return 1;
  }

  LOG_INFO("Starting Prudynt Video Server.");
#if defined(WEBSOCKET_ENABLED)
  LOG_INFO("WebSocket module compiled; runtime state: " << (cfg->websocket.enabled ? "enabled" : "disabled"));
#else
  LOG_INFO("WebSocket module not compiled into this build.");
#endif
  LOG_INFO("HTTP server is " << ((cfg->http.enabled && (cfg->http.mjpeg_enabled || cfg->http.api_enabled)) ? "enabled" : "disabled"));
  LOG_INFO("Motion module is " << (cfg->motion.enabled ? "enabled" : "disabled"));

  if (!instance_lock.acquire()) {
    LOG_ERROR("Prudynt is already running. Exiting.");
    return 1;
  }

  // Install crash signal handlers to clean up lock file and collect diagnostics on abnormal termination
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_signal_handler_extended;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND; // Get detailed signal info, reset to default after first invocation

  sigaction(SIGSEGV, &sa, nullptr); // Segmentation fault
  sigaction(SIGABRT, &sa, nullptr); // Abort signal
  sigaction(SIGILL, &sa, nullptr);  // Illegal instruction
  sigaction(SIGFPE, &sa, nullptr);  // Floating point exception
  sigaction(SIGBUS, &sa, nullptr);  // Bus error

  sigemptyset(&shutdown_signal_set);
  sigaddset(&shutdown_signal_set, SIGINT);
  sigaddset(&shutdown_signal_set, SIGTERM);
  int sigmask_ret = pthread_sigmask(SIG_BLOCK, &shutdown_signal_set, nullptr);
  if (sigmask_ret != 0) {
    LOG_ERROR("Failed to block shutdown signals, pthread_sigmask returned " << sigmask_ret);
    return 1;
  }

  if (pthread_create(&signal_thread, nullptr, shutdown_signal_thread, &shutdown_signal_set) != 0) {
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
  const char *cpu_info = (cfg->sysinfo.cpu && cfg->sysinfo.cpu[0] != '\0') ? cfg->sysinfo.cpu : "unknown";
  LOG_INFO("Audio input: selected AI device " << audio_input_device_id << " ("
                                              << (mic_is_digital ? "digital" : "analog") << " mic, CPU " << cpu_info
                                              << ")");

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
  global_audio[0]->msgChannel = std::make_shared<MsgChannel<AudioFrame>>(cfg->audio.buffer_cap_frames);
  global_backchannel = std::make_shared<backchannel_stream>();
  global_audio_output = std::make_shared<audio_output_stream>();

  pthread_create(&cw_thread, nullptr, ConfigWatcher::thread_entry, nullptr);

#if defined(WEBSOCKET_ENABLED)
  pthread_create(&ws_thread, nullptr, WS::run, &ws);
#endif

  if (cfg->http.enabled && (cfg->http.mjpeg_enabled || cfg->http.api_enabled)) {
    http_mjpeg.start(cfg->http.port, cfg->http.mjpeg_enabled, cfg->http.api_enabled,
                     cfg->http.auth_required, cfg->http.username, cfg->http.password);
    http_mjpeg_started = true;
  }

  ipc_server.start();

  while (!global_shutdown_requested.load(std::memory_order_relaxed)) {
    global_restart = true;
    if (cfg->audio.output_enabled && (global_restart_audio || startup)) {
      int ret = pthread_create(&audio_output_thread, nullptr, AudioOutputWorker::thread_entry, nullptr);
      LOG_DEBUG_OR_ERROR(ret, "create audio output thread");
    }

    if (cfg->audio.output_enabled && (global_restart_audio || startup)) {
      int ret = pthread_create(&backchannel_thread, nullptr, BackchannelWorker::thread_entry, NULL);
      LOG_DEBUG_OR_ERROR(ret, "create backchannel thread");
    }

    if (cfg->audio.input_enabled && (global_restart_audio || startup)) {
      StartHelper sh{0};
      int ret = pthread_create(&global_audio[0]->thread, nullptr, AudioWorker::thread_entry, static_cast<void *>(&sh));
      LOG_DEBUG_OR_ERROR(ret, "create audio thread");
      // wait for initialization done
      sh.has_started.acquire();
    }
    if (global_restart_video || startup) {
      if (cfg->stream0.enabled) {
        if (cfg->stream0.video_enabled) {
          start_video(0);
        }
      }

      if (cfg->stream1.enabled) {
        if (cfg->stream1.video_enabled) {
          start_video(1);
        }
      }

      if (cfg->stream2.enabled) {
        StartHelper sh{2};
        int ret = pthread_create(&global_jpeg[0]->thread, nullptr, JPEGWorker::thread_entry, static_cast<void *>(&sh));
        LOG_DEBUG_OR_ERROR(ret, "create jpeg thread");
        // wait for initialization done
        sh.has_started.acquire();
      }

      if (cfg->stream3.enabled) {
        StartHelper sh{3};
        int ret = pthread_create(&global_jpeg[1]->thread, nullptr, JPEGWorker::thread_entry, static_cast<void *>(&sh));
        LOG_DEBUG_OR_ERROR(ret, "create jpeg thread 2");
        sh.has_started.acquire();
      }

      if (cfg->stream0.osd.enabled || cfg->stream1.osd.enabled) {
        int ret = pthread_create(&osd_thread, nullptr, OSD::thread_entry, NULL);
        LOG_DEBUG_OR_ERROR(ret, "create osd thread");
      }

      if (cfg->motion.enabled) {
        int ret = pthread_create(&motion_thread, nullptr, Motion::run, &motion);
        LOG_DEBUG_OR_ERROR(ret, "create motion thread");
      }

      if (startup && !daynight_thread_started && cfg->get<bool>("daynight.enabled")) {
        int ret = pthread_create(&daynight_thread, nullptr, DayNightWorkerNS::thread_entry, nullptr);
        LOG_DEBUG_OR_ERROR(ret, "create daynight thread");
        if (ret == 0) {
          daynight_thread_started = true;
        }
      }
    }

    // start rtsp server
    if (global_rtsp_thread_signal != 0 && (global_restart_rtsp || startup)) {
      int ret = pthread_create(&rtsp_thread, nullptr, RTSP::run, &rtsp);
      LOG_DEBUG_OR_ERROR(ret, "create rtsp thread");
    }

    /* we should wait a short period to ensure all services are up
     * and running, additionally we add the timespan which is configured as
     * OSD startup delay.
     */
    usleep(250000 + (cfg->stream0.osd.start_delay * 1000) + cfg->stream1.osd.start_delay * 1000);

    LOG_DEBUG("main thread is going to sleep");
    std::unique_lock lck(mutex_main);

    startup = false;
    global_restart = false;
    global_restart_video = false;
    global_restart_audio = false;
    global_restart_rtsp = false;

    while (!global_restart_rtsp && !global_restart_video && !global_restart_audio &&
           !global_shutdown_requested.load(std::memory_order_relaxed))
      global_cv_worker_restart.wait(lck);

    bool shutting_down = global_shutdown_requested.load(std::memory_order_relaxed);
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

    // stop audio
    if (global_audio[0]->imp_audio && global_restart_audio) {
      global_audio[0]->running = false;
      global_audio[0]->should_grab_frames.notify_one();
      int ret = pthread_join(global_audio[0]->thread, NULL);
      LOG_DEBUG_OR_ERROR(ret, "join audio thread");
    }

    if (global_audio_output && global_audio_output->running && global_restart_audio) {
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

      // stop jpeg
      if (global_jpeg[0]->imp_encoder) {
        global_jpeg[0]->running = false;
        global_jpeg[0]->should_grab_frames.notify_one();
        int ret = pthread_join(global_jpeg[0]->thread, NULL);
        LOG_DEBUG_OR_ERROR(ret, "join jpeg thread");
      }

      if (global_jpeg[1]->imp_encoder) {
        global_jpeg[1]->running = false;
        global_jpeg[1]->should_grab_frames.notify_one();
        int ret = pthread_join(global_jpeg[1]->thread, NULL);
        LOG_DEBUG_OR_ERROR(ret, "join jpeg thread 2");
      }

      // stop stream1
      if (global_video[1]->imp_encoder) {
        global_video[1]->running = false;
        global_video[1]->should_grab_frames.notify_one();
        int ret = pthread_join(global_video[1]->thread, NULL);
        LOG_DEBUG_OR_ERROR(ret, "join stream1 thread");
      }

      // stop stream0
      if (global_video[0]->imp_encoder) {
        global_video[0]->running = false;
        global_video[0]->should_grab_frames.notify_one();
        int ret = pthread_join(global_video[0]->thread, NULL);
        LOG_DEBUG_OR_ERROR(ret, "join stream0 thread");
      }
    }

    if (global_shutdown_requested.load(std::memory_order_relaxed)) {
      break;
    }
  }

  join_signal_thread(false);

  if (daynight_thread_started) {
    int ret = pthread_join(daynight_thread, nullptr);
    LOG_DEBUG_OR_ERROR(ret, "join daynight thread");
  }

#if defined(WEBSOCKET_ENABLED)
  if (cfg->websocket.enabled) {
    int ret = pthread_join(ws_thread, nullptr);
    LOG_DEBUG_OR_ERROR(ret, "join websocket thread");
  }
#endif

  int ret = pthread_join(cw_thread, nullptr);
  LOG_DEBUG_OR_ERROR(ret, "join config watcher thread");

  if (http_mjpeg_started) {
    http_mjpeg.stop();
  }

  ipc_server.stop();

  ImagingControl::stop();
  return 0;
}
