#include "DayNightWorker.hpp"

#include "Config.hpp"
#include "DayNightAlgo.hpp"
#include "Logger.hpp"
#include "globals.hpp"
#include "imp_hal.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace std::chrono;

namespace DayNightWorkerNS {

constexpr const char *kPrudyntRunDir = "/run/prudynt";
constexpr const char *kBrightnessPath = "/run/prudynt/daynight_brightness";
constexpr const char *kModePath = "/run/prudynt/daynight_mode";

struct Profile {
  int EVmin;
  int EVmax;
  int gb_delta;
  int gb_abs;
  int base_night;
  int base_day;
  int settle;
  int sec_margin_percent; // adds to day_above for secondary EV gate
};

static Logger::Level daynight_log_level = Logger::INFO;
static std::string daynight_log_level_label = "INFO";

static const char *select_daynight_log_level_string() {
  if (cfg && cfg->daynight.loglevel && cfg->daynight.loglevel[0] != '\0') {
    return cfg->daynight.loglevel;
  }
  if (cfg && cfg->general.loglevel && cfg->general.loglevel[0] != '\0') {
    return cfg->general.loglevel;
  }
  return "INFO";
}

static void refresh_daynight_log_level() {
  const char *selected = select_daynight_log_level_string();
  if (!selected || selected[0] == '\0') {
    selected = "INFO";
  }
  if (daynight_log_level_label != selected) {
    daynight_log_level_label = selected;
    daynight_log_level = Logger::parseLevel(daynight_log_level_label);
  }
}

static bool daynight_should_log(Logger::Level lvl) {
  return daynight_log_level >= lvl;
}

static Profile get_profile() {
  Profile p{};
  // Baseline defaults
  p.gb_delta = 15;
  p.gb_abs = 145;
  p.base_night = 6;
  p.base_day = 4;
  p.settle = 20;
  p.sec_margin_percent = 5;
#if defined(PLATFORM_T23)
  // T23 mapping matching original script thresholds
  p.EVmin = 42857;
  p.EVmax = 2227731;
#elif defined(PLATFORM_T31)
  // T31 observed EV range appears in tens of thousands; map to ~0..60k
  p.EVmin = 0;
  p.EVmax = 60000;
#elif defined(PLATFORM_T21) || defined(PLATFORM_T30)
  // T21/T30 observed EV range is much lower (~0..50k). Map accordingly.
  p.EVmin = 0;
  p.EVmax = 50000;
#else
  // Conservative fallback
  p.EVmin = 40000;
  p.EVmax = 2400000;
#endif
  return p;
}

static inline int clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static inline int ev_from_percent(const Profile &pr, int pct) {
  pct = clampi(pct, 0, 100);
  long long range = static_cast<long long>(pr.EVmax) - static_cast<long long>(pr.EVmin);
  long long ev = static_cast<long long>(pr.EVmax) - (range * pct) / 100LL;
  return static_cast<int>(ev);
}

static inline int percent_from_ev(const Profile &pr, int ev) {
  long long range = static_cast<long long>(pr.EVmax) - static_cast<long long>(pr.EVmin);
  if (range <= 0)
    return 0;
  long long num = (static_cast<long long>(pr.EVmax) - static_cast<long long>(ev)) * 100LL;
  int pct = static_cast<int>(num / range);
  return clampi(pct, 0, 100);
}

static inline int brightness_percent_from_ev(const Profile &pr,
                                             const DayNightAlgo::Params &params,
                                             int ev) {
  if (ev < 0)
    return -1;

  long long dark_ev = params.ev_night_high;
  long long bright_ev = params.ev_day_low_primary;

  if (dark_ev > bright_ev) {
    // Map EV to 0-100% with extended range to avoid always hitting 100%
    // Use a wider range: from 50% below bright_ev to 50% above dark_ev
    long long nominal_range = dark_ev - bright_ev;
    long long bright_extended = bright_ev - (nominal_range / 2);
    long long dark_extended = dark_ev + (nominal_range / 2);
    long long extended_range = dark_extended - bright_extended; // Double the range

    if (extended_range > 0) {
      long long num = (static_cast<long long>(ev) - bright_extended) * 100LL;
      int pct = static_cast<int>(num / extended_range);
      return clampi(pct, 0, 100);
    }
  }

  return percent_from_ev(pr, ev);
}

static void export_brightness_value(int pct, const char *mode) {
  static int last_written_pct = std::numeric_limits<int>::min();
  static std::string last_written_mode;

  const char *mode_str = (mode && mode[0] != '\0') ? mode : "unknown";

  if (pct == last_written_pct && last_written_mode == mode_str)
    return;

  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(kPrudyntRunDir, ec);
  if (ec && daynight_should_log(Logger::DEBUG)) {
    LOG_DEBUG("DayNight: failed to create run directory " << kPrudyntRunDir << ": " << ec.message());
  }

  if (pct < 0) {
    if (::unlink(kBrightnessPath) != 0 && errno != ENOENT && daynight_should_log(Logger::DEBUG)) {
      LOG_DEBUG("DayNight: failed to unlink " << kBrightnessPath << ": " << strerror(errno));
    }
    if (::unlink(kModePath) != 0 && errno != ENOENT && daynight_should_log(Logger::DEBUG)) {
      LOG_DEBUG("DayNight: failed to unlink " << kModePath << ": " << strerror(errno));
    }
    last_written_pct = pct;
    last_written_mode.clear();
    return;
  }

  char buf[16];
  int len = std::snprintf(buf, sizeof(buf), "%d\n", pct);
  if (len < 0)
    len = 0;

  int fd = ::open(kBrightnessPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    if (daynight_should_log(Logger::DEBUG)) {
      LOG_DEBUG("DayNight: failed to open " << kBrightnessPath << ": " << strerror(errno));
    }
  } else {
    ssize_t written = ::write(fd, buf, len);
    if (written != len && daynight_should_log(Logger::DEBUG)) {
      LOG_DEBUG("DayNight: short write to " << kBrightnessPath << ": " << strerror(errno));
    }
    ::close(fd);
  }

  int mode_fd = ::open(kModePath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (mode_fd < 0) {
    if (daynight_should_log(Logger::DEBUG)) {
      LOG_DEBUG("DayNight: failed to open " << kModePath << ": " << strerror(errno));
    }
  } else {
    size_t mode_len = std::strlen(mode_str);
    if (::write(mode_fd, mode_str, mode_len) != static_cast<ssize_t>(mode_len) &&
        daynight_should_log(Logger::DEBUG)) {
      LOG_DEBUG("DayNight: short write to " << kModePath << ": " << strerror(errno));
    }
    if (::write(mode_fd, "\n", 1) != 1 && daynight_should_log(Logger::DEBUG)) {
      LOG_DEBUG("DayNight: failed to terminate " << kModePath << ": " << strerror(errno));
    }
    ::close(mode_fd);
  }

  last_written_pct = pct;
  last_written_mode = mode_str;
}

// Helper to check if current time is within the photosensing schedule
static bool is_within_schedule() {
  if (!cfg || !cfg->daynight.schedule.enabled) {
    return true; // Schedule disabled, always active
  }

  const char *start_at = cfg->daynight.schedule.start_at;
  const char *stop_at = cfg->daynight.schedule.stop_at;

  if (!start_at || !stop_at || std::strlen(start_at) == 0 || std::strlen(stop_at) == 0) {
    return true; // No times configured, always active
  }

  // Parse HH:MM format
  int start_hour = 0, start_min = 0, stop_hour = 0, stop_min = 0;
  if (std::sscanf(start_at, "%d:%d", &start_hour, &start_min) != 2 ||
      std::sscanf(stop_at, "%d:%d", &stop_hour, &stop_min) != 2) {
    return true; // Invalid format, always active
  }

  // Get current time
  std::time_t now = std::time(nullptr);
  std::tm *local_time = std::localtime(&now);
  int current_hour = local_time->tm_hour;
  int current_min = local_time->tm_min;

  // Convert to minutes since midnight for easier comparison
  int start_mins = start_hour * 60 + start_min;
  int stop_mins = stop_hour * 60 + stop_min;
  int current_mins = current_hour * 60 + current_min;

  // Handle overnight schedules (e.g., 22:00 to 06:00)
  if (start_mins <= stop_mins) {
    // Same day schedule (e.g., 06:00 to 22:00)
    return current_mins >= start_mins && current_mins < stop_mins;
  } else {
    // Overnight schedule (e.g., 22:00 to 06:00)
    return current_mins >= start_mins || current_mins < stop_mins;
  }
}

static int read_ev(int &out_ev) {
  return hal::isp::get_ev(out_ev);
}

static int read_awb(int &out_gr, int &out_gb) {
  out_gr = -1;
  out_gb = -1;
  return hal::isp::get_awb_weighted_gains(out_gr, out_gb);
}

static void apply_mode(DayNightAlgo::Mode m) {
  const char *script_cfg = cfg->get<const char *>("daynight.script_path");
  const char *script = (script_cfg && std::strlen(script_cfg) > 0) ? script_cfg : "/sbin/daynight";

  if (m == DayNightAlgo::Mode::Day) {
    // Switch to day bin if configured and enabled
    if (cfg->daynight.controls.binswitch) {
      const char *day_bin = cfg->get<const char *>("daynight.day_bin_path");
      if (day_bin && day_bin[0] != '\0') {
        int bin_ret = hal::isp::switch_bin(day_bin);
        if (bin_ret != 0 && daynight_should_log(Logger::WARN)) {
          LOG_WARN("Failed to switch to day bin '" << day_bin << "': " << bin_ret);
        } else if (bin_ret == 0 && daynight_should_log(Logger::INFO)) {
          LOG_INFO("Switched to day IQ bin: " << day_bin);
        }
      }
    }

    // Switch ISP running mode only if color control is enabled
    if (cfg->daynight.controls.color) {
      int ret = hal::isp::set_running_mode(hal::isp::RunningMode::Day);
      if (ret != 0) {
        if (daynight_should_log(Logger::WARN)) {
          LOG_WARN("SetISPRunningMode(DAY) failed: " << ret);
        }
      }
    }
    std::string cmd = std::string(script) + " day";
    (void)std::system(cmd.c_str());
  } else if (m == DayNightAlgo::Mode::Night) {
    // Switch to night bin if configured and enabled
    if (cfg->daynight.controls.binswitch) {
      const char *night_bin = cfg->get<const char *>("daynight.night_bin_path");
      if (night_bin && night_bin[0] != '\0') {
        int bin_ret = hal::isp::switch_bin(night_bin);
        if (bin_ret != 0 && daynight_should_log(Logger::WARN)) {
          LOG_WARN("Failed to switch to night bin '" << night_bin << "': " << bin_ret);
        } else if (bin_ret == 0 && daynight_should_log(Logger::INFO)) {
          LOG_INFO("Switched to night IQ bin: " << night_bin);
        }
      }
    }

    // Switch ISP running mode only if color control is enabled
    if (cfg->daynight.controls.color) {
      int ret = hal::isp::set_running_mode(hal::isp::RunningMode::Night);
      if (ret != 0) {
        if (daynight_should_log(Logger::WARN)) {
          LOG_WARN("SetISPRunningMode(NIGHT) failed: " << ret);
        }
      }
    }
    std::string cmd = std::string(script) + " night";
    (void)std::system(cmd.c_str());
  }
}

/* LEGACY - not used by simple algorithm
static DayNightAlgo::Mode configured_running_mode() {
  if (!cfg)
    return DayNightAlgo::Mode::Unknown;

  int configured = cfg->image.running_mode;
  if (configured == static_cast<int>(hal::isp::RunningMode::Day))
    return DayNightAlgo::Mode::Day;
  if (configured == static_cast<int>(hal::isp::RunningMode::Night))
    return DayNightAlgo::Mode::Night;
  return DayNightAlgo::Mode::Unknown;
}

static DayNightAlgo::Mode infer_initial_mode(const DayNightAlgo::Params &params,
                                             const DayNightAlgo::Signals &sig) {
  if (sig.ev >= 0) {
    if (sig.ev > params.ev_night_high)
      return DayNightAlgo::Mode::Night;
    if (sig.ev < params.ev_day_low_primary)
      return DayNightAlgo::Mode::Day;
  }
  DayNightAlgo::Mode cfg_mode = configured_running_mode();
  if (cfg_mode != DayNightAlgo::Mode::Unknown)
    return cfg_mode;
  return DayNightAlgo::Mode::Day;
}
*/

void *thread_entry(void *arg) {
  (void)arg;
  refresh_daynight_log_level();
  if (daynight_should_log(Logger::INFO)) {
    LOG_INFO("DayNightWorker: starting (SIMPLE total_gain algorithm, loglevel=" << daynight_log_level_label << ")");
  }

  // ============================================================================
  // LEGACY ALGORITHM - NOT CURRENTLY USED
  // ============================================================================
  // The original algorithm below is disabled because:
  // 1. GB/GR gains are always 0 on T23 platforms (ISP limitation)
  // 2. EV thresholds are miscalibrated (expects 42k-2.2M range, actual is 1k-30k)
  // 3. Brightness percentage calculation is broken due to wrong EV ranges
  //
  // This code is preserved for reference but not executed.
  // ============================================================================
  /*
  DayNightAlgo::Params params{};
  DayNightAlgo::State state{};
  DayNightAlgo::init(state);

  // Skip automatic switching for N iterations after forced mode
  int skip_auto_switch_iterations = 0;

  // Anti-flapping: prevent rapid mode changes
  int anti_flap_cooldown = 0;
  const int anti_flap_iterations = 30; // ~30 seconds minimum between automatic switches

  // SoC profile mapping
  Profile pr = get_profile();

  // Load config thresholds. Prefer explicit EV thresholds if configured;
  // otherwise use percent mapping.
  int ev_night_high_cfg = cfg->get<int>("daynight.ev_night_high");
  int ev_day_low_primary_cfg = cfg->get<int>("daynight.ev_day_low_primary");
  int ev_day_low_secondary_cfg = cfg->get<int>("daynight.ev_day_low_secondary");

  int below_pct = cfg->get<int>("daynight.switch_below_percent");
  int above_pct = cfg->get<int>("daynight.switch_above_percent");
  int tol_pct = cfg->get<int>("daynight.tolerance_percent");

  if (ev_night_high_cfg > 0 && ev_day_low_primary_cfg > 0) {
    params.ev_night_high = ev_night_high_cfg;
    params.ev_day_low_primary = ev_day_low_primary_cfg;
    // default to primary if secondary not set
    params.ev_day_low_secondary =
        (ev_day_low_secondary_cfg > 0) ? ev_day_low_secondary_cfg : ev_day_low_primary_cfg;
    // derive counters from tolerance percent if provided
    params.night_count_threshold = pr.base_night + (tol_pct + 24) / 25;
    params.day_count_threshold = pr.base_day + (tol_pct + 24) / 25;
  } else {
    // Percent-based mapping fallback
    params.ev_night_high = ev_from_percent(pr, below_pct);
    params.ev_day_low_primary = ev_from_percent(pr, above_pct);
    int secondary_pct = clampi(above_pct + pr.sec_margin_percent, 0, 100);
    params.ev_day_low_secondary = ev_from_percent(pr, secondary_pct);
    params.night_count_threshold = pr.base_night + (tol_pct + 24) / 25; // +1 per 25%
    params.day_count_threshold = pr.base_day + (tol_pct + 24) / 25;
  }

  params.gb_gain_delta = pr.gb_delta;
  params.gb_gain_absolute = pr.gb_abs;
  params.settle_samples_for_gb_record = pr.settle;

  int interval_ms = cfg->get<int>("daynight.sample_interval_ms");
  if (interval_ms <= 0)
    interval_ms = 1000;

  // If we start while already in night, capture initial GB/GR minima window
  state.settle_remaining = params.settle_samples_for_gb_record;

  DayNightAlgo::Mode current = DayNightAlgo::Mode::Unknown;
  bool initial_mode_applied = false;

  while (!global_shutdown_requested.load(std::memory_order_relaxed)) {
    refresh_daynight_log_level();

    // Check for manual force_mode override
    const char *force_mode_str = cfg->daynight.force_mode.load(std::memory_order_relaxed);
    if (force_mode_str != nullptr) {
      DayNightAlgo::Mode forced_mode = DayNightAlgo::Mode::Unknown;
      if (std::strcmp(force_mode_str, "day") == 0) {
        forced_mode = DayNightAlgo::Mode::Day;
      } else if (std::strcmp(force_mode_str, "night") == 0) {
        forced_mode = DayNightAlgo::Mode::Night;
      }

      if (forced_mode != DayNightAlgo::Mode::Unknown && forced_mode != current) {
        if (daynight_should_log(Logger::INFO)) {
          LOG_INFO("DayNight: applying forced mode " << force_mode_str);
        }
        apply_mode(forced_mode);
        current = forced_mode;
        if (current == DayNightAlgo::Mode::Night)
          DayNightAlgo::on_enter_night(params, state);
        else if (current == DayNightAlgo::Mode::Day)
          DayNightAlgo::on_enter_day(state);
        cfg->daynight.live_mode.store(force_mode_str, std::memory_order_relaxed);
        initial_mode_applied = true;
        // Skip automatic algorithm for next ~10 seconds to let forced mode stick
        skip_auto_switch_iterations = 10;
      }
      // Clear the force flag after applying
      cfg->daynight.force_mode.store(nullptr, std::memory_order_relaxed);
    }

    int ev = -1, gr = -1, gb = -1;
    (void)read_ev(ev);
    (void)read_awb(gr, gb);

    // Read additional pure ISP sensor values for analysis
    int total_gain = -1, ae_luma = -1, awb_ct = -1;
    (void)hal::isp::get_total_gain(total_gain);
    (void)hal::isp::get_ae_luma(ae_luma);
    (void)hal::isp::get_awb_color_temp(awb_ct);

    DayNightAlgo::Signals sig{ev, gb, gr};
    DayNightAlgo::update_minima_window(state, sig);
    auto dec = DayNightAlgo::decide(params, state, sig);

    // Live status update for metrics - export ALL raw sensor values
    int bright_pct = brightness_percent_from_ev(pr, params, ev);
    cfg->daynight.live_brightness_percent.store(bright_pct, std::memory_order_relaxed);
    cfg->daynight.live_ev.store(ev, std::memory_order_relaxed);
    cfg->daynight.live_gb.store(gb, std::memory_order_relaxed);
    cfg->daynight.live_gr.store(gr, std::memory_order_relaxed);
    cfg->daynight.live_total_gain.store(total_gain, std::memory_order_relaxed);
    cfg->daynight.live_ae_luma.store(ae_luma, std::memory_order_relaxed);
    cfg->daynight.live_awb_color_temp.store(awb_ct, std::memory_order_relaxed);

    if (daynight_should_log(Logger::DEBUG)) {
      LOG_DEBUG("DayNight: brightness%=" << bright_pct << " EV=" << ev << " GB=" << gb << " GR=" << gr
                                          << " TotalGain=" << total_gain << " AELuma=" << ae_luma
                                          << " recGB=" << state.gb_gain_record << " nCnt=" << state.night_count
                                          << " dCnt=" << state.day_count
                                          << " ircut=" << (state.ircut_engaged ? "1" : "0") << " -> "
                                          << (dec.toggled ? (dec.target == DayNightAlgo::Mode::Day ? "DAY" : "NIGHT")
                                                         : "HOLD"));
    }

    if (!initial_mode_applied) {
      DayNightAlgo::Mode inferred = infer_initial_mode(params, sig);
      if (inferred != DayNightAlgo::Mode::Unknown) {
        apply_mode(inferred);
        current = inferred;
        if (current == DayNightAlgo::Mode::Night)
          DayNightAlgo::on_enter_night(params, state);
        else if (current == DayNightAlgo::Mode::Day)
          DayNightAlgo::on_enter_day(state);
        cfg->daynight.live_mode.store(current == DayNightAlgo::Mode::Day ? "day" : "night",
                                      std::memory_order_relaxed);
        initial_mode_applied = true;
        if (daynight_should_log(Logger::INFO)) {
          LOG_INFO("DayNight: applied initial mode "
                   << (current == DayNightAlgo::Mode::Day ? "day" : "night"));
        }
      }
    }

    if (dec.toggled && dec.target != current) {
      // Skip automatic switching if we just forced a mode
      if (skip_auto_switch_iterations > 0) {
        if (daynight_should_log(Logger::DEBUG)) {
          LOG_DEBUG("DayNight: skipping automatic switch (forced mode cooldown: "
                    << skip_auto_switch_iterations << " iterations left)");
        }
        skip_auto_switch_iterations--;
      } else if (anti_flap_cooldown > 0) {
        // Prevent rapid toggling during twilight
        if (daynight_should_log(Logger::DEBUG)) {
          LOG_DEBUG("DayNight: skipping automatic switch (anti-flap cooldown: "
                    << anti_flap_cooldown << " iterations left)");
        }
        anti_flap_cooldown--;
      } else {
        apply_mode(dec.target);
        current = dec.target;
        if (current == DayNightAlgo::Mode::Night)
          DayNightAlgo::on_enter_night(params, state);
        else if (current == DayNightAlgo::Mode::Day)
          DayNightAlgo::on_enter_day(state);
        // reflect new mode in metrics
        cfg->daynight.live_mode.store(current == DayNightAlgo::Mode::Day ? "day" : "night",
                                      std::memory_order_relaxed);
        // Set anti-flap cooldown after successful switch
        anti_flap_cooldown = anti_flap_iterations;
        if (daynight_should_log(Logger::INFO)) {
          LOG_INFO("DayNight: automatic mode switch completed, cooldown set for "
                   << anti_flap_iterations << " iterations");
        }
      }
    } else {
      // Decrement counters even when no toggle decision is made
      if (skip_auto_switch_iterations > 0) {
        skip_auto_switch_iterations--;
      }
      if (anti_flap_cooldown > 0) {
        anti_flap_cooldown--;
      }
    }

    const char *mode_str = (current == DayNightAlgo::Mode::Day)
                   ? "day"
                   : (current == DayNightAlgo::Mode::Night ? "night" : "unknown");
    cfg->daynight.live_mode.store(mode_str, std::memory_order_relaxed);
    export_brightness_value(bright_pct, mode_str);

    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
  END OF LEGACY ALGORITHM */
  // ============================================================================

  // ============================================================================
  // NEW SIMPLE TOTAL GAIN ALGORITHM
  // ============================================================================
  DayNightAlgo::SimpleParams simple_params{};
  DayNightAlgo::SimpleState simple_state{};
  DayNightAlgo::simple_init(simple_state);

  // Load thresholds from config
  simple_params.total_gain_night_threshold = cfg->get<int>("daynight.total_gain_night_threshold");
  simple_params.total_gain_day_threshold = cfg->get<int>("daynight.total_gain_day_threshold");
  simple_params.night_count_threshold = cfg->get<int>("daynight.night_count_threshold");
  simple_params.day_count_threshold = cfg->get<int>("daynight.day_count_threshold");
  simple_params.ev_night_threshold = cfg->get<int>("daynight.ev_night_threshold");
  simple_params.ev_day_threshold = cfg->get<int>("daynight.ev_day_threshold");

  int interval_ms = cfg->get<int>("daynight.sample_interval_ms");
  if (interval_ms <= 0)
    interval_ms = 1000;

  DayNightAlgo::Mode current = DayNightAlgo::Mode::Unknown;
  bool initial_mode_applied = false;

  // Anti-flapping cooldown
  int anti_flap_cooldown = 0;
  const int anti_flap_iterations = 30; // ~30 seconds minimum between switches

  while (!global_shutdown_requested.load(std::memory_order_relaxed)) {
    refresh_daynight_log_level();

    // Check for manual force_mode override
    const char *force_mode_str = cfg->daynight.force_mode.load(std::memory_order_relaxed);
    if (force_mode_str != nullptr) {
      DayNightAlgo::Mode forced_mode = DayNightAlgo::Mode::Unknown;
      if (std::strcmp(force_mode_str, "day") == 0) {
        forced_mode = DayNightAlgo::Mode::Day;
      } else if (std::strcmp(force_mode_str, "night") == 0) {
        forced_mode = DayNightAlgo::Mode::Night;
      }

      if (forced_mode != DayNightAlgo::Mode::Unknown && forced_mode != current) {
        if (daynight_should_log(Logger::INFO)) {
          LOG_INFO("DayNight: applying forced mode " << force_mode_str);
        }
        apply_mode(forced_mode);
        current = forced_mode;
        simple_state.is_night = (current == DayNightAlgo::Mode::Night);
        cfg->daynight.live_mode.store(force_mode_str, std::memory_order_relaxed);
        initial_mode_applied = true;
      }
      cfg->daynight.force_mode.store(nullptr, std::memory_order_relaxed);
    }

    // Read ISP sensor values
    int ev = -1, gr = -1, gb = -1;
    (void)read_ev(ev);
    (void)read_awb(gr, gb);

    int total_gain = -1, ae_luma = -1, awb_ct = -1;
    (void)hal::isp::get_total_gain(total_gain);
    (void)hal::isp::get_ae_luma(ae_luma);
    (void)hal::isp::get_awb_color_temp(awb_ct);

    // Store all raw sensor values for web UI
    cfg->daynight.live_ev.store(ev, std::memory_order_relaxed);
    cfg->daynight.live_gb.store(gb, std::memory_order_relaxed);
    cfg->daynight.live_gr.store(gr, std::memory_order_relaxed);
    cfg->daynight.live_total_gain.store(total_gain, std::memory_order_relaxed);
    cfg->daynight.live_ae_luma.store(ae_luma, std::memory_order_relaxed);
    cfg->daynight.live_awb_color_temp.store(awb_ct, std::memory_order_relaxed);

    // Calculate brightness percentage (for display only, not used in algorithm)
    Profile pr = get_profile();
    DayNightAlgo::Params legacy_params{};
    int bright_pct = brightness_percent_from_ev(pr, legacy_params, ev);
    cfg->daynight.live_brightness_percent.store(bright_pct, std::memory_order_relaxed);

    // Run the simple algorithm using total_gain (or EV fallback for T10/T20)
    // Only run if within schedule window
    bool within_schedule = is_within_schedule();
    auto dec = DayNightAlgo::simple_decide(simple_params, simple_state, total_gain, ev);

    if (daynight_should_log(Logger::DEBUG)) {
      LOG_DEBUG("DayNight: TotalGain=" << total_gain << " EV=" << ev << " AELuma=" << ae_luma
                                       << " nCnt=" << simple_state.night_count
                                       << " dCnt=" << simple_state.day_count
                                       << " mode=" << (simple_state.is_night ? "NIGHT" : "DAY")
                                       << " schedule=" << (within_schedule ? "ACTIVE" : "INACTIVE") << " -> "
                                       << (dec.toggled ? (dec.target == DayNightAlgo::Mode::Day ? "DAY" : "NIGHT")
                                                      : "HOLD"));
    }

    // Apply initial mode if not set - infer from current sensor readings
    // IMPORTANT: This happens regardless of schedule to ensure camera starts in correct mode
    if (!initial_mode_applied) {
      DayNightAlgo::Mode initial = DayNightAlgo::Mode::Unknown;
      
      // Infer initial mode from sensor readings to avoid black screen on boot in dark conditions
      if (total_gain >= 0) {
        // Use total_gain if available
        if (total_gain > simple_params.total_gain_night_threshold) {
          initial = DayNightAlgo::Mode::Night;
        } else if (total_gain < simple_params.total_gain_day_threshold) {
          initial = DayNightAlgo::Mode::Day;
        }
      } else if (ev >= 0) {
        // Fallback to EV for platforms without total_gain
        if (ev > simple_params.ev_night_threshold) {
          initial = DayNightAlgo::Mode::Night;
        } else if (ev < simple_params.ev_day_threshold) {
          initial = DayNightAlgo::Mode::Day;
        }
      }
      
      if (initial != DayNightAlgo::Mode::Unknown) {
        apply_mode(initial);
        current = initial;
        simple_state.is_night = (current == DayNightAlgo::Mode::Night);
        cfg->daynight.live_mode.store(current == DayNightAlgo::Mode::Day ? "day" : "night",
                                      std::memory_order_relaxed);
        initial_mode_applied = true;
        if (daynight_should_log(Logger::INFO)) {
          const char *schedule_status = within_schedule ? "within schedule" : "outside schedule";
          LOG_INFO("DayNight: applied initial mode " << (current == DayNightAlgo::Mode::Day ? "day" : "night")
                   << " (total_gain=" << total_gain << ", ev=" << ev << ", " << schedule_status << ")");
        }
      }
    }

    // Handle mode switching with anti-flap cooldown
    // Schedule check ONLY applies to automatic switches, NOT to initial mode detection
    // This ensures camera starts in correct mode even when booting outside schedule window
    bool photosensing_enabled = cfg->daynight.enabled && within_schedule;
    if (dec.toggled && dec.target != current) {
      if (!cfg->daynight.enabled) {
        // Photosensing is disabled globally - skip automatic switching
        if (daynight_should_log(Logger::DEBUG)) {
          LOG_DEBUG("DayNight: skipping automatic switch (photosensing disabled)");
        }
      } else if (!within_schedule) {
        // Outside schedule window - skip automatic switching
        if (daynight_should_log(Logger::DEBUG)) {
          LOG_DEBUG("DayNight: skipping automatic switch (outside schedule window)");
        }
      } else if (anti_flap_cooldown > 0) {
        if (daynight_should_log(Logger::DEBUG)) {
          LOG_DEBUG("DayNight: skipping switch (anti-flap cooldown: " << anti_flap_cooldown << " iterations)");
        }
        anti_flap_cooldown--;
      } else {
        apply_mode(dec.target);
        current = dec.target;
        simple_state.is_night = (current == DayNightAlgo::Mode::Night);
        cfg->daynight.live_mode.store(current == DayNightAlgo::Mode::Day ? "day" : "night",
                                      std::memory_order_relaxed);
        anti_flap_cooldown = anti_flap_iterations;
        if (daynight_should_log(Logger::INFO)) {
          LOG_INFO("DayNight: switched to " << (current == DayNightAlgo::Mode::Day ? "DAY" : "NIGHT")
                   << " (total_gain=" << total_gain << ")");
        }
      }
    } else if (anti_flap_cooldown > 0) {
      anti_flap_cooldown--;
    }

    const char *mode_str = (current == DayNightAlgo::Mode::Day) ? "day" : "night";
    cfg->daynight.live_mode.store(mode_str, std::memory_order_relaxed);
    export_brightness_value(bright_pct, mode_str);

    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }

  refresh_daynight_log_level();
  if (daynight_should_log(Logger::INFO)) {
    LOG_INFO("DayNightWorker: shutting down");
  }
  export_brightness_value(-1, "unknown");
  return nullptr;
}

} // namespace DayNightWorkerNS
