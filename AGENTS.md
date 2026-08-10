# AGENTS.md -- prudynt-t

prudynt-t is a C++20 video streaming server for Ingenic MIPS (mipsel) SoCs,
cross-compiled from x86_64. It targets Thingino firmware cameras and ships two
binaries: `prudynt` (the server) and `prudyntctl` (a Unix-socket client for
`/run/prudynt/prudynt.sock`).

This is a **build-by-overlay** checkout -- it lives under
`firmware/overrides/prudynt-t` of the Thingino builder image and is consumed
both standalone (Docker/`build.sh`) and via the buildroot package
(`buildroot_dev.sh`).

## Build -- read this before invoking make directly

Don't call `make` blindly. The Makefile expects a fully-set cross environment
(`CROSS_COMPILE`, `-DPLATFORM_<SOC>`, `-DBINARY_*`, libc define, `-isystem`
third-party include dirs) that `build.sh` assembles for you.

Two-stage flow, always:

```
./build.sh deps   <SOC> [flags]   # builds 3rdparty/ libs (one-time per SOC)
./build.sh prudynt <SOC> [flags]   # builds bin/prudynt + bin/prudyntctl
# or combined:
./build.sh full   <SOC> [flags]
```

- SOCs: `T10 T20 T21 T23 T30 T31 C100 T40 T41`. `T31` is the fallback default
  when no `-DPLATFORM_*` is passed.
- Binary type flags: `-static` | `-hybrid` | (default = dynamic).
  `-debug` forces `-static` plus `-O0 -g`, `DEBUG_STRIP=0`, `-DENABLE_LOG_DEBUG`.
- libc: `--libc-uclibc` (default in `build.sh`) | `--libc-musl`. uClibc builds
  add `-fno-stack-protector` (thingino uClibc toolchain is built with
  `--disable-libssp`). The Dockerfile and CI use the **musl** toolchain.
- `build.sh` auto-downloads the matching Thingino toolchain into `toolchain/`
  (xburst1 vs xburst2 picked from SOC) unless `PRUDYNT_CROSS` is already set.
  Set `PRUDYNT_CROSS=ccache <prefix>` to override; ccache is auto-prepended if
  installed.
- If `/nfs` exists at build end, `prudynt` and `res/prudynt.json` are copied to
  `/nfs/prudynt-<soc-lower>` and `/nfs/prudynt-<soc-lower>.json` for NFS deploy.
- `res/prudynt.json` is the reference runtime config.

### Buildroot (in-tree) path

`buildroot_dev.sh` rebuilds prudynt against an already-built Thingino buildroot
profile in `~/output/<profile>`. Requires `dialog` unless a profile name is
passed on the CLI. Flags: `-d` debug, `-c` no ccache, `-b
dynamic|static|hybrid`. It does `rm -rf 3rdparty && make distclean` first, so
don't expect `3rdparty/` to survive across runs of this script.

### Direct make (only if you know what you're doing)

`make` alone won't link -- you must replicate the env that `build.sh prudynt`
passes: `CROSS_COMPILE`, `CFLAGS` with `-DPLATFORM_<SOC>` + `-DBINARY_*` +
`-DLIBC_UCLIBC` (or nothing for musl) + `-isystem 3rdparty/install/include/...`,
`LDFLAGS=-L./3rdparty/install/lib`, and `PKG_CONFIG_PATH` pointing at
`3rdparty/install/lib/pkgconfig`. When in doubt, run `./build.sh prudynt ...`
and read what it invokes.

### Makefile feature toggles (env vars)

`USE_WEBSOCKETS=0|1` (legacy alias `WEBSOCKET_ENABLED`), `USE_PREBUFFER=0|1`,
`USE_FLAC`, `USE_MP3`, `USE_OPUS`, `USE_AAC`, `USE_EXECINFO=0|1` (needs
libexecinfo). All default to `1` except `USE_EXECINFO`. These map to
`-D<FEATURE>_ENABLED` / `-DUSE_<CODEC>=<n>` defines in CFLAGS.

### Clean

- `make clean` -- removes `obj/` and the generated `include/<SOC>/<ver>/<lang>/version.hpp`
- `make distclean` -- also removes `bin/`
- `rm -rf 3rdparty` (manual) -- needed when switching SOC/libc/build type or
  after `--clean-all` of `build.sh deps`.

## Layout

- `src/` -- server sources (`main.cpp` entry) plus `prudyntctl.cpp` (separate
  binary, no link against IMP/codec libs).
- `src/simple-rtsp/` -- custom RTSP server (replaces live555); built as part of
  the main target, see `docs/rtsp.md`.
- `include/` -- **git submodule** (`gtxaspec/ingenic-headers`). Per-platform SDK
  headers live under `include/<SOC>/<SDK_VERSION>/<lang>/` (lang is `en` for
  T31/C100, `zh` otherwise). `version.hpp` is generated here from
  `src/version.tpl.hpp`. **Do not edit files under `include/` here** -- they
  come from the submodule.
- `3rdparty/` -- gitignored; populated by `build.sh deps`. Holds libimp,
  libhelix-aac/mp3, libflac-lite, libwebsockets, opus, faac, jct, libschrift,
  curl, and the musl/uclibc shim. Read `build.sh` `deps()` for the canonical
  versions and patch flow (local patches live in `res/<lib>/*.patch`).
- `res/` -- runtime config (`prudynt.json`), per-lib patches, OSD/color/IRCUT/
  IRLED/daynight assets, `imp-control` JSON definitions.
- `tests/` -- ad-hoc, no harness. Each test is self-contained and built by
  hand (compile commands are in the file headers, e.g.
  `g++ -std=c++17 test_daynight_algo.cpp -o ...`). `test_shared_rotation.c`
  is a device-only integration probe against IMP, not a unit test.
- `docs/` -- design notes per subsystem (rtsp, audio, video, osd, mp4-control,
  prebuffer, webrtc, metrics, HAL platform matrix). Treat these as the
  authoritative architecture reference.
- `.github/workflows/pru.yaml` -- CI matrix: T10/T20/T21/T23/T30/T31/C100 ×
  static/dynamic/hybrid using musl gcc14 toolchain. Releases go under tag
  `release`.

## Code style / conventions

- Only ASCII code, no emojis, no unicode characters. E.g. replace en- and em-dash
  with double- and tripple- hyphens.
- `.clang-format` is `BasedOnStyle: LLVM`; keep new code formatted with it.
  No `ColumnLimit` is set (header comment is commented out).
- C++20 (`-std=c++20`); C and C++ both compiled with `-Wall -Wextra
  -Wno-unused-parameter`.
- Header/source pairs are co-located in `src/` (`Foo.cpp` + `Foo.hpp`).
- Workers follow a consistent `*Worker.cpp/.hpp` pattern; the
  `MsgChannel.hpp` tap/drain model is central to RTSP/WS/HTTP-MJPEG delivery
  (see `docs/rtsp.md` before touching the drain loop).
- OpenSSL is disabled (`-DNO_OPENSSL=1` is forced by `build.sh` and the
  Makefile); don't add code that requires it.
- Backchannel / two-way audio path lives across `IMPBackchannel`,
  `BackchannelWorker`, `AudioOutputWorker`, `AudioReframer`, `Opus`,
  `AACEncoder`. Much of recent history is backchannel/SDP fix work; PRs here
  tend to be reverted/relitigated -- check `git log` before changing SDP or
  PLAY/SETUP handling in `simple-rtsp`.

## Verification

There is no automated test or lint target in the Makefile. Before claiming
done:

1. `./build.sh prudynt <SOC>` succeeds end-to-end (run `./build.sh deps` first
   if `3rdparty/` is empty or the SOC/libc/binary-type changed).
2. Optionally `clang-format --dry-run -Werror` on changed C/C++ files.
3. For runtime behaviour changes, smoke-test on a camera -- see the project
   skills (`device-smoke-cycle`, `nfs-dev-deploy`, `build-and-ota`,
   `collect-diagnostics`) for the safe workflow.

## Gotchas

- Switching SOC or libc requires `rm -rf 3rdparty` before re-running
  `./build.sh deps` (or pass `--clean-all`).
- The `include/` submodule must be initialised (`git submodule update
  --init`) or platform headers will be missing.
- `version.hpp` is generated at build time into the platform include dir; if
  `make clean` ran, the next build regenerates it. Don't commit it (it's
  gitignored).
- T40/T41 are XBurst2 with kernel-4.x SDKs and use different
  `IMP_ISP_Tuning_*` signatures than T10-T31/C100 -- see
  `docs/HAL_PLATFORM_SUPPORT.md` before touching `imp_hal.cpp` / `imp_control.cpp`.
- `buildroot_dev.sh` rebuilds from a **musl** buildroot sysroot regardless of
  the `--libc-*` flag used in `build.sh`; don't mix its `3rdparty/` with one
  built for uclibc.
