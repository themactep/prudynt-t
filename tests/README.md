# Tests

Most files here are host-side unit tests with the build command in the file
header (`test_daynight_algo.cpp`, `test_h264_sps_vui.cpp`,
`test_h265_sps_vui.cpp`). `test_shared_rotation.c` is a device-only probe
against IMP.

`http_auth_matrix.py` is different: it is an integration test that runs
against a **live camera** after a test build is deployed. It validates the
HTTP authentication model described below.

## HTTP auth model

The `HTTPMJPEG` server (default port 8080) serves the config API, fMP4 and
MJPEG endpoints. Access is granted when any of these holds:

- the request carries the API key, either as `X-API-Key: <key>` or
  `?token=<key>` (`/etc/thingino-api.key`); or
- the peer is loopback (`127.0.0.0/8`); or
- the peer matches `webui.auth_bypass_ips` in `/etc/thingino.json`, using
  the same syntax as `/var/www/x/auth.sh`: exact IPs, `192.168.1.` prefixes,
  or CIDR ranges.

One endpoint is intentionally **not** gated: `GET /api/v1/osd-sei` returns
read-only OSD text metadata. The test asserts it is reachable so that a
future change turning it into a stream (or widening it) is noticed.

When the libwebsockets server is built in (default port 8089), its
`/preview.jpg` and `/ch0.mp4` endpoints require the token written to
`/run/prudynt/websocket_token` unless `websocket.http_secured` is false.
`--ws-port` checks that.

## http_auth_matrix.py

### Requirements

- Host: Python 3 (stdlib only) and `ssh` access to the camera.
- Camera: a test build, plus `jct` and `curl` (both in firmware).

### Usage

```sh
# Full matrix (mutates and restores webui.auth_bypass_ips over ssh):
tests/http_auth_matrix.py 192.168.88.112

# Explicit ssh target / key, and the optional websocket leg:
tests/http_auth_matrix.py 192.168.88.112 --ssh root@192.168.88.112 --ws-port 8089

# Do not touch the camera config: checks only the current bypass mode,
# the API key, and loopback.
tests/http_auth_matrix.py 192.168.88.112 --no-mutate
```

Options: `--port` (default 8080), `--ssh` (default `root@<host>`, empty
disables the ssh legs), `--api-key` (default: read over ssh), `--no-mutate`,
`--ws-port`.

### What it checks

| Leg | Request | Expected |
|-----|---------|----------|
| untrusted no-key | each protected path | 401 |
| untrusted no-key | `/api/v1/osd-sei` | 200 (metadata, by design) |
| untrusted with-key | each protected path | 200 |
| trusted no-key | each protected path | 200 |
| trusted no-key | `/api/v1/osd-sei` | 200 |
| loopback no-key | each protected path | 200 |
| `--ws-port` no-token | `/preview.jpg`, `/ch0.mp4` | 403 |
| `--ws-port` with-token | `/preview.jpg`, `/ch0.mp4` | 200 |
| `--ws-port` config | `websocket.http_secured` | true |

Protected paths: `/api/v1/config/rtsp`, `/api/v1/config/stream0`,
`/ch0.mp4`, `/mjpg?ch=0`, `/mjpeg?ch=0`, `/x/mjpg?ch=0`, `/x/mjpeg?ch=0`.

### How the untrusted leg is forced

The test host is normally in `webui.auth_bypass_ips`. To exercise the
untrusted path it rewrites `webui.auth_bypass_ips` to the RFC 5737 sentinel
`203.0.113.1/32` with `jct`, runs the leg, then points it at the test host
IP for the trusted leg. The original value is restored in a `finally` block,
so it survives failures and Ctrl-C. Loopback is checked by running `curl`
on the camera over ssh.

Stream endpoints are long-lived; the host side reads the status line and
closes. On camera, `curl --max-time` exits 28 on a live stream but still
prints the status code, which is what the test parses.
