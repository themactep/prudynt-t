#pragma once

#include <cstdint>

namespace DayNightAlgo {

enum class Mode { Day = 0, Night = 1, Unknown = -1 };

struct Decision {
  Mode target = Mode::Unknown;
  int reason = 0;
  bool toggled = false;
};

// ── Simple total-gain algorithm (active) ──────────────────────────

struct Params {
  int total_gain_night_threshold = 3000; // Switch to night when gain > this
  int total_gain_day_threshold = 300;    // Switch to day when gain < this
  int night_count_threshold = 6;
  int day_count_threshold = 4;

  // EV fallback for platforms without total_gain (T10, T20)
  int ev_night_threshold = 1500000;
  int ev_day_threshold = 200000;
};

struct State {
  bool is_night = false;
  int night_count = 0;
  int day_count = 0;
};

inline void init(State &s) {
  s = {};
  s.is_night = false;
}

inline Decision decide(const Params &p, State &s, int total_gain, int ev) {
  Decision d{};

  if (total_gain >= 0) {
    if (total_gain > p.total_gain_night_threshold) {
      s.day_count = 0;
      if (++s.night_count >= p.night_count_threshold) {
        if (!s.is_night) {
          d.target = Mode::Night;
          d.reason = 1;
          d.toggled = true;
        }
      }
    } else if (total_gain < p.total_gain_day_threshold) {
      s.night_count = 0;
      if (++s.day_count >= p.day_count_threshold) {
        if (s.is_night) {
          d.target = Mode::Day;
          d.reason = 3;
          d.toggled = true;
        }
      }
    } else {
      if (s.night_count > 0) --s.night_count;
      if (s.day_count > 0) --s.day_count;
    }
  } else if (ev >= 0) {
    if (ev > p.ev_night_threshold) {
      s.day_count = 0;
      if (++s.night_count >= p.night_count_threshold) {
        if (!s.is_night) {
          d.target = Mode::Night;
          d.reason = 1;
          d.toggled = true;
        }
      }
    } else if (ev < p.ev_day_threshold) {
      s.night_count = 0;
      if (++s.day_count >= p.day_count_threshold) {
        if (s.is_night) {
          d.target = Mode::Day;
          d.reason = 3;
          d.toggled = true;
        }
      }
    } else {
      if (s.night_count > 0) --s.night_count;
      if (s.day_count > 0) --s.day_count;
    }
  } else {
    s.night_count = 0;
    s.day_count = 0;
  }

  return d;
}

} // namespace DayNightAlgo
