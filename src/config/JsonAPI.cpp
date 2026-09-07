#include "config/JsonAPI.hpp"
#include "audio/AudioOutputWorker.hpp"
#include "config/Config.hpp"
#include "recording/MP4Recorder.hpp"
#include "stream/globals.hpp"
#include "isp/imp_hal.hpp"

extern "C" {
#include <json_config.h>
// Forward declare string parser present in json_parse.c but not in the header
JsonValue *parse_json_string(const char *json_str);
}

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <imp/imp_audio.h>
#include <imp/imp_isp.h>
#include <sstream>
#include <unistd.h>

namespace {

// Tiny JSON builder helpers (string-based)
inline void add_key(std::string &out, bool &sep, const char *k,
                    const char *open = "") {
  if (sep)
    out.push_back(',');
  else
    sep = true;
  out.push_back('"');
  out += k;
  out.push_back('"');
  out.push_back(':');
  out += open;
}
inline void add_str(std::string &out, const char *s) {
  out.push_back('"');
  if (s) {
    for (const char *p = s; *p; ++p) {
      unsigned char c = static_cast<unsigned char>(*p);
      switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
      }
    }
  }
  out.push_back('"');
}
inline void add_num(std::string &out, int v) {
  char b[32];
  std::snprintf(b, sizeof(b), "%d", v);
  out += b;
}
inline void add_bool(std::string &out, bool v) {
  out += (v ? "true" : "false");
}
inline void add_null(std::string &out) {
  out += "null";
}

// Look up a child by key (convenience)
JsonValue *obj_get(JsonValue *obj, const char *k) {
  return get_object_item(obj, k);
}

// Parse hex color string like #RRGGBBAA into ARGB uint
inline unsigned int hexColorToUint(const char *s) {
  if (!s || s[0] != '#')
    return 0;
  unsigned int r = 0, g = 0, b = 0, a = 255; // default opaque
  size_t n = std::strlen(s);
  if (n == 9) { // #RRGGBBAA
    unsigned int rr, gg, bb, aa;
    if (sscanf(s, "#%02x%02x%02x%02x", &rr, &gg, &bb, &aa) == 4) {
      r = rr;
      g = gg;
      b = bb;
      a = aa;
    }
  } else if (n == 7) { // #RRGGBB
    unsigned int rr, gg, bb;
    if (sscanf(s, "#%02x%02x%02x", &rr, &gg, &bb) == 3) {
      r = rr;
      g = gg;
      b = bb;
    }
  }
  return (a << 24) | (r << 16) | (g << 8) | b;
}

inline void add_hexstr(std::string &out, unsigned int argb) {
  char hexbuf[12];
  unsigned a = (argb >> 24) & 0xFF, r = (argb >> 16) & 0xFF,
           g = (argb >> 8) & 0xFF, b = (argb) & 0xFF;
  std::snprintf(hexbuf, sizeof(hexbuf), "#%02X%02X%02X%02X", r, g, b, a);
  add_str(out, hexbuf);
}
// Forward declaration for nested OSD helper used by handle_stream
void handle_osd(JsonValue *obj, int idx, std::string &sect, bool &s2,
                bool &wrote);

void handle_stream_stats(int idx, std::string &out) {
  int fps = 0;
  int bps = 0;
  if (idx == 0) {
    fps = cfg->stream0.stats.fps;
    bps = cfg->stream0.stats.bps;
  } else if (idx == 1) {
    fps = cfg->stream1.stats.fps;
    bps = cfg->stream1.stats.bps;
  } else {
    fps = cfg->stream2.stats.fps;
    bps = cfg->stream2.stats.bps;
  }
  out += "{";
  bool s = false;
  add_key(out, s, "fps");
  add_num(out, fps);
  add_key(out, s, "Bps");
  add_num(out, bps);
  out += "}";
}

void handle_stream(JsonValue *obj, int idx, std::string &out, bool &sep) {
  const char *root = idx == 0 ? "stream0" : (idx == 1 ? "stream1" : "stream2");
  bool wrote = false;
  std::string sect;
  sect.reserve(256);
  sect += "{";
  bool s2 = false;

  auto add_int = [&](const char *key, const std::string &path) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_NUMBER) {
        cfg->set<int>(path, (int)v->value.number.integer);
      }
      add_key(sect, s2, key);
      add_num(sect, cfg->get<int>(path));
      wrote = true;
    }
  };
  auto add_boolk = [&](const char *key, const std::string &path) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_BOOL) {
        cfg->set<bool>(path, v->value.boolean != 0);
      }
      add_key(sect, s2, key);
      add_bool(sect, cfg->get<bool>(path));
      wrote = true;
    }
  };
  auto add_strk = [&](const char *key, const std::string &path,
                      bool upper = false) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_STRING && v->value.string) {
        std::string val = v->value.string;
        if (upper)
          std::transform(val.begin(), val.end(), val.begin(), ::toupper);
        cfg->set<const char *>(path, strdup(val.c_str()));
        if (upper)
          global_restart_video = true;
      }
      add_key(sect, s2, key);
      add_str(sect, cfg->get<const char *>(path));
      wrote = true;
    }
  };

  // Scalars
  if (idx < 2) {
    add_boolk("enabled", std::string(root) + ".enabled");
  }
  if (idx < 2) {
    add_boolk("audio_enabled", std::string(root) + ".audio_enabled");
  }

  add_strk("rtsp_endpoint", std::string(root) + ".rtsp_endpoint");
  add_strk("rtsp_info", std::string(root) + ".rtsp_info");
  add_strk("format", std::string(root) + ".format");
  add_strk("mode", std::string(root) + ".mode", true);

  add_int("gop", std::string(root) + ".gop");
  add_int("max_gop", std::string(root) + ".max_gop");
  add_int("fps", std::string(root) + ".fps");
  add_int("buffers", std::string(root) + ".buffers");
  add_int("width", std::string(root) + ".width");
  add_int("height", std::string(root) + ".height");
  add_int("bitrate", std::string(root) + ".bitrate");
  add_int("rotation", std::string(root) + ".rotation");
  add_int("profile", std::string(root) + ".profile");

  // RC params
  add_int("qp_init", std::string(root) + ".qp_init");
  add_int("qp_min", std::string(root) + ".qp_min");
  add_int("qp_max", std::string(root) + ".qp_max");
  add_int("ip_delta", std::string(root) + ".ip_delta");
  add_int("pb_delta", std::string(root) + ".pb_delta");
  add_int("max_bitrate", std::string(root) + ".max_bitrate");

  // Actions / stats
  if (JsonValue *stats = obj_get(obj, "stats");
      stats && stats->type == JSON_NULL) {
    add_key(sect, s2, "stats");
    handle_stream_stats(idx, sect);
    wrote = true;
  }
  if (JsonValue *req_idr = obj_get(obj, "request_idr");
      req_idr && req_idr->type == JSON_NULL) {
    if (idx < 2 && global_video[idx]) {
      global_video[idx]->idr_fix = 1;
    }
    add_key(sect, s2, "request_idr");
    add_str(sect, "initiated");
    wrote = true;
  }
  // Nested OSD
  if (idx < 2) {
    if (JsonValue *osd = obj_get(obj, "osd"); osd && osd->type == JSON_OBJECT) {
      add_key(sect, s2, "osd", "{");
      bool s3 = false;
      bool wrote_osd = false;
      handle_osd(osd, idx, sect, s3, wrote_osd);
      sect += "}";
      wrote = wrote || wrote_osd;
    }
  }

  if (wrote) {
    add_key(out, sep, root, "");
    out += sect;
    out += "}";
  }
}

void handle_image(JsonValue *obj, std::string &out, bool &sep) {
  add_key(out, sep, "image", "{");
  bool s2 = false;
  bool wrote = false;
  auto add_int = [&](const char *key, const char *path, auto setter) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_NUMBER) {
        cfg->set<int>(path, (int)v->value.number.integer);
        if (setter)
          setter();
      }
      add_key(out, s2, key);
      add_num(out, cfg->get<int>(path));
      wrote = true;
    }
  };
  auto add_boolk = [&](const char *key, const char *path, auto setter_true,
                       auto setter_false) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_BOOL) {
        cfg->set<bool>(path, v->value.boolean != 0);
        if (v->value.boolean) {
          if (setter_true)
            setter_true();
        } else {
          if (setter_false)
            setter_false();
        }
      }
      add_key(out, s2, key);
      add_bool(out, cfg->get<bool>(path));
      wrote = true;
    }
  };

  // Scalars and side-effects
  add_int("brightness", "image.brightness",
          [] { hal::isp::set_brightness(cfg->image.brightness); });
  add_int("contrast", "image.contrast",
          [] { hal::isp::set_contrast(cfg->image.contrast); });
  if (hal::caps().has_isp_hue) {
    add_int("hue", "image.hue", [] { hal::isp::set_hue(cfg->image.hue); });
  }
  add_int("saturation", "image.saturation",
          [] { hal::isp::set_saturation(cfg->image.saturation); });
  add_int("sharpness", "image.sharpness",
          [] { hal::isp::set_sharpness(cfg->image.sharpness); });
  if (hal::caps().has_isp_sinter) {
    add_int("sinter_strength", "image.sinter_strength",
            [] { hal::isp::set_sinter_strength(cfg->image.sinter_strength); });
  }
  if (hal::caps().has_isp_temper) {
    add_int("temper_strength", "image.temper_strength",
            [] { hal::isp::set_temper_strength(cfg->image.temper_strength); });
  } else if (obj_get(obj, "temper_strength")) {
    add_key(out, s2, "temper_strength");
    add_null(out);
    wrote = true;
  }

  add_boolk(
      "vflip", "image.vflip", [] { hal::isp::set_vflip(true); },
      [] { hal::isp::set_vflip(false); });
  add_boolk(
      "hflip", "image.hflip", [] { hal::isp::set_hflip(true); },
      [] { hal::isp::set_hflip(false); });

  add_int("anti_flicker", "image.anti_flicker",
          [] { hal::isp::set_anti_flicker(cfg->image.anti_flicker); });

  add_boolk(
      "isp_bypass", "image.isp_bypass",
      [] { hal::isp::set_isp_bypass(true); },
      [] { hal::isp::set_isp_bypass(false); });

  if (JsonValue *rm = obj_get(obj, "running_mode")) {
    if (rm->type == JSON_NUMBER) {
      int mode = (int)rm->value.number.integer;
      cfg->set<int>("image.running_mode", mode);
      // Apply running_mode to the ISP for night/day switching
      // (monochrome/color, AE strategy, IRCUT, etc.).  Framerate is
      // not changed --- the sensor and encoder stay at the configured rate.
      hal::isp::set_running_mode(cfg->image.running_mode);
    }
    add_key(out, s2, "running_mode");
    add_num(out, cfg->get<int>("image.running_mode"));
    wrote = true;
  }
  if (hal::caps().has_isp_ae_comp) {
    add_int("ae_compensation", "image.ae_compensation",
            [] { hal::isp::set_ae_compensation(cfg->image.ae_compensation); });
  }
  if (hal::caps().has_isp_dpc) {
    add_int("dpc_strength", "image.dpc_strength",
            [] { hal::isp::set_dpc_strength(cfg->image.dpc_strength); });
  }
  if (hal::caps().has_isp_drc) {
    add_int("drc_strength", "image.drc_strength",
            [] { hal::isp::set_drc_strength(cfg->image.drc_strength); });
  }
  if (hal::caps().has_isp_defog) {
    add_int("defog_strength", "image.defog_strength", [] {
      hal::isp::set_defog_strength((uint8_t)cfg->image.defog_strength);
    });
  }
  if (hal::caps().has_isp_backlight_comp) {
    add_int("backlight_compensation", "image.backlight_compensation", [] {
      hal::isp::set_backlight_comp(cfg->image.backlight_compensation);
    });
  }
  add_int("highlight_depress", "image.highlight_depress", [] {
    hal::isp::set_highlight_depress(cfg->image.highlight_depress);
  });
  add_int("max_again", "image.max_again",
          [] { hal::isp::set_max_again(cfg->image.max_again); });
  add_int("max_dgain", "image.max_dgain",
          [] { hal::isp::set_max_dgain(cfg->image.max_dgain); });

  // WB bundle
  if (JsonValue *wbmode = obj_get(obj, "core_wb_mode");
      wbmode && wbmode->type == JSON_NUMBER) {
    cfg->set<int>("image.core_wb_mode", (int)wbmode->value.number.integer);
  }
  if (JsonValue *rg = obj_get(obj, "wb_rgain"); rg && rg->type == JSON_NUMBER) {
    cfg->set<int>("image.wb_rgain", (int)rg->value.number.integer);
  }
  if (JsonValue *bg = obj_get(obj, "wb_bgain"); bg && bg->type == JSON_NUMBER) {
    cfg->set<int>("image.wb_bgain", (int)bg->value.number.integer);
  }
  if (obj_get(obj, "core_wb_mode") || obj_get(obj, "wb_rgain") ||
      obj_get(obj, "wb_bgain")) {
    hal::isp::set_wb(cfg->image.core_wb_mode, cfg->image.wb_rgain,
                     cfg->image.wb_bgain);
    add_key(out, s2, "core_wb_mode");
    add_num(out, cfg->get<int>("image.core_wb_mode"));
    add_key(out, s2, "wb_rgain");
    add_num(out, cfg->get<int>("image.wb_rgain"));
    add_key(out, s2, "wb_bgain");
    add_num(out, cfg->get<int>("image.wb_bgain"));
    wrote = true;
  }

  if (!wrote) {
    out.erase(out.size() - 1);
    sep = (out.back() == ',');
    return;
  }
  out += "}";
}

void handle_osd(JsonValue *obj, int idx, std::string &sect, bool &s2,
                bool &wrote) {
  const char *root = "osd";

  // sei
  if (JsonValue *node = obj_get(obj, "sei");
      node && (node->type == JSON_OBJECT || node->type == JSON_NULL)) {
    const std::string base = std::string(root) + ".sei.";
    if (node->type == JSON_OBJECT) {
      if (JsonValue *en = obj_get(node, "enabled")) {
        if (en->type == JSON_BOOL)
          cfg->set<bool>(base + "enabled", en->value.boolean != 0);
      }
      // entries --- stored in JSON config file directly, not in struct
      if (JsonValue *elems = obj_get(node, "entries");
          elems && elems->type == JSON_OBJECT) {
        del_nested_item(cfg->jsonConfig, "osd.sei.entries");
        JsonValue *osd = get_nested_item(cfg->jsonConfig, "osd");
        if (osd && osd->type == JSON_OBJECT) {
          JsonValue *seiSec = get_nested_item(osd, "sei");
          if (!seiSec || seiSec->type != JSON_OBJECT) {
            seiSec = create_json_value(JSON_OBJECT);
            add_to_object(osd, "sei", seiSec);
          }
          JsonValue *cloned = clone_json_value(elems);
          if (cloned)
            add_to_object(seiSec, "entries", cloned);
        }
        cfg->mark_dirty();
        global_reload_osd = true;
      }
    }
    add_key(sect, s2, "sei", "{");
    bool sp = false;
    add_key(sect, sp, "enabled");
    add_bool(sect, cfg->get<bool>(base + "enabled"));
    add_key(sect, sp, "entries");
    JsonValue *cfgElems = get_nested_item(cfg->jsonConfig, "osd.sei.entries");
    if (cfgElems) {
      char *js = json_to_string(cfgElems, 0);
      sect += js ? js : "{}";
      free(js);
    } else {
      sect += "{}";
    }
    sect += "}";
    wrote = true;
  }

  // burnin
  if (JsonValue *node = obj_get(obj, "burnin");
      node && (node->type == JSON_OBJECT || node->type == JSON_NULL)) {
    const std::string base = std::string(root) + ".burnin.";
    if (node->type == JSON_OBJECT) {
      if (JsonValue *en = obj_get(node, "enabled")) {
        if (en->type == JSON_BOOL)
          cfg->set<bool>(base + "enabled", en->value.boolean != 0);
      }
      if (JsonValue *bg = obj_get(node, "background_color")) {
        if (bg->type == JSON_STRING)
          cfg->set<const char *>(base + "background_color", bg->value.string);
      }
      if (JsonValue *fmt = obj_get(node, "format")) {
        if (fmt->type == JSON_STRING)
          cfg->set<const char *>(base + "format", fmt->value.string);
      }
      if (JsonValue *sc = obj_get(node, "scale")) {
        if (sc->type == JSON_NUMBER)
          cfg->set<int>(base + "scale", (int)sc->value.number.integer);
      }
      if (JsonValue *fc = obj_get(node, "fill_color")) {
        if (fc->type == JSON_STRING)
          cfg->set<const char *>(base + "fill_color", fc->value.string);
      }
      if (JsonValue *oc = obj_get(node, "outline_color")) {
        if (oc->type == JSON_STRING)
          cfg->set<const char *>(base + "outline_color", oc->value.string);
      }
    }
    add_key(sect, s2, "burnin", "{");
    bool sp = false;
    add_key(sect, sp, "enabled");
    add_bool(sect, cfg->get<bool>(base + "enabled"));
    add_key(sect, sp, "background_color");
    add_str(sect, cfg->get<const char *>(base + "background_color"));
    add_key(sect, sp, "format");
    add_str(sect, cfg->get<const char *>(base + "format"));
    add_key(sect, sp, "scale");
    add_num(sect, cfg->get<int>(base + "scale"));
    add_key(sect, sp, "fill_color");
    add_str(sect, cfg->get<const char *>(base + "fill_color"));
    add_key(sect, sp, "outline_color");
    add_str(sect, cfg->get<const char *>(base + "outline_color"));
    sect += "}";
    wrote = true;
  }
}

void handle_audio(JsonValue *obj, std::string &out, bool &sep) {
  add_key(out, sep, "audio", "{");
  bool s2 = false;
  bool wrote = false;

  auto add_int = [&](const char *key, const char *path, bool restart_audio) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_NUMBER) {
        cfg->set<int>(path, (int)v->value.number.integer);
        if (restart_audio)
          global_restart_audio = true;
      }
      add_key(out, s2, key);
      add_num(out, cfg->get<int>(path));
      wrote = true;
    }
  };
  auto add_boolk_a = [&](const char *key, const char *path, bool rest_rtsp,
                         bool rest_audio) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_BOOL) {
        cfg->set<bool>(path, v->value.boolean != 0);
        if (rest_audio)
          global_restart_audio = true;
        if (rest_rtsp)
          global_restart_rtsp = true;
      }
      add_key(out, s2, key);
      add_bool(out, cfg->get<bool>(path));
      wrote = true;
    }
  };
  auto add_strk_a = [&](const char *key, const char *path, bool restart_audio) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_STRING && v->value.string) {
        cfg->set<const char *>(path, strdup(v->value.string));
        if (restart_audio)
          global_restart_audio = true;
      }
      add_key(out, s2, key);
      add_str(out, cfg->get<const char *>(path));
      wrote = true;
    }
  };

  // Input
  add_boolk_a("mic_enabled", "audio.mic_enabled", false, true);
  add_boolk_a("mic_hq", "audio.mic_hq", false, true);
  add_strk_a("mic_format", "audio.mic_format", true);

  // mic_vol - apply immediately without restart
  if (JsonValue *v = obj_get(obj, "mic_vol")) {
    if (v->type == JSON_NUMBER) {
      int vol = (int)v->value.number.integer;
      if (cfg->set<int>("audio.mic_vol", vol)) {
        // Apply to hardware immediately like WS.cpp does
        for (int i = 0; i < NUM_AUDIO_CHANNELS; i++) {
          if (global_audio[i]) {
            IMP_AI_SetVol(i, global_audio[i]->aiChn, vol);
          }
        }
      }
    }
    add_key(out, s2, "mic_vol");
    add_num(out, cfg->get<int>("audio.mic_vol"));
    wrote = true;
  }

  // mic_gain - apply immediately without restart
  if (JsonValue *v = obj_get(obj, "mic_gain")) {
    if (v->type == JSON_NUMBER) {
      int gain = (int)v->value.number.integer;
      if (cfg->set<int>("audio.mic_gain", gain)) {
        // Apply to hardware immediately like WS.cpp does
        for (int i = 0; i < NUM_AUDIO_CHANNELS; i++) {
          if (global_audio[i]) {
            IMP_AI_SetGain(i, global_audio[i]->aiChn, gain);
          }
        }
      }
    }
    add_key(out, s2, "mic_gain");
    add_num(out, cfg->get<int>("audio.mic_gain"));
    wrote = true;
  }
#if defined(LIB_AUDIO_PROCESSING)

  // mic_alc_gain - apply immediately without restart (platform-specific)
  if (hal::caps().has_audio_alc) {
    if (JsonValue *v = obj_get(obj, "mic_alc_gain")) {
      if (v->type == JSON_NUMBER) {
        int alc_gain = (int)v->value.number.integer;
        if (cfg->set<int>("audio.mic_alc_gain", alc_gain)) {
          // Apply to hardware immediately like WS.cpp does
          hal::audio::set_ai_alc(alc_gain);
        }
      }
      add_key(out, s2, "mic_alc_gain");
      add_num(out, cfg->get<int>("audio.mic_alc_gain"));
      wrote = true;
    }
  } else {
    add_int("mic_alc_gain", "audio.mic_alc_gain", false);
  }

  add_int("mic_noise_suppression", "audio.mic_noise_suppression", true);

  // mic_high_pass_filter - apply immediately without restart
  if (JsonValue *v = obj_get(obj, "mic_high_pass_filter")) {
    if (v->type == JSON_BOOL) {
      bool enable = v->value.boolean != 0;
      if (cfg->set<bool>("audio.mic_high_pass_filter", enable)) {
        // Apply to hardware immediately like WS.cpp does
        for (int i = 0; i < NUM_AUDIO_CHANNELS; i++) {
          if (global_audio[i]) {
            IMPAudioIOAttr ioattr;
            int ret = IMP_AI_GetPubAttr(i, &ioattr);
            if (ret == 0) {
              if (enable) {
                IMP_AI_EnableHpf(&ioattr);
              } else {
                IMP_AI_DisableHpf();
              }
            }
          }
        }
      }
    }
    add_key(out, s2, "mic_high_pass_filter");
    add_bool(out, cfg->get<bool>("audio.mic_high_pass_filter"));
    wrote = true;
  }

  add_boolk_a("mic_agc_enabled", "audio.mic_agc_enabled", false, true);
  add_int("mic_agc_target_level_dbfs", "audio.mic_agc_target_level_dbfs", true);
  add_int("mic_agc_compression_gain_db", "audio.mic_agc_compression_gain_db",
          true);
  add_boolk_a("force_stereo", "audio.force_stereo", false, true);
  // Output
  add_boolk_a("spk_enabled", "audio.spk_enabled", true, true);

  // spk_vol - apply immediately without restart
  if (JsonValue *v = obj_get(obj, "spk_vol")) {
    if (v->type == JSON_NUMBER) {
      int vol = (int)v->value.number.integer;
      if (cfg->set<int>("audio.spk_vol", vol)) {
        // Apply to hardware immediately via AudioOutputWorker
        AudioOutputWorker::applyVolumeGain(true, vol, false, 0);
      }
    }
    add_key(out, s2, "spk_vol");
    add_num(out, cfg->get<int>("audio.spk_vol"));
    wrote = true;
  }

  // spk_gain - apply immediately without restart
  if (JsonValue *v = obj_get(obj, "spk_gain")) {
    if (v->type == JSON_NUMBER) {
      int gain = (int)v->value.number.integer;
      if (cfg->set<int>("audio.spk_gain", gain)) {
        // Apply to hardware immediately via AudioOutputWorker
        AudioOutputWorker::applyVolumeGain(false, 0, true, gain);
      }
    }
    add_key(out, s2, "spk_gain");
    add_num(out, cfg->get<int>("audio.spk_gain"));
    wrote = true;
  }
#endif

  if (!wrote) {
    out.erase(out.size() - 1);
    return;
  }
  out += "}";
}

void handle_motion(JsonValue *obj, std::string &out, bool &sep) {
  add_key(out, sep, "motion", "{");
  bool s2 = false;
  bool wrote = false;
  auto add_int = [&](const char *key, const char *path) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_NUMBER)
        cfg->set<int>(path, (int)v->value.number.integer);
      add_key(out, s2, key);
      add_num(out, cfg->get<int>(path));
      wrote = true;
    }
  };
  auto add_boolk_m = [&](const char *key, const char *path,
                         bool restart_video = false) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_BOOL) {
        cfg->set<bool>(path, v->value.boolean != 0);
        if (restart_video)
          global_restart_video = true;
      }
      add_key(out, s2, key);
      add_bool(out, cfg->get<bool>(path));
      wrote = true;
    }
  };
  auto add_strs = [&](const char *key, const char *path) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_STRING && v->value.string)
        cfg->set<const char *>(path, strdup(v->value.string));
      add_key(out, s2, key);
      add_str(out, cfg->get<const char *>(path));
      wrote = true;
    }
  };

  add_int("monitor_stream", "motion.monitor_stream");
  add_int("debounce_time_s", "motion.debounce_time_s");
  add_int("post_time_s", "motion.post_time_s");
  add_int("cooldown_time_s", "motion.cooldown_time_s");
  add_int("motor_settle_ms", "motion.motor_settle_ms");
  add_int("init_time_s", "motion.init_time_s");
  add_int("min_time_s", "motion.min_time_s");
  add_int("ivs_polling_timeout_ms", "motion.ivs_polling_timeout_ms");
  add_int("sensitivity", "motion.sensitivity");
  add_int("skip_frame_count", "motion.skip_frame_count");
  add_int("frame_width", "motion.frame_width");
  add_int("frame_height", "motion.frame_height");
  add_int("roi_0_x", "motion.roi_0_x");
  add_int("roi_0_y", "motion.roi_0_y");
  add_int("roi_1_x", "motion.roi_1_x");
  add_int("roi_1_y", "motion.roi_1_y");
  add_int("roi_count", "motion.roi_count");
  add_boolk_m("enabled", "motion.enabled", /*restart_video*/ true);
  add_strs("script_path", "motion.script_path");

  if (JsonValue *rois = obj_get(obj, "rois")) {
    if (rois->type == JSON_NULL) {
      add_key(out, s2, "rois", "[");
      bool sA = false;
      int cnt = std::min(cfg->motion.roi_count, (int)cfg->motion.rois.size());
      for (int i = 0; i < cnt; i++) {
        if (sA)
          out += ",";
        else
          sA = true;
        const auto &r = cfg->motion.rois[i];
        out += "[";
        add_num(out, r.p0_x);
        out += ",";
        add_num(out, r.p0_y);
        out += ",";
        add_num(out, r.p1_x);
        out += ",";
        add_num(out, r.p1_y);
        out += "]";
      }
      out += "]";
      wrote = true;
    } else if (rois->type == JSON_ARRAY) {
      // Expect array of [x0,y0,x1,y1]
      int i = 0;
      for (JsonArrayItem *it = rois->value.array_head;
           it && i < (int)cfg->motion.rois.size(); it = it->next, ++i) {
        JsonValue *sub = it->value;
        if (sub && sub->type == JSON_ARRAY) {
          int vals[4] = {0};
          int j = 0;
          for (JsonArrayItem *jt = sub->value.array_head; jt && j < 4;
               jt = jt->next, ++j) {
            JsonValue *jv = jt->value;
            if (jv && jv->type == JSON_NUMBER)
              vals[j] = (int)jv->value.number.integer;
          }
          cfg->motion.rois[i].p0_x = vals[0];
          cfg->motion.rois[i].p0_y = vals[1];
          cfg->motion.rois[i].p1_x = vals[2];
          cfg->motion.rois[i].p1_y = vals[3];
        }
      }
      cfg->motion.roi_count = i;
      add_key(out, s2, "rois");
      out += "\"ok\"";
      wrote = true;
    }
  }

  if (!wrote) {
    out.erase(out.size() - 1);
    return;
  }
  out += "}";
}

void handle_privacy(JsonValue *obj, std::string &out, bool &sep) {
  add_key(out, sep, "privacy", "{");
  bool s2 = false;
  bool wrote = false;

  // Read enabled state from request
  if (JsonValue *v = obj_get(obj, "enabled")) {
    if (v->type == JSON_BOOL) {
      bool enabled = v->value.boolean != 0;
      // Apply privacy to all channels via FIFO
      const char *fifo_path = "/run/prudynt/video_ctrl";
      int fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
      if (fd >= 0) {
        const char *cmd = enabled ? "PRIVACY channel=all value=1\n"
                                  : "PRIVACY channel=all value=0\n";
        write(fd, cmd, strlen(cmd));
        close(fd);
      }
      add_key(out, s2, "enabled");
      add_bool(out, enabled);
      wrote = true;
    } else if (v->type == JSON_NULL) {
      // Just query the state
      bool any_privacy = false;
      for (int i = 0; i < NUM_VIDEO_CHANNELS; i++) {
        if (global_video[i] && global_video[i]->privacy_requested.load(
                                   std::memory_order_relaxed)) {
          any_privacy = true;
          break;
        }
      }
      add_key(out, s2, "enabled");
      add_bool(out, any_privacy);
      wrote = true;
    }
  }

  // save_state: persist runtime toggles to config
  if (JsonValue *v = obj_get(obj, "save_state")) {
    if (v->type == JSON_BOOL) {
      cfg->privacy.save_state = v->value.boolean != 0;
      add_key(out, s2, "save_state");
      add_bool(out, cfg->privacy.save_state);
      wrote = true;
    } else if (v->type == JSON_NULL) {
      add_key(out, s2, "save_state");
      add_bool(out, cfg->privacy.save_state);
      wrote = true;
    }
  }

  if (!wrote) {
    out.erase(out.size() - 1);
    return;
  }
  out += "}";
}

void handle_daynight(JsonValue *obj, std::string &out, bool &sep) {
  const size_t section_start = out.size();
  const bool prev_sep = sep;
  add_key(out, sep, "daynight", "{");
  bool s2 = false;
  bool wrote = false;
  auto add_int = [&](const char *key, const char *path) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_NUMBER)
        cfg->set<int>(path, (int)v->value.number.integer);
      add_key(out, s2, key);
      add_num(out, cfg->get<int>(path));
      wrote = true;
    }
  };
  auto add_boolk = [&](const char *key, const char *path) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_BOOL)
        cfg->set<bool>(path, v->value.boolean != 0);
      add_key(out, s2, key);
      add_bool(out, cfg->get<bool>(path));
      wrote = true;
    }
  };
  auto add_strk = [&](const char *key, const char *path, bool upper = false) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_STRING && v->value.string) {
        std::string val = v->value.string;
        if (upper) {
          std::transform(val.begin(), val.end(), val.begin(), ::toupper);
        }
        cfg->set<const char *>(path, strdup(val.c_str()));
      }
      add_key(out, s2, key);
      add_str(out, cfg->get<const char *>(path));
      wrote = true;
    }
  };

  // Basic settings
  add_boolk("enabled", "daynight.enabled");
  add_strk("loglevel", "daynight.loglevel", true);
  add_strk("script_path", "daynight.script_path");

  // Controls (hardware toggles)
  if (JsonValue *controls_obj = obj_get(obj, "controls")) {
    if (controls_obj->type == JSON_OBJECT) {
      add_key(out, s2, "controls", "{");
      bool s3 = false;
      auto add_ctrl = [&](const char *key, const char *path) {
        if (JsonValue *v = obj_get(controls_obj, key)) {
          if (v->type == JSON_BOOL) {
            cfg->set<bool>(path, v->value.boolean);
          }
          add_key(out, s3, key);
          add_bool(out, cfg->get<bool>(path));
          wrote = true;
        }
      };

      add_ctrl("color", "daynight.controls.color");
      add_ctrl("ircut", "daynight.controls.ircut");
      add_ctrl("ir850", "daynight.controls.ir850");
      add_ctrl("ir940", "daynight.controls.ir940");
      add_ctrl("white", "daynight.controls.white");
      out += "}";
    }
  } else {
    // Read-only output of current controls
    add_key(out, s2, "controls", "{");
    bool s3 = false;

    add_key(out, s3, "color");
    add_bool(out, cfg->daynight.controls.color);
    add_key(out, s3, "ircut");
    add_bool(out, cfg->daynight.controls.ircut);
    add_key(out, s3, "ir850");
    add_bool(out, cfg->daynight.controls.ir850);
    add_key(out, s3, "ir940");
    add_bool(out, cfg->daynight.controls.ir940);
    add_key(out, s3, "white");
    add_bool(out, cfg->daynight.controls.white);
    out += "}";
  }

  // Schedule (time-based restrictions)
  if (JsonValue *schedule_obj = obj_get(obj, "schedule")) {
    if (schedule_obj->type == JSON_OBJECT) {
      add_key(out, s2, "schedule", "{");
      bool s3 = false;
      auto add_sched = [&](const char *key, const char *path, bool is_bool) {
        if (JsonValue *v = obj_get(schedule_obj, key)) {
          if (is_bool && v->type == JSON_BOOL) {
            cfg->set<bool>(path, v->value.boolean);
            add_key(out, s3, key);
            add_bool(out, cfg->get<bool>(path));
            wrote = true;
          } else if (!is_bool && v->type == JSON_STRING) {
            cfg->set<const char *>(path, strdup(v->value.string));
            add_key(out, s3, key);
            add_str(out, cfg->get<const char *>(path));
            wrote = true;
          } else if (v->type == JSON_NULL) {
            // Read current value when null is provided
            add_key(out, s3, key);
            if (is_bool) {
              add_bool(out, cfg->get<bool>(path));
            } else {
              add_str(out, cfg->get<const char *>(path));
            }
            wrote = true;
          }
        }
      };
      add_sched("enabled", "daynight.schedule.enabled", true);
      add_sched("start_at", "daynight.schedule.start_at", false);
      add_sched("stop_at", "daynight.schedule.stop_at", false);
      out += "}";
    }
  } else {
    // Read-only output of current schedule
    add_key(out, s2, "schedule", "{");
    bool s3 = false;
    add_key(out, s3, "enabled");
    add_bool(out, cfg->daynight.schedule.enabled);
    add_key(out, s3, "start_at");
    add_str(out, cfg->daynight.schedule.start_at);
    add_key(out, s3, "stop_at");
    add_str(out, cfg->daynight.schedule.stop_at);
    out += "}";
  }

  // Percentage thresholds (simple algorithm)

  // Total gain thresholds (simple algorithm)
  add_int("total_gain_night_threshold", "daynight.total_gain_night_threshold");
  add_int("total_gain_day_threshold", "daynight.total_gain_day_threshold");

  // Algorithm parameters
  add_int("sample_interval_ms", "daynight.sample_interval_ms");

  // Expert/advanced parameters (legacy algorithm)

  // Manual mode override
  if (JsonValue *v = obj_get(obj, "force_mode")) {
    if (v->type == JSON_STRING && v->value.string) {
      const char *mode = v->value.string;
      if (std::strcmp(mode, "day") == 0 || std::strcmp(mode, "night") == 0) {
        cfg->daynight.force_mode.store(strdup(mode));
      }
      add_key(out, s2, "force_mode");
      add_str(out, mode);
      wrote = true;
    }
  }

  // Manual bin switching
  if (JsonValue *v = obj_get(obj, "switch_bin")) {
    if (v->type == JSON_STRING && v->value.string) {
      const char *bin_path = v->value.string;
      int ret = hal::isp::switch_bin(bin_path);
      add_key(out, s2, "switch_bin");
      if (ret == 0) {
        add_str(out, "success");
      } else {
        add_str(out, "failed");
      }
      wrote = true;
    }
  }

  // -- Live sensor telemetry --------------------------------------
  // Photosensing is delegated to daynightd.
  // Individual sensor queries relay data from /run/thingino/daynight_sensors.
  // The "status" and "history" keys source from daynightd's JSON files.

  auto relay_sensor_file = [&](const char *key, const char *path) -> bool {
    if (!obj_get(obj, key))
      return false;
    FILE *fp = fopen(path, "r");
    if (!fp) {
      add_key(out, s2, key);
      add_str(out, "unavailable");
      return true;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    if (sz <= 0 || sz > 65536) {
      fclose(fp);
      add_key(out, s2, key);
      add_str(out, "unavailable");
      return true;
    }
    std::string buf(sz, '\0');
    size_t n = fread(&buf[0], 1, sz, fp);
    fclose(fp);
    if (n <= 0) {
      add_key(out, s2, key);
      add_str(out, "unavailable");
      return true;
    }
    buf.resize(n);
    /* Trim trailing whitespace/newlines */
    while (!buf.empty() && (buf.back() == '\n' || buf.back() == ' '))
      buf.pop_back();
    add_key(out, s2, key);
    out += buf; /* raw JSON embed */
    return true;
  };

  if (relay_sensor_file("status", "/run/thingino/daynight_sensors"))
    wrote = true;

  if (relay_sensor_file("history", "/run/thingino/daynight_history"))
    wrote = true;

  /* Individual field queries relayed from sensor file (cached parse) */
  auto relay_sensor_field = [&](const char *key, const char *json_key) {
    if (!obj_get(obj, key))
      return;
    /* Read mode from the simple mode file (fast path) */
    if (strcmp(key, "mode") == 0 || strcmp(key, "brightness_percent") == 0) {
      const char *fpath = (strcmp(key, "mode") == 0)
                              ? "/run/thingino/daynight_mode"
                              : "/run/thingino/daynight_brightness";
      FILE *fp = fopen(fpath, "r");
      if (fp) {
        char buf[32] = {};
        if (fgets(buf, sizeof(buf), fp)) {
          size_t l = strlen(buf);
          while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == ' '))
            buf[--l] = '\0';
          add_key(out, s2, key);
          if (strcmp(key, "mode") == 0)
            add_str(out, buf);
          else
            add_num(out, atoi(buf));
          wrote = true;
        }
        fclose(fp);
      }
    }
  };

  relay_sensor_field("mode", "mode");
  relay_sensor_field("brightness_percent", "brightness_percent");
  relay_sensor_field("ev", "ev");
  relay_sensor_field("total_gain", "total_gain");
  relay_sensor_field("gb", "gb");
  relay_sensor_field("gr", "gr");
  relay_sensor_field("ae_luma", "ae_luma");
  relay_sensor_field("awb_color_temp", "awb_color_temp");

  if (!wrote) {
    out.resize(section_start);
    sep = prev_sep;
    return;
  }
  out += "}";
}

void handle_info(JsonValue *obj, std::string &out, bool &sep) {
  add_key(out, sep, "info", "{");
  bool s2 = false;
  bool wrote = false;
  // Currently only imp_system_version (read)
  if (JsonValue *v = obj_get(obj, "imp_system_version");
      v && v->type == JSON_NULL) {
    add_key(out, s2, "imp_system_version");
    add_str(out, "unknown");
    wrote = true;
  }
  if (!wrote) {
    out.erase(out.size() - 1);
    return;
  }
  out += "}";
}

void handle_action(JsonValue *obj, std::string &out, bool &sep) {
  add_key(out, sep, "action", "{");
  bool s2 = false;
  bool wrote = false;
  if (JsonValue *rst = obj_get(obj, "restart_thread");
      rst && rst->type == JSON_NUMBER) {
    int mask = (int)rst->value.number.integer;
    if (mask & 1)
      global_restart_rtsp = true;
    if (mask & 2)
      global_restart_video = true;
    if (mask & 4)
      global_restart_audio = true;
    add_key(out, s2, "restart_thread");
    add_str(out, "ok");
    wrote = true;
  }
  if (JsonValue *sv = obj_get(obj, "save_config");
      sv && sv->type == JSON_NULL) {
    cfg->updateConfig();
    add_key(out, s2, "save_config");
    add_str(out, "ok");
    wrote = true;
  }
  if (JsonValue *cap = obj_get(obj, "capture"); cap) {
    // Stub hook for capture; WebUI should use /cgi-bin/chX.jpg
    add_key(out, s2, "capture");
    add_str(out, "use_snapshot_api");
    wrote = true;
  }
  if (!wrote) {
    out.erase(out.size() - 1);
    return;
  }
  out += "}";
}

void handle_rtsp(JsonValue *obj, std::string &out, bool &sep) {
  add_key(out, sep, "rtsp", "{");
  bool s2 = false;
  auto wrote = false;
  auto add_int = [&](const char *key, const char *path) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_NUMBER) {
        cfg->set<int>(path, (int)v->value.number.integer);
        global_restart_rtsp = true;
      }
      add_key(out, s2, key);
      add_num(out, cfg->get<int>(path));
      wrote = true;
    }
  };
  auto add_str_r = [&](const char *key, const char *path) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_STRING && v->value.string) {
        cfg->set<const char *>(path, strdup(v->value.string));
        global_restart_rtsp = true;
      }
      add_key(out, s2, key);
      add_str(out, cfg->get<const char *>(path));
      wrote = true;
    }
  };
  auto add_bool_r = [&](const char *key, const char *path) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_BOOL) {
        cfg->set<bool>(path, v->value.boolean != 0);
        global_restart_rtsp = true;
      }
      add_key(out, s2, key);
      add_bool(out, cfg->get<bool>(path));
      wrote = true;
    }
  };
  add_int("port", "rtsp.port");
  add_int("est_bitrate", "rtsp.est_bitrate");
  add_int("out_buffer_size", "rtsp.out_buffer_size");
  add_int("send_buffer_size", "rtsp.send_buffer_size");
  add_int("send_timeout", "rtsp.send_timeout");
  add_bool_r("auth_required", "rtsp.auth_required");
  add_str_r("name", "rtsp.name");
  add_str_r("username", "rtsp.username");
  add_str_r("password", "rtsp.password");
  if (!wrote) {
    out.erase(out.size() - 1);
    return;
  }
  out += "}";
}

void handle_sensor(JsonValue *obj, std::string &out, bool &sep) {
  add_key(out, sep, "sensor", "{");
  bool s2 = false;
  bool wrote = false;
  auto add_int = [&](const char *key, const char *path) {
    if (obj_get(obj, key)) {
      add_key(out, s2, key);
      add_num(out, cfg->get<int>(path));
      wrote = true;
    }
  };
  auto add_str_s = [&](const char *key, const char *path) {
    if (obj_get(obj, key)) {
      add_key(out, s2, key);
      add_str(out, cfg->get<const char *>(path));
      wrote = true;
    }
  };
  auto add_uint = [&](const char *key, const char *path) {
    if (obj_get(obj, key)) {
      add_key(out, s2, key);
      char b[16];
      std::snprintf(b, sizeof(b), "%#x", cfg->get<unsigned int>(path));
      out += '"';
      out += b;
      out += '"';
      wrote = true;
    }
  };
  add_str_s("model", "sensor.model");
  add_int("fps", "sensor.fps");
  add_int("width", "sensor.width");
  add_int("height", "sensor.height");
  add_uint("i2c_address", "sensor.i2c_address");
  if (!wrote) {
    out.erase(out.size() - 1);
    return;
  }
  out += "}";
}

void handle_stream2(JsonValue *obj, std::string &out, bool &sep) {
  add_key(out, sep, "stream2", "{");
  bool s2 = false;
  bool wrote = false;
  auto add_bool_s2 = [&](const char *key, const char *path) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_BOOL)
        cfg->set<bool>(path, v->value.boolean != 0);
      add_key(out, s2, key);
      add_bool(out, cfg->get<bool>(path));
      wrote = true;
    }
  };
  auto add_int = [&](const char *key, const char *path) {
    if (JsonValue *v = obj_get(obj, key)) {
      if (v->type == JSON_NUMBER)
        cfg->set<int>(path, (int)v->value.number.integer);
      add_key(out, s2, key);
      add_num(out, cfg->get<int>(path));
      wrote = true;
    }
  };
  if (JsonValue *stats = obj_get(obj, "stats");
      stats && stats->type == JSON_NULL) {
    add_key(out, s2, "stats");
    handle_stream_stats(2, out);
    wrote = true;
  }
  add_bool_s2("jpeg_enabled", "stream2.enabled");
  add_int("jpeg_quality", "stream2.jpeg_quality");
  add_int("jpeg_channel", "stream2.jpeg_channel");
  add_int("fps", "stream2.fps");
  if (!wrote) {
    out.erase(out.size() - 1);
    return;
  }
  out += "}";
}

void handle_general(JsonValue *obj, std::string &out, bool &sep) {
  JsonValue *ll = obj_get(obj, "loglevel");
  if (ll) {
    add_key(out, sep, "general", "{");
    bool s2 = false;
    if (ll->type == JSON_STRING) {
      cfg->set<const char *>(std::string("general.loglevel"),
                             strdup(ll->value.string));
      add_key(out, s2, "loglevel");
      add_str(out, cfg->general.loglevel);
    } else if (ll->type == JSON_NULL) {
      add_key(out, s2, "loglevel");
      add_str(out, cfg->general.loglevel);
    }
    out += "}";
  }
}

void handle_mp4(JsonValue *obj, std::string &out, bool &sep) {
  add_key(out, sep, "mp4", "{");
  bool s2 = false;
  bool wrote = false;

  // Handle start command
  if (JsonValue *start = obj_get(obj, "start")) {
    if (start->type == JSON_OBJECT) {
      int channel = 0;
      JsonValue *ch = obj_get(start, "channel");
      if (ch && ch->type == JSON_NUMBER) {
        channel = (int)ch->value.number.integer;
      }

      if (channel >= 0 && channel < NUM_VIDEO_CHANNELS) {
        // Build the FIFO command to start recording
        std::string fifo_cmd = "START ch=" + std::to_string(channel);

        // Send mount and directory with hostname expansion
        if (cfg && cfg->recorder.mount && cfg->recorder.mount[0]) {
          fifo_cmd += " mount=" + std::string(cfg->recorder.mount);
        }

        if (cfg && cfg->recorder.device_path && cfg->recorder.device_path[0]) {
          std::string device_path = cfg->recorder.device_path;
          // Expand %hostname variable
          size_t pos = device_path.find("%hostname");
          if (pos != std::string::npos) {
            char hostname[256] = {0};
            gethostname(hostname, sizeof(hostname) - 1);
            device_path.replace(pos, 9, hostname);
          }
          fifo_cmd += " dir=" + device_path;
        }

        if (cfg && cfg->recorder.filename && cfg->recorder.filename[0]) {
          fifo_cmd += " template=" + std::string(cfg->recorder.filename);
        }

        // Enable loop mode for continuous recording until manual stop
        fifo_cmd += " loop=1";

        // Use duration from config or JSON for segment length
        JsonValue *dur = obj_get(start, "duration");
        if (dur && dur->type == JSON_NUMBER) {
          fifo_cmd += " dur=" + std::to_string((int)dur->value.number.integer);
        } else if (cfg && cfg->recorder.duration_s > 0) {
          fifo_cmd += " dur=" + std::to_string(cfg->recorder.duration_s);
        }

        // Write to mp4ctl FIFO
        int fd = open("/run/prudynt/mp4ctl", O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
          fifo_cmd += "\n";
          ssize_t written = write(fd, fifo_cmd.c_str(), fifo_cmd.length());
          close(fd);

          add_key(out, s2, "start");
          if (written > 0) {
            add_str(out, "ok");
          } else {
            add_str(out, "error");
          }
        } else {
          add_key(out, s2, "start");
          add_str(out, "fifo_unavailable");
        }
        wrote = true;
      } else {
        add_key(out, s2, "start");
        add_str(out, "invalid_channel");
        wrote = true;
      }
    }
  }

  // Handle stop command
  if (JsonValue *stop = obj_get(obj, "stop")) {
    if (stop->type == JSON_OBJECT) {
      int channel = -1;
      JsonValue *ch = obj_get(stop, "channel");
      if (ch && ch->type == JSON_NUMBER) {
        channel = (int)ch->value.number.integer;
      }

      if (channel >= 0 && channel < NUM_VIDEO_CHANNELS) {
        // Stop loop recording for specific channel
        std::string fifo_cmd = "STOP LOOP ch=" + std::to_string(channel) + "\n";
        int fd = open("/run/prudynt/mp4ctl", O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
          ssize_t written = write(fd, fifo_cmd.c_str(), fifo_cmd.length());
          close(fd);

          add_key(out, s2, "stop");
          if (written > 0) {
            add_str(out, "ok");
          } else {
            add_str(out, "error");
          }
        } else {
          add_key(out, s2, "stop");
          add_str(out, "fifo_unavailable");
        }
        wrote = true;
      } else if (channel == -1) {
        // Stop all channels
        std::string fifo_cmd = "STOP\n";
        int fd = open("/run/prudynt/mp4ctl", O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
          ssize_t written = write(fd, fifo_cmd.c_str(), fifo_cmd.length());
          close(fd);

          add_key(out, s2, "stop");
          if (written > 0) {
            add_str(out, "ok");
          } else {
            add_str(out, "error");
          }
        } else {
          add_key(out, s2, "stop");
          add_str(out, "fifo_unavailable");
        }
        wrote = true;
      } else {
        add_key(out, s2, "stop");
        add_str(out, "invalid_channel");
        wrote = true;
      }
    } else if (stop->type == JSON_NULL) {
      // Stop all channels
      std::string fifo_cmd = "STOP\n";
      int fd = open("/run/prudynt/mp4ctl", O_WRONLY | O_NONBLOCK);
      if (fd >= 0) {
        ssize_t written = write(fd, fifo_cmd.c_str(), fifo_cmd.length());
        close(fd);

        add_key(out, s2, "stop");
        if (written > 0) {
          add_str(out, "ok");
        } else {
          add_str(out, "error");
        }
      } else {
        add_key(out, s2, "stop");
        add_str(out, "fifo_unavailable");
      }
      wrote = true;
    }
  }

  // Handle status query
  if (JsonValue *status = obj_get(obj, "status")) {
    if (status->type == JSON_NULL || status->type == JSON_OBJECT) {
      add_key(out, s2, "status", "{");
      bool s3 = false;

      for (int ch = 0; ch < NUM_VIDEO_CHANNELS; ch++) {
        add_key(out, s3, ("ch" + std::to_string(ch)).c_str());
        add_bool(out, global_mp4_recorders[ch].isActive());
      }

      out += "}";
      wrote = true;
    }
  }

  if (!wrote) {
    out.erase(out.size() - 1);
    return;
  }
  out += "}";
}
} // namespace

namespace JsonAPI {

bool process_json(const std::string &in, std::string &out) {
  cfg->reset_dirty();
  JsonValue *root = parse_json_string(in.c_str());
  if (!root || root->type != JSON_OBJECT) {
    if (root)
      free_json_value(root);
    out = "{}";
    return false;
  }

  // Special case: dump_config returns full config directly, not wrapped
  if (JsonValue *action_obj = obj_get(root, "action");
      action_obj && action_obj->type == JSON_OBJECT) {
    if (JsonValue *dump_val = obj_get(action_obj, "dump_config");
        dump_val && dump_val->type == JSON_NULL) {
      char *json_str = json_to_string(cfg->jsonConfig, 0); // 0 = compact
      if (json_str) {
        out = json_str;
        free(json_str);
        free_json_value(root);
        return true;
      }
    }
  }

  out = "{";
  bool sep = false;

  // Track restart flags before processing to detect changes
  bool prev_restart_rtsp = global_restart_rtsp;
  bool prev_restart_video = global_restart_video;
  bool prev_restart_audio = global_restart_audio;

  // Iterate top-level keys
  for (JsonKeyValue *kv = root->value.object_head; kv; kv = kv->next) {
    const char *k = kv->key;
    JsonValue *v = kv->value;
    if (!strcmp(k, "stream0") && v && v->type == JSON_OBJECT) {
      handle_stream(v, 0, out, sep);
    } else if (!strcmp(k, "stream1") && v && v->type == JSON_OBJECT) {
      handle_stream(v, 1, out, sep);
    } else if (!strcmp(k, "stream2") && v && v->type == JSON_OBJECT) {
      handle_stream(v, 2, out, sep);
    } else if (!strcmp(k, "image") && v && v->type == JSON_OBJECT) {
      handle_image(v, out, sep);
    } else if (!strcmp(k, "osd") && v && v->type == JSON_OBJECT) {
      bool dummy_wrote = false;
      handle_osd(v, 0, out, sep, dummy_wrote);
    } else if (!strcmp(k, "general") && v && v->type == JSON_OBJECT) {
      handle_general(v, out, sep);
    } else if (!strcmp(k, "rtsp") && v && v->type == JSON_OBJECT) {
      handle_rtsp(v, out, sep);
    } else if (!strcmp(k, "sensor") && v && v->type == JSON_OBJECT) {
      handle_sensor(v, out, sep);
    } else if (!strcmp(k, "stream2") && v && v->type == JSON_OBJECT) {
      handle_stream2(v, out, sep);
    } else if (!strcmp(k, "audio") && v && v->type == JSON_OBJECT) {
      handle_audio(v, out, sep);
    } else if (!strcmp(k, "motion") && v && v->type == JSON_OBJECT) {
      handle_motion(v, out, sep);
    } else if (!strcmp(k, "privacy") && v && v->type == JSON_OBJECT) {
      handle_privacy(v, out, sep);
    } else if (!strcmp(k, "info") && v && v->type == JSON_OBJECT) {
      handle_info(v, out, sep);
    } else if (!strcmp(k, "daynight") && v && v->type == JSON_OBJECT) {
      handle_daynight(v, out, sep);
    } else if (!strcmp(k, "action") && v && v->type == JSON_OBJECT) {
      handle_action(v, out, sep);
    } else if (!strcmp(k, "mp4") && v && v->type == JSON_OBJECT) {
      handle_mp4(v, out, sep);
    }
  }

  out += "}";
  free_json_value(root);

  // Notify main thread if any restart flags were set
  if (global_restart_rtsp != prev_restart_rtsp ||
      global_restart_video != prev_restart_video ||
      global_restart_audio != prev_restart_audio) {
    global_cv_worker_restart.notify_one();
  }

  return true;
}

} // namespace JsonAPI
