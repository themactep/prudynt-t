#include "SdpGenerator.hpp"
#include <cstdio>
#include <cstring>

namespace simple_rtsp {

// Helper: hex-encode a byte array (lowercase, for profile-level-id)
static std::string hexEncode(const uint8_t *data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", data[i]);
        out += buf;
    }
    return out;
}

// Parse H.265 VPS NAL unit (with 2-byte NAL header, no start code) to extract
// profile_tier_level fields required by RFC 7798 fmtp line.
// Returns true on success.
static bool parseVpsProfileTier(const std::vector<uint8_t> &vps,
                                int &profileSpace, int &tierFlag,
                                int &profileIdc, int &levelIdc) {
    // VPS NAL unit: 2-byte header + RBSP
    //   byte 0-1: NAL header (F|Type|LayerId, LayerId|TID)
    //   byte 2:   vps_id(4)|base_internal(1)|base_available(1)|max_layers_upper(2)
    //   byte 3:   max_layers_lower(4)|max_sub_layers(3)|temporal_nesting(1)
    //   byte 4-5: reserved_0xffff_16bits
    //   byte 6:   profile_tier_level start
    //     byte 6:    profile_space(2)|tier_flag(1)|profile_idc(5)
    //     byte 7-10: compatibility_flags (32 bits)
    //     byte 11-16: constraint/reserved (48 bits)
    //     byte 17:   level_idc (8 bits)
    // Minimum VPS size: 2 (header) + 4 (vps fields) + 12 (profile_tier_level) = 18
    if (vps.size() < 18)
        return false;

    // Verify this is actually a VPS NAL (type 32)
    if (vps.size() >= 2) {
        uint8_t nalType = (vps[0] >> 1) & 0x3F;
        if (nalType != 32)
            return false;
    }

    profileSpace = (vps[6] >> 6) & 0x03;
    tierFlag    = (vps[6] >> 5) & 0x01;
    profileIdc  =  vps[6]       & 0x1F;
    levelIdc    =  vps[17];
    return true;
}

// Generate AAC AudioSpecificConfig hex string (RFC 3640, ISO 14496-3)
// Returns 4-digit hex string for two-byte config: objectType(5b) + freqIdx(4b) + chCfg(4b) + fill(3b)
static std::string makeAacConfig(unsigned sampleRate, unsigned channels) {
    // Sampling frequency index table (ISO 14496-3 Table 1.16)
    unsigned freqIdx = 15; // reserved
    static const unsigned freqs[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000,  7350
    };
    for (unsigned i = 0; i < sizeof(freqs)/sizeof(freqs[0]); ++i) {
        if (freqs[i] == sampleRate) { freqIdx = i; break; }
    }

    // objectType=2 (AAC-LC), channelConfiguration=channels
    uint16_t combined = (2u << 11) | (freqIdx << 7) | ((channels & 0x0f) << 3);
    char buf[5];
    snprintf(buf, sizeof(buf), "%04x", combined);
    return std::string(buf);
}

std::string generateSdp(const VideoStreamConfig &video,
                        const AudioStreamConfig *audio,
                        const char *serverIp,
                        const char *streamName,
                        const std::vector<BackchannelConfig> *backchannel,
                        const SubtitleStreamConfig *subtitle) {
    char buf[SDP_BUF_SIZE];
    int off = 0;

    // ── Session description ─────────────────────────────────────────────
    // Bandwidth hint (session-level, RFC 4566 §5: b= before a=)
    char bws[32] = "";
    if (video.bitrate > 0) {
        snprintf(bws, sizeof(bws), "b=AS:%d\r\n", video.bitrate);
        fprintf(stderr, "SDP: b=AS:%d (video.bitrate=%d)\n", video.bitrate, video.bitrate);
    } else {
        fprintf(stderr, "SDP: SKIPPED b=AS (video.bitrate=%d)\n", video.bitrate);
    }

    off += snprintf(buf + off, sizeof(buf) - off,
        "v=0\r\n"
        "o=- %d 1 IN IP4 %s\r\n"
        "s=%s\r\n"
        "t=0 0\r\n"
        "%s"
        "a=control:*\r\n",
        rand(), serverIp,
        streamName,
        bws);

    // ── Video media ────────────────────────────────────────────────────
    bool isH265 = (video.codec == "H265");
    const char *rtpFmt = isH265 ? "H265" : "H264";
    int pt = video.payloadType;

    off += snprintf(buf + off, sizeof(buf) - off,
        "m=video 0 RTP/AVP %d\r\n"
        "a=control:track1\r\n"
        "a=rtpmap:%d %s/%d\r\n"
        "a=framerate:%d\r\n"
        "a=framesize:%d %d-%d\r\n",
        pt,
        pt, rtpFmt, video.clockRate,
        video.fps,
        pt, video.width, video.height);

    if (video.haveCodecConfig && !video.sps.empty()) {
        if (isH265) {
            // H.265: fmtp with profile-tier-level (RFC 7798 §7.1 mandatory)
            // followed by sprop-vps, sprop-sps, sprop-pps
            int profSpace = 0, tierFlag = 0, profIdc = 1, levelIdc = 90;
            if (!video.vps.empty())
                parseVpsProfileTier(video.vps, profSpace, tierFlag, profIdc, levelIdc);

            off += snprintf(buf + off, sizeof(buf) - off,
                "a=fmtp:%d profile-space=%d;tier-flag=%d;"
                "profile-id=%d;level-id=%d\r\n",
                pt, profSpace, tierFlag, profIdc, levelIdc);

            if (!video.vps.empty()) {
                off += snprintf(buf + off, sizeof(buf) - off,
                    "a=sprop-vps=%s\r\n",
                    base64Encode(video.vps.data(), video.vps.size()).c_str());
            }
            off += snprintf(buf + off, sizeof(buf) - off,
                "a=sprop-sps=%s\r\n",
                base64Encode(video.sps.data(), video.sps.size()).c_str());
        } else {
            // H.264: fmtp with packetization-mode=1 (required for FU-A)
            std::string profileLevelId;
            std::string sprop;
            if (video.sps.size() >= 4) {
                std::vector<uint8_t> sps = video.sps;
                // Read profile-level-id from the (already-patched) SPS instead
                // of hardcoding a fixed value. VideoWorker rewrites level_idc
                // to the minimum level that fits the real resolution/fps.
                profileLevelId = hexEncode(sps.data() + 1, 3);
                sprop = base64Encode(video.sps.data(), video.sps.size());
            } else {
                profileLevelId = "42001f";
                sprop = base64Encode(video.sps.data(), video.sps.size());
            }
            if (!video.pps.empty()) {
                sprop += ",";
                sprop += base64Encode(video.pps.data(), video.pps.size());
            }
            off += snprintf(buf + off, sizeof(buf) - off,
                "a=fmtp:%d packetization-mode=1;profile-level-id=%s;"
                "sprop-parameter-sets=%s\r\n",
                pt, profileLevelId.c_str(), sprop.c_str());
        }
        // PPS for H.265
        if (isH265 && !video.pps.empty()) {
            off += snprintf(buf + off, sizeof(buf) - off,
                "a=sprop-pps=%s\r\n",
                base64Encode(video.pps.data(), video.pps.size()).c_str());
        }
    }

    // ── Audio media ────────────────────────────────────────────────────
    if (audio) {
        const char *encName = "mpeg4-generic";
        int audioClk = audio->sampleRate;
        int audioCh = audio->channels;

        if (audio->codec == "OPUS") {
            encName = "OPUS";
            audioClk = 48000;
            audioCh = 2; // Opus SDP always advertises stereo
        } else if (audio->codec == "PCMU") {
            encName = "PCMU";
            audioClk = 8000;
        } else if (audio->codec == "PCMA") {
            encName = "PCMA";
            audioClk = 8000;
        } else if (audio->codec == "L16") {
            encName = "L16";
        }

        off += snprintf(buf + off, sizeof(buf) - off,
            "m=audio 0 RTP/AVP %d\r\n"
            "a=control:track2\r\n"
            "a=rtpmap:%d %s/%d/%d\r\n",
            audio->payloadType,
            audio->payloadType, encName, audioClk, audioCh);

        if (audio->codec == "AAC") {
            // Use the encoder's real ASC if provided (essential for HE-AAC),
            // otherwise fall back to the hardcoded makeAacConfig.
            std::string aacCfg;
            if (!audio->aacConfig.empty()) {
                aacCfg = hexEncode(audio->aacConfig.data(), audio->aacConfig.size());
            } else {
                aacCfg = makeAacConfig(audio->sampleRate, audio->channels);
            }
            off += snprintf(buf + off, sizeof(buf) - off,
                "a=fmtp:%d streamtype=5;profile-level-id=15;mode=AAC-hbr;"
                "config=%s;"
                "sizelength=13;indexlength=3;indexdeltalength=3\r\n",
                audio->payloadType, aacCfg.c_str());
        }
    }

    // ── Subtitle media (OSD text) ────────────────────────────────────
    if (subtitle) {
        off += snprintf(buf + off, sizeof(buf) - off,
            "m=text 0 RTP/AVP %d\r\n"
            "a=control:track4\r\n"
            "a=rtpmap:%d %s/%d\r\n"
            "a=ptime:1.000\r\n",
            subtitle->payloadType,
            subtitle->payloadType,
            subtitle->codec.c_str(),
            subtitle->clockRate);
    }

    // ── Backchannel (talkback) — announced in main SDP so go2rtc
    // discovers it via the same RTSP session.  Clients that don't
    // support talkback simply ignore the sendonly track.
    // Per ONVIF Streaming Spec §5.3, backchannel tracks use
    // a=sendonly (client→server direction), matching the legacy
    // live555 server behaviour that go2rtc expects.
    if (backchannel && !backchannel->empty()) {
        off += snprintf(buf + off, sizeof(buf) - off, "m=audio 0 RTP/AVP");
        for (const auto &bc : *backchannel)
            off += snprintf(buf + off, sizeof(buf) - off, " %d", bc.payloadType);
        off += snprintf(buf + off, sizeof(buf) - off,
            "\r\n"
            "a=control:track0\r\n"
            "a=sendonly\r\n");
        for (const auto &bc : *backchannel) {
            const char *encName = bc.codec.c_str();
            int clk = bc.sampleRate;
            if (bc.codec == "PCMU") clk = 8000;
            else if (bc.codec == "PCMA") clk = 8000;
            if (bc.codec == "OPUS")
                off += snprintf(buf + off, sizeof(buf) - off,
                    "a=rtpmap:%d %s/%d/2\r\n", bc.payloadType, encName, clk);
            else
                off += snprintf(buf + off, sizeof(buf) - off,
                    "a=rtpmap:%d %s/%d\r\n", bc.payloadType, encName, clk);
            if (bc.codec == "mpeg4-generic") {
                std::string aacCfg = makeAacConfig(bc.sampleRate, 1);
                off += snprintf(buf + off, sizeof(buf) - off,
                    "a=fmtp:%d streamtype=5;profile-level-id=15;mode=AAC-hbr;"
                    "config=%s;"
                    "sizelength=13;indexlength=3;indexdeltalength=3\r\n",
                    bc.payloadType, aacCfg.c_str());
            }
        }
    }

    return std::string(buf);
}

std::string generateAudioOnlySdp(const AudioStreamConfig &audio,
                                 const char *serverIp,
                                 const char *streamName) {
    char buf[SDP_BUF_SIZE];
    int off = 0;

    off += snprintf(buf + off, sizeof(buf) - off,
        "v=0\r\n"
        "o=- %d 1 IN IP4 %s\r\n"
        "s=%s\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n",
        rand(), serverIp,
        streamName);

    const char *encName = "mpeg4-generic";
    int audioClk = audio.sampleRate;
    int audioCh = audio.channels;

    if (audio.codec == "OPUS") {
        encName = "OPUS";
        audioClk = 48000;
        audioCh = 2;
    } else if (audio.codec == "PCMU") {
        encName = "PCMU";
        audioClk = 8000;
    } else if (audio.codec == "PCMA") {
        encName = "PCMA";
        audioClk = 8000;
    } else if (audio.codec == "L16") {
        encName = "L16";
    }

    off += snprintf(buf + off, sizeof(buf) - off,
        "m=audio 0 RTP/AVP %d\r\n"
        "a=control:track1\r\n"
        "a=rtpmap:%d %s/%d/%d\r\n",
        audio.payloadType,
        audio.payloadType, encName, audioClk, audioCh);

    if (audio.codec == "AAC") {
        std::string aacCfg;
        if (!audio.aacConfig.empty()) {
            aacCfg = hexEncode(audio.aacConfig.data(), audio.aacConfig.size());
        } else {
            aacCfg = makeAacConfig(audio.sampleRate, audio.channels);
        }
        off += snprintf(buf + off, sizeof(buf) - off,
            "a=fmtp:%d streamtype=5;profile-level-id=15;mode=AAC-hbr;"
            "config=%s;"
            "sizelength=13;indexlength=3;indexdeltalength=3\r\n",
            audio.payloadType, aacCfg.c_str());
    }

    return std::string(buf);
}

std::string generateBackchannelSdp(const std::vector<BackchannelConfig> &formats,
                                   const char *serverIp,
                                   const char *streamName) {
    char buf[SDP_BUF_SIZE];
    int off = 0;

    off += snprintf(buf + off, sizeof(buf) - off,
        "v=0\r\n"
        "o=- %d 1 IN IP4 %s\r\n"
        "s=%s\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n",
        rand(), serverIp,
        streamName);

    if (formats.empty()) {
        off += snprintf(buf + off, sizeof(buf) - off,
            "m=audio 0 RTP/AVP 0\r\n"
            "a=control:track0\r\n"
            "a=sendonly\r\n"
            "a=rtpmap:0 PCMU/8000\r\n");
    } else {
        off += snprintf(buf + off, sizeof(buf) - off, "m=audio 0 RTP/AVP");
        for (const auto &bc : formats)
            off += snprintf(buf + off, sizeof(buf) - off, " %d", bc.payloadType);
        off += snprintf(buf + off, sizeof(buf) - off,
            "\r\n"
            "a=control:track0\r\n"
            "a=sendonly\r\n");
        for (const auto &bc : formats) {
            const char *encName = bc.codec.c_str();
            int clk = bc.sampleRate;
            if (bc.codec == "PCMU") clk = 8000;
            else if (bc.codec == "PCMA") clk = 8000;
            if (bc.codec == "OPUS")
                off += snprintf(buf + off, sizeof(buf) - off,
                    "a=rtpmap:%d %s/%d/2\r\n", bc.payloadType, encName, clk);
            else
                off += snprintf(buf + off, sizeof(buf) - off,
                    "a=rtpmap:%d %s/%d\r\n", bc.payloadType, encName, clk);
            if (bc.codec == "mpeg4-generic") {
                std::string aacCfg = makeAacConfig(bc.sampleRate, 1);
                off += snprintf(buf + off, sizeof(buf) - off,
                    "a=fmtp:%d streamtype=5;profile-level-id=15;mode=AAC-hbr;"
                    "config=%s;"
                    "sizelength=13;indexlength=3;indexdeltalength=3\r\n",
                    bc.payloadType, aacCfg.c_str());
            }
        }
    }

    return std::string(buf);
}

} // namespace simple_rtsp
