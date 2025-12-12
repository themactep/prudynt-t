// MP4Muxer_simple.cpp - minimal fragmented MP4 writer for H.264/AAC
// Generates an init segment (ftyp+moov) and moof+mdat fragments
// for each video/audio sample. No external dependencies.

#include "Logger.hpp"
#include "MP4Muxer.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

static void write_u32(std::vector<uint8_t> &out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

static void write_u16(std::vector<uint8_t> &out, uint16_t v) {
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

static void write_u8(std::vector<uint8_t> &out, uint8_t v) {
  out.push_back(v);
}

static void write_u24(std::vector<uint8_t> &out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

static void write_box(std::vector<uint8_t> &out, const char type[4], const std::vector<uint8_t> &payload) {
  uint32_t size = 8u + static_cast<uint32_t>(payload.size());
  write_u32(out, size);
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), payload.begin(), payload.end());
}

struct TrackState {
  bool enabled = false;
  uint32_t track_id = 0;
  uint32_t timescale = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t channels = 0;
  uint32_t default_duration = 0;
  std::vector<uint8_t> codec_config; // avcC or AudioSpecificConfig
  uint64_t last_pts = 0;
  bool have_last_pts = false;
};

class SimpleMP4Muxer : public MP4Muxer {
public:
  SimpleMP4Muxer() = default;
  ~SimpleMP4Muxer() override = default;

  bool init(const InitParams &params) override {
    video_.enabled = true;
    video_.track_id = 1;
    video_.timescale = 90000; // common video timescale
    video_.width = static_cast<uint32_t>(params.width);
    video_.height = static_cast<uint32_t>(params.height);
    video_.codec_config = params.avcC;
    if (params.fps > 0) {
      video_.default_duration = static_cast<uint32_t>(video_.timescale / params.fps);
    }

    if (!params.aacConfig.empty()) {
      audio_.enabled = true;
      audio_.track_id = 2;
      audio_.timescale = static_cast<uint32_t>(params.sampleRate);
      audio_.codec_config = params.aacConfig;
      audio_.channels = static_cast<uint16_t>(params.channels);
      audio_.default_duration = 1024; // AAC-LC frame size in samples
      LOG_INFO("SimpleMP4Muxer: audio enabled sample_rate=" << audio_.timescale << "Hz channels=" << audio_.channels
                                                            << " config_bytes=" << audio_.codec_config.size());
    } else {
      LOG_INFO("SimpleMP4Muxer: audio disabled (no AAC config provided)");
    }

    next_seq_ = 1;
    return true;
  }

  std::vector<uint8_t> getInitSegment() override {
    std::vector<uint8_t> out;
    write_ftyp(out);
    write_moov(out);
    return out;
  }

  std::vector<uint8_t> muxVideo(const uint8_t *data, size_t size, int64_t pts_ms, bool isKey) override {
    if (!video_.enabled || !data || size == 0) {
      return {};
    }
    return write_fragment(video_, next_seq_++, data, size, pts_ms, isKey, true);
  }

  std::vector<uint8_t> muxAudio(const uint8_t *data, size_t size, int64_t pts_ms) override {
    if (!audio_.enabled) {
      if (!audio_warn_no_track_reported_) {
        LOG_WARN("SimpleMP4Muxer: muxAudio called but audio track is not "
                 "initialized");
        audio_warn_no_track_reported_ = true;
      }
      return {};
    }
    if (!data || size == 0) {
      if (!audio_warn_empty_sample_reported_) {
        LOG_WARN("SimpleMP4Muxer: muxAudio received an empty audio sample");
        audio_warn_empty_sample_reported_ = true;
      }
      return {};
    }

    uint32_t seq = next_seq_;
    auto fragment = write_fragment(audio_, next_seq_++, data, size, pts_ms, false, false);
    audio_sample_count_++;
    if (audio_sample_count_ == 1) {
      LOG_INFO("SimpleMP4Muxer: first audio sample accepted seq=" << seq << " pts_ms=" << pts_ms << " bytes=" << size);
    } else if ((audio_sample_count_ % 50) == 0) {
      LOG_DDEBUG("SimpleMP4Muxer(audio): sample #" << audio_sample_count_ << " seq=" << seq << " pts_ms=" << pts_ms
                                                   << " bytes=" << size);
    }
    return fragment;
  }

  void close() override {
  }

private:
  void write_ftyp(std::vector<uint8_t> &out) {
    std::vector<uint8_t> payload;
    // major_brand 'iso5'
    payload.insert(payload.end(), {'i', 's', 'o', '5'});
    // minor_version
    write_u32(payload, 0x00000200u);
    // compatible_brands: iso5, iso6, mp41
    payload.insert(payload.end(), {'i', 's', 'o', '5'});
    payload.insert(payload.end(), {'i', 's', 'o', '6'});
    payload.insert(payload.end(), {'m', 'p', '4', '1'});
    write_box(out, "ftyp", payload);
  }

  void write_moov(std::vector<uint8_t> &out) {
    std::vector<uint8_t> moov_payload;

    // mvhd
    {
      std::vector<uint8_t> mvhd;
      write_u8(mvhd, 0);            // version
      write_u24(mvhd, 0);           // flags
      write_u32(mvhd, 0);           // creation_time
      write_u32(mvhd, 0);           // modification_time
      write_u32(mvhd, 90000);       // timescale
      write_u32(mvhd, 0);           // duration (0 for fragmented)
      write_u32(mvhd, 0x00010000u); // rate 1.0
      write_u16(mvhd, 0x0100);      // volume 1.0
      write_u16(mvhd, 0);           // reserved
      write_u32(mvhd, 0);           // reserved
      write_u32(mvhd, 0);           // reserved
      // matrix
      write_u32(mvhd, 0x00010000u);
      write_u32(mvhd, 0);
      write_u32(mvhd, 0);
      write_u32(mvhd, 0);
      write_u32(mvhd, 0x00010000u);
      write_u32(mvhd, 0);
      write_u32(mvhd, 0);
      write_u32(mvhd, 0);
      write_u32(mvhd, 0x40000000u);
      // pre_defined
      for (int i = 0; i < 6; ++i)
        write_u32(mvhd, 0);
      // next_track_ID
      write_u32(mvhd, audio_.enabled ? 3u : 2u);
      write_box(moov_payload, "mvhd", mvhd);
    }

    if (video_.enabled) {
      write_trak(moov_payload, video_, true);
    }
    if (audio_.enabled) {
      write_trak(moov_payload, audio_, false);
    }

    // mvex/trex for each track (default fragment values)
    {
      std::vector<uint8_t> mvex;
      if (video_.enabled)
        write_trex(mvex, video_.track_id);
      if (audio_.enabled)
        write_trex(mvex, audio_.track_id);
      write_box(moov_payload, "mvex", mvex);
    }

    write_box(out, "moov", moov_payload);
  }

  void write_trex(std::vector<uint8_t> &out, uint32_t track_id) {
    std::vector<uint8_t> trex;
    write_u8(trex, 0);  // version
    write_u24(trex, 0); // flags
    write_u32(trex, track_id);
    write_u32(trex, 1); // default_sample_description_index
    write_u32(trex, 0); // default_sample_duration
    write_u32(trex, 0); // default_sample_size
    write_u32(trex, 0); // default_sample_flags
    write_box(out, "trex", trex);
  }

  void write_trak(std::vector<uint8_t> &out, const TrackState &t, bool is_video) {
    std::vector<uint8_t> trak;

    // tkhd
    {
      std::vector<uint8_t> tkhd;
      write_u8(tkhd, 0);
      // flags: track enabled | in movie
      write_u24(tkhd, 0x000007u);
      write_u32(tkhd, 0); // creation_time
      write_u32(tkhd, 0); // modification_time
      write_u32(tkhd, t.track_id);
      write_u32(tkhd, 0);                     // reserved
      write_u32(tkhd, 0);                     // duration (0 for fragmented)
      write_u32(tkhd, 0);                     // reserved
      write_u32(tkhd, 0);                     // reserved
      write_u16(tkhd, 0);                     // layer
      write_u16(tkhd, 0);                     // alternate_group
      write_u16(tkhd, is_video ? 0 : 0x0100); // volume
      write_u16(tkhd, 0);                     // reserved
      // matrix
      write_u32(tkhd, 0x00010000u);
      write_u32(tkhd, 0);
      write_u32(tkhd, 0);
      write_u32(tkhd, 0);
      write_u32(tkhd, 0x00010000u);
      write_u32(tkhd, 0);
      write_u32(tkhd, 0);
      write_u32(tkhd, 0);
      write_u32(tkhd, 0x40000000u);
      // width/height (16.16)
      write_u32(tkhd, is_video ? (t.width << 16) : 0);
      write_u32(tkhd, is_video ? (t.height << 16) : 0);
      write_box(trak, "tkhd", tkhd);
    }

    // mdia
    std::vector<uint8_t> mdia;
    {
      // mdhd
      std::vector<uint8_t> mdhd;
      write_u8(mdhd, 0);
      write_u24(mdhd, 0); // flags
      write_u32(mdhd, 0); // creation_time
      write_u32(mdhd, 0); // modification_time
      write_u32(mdhd, t.timescale);
      write_u32(mdhd, 0);      // duration
      write_u16(mdhd, 0x55c4); // language (und)
      write_u16(mdhd, 0);      // pre_defined
      write_box(mdia, "mdhd", mdhd);
    }
    {
      // hdlr
      std::vector<uint8_t> hdlr;
      write_u8(hdlr, 0);
      write_u24(hdlr, 0); // flags
      write_u32(hdlr, 0); // pre_defined
      if (is_video) {
        hdlr.insert(hdlr.end(), {'v', 'i', 'd', 'e'});
      } else {
        hdlr.insert(hdlr.end(), {'s', 'o', 'u', 'n'});
      }
      write_u32(hdlr, 0); // reserved
      write_u32(hdlr, 0); // reserved
      write_u32(hdlr, 0); // reserved
      // name (empty)
      write_u8(hdlr, 0);
      write_box(mdia, "hdlr", hdlr);
    }

    // minf
    std::vector<uint8_t> minf;
    {
      if (is_video) {
        std::vector<uint8_t> vmhd;
        write_u8(vmhd, 0);  // version
        write_u24(vmhd, 1); // flags: quicktime requirement
        write_u16(vmhd, 0); // graphicsmode
        write_u16(vmhd, 0); // opcolor[0]
        write_u16(vmhd, 0); // opcolor[1]
        write_u16(vmhd, 0); // opcolor[2]
        write_box(minf, "vmhd", vmhd);
      } else {
        std::vector<uint8_t> smhd;
        write_u8(smhd, 0);  // version
        write_u24(smhd, 0); // flags
        write_u16(smhd, 0); // balance
        write_u16(smhd, 0);
        write_box(minf, "smhd", smhd);
      }
    }
    {
      // dinf/dref/url
      std::vector<uint8_t> dref_payload;
      write_u8(dref_payload, 0);  // version
      write_u24(dref_payload, 0); // flags
      write_u32(dref_payload, 1); // entry_count
      std::vector<uint8_t> url;
      write_u8(url, 0);
      write_u24(url, 0x000001u); // flags: self-contained
      write_box(dref_payload, "url ", url);
      std::vector<uint8_t> dinf;
      write_box(dinf, "dref", dref_payload);
      write_box(minf, "dinf", dinf);
    }

    // stbl
    std::vector<uint8_t> stbl;
    {
      // stsd
      std::vector<uint8_t> stsd;
      write_u8(stsd, 0);
      write_u24(stsd, 0); // flags
      write_u32(stsd, 1); // entry_count

      if (is_video) {
        // avc1 sample entry
        std::vector<uint8_t> avc1;
        write_u32(avc1, 0); // reserved (first 4 bytes)
        write_u16(avc1, 0); // reserved (next 2 bytes)
        write_u16(avc1, 1); // data_reference_index
        write_u16(avc1, 0); // pre_defined
        write_u16(avc1, 0); // reserved
        for (int i = 0; i < 3; ++i)
          write_u32(avc1, 0);
        write_u16(avc1, static_cast<uint16_t>(t.width));
        write_u16(avc1, static_cast<uint16_t>(t.height));
        write_u32(avc1, 0x00480000u); // horizresolution 72 dpi
        write_u32(avc1, 0x00480000u); // vertresolution
        write_u32(avc1, 0);           // reserved
        write_u16(avc1, 1);           // frame_count
        // compressorname (32 bytes)
        write_u8(avc1, 0);
        for (int i = 0; i < 31; ++i)
          write_u8(avc1, 0);
        write_u16(avc1, 0x0018); // depth
        write_u16(avc1, 0xFFFF); // pre_defined

        // avcC box with codec config
        std::vector<uint8_t> avcC = t.codec_config;
        write_box(avc1, "avcC", avcC);

        write_box(stsd, "avc1", avc1);
      } else {
        // mp4a sample entry
        std::vector<uint8_t> mp4a;
        write_u32(mp4a, 0); // reserved (first 4 bytes)
        write_u16(mp4a, 0); // reserved (next 2 bytes)
        write_u16(mp4a, 1); // data_reference_index
        write_u32(mp4a, 0); // reserved (8 bytes total)
        write_u32(mp4a, 0);
        uint16_t channelcount = t.channels ? t.channels : 2;
        write_u16(mp4a, channelcount);      // channelcount
        write_u16(mp4a, 16);                // samplesize
        write_u16(mp4a, 0);                 // pre_defined
        write_u16(mp4a, 0);                 // reserved
        write_u32(mp4a, t.timescale << 16); // samplerate 16.16

        // esds with AudioSpecificConfig
        std::vector<uint8_t> esds;
        write_u8(esds, 0);  // version
        write_u24(esds, 0); // flags
        // ES_Descriptor tag 0x03
        write_u8(esds, 0x03);
        write_u8(esds, 0x19); // length (approx, minimal)
        write_u16(esds, 1);   // ES_ID
        write_u8(esds, 0);    // flags
        // DecoderConfigDescriptor 0x04
        write_u8(esds, 0x04);
        write_u8(esds, 0x11);
        write_u8(esds, 0x40); // object type (AAC LC)
        write_u8(esds, 0x15); // stream type
        write_u24(esds, 0);   // bufferSizeDB (stubbed)
        write_u32(esds, 0);   // maxBitrate
        write_u32(esds, 0);   // avgBitrate
        // DecoderSpecificInfo 0x05
        write_u8(esds, 0x05);
        write_u8(esds, static_cast<uint8_t>(t.codec_config.size()));
        esds.insert(esds.end(), t.codec_config.begin(), t.codec_config.end());
        // SLConfigDescriptor 0x06
        write_u8(esds, 0x06);
        write_u8(esds, 0x01);
        write_u8(esds, 0x02);

        write_box(mp4a, "esds", esds);
        write_box(stsd, "mp4a", mp4a);
      }

      write_box(stbl, "stsd", stsd);
    }
    {
      // empty stts/stsc/stsz/stco for fragmented MP4
      std::vector<uint8_t> stts;
      write_u8(stts, 0);
      write_u24(stts, 0);
      write_u32(stts, 0); // entry_count
      write_box(stbl, "stts", stts);

      std::vector<uint8_t> stsc;
      write_u8(stsc, 0);
      write_u24(stsc, 0);
      write_u32(stsc, 0);
      write_box(stbl, "stsc", stsc);

      std::vector<uint8_t> stsz;
      write_u8(stsz, 0);
      write_u24(stsz, 0);
      write_u32(stsz, 0); // sample_size
      write_u32(stsz, 0); // sample_count
      write_box(stbl, "stsz", stsz);

      std::vector<uint8_t> stco;
      write_u8(stco, 0);
      write_u24(stco, 0);
      write_u32(stco, 0);
      write_box(stbl, "stco", stco);
    }

    write_box(minf, "stbl", stbl);
    write_box(mdia, "minf", minf);
    write_box(trak, "mdia", mdia);
    write_box(out, "trak", trak);
  }

  std::vector<uint8_t> write_fragment(TrackState &t, uint32_t seq, const uint8_t *data, size_t size, int64_t pts_ms,
                                      bool isKey, bool isVideo) {
    std::vector<uint8_t> out;

    uint64_t pts = (pts_ms <= 0) ? 0 : static_cast<uint64_t>(pts_ms) * t.timescale / 1000ull;
    const uint64_t prev_pts = t.last_pts;

    uint32_t sample_duration = 0;
    bool duration_was_clamped = false;
    bool used_default_duration = false;
    bool forced_min_duration = false;
    bool pts_regressed = (t.have_last_pts && pts <= prev_pts);

    if (t.have_last_pts && pts > t.last_pts) {
      uint64_t delta = pts - t.last_pts;
      if (delta > 0xFFFFFFFFull) {
        delta = 0xFFFFFFFFull;
        duration_was_clamped = true;
      }
      sample_duration = static_cast<uint32_t>(delta);
    }
    if (sample_duration == 0) {
      sample_duration = t.default_duration;
      used_default_duration = (sample_duration != 0);
    }
    if (sample_duration == 0) {
      sample_duration = isVideo ? (t.timescale / 30) : (t.timescale / 50);
      forced_min_duration = true;
      if (sample_duration == 0) {
        sample_duration = 1;
      }
    }

    if (!isVideo) {
      if (pts_regressed && audio_pts_regress_warnings_ < 5) {
        LOG_WARN("SimpleMP4Muxer(audio): non-monotonic PTS (prev=" << prev_pts << " new=" << pts << ")");
        ++audio_pts_regress_warnings_;
      }
      if (duration_was_clamped && audio_duration_clamp_warnings_ < 5) {
        LOG_WARN("SimpleMP4Muxer(audio): clamped sample duration to 0xFFFFFFFF");
        ++audio_duration_clamp_warnings_;
      }
      if (used_default_duration && audio_duration_fallback_warnings_ < 5) {
        LOG_INFO("SimpleMP4Muxer(audio): using default duration=" << sample_duration);
        ++audio_duration_fallback_warnings_;
      }
      if (forced_min_duration && audio_forced_duration_warnings_ < 5) {
        LOG_WARN("SimpleMP4Muxer(audio): synthesized duration due to missing "
                 "timing -> "
                 << sample_duration);
        ++audio_forced_duration_warnings_;
      }
    }

    t.last_pts = pts;
    t.have_last_pts = true;

    // moof
    std::vector<uint8_t> moof;
    {
      // mfhd
      std::vector<uint8_t> mfhd;
      write_u8(mfhd, 0);
      write_u24(mfhd, 0);
      write_u32(mfhd, seq);
      write_box(moof, "mfhd", mfhd);
    }
    size_t data_offset_patch_pos = 0;
    {
      std::vector<uint8_t> traf;
      // tfhd
      std::vector<uint8_t> tfhd;
      write_u8(tfhd, 0);
      // flags: default-base-is-moof (0x020000)
      write_u24(tfhd, 0x020000u);
      write_u32(tfhd, t.track_id);
      write_box(traf, "tfhd", tfhd);

      // tfdt
      std::vector<uint8_t> tfdt;
      write_u8(tfdt, 0);
      write_u24(tfdt, 0);
      write_u32(tfdt, static_cast<uint32_t>(pts));
      write_box(traf, "tfdt", tfdt);

      // trun (one sample)
      std::vector<uint8_t> trun;
      write_u8(trun, 0);
      // flags: data-offset-present | sample-duration-present |
      // sample-size-present | sample-flags-present
      write_u24(trun, 0x000701u);
      write_u32(trun, 1); // sample_count
      // data_offset placeholder (patched later)
      write_u32(trun, 0);

      write_u32(trun, sample_duration);
      write_u32(trun, static_cast<uint32_t>(size));
      uint32_t flags = isVideo ? (isKey ? 0x02000000u : 0x01010000u) : 0x00000000u;
      write_u32(trun, flags);

      const size_t trun_box_offset = traf.size();
      write_box(traf, "trun", trun);

      const size_t traf_box_offset = moof.size();
      write_box(moof, "traf", traf);

      // data_offset bytes live inside the trun payload (after version/flags +
      // sample_count)
      data_offset_patch_pos = traf_box_offset + 8 + trun_box_offset + 16;
    }

    // Now we know moof size; data_offset in trun = moof_size + 8 (size+type of
    // mdat)
    uint32_t moof_size = 8u + static_cast<uint32_t>(moof.size());
    uint32_t data_offset = moof_size + 8u; // mdat header

    // Patch data_offset in trun (if we recorded a valid position)
    if (data_offset_patch_pos && data_offset_patch_pos + 4 <= moof.size()) {
      moof[data_offset_patch_pos + 0] = static_cast<uint8_t>((data_offset >> 24) & 0xFF);
      moof[data_offset_patch_pos + 1] = static_cast<uint8_t>((data_offset >> 16) & 0xFF);
      moof[data_offset_patch_pos + 2] = static_cast<uint8_t>((data_offset >> 8) & 0xFF);
      moof[data_offset_patch_pos + 3] = static_cast<uint8_t>(data_offset & 0xFF);
    }

    write_box(out, "moof", moof);

    // mdat with raw sample
    std::vector<uint8_t> mdat_payload;
    // For video, assume NAL units are already length-prefixed or Annex-B; we
    // just dump as-is. For real-world use, you may want to convert Annex-B to
    // length-prefixed here.
    mdat_payload.insert(mdat_payload.end(), data, data + size);
    write_box(out, "mdat", mdat_payload);

    (void)isVideo; // currently unused, reserved for future logic
    return out;
  }

  TrackState video_;
  TrackState audio_;
  uint32_t next_seq_ = 1;
  bool audio_warn_no_track_reported_ = false;
  bool audio_warn_empty_sample_reported_ = false;
  uint64_t audio_sample_count_ = 0;
  uint32_t audio_pts_regress_warnings_ = 0;
  uint32_t audio_duration_fallback_warnings_ = 0;
  uint32_t audio_forced_duration_warnings_ = 0;
  uint32_t audio_duration_clamp_warnings_ = 0;
};

} // namespace

extern "C" MP4Muxer *CreateSimpleMP4Muxer() {
  return new SimpleMP4Muxer();
}

extern "C" void DestroySimpleMP4Muxer(MP4Muxer *m) {
  delete m;
}
