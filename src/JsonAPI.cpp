#include "JsonAPI.hpp"
#include "Config.hpp"
#include "globals.hpp"

extern "C" {
#include <3rdparty/jct/src/json_config.h>
// Forward declare string parser present in json_parse.c but not in the header
JsonValue *parse_json_string(const char *json_str);
}

#include <imp/imp_isp.h>
#include <sstream>
#include <cstring>
#include <algorithm>

namespace {
    // Tiny JSON builder helpers (string-based)
    inline void add_key(std::string &out, bool &sep, const char *k, const char *open=""){
        if (sep) out.push_back(','); else sep=true;
        out.push_back('"'); out += k; out += '":'; out += open;
    }
    inline void add_str(std::string &out, const char *s){ out.push_back('"'); out += s ? s : ""; out.push_back('"'); }
    inline void add_num(std::string &out, int v){ char b[32]; std::snprintf(b,sizeof(b),"%d",v); out += b; }
    inline void add_bool(std::string &out, bool v){ out += (v?"true":"false"); }

    // Look up a child by key (convenience)
    JsonValue* obj_get(JsonValue *obj, const char *k){ return get_object_item(obj, k); }

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

        auto add_int = [&](const char* key, const char* path, int &ref){
            if (JsonValue* v = obj_get(obj, key)){
                if (v->type == JSON_NUMBER){ cfg->set<int>(path, (int)v->value.number); }
                add_key(sect,s2,key); add_num(sect, cfg->get<int>(path)); wrote=true;
            }
        };
        auto add_boolk = [&](const char* key, const char* path, bool &ref){
            if (JsonValue* v = obj_get(obj, key)){
                if (v->type == JSON_BOOL){ cfg->set<bool>(path, v->value.boolean!=0); }
                add_key(sect,s2,key); add_bool(sect, cfg->get<bool>(path)); wrote=true;
            }
        };
        auto add_strk = [&](const char* key, const char* path, const char* &ref, bool upper=false){
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
        if (idx<2){ add_boolk("enabled", std::string(root)+".enabled", idx==0?cfg->stream0.enabled:cfg->stream1.enabled); }
    #if defined(AUDIO_SUPPORT)
        if (idx<2){ add_boolk("audio_enabled", std::string(root)+".audio_enabled", idx==0?cfg->stream0.audio_enabled:cfg->stream1.audio_enabled); }
    #endif
        if (idx<2){ add_boolk("scale_enabled", std::string(root)+".scale_enabled", idx==0?cfg->stream0.scale_enabled:cfg->stream1.scale_enabled); }

        add_strk("rtsp_endpoint", std::string(root)+".rtsp_endpoint", idx==0?cfg->stream0.rtsp_endpoint: (idx==1?cfg->stream1.rtsp_endpoint:cfg->stream2.rtsp_endpoint));
        add_strk("rtsp_info", std::string(root)+".rtsp_info", idx==0?cfg->stream0.rtsp_info: (idx==1?cfg->stream1.rtsp_info:cfg->stream2.rtsp_info));
        add_strk("format", std::string(root)+".format", idx==0?cfg->stream0.format: (idx==1?cfg->stream1.format:cfg->stream2.format));
        add_strk("mode", std::string(root)+".mode", idx==0?cfg->stream0.mode:cfg->stream1.mode, true);

        add_int("gop", std::string(root)+".gop", idx==0?cfg->stream0.gop:cfg->stream1.gop);
        add_int("max_gop", std::string(root)+".max_gop", idx==0?cfg->stream0.max_gop:cfg->stream1.max_gop);
        add_int("fps", std::string(root)+".fps", idx==0?cfg->stream0.fps: (idx==1?cfg->stream1.fps:cfg->stream2.fps));
        add_int("buffers", std::string(root)+".buffers", idx==0?cfg->stream0.buffers:cfg->stream1.buffers);
        add_int("width", std::string(root)+".width", idx==0?cfg->stream0.width: (idx==1?cfg->stream1.width:cfg->stream2.width));
        add_int("height", std::string(root)+".height", idx==0?cfg->stream0.height: (idx==1?cfg->stream1.height:cfg->stream2.height));
        add_int("bitrate", std::string(root)+".bitrate", idx==0?cfg->stream0.bitrate:cfg->stream1.bitrate);
        add_int("rotation", std::string(root)+".rotation", idx==0?cfg->stream0.rotation:cfg->stream1.rotation);
        add_int("scale_width", std::string(root)+".scale_width", idx==0?cfg->stream0.scale_width:cfg->stream1.scale_width);
        add_int("scale_height", std::string(root)+".scale_height", idx==0?cfg->stream0.scale_height:cfg->stream1.scale_height);
        add_int("profile", std::string(root)+".profile", idx==0?cfg->stream0.profile:cfg->stream1.profile);

        // RC params
        add_int("qp_init", std::string(root)+".qp_init", idx==0?cfg->stream0.qp_init:cfg->stream1.qp_init);
        add_int("qp_min", std::string(root)+".qp_min", idx==0?cfg->stream0.qp_min:cfg->stream1.qp_min);
        add_int("qp_max", std::string(root)+".qp_max", idx==0?cfg->stream0.qp_max:cfg->stream1.qp_max);
        add_int("ip_delta", std::string(root)+".ip_delta", idx==0?cfg->stream0.ip_delta:cfg->stream1.ip_delta);
        add_int("pb_delta", std::string(root)+".pb_delta", idx==0?cfg->stream0.pb_delta:cfg->stream1.pb_delta);
        add_int("max_bitrate", std::string(root)+".max_bitrate", idx==0?cfg->stream0.max_bitrate:cfg->stream1.max_bitrate);

        // Actions / stats
        if (JsonValue* stats = obj_get(obj, "stats"); stats && stats->type==JSON_NULL){
            add_key(sect,s2,"stats"); handle_stream_stats(idx, sect); wrote=true;
        }
        if (JsonValue* req_idr = obj_get(obj, "request_idr"); req_idr && req_idr->type==JSON_NULL){
            if (idx<2 && global_video[idx]){ global_video[idx]->idr_fix = 1; }
            add_key(sect,s2,"request_idr"); add_str(sect, "initiated"); wrote=true;
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
        auto add_null_read = [&](const char* key, const char* path){
            if (JsonValue* v = obj_get(obj, key)){ if (v->type==JSON_NULL){ add_key(out,s2,key); add_num(out, cfg->get<int>(path)); wrote=true; } }
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

    void handle_rtsp(JsonValue *obj, std::string &out, bool &sep){
        add_key(out, sep, "rtsp", "{"); bool s2=false; auto wrote=false;
        auto add_int=[&](const char* key, const char* path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_NUMBER) cfg->set<int>(path,(int)v->value.number); add_key(out,s2,key); add_num(out,cfg->get<int>(path)); wrote=true; }};
        auto add_str=[&](const char* key, const char* path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_STRING && v->value.string) cfg->set<const char*>(path, strdup(v->value.string)); add_key(out,s2,key); add_str(out,cfg->get<const char*>(path)); wrote=true; }};
        auto add_bool=[&](const char* key, const char* path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_BOOL) cfg->set<bool>(path, v->value.boolean!=0); add_key(out,s2,key); add_bool(out,cfg->get<bool>(path)); wrote=true; }};
        add_int("port","rtsp.port"); add_int("est_bitrate","rtsp.est_bitrate"); add_int("out_buffer_size","rtsp.out_buffer_size"); add_int("send_buffer_size","rtsp.send_buffer_size");
        add_bool("auth_required","rtsp.auth_required");
        add_str("name","rtsp.name"); add_str("username","rtsp.username"); add_str("password","rtsp.password");
        if(!wrote){ out.erase(out.size()-1); return; } out += "}";
    }

    void handle_sensor(JsonValue *obj, std::string &out, bool &sep){
        add_key(out, sep, "sensor", "{"); bool s2=false; bool wrote=false;
        auto add_int=[&](const char* key,const char* path){ if(JsonValue* v=obj_get(obj,key)){ add_key(out,s2,key); add_num(out,cfg->get<int>(path)); wrote=true; }};
        auto add_str=[&](const char* key,const char* path){ if(JsonValue* v=obj_get(obj,key)){ add_key(out,s2,key); add_str(out,cfg->get<const char*>(path)); wrote=true; }};
        auto add_uint=[&](const char* key,const char* path){ if(JsonValue* v=obj_get(obj,key)){ add_key(out,s2,key); char b[16]; std::snprintf(b,sizeof(b),"%#x", cfg->get<unsigned int>(path)); out += '"'; out += b; out += '"'; wrote=true; }};
        add_str("model","sensor.model"); add_int("fps","sensor.fps"); add_int("width","sensor.width"); add_int("height","sensor.height"); add_uint("i2c_address","sensor.i2c_address");
        if(!wrote){ out.erase(out.size()-1); return; } out += "}";
    }

    void handle_stream2(JsonValue *obj, std::string &out, bool &sep){
        add_key(out, sep, "stream2", "{"); bool s2=false; bool wrote=false;
        auto add_bool=[&](const char* key,const char* path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_BOOL) cfg->set<bool>(path, v->value.boolean!=0); add_key(out,s2,key); add_bool(out,cfg->get<bool>(path)); wrote=true; }};
        auto add_int=[&](const char* key,const char* path){ if(JsonValue* v=obj_get(obj,key)){ if(v->type==JSON_NUMBER) cfg->set<int>(path,(int)v->value.number); add_key(out,s2,key); add_num(out,cfg->get<int>(path)); wrote=true; }};
        if (JsonValue* stats = obj_get(obj, "stats"); stats && stats->type==JSON_NULL){ add_key(out,s2,"stats"); handle_stream_stats(2,out); wrote=true; }
        add_bool("jpeg_enabled","stream2.enabled");
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
        // TODO: audio, motion, osd, info, action
    }

    out += "}";
    free_json_value(root);
    return true;
}

} // namespace JsonAPI

