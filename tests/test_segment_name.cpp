// Host test for unique_segment_path(): local-time names must never be reused.
// Build and run:
//   c++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/test_segment_name.cpp -o /tmp/test_segment_name
//   /tmp/test_segment_name

#include "recording/segment_name.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool cond, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: tests/test_segment_name.cpp:%d\n", line);
    ++failures;
  }
}

#define CHECK(cond) check((cond), __LINE__)

void touch(const fs::path &p) { std::ofstream(p).put('x'); }

} // namespace

int main() {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path dir =
      fs::temp_directory_path() / ("prudynt-segname-" + std::to_string(stamp));
  fs::create_directories(dir);

  // Free name: used verbatim (the normal case; matches the OSD time).
  const fs::path base = dir / "2026-11-01_02-30-00.mp4";
  CHECK(unique_segment_path(base) == base);

  // DST fall-back repeats the local hour: same-second names collide and must
  // be disambiguated rather than reused.
  touch(base);
  CHECK(unique_segment_path(base) == dir / "2026-11-01_02-30-00-1.mp4");
  touch(dir / "2026-11-01_02-30-00-1.mp4");
  CHECK(unique_segment_path(base) == dir / "2026-11-01_02-30-00-2.mp4");

  // Extension-less template: the suffix goes at the end.
  const fs::path noext = dir / "clip";
  CHECK(unique_segment_path(noext) == noext);
  touch(noext);
  CHECK(unique_segment_path(noext) == dir / "clip-1");

  fs::remove_all(dir);

  if (failures == 0)
    std::printf("PASS: unique_segment_path disambiguates existing names\n");
  return failures ? 1 : 0;
}
