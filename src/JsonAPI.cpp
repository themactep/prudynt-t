#include "JsonAPI.hpp"
#include "Config.hpp"
#include "globals.hpp"

extern "C" {
#include <json_config.h>
// Forward declare string parser present in json_parse.c but not in the header
JsonValue *parse_json_string(const char *json_str);
}

#include <imp/imp_isp.h>
#include <sstream>
#include <cstring>
#include <algorithm>
#if defined(AUDIO_SUPPORT)
#include <imp/imp_audio.h>
#endif


namespace {

    // Tiny JSON builder helpers (string-based)
    inline void add_key(std::string &out, bool &sep, const char *k, const char *open=""){
        if (sep) out.push_back(','); else sep=true;
        out.push_back('"'); out += k; out.push_back('"'); out.push_back(':'); out += open;
    }
    inline void add_str(std::string &out, const char *s){ out.push_back('"'); out += s ? s : ""; out.push_back('"'); }
    inline void add_num(std::string &out, int v){ char b[32]; std::snprintf(b,sizeof(b),"%d",v); out += b; }
    inline void add_bool(std::string &out, bool v){ out += (v?"true":"false"); }

    // Look up a child by key (convenience)
    JsonValue* obj_get(JsonValue *obj, const char *k){ return get_object_item(obj, k); }

    // Parse hex color string like #RRGGBBAA into ARGB uint
    inline unsigned int hexColorToUint(const char *s){
        if (!s || s[0] != '#') return 0;
        unsigned int r=0,g=0,b=0,a=255; // default opaque
        size_t n = std::strlen(s);
        if (n == 9) { // #RRGGBBAA
            unsigned int rr,gg,bb,aa;
            if (sscanf(s, "#%02x%02x%02x%02x", &rr,&gg,&bb,&aa) == 4){ r=rr; g=gg; b=bb; a=aa; }
        } else if (n == 7) { // #RRGGBB
            unsigned int rr,gg,bb;
            if (sscanf(s, "#%02x%02x%02x", &rr,&gg,&bb) == 3){ r=rr; g=gg; b=bb; }
        }
        return (a<<24) | (r<<16) | (g<<8) | b;
    }

    inline void add_hexstr(std::string &out, unsigned int argb){
        char hexbuf[12]; unsigned a=(argb>>24)&0xFF, r=(argb>>16)&0xFF, g=(argb>>8)&0xFF, b=(argb)&0xFF;
        std::snprintf(hexbuf, sizeof(hexbuf), "#%02X%02X%02X%02X", r,g,b,a);
        add_str(out, hexbuf);
    }
    // Forward declaration for nested OSD helper used by handle_stream
    void handle_osd(JsonValue *obj, int idx, std::string &sect, bool &s2, bool &wrote);


    void handle_stream_stats(int idx, std::string &out){
        int fps=0; int bps=0;
        if (idx==0){ fps=cfg->stream0.stats.fps; bps=cfg->stream0.stats.bps; }
        else if (idx==1){ fps=cfg->stream1.stats.fps; bps=cfg->stream1.stats.bps; }
        else { fps=cfg->stream2.stats.fps; bps=cfg->stream2.stats.bps; }
        out += "{"; bool s=false; add_key(out,s,"fps"); add_num(out,fps); add_key(out,s,"Bps"); add_num(out,bps); out += "}";
    }

    void handle_stream(JsonValue *obj, int idx, std::string &out, bool &sep){
        const char* root = idx==0?"stream0":(idx==1?"stream1":"stream2");
        bool wrote=false; std::string sect; sect.reserve(256);
        sect += "{"; bool s2=false;

        auto add_int = [&](const char* key, const std::string &path){
            if (JsonValue* v = obj_get(obj, key)){
                if (v->type == JSON_NUMBER){ cfg->set<int>(path, (int)v->value.number); }
                add_key(sect,s2,key); add_num(sect, cfg->get<int>(path)); wrote=true;
            }
        };
        auto add_boolk = [&](const char* key, const std::string &path){
            if (JsonValue* v = obj_get(obj, key)){
                if (v->type == JSON_BOOL){ cfg->set<bool>(path, v->value.boolean!=0); }
                add_key(sect,s2,key); add_bool(sect, cfg->get<bool>(path)); wrote=true;
            }
        };
        auto add_strk = [&](const char* key, const std::string &path, bool upper=false){
            if (JsonValue* v = obj_get(obj, key)){
                if (v->type == JSON_STRING && v->value.string){
                    std::string val = v->value.string; if (upper) std::transform(val.begin(), val.end(), val.begin(), ::toupper);
                    cfg->set<const char*>(path, strdup(val.c_str()));
                    if (upper) global_restart_video = true;
                }
                add_key(sect,s2,key); add_str(sect, cfg->get<const char*>(path)); wrote=true;
            }
        };

        // Scalars
        if (idx<2){ add_boolk("enabled", std::string(root)+".enabled"); }
    #if defined(AUDIO_SUPPORT)
        if (idx<2){ add_boolk("audio_enabled", std::string(root)+".audio_enabled"); }
    #endif
        if (idx<2){ add_boolk("scale_enabled", std::string(root)+".scale_enabled"); }

        add_strk("rtsp_endpoint", std::string(root)+".rtsp_endpoint");
        add_strk("rtsp_info", std::string(root)+".rtsp_info");
        add_strk("format", std::string(root)+".format");
        add_strk("mode", std::string(root)+".mode", true);

        add_int("gop", std::string(root)+".gop");
        add_int("max_gop", std::string(root)+".max_gop");
        add_int("fps", std::string(root)+".fps");
        add_int("buffers", std::string(root)+".buffers");
        add_int("width", std::string(root)+".width");
        add_int("height", std::string(root)+".height");
        add_int("bitrate", std::string(root)+".bitrate");
        add_int("rotation", std::string(root)+".rotation");
        add_int("scale_width", std::string(root)+".scale_width");
        add_int("scale_height", std::string(root)+".scale_height");
        add_int("profile", std::string(root)+".profile");

        // RC params
        add_int("qp_init", std::string(root)+".qp_init");
        add_int("qp_min", std::string(root)+".qp_min");
        add_int("qp_max", std::string(root)+".qp_max");
        add_int("ip_delta", std::string(root)+".ip_delta");
        add_int("pb_delta", std::string(root)+".pb_delta");
        add_int("max_bitrate", std::string(root)+".max_bitrate");

        // Actions / stats
        if (JsonValue* stats = obj_get(obj, "stats"); stats && stats->type==JSON_NULL){
            add_key(sect,s2,"stats"); handle_stream_stats(idx, sect); wrote=true;
        }
        if (JsonValue* req_idr = obj_get(obj, "request_idr"); req_idr && req_idr->type==JSON_NULL){
            if (idx<2 && global_video[idx]){ global_video[idx]->idr_fix = 1; }
            add_key(sect,s2,"request_idr"); add_str(sect, "initiated"); wrote=true;
        }
        // Nested OSD
        if (idx < 2) {
            if (JsonValue* osd = obj_get(obj, "osd"); osd && osd->type == JSON_OBJECT) {
                add_key(sect, s2, "osd", "{"); bool s3=false; bool wrote_osd=false;
                handle_osd(osd, idx, sect, s3, wrote_osd);
                sect += "}"; wrote = wrote || wrote_osd;
            }
        }

        if (wrote){ add_key(out, sep, root, ""); out += sect; out += "}"; }
    }

    void handle_image(JsonValue *obj, std::string &out, bool &sep){
        add_key(out, sep, "image", "{"); bool s2=false; bool wrote=false;
        auto add_int = [&](const char* key, const char* path, auto setter){
            if (JsonValue* v = obj_get(obj, key)){
                if (v->type == JSON_NUMBER){ cfg->set<int>(path, (int)v->value.number); if (setter) setter(); }
                add_key(out,s2,key); add_num(out, cfg->get<int>(path)); wrote=true;
            }
        };
        auto add_boolk = [&](const char* key, const char* path, auto setter_true, auto setter_false){
            if (JsonValue* v = obj_get(obj, key)){
                if (v->type == JSON_BOOL){ cfg->set<bool>(path, v->value.boolean!=0); if (v->value.boolean) { if (setter_true) setter_true(); } else { if (setter_false) setter_false(); } }
                add_key(out,s2,key); add_bool(out, cfg->get<bool>(path)); wrote=true;
            }
        };

        // Scalars and side-effects
        add_int("brightness", "image.brightness", []{ IMP_ISP_Tuning_SetBrightness(cfg->image.brightness); });
        add_int("contrast",   "image.contrast",   []{ IMP_ISP_Tuning_SetContrast(cfg->image.contrast); });
    #if !defined(PLATFORM_T10) && !defined(PLATFORM_T20) && !defined(PLATFORM_T21) && !defined(PLATFORM_T23) && !defined(PLATFORM_T30)
        add_int("hue",        "image.hue",        []{ IMP_ISP_Tuning_SetBcshHue(cfg->image.hue); });
    #endif
        add_int("saturation", "image.saturation", []{ IMP_ISP_Tuning_SetSaturation(cfg->image.saturation); });
        add_int("sharpness",  "image.sharpness",  []{ IMP_ISP_Tuning_SetSharpness(cfg->image.sharpness); });
    #if !defined(PLATFORM_T21)
        add_int("sinter_strength", "image.sinter_strength", []{ IMP_ISP_Tuning_SetSinterStrength(cfg->image.sinter_strength); });
    #endif
        add_int("temper_strength", "image.temper_strength", []{ IMP_ISP_Tuning_SetTemperStrength(cfg->image.temper_strength); });

        add_boolk("vflip", "image.vflip",
            []{ IMP_ISP_Tuning_SetISPVflip(IMPISP_TUNING_OPS_MODE_ENABLE); },
            []{ IMP_ISP_Tuning_SetISPVflip(IMPISP_TUNING_OPS_MODE_DISABLE); });
        add_boolk("hflip", "image.hflip",
            []{ IMP_ISP_Tuning_SetISPHflip(IMPISP_TUNING_OPS_MODE_ENABLE); },
            []{ IMP_ISP_Tuning_SetISPHflip(IMPISP_TUNING_OPS_MODE_DISABLE); });

        add_int("anti_flicker", "image.anti_flicker", []{ IMP_ISP_Tuning_SetAntiFlickerAttr((IMPISPAntiflickerAttr)cfg->image.anti_flicker); });

        if (JsonValue* rm = obj_get(obj, "running_mode")){
            if (rm->type == JSON_NUMBER){ cfg->set<int>("image.running_mode", (int)rm->value.number); IMP_ISP_Tuning_SetISPRunningMode((IMPISPRunningMode)cfg->image.running_mode); }
            add_key(out,s2,"running_mode"); add_num(out, cfg->get<int>("image.running_mode")); wrote=true;
        }
    #if !defined(PLATFORM_T21)
        add_int("ae_compensation", "image.ae_compensation", []{ IMP_ISP_Tuning_SetAeComp(cfg->image.ae_compensation); });
    #endif
    #if !defined(PLATFORM_T10) && !defined(PLATFORM_T20) && !defined(PLATFORM_T21) && !defined(PLATFORM_T23) && !defined(PLATFORM_T30)
        add_int("dpc_strength", "image.dpc_strength", []{ IMP_ISP_Tuning_SetDPC_Strength(cfg->image.dpc_strength); });
        add_int("drc_strength", "image.drc_strength", []{ IMP_ISP_Tuning_SetDRC_Strength(cfg->image.drc_strength); });
        add_int("defog_strength", "image.defog_strength", []{ uint8_t t=(uint8_t)cfg->image.defog_strength; IMP_ISP_Tuning_SetDefog_Strength(&t); });
        add_int("backlight_compensation", "image.backlight_compensation", []{ IMP_ISP_Tuning_SetBacklightComp(cfg->image.backlight_compensation); });
    #endif
        add_int("highlight_depress", "image.highlight_depress", []{ IMP_ISP_Tuning_SetHiLightDepress(cfg->image.highlight_depress); });
        add_int("max_again", "image.max_again", []{ IMP_ISP_Tuning_SetMaxAgain(cfg->image.max_again); });
        add_int("max_dgain", "image.max_dgain", []{ IMP_ISP_Tuning_SetMaxDgain(cfg->image.max_dgain); });

        // WB bundle
        if (JsonValue* wbmode = obj_get(obj, "core_wb_mode"); wbmode && wbmode->type==JSON_NUMBER){ cfg->set<int>("image.core_wb_mode", (int)wbmode->value.number); }
        if (JsonValue* rg = obj_get(obj, "wb_rgain"); rg && rg->type==JSON_NUMBER){ cfg->set<int>("image.wb_rgain", (int)rg->value.number); }
        if (JsonValue* bg = obj_get(obj, "wb_bgain"); bg && bg->type==JSON_NUMBER){ cfg->set<int>("image.wb_bgain", (int)bg->value.number); }
        if (obj_get(obj, "core_wb_mode") || obj_get(obj, "wb_rgain") || obj_get(obj, "wb_bgain")){
            IMPISPWB wb{}; if (IMP_ISP_Tuning_GetWB(&wb)==0){ wb.mode=(isp_core_wb_mode)cfg->image.core_wb_mode; wb.rgain=cfg->image.wb_rgain; wb.bgain=cfg->image.wb_bgain; IMP_ISP_Tuning_SetWB(&wb); }
            add_key(out,s2,"core_wb_mode"); add_num(out, cfg->get<int>("image.core_wb_mode"));
            add_key(out,s2,"wb_rgain"); add_num(out, cfg->get<int>("image.wb_rgain"));
            add_key(out,s2,"wb_bgain"); add_num(out, cfg->get<int>("image.wb_bgain")); wrote=true;
        }

        if (!wrote){ out.erase(out.size()-1); sep = (out.back()==','); return; }
        out += "}";
    }

    void handle_osd(JsonValue *obj, int idx, std::string &sect, bool &s2, bool &wrote){
        const char* root = idx==0?"stream0.osd":"stream1.osd";
        auto add_int=[&](const char* key,const std::string &path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_NUMBER) cfg->set<int>(path,(int)v->value.number); add_key(sect,s2,key); add_num(sect,cfg->get<int>(path)); wrote=true; }};
        auto add_boolk2=[&](const char* key,const std::string &path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_BOOL) cfg->set<bool>(path, v->value.boolean!=0); add_key(sect,s2,key); add_bool(sect,cfg->get<bool>(path)); wrote=true; }};
        auto add_strs=[&](const char* key,const std::string &path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_STRING && v->value.string) cfg->set<const char*>(path, strdup(v->value.string)); add_key(sect,s2,key); add_str(sect,cfg->get<const char*>(path)); wrote=true; }};
        auto add_hex=[&](const char* key,const std::string &path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_STRING && v->value.string) cfg->set<unsigned int>(path, hexColorToUint(v->value.string)); add_key(sect,s2,key); add_hexstr(sect, cfg->get<unsigned int>(path)); wrote=true; }};

        // Numbers
        add_int("font_size",std::string(root)+".font_size");
        add_int("font_stroke_size",std::string(root)+".font_stroke_size");
        add_int("logo_height",std::string(root)+".logo_height");
        add_int("logo_width",std::string(root)+".logo_width");
        add_int("time_rotation",std::string(root)+".time_rotation");
        add_int("usertext_rotation",std::string(root)+".usertext_rotation");
        add_int("uptime_rotation",std::string(root)+".uptime_rotation");
        add_int("logo_rotation",std::string(root)+".logo_rotation");
        add_int("logo_transparency",std::string(root)+".logo_transparency");
        add_int("start_delay",std::string(root)+".start_delay");

        // Bools
        add_boolk2("enabled",std::string(root)+".enabled");
        add_boolk2("time_enabled",std::string(root)+".time_enabled");
        add_boolk2("usertext_enabled",std::string(root)+".usertext_enabled");
        add_boolk2("uptime_enabled",std::string(root)+".uptime_enabled");
        add_boolk2("logo_enabled",std::string(root)+".logo_enabled");

        // Strings
        add_strs("font_path",std::string(root)+".font_path");
        add_strs("time_format",std::string(root)+".time_format");
        add_strs("uptime_format",std::string(root)+".uptime_format");
        add_strs("usertext_format",std::string(root)+".usertext_format");
        add_strs("logo_path",std::string(root)+".logo_path");
        add_strs("time_position",std::string(root)+".time_position");
        add_strs("uptime_position",std::string(root)+".uptime_position");
        add_strs("usertext_position",std::string(root)+".usertext_position");
        add_strs("logo_position",std::string(root)+".logo_position");

        // Colors (accept hex string)
        add_hex("time_font_color",std::string(root)+".time_font_color");
        add_hex("time_font_stroke_color",std::string(root)+".time_font_stroke_color");
        add_hex("uptime_font_color",std::string(root)+".uptime_font_color");
        add_hex("uptime_font_stroke_color",std::string(root)+".uptime_font_stroke_color");
        add_hex("usertext_font_color",std::string(root)+".usertext_font_color");
        add_hex("usertext_font_stroke_color",std::string(root)+".usertext_font_stroke_color");
    }

#if defined(AUDIO_SUPPORT)
    void handle_audio(JsonValue *obj, std::string &out, bool &sep){
        add_key(out, sep, "audio", "{"); bool s2=false; bool wrote=false;
        auto add_int=[&](const char* key,const char* path, bool restart_audio=false){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_NUMBER){ cfg->set<int>(path,(int)v->value.number); if(restart_audio) global_restart_audio = true; } add_key(out,s2,key); add_num(out,cfg->get<int>(path)); wrote=true; }};
        auto add_boolk_a=[&](const char* key,const char* path, bool rest_rtsp=false, bool rest_audio=true){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_BOOL){ cfg->set<bool>(path, v->value.boolean!=0); if (rest_audio) global_restart_audio = true; if (rest_rtsp) global_restart_rtsp = true; } add_key(out,s2,key); add_bool(out,cfg->get<bool>(path)); wrote=true; }};
        auto add_strk_a=[&](const char* key,const char* path, bool restart_audio=false){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_STRING && v->value.string){ cfg->set<const char*>(path, strdup(v->value.string)); if(restart_audio) global_restart_audio = true; } add_key(out,s2,key); add_str(out,cfg->get<const char*>(path)); wrote=true; }};

        // Input
        add_boolk_a("input_enabled","audio.input_enabled", /*rest_rtsp*/false, /*rest_audio*/true);
        add_strk_a("input_format","audio.input_format", true);
        add_int("input_vol","audio.input_vol", false);
        add_int("input_gain","audio.input_gain", true);
        add_int("input_bitrate","audio.input_bitrate", true);
        add_int("input_sample_rate","audio.input_sample_rate", true);
    #if defined(LIB_AUDIO_PROCESSING)
        add_int("input_alc_gain","audio.input_alc_gain", false);
        add_int("input_noise_suppression","audio.input_noise_suppression", true);
        add_boolk_a("input_high_pass_filter","audio.input_high_pass_filter", false, true);
        add_boolk_a("input_agc_enabled","audio.input_agc_enabled", false, true);
        add_int("input_agc_target_level_dbfs","audio.input_agc_target_level_dbfs", true);
        add_int("input_agc_compression_gain_db","audio.input_agc_compression_gain_db", true);
        add_boolk_a("force_stereo","audio.force_stereo", false, true);
        // Output
        add_boolk_a("output_enabled","audio.output_enabled", /*rest_rtsp*/true, /*rest_audio*/true);
        add_int("output_sample_rate","audio.output_sample_rate", true);
    #endif
        add_int("buffer_warn_frames","audio.buffer_warn_frames", false);
        add_int("buffer_cap_frames","audio.buffer_cap_frames", false);

        if(!wrote){ out.erase(out.size()-1); return; } out += "}";
    }
#endif

    void handle_motion(JsonValue *obj, std::string &out, bool &sep){
        add_key(out, sep, "motion", "{"); bool s2=false; bool wrote=false;
        auto add_int=[&](const char* key,const char* path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_NUMBER) cfg->set<int>(path,(int)v->value.number); add_key(out,s2,key); add_num(out,cfg->get<int>(path)); wrote=true; }};
        auto add_boolk_m=[&](const char* key,const char* path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_BOOL) cfg->set<bool>(path, v->value.boolean!=0); add_key(out,s2,key); add_bool(out,cfg->get<bool>(path)); wrote=true; }};
        auto add_strs=[&](const char* key,const char* path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_STRING && v->value.string) cfg->set<const char*>(path, strdup(v->value.string)); add_key(out,s2,key); add_str(out,cfg->get<const char*>(path)); wrote=true; }};

        add_int("monitor_stream","motion.monitor_stream");
        add_int("debounce_time","motion.debounce_time");
        add_int("post_time","motion.post_time");
        add_int("cooldown_time","motion.cooldown_time");
        add_int("init_time","motion.init_time");
        add_int("min_time","motion.min_time");
        add_int("ivs_polling_timeout","motion.ivs_polling_timeout");
        add_int("sensitivity","motion.sensitivity");
        add_int("skip_frame_count","motion.skip_frame_count");
        add_int("frame_width","motion.frame_width");
        add_int("frame_height","motion.frame_height");
        add_int("roi_0_x","motion.roi_0_x"); add_int("roi_0_y","motion.roi_0_y");
        add_int("roi_1_x","motion.roi_1_x"); add_int("roi_1_y","motion.roi_1_y");
        add_int("roi_count","motion.roi_count");
        add_boolk_m("enabled","motion.enabled");
        add_strs("script_path","motion.script_path");

        if (JsonValue* rois = obj_get(obj, "rois")){
            if (rois->type == JSON_NULL){
                add_key(out,s2,"rois", "["); bool sA=false;
                int cnt = std::min(cfg->motion.roi_count, (int)cfg->motion.rois.size());
                for (int i=0;i<cnt;i++){ if (sA) out += ","; else sA=true; const auto &r = cfg->motion.rois[i]; out += "["; add_num(out, r.p0_x); out += ","; add_num(out, r.p0_y); out += ","; add_num(out, r.p1_x); out += ","; add_num(out, r.p1_y); out += "]"; }
                out += "]"; wrote=true;
            } else if (rois->type == JSON_ARRAY){
                // Expect array of [x0,y0,x1,y1]
                int i=0; for (JsonArrayItem *it = rois->value.array_head; it && i < (int)cfg->motion.rois.size(); it = it->next, ++i){
                    JsonValue *sub = it->value;
                    if (sub && sub->type == JSON_ARRAY){
                        int vals[4]={0}; int j=0; for (JsonArrayItem *jt = sub->value.array_head; jt && j<4; jt = jt->next, ++j){ JsonValue *jv = jt->value; if (jv && jv->type==JSON_NUMBER) vals[j]=(int)jv->value.number; }
                        cfg->motion.rois[i].p0_x = vals[0]; cfg->motion.rois[i].p0_y = vals[1]; cfg->motion.rois[i].p1_x = vals[2]; cfg->motion.rois[i].p1_y = vals[3];
                    }
                }
                cfg->motion.roi_count = i;
                add_key(out,s2,"rois"); out += "\"ok\""; wrote=true;
            }
        }

        if(!wrote){ out.erase(out.size()-1); return; } out += "}";
    }

    void handle_info(JsonValue *obj, std::string &out, bool &sep){
        add_key(out, sep, "info", "{"); bool s2=false; bool wrote=false;
        // Currently only imp_system_version (read)
        if (JsonValue* v = obj_get(obj, "imp_system_version"); v && v->type == JSON_NULL){
            add_key(out,s2,"imp_system_version"); add_str(out, "unknown"); wrote=true;
        }
        if(!wrote){ out.erase(out.size()-1); return; } out += "}";
    }

    void handle_action(JsonValue *obj, std::string &out, bool &sep){
        add_key(out, sep, "action", "{"); bool s2=false; bool wrote=false;
        if (JsonValue* rst = obj_get(obj, "restart_thread"); rst && rst->type==JSON_NUMBER){
            int mask = (int)rst->value.number;
            if (mask & 1) global_restart_rtsp = true;
            if (mask & 2) global_restart_video = true;
            if (mask & 4) global_restart_audio = true;
            add_key(out,s2,"restart_thread"); add_str(out, "ok"); wrote=true;
        }
        if (JsonValue* sv = obj_get(obj, "save_config"); sv && sv->type==JSON_NULL){
            cfg->updateConfig(); add_key(out,s2,"save_config"); add_str(out, "ok"); wrote=true;
        }
        if (JsonValue* cap = obj_get(obj, "capture"); cap){
            // Stub hook for capture; WebUI should use /cgi-bin/chX.jpg
            add_key(out,s2,"capture"); add_str(out, "use_snapshot_api"); wrote=true;
        }
        if(!wrote){ out.erase(out.size()-1); return; } out += "}";
    }


    void handle_rtsp(JsonValue *obj, std::string &out, bool &sep){
        add_key(out, sep, "rtsp", "{"); bool s2=false; auto wrote=false;
        auto add_int=[&](const char* key, const char* path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_NUMBER) cfg->set<int>(path,(int)v->value.number); add_key(out,s2,key); add_num(out,cfg->get<int>(path)); wrote=true; }};
        auto add_str_r=[&](const char* key, const char* path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_STRING && v->value.string) cfg->set<const char*>(path, strdup(v->value.string)); add_key(out,s2,key); add_str(out,cfg->get<const char*>(path)); wrote=true; }};
        auto add_bool_r=[&](const char* key, const char* path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_BOOL) cfg->set<bool>(path, v->value.boolean!=0); add_key(out,s2,key); add_bool(out,cfg->get<bool>(path)); wrote=true; }};
        add_int("port","rtsp.port"); add_int("est_bitrate","rtsp.est_bitrate"); add_int("out_buffer_size","rtsp.out_buffer_size"); add_int("send_buffer_size","rtsp.send_buffer_size");
        add_bool_r("auth_required","rtsp.auth_required");
        add_str_r("name","rtsp.name"); add_str_r("username","rtsp.username"); add_str_r("password","rtsp.password");
        if(!wrote){ out.erase(out.size()-1); return; } out += "}";
    }

    void handle_sensor(JsonValue *obj, std::string &out, bool &sep){
        add_key(out, sep, "sensor", "{"); bool s2=false; bool wrote=false;
        auto add_int=[&](const char* key,const char* path){ if(JsonValue* v=obj_get(obj,key)){ add_key(out,s2,key); add_num(out,cfg->get<int>(path)); wrote=true; }};
        auto add_str_s=[&](const char* key,const char* path){ if(JsonValue* v=obj_get(obj,key)){ add_key(out,s2,key); add_str(out,cfg->get<const char*>(path)); wrote=true; }};
        auto add_uint=[&](const char* key,const char* path){ if(JsonValue* v=obj_get(obj,key)){ add_key(out,s2,key); char b[16]; std::snprintf(b,sizeof(b),"%#x", cfg->get<unsigned int>(path)); out += '"'; out += b; out += '"'; wrote=true; }};
        add_str_s("model","sensor.model"); add_int("fps","sensor.fps"); add_int("width","sensor.width"); add_int("height","sensor.height"); add_uint("i2c_address","sensor.i2c_address");
        if(!wrote){ out.erase(out.size()-1); return; } out += "}";
    }

    void handle_stream2(JsonValue *obj, std::string &out, bool &sep){
        add_key(out, sep, "stream2", "{"); bool s2=false; bool wrote=false;
        auto add_bool_s2=[&](const char* key,const char* path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_BOOL) cfg->set<bool>(path, v->value.boolean!=0); add_key(out,s2,key); add_bool(out,cfg->get<bool>(path)); wrote=true; }};
        auto add_int=[&](const char* key,const char* path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_NUMBER) cfg->set<int>(path,(int)v->value.number); add_key(out,s2,key); add_num(out,cfg->get<int>(path)); wrote=true; }};
        if (JsonValue* stats = obj_get(obj, "stats"); stats && stats->type==JSON_NULL){ add_key(out,s2,"stats"); handle_stream_stats(2,out); wrote=true; }
        add_bool_s2("jpeg_enabled","stream2.enabled");
        add_int("jpeg_quality","stream2.jpeg_quality");
        add_int("jpeg_channel","stream2.jpeg_channel");
        add_int("fps","stream2.fps");
        if(!wrote){ out.erase(out.size()-1); return; } out += "}";
    }

    void handle_general(JsonValue *obj, std::string &out, bool &sep){
        JsonValue *ll = obj_get(obj, "loglevel");
        if (ll){
            add_key(out, sep, "general", "{"); bool s2=false;
            if (ll->type == JSON_STRING){
                cfg->set<const char*>(std::string("general.loglevel"), strdup(ll->value.string));
                add_key(out,s2,"loglevel"); add_str(out, cfg->general.loglevel);
            } else if (ll->type == JSON_NULL){
                add_key(out,s2,"loglevel"); add_str(out, cfg->general.loglevel);
            }
            out += "}";
        }
    }
}

namespace JsonAPI {

bool process_json(const std::string &in, std::string &out)
{
    JsonValue *root = parse_json_string(in.c_str());
    if (!root || root->type != JSON_OBJECT){ if(root) free_json_value(root); out = "{}"; return false; }

    out = "{"; bool sep=false;

    // Iterate top-level keys
    for (JsonKeyValue *kv = root->value.object_head; kv; kv = kv->next){
        const char *k = kv->key; JsonValue *v = kv->value;
        if (!strcmp(k, "stream0") && v && v->type == JSON_OBJECT){ handle_stream(v, 0, out, sep); }
        else if (!strcmp(k, "stream1") && v && v->type == JSON_OBJECT){ handle_stream(v, 1, out, sep); }
        else if (!strcmp(k, "stream2") && v && v->type == JSON_OBJECT){ handle_stream(v, 2, out, sep); }
        else if (!strcmp(k, "image")   && v && v->type == JSON_OBJECT){ handle_image(v, out, sep); }
        else if (!strcmp(k, "general") && v && v->type == JSON_OBJECT){ handle_general(v, out, sep); }
        else if (!strcmp(k, "rtsp")    && v && v->type == JSON_OBJECT){ handle_rtsp(v, out, sep); }
        else if (!strcmp(k, "sensor")  && v && v->type == JSON_OBJECT){ handle_sensor(v, out, sep); }
        else if (!strcmp(k, "stream2") && v && v->type == JSON_OBJECT){ handle_stream2(v, out, sep); }
    #if defined(AUDIO_SUPPORT)
        else if (!strcmp(k, "audio")   && v && v->type == JSON_OBJECT){ handle_audio(v, out, sep); }
    #endif
        else if (!strcmp(k, "motion")  && v && v->type == JSON_OBJECT){ handle_motion(v, out, sep); }
        else if (!strcmp(k, "info")    && v && v->type == JSON_OBJECT){ handle_info(v, out, sep); }
        else if (!strcmp(k, "action")  && v && v->type == JSON_OBJECT){ handle_action(v, out, sep); }
    }

    out += "}";
    free_json_value(root);
    return true;
}

} // namespace JsonAPI

