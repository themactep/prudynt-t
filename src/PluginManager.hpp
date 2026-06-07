#ifndef PLUGIN_MANAGER_HPP
#define PLUGIN_MANAGER_HPP

#include "plugin_api.h"

#include <string>
#include <vector>

class PluginManager {
public:
  PluginManager();
  ~PluginManager();

  // Scan a directory for plugin .so files and load them.
  // Returns the number of plugins successfully loaded.
  int loadDirectory(const std::string &dir_path);

  // Load a single plugin by full path to .so.
  // Returns 0 on success.
  int loadPlugin(const std::string &so_path);

  // Unload a plugin by name.
  void unloadPlugin(const std::string &name);

  // Unload all plugins.
  void unloadAll();

  // Start all loaded plugins.
  int startAll();

  // Stop all loaded plugins.
  void stopAll();

  // Get list of loaded plugin names.
  std::vector<std::string> loadedPlugins() const;

private:
  struct LoadedPlugin {
    std::string name;
    std::string path;
    void *handle;                       // dlopen handle
    prudynt_plugin_t *desc;             // plugin descriptor
    prudynt_ctx_t ctx_storage;          // lightweight context for this plugin
  };

  std::vector<LoadedPlugin> plugins_;

  // Host services vtable shared by all plugins
  prudynt_host_services_t services_;

  static const char *config_get_string_fn(prudynt_ctx_t *ctx, const char *path);
  static int config_get_int_fn(prudynt_ctx_t *ctx, const char *path);
  static int config_get_bool_fn(prudynt_ctx_t *ctx, const char *path);
  static uint64_t register_video_tap_fn(prudynt_ctx_t *ctx, int enc_chn,
                                        void (*callback)(const uint8_t *, size_t,
                                                         int64_t, int, void *),
                                        void *userdata);
  static void unregister_video_tap_fn(prudynt_ctx_t *ctx, int enc_chn,
                                      uint64_t tap_id);
  static uint64_t register_audio_tap_fn(prudynt_ctx_t *ctx, int ai_chn,
                                        void (*callback)(const uint8_t *, size_t,
                                                         int64_t, void *),
                                        void *userdata);
  static void unregister_audio_tap_fn(prudynt_ctx_t *ctx, int ai_chn,
                                      uint64_t tap_id);
  static void request_rtsp_restart_fn(prudynt_ctx_t *ctx);
  static void request_video_restart_fn(prudynt_ctx_t *ctx);
  static void request_audio_restart_fn(prudynt_ctx_t *ctx);
  static void request_shutdown_fn(prudynt_ctx_t *ctx);
};

#endif // PLUGIN_MANAGER_HPP
