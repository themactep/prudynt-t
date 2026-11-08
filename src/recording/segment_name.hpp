#ifndef RECORDING_SEGMENT_NAME_HPP
#define RECORDING_SEGMENT_NAME_HPP

#include <filesystem>
#include <string>
#include <system_error>

// Pick a segment filename that does not already exist.  The preferred name is
// used as-is when free; otherwise the first free "<stem>-<n><ext>" is returned.
// An empty path means the caller should not start a segment (stat error or no
// free slot); it must never fall back to overwriting an existing recording.
//
// Wall-clock names are built from local time so a clip's filename matches the
// OSD burn-in.  That makes exact collisions possible: at DST fall-back the
// local hour repeats, and coarse strftime templates or a clock step can collide
// too.  Disambiguating here keeps local-time names without ever truncating an
// earlier segment.
inline std::filesystem::path
unique_segment_path(const std::filesystem::path &wanted) {
  std::error_code ec;
  if (!std::filesystem::exists(wanted, ec))
    return ec ? std::filesystem::path{} : wanted;

  const std::filesystem::path dir = wanted.parent_path();
  const std::string stem = wanted.stem().string();
  const std::string ext = wanted.extension().string();
  for (int n = 1; n <= 999; ++n) {
    std::filesystem::path candidate =
        dir / (stem + "-" + std::to_string(n) + ext);
    if (!std::filesystem::exists(candidate, ec))
      return ec ? std::filesystem::path{} : candidate;
  }
  return {};
}

#endif // RECORDING_SEGMENT_NAME_HPP
