#!/bin/sh
# Build and run the host-side unit tests (no IMP SDK or codec libraries).
# test_shared_rotation.c (device probe) and http_auth_matrix.py (live camera)
# are intentionally excluded.
set -eu

cd "$(dirname "$0")/.."

CXX=${CXX:-c++}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT INT TERM

echo "== test_daynight_algo"
$CXX -std=c++17 -Wall -Wextra tests/test_daynight_algo.cpp -o "$out/daynight"
"$out/daynight"

echo "== test_h264_sps_vui"
$CXX -std=c++17 -Wall -Wextra -Isrc tests/test_h264_sps_vui.cpp \
  src/video/H264SpsRewrite.cpp -o "$out/h264"
"$out/h264"

echo "== test_h265_sps_vui"
$CXX -std=c++17 -Wall -Wextra -Isrc tests/test_h265_sps_vui.cpp \
  src/video/H265SpsRewrite.cpp -o "$out/h265"
"$out/h265"

echo "== test_g711"
$CXX -std=c++20 -Wall -Wextra -Werror -Isrc tests/test_g711.cpp -o "$out/g711"
"$out/g711"

echo "== test_msgchannel"
$CXX -std=c++20 -Wall -Wextra -Werror -Isrc tests/test_msgchannel.cpp -o "$out/msgchannel"
"$out/msgchannel"
