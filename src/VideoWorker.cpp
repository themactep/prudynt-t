#include "VideoWorker.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "Config.hpp"
#include "IMPEncoder.hpp"
#include "IMPFramesource.hpp"
#include "Logger.hpp"
#include "PreTriggerBuffer.hpp"
#include "VideoPrivacyMask.hpp"
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
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
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
  int64_t mp4_sample_ts_us = -1;
  int64_t mp4_sample_ts_base_us = -1;
  bool mp4_waiting_frame_end = false;
  bool mp4_inserted_codec_config = false;

#ifdef PREBUFFER_ENABLED
  // Prebuffer frame accumulation (similar to mp4_sample)
  std::vector<uint8_t> prebuffer_sample;
  bool prebuffer_sample_is_key = false;
  int64_t prebuffer_sample_ts_us = -1;
#endif

  // Queue for frames that arrive during prebuffer flush
  struct PendingFrame {
    std::vector<uint8_t> data;
    int64_t timestamp_us;
    bool is_keyframe;
  };
  std::vector<PendingFrame> pending_frames_during_flush;
  auto compute_frame_switch_threshold = [](int fps_value) -> int64_t {
    int fps = fps_value > 0 ? fps_value : 25;
    int64_t frame_period = 1000000LL / fps;
    return std::max<int64_t>(frame_period / 2, 2000LL);
  };
  int last_mp4_fps = (video_state && video_state->stream) ? video_state->stream->fps : 0;
  int64_t mp4_frame_switch_threshold_us = compute_frame_switch_threshold(last_mp4_fps);

  int64_t ts_last_nonzero_us = 0;
  int64_t ts_last_frame_us = 0;
  int64_t ts_last_rtp_us = 0;
  int64_t ts_current_frame_us = 0;
  bool ts_have_current_frame = false;

  auto reset_mp4_sample = [&]() {
    mp4_sample.clear();
    mp4_sample_is_key = false;
    mp4_sample_ts_us = -1;
    mp4_waiting_frame_end = false;
    mp4_inserted_codec_config = false;
  };

#ifdef PREBUFFER_ENABLED
  auto reset_prebuffer_sample = [&]() {
    prebuffer_sample.clear();
    prebuffer_sample_is_key = false;
    prebuffer_sample_ts_us = -1;
  };

  auto flush_prebuffer_sample = [&]() {
    if (prebuffer_sample.empty() || !global_video[encChn]->prebuffer) {
      reset_prebuffer_sample();
      return;
    }

    global_video[encChn]->prebuffer->addFrame(
      prebuffer_sample.data(),
      prebuffer_sample.size(),
      prebuffer_sample_ts_us,
      prebuffer_sample_is_key
    );
    reset_prebuffer_sample();
  };
#endif

  auto reset_mp4_state = [&]() {
    reset_mp4_sample();
    mp4_sample_ts_base_us = -1;
  };

  auto flush_mp4_sample = [&](bool recorder_active) {
    static bool was_recorder_active = false;

    if (!recorder_active) {
      if (was_recorder_active) {
#ifdef PREBUFFER_ENABLED
        pending_frames_during_flush.clear();  // Clear pending frames
#endif
      }
      was_recorder_active = false;
      reset_mp4_sample();
      return;
    }
    was_recorder_active = true;

    if (mp4_sample.empty()) {
      reset_mp4_sample();
      return;
    }

#ifdef PREBUFFER_ENABLED
    // Queue frames while prebuffer is being flushed (instead of skipping)
    if (video_state && video_state->mp4_prebuffer_flushing.load(std::memory_order_acquire)) {
      // Queue this frame for later
      PendingFrame pf;
      pf.data = std::move(mp4_sample);
      pf.timestamp_us = mp4_sample_ts_us;
      pf.is_keyframe = mp4_sample_is_key;
      pending_frames_during_flush.push_back(std::move(pf));

      reset_mp4_sample();
      mp4_sample_ts_base_us = -1;  // Reset timestamp base so we recalculate after flush
      return;
    }

    // If we have pending frames from during the flush, write them first
    if (!pending_frames_during_flush.empty()) {

      int64_t prebuffer_offset = video_state ? video_state->mp4_prebuffer_offset_ms.load(std::memory_order_relaxed) : 0;

      // Find the first keyframe in pending frames to establish timestamp base
      int64_t pending_ts_base_us = -1;
      for (const auto& pf : pending_frames_during_flush) {
        if (pf.is_keyframe) {
          pending_ts_base_us = pf.timestamp_us;
          break;
        }
      }
      // If no keyframe, use first frame's timestamp
      if (pending_ts_base_us < 0 && !pending_frames_during_flush.empty()) {
        pending_ts_base_us = pending_frames_during_flush.front().timestamp_us;
      }

      for (const auto& pf : pending_frames_during_flush) {
        int64_t relative_ts_us = pf.timestamp_us;
        if (pending_ts_base_us >= 0) {
          relative_ts_us -= pending_ts_base_us;
        }
        if (relative_ts_us < 0) relative_ts_us = 0;
        int64_t pts_ms = relative_ts_us / 1000 + prebuffer_offset;

        channel_recorder.writeVideo(pf.data.data(), pf.data.size(), pts_ms, pf.is_keyframe);
      }

      // Update timestamp base for subsequent live frames
      if (!pending_frames_during_flush.empty()) {
        mp4_sample_ts_base_us = pending_frames_during_flush.back().timestamp_us;
      }

      pending_frames_during_flush.clear();
    }
#endif

    bool waiting_for_idr = video_state ? video_state->mp4_waiting_for_idr.load(std::memory_order_relaxed) : false;
    if (waiting_for_idr) {
      if (!mp4_sample_is_key) {
        // Reset full state including timestamp base so next frame starts fresh
        reset_mp4_state();
        return;
      }
      int64_t required_ts = video_state ? video_state->mp4_required_idr_ts_us.load(std::memory_order_relaxed) : -1;
      if (required_ts >= 0 && mp4_sample_ts_us <= required_ts) {
        // Reset full state including timestamp base so next frame starts fresh
        reset_mp4_state();
        return;
      }
      // IDR accepted - timestamp base will be set from this frame's timestamp
      // when the next sample is accumulated (line 316)
      video_state->mp4_waiting_for_idr.store(false, std::memory_order_relaxed);
    }

    int64_t pts_ms = 0;
    if (mp4_sample_ts_us >= 0) {
      int64_t relative_ts_us = mp4_sample_ts_us;
      if (mp4_sample_ts_base_us >= 0) {
        relative_ts_us -= mp4_sample_ts_base_us;
      }
      if (relative_ts_us < 0) {
        relative_ts_us = 0;
      }
      pts_ms = relative_ts_us / 1000;

      // Add prebuffer offset so live frames continue after prebuffer frames
      int64_t prebuffer_offset = video_state ? video_state->mp4_prebuffer_offset_ms.load(std::memory_order_relaxed) : 0;
      if (prebuffer_offset > 0) {
        pts_ms += prebuffer_offset;
      }
    }
    if (mp4_sample_is_key && video_state) {
      video_state->mp4_last_idr_ts_us.store(mp4_sample_ts_us, std::memory_order_relaxed);
      video_state->mp4_required_idr_ts_us.store(mp4_sample_ts_us, std::memory_order_relaxed);
    }
    channel_recorder.writeVideo(mp4_sample.data(), mp4_sample.size(), pts_ms, mp4_sample_is_key);
    reset_mp4_sample();
  };

  while (global_video[encChn]->running) {
    bool stream_is_h265 = false;
    if (video_state && video_state->stream && video_state->stream->format) {
      stream_is_h265 = (std::strcmp(video_state->stream->format, "H265") == 0);
    }

    if (video_state && video_state->mp4_waiting_for_idr.load(std::memory_order_relaxed)) {
      uint64_t now_ms = monotonic_ms();
      uint64_t last_req = video_state->mp4_last_idr_request_ms.load(std::memory_order_relaxed);
      if (now_ms - last_req >= kIdrRequestIntervalMs) {
        IMP_Encoder_RequestIDR(encChn);
        video_state->mp4_last_idr_request_ms.store(now_ms, std::memory_order_relaxed);
      }
    }

    /* bool helper to check if this is the active jpeg channel and a jpeg is
     * requested while the channel is inactive
     */
    bool jpeg_wants_frames = false;
    for (int j = 0; j < NUM_JPEG_CHANNELS; ++j) {
      auto jpeg_stream_state = global_jpeg[j];
      if (jpeg_stream_state && encChn == jpeg_stream_state->streamChn) {
        jpeg_wants_frames = true;
        break;
      }
    }
    run_for_jpeg = (jpeg_wants_frames && global_video[encChn]->run_for_jpeg);

    /* now we need to verify that
     * 1. a client is connected (hasDataCallback)
     * 2. a jpeg is requested
     * 3. recording explicitly forces the video loop active
     */
    // Keep video loop active when prebuffer is enabled (even without RTSP clients)
#ifdef PREBUFFER_ENABLED
    bool prebuffer_active = global_video[encChn]->prebuffer && global_video[encChn]->prebuffer->isEnabled();
#else
    bool prebuffer_active = false;
#endif
    if (global_video[encChn]->hasDataCallback || run_for_jpeg || global_force_video_active || prebuffer_active) {
      int current_stream_fps = (video_state && video_state->stream) ? video_state->stream->fps : last_mp4_fps;
      if (current_stream_fps != last_mp4_fps) {
        last_mp4_fps = current_stream_fps;
        mp4_frame_switch_threshold_us = compute_frame_switch_threshold(last_mp4_fps);
      }
      if (IMP_Encoder_PollingStream(encChn, cfg->general.imp_polling_timeout_ms) == 0) {
        IMPEncoderStream stream;
        if (IMP_Encoder_GetStream(encChn, &stream, GET_STREAM_BLOCKING) != 0) {
          LOG_ERROR("IMP_Encoder_GetStream(" << encChn << ") failed");
          error_count++;
          continue;
        }

        if (stream.packCount == 0) {
          IMP_Encoder_ReleaseStream(encChn, &stream);
          continue;
        }

        int64_t nominal_frame_step_us = 33333;
        if (video_state && video_state->stream && video_state->stream->fps > 0) {
          nominal_frame_step_us = 1000000LL / video_state->stream->fps;
        }
        if (nominal_frame_step_us < 1000) {
          nominal_frame_step_us = 1000;
        }

        for (uint32_t i = 0; i < stream.packCount; ++i) {
          bool recorder_active = channel_recorder.isActive();
          bool recorder_accepts_samples = recorder_active;
          if ((!recorder_active || !recorder_accepts_samples) && mp4_sample_ts_us != -1) {
            reset_mp4_state();
          }
          if (!recorder_accepts_samples) {
            mp4_sample_ts_base_us = -1;
          }

          fps++;
          bps += stream.pack[i].length;

          uint8_t *start = hal::encoder::get_pack_data_start(stream, i);
          uint32_t length = hal::encoder::get_pack_data_length(stream, i);
          uint8_t *end = start + length;
          bool frame_start = (i == 0) || stream.pack[i - 1].frameEnd;
          if (frame_start) {
            uint32_t frame_end_idx = i;
            while (frame_end_idx + 1 < stream.packCount && !stream.pack[frame_end_idx].frameEnd) {
              ++frame_end_idx;
            }

            int64_t frame_ts_us = 0;
            for (uint32_t j = i; j <= frame_end_idx; ++j) {
              if (stream.pack[j].timestamp > 0) {
                frame_ts_us = stream.pack[j].timestamp;
              }
            }

            if (frame_ts_us > 0) {
              if (frame_ts_us > ts_last_nonzero_us) {
                ts_last_nonzero_us = frame_ts_us;
              } else {
                frame_ts_us = ts_last_nonzero_us;
              }
            } else if (ts_last_nonzero_us > 0) {
              frame_ts_us = ts_last_nonzero_us;
            } else if (ts_last_frame_us > 0) {
              frame_ts_us = ts_last_frame_us + nominal_frame_step_us;
            }

            // Guard against IMP encoder timestamp domain transitions
            // (relative→rebased) which cause large forward jumps.
            if (ts_last_frame_us > 0 && frame_ts_us > ts_last_frame_us) {
              int64_t fwd_step = frame_ts_us - ts_last_frame_us;
              if (fwd_step > 2 * nominal_frame_step_us) {
                frame_ts_us = ts_last_frame_us + nominal_frame_step_us;
                ts_last_nonzero_us = frame_ts_us;
              }
            }

            if (ts_last_frame_us > 0 && frame_ts_us <= ts_last_frame_us) {
              frame_ts_us = ts_last_frame_us + nominal_frame_step_us;
            }
            if (frame_ts_us <= 0) {
              frame_ts_us = (ts_last_frame_us > 0) ? (ts_last_frame_us + nominal_frame_step_us) : nominal_frame_step_us;
            }

            ts_current_frame_us = frame_ts_us;
            ts_have_current_frame = true;
          }

          int64_t pack_ts_us = stream.pack[i].timestamp;
          if (pack_ts_us > ts_last_nonzero_us) {
            ts_last_nonzero_us = pack_ts_us;
          }
          if (ts_have_current_frame && ts_current_frame_us > 0) {
            pack_ts_us = ts_current_frame_us;
          } else if (pack_ts_us <= 0 && ts_last_nonzero_us > 0) {
            pack_ts_us = ts_last_nonzero_us;
          }

          if (stream.pack[i].frameEnd && pack_ts_us > 0) {
            ts_last_frame_us = pack_ts_us;
          }

          int64_t rtsp_ts_us = pack_ts_us;
          if (rtsp_ts_us <= 0) {
            rtsp_ts_us = (ts_last_rtp_us > 0) ? (ts_last_rtp_us + nominal_frame_step_us) : nominal_frame_step_us;
          }
          constexpr int64_t kMinRtpStepUs = 12;
          if (ts_last_rtp_us > 0 && rtsp_ts_us <= ts_last_rtp_us) {
            rtsp_ts_us = ts_last_rtp_us + kMinRtpStepUs;
          }
          ts_last_rtp_us = rtsp_ts_us;

          uint32_t h264_nal = hal::encoder::get_h264_nal_type(stream.pack[i]);
          uint32_t h265_nal = hal::encoder::get_h265_nal_type(stream.pack[i]);

          ptrdiff_t raw_payload_len = end - (start + 4);
          if (raw_payload_len <= 0) {
            continue;
          }
          size_t payload_len = static_cast<size_t>(raw_payload_len);
          const uint8_t *payload_ptr = start + 4;

          bool nal_is_vps = false;
          bool nal_is_sps = false;
          bool nal_is_pps = false;
          bool nal_is_idr = false;
          bool nal_is_hevc_idr = false;

          auto append_length_prefixed_nal = [&](const uint8_t *src, size_t len) {
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

          auto append_length_prefixed_nal_vec = [&](const std::vector<uint8_t> &nal) {
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
            nal_is_vps = (h265_nal == 32);
            nal_is_sps = (h265_nal == 33);
            nal_is_pps = (h265_nal == 34);
            nal_is_hevc_idr = (h265_nal >= 16 && h265_nal <= 21);
          } else {
            nal_is_sps = (h264_nal == 7);
            nal_is_pps = (h264_nal == 8);
            nal_is_idr = (h264_nal == 5);
          }

          if (nal_is_vps || nal_is_sps || nal_is_pps) {
            std::lock_guard<std::mutex> lock(global_video[encChn]->codec_config_mutex);
            if (nal_is_vps) {
              global_video[encChn]->latest_vps.assign(start + 4, end);
              global_video[encChn]->have_vps = true;
            } else if (nal_is_sps) {
              global_video[encChn]->latest_sps.assign(start + 4, end);
              global_video[encChn]->have_sps = true;
            } else {
              global_video[encChn]->latest_pps.assign(start + 4, end);
              global_video[encChn]->have_pps = true;
            }
          }

          if ((nal_is_idr || nal_is_hevc_idr) && video_state) {
            video_state->mp4_last_idr_ts_us.store(pack_ts_us, std::memory_order_relaxed);
          }

          if (recorder_accepts_samples && payload_len > 0 && !(nal_is_vps || nal_is_sps || nal_is_pps)) {
            bool pack_frame_end = stream.pack[i].frameEnd;

            if (mp4_sample_ts_us != -1 && !mp4_sample.empty()) {
              if (mp4_waiting_frame_end) {
                if (pack_ts_us != mp4_sample_ts_us) {
                  flush_mp4_sample(recorder_active);
                }
              } else {
                int64_t delta = pack_ts_us - mp4_sample_ts_us;
                if (delta <= 0 || delta >= mp4_frame_switch_threshold_us) {
                  flush_mp4_sample(recorder_active);
                }
              }
            }

            if (mp4_sample_ts_us == -1) {
              mp4_sample_ts_us = pack_ts_us;
              if (mp4_sample_ts_base_us == -1) {
                mp4_sample_ts_base_us = pack_ts_us;
              }
            }

            if (mp4_sample.empty()) {
              mp4_sample.reserve(stream.packCount * 512);
            }

            bool waiting_for_idr_flag =
                video_state ? video_state->mp4_waiting_for_idr.load(std::memory_order_relaxed) : false;
            if (mp4_sample.empty() && waiting_for_idr_flag && !mp4_inserted_codec_config && video_state) {
              std::vector<uint8_t> vps_copy;
              std::vector<uint8_t> sps_copy;
              std::vector<uint8_t> pps_copy;
              {
                std::lock_guard<std::mutex> lock(video_state->codec_config_mutex);
                vps_copy = video_state->latest_vps;
                sps_copy = video_state->latest_sps;
                pps_copy = video_state->latest_pps;
              }
              // For H.265, prepend VPS before SPS
              if (!vps_copy.empty()) {
                append_length_prefixed_nal_vec(vps_copy);
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

            nalu.imp_ts = rtsp_ts_us;
            nalu.time.tv_sec = static_cast<time_t>(rtsp_ts_us / 1000000LL);
            nalu.time.tv_usec = static_cast<suseconds_t>(rtsp_ts_us % 1000000LL);

            // We use start+4 because the encoder inserts 4-byte MPEG
            // 'startcodes' at the beginning of each NAL. Live555 complains.
            nalu.data.insert(nalu.data.end(), start + 4, end);

            // Add frame boundary metadata for complete frame detection
            static uint32_t frame_counter = 0;
            if (frame_start) {
              frame_counter++;  // New frame starting
            }
            nalu.frame_id = frame_counter;
            nalu.packet_index = i;
            nalu.packet_count = stream.packCount;
            nalu.is_frame_start = frame_start;
            nalu.is_frame_end = stream.pack[i].frameEnd;

            if (global_video[encChn]->idr == false) {
              if (nal_is_sps || nal_is_pps || nal_is_idr || nal_is_hevc_idr) {
                global_video[encChn]->idr = true;
              }
            }

            if (global_video[encChn]->idr == true) {
              bool delivered = false;
              // Use non-blocking write() to avoid stalling encoder on slow clients
              // (go2rtc-inspired: drop oldest frame rather than block producer)
              try {
                delivered = global_video[encChn]->msgChannel->write(nalu);
                if (delivered) {
                  std::unique_lock<std::mutex> lock_stream{global_video[encChn]->onDataCallbackLock};
                  if (global_video[encChn]->onDataCallback)
                    global_video[encChn]->onDataCallback();
                } else {
                  LOG_DDEBUG("video channel:" << encChn << " msgChannel full, dropped oldest NAL");
                  // Still notify so consumer processes queued data
                  std::unique_lock<std::mutex> lock_stream{global_video[encChn]->onDataCallbackLock};
                  if (global_video[encChn]->onDataCallback)
                    global_video[encChn]->onDataCallback();
                }
              } catch (const std::exception& e) {
                LOG_ERROR("video channel:" << encChn << ", frame_id:" << nalu.frame_id
                         << ", packet:" << nalu.packet_index << "/" << nalu.packet_count
                         << " - Failed to queue: " << e.what());
                delivered = false;
              }
              std::vector<VideoTapEntry> taps_copy;
              {
                std::lock_guard<std::mutex> tap_lock(global_video[encChn]->tap_mutex);
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
                static uint32_t clog_count[NUM_VIDEO_CHANNELS] = {};
                static uint64_t clog_last_log_ms[NUM_VIDEO_CHANNELS] = {};
                clog_count[encChn]++;
                uint64_t now_ms = monotonic_ms();
                if (now_ms - clog_last_log_ms[encChn] >= 5000) {
                  LOG_WARN("video channel:" << encChn << " - msgChannel sink clogged, "
                                            << clog_count[encChn] << " frames dropped in last 5s");
                  clog_count[encChn] = 0;
                  clog_last_log_ms[encChn] = now_ms;
                }
              }
            }
#if defined(USE_AUDIO_STREAM_REPLICATOR)
            /* Since the audio stream is permanently in use by the stream
             * replicator, and the audio grabber and encoder standby is also
             * controlled by the video threads we need to wakeup the audio
             * thread
             */
            if (cfg->audio.input_enabled && !global_audio[0]->active && !global_restart) {
              LOG_DDEBUG("NOTIFY AUDIO " << !global_audio[0]->active << " " << cfg->audio.input_enabled);
              global_audio[0]->should_grab_frames.notify_one();
            }
#endif
          }

#ifdef PREBUFFER_ENABLED
          // Capture frame for prebuffer if enabled
          // This is OUTSIDE the hasDataCallback block so prebuffer works without RTSP clients
          // Accumulate all NAL units (except SPS/PPS) into prebuffer_sample, flush on frameEnd
          if (global_video[encChn]->prebuffer && global_video[encChn]->prebuffer->isEnabled()) {
            // Skip VPS/SPS/PPS for prebuffer (they're in the avcC/hvcC)
            if (!(nal_is_vps || nal_is_sps || nal_is_pps) && payload_len > 0) {
              // Set timestamp from first NAL unit of frame
              if (prebuffer_sample_ts_us == -1) {
                prebuffer_sample_ts_us = stream.pack[i].timestamp;
              }

              // Mark as keyframe if any NAL is IDR
              if (nal_is_idr || nal_is_hevc_idr) {
                prebuffer_sample_is_key = true;
              }

              // Append length-prefixed NAL unit (same format as MP4)
              size_t write_offset = prebuffer_sample.size();
              prebuffer_sample.resize(write_offset + 4 + payload_len);
              uint8_t *dst = prebuffer_sample.data() + write_offset;
              uint32_t be_len = static_cast<uint32_t>(payload_len);
              dst[0] = static_cast<uint8_t>((be_len >> 24) & 0xFF);
              dst[1] = static_cast<uint8_t>((be_len >> 16) & 0xFF);
              dst[2] = static_cast<uint8_t>((be_len >> 8) & 0xFF);
              dst[3] = static_cast<uint8_t>(be_len & 0xFF);
              std::memcpy(dst + 4, start + 4, payload_len);

              // Flush on frame end
              if (stream.pack[i].frameEnd) {
                flush_prebuffer_sample();
              }
            }
          }
#endif
        }

        // Ensure final packet guarantees callback
        // Even if last write failed silently in edge cases,
        // call callback again to ensure last packet is processed
        if (global_video[encChn]->hasDataCallback && stream.packCount > 0) {
          std::unique_lock<std::mutex> lock_stream{global_video[encChn]->onDataCallbackLock};
          if (global_video[encChn]->onDataCallback) {
            global_video[encChn]->onDataCallback();
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
          global_video[encChn]->stream->osd.stats.ts = global_video[encChn]->stream->stats.ts;
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
        LOG_DDEBUG("IMP_Encoder_PollingStream(" << encChn << ", " << cfg->general.imp_polling_timeout_ms << ") timeout !");
      }
    } else if (global_video[encChn]->onDataCallback == nullptr && !global_restart_video &&
               !global_video[encChn]->run_for_jpeg && !global_force_video_active && !prebuffer_active) {
      LOG_DDEBUG("VIDEO LOCK" << " channel:" << encChn
                              << " hasCallbackIsNull:" << (global_video[encChn]->onDataCallback == nullptr)
                              << " restartVideo:" << global_restart_video
                              << " runForJpeg:" << global_video[encChn]->run_for_jpeg);

      global_video[encChn]->stream->stats.bps = 0;
      global_video[encChn]->stream->stats.fps = 0;
      global_video[encChn]->stream->osd.stats.bps = 0;
      global_video[encChn]->stream->osd.stats.fps = 0;

      std::unique_lock<std::mutex> lock_stream{mutex_main};
      global_video[encChn]->active = false;
      // Also check prebuffer_active to prevent sleeping when prebuffer needs frames
#ifdef PREBUFFER_ENABLED
      bool prebuffer_active_inner = global_video[encChn]->prebuffer && global_video[encChn]->prebuffer->isEnabled();
#else
      bool prebuffer_active_inner = false;
#endif
      while (global_video[encChn]->onDataCallback == nullptr && !global_restart_video &&
             !global_video[encChn]->run_for_jpeg && !global_force_video_active && !prebuffer_active_inner)
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

  global_video[encChn]->imp_framesource = IMPFramesource::createNew(global_video[encChn]->stream, &cfg->sensor, encChn);
  global_video[encChn]->imp_encoder =
      IMPEncoder::createNew(global_video[encChn]->stream, encChn, encChn, global_video[encChn]->name);
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

  std::shared_ptr<VideoPrivacyMask> privacy_mask;
  {
    std::lock_guard<std::mutex> lock(global_video[encChn]->privacy_mutex);
    global_video[encChn]->privacy_mask.reset();
    global_video[encChn]->privacy_mask = std::make_shared<VideoPrivacyMask>(encChn, global_video[encChn]->stream);
    privacy_mask = global_video[encChn]->privacy_mask;
  }

  if (privacy_mask) {
    bool desired = global_video[encChn]->privacy_requested.load(std::memory_order_relaxed);
    if (desired) {
      privacy_mask->setEnabled(true);
    }
  }

  // inform main that initialization is complete
  sh->has_started.release();

#ifdef PREBUFFER_ENABLED
  // Initialize prebuffer if enabled
  if (cfg->recorder.prebuffer_enabled) {
    global_video[encChn]->prebuffer = std::make_unique<PreTriggerBuffer>();
    int fps = global_video[encChn]->stream ? global_video[encChn]->stream->fps : 25;
    bool init_success = global_video[encChn]->prebuffer->init(
        cfg->recorder.prebuffer_seconds,
        fps,
        cfg->recorder.prebuffer_max_memory_mb,
        cfg->recorder.prebuffer_keyframe_only
    );
    if (init_success) {
      LOG_INFO("PreTriggerBuffer initialized for channel " << encChn);
    } else {
      LOG_WARN("Failed to initialize PreTriggerBuffer for channel " << encChn);
      global_video[encChn]->prebuffer.reset();
    }
  }
#endif

  ret = IMP_Encoder_StartRecvPic(encChn);
  LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_StartRecvPic(" << encChn << ")");
  if (ret != 0)
    return 0;
  // Flush immediately after starting the encoder so the ring buffer contains
  // only a fresh IDR with consistent timestamps.  Without this, the IMP
  // encoder begins with a relative counter (starting at 0) and transitions to
  // absolute system uptime at the first GOP boundary (~4 s), causing a large
  // forward timestamp jump visible to the first RTSP client that connects.
  IMP_Encoder_RequestIDR(encChn);
  IMP_Encoder_FlushStream(encChn);
  LOG_DEBUG("IMPEncoder flush after StartRecvPic(" << encChn << ") to clear startup timestamp transition");

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

  {
    std::lock_guard<std::mutex> lock(global_video[encChn]->privacy_mutex);
    global_video[encChn]->privacy_mask.reset();
  }

#ifdef PREBUFFER_ENABLED
  // Cleanup prebuffer
  if (global_video[encChn]->prebuffer) {
    global_video[encChn]->prebuffer.reset();
    LOG_DEBUG("PreTriggerBuffer cleaned up for channel " << encChn);
  }
#endif

  return 0;
}
