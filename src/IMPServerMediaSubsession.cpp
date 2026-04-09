#include "IMPServerMediaSubsession.hpp"
#include "Config.hpp"
#include "GroupsockHelper.hh"
#include "H264VideoRTPSink.hh"
#include "H264VideoStreamDiscreteFramer.hh"
#include "H265VideoRTPSink.hh"
#include "H265VideoStreamDiscreteFramer.hh"
#include "IMPDeviceSource.hpp"
#include <iostream>
#include <memory>
#include <sys/socket.h>

// Modify method to accept pointers for the NAL units
IMPServerMediaSubsession *IMPServerMediaSubsession::createNew(UsageEnvironment &env,
                                                              H264NALUnit *vps, // Change to pointer to make optional
                                                              H264NALUnit sps, H264NALUnit pps, int encChn) {
  // Pass along the pointers; they may be nullptr
  return new IMPServerMediaSubsession(env, vps, sps, pps, encChn);
}

// Modify the constructor accordingly
IMPServerMediaSubsession::IMPServerMediaSubsession(UsageEnvironment &env,
                                                   H264NALUnit *vps, // Change to pointer to make optional
                                                   H264NALUnit sps, H264NALUnit pps, int encChn)
    : OnDemandServerMediaSubsession(env, true), vps(vps ? new H264NALUnit(*vps) : nullptr), // Copy if not nullptr
      sps(sps), pps(pps), encChn(encChn) {
  LOG_DEBUG("IMPServerMediaSubsession ctor: ch=" << encChn
            << " sps.size=" << this->sps.data.size()
            << " pps.size=" << this->pps.data.size()
            << " vps_addr=" << reinterpret_cast<uintptr_t>(this->vps));
}

// Destructor - we should delete the VPS if it was allocated
IMPServerMediaSubsession::~IMPServerMediaSubsession() {
  LOG_DEBUG("IMPServerMediaSubsession dtor: ch=" << encChn
            << " vps_addr=" << reinterpret_cast<uintptr_t>(vps)
            << " fSDPLines_addr=" << reinterpret_cast<uintptr_t>(fSDPLines));
  delete vps; // Safe to delete nullptr if vps is not set
}

FramedSource *IMPServerMediaSubsession::createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate) {
  LOG_DEBUG("Create Stream Source. encChn=" << encChn << " clientSessionId=" << clientSessionId);
  estBitrate = cfg->rtsp.est_bitrate; // The expected bitrate?

  auto imp = IMPDeviceSource<H264NALUnit, video_stream>::createNew(envir(), encChn, global_video[encChn], "video");
  if (!imp) {
    LOG_ERROR("createNewStreamSource: IMPDeviceSource::createNew returned null for ch" << encChn);
    return nullptr;
  }
  // Here we need to decide based on the format whether to use H264 or H265
  // framer
  FramedSource *framer = nullptr;
  if (vps) {
    framer = H265VideoStreamDiscreteFramer::createNew(envir(), imp, false, false);
  } else { // Let's assume the default is H264 if not H265
    framer = H264VideoStreamDiscreteFramer::createNew(envir(), imp, false, false);
  }
  LOG_DEBUG("createNewStreamSource: returning framer_addr=" << reinterpret_cast<uintptr_t>(framer)
            << " for ch" << encChn);
  return framer;
}

// Modify RTP Sink creation to conditionally include VPS
RTPSink *IMPServerMediaSubsession::createNewRTPSink(Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
                                                    FramedSource *fs) {
  LOG_DEBUG("createNewRTPSink: ch=" << encChn << " sps.size=" << sps.data.size()
            << " pps.size=" << pps.data.size() << " vps_addr="
            << reinterpret_cast<uintptr_t>(vps));
  if (sps.data.empty()) {
    LOG_ERROR("createNewRTPSink: SPS data is empty for ch" << encChn << " — this will crash!");
  }
  if (pps.data.empty()) {
    LOG_ERROR("createNewRTPSink: PPS data is empty for ch" << encChn << " — this will crash!");
  }
  increaseSendBufferTo(envir(), rtpGroupsock->socketNum(), cfg->rtsp.send_buffer_size);

  // Set send timeout to detect and disconnect stalled RTSP clients
  // (inspired by go2rtc which uses a 5s write deadline on TCP sockets)
  if (cfg->rtsp.send_timeout_s > 0) {
    struct timeval tv;
    tv.tv_sec = cfg->rtsp.send_timeout_s;
    tv.tv_usec = 0;
    setsockopt(rtpGroupsock->socketNum(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }
  // Use VPS only if it's available (non-nullptr, and we are in H265 mode)
  if (vps) {
    RTPSink *sink = H265VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic, &vps->data[0],
                                                vps->data.size(), // Now using pointer, check and dereference
                                                &sps.data[0], sps.data.size(), &pps.data[0], pps.data.size());
    LOG_DEBUG("createNewRTPSink: created H265 sink_addr=" << reinterpret_cast<uintptr_t>(sink)
              << " for ch=" << encChn);
    return sink;
  } else {
    // For H264 or other formats, VPS is not used
    RTPSink *sink = H264VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic, &sps.data[0],
                                                sps.data.size(), &pps.data[0], pps.data.size());
    LOG_DEBUG("createNewRTPSink: created H264 sink_addr=" << reinterpret_cast<uintptr_t>(sink)
              << " for ch=" << encChn);
    return sink;
  }

  // enabling this allows stream resolution changes
  // not only the first sdp is used
  // delete[] fSDPLines; fSDPLines = NULL;
}

char const *IMPServerMediaSubsession::sdpLines(int addressFamily) {
  // Check if encoder codec config (SPS/PPS/VPS) has changed since last SDP.
  // If so, update our copies and invalidate cached SDP so live555 regenerates it.
  // This enables dynamic resolution/profile changes without RTSP server restart.
  LOG_DEBUG("sdpLines: ch=" << encChn << " sps.size=" << sps.data.size() << " pps.size=" << pps.data.size());
  if (encChn >= 0 && encChn < NUM_VIDEO_CHANNELS && global_video[encChn]) {
    std::lock_guard<std::mutex> lock(global_video[encChn]->parameterCache.mutex);
    const auto &cache = global_video[encChn]->parameterCache;
    bool changed = false;

    if (cache.have_sps && cache.sps.data != lastKnownSps) {
      sps.data = cache.sps.data;
      lastKnownSps = cache.sps.data;
      changed = true;
    }
    if (cache.have_pps && cache.pps.data != lastKnownPps) {
      pps.data = cache.pps.data;
      lastKnownPps = cache.pps.data;
      changed = true;
    }
    if (vps && cache.have_vps && cache.vps.data != lastKnownVps) {
      vps->data = cache.vps.data;
      lastKnownVps = cache.vps.data;
      changed = true;
    }

    if (changed) {
      LOG_DEBUG("sdpLines: codec config changed for ch=" << encChn
                << " new sps.size=" << sps.data.size()
                << " pps.size=" << pps.data.size()
                << " vps.size=" << (vps ? vps->data.size() : 0));
      delete[] fSDPLines;
      fSDPLines = NULL;
    }
  }

  char const *lines = OnDemandServerMediaSubsession::sdpLines(addressFamily);
  LOG_DEBUG("sdpLines: base returned addr=" << reinterpret_cast<uintptr_t>(lines)
            << " for ch=" << encChn);
  return lines;
}
