#include "video/JPEGWorker.hpp"

#include "config/Config.hpp"
#include "util/Logger.hpp"
#include "util/WorkerUtils.hpp"
#include "stream/globals.hpp"
#include "isp/imp_hal.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>  // For O_RDWR, O_CREAT, O_TRUNC flags
#include <unistd.h> // For open(), close(), etc.

#define MODULE "JPEGWorker"

JPEGWorker::JPEGWorker(int jpgChnIndex, int impEncoderChn)
    : jpgChn(jpgChnIndex), impEncChn(impEncoderChn) {
  LOG_DEBUG("JPEGWorker created for JPEG channel index "
            << jpgChn << " (IMP Encoder Channel " << impEncChn << ")");
}

JPEGWorker::~JPEGWorker() {
  LOG_DEBUG("JPEGWorker destroyed for JPEG channel index " << jpgChn);
}

bool JPEGWorker::ensure_running(int jpgChn) {
  static std::mutex start_mutex;
  std::lock_guard<std::mutex> start_lock(start_mutex);

  if (global_shutdown_requested.load(std::memory_order_relaxed))
    return false;

  if (jpgChn < 0 || jpgChn >= NUM_JPEG_CHANNELS || !global_jpeg[jpgChn]) {
    return false;
  }

  // Respect the config enable flag: a snapshot/MJPEG request must not spawn a
  // worker for a stream the user has disabled.
  if (!global_jpeg[jpgChn]->stream ||
      !global_jpeg[jpgChn]->stream->enabled) {
    LOG_DEBUG("JPEG channel " << jpgChn << " disabled by config, "
              "refusing lazy start");
    return false;
  }

  if (global_jpeg[jpgChn]->imp_encoder ||
      global_jpeg[jpgChn]->running.load(std::memory_order_relaxed)) {
    return true;
  }

  StartHelper sh{global_jpeg[jpgChn]->encChn};
  int ret = pthread_create(&global_jpeg[jpgChn]->thread, nullptr,
                           JPEGWorker::thread_entry, static_cast<void *>(&sh));
  LOG_DEBUG_OR_ERROR(ret, "create lazy jpeg thread " << jpgChn);
  if (ret != 0) {
    return false;
  }

  sh.has_started.acquire();
  return global_jpeg[jpgChn]->imp_encoder != nullptr;
}

int JPEGWorker::save_jpeg_stream(int fd, IMPEncoderStream *stream) {
  auto write_chunk = [&](const void *ptr, size_t len) -> bool {
    if (!len)
      return true;
    int ret = write(fd, ptr, len);
    if (ret != static_cast<int>(len)) {
      LOG_ERROR("Stream write error: " << strerror(errno));
      return false;
    }
    return true;
  };

  const int nr_pack = stream->packCount;
  for (int i = 0; i < nr_pack; i++) {
    auto slices = hal::encoder::get_pack_slices(*stream, i);
    if (!slices.first_ptr || slices.first_len == 0)
      continue;

    if (!write_chunk(slices.first_ptr, slices.first_len))
      return -1;
    if (slices.second_ptr && slices.second_len > 0) {
      if (!write_chunk(slices.second_ptr, slices.second_len))
        return -1;
    }
  }

  return 0;
}

// Main processing loop, adapted from Worker::jpeg_grabber
void JPEGWorker::run() {
  LOG_DEBUG("Start JPEG processing run loop for index "
            << jpgChn << " (IMP Encoder Channel " << impEncChn << ")");

  // Initial target FPS based on idle setting
  int targetFps = global_jpeg[jpgChn]->stream->jpeg_idle_fps;

  // Local stats counters
  uint32_t bps{0}; // Bytes per second
  uint32_t fps{0}; // frames per second

  // timestamp for stream stats calculation
  unsigned long long ms{0};

  // Initialize timestamp for stats calculation (ensure it's set before first
  // use)
  gettimeofday(&global_jpeg[jpgChn]->stream->stats.ts, NULL);
  global_jpeg[jpgChn]->stream->stats.ts.tv_sec -= 10;

  while (global_jpeg[jpgChn]->running) {
    /*
     * if jpeg_idle_fps = 0, the thread is put into sleep until a client is
     * connected. if jpeg_idle_fps > 0, we try to reach a frame rate of
     * stream.jpeg_idle_fps. enen if no client is connected. if a client is
     * connected via HTTP we try to reach a framerate of stream.fps the thread
     * will fallback into idle / sleep mode if no client request was made for
     * more than a second
     */
    auto now = steady_clock::now();

    std::unique_lock lck(mutex_main);
    bool request_or_overrun = global_jpeg[jpgChn]->request_or_overrun();
    lck.unlock();

    if (global_jpeg[jpgChn]->running &&
        (request_or_overrun || targetFps)) {
      auto diff_last_image =
          duration_cast<milliseconds>(now - global_jpeg[jpgChn]->last_image)
              .count();

      // remove targetFps/10 milliseconds as image creation time guard against
      // division by zero if targetFps is 0
      int next_interval_ms =
          (targetFps > 0) ? ((1000 / targetFps) - (targetFps / 10)) : 0;

      // throttle capture cadence toward requested fps while accounting for
      // encode time
      if ((targetFps > 0 && diff_last_image >= next_interval_ms) ||
          (targetFps == 0 && request_or_overrun)) {
        // check if current jpeg channal is running if not start it
        if (!global_video[global_jpeg[jpgChn]->streamChn]->active) {
          // required video channel was not running, we need to start it and set
          // run_for_jpeg as a reason.
          std::unique_lock<std::mutex> lock_stream{mutex_main};
          global_video[global_jpeg[jpgChn]->streamChn]->run_for_jpeg = true;
          global_video[global_jpeg[jpgChn]->streamChn]
              ->should_grab_frames.notify_one();
          lock_stream.unlock();
          global_video[global_jpeg[jpgChn]->streamChn]->is_activated.acquire();
        }

        // subscriber is connected
        if (request_or_overrun) {
          if (targetFps != global_jpeg[jpgChn]->stream->fps)
            targetFps = global_jpeg[jpgChn]->stream->fps;
        }
        // no subscriber is connected
        else {
          if (targetFps != global_jpeg[jpgChn]->stream->jpeg_idle_fps)
            targetFps = global_jpeg[jpgChn]->stream->jpeg_idle_fps;
        }

        // Apply dynamic reconfiguration if requested
        if (global_jpeg[jpgChn]->reconfig.load()) {
          int new_w = global_jpeg[jpgChn]->req_width.load();
          int new_h = global_jpeg[jpgChn]->req_height.load();
          int new_fps = global_jpeg[jpgChn]->req_fps.load();
          int new_q = global_jpeg[jpgChn]->req_quality.load();

          bool size_change = (new_w > 0 && new_h > 0 &&
                              (new_w != global_jpeg[jpgChn]->stream->width ||
                               new_h != global_jpeg[jpgChn]->stream->height));
          bool quality_change = (new_q >= 1 && new_q <= 100 &&
                                 new_q != global_jpeg[jpgChn]->stream->jpeg_quality);

          if (size_change || quality_change) {
            // Recreating the channel steals ISP frames from the H.264 encoder
            // and can cause decode errors on the RTSP stream, so only do it
            // when the output size or quality actually changed.
            if (global_jpeg[jpgChn]->imp_encoder) {
              global_jpeg[jpgChn]->imp_encoder->deinit();
            }
            if (size_change) {
              global_jpeg[jpgChn]->stream->width = new_w;
              global_jpeg[jpgChn]->stream->height = new_h;
            }
            if (quality_change) {
              global_jpeg[jpgChn]->stream->jpeg_quality = new_q;
            }
            int init_ret = 0;
            if (global_jpeg[jpgChn]->imp_encoder) {
              init_ret = global_jpeg[jpgChn]->imp_encoder->init();
            }
            if (init_ret != 0) {
              LOG_ERROR("JPEG reinit failed (ret=" << init_ret
                                                   << "); channel may stall");
            } else {
              IMP_Encoder_StartRecvPic(global_jpeg[jpgChn]->encChn);
            }
          }
          // FPS change: update stream->fps for the worker's polling logic.
          // No encoder reinit needed --- the JPEG encoder handles fps changes
          // by adjusting the poll interval.
          if (new_fps > 0 && new_fps != global_jpeg[jpgChn]->stream->fps) {
            global_jpeg[jpgChn]->stream->fps = new_fps;
          }

          // Reset request parameters to -1 to prevent accidental re-triggering
          global_jpeg[jpgChn]->req_width.store(-1);
          global_jpeg[jpgChn]->req_height.store(-1);
          global_jpeg[jpgChn]->req_fps.store(-1);
          global_jpeg[jpgChn]->req_quality.store(-1);
          global_jpeg[jpgChn]->reconfig.store(false);
        }

        if (IMP_Encoder_PollingStream(global_jpeg[jpgChn]->encChn,
                                      cfg->general.imp_polling_timeout_ms) ==
            0) {
          if (!global_jpeg[jpgChn]->running)
            break;
          IMPEncoderStream stream{};
          if (IMP_Encoder_GetStream(global_jpeg[jpgChn]->encChn, &stream,
                                    GET_STREAM_BLOCKING) == 0) {
            if (stream.pack == nullptr || stream.packCount <= 0) {
              LOG_WARN("JPEGWorker: encoder returned empty stream on channel "
                       << global_jpeg[jpgChn]->encChn
                       << " (packCount=" << stream.packCount << ")");
              IMP_Encoder_ReleaseStream(global_jpeg[jpgChn]->encChn, &stream);
              continue;
            }

            size_t total_size = 0;
            for (uint32_t i = 0; i < stream.packCount; i++) {
              auto slices = hal::encoder::get_pack_slices(stream, i);
              total_size += slices.first_len + slices.second_len;
            }
            if (total_size == 0) {
              LOG_WARN(
                  "JPEGWorker: encoder returned zero-length stream on channel "
                  << global_jpeg[jpgChn]->encChn);
              IMP_Encoder_ReleaseStream(global_jpeg[jpgChn]->encChn, &stream);
              continue;
            }

            fps++;
            bps += total_size;

            // Build in-memory JPEG snapshot buffer for HTTP/IPC consumers
            if (total_size) {
              std::unique_lock buf_lock(mutex_main);
              auto &buf = global_jpeg[jpgChn]->snapshot_buf;
              buf.resize(total_size);
              unsigned char *dst = buf.data();
              // Second pass: copy data into buffer
              for (uint32_t i = 0; i < stream.packCount; i++) {
                auto slices = hal::encoder::get_pack_slices(stream, i);
                if (slices.first_len) {
                  std::memcpy(dst, slices.first_ptr, slices.first_len);
                  dst += slices.first_len;
                }
                if (slices.second_len) {
                  std::memcpy(dst, slices.second_ptr, slices.second_len);
                  dst += slices.second_len;
                }
              }
            }

            uint32_t seq = ++global_jpeg[jpgChn]->frame_seq;
            LOG_TRACE("JPG " << jpgChn << " seq=" << seq << " dt="
                             << diff_last_image << "ms size=" << total_size);

            if (global_jpeg[jpgChn]->stream->jpeg_refresh > 0) {
              const char *tempPath = "/tmp/snapshot.tmp";
              const char *finalPath = global_jpeg[jpgChn]->stream->jpeg_path;

              int snap_fd = open(tempPath, O_RDWR | O_CREAT | O_TRUNC, 0666);
              if (snap_fd >= 0) {
                save_jpeg_stream(snap_fd, &stream);
                close(snap_fd);

                if (rename(tempPath, finalPath) != 0) {
                  LOG_ERROR("Failed to move JPEG snapshot from "
                            << tempPath << " to " << finalPath);
                  std::remove(tempPath);
                }
              } else {
                LOG_ERROR(
                    "Failed to open JPEG snapshot for writing: " << tempPath);
              }
            }

            IMP_Encoder_ReleaseStream(global_jpeg[jpgChn]->encChn,
                                      &stream); // Release stream after saving
          }

          ms = WorkerUtils::tDiffInMs(&global_jpeg[jpgChn]->stream->stats.ts);
          if (ms > 1000) {
            global_jpeg[jpgChn]->stream->stats.fps = fps;
            global_jpeg[jpgChn]->stream->stats.bps = bps;
            fps = 0;
            bps = 0;
            gettimeofday(&global_jpeg[jpgChn]->stream->stats.ts, NULL);

            LOG_DDEBUG("JPG "
                       << jpgChn
                       << " fps: " << global_jpeg[jpgChn]->stream->stats.fps
                       << " bps: " << global_jpeg[jpgChn]->stream->stats.bps
                       << " diff_last_image: " << diff_last_image
                       << " request_or_overrun: " << request_or_overrun
                       << " targetFps: " << targetFps << " ms: " << ms);
          }
        }

        global_jpeg[jpgChn]->last_image = steady_clock::now();
      } else {
        usleep(1000);
      }
    } else {
      LOG_DDEBUG("JPEG LOCK" << " channel:" << jpgChn);

      global_jpeg[jpgChn]->stream->stats.bps = 0;
      global_jpeg[jpgChn]->stream->stats.fps = 0;
      targetFps = 0;

      std::unique_lock<std::mutex> lock_stream{mutex_main};
      global_jpeg[jpgChn]->active = false;
      global_video[global_jpeg[jpgChn]->streamChn]->run_for_jpeg = false;
      while (global_jpeg[jpgChn]->running &&
             !global_jpeg[jpgChn]->request_or_overrun() &&
             !global_restart_video) {
        // Pause the JPEG encoder while nothing is requesting snapshots so
        // an idle preview doesn't burn encoder cycles.
        if (!global_jpeg[jpgChn]->encoder_paused) {
          IMP_Encoder_StopRecvPic(global_jpeg[jpgChn]->encChn);
          global_jpeg[jpgChn]->encoder_paused = true;
          LOG_INFO("jpeg" << jpgChn << ": encoder paused (idle)");
        }
        global_jpeg[jpgChn]->should_grab_frames.wait(lock_stream);
      }

      if (global_jpeg[jpgChn]->encoder_paused) {
        IMP_Encoder_StartRecvPic(global_jpeg[jpgChn]->encChn);
        global_jpeg[jpgChn]->encoder_paused = false;
        LOG_INFO("jpeg" << jpgChn << ": encoder resumed");
      }

      targetFps = global_jpeg[jpgChn]->stream->fps;

      global_jpeg[jpgChn]->is_activated.release();
      global_jpeg[jpgChn]->active = true;

      LOG_DDEBUG("JPEG UNLOCK" << " channel:" << jpgChn);
    }
  }

  LOG_DEBUG("Exiting JPEG processing run loop for index " << jpgChn);
}

// Static entry point for creating the thread
void *JPEGWorker::thread_entry(void *arg) {
  LOG_DEBUG("Start jpeg_grabber thread.");

  StartHelper *sh = static_cast<StartHelper *>(arg);
  const int impEncChn = sh->encChn;
  int jpgChn = impEncChn - 2;
  int ret;

  // Stream geometry is resolved once in resolve_all_stream_geometry
  // (IMPSystem::init), so use the stream's own config as-is.
  auto *stream_cfg = global_jpeg[jpgChn]->stream;
  global_jpeg[jpgChn]->streamChn = stream_cfg->jpeg_channel;
  global_jpeg[jpgChn]->base_width = stream_cfg->width;
  global_jpeg[jpgChn]->base_height = stream_cfg->height;

  const char *stream_name =
      (stream_cfg == &cfg->stream2) ? "stream2" : "stream3";

  global_jpeg[jpgChn]->imp_encoder = IMPEncoder::createNew(
      stream_cfg, impEncChn, global_jpeg[jpgChn]->streamChn,
      global_jpeg[jpgChn]->streamChn, stream_name);

  if (!global_jpeg[jpgChn]->imp_encoder) {
    LOG_ERROR("Failed to create JPEG encoder for channel " << jpgChn);
    sh->has_started.release();
    return nullptr;
  }

  // inform main that initialization is complete
  sh->has_started.release();

  ret = IMP_Encoder_StartRecvPic(global_jpeg[jpgChn]->encChn);
  LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_StartRecvPic("
                              << global_jpeg[jpgChn]->encChn << ")");
  if (ret != 0)
    return 0;

  global_jpeg[jpgChn]->active = true;
  global_jpeg[jpgChn]->running = true;
  JPEGWorker worker(jpgChn, impEncChn);
  worker.run();

  if (global_jpeg[jpgChn]->imp_encoder) {
    global_jpeg[jpgChn]->imp_encoder->deinit();

    delete global_jpeg[jpgChn]->imp_encoder;
    global_jpeg[jpgChn]->imp_encoder = nullptr;
  }

  return 0;
}
