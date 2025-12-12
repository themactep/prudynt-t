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
#include <thread>

using namespace std::chrono;

namespace DayNightWorkerNS {

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
      LOG_WARN("SetISPRunningMode(DAY) failed: " << ret);
    }
    std::string cmd = std::string(script) + " day";
    (void)std::system(cmd.c_str());
  } else if (m == DayNightAlgo::Mode::Night) {
    int ret = hal::isp::set_running_mode(hal::isp::RunningMode::Night);
    if (ret != 0) {
      LOG_WARN("SetISPRunningMode(NIGHT) failed: " << ret);
    }
    std::string cmd = std::string(script) + " night";
    (void)std::system(cmd.c_str());
  }
}

void *thread_entry(void *arg) {
  (void)arg;
  LOG_INFO("DayNightWorker: starting (percent-based algo)");

  DayNightAlgo::Params params{};
  DayNightAlgo::State state{};
  DayNightAlgo::init(state);

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
    params.ev_day_low_secondary =
        (ev_day_low_secondary_cfg > 0) ? ev_day_low_secondary_cfg : ev_day_low_primary_cfg;
    params.night_count_threshold = pr.base_night + (tol_pct + 24) / 25;
    params.day_count_threshold = pr.base_day + (tol_pct + 24) / 25;
  } else {
    params.ev_night_high = ev_from_percent(pr, below_pct);
    params.ev_day_low_primary = ev_from_percent(pr, above_pct);
    int secondary_pct = clampi(above_pct + pr.sec_margin_percent, 0, 100);
    params.ev_day_low_secondary = ev_from_percent(pr, secondary_pct);
    params.night_count_threshold = pr.base_night + (tol_pct + 24) / 25;
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

  while (!global_shutdown_requested.load(std::memory_order_relaxed)) {
    int ev = -1, gr = -1, gb = -1;
    (void)read_ev(ev);
    (void)read_awb(gr, gb);

    DayNightAlgo::Signals sig{ev, gb, gr};
    DayNightAlgo::update_minima_window(state, sig);
    auto dec = DayNightAlgo::decide(params, state, sig);

    int bright_pct = percent_from_ev(pr, ev);
    cfg->daynight.live_brightness_percent.store(bright_pct, std::memory_order_relaxed);
    cfg->daynight.live_ev.store(ev, std::memory_order_relaxed);
    cfg->daynight.live_gb.store(gb, std::memory_order_relaxed);
    cfg->daynight.live_gr.store(gr, std::memory_order_relaxed);
    const char *mode_str = (current == DayNightAlgo::Mode::Day)
                               ? "day"
                               : (current == DayNightAlgo::Mode::Night ? "night" : "unknown");
    cfg->daynight.live_mode.store(mode_str, std::memory_order_relaxed);

    LOG_DEBUG("DayNight: brightness%=" << bright_pct << " EV=" << ev << " GB=" << gb << " GR=" << gr
                      << " recGB=" << state.gb_gain_record << " nCnt=" << state.night_count
                      << " dCnt=" << state.day_count
                      << " ircut=" << (state.ircut_engaged ? "1" : "0") << " -> "
                      << (dec.toggled ? (dec.target == DayNightAlgo::Mode::Day ? "DAY" : "NIGHT")
                               : "HOLD"));

    if (dec.toggled && dec.target != current) {
      apply_mode(dec.target);
      current = dec.target;
      if (current == DayNightAlgo::Mode::Night)
        DayNightAlgo::on_enter_night(params, state);
      else if (current == DayNightAlgo::Mode::Day)
        DayNightAlgo::on_enter_day(state);
      cfg->daynight.live_mode.store(current == DayNightAlgo::Mode::Day ? "day" : "night",
                                    std::memory_order_relaxed);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }

  LOG_INFO("DayNightWorker: shutting down");
  return nullptr;
}

} // namespace DayNightWorkerNS
