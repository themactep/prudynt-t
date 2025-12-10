#include "AudioOutputControl.hpp"
#include "AudioOutputWorker.hpp"
#include "AudioWorker.hpp"
#include "BackchannelWorker.hpp"
#include "Config.hpp"
#include "ConfigWatcher.hpp"
#include "IMPBackchannel.hpp"
#include "IMPSystem.hpp"
#include "ImagingControl.hpp"
#include "JPEGWorker.hpp"
#include "Logger.hpp"
#include "MP4ControlSocket.hpp"
#include "Motion.hpp"
#include "RTSP.hpp"
#include "VideoWorker.hpp"
#include "WS.hpp"
#include "WorkerUtils.hpp"
#include "globals.hpp"
#include "version.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <signal.h>
#include <thread>
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

std::shared_ptr<jpeg_stream> global_jpeg[NUM_VIDEO_CHANNELS] = {nullptr};
std::shared_ptr<video_stream> global_video[NUM_VIDEO_CHANNELS] = {nullptr};
#if defined(AUDIO_SUPPORT)
std::shared_ptr<audio_stream> global_audio[NUM_AUDIO_CHANNELS] = {nullptr};
std::shared_ptr<backchannel_stream> global_backchannel = nullptr;
std::shared_ptr<audio_output_stream> global_audio_output = nullptr;
#endif

std::shared_ptr<CFG> cfg = std::make_shared<CFG>();

WS ws;
RTSP rtsp;
Motion motion;
IMPSystem *imp_system = nullptr;

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
  int ret = pthread_create(&global_video[encChn]->thread, nullptr,
                           VideoWorker::thread_entry, static_cast<void *>(&sh));
  LOG_DEBUG_OR_ERROR(ret, "create video[" << encChn << "] thread");

  // wait for initialization done
  sh.has_started.acquire();
}

int main(int argc, const char *argv[]) {
  LOG_INFO("PRUDYNT-T Next-Gen Video Daemon: " << FULL_VERSION_STRING);

  pthread_t cw_thread;
  pthread_t ws_thread;
  pthread_t osd_thread;
  pthread_t rtsp_thread;
  pthread_t motion_thread;
  pthread_t backchannel_thread;
  pthread_t audio_output_thread;
  pthread_t signal_thread;
  bool signal_thread_started = false;

  if (Logger::init(cfg->general.loglevel)) {
    LOG_ERROR("Logger initialization failed.");
    return 1;
  }
  LOG_INFO("Starting Prudynt Video Server.");

  sigemptyset(&shutdown_signal_set);
  sigaddset(&shutdown_signal_set, SIGINT);
  sigaddset(&shutdown_signal_set, SIGTERM);
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

  // Start Unix domain socket control server for MP4 recording
  std::thread(MP4ControlSocket::run).detach();
  std::thread(AudioOutputControl::run).detach();
  ImagingControl::start();

  global_video[0] = std::make_shared<video_stream>(0, &cfg->stream0, "stream0");
  global_video[1] = std::make_shared<video_stream>(1, &cfg->stream1, "stream1");
  global_jpeg[0] = std::make_shared<jpeg_stream>(2, &cfg->stream2);

#if defined(AUDIO_SUPPORT)
  global_audio[0] = std::make_shared<audio_stream>(1, 0, 0);
  global_backchannel = std::make_shared<backchannel_stream>();
  global_audio_output = std::make_shared<audio_output_stream>();
#endif

  pthread_create(&cw_thread, nullptr, ConfigWatcher::thread_entry, nullptr);
  pthread_create(&ws_thread, nullptr, WS::run, &ws);

  while (!global_shutdown_requested.load(std::memory_order_relaxed)) {
    global_restart = true;
#if defined(AUDIO_SUPPORT)
    if (cfg->audio.output_enabled && (global_restart_audio || startup)) {
      int ret = pthread_create(&audio_output_thread, nullptr,
                               AudioOutputWorker::thread_entry, nullptr);
      LOG_DEBUG_OR_ERROR(ret, "create audio output thread");
    }

    if (cfg->audio.output_enabled && (global_restart_audio || startup)) {
      int ret = pthread_create(&backchannel_thread, nullptr,
                               BackchannelWorker::thread_entry, NULL);
      LOG_DEBUG_OR_ERROR(ret, "create backchannel thread");
    }

    if (cfg->audio.input_enabled && (global_restart_audio || startup)) {
      StartHelper sh{0};
      int ret =
          pthread_create(&global_audio[0]->thread, nullptr,
                         AudioWorker::thread_entry, static_cast<void *>(&sh));
      LOG_DEBUG_OR_ERROR(ret, "create audio thread");
      // wait for initialization done
      sh.has_started.acquire();
    }
#endif
    if (global_restart_video || startup) {
      if (cfg->stream0.enabled) {
        start_video(0);
      }

      if (cfg->stream1.enabled) {
        start_video(1);
      }

      if (cfg->stream2.enabled) {
        StartHelper sh{2};
        int ret =
            pthread_create(&global_jpeg[0]->thread, nullptr,
                           JPEGWorker::thread_entry, static_cast<void *>(&sh));
        LOG_DEBUG_OR_ERROR(ret, "create jpeg thread");
        // wait for initialization done
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
    usleep(250000 + (cfg->stream0.osd.start_delay * 1000) +
           cfg->stream1.osd.start_delay * 1000);

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

    // stop audio
    if (global_audio[0]->imp_audio && global_restart_audio) {
      global_audio[0]->running = false;
      global_audio[0]->should_grab_frames.notify_one();
      int ret = pthread_join(global_audio[0]->thread, NULL);
      LOG_DEBUG_OR_ERROR(ret, "join audio thread");
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
  ImagingControl::stop();
  return 0;
}
