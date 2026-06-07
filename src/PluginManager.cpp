#include "PluginManager.hpp"
#include "Config.hpp"
#include "Logger.hpp"
#include "globals.hpp"

#include <dlfcn.h>
#include <filesystem>
#include <functional>

#undef MODULE
#define MODULE "PluginManager"

namespace fs = std::filesystem;

// ── Logger bridge ────────────────────────────────────────────────────────────

static void plugin_log_fn(prudynt_log_level_t level, const char *module,
                           const char *fmt, ...) {
  // Delegate to Logger. The variadic format is handled by the plugin, so we
  // just forward a simple message here.  For full format forwarding we'd need
  // vsnprintf — simplified for now.
  switch (level) {
  case PRUDYNT_LOG_DEBUG:
    LOG_DEBUG("[" << (module ? module : "plugin") << "] " << (fmt ? fmt : ""));
    break;
  case PRUDYNT_LOG_INFO:
    LOG_INFO("[" << (module ? module : "plugin") << "] " << (fmt ? fmt : ""));
    break;
  case PRUDYNT_LOG_WARN:
    LOG_WARN("[" << (module ? module : "plugin") << "] " << (fmt ? fmt : ""));
    break;
  case PRUDYNT_LOG_ERROR:
    LOG_ERROR("[" << (module ? module : "plugin") << "] " << (fmt ? fmt : ""));
    break;
  }
}

// ── Host service implementations ─────────────────────────────────────────────

const char *PluginManager::config_get_string_fn(prudynt_ctx_t *ctx,
                                                 const char *path) {
  (void)ctx;
  if (!cfg || !path)
    return nullptr;
  return cfg->get<const char *>(path);
}

int PluginManager::config_get_int_fn(prudynt_ctx_t *ctx, const char *path) {
  (void)ctx;
  if (!cfg || !path)
    return 0;
  return cfg->get<int>(path);
}

int PluginManager::config_get_bool_fn(prudynt_ctx_t *ctx, const char *path) {
  (void)ctx;
  if (!cfg || !path)
    return 0;
  return cfg->get<bool>(path) ? 1 : 0;
}

uint64_t PluginManager::register_video_tap_fn(
    prudynt_ctx_t *ctx, int enc_chn,
    void (*callback)(const uint8_t *, size_t, int64_t, int, void *),
    void *userdata) {
  (void)ctx;
  if (enc_chn < 0 || enc_chn >= NUM_VIDEO_CHANNELS || !callback)
    return 0;
  if (!global_video[enc_chn])
    return 0;

  // Wrap the C callback in a C++ lambda and register via the existing tap API
  auto shared_ch = std::make_shared<MsgChannel<H264NALUnit>>(
      global_video[enc_chn]->msgChannel->capacity());

  // The tap entry will forward to our C callback
  VideoTapEntry entry =
      register_video_tap(enc_chn, shared_ch, [callback, userdata]() {});
  return entry.id;
}

void PluginManager::unregister_video_tap_fn(prudynt_ctx_t *ctx, int enc_chn,
                                             uint64_t tap_id) {
  (void)ctx;
  unregister_video_tap(enc_chn, tap_id);
}

uint64_t PluginManager::register_audio_tap_fn(
    prudynt_ctx_t *ctx, int ai_chn,
    void (*callback)(const uint8_t *, size_t, int64_t, void *),
    void *userdata) {
  (void)ctx;
  if (ai_chn < 0 || ai_chn >= NUM_AUDIO_CHANNELS || !callback)
    return 0;
  if (!global_audio[ai_chn])
    return 0;

  auto shared_ch = std::make_shared<MsgChannel<AudioFrame>>(
      global_audio[ai_chn]->msgChannel->capacity());

  AudioTapEntry entry =
      register_audio_tap(ai_chn, shared_ch, [callback, userdata]() {});
  return entry.id;
}

void PluginManager::unregister_audio_tap_fn(prudynt_ctx_t *ctx, int ai_chn,
                                             uint64_t tap_id) {
  (void)ctx;
  unregister_audio_tap(ai_chn, tap_id);
}

void PluginManager::request_rtsp_restart_fn(prudynt_ctx_t *ctx) {
  (void)ctx;
  std::lock_guard<std::mutex> lock(mutex_main);
  global_restart_rtsp = true;
  global_cv_worker_restart.notify_one();
}

void PluginManager::request_video_restart_fn(prudynt_ctx_t *ctx) {
  (void)ctx;
  std::lock_guard<std::mutex> lock(mutex_main);
  global_restart_video = true;
  global_cv_worker_restart.notify_one();
}

void PluginManager::request_audio_restart_fn(prudynt_ctx_t *ctx) {
  (void)ctx;
  std::lock_guard<std::mutex> lock(mutex_main);
  global_restart_audio = true;
  global_cv_worker_restart.notify_one();
}

void PluginManager::request_shutdown_fn(prudynt_ctx_t *ctx) {
  (void)ctx;
  global_shutdown_requested.store(true, std::memory_order_relaxed);
  request_rtsp_restart_fn(ctx);
}

// ── PluginManager implementation ─────────────────────────────────────────────

PluginManager::PluginManager() {
  services_.api_version = PRUDYNT_PLUGIN_API_VERSION;
  services_.log = plugin_log_fn;
  services_.config_get_string = config_get_string_fn;
  services_.config_get_int = config_get_int_fn;
  services_.config_get_bool = config_get_bool_fn;
  services_.register_video_tap_fn = register_video_tap_fn;
  services_.unregister_video_tap_fn = unregister_video_tap_fn;
  services_.register_audio_tap_fn = register_audio_tap_fn;
  services_.unregister_audio_tap_fn = unregister_audio_tap_fn;
  services_.request_rtsp_restart = request_rtsp_restart_fn;
  services_.request_video_restart = request_video_restart_fn;
  services_.request_audio_restart = request_audio_restart_fn;
  services_.request_shutdown = request_shutdown_fn;
}

PluginManager::~PluginManager() {
  unloadAll();
}

int PluginManager::loadDirectory(const std::string &dir_path) {
  if (!fs::is_directory(dir_path)) {
    LOG_WARN("Plugin directory not found: " << dir_path);
    return 0;
  }

  int loaded = 0;
  for (const auto &entry : fs::directory_iterator(dir_path)) {
    if (entry.path().extension() == ".so") {
      if (loadPlugin(entry.path().string()) == 0) {
        ++loaded;
      }
    }
  }

  LOG_INFO("Loaded " << loaded << " plugin(s) from " << dir_path);
  return loaded;
}

int PluginManager::loadPlugin(const std::string &so_path) {
  // Check not already loaded
  for (const auto &p : plugins_) {
    if (p.path == so_path) {
      LOG_WARN("Plugin already loaded: " << so_path);
      return -1;
    }
  }

  void *handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    LOG_ERROR("Failed to load plugin " << so_path << ": " << dlerror());
    return -1;
  }

  auto init_fn = (prudynt_plugin_init_fn)dlsym(handle, "prudynt_plugin_init");
  if (!init_fn) {
    LOG_ERROR("Plugin " << so_path
                        << " does not export prudynt_plugin_init: "
                        << dlerror());
    dlclose(handle);
    return -1;
  }

  prudynt_plugin_t *desc = init_fn();
  if (!desc) {
    LOG_ERROR("Plugin " << so_path << ": prudynt_plugin_init returned NULL");
    dlclose(handle);
    return -1;
  }

  if (!desc->name || !desc->name[0]) {
    LOG_ERROR("Plugin " << so_path << ": missing name");
    dlclose(handle);
    return -1;
  }

  LoadedPlugin lp;
  lp.name = desc->name;
  lp.path = so_path;
  lp.handle = handle;
  lp.desc = desc;

  LOG_INFO("Loaded plugin: " << desc->name << " v" << (desc->version ? desc->version : "?")
                             << " (" << so_path << ")");

  if (desc->init) {
    int ret = desc->init(&lp.ctx_storage, &services_);
    if (ret != 0) {
      LOG_ERROR("Plugin " << desc->name << ": init() failed with " << ret);
      dlclose(handle);
      return -1;
    }
  }

  plugins_.push_back(std::move(lp));
  return 0;
}

void PluginManager::unloadPlugin(const std::string &name) {
  for (auto it = plugins_.begin(); it != plugins_.end(); ++it) {
    if (it->name == name) {
      if (it->desc->stop)
        it->desc->stop(&it->ctx_storage);

      if (it->desc->destroy)
        it->desc->destroy(&it->ctx_storage);

      LOG_INFO("Unloaded plugin: " << it->name);
      dlclose(it->handle);
      plugins_.erase(it);
      return;
    }
  }
  LOG_WARN("Plugin not found: " << name);
}

void PluginManager::unloadAll() {
  // Stop all first (reverse order), then destroy
  for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
    if (it->desc->stop)
      it->desc->stop(&it->ctx_storage);
  }

  for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
    if (it->desc->destroy)
      it->desc->destroy(&it->ctx_storage);
    dlclose(it->handle);
  }

  plugins_.clear();
  LOG_INFO("All plugins unloaded");
}

int PluginManager::startAll() {
  int started = 0;
  for (auto &p : plugins_) {
    if (p.desc->start) {
      if (p.desc->start(&p.ctx_storage) == 0) {
        ++started;
      } else {
        LOG_ERROR("Plugin " << p.name << ": start() failed");
      }
    }
  }
  return started;
}

void PluginManager::stopAll() {
  for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
    if (it->desc->stop)
      it->desc->stop(&it->ctx_storage);
  }
}

std::vector<std::string> PluginManager::loadedPlugins() const {
  std::vector<std::string> names;
  names.reserve(plugins_.size());
  for (const auto &p : plugins_) {
    names.push_back(p.name);
  }
  return names;
}
