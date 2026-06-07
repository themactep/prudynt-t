// Example prudynt plugin — demonstrates the plugin API lifecycle.
// Build (host):
//   gcc -shared -fPIC -Isrc plugins/example/example_plugin.c -o plugins/example/example.so -Wall
// Build (cross):
//   ${CROSS_COMPILE}gcc -shared -fPIC -Isrc plugins/example/example_plugin.c -o plugins/example/example.so -Wall

#include "../../src/plugin_api.h"

static prudynt_ctx_t *g_ctx = NULL;
static const prudynt_host_services_t *g_svc = NULL;

static int example_init(prudynt_ctx_t *ctx,
                         const prudynt_host_services_t *svc) {
  g_ctx = ctx;
  g_svc = svc;
  svc->log(PRUDYNT_LOG_INFO, "example", "plugin initialised");
  return 0;
}

static int example_start(prudynt_ctx_t *ctx) {
  (void)ctx;
  g_svc->log(PRUDYNT_LOG_INFO, "example", "plugin started");

  // Read a config value to prove config access works
  const char *loglevel = g_svc->config_get_string(g_ctx, "general.loglevel");
  g_svc->log(PRUDYNT_LOG_INFO, "example",
             "current log level: %s", loglevel ? loglevel : "(unknown)");

  return 0;
}

static void example_stop(prudynt_ctx_t *ctx) {
  (void)ctx;
  g_svc->log(PRUDYNT_LOG_INFO, "example", "plugin stopped");
}

static void example_destroy(prudynt_ctx_t *ctx) {
  (void)ctx;
  g_svc->log(PRUDYNT_LOG_INFO, "example", "plugin destroyed");
  g_ctx = NULL;
  g_svc = NULL;
}

static prudynt_plugin_t g_plugin = {
    .name = "example",
    .version = "1.0.0",
    .description = "Example plugin demonstrating the prudynt plugin API",
    .init = example_init,
    .start = example_start,
    .stop = example_stop,
    .destroy = example_destroy,
};

PRUDYNT_PLUGIN_EXPORT prudynt_plugin_t *prudynt_plugin_init(void) {
  return &g_plugin;
}
