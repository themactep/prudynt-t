// WebSocket plugin for prudynt — wraps the integrated WS class into the
// plugin API.  When the WS module is compiled into the main binary, this
// plugin provides lifecycle management (start/stop) via PluginManager.
//
// In the future,  WS.cpp can be extracted to this directory and compiled
// as a standalone .so that links against the prudynt plugin API.

#include "../../src/plugin_api.h"
#include "../../src/WS.hpp"
#include "../../src/globals.hpp"

#include <cstdio>
#include <cstring>

// The global WS instance is declared in main.cpp under WEBSOCKET_ENABLED.
// When this plugin is active we use it through the extern reference.
extern WS ws;

static prudynt_ctx_t *g_ctx = NULL;
static const prudynt_host_services_t *g_svc = NULL;

static int ws_init(prudynt_ctx_t *ctx, const prudynt_host_services_t *svc) {
  g_ctx = ctx;
  g_svc = svc;
  return 0;
}

static int ws_start(prudynt_ctx_t *ctx) {
  (void)ctx;
  if (!cfg || !cfg->websocket.enabled) {
    g_svc->log(PRUDYNT_LOG_INFO, "websocket", "disabled by config");
    return 0;
  }

  g_svc->log(PRUDYNT_LOG_INFO, "websocket", "starting WebSocket server");

  // Start the WS thread.  WS::run is the thread entry point that takes
  // a WS* argument (same pattern used by main.cpp line 642).
  pthread_t th;
  int ret = pthread_create(&th, nullptr, WS::run, &ws);
  if (ret != 0) {
    g_svc->log(PRUDYNT_LOG_ERROR, "websocket",
               "failed to create thread: %d", ret);
    return -1;
  }
  // Detach — the WS thread manages its own lifecycle.  The main loop
  // will join via ws.stop() + pthread_join on shutdown.
  pthread_detach(th);

  return 0;
}

static void ws_stop(prudynt_ctx_t *ctx) {
  (void)ctx;
  if (cfg && cfg->websocket.enabled) {
    g_svc->log(PRUDYNT_LOG_INFO, "websocket", "stopping");
    ws.stop();
  }
}

static void ws_destroy(prudynt_ctx_t *ctx) {
  (void)ctx;
  g_ctx = NULL;
  g_svc = NULL;
}

static prudynt_plugin_t g_ws_plugin = {
    .name = "websocket",
    .version = "1.0.0",
    .description = "WebSocket control server (libwebsockets)",
    .init = ws_init,
    .start = ws_start,
    .stop = ws_stop,
    .destroy = ws_destroy,
};

PRUDYNT_PLUGIN_EXPORT prudynt_plugin_t *prudynt_plugin_init(void) {
  return &g_ws_plugin;
}
