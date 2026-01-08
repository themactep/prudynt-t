#pragma once

#include <cmath>
#include <cstdint>

namespace DayNightAlgo {

enum class Mode { Day = 0, Night = 1, Unknown = -1 };

struct Params {
  int ev_night_high = 1900000;
  int night_count_threshold = 6; // >5 in script

  int ev_day_low_primary = 479832;
  int ev_day_low_secondary = 361880;
  int gb_gain_delta = 15;
  int gb_gain_absolute = 145;
  int day_count_threshold = 4; // >3 in script

  int settle_samples_for_gb_record = 20;
};

struct Signals {
  int ev = -1;      // script uses iso_buf = EV
  int gb_gain = -1; // AWB b/g weighted value (integer)
  int gr_gain = -1; // AWB r/g weighted value (integer)
};

struct State {
  bool ircut_engaged = true; // true when in night per script
  int night_count = 0;
  int day_count = 0;
  int gb_gain_record = 200; // sentinel high
  int gr_gain_record = 200; // sentinel high
  int settle_remaining = 0; // samples left to capture minima after entering night
};

struct Decision {
  Mode target = Mode::Unknown; // desired mode if toggle is authorized
  int reason = 0;              // 1=night by EV, 3=day by EV/GB
  bool toggled = false;        // thresholds/counters authorize switching now
};

inline void init(State &s) {
  s = {};
  s.ircut_engaged = true;
  s.gb_gain_record = 200;
  s.gr_gain_record = 200;
}

inline void on_enter_night(const Params &p, State &s) {
  s.ircut_engaged = true;
  s.settle_remaining = p.settle_samples_for_gb_record;
  s.gb_gain_record = 200;
  s.gr_gain_record = 200;
  s.day_count = 0;
}

inline void on_enter_day(State &s) {
  s.ircut_engaged = false;
  s.day_count = 0;
  s.night_count = 0; // Reset night counter to prevent immediate flip-back
  s.settle_remaining = 0; // No settle needed in day mode
}

inline void update_minima_window(State &s, const Signals &sig) {
  if (s.settle_remaining > 0) {
    if (sig.gb_gain >= 0 && sig.gb_gain < s.gb_gain_record)
      s.gb_gain_record = sig.gb_gain;
    if (sig.gr_gain >= 0 && sig.gr_gain < s.gr_gain_record)
      s.gr_gain_record = sig.gr_gain;
    --s.settle_remaining;
  }
}

inline Decision decide(const Params &p, State &s, const Signals &sig) {
  Decision d{};

  // Night path: EV high for N samples
  // Only check if we're not in the settle window (where EV might be contaminated by IR)
  if (s.settle_remaining == 0 && sig.ev > p.ev_night_high) {
    if (++s.night_count >= p.night_count_threshold) {
      d.target = Mode::Night;
      d.reason = 1;
      d.toggled = true;
    }
  } else {
    s.night_count = 0;
  }

  // Day path: only when currently in night (IR-cut engaged)
  if (s.ircut_engaged && sig.ev < p.ev_day_low_primary) {
    // Treat zero/negative GB as unavailable on some platforms in NIGHT
    bool have_gb = (sig.gb_gain > 0);
    bool gb_delta_ok = have_gb && (sig.gb_gain > s.gb_gain_record + p.gb_gain_delta);
    if (have_gb) {
      if (gb_delta_ok) {
        if (sig.ev < p.ev_day_low_secondary || sig.gb_gain > p.gb_gain_absolute) {
          if (++s.day_count >= p.day_count_threshold) {
            d.target = Mode::Day;
            d.reason = 3;
            d.toggled = true;
          }
        } else {
          s.day_count = 0;
        }
      } else {
        s.day_count = 0;
      }
    } else {
      // Fallback: allow EV-only day exit when AWB is unavailable
      if (sig.ev < p.ev_day_low_secondary) {
        if (++s.day_count >= p.day_count_threshold) {
          d.target = Mode::Day;
          d.reason = 3;
          d.toggled = true;
        }
      } else {
        s.day_count = 0;
      }
    }
  } else {
    s.day_count = 0;
  }

  return d;
}

// ============================================================================
// SIMPLE TOTAL GAIN ALGORITHM (NEW - CURRENTLY ACTIVE)
// ============================================================================
// This simplified algorithm uses only total_gain for reliable day/night detection
// since GB/GR AWB gains are not available on T23 platforms.
//
// Total Gain behavior:
//   - Low gain values (< 100) = Bright conditions = Day mode
//   - High gain values (> 200) = Dark conditions = Night mode
//
// These thresholds should be calibrated using real sensor data collection.
// ============================================================================

struct SimpleParams {
  int total_gain_night_threshold = 200;  // Switch to night when gain > this
  int total_gain_day_threshold = 100;    // Switch to day when gain < this
  int night_count_threshold = 6;         // Consecutive samples before switching to night
  int day_count_threshold = 4;           // Consecutive samples before switching to day
};

struct SimpleState {
  bool is_night = true;  // Current mode (true = night, false = day)
  int night_count = 0;
  int day_count = 0;
};

inline void simple_init(SimpleState &s) {
  s = {};
  s.is_night = true;
}

inline Decision simple_decide(const SimpleParams &p, SimpleState &s, int total_gain) {
  Decision d{};

  // Invalid gain value - no decision
  if (total_gain < 0) {
    s.night_count = 0;
    s.day_count = 0;
    return d;
  }

  // Night detection: gain above threshold
  if (total_gain > p.total_gain_night_threshold) {
    s.day_count = 0;
    if (++s.night_count >= p.night_count_threshold) {
      if (!s.is_night) {
        d.target = Mode::Night;
        d.reason = 1;
        d.toggled = true;
      }
    }
  }
  // Day detection: gain below threshold
  else if (total_gain < p.total_gain_day_threshold) {
    s.night_count = 0;
    if (++s.day_count >= p.day_count_threshold) {
      if (s.is_night) {
        d.target = Mode::Day;
        d.reason = 3;
        d.toggled = true;
      }
    }
  }
  // In between thresholds - reset counters (hysteresis zone)
  else {
    s.night_count = 0;
    s.day_count = 0;
  }

  return d;
}

} // namespace DayNightAlgo
