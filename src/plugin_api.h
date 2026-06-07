#ifndef PRUDYNT_PLUGIN_API_H
#define PRUDYNT_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* API version — increment on breaking changes */
#define PRUDYNT_PLUGIN_API_VERSION 1

/* Opaque context for the plugin (owned by the host).
 * The struct is empty — only the host knows what goes inside.
 * Plugins receive a pointer and pass it back to host service calls. */
typedef struct prudynt_ctx {
  int reserved;
} prudynt_ctx_t;

/* Logger severity levels */
typedef enum {
  PRUDYNT_LOG_DEBUG,
  PRUDYNT_LOG_INFO,
  PRUDYNT_LOG_WARN,
  PRUDYNT_LOG_ERROR,
} prudynt_log_level_t;

/* Log callback provided by the host */
typedef void (*prudynt_log_fn)(prudynt_log_level_t level, const char *module,
                               const char *fmt, ...);

/* ── Host-provided services accessible to plugins ────────────────────────────*/

/* Host service table — passed to plugin at init() */
typedef struct {
  /* API version for ABI compatibility */
  uint32_t api_version;

  /* Logging */
  prudynt_log_fn log;

  /* Config access: get a string config value by path (e.g. "general.loglevel").
   * Returns the value or NULL if not found. The returned string is owned by the
   * host and must not be freed. */
  const char *(*config_get_string)(prudynt_ctx_t *ctx, const char *path);

  /* Config access: get an integer config value. Returns 0 if not found. */
  int (*config_get_int)(prudynt_ctx_t *ctx, const char *path);

  /* Config access: get a boolean config value. Returns false if not found. */
  int (*config_get_bool)(prudynt_ctx_t *ctx, const char *path);

  /* Register a video data tap for a given encoder channel (0 or 1).
   * |callback| is invoked for each H.264/H.265 NAL unit.
   * |userdata| is passed back to the callback.
   * Returns a tap ID (for unregister) or 0 on failure. */
  uint64_t (*register_video_tap_fn)(prudynt_ctx_t *ctx, int enc_chn,
                                    void (*callback)(const uint8_t *data,
                                                     size_t len,
                                                     int64_t timestamp_us,
                                                     int is_keyframe,
                                                     void *userdata),
                                    void *userdata);

  /* Unregister a previously registered video tap. */
  void (*unregister_video_tap_fn)(prudynt_ctx_t *ctx, int enc_chn,
                                  uint64_t tap_id);

  /* Register an audio data tap for a given audio channel (0).
   * |callback| is invoked for each encoded audio frame.
   * Returns a tap ID (for unregister) or 0 on failure. */
  uint64_t (*register_audio_tap_fn)(prudynt_ctx_t *ctx, int ai_chn,
                                    void (*callback)(const uint8_t *data,
                                                     size_t len,
                                                     int64_t timestamp_us,
                                                     void *userdata),
                                    void *userdata);

  /* Unregister a previously registered audio tap. */
  void (*unregister_audio_tap_fn)(prudynt_ctx_t *ctx, int ai_chn,
                                  uint64_t tap_id);

  /* Request the main loop to restart RTSP (e.g., after config change). */
  void (*request_rtsp_restart)(prudynt_ctx_t *ctx);

  /* Request the main loop to restart video streams. */
  void (*request_video_restart)(prudynt_ctx_t *ctx);

  /* Request the main loop to restart audio. */
  void (*request_audio_restart)(prudynt_ctx_t *ctx);

  /* Trigger a graceful shutdown. */
  void (*request_shutdown)(prudynt_ctx_t *ctx);
} prudynt_host_services_t;

/* ── Plugin descriptor ───────────────────────────────────────────────────────*/

/* Each plugin exports a prudynt_plugin_t struct via prudynt_plugin_init(). */
typedef struct {
  /* Plugin metadata */
  const char *name;
  const char *version;
  const char *description;

  /* Lifecycle: called once when the plugin is loaded.
   * Return 0 on success, non-zero to reject loading. */
  int (*init)(prudynt_ctx_t *ctx, const prudynt_host_services_t *svc);

  /* Start: called when streams are started (after each config load/restart).
   * Return 0 on success. */
  int (*start)(prudynt_ctx_t *ctx);

  /* Stop: called when streams are being torn down (before restart or shutdown). */
  void (*stop)(prudynt_ctx_t *ctx);

  /* Destroy: called just before the plugin is unloaded (dlclose). */
  void (*destroy)(prudynt_ctx_t *ctx);
} prudynt_plugin_t;

/* ── Plugin entry point ──────────────────────────────────────────────────────*/

/* Every plugin shared library must export this function.
 * It returns a pointer to a static prudynt_plugin_t descriptor. */
#define PRUDYNT_PLUGIN_EXPORT __attribute__((visibility("default")))

typedef prudynt_plugin_t *(*prudynt_plugin_init_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* PRUDYNT_PLUGIN_API_H */
