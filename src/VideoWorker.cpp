#include "VideoWorker.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "Config.hpp"
#include "IMPEncoder.hpp"
#include "IMPFramesource.hpp"
#include "Logger.hpp"
#include "WorkerUtils.hpp"
#include "globals.hpp"

#undef MODULE
#define MODULE "VideoWorker"

VideoWorker::VideoWorker(int chn) : encChn(chn) {
  LOG_DEBUG("VideoWorker created for channel " << encChn);
}

VideoWorker::~VideoWorker() {
  LOG_DEBUG("VideoWorker destroyed for channel " << encChn);
}

void VideoWorker::run() {
  LOG_DEBUG("Start video processing run loop for stream " << encChn);

  auto &channel_recorder = global_mp4_recorders[encChn];
  auto monotonic_ms = []() -> uint64_t {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  };
  constexpr uint64_t kIdrRequestIntervalMs = 250;

  uint32_t bps = 0;
  uint32_t fps = 0;
  uint32_t error_count = 0; // Keep track of polling errors
  unsigned long long ms = 0;
  bool run_for_jpeg = false;
  auto video_state = global_video[encChn];
  std::vector<uint8_t> mp4_sample;
  bool mp4_sample_is_key = false;
  int64_t mp4_sample_ts = -1;
  int64_t mp4_sample_ts_base = -1;
  bool mp4_waiting_frame_end = false;
  bool mp4_inserted_codec_config = false;
  auto compute_frame_switch_threshold = [](int fps_value) -> int64_t {
    int fps = fps_value > 0 ? fps_value : 25;
    int64_t frame_period = 1000000LL / fps;
    return std::max<int64_t>(frame_period / 2, 2000LL);
  };
  int last_mp4_fps =
      (video_state && video_state->stream) ? video_state->stream->fps : 0;
  int64_t mp4_frame_switch_threshold =
      compute_frame_switch_threshold(last_mp4_fps);

  auto reset_mp4_sample = [&]() {
    mp4_sample.clear();
    mp4_sample_is_key = false;
    mp4_sample_ts = -1;
    mp4_waiting_frame_end = false;
    mp4_inserted_codec_config = false;
  };

  auto reset_mp4_state = [&]() {
    reset_mp4_sample();
    mp4_sample_ts_base = -1;
  };

  auto flush_mp4_sample = [&](bool recorder_active) {
    if (!recorder_active || mp4_sample.empty()) {
      reset_mp4_sample();
      return;
    }

    bool waiting_for_idr =
        video_state
            ? video_state->mp4_waiting_for_idr.load(std::memory_order_relaxed)
            : false;
    if (waiting_for_idr) {
      if (!mp4_sample_is_key) {
        reset_mp4_sample();
        return;
      }
      int64_t required_ts =
          video_state
              ? video_state->mp4_required_idr_ts.load(std::memory_order_relaxed)
              : -1;
      if (required_ts >= 0 && mp4_sample_ts <= required_ts) {
        reset_mp4_sample();
        return;
      }
      video_state->mp4_waiting_for_idr.store(false, std::memory_order_relaxed);
    }

    int64_t pts_ms = 0;
    if (mp4_sample_ts >= 0) {
      int64_t relative_ts = mp4_sample_ts;
      if (mp4_sample_ts_base >= 0) {
        relative_ts -= mp4_sample_ts_base;
      }
      if (relative_ts < 0) {
        relative_ts = 0;
      }
      pts_ms = relative_ts / 1000;
    }
    if (mp4_sample_is_key && video_state) {
      video_state->mp4_last_idr_ts.store(mp4_sample_ts,
                                         std::memory_order_relaxed);
      video_state->mp4_required_idr_ts.store(mp4_sample_ts,
                                             std::memory_order_relaxed);
    }
    channel_recorder.writeVideo(mp4_sample.data(), mp4_sample.size(), pts_ms,
                                mp4_sample_is_key);
    reset_mp4_sample();
  };

  while (global_video[encChn]->running) {
    bool stream_is_h265 = false;
    if (video_state && video_state->stream && video_state->stream->format) {
      stream_is_h265 = (std::strcmp(video_state->stream->format, "H265") == 0);
    }

    if (video_state &&
        video_state->mp4_waiting_for_idr.load(std::memory_order_relaxed)) {
      uint64_t now_ms = monotonic_ms();
      uint64_t last_req =
          video_state->mp4_last_idr_request_ms.load(std::memory_order_relaxed);
      if (now_ms - last_req >= kIdrRequestIntervalMs) {
        IMP_Encoder_RequestIDR(encChn);
        video_state->mp4_last_idr_request_ms.store(now_ms,
                                                   std::memory_order_relaxed);
      }
    }

    /* bool helper to check if this is the active jpeg channel and a jpeg is
     * requested while the channel is inactive
     */
    run_for_jpeg = (encChn == global_jpeg[0]->streamChn &&
                    global_video[encChn]->run_for_jpeg);

    /* now we need to verify that
     * 1. a client is connected (hasDataCallback)
     * 2. a jpeg is requested
     * 3. recording explicitly forces the video loop active
     */
    if (global_video[encChn]->hasDataCallback || run_for_jpeg ||
        global_force_video_active) {
      int current_stream_fps = (video_state && video_state->stream)
                                   ? video_state->stream->fps
                                   : last_mp4_fps;
      if (current_stream_fps != last_mp4_fps) {
        last_mp4_fps = current_stream_fps;
        mp4_frame_switch_threshold =
            compute_frame_switch_threshold(last_mp4_fps);
      }
      if (IMP_Encoder_PollingStream(encChn, cfg->general.imp_polling_timeout) ==
          0) {
        IMPEncoderStream stream;
        if (IMP_Encoder_GetStream(encChn, &stream, GET_STREAM_BLOCKING) != 0) {
          LOG_ERROR("IMP_Encoder_GetStream(" << encChn << ") failed");
          error_count++;
          continue;
        }

        /* timestamp fix, can be removed if solved
        int64_t nal_ts = stream.pack[stream.packCount - 1].timestamp;
        struct timeval encoder_time;
        encoder_time.tv_sec = nal_ts / 1000000;
        encoder_time.tv_usec = nal_ts % 1000000;
        */

        for (uint32_t i = 0; i < stream.packCount; ++i) {
          bool recorder_active = channel_recorder.isActive();
          bool recorder_accepts_samples = recorder_active;
          if ((!recorder_active || !recorder_accepts_samples) &&
              mp4_sample_ts != -1) {
            reset_mp4_state();
          }
          if (!recorder_accepts_samples) {
            mp4_sample_ts_base = -1;
          }

          fps++;
          bps += stream.pack[i].length;

#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || \
    defined(PLATFORM_C100)
          uint8_t *start = (uint8_t *)stream.virAddr + stream.pack[i].offset;
          uint8_t *end = start + stream.pack[i].length;
          uint32_t h264_nal = stream.pack[i].nalType.h264NalType;
          uint32_t h265_nal = stream.pack[i].nalType.h265NalType;
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) ||                        \
    defined(PLATFORM_T21) || defined(PLATFORM_T23)
          uint8_t *start = (uint8_t *)stream.pack[i].virAddr;
          uint8_t *end =
              (uint8_t *)stream.pack[i].virAddr + stream.pack[i].length;
          uint32_t h264_nal = stream.pack[i].dataType.h264Type;
          uint32_t h265_nal = 0;
#elif defined(PLATFORM_T30)
          uint8_t *start = (uint8_t *)stream.pack[i].virAddr;
          uint8_t *end =
              (uint8_t *)stream.pack[i].virAddr + stream.pack[i].length;
          uint32_t h264_nal = stream.pack[i].dataType.h264Type;
          uint32_t h265_nal = stream.pack[i].dataType.h265Type;
#endif

          ptrdiff_t raw_payload_len = end - (start + 4);
          if (raw_payload_len <= 0) {
            continue;
          }
          size_t payload_len = static_cast<size_t>(raw_payload_len);
          const uint8_t *payload_ptr = start + 4;

          bool nal_is_sps = false;
          bool nal_is_pps = false;
          bool nal_is_idr = false;
          bool nal_is_hevc_idr = false;

          auto append_length_prefixed_nal = [&](const uint8_t *src,
                                                size_t len) {
            if (!src || len == 0) {
              return;
            }
            size_t write_offset = mp4_sample.size();
            mp4_sample.resize(write_offset + 4 + len);
            uint8_t *dst = mp4_sample.data() + write_offset;
            uint32_t be_len = static_cast<uint32_t>(len);
            dst[0] = static_cast<uint8_t>((be_len >> 24) & 0xFF);
            dst[1] = static_cast<uint8_t>((be_len >> 16) & 0xFF);
            dst[2] = static_cast<uint8_t>((be_len >> 8) & 0xFF);
            dst[3] = static_cast<uint8_t>(be_len & 0xFF);
            std::memcpy(dst + 4, src, len);
          };

          auto append_length_prefixed_nal_vec =
              [&](const std::vector<uint8_t> &nal) {
                if (!nal.empty()) {
                  append_length_prefixed_nal(nal.data(), nal.size());
                }
              };

          if (payload_len > 0) {
            if (stream_is_h265) {
              if (payload_len >= 2) {
                h265_nal = (payload_ptr[0] >> 1) & 0x3F;
              }
            } else {
              h264_nal = payload_ptr[0] & 0x1F;
            }
          }

          if (stream_is_h265) {
            nal_is_sps = (h265_nal == 33);
            nal_is_pps = (h265_nal == 34);
            nal_is_hevc_idr = (h265_nal >= 16 && h265_nal <= 21);
          } else {
            nal_is_sps = (h264_nal == 7);
            nal_is_pps = (h264_nal == 8);
            nal_is_idr = (h264_nal == 5);
          }

          if (nal_is_sps || nal_is_pps) {
            std::lock_guard<std::mutex> lock(
                global_video[encChn]->codec_config_mutex);
            if (nal_is_sps) {
              global_video[encChn]->latest_sps.assign(start + 4, end);
              global_video[encChn]->have_sps = true;
            } else {
              global_video[encChn]->latest_pps.assign(start + 4, end);
              global_video[encChn]->have_pps = true;
            }
          }

          if ((nal_is_idr || nal_is_hevc_idr) && video_state) {
            video_state->mp4_last_idr_ts.store(stream.pack[i].timestamp,
                                               std::memory_order_relaxed);
          }

          if (recorder_accepts_samples && payload_len > 0 &&
              !(nal_is_sps || nal_is_pps)) {
            int64_t pack_ts = stream.pack[i].timestamp;
            bool pack_frame_end = stream.pack[i].frameEnd;

            if (mp4_sample_ts != -1 && !mp4_sample.empty()) {
              if (mp4_waiting_frame_end) {
                if (pack_ts != mp4_sample_ts) {
                  flush_mp4_sample(recorder_active);
                }
              } else {
                int64_t delta = pack_ts - mp4_sample_ts;
                if (delta <= 0 || delta >= mp4_frame_switch_threshold) {
                  flush_mp4_sample(recorder_active);
                }
              }
            }

            if (mp4_sample_ts == -1) {
              mp4_sample_ts = pack_ts;
              if (mp4_sample_ts_base == -1) {
                mp4_sample_ts_base = pack_ts;
              }
            }

            if (mp4_sample.empty()) {
              mp4_sample.reserve(stream.packCount * 512);
            }

            bool waiting_for_idr_flag =
                video_state ? video_state->mp4_waiting_for_idr.load(
                                  std::memory_order_relaxed)
                            : false;
            if (mp4_sample.empty() && waiting_for_idr_flag &&
                !mp4_inserted_codec_config && video_state) {
              std::vector<uint8_t> sps_copy;
              std::vector<uint8_t> pps_copy;
              {
                std::lock_guard<std::mutex> lock(
                    video_state->codec_config_mutex);
                sps_copy = video_state->latest_sps;
                pps_copy = video_state->latest_pps;
              }
              append_length_prefixed_nal_vec(sps_copy);
              append_length_prefixed_nal_vec(pps_copy);
              mp4_inserted_codec_config = true;
            }

            append_length_prefixed_nal(start + 4, payload_len);

            if (nal_is_idr || nal_is_hevc_idr) {
              mp4_sample_is_key = true;
            }

            if (pack_frame_end) {
              flush_mp4_sample(recorder_active);
            } else {
              mp4_waiting_frame_end = true;
            }
          }

          if (global_video[encChn]->hasDataCallback) {
            H264NALUnit nalu;

            /* timestamp fix, can be removed if solved
            nalu.imp_ts = stream.pack[i].timestamp;
            nalu.time = encoder_time;
            */

            // We use start+4 because the encoder inserts 4-byte MPEG
            // 'startcodes' at the beginning of each NAL. Live555 complains.
            nalu.data.insert(nalu.data.end(), start + 4, end);
            if (global_video[encChn]->idr == false) {
              if (nal_is_sps || nal_is_pps || nal_is_idr || nal_is_hevc_idr) {
                global_video[encChn]->idr = true;
              }
            }

            if (global_video[encChn]->idr == true) {
              bool delivered = false;
              if (global_video[encChn]->msgChannel->write(nalu)) {
                delivered = true;
                std::unique_lock<std::mutex> lock_stream{
                    global_video[encChn]->onDataCallbackLock};
                if (global_video[encChn]->onDataCallback)
                  global_video[encChn]->onDataCallback();
              }
              std::vector<VideoTapEntry> taps_copy;
              {
                std::lock_guard<std::mutex> tap_lock(
                    global_video[encChn]->tap_mutex);
                taps_copy = global_video[encChn]->video_taps;
              }
              if (!taps_copy.empty()) {
                for (auto &tap : taps_copy) {
                  if (auto queue = tap.queue.lock()) {
                    queue->write(nalu);
                    if (tap.notify) {
                      tap.notify();
                    }
                  }
                }
              }
              if (!delivered) {
                LOG_ERROR("video " << "channel:" << encChn << ", "
                                   << "package:" << i << " of "
                                   << stream.packCount << ", " << "packageSize:"
                                   << nalu.data.size() << ".  !sink clogged!");
              }
            }
#if defined(USE_AUDIO_STREAM_REPLICATOR)
            /* Since the audio stream is permanently in use by the stream
             * replicator, and the audio grabber and encoder standby is also
             * controlled by the video threads we need to wakeup the audio
             * thread
             */
            if (cfg->audio.input_enabled && !global_audio[0]->active &&
                !global_restart) {
              LOG_DDEBUG("NOTIFY AUDIO " << !global_audio[0]->active << " "
                                         << cfg->audio.input_enabled);
              global_audio[0]->should_grab_frames.notify_one();
            }
#endif
          }
        }

        IMP_Encoder_ReleaseStream(encChn, &stream);

        ms = WorkerUtils::tDiffInMs(&global_video[encChn]->stream->stats.ts);
        if (ms > 1000) {
          /* currently we write into osd and stream stats,
           * osd will be removed and redesigned in future
           */
          global_video[encChn]->stream->stats.bps = bps;
          global_video[encChn]->stream->osd.stats.bps = bps;
          global_video[encChn]->stream->stats.fps = fps;
          global_video[encChn]->stream->osd.stats.fps = fps;

          fps = 0;
          bps = 0;
          gettimeofday(&global_video[encChn]->stream->stats.ts, NULL);
          global_video[encChn]->stream->osd.stats.ts =
              global_video[encChn]->stream->stats.ts;
          /*
          IMPEncoderCHNStat encChnStats;
          IMP_Encoder_Query(channel->encChn, &encChnStats);
          LOG_DEBUG("ChannelStats::" << channel->encChn <<
                      ", registered:" << encChnStats.registered <<
                      ", leftPics:" << encChnStats.leftPics <<
                      ", leftStreamBytes:" << encChnStats.leftStreamBytes <<
                      ", leftStreamFrames:" << encChnStats.leftStreamFrames <<
                      ", curPacks:" << encChnStats.curPacks <<
                      ", work_done:" << encChnStats.work_done);
          */
          if (global_video[encChn]->idr_fix) {
            IMP_Encoder_RequestIDR(encChn);
            global_video[encChn]->idr_fix--;
          }
        }
      } else {
        error_count++;
        LOG_DDEBUG("IMP_Encoder_PollingStream("
                   << encChn << ", " << cfg->general.imp_polling_timeout
                   << ") timeout !");
      }
    } else if (global_video[encChn]->onDataCallback == nullptr &&
               !global_restart_video && !global_video[encChn]->run_for_jpeg &&
               !global_force_video_active) {
      LOG_DDEBUG("VIDEO LOCK"
                 << " channel:" << encChn << " hasCallbackIsNull:"
                 << (global_video[encChn]->onDataCallback == nullptr)
                 << " restartVideo:" << global_restart_video
                 << " runForJpeg:" << global_video[encChn]->run_for_jpeg);

      global_video[encChn]->stream->stats.bps = 0;
      global_video[encChn]->stream->stats.fps = 0;
      global_video[encChn]->stream->osd.stats.bps = 0;
      global_video[encChn]->stream->osd.stats.fps = 0;

      std::unique_lock<std::mutex> lock_stream{mutex_main};
      global_video[encChn]->active = false;
      while (global_video[encChn]->onDataCallback == nullptr &&
             !global_restart_video && !global_video[encChn]->run_for_jpeg &&
             !global_force_video_active)
        global_video[encChn]->should_grab_frames.wait(lock_stream);

      global_video[encChn]->active = true;
      global_video[encChn]->is_activated.release();

      // unlock audio
      global_audio[0]->should_grab_frames.notify_one();

      LOG_DDEBUG("VIDEO UNLOCK" << " channel:" << encChn);
    }
  }
}

void *VideoWorker::thread_entry(void *arg) {
  StartHelper *sh = static_cast<StartHelper *>(arg);
  int encChn = sh->encChn;

  LOG_DEBUG("Start stream_grabber thread for stream " << encChn);

  int ret;

  global_video[encChn]->imp_framesource = IMPFramesource::createNew(
      global_video[encChn]->stream, &cfg->sensor, encChn);
  global_video[encChn]->imp_encoder = IMPEncoder::createNew(
      global_video[encChn]->stream, encChn, encChn, global_video[encChn]->name);
  if (!global_video[encChn]->imp_encoder) {
    LOG_ERROR("Failed to create encoder for stream " << encChn);
    sh->has_started.release();
    if (global_video[encChn]->imp_framesource) {
      delete global_video[encChn]->imp_framesource;
      global_video[encChn]->imp_framesource = nullptr;
    }
    return 0;
  }

  global_video[encChn]->imp_framesource->enable();
  global_video[encChn]->run_for_jpeg = false;

  // inform main that initialization is complete
  sh->has_started.release();

  ret = IMP_Encoder_StartRecvPic(encChn);
  LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_StartRecvPic(" << encChn << ")");
  if (ret != 0)
    return 0;

  /* 'active' indicates, the thread is activly polling and grabbing images
   * 'running' describes the runlevel of the thread, if this value is set to
   * false the thread exits and cleanup all ressources
   */
  global_video[encChn]->active = true;
  global_video[encChn]->running = true;
  VideoWorker worker(encChn);
  worker.run();

  ret = IMP_Encoder_StopRecvPic(encChn);
  LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_StopRecvPic(" << encChn << ")");

  if (global_video[encChn]->imp_framesource) {
    global_video[encChn]->imp_framesource->disable();

    if (global_video[encChn]->imp_encoder) {
      global_video[encChn]->imp_encoder->deinit();
      delete global_video[encChn]->imp_encoder;
      global_video[encChn]->imp_encoder = nullptr;
    }
  }

  return 0;
}
