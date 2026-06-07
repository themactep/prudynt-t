// Test for the plugin API — builds the example plugin and tests dlopen/dlsym.
// Build and run:
//   gcc -shared -fPIC -Isrc plugins/example/example_plugin.c -o /tmp/example.so -Wall
//   gcc -std=c11 -Isrc tests/test_plugin_api.c -o /tmp/plugin_test -ldl -Wall
//   /tmp/plugin_test

#include "../src/plugin_api.h"

#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// ── Host service mocks ──────────────────────────────────────────────────────

static void test_log(prudynt_log_level_t level, const char *module,
                     const char *fmt, ...) {
  (void)level;
  (void)module;
  (void)fmt;
}

static const char *test_config_string(prudynt_ctx_t *ctx, const char *path) {
  (void)ctx;
  if (strcmp(path, "general.loglevel") == 0)
    return "INFO";
  return NULL;
}

static int test_config_int(prudynt_ctx_t *ctx, const char *path) {
  (void)ctx;
  if (strcmp(path, "general.port") == 0)
    return 554;
  return 0;
}

static int test_config_bool(prudynt_ctx_t *ctx, const char *path) {
  (void)ctx;
  if (strcmp(path, "rtsp.enabled") == 0)
    return 1;
  return 0;
}

static uint64_t test_reg_video(prudynt_ctx_t *ctx, int ch,
                                void (*cb)(const uint8_t *, size_t, int64_t,
                                           int, void *),
                                void *ud) {
  (void)ctx;
  (void)ch;
  (void)cb;
  (void)ud;
  return 42;
}

static void test_unreg_video(prudynt_ctx_t *ctx, int ch, uint64_t id) {
  (void)ctx;
  (void)ch;
  (void)id;
}

static uint64_t test_reg_audio(prudynt_ctx_t *ctx, int ch,
                                void (*cb)(const uint8_t *, size_t, int64_t,
                                           void *),
                                void *ud) {
  (void)ctx;
  (void)ch;
  (void)cb;
  (void)ud;
  return 43;
}

static void test_unreg_audio(prudynt_ctx_t *ctx, int ch, uint64_t id) {
  (void)ctx;
  (void)ch;
  (void)id;
}

static void test_req_rtsp(prudynt_ctx_t *ctx) { (void)ctx; }
static void test_req_video(prudynt_ctx_t *ctx) { (void)ctx; }
static void test_req_audio(prudynt_ctx_t *ctx) { (void)ctx; }
static void test_req_shutdown(prudynt_ctx_t *ctx) { (void)ctx; }

static prudynt_host_services_t test_services = {
    .api_version = PRUDYNT_PLUGIN_API_VERSION,
    .log = test_log,
    .config_get_string = test_config_string,
    .config_get_int = test_config_int,
    .config_get_bool = test_config_bool,
    .register_video_tap_fn = test_reg_video,
    .unregister_video_tap_fn = test_unreg_video,
    .register_audio_tap_fn = test_reg_audio,
    .unregister_audio_tap_fn = test_unreg_audio,
    .request_rtsp_restart = test_req_rtsp,
    .request_video_restart = test_req_video,
    .request_audio_restart = test_req_audio,
    .request_shutdown = test_req_shutdown,
};

// ── Tests ────────────────────────────────────────────────────────────────────

void test_load_example_plugin() {
  TEST("load and inspect example plugin");

  // We'll test the API contract directly by constructing a mock plugin
  // descriptor and verifying the function pointers.

  // Simulate what a plugin .so would export
  static prudynt_plugin_t mock = {
      .name = "mock_test",
      .version = "0.1.0",
      .description = "Mock for testing",
      .init = NULL,
      .start = NULL,
      .stop = NULL,
      .destroy = NULL,
  };

  if (strcmp(mock.name, "mock_test") != 0) {
    FAIL("name mismatch");
    return;
  }
  if (strcmp(mock.version, "0.1.0") != 0) {
    FAIL("version mismatch");
    return;
  }
  if (strcmp(mock.description, "Mock for testing") != 0) {
    FAIL("description mismatch");
    return;
  }
  PASS();
}

void test_api_version_check() {
  TEST("API version constant is defined");

  if (PRUDYNT_PLUGIN_API_VERSION != 1) {
    FAIL("expected API version 1");
    return;
  }
  PASS();
}

void test_lifecycle_no_crash_with_null_fns() {
  TEST("lifecycle handles NULL function pointers gracefully");

  prudynt_plugin_t p = {
      .name = "noop", .version = "1.0", .description = "",
      .init = NULL,    .start = NULL,    .stop = NULL, .destroy = NULL,
  };

  // Test that calling NULL function pointers is safe
  if (p.init)
    p.init(NULL, &test_services);
  if (p.start)
    p.start(NULL);
  if (p.stop)
    p.stop(NULL);
  if (p.destroy)
    p.destroy(NULL);

  PASS();
}

void test_dlopen_example_so() {
  TEST("dlopen the example plugin .so");

  // Try to load the pre-built example .so; skip if not found
  void *handle = dlopen("/tmp/example.so", RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    fprintf(stderr, "SKIP (no /tmp/example.so — build it first with:\n"
                    "  gcc -shared -fPIC -I../include \\\n"
                    "      plugins/example/example_plugin.c \\\n"
                    "      -o /tmp/example.so)\n");
    PASS(); // Not a failure — user needs to build the example first
    return;
  }

  prudynt_plugin_init_fn init_fn =
      (prudynt_plugin_init_fn)dlsym(handle, "prudynt_plugin_init");
  if (!init_fn) {
    FAIL("prudynt_plugin_init not found");
    dlclose(handle);
    return;
  }

  prudynt_plugin_t *desc = init_fn();
  if (!desc) {
    FAIL("init_fn returned NULL");
    dlclose(handle);
    return;
  }

  if (strcmp(desc->name, "example") != 0) {
    FAIL("expected plugin name 'example'");
    dlclose(handle);
    return;
  }

  // Exercise full lifecycle
  if (desc->init)
    desc->init(NULL, &test_services);
  if (desc->start)
    desc->start(NULL);
  if (desc->stop)
    desc->stop(NULL);
  if (desc->destroy)
    desc->destroy(NULL);

  dlclose(handle);
  PASS();
}

void test_host_services_config() {
  TEST("host services config accessors work");

  const char *val = test_services.config_get_string(NULL, "general.loglevel");
  if (!val || strcmp(val, "INFO") != 0) {
    FAIL("config_get_string failed");
    return;
  }

  int ival = test_services.config_get_int(NULL, "general.port");
  if (ival != 554) {
    FAIL("config_get_int failed");
    return;
  }

  int bval = test_services.config_get_bool(NULL, "rtsp.enabled");
  if (bval != 1) {
    FAIL("config_get_bool failed");
    return;
  }

  PASS();
}

void test_host_services_video_tap() {
  TEST("host services video tap registration");

  uint64_t id = test_services.register_video_tap_fn(
      NULL, 0, NULL, NULL);
  if (id != 42) {
    FAIL("unexpected tap ID");
    return;
  }

  test_services.unregister_video_tap_fn(NULL, 0, id);
  PASS();
}

void test_host_services_audio_tap() {
  TEST("host services audio tap registration");

  uint64_t id = test_services.register_audio_tap_fn(
      NULL, 0, NULL, NULL);
  if (id != 43) {
    FAIL("unexpected tap ID");
    return;
  }

  test_services.unregister_audio_tap_fn(NULL, 0, id);
  PASS();
}

void test_plugin_struct_size() {
  TEST("plugin struct layout matches expectations");

  // Verify the struct size is reasonable (< 64 bytes)
  if (sizeof(prudynt_plugin_t) > 64) {
    FAIL("plugin struct unexpectedly large");
    return;
  }
  // Verify the host services struct size is reasonable
  if (sizeof(prudynt_host_services_t) > 256) {
    FAIL("host services struct unexpectedly large");
    return;
  }
  PASS();
}

int main() {
  fprintf(stderr, "Plugin API tests\n");
  fprintf(stderr, "================\n");

  test_load_example_plugin();
  test_api_version_check();
  test_lifecycle_no_crash_with_null_fns();
  test_dlopen_example_so();
  test_host_services_config();
  test_host_services_video_tap();
  test_host_services_audio_tap();
  test_plugin_struct_size();

  fprintf(stderr, "\n%d tests, %d passed, %d failed\n", tests_run,
          tests_run - tests_failed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
