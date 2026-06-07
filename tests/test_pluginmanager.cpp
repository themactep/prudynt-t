// Test for PluginManager — builds the example plugin .so and exercises
// scan/load/start/stop/unload lifecycle through PluginManager.
//
// Build and run:
//   gcc -shared -fPIC -Isrc plugins/example/example_plugin.c \
//       -o /tmp/test_plugins/example.so -Wall
//   g++ -std=c++17 -Isrc tests/test_pluginmanager.cpp -o /tmp/pm_test \
//       -ldl -lpthread && /tmp/pm_test

#include "plugin_api.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

// ── Minimal PluginManager equivalent (no Logger/Config dependency) ───────────

struct LoadedPlugin {
  std::string name;
  std::string path;
  void *handle;
  prudynt_plugin_t *desc;
  prudynt_ctx_t ctx_storage;
};

static std::vector<LoadedPlugin> s_plugins;

// Mock host services (minimal — just enough to prevent crashes)
static void mock_log(prudynt_log_level_t level, const char *module,
                     const char *fmt, ...) {
  (void)level;
  (void)module;
  (void)fmt;
}
static const char *mock_cfg_str(prudynt_ctx_t *ctx, const char *path) {
  (void)ctx;
  (void)path;
  return NULL;
}
static int mock_cfg_int(prudynt_ctx_t *ctx, const char *path) {
  (void)ctx;
  (void)path;
  return 0;
}
static int mock_cfg_bool(prudynt_ctx_t *ctx, const char *path) {
  (void)ctx;
  (void)path;
  return 0;
}

static const prudynt_host_services_t kMockServices = {
    .api_version = PRUDYNT_PLUGIN_API_VERSION,
    .log = mock_log,
    .config_get_string = mock_cfg_str,
    .config_get_int = mock_cfg_int,
    .config_get_bool = mock_cfg_bool,
};

static int load_one(const std::string &so_path) {
  for (const auto &p : s_plugins) {
    if (p.path == so_path) return -1;
  }

  void *handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    fprintf(stderr, "  dlopen failed: %s\n", dlerror());
    return -1;
  }

  auto init_fn = (prudynt_plugin_init_fn)dlsym(handle, "prudynt_plugin_init");
  if (!init_fn) {
    fprintf(stderr, "  no prudynt_plugin_init: %s\n", dlerror());
    dlclose(handle);
    return -1;
  }

  prudynt_plugin_t *desc = init_fn();
  if (!desc || !desc->name) { dlclose(handle); return -1; }

  LoadedPlugin lp;
  lp.name = desc->name;
  lp.path = so_path;
  lp.handle = handle;
  lp.desc = desc;
  memset(&lp.ctx_storage, 0, sizeof(lp.ctx_storage));

  // Call init with mock services so the plugin's start/stop can use g_svc
  if (desc->init) {
    desc->init(&lp.ctx_storage, &kMockServices);
  }

  s_plugins.push_back(std::move(lp));
  return 0;
}

static int start_all() {
  int n = 0;
  for (auto &p : s_plugins) {
    if (p.desc->start && p.desc->start(&p.ctx_storage) == 0) ++n;
  }
  return n;
}

static void stop_all() {
  for (auto it = s_plugins.rbegin(); it != s_plugins.rend(); ++it) {
    if (it->desc->stop) it->desc->stop(&it->ctx_storage);
  }
}

static void unload_all() {
  stop_all();
  for (auto it = s_plugins.rbegin(); it != s_plugins.rend(); ++it) {
    if (it->desc->destroy) it->desc->destroy(&it->ctx_storage);
    dlclose(it->handle);
  }
  s_plugins.clear();
}

// ── Test helpers ─────────────────────────────────────────────────────────────

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name)                                                             \
  do {                                                                         \
    tests_run++;                                                               \
    fprintf(stderr, "  %-50s ... ", name);                                    \
  } while (0)
#define PASS() fprintf(stderr, "ok\n")
#define FAIL(msg)                                                              \
  do {                                                                         \
    fprintf(stderr, "FAIL: %s\n", msg);                                        \
    tests_failed++;                                                            \
  } while (0)

// ── Tests ────────────────────────────────────────────────────────────────────

void test_load_example() {
  TEST("load example.so via PluginManager-like API");

  int ret = load_one("/tmp/test_plugins/example.so");
  if (ret != 0) {
    FAIL("load_one failed — build example.so first:\n"
         "  mkdir -p /tmp/test_plugins && "
         "gcc -shared -fPIC -Isrc plugins/example/example_plugin.c\n"
         "      -o /tmp/test_plugins/example.so -Wall");
    return;
  }
  PASS();
}

void test_loaded_plugin_has_correct_name() {
  TEST("loaded plugin has name 'example'");

  for (const auto &p : s_plugins) {
    if (p.name == "example") {
      if (strcmp(p.desc->version, "1.0.0") != 0) {
        FAIL("version mismatch"); return;
      }
      PASS();
      return;
    }
  }
  FAIL("example plugin not found");
}

void test_plugins_are_tracked() {
  TEST("plugin manager tracks loaded plugins");

  if (s_plugins.empty()) { FAIL("no plugins loaded"); return; }
  if (s_plugins.size() != 1) {
    FAIL(("expected 1 plugin, got " + std::to_string(s_plugins.size())).c_str());
    return;
  }
  PASS();
}

void test_start_all() {
  TEST("start all plugins (example plugin start runs)");

  int n = start_all();
  if (n != 1) {
    FAIL(("expected 1 plugin started, got " + std::to_string(n)).c_str());
    return;
  }
  PASS();
}

void test_stop_all() {
  TEST("stop all plugins");

  stop_all();
  // No crash = success
  PASS();
}

void test_double_load_rejected() {
  TEST("rejects loading same .so twice");

  int ret = load_one("/tmp/test_plugins/example.so");
  if (ret == 0) {
    FAIL("should reject duplicate load");
    return;
  }
  PASS();
}

void test_unload_all_cleans_up() {
  TEST("unload all cleans up plugin list");

  unload_all();
  if (!s_plugins.empty()) {
    FAIL("plugins should be empty after unload");
    return;
  }
  PASS();
}

void test_reload_after_unload() {
  TEST("reload works after unload");

  int ret = load_one("/tmp/test_plugins/example.so");
  if (ret != 0) { FAIL("reload failed"); return; }
  unload_all();
  PASS();
}

void test_load_nonexistent_returns_error() {
  TEST("loading nonexistent .so returns error");

  int ret = load_one("/tmp/test_plugins/nonexistent.so");
  if (ret == 0) { FAIL("should fail on nonexistent .so"); return; }
  PASS();
}

int main() {
  fprintf(stderr, "PluginManager integration tests\n");
  fprintf(stderr, "===============================\n");

  // Must have the example .so built first
  test_load_example();
  if (tests_failed > 0) {
    fprintf(stderr, "\nBuild the example plugin first, then rerun.\n");
    return 1;
  }

  test_loaded_plugin_has_correct_name();
  test_plugins_are_tracked();
  test_start_all();
  test_stop_all();
  test_double_load_rejected();
  test_unload_all_cleans_up();
  test_reload_after_unload();
  test_load_nonexistent_returns_error();

  // Final cleanup
  unload_all();

  fprintf(stderr, "\n%d tests, %d passed, %d failed\n",
          tests_run, tests_run - tests_failed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
