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
    long long range = dark_ev - bright_ev;
    long long num = (dark_ev - static_cast<long long>(ev)) * 100LL;
    int pct = static_cast<int>(num / range);
    return clampi(pct, 0, 100);
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
    int ret = hal::isp::set_running_mode(hal::isp::RunningMode::Day);
    if (ret != 0) {
      if (daynight_should_log(Logger::WARN)) {
        LOG_WARN("SetISPRunningMode(DAY) failed: " << ret);
      }
    }
    std::string cmd = std::string(script) + " day";
    (void)std::system(cmd.c_str());
  } else if (m == DayNightAlgo::Mode::Night) {
    int ret = hal::isp::set_running_mode(hal::isp::RunningMode::Night);
    if (ret != 0) {
      if (daynight_should_log(Logger::WARN)) {
        LOG_WARN("SetISPRunningMode(NIGHT) failed: " << ret);
      }
    }
    std::string cmd = std::string(script) + " night";
    (void)std::system(cmd.c_str());
  }
}

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

void *thread_entry(void *arg) {
  (void)arg;
  refresh_daynight_log_level();
  if (daynight_should_log(Logger::INFO)) {
    LOG_INFO("DayNightWorker: starting (percent-based algo, loglevel=" << daynight_log_level_label << ")");
  }

  DayNightAlgo::Params params{};
  DayNightAlgo::State state{};
  DayNightAlgo::init(state);

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
    int ev = -1, gr = -1, gb = -1;
    (void)read_ev(ev);
    (void)read_awb(gr, gb);

    DayNightAlgo::Signals sig{ev, gb, gr};
    DayNightAlgo::update_minima_window(state, sig);
    auto dec = DayNightAlgo::decide(params, state, sig);

    // Live status update for metrics
    int bright_pct = brightness_percent_from_ev(pr, params, ev);
    cfg->daynight.live_brightness_percent.store(bright_pct, std::memory_order_relaxed);
    cfg->daynight.live_ev.store(ev, std::memory_order_relaxed);
    cfg->daynight.live_gb.store(gb, std::memory_order_relaxed);
    cfg->daynight.live_gr.store(gr, std::memory_order_relaxed);

    if (daynight_should_log(Logger::DEBUG)) {
      LOG_DEBUG("DayNight: brightness%=" << bright_pct << " EV=" << ev << " GB=" << gb << " GR=" << gr
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
      apply_mode(dec.target);
      current = dec.target;
      if (current == DayNightAlgo::Mode::Night)
        DayNightAlgo::on_enter_night(params, state);
      else if (current == DayNightAlgo::Mode::Day)
        DayNightAlgo::on_enter_day(state);
      // reflect new mode in metrics
      cfg->daynight.live_mode.store(current == DayNightAlgo::Mode::Day ? "day" : "night",
                                    std::memory_order_relaxed);
    }

    const char *mode_str = (current == DayNightAlgo::Mode::Day)
                   ? "day"
                   : (current == DayNightAlgo::Mode::Night ? "night" : "unknown");
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
