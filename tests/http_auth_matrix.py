#!/usr/bin/env python3
"""HTTP auth matrix test for a running prudynt build.

Verifies the security property that makes the API key gate worth having:
an untrusted client can not fetch streams or config, while loopback,
clients listed in ``webui.auth_bypass_ips``, and explicit key holders are
allowed.

Run from the host against a camera that has a test build deployed:

    tests/http_auth_matrix.py 192.168.88.112
    tests/http_auth_matrix.py 192.168.88.112 --ssh root@192.168.88.112
    tests/http_auth_matrix.py 192.168.88.112 --ws-port 8089

The untrusted leg rewrites ``webui.auth_bypass_ips`` on the camera with
``jct`` and restores the original value on exit (including on Ctrl-C), so
SSH access is needed for the full matrix.  ``--no-mutate`` runs only the
current-config and key legs.

Exit status: 0 when every check passes, 1 otherwise.
"""

import argparse
import subprocess
import sys
import urllib.error
import urllib.request

# Endpoints that must never be reachable without the API key (or a
# trusted/loopback source).  Stream endpoints are long-lived; the status
# line is all we need.
PROTECTED_PATHS = [
    "/api/v1/config/rtsp",
    "/api/v1/config/stream0",
    "/ch0.mp4",
    "/mjpg?ch=0",
    "/mjpeg?ch=0",
    "/x/mjpg?ch=0",
    "/x/mjpeg?ch=0",
]
# Read-only metadata the server deliberately leaves open.  Asserted so a
# future change that turns it into a stream (or exposes more) is noticed.
OSD_PATH = "/api/v1/osd-sei"
# RFC 5737 TEST-NET-3: can not collide with a camera LAN address.
UNTRUSTED_SENTINEL = "203.0.113.1/32"

PASS = 0
FAIL = 0


def record(ok, name, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"ok   {name}" + (f"  {detail}" if detail else ""))
    else:
        FAIL += 1
        print(f"FAIL {name}" + (f"  {detail}" if detail else ""))


def http_status(url, headers=None, timeout=5, method="GET"):
    """Return the HTTP status, or None on connection/timeout errors."""
    req = urllib.request.Request(url, headers=headers or {}, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status
    except urllib.error.HTTPError as e:
        return e.code
    except (urllib.error.URLError, TimeoutError, OSError):
        return None


def ssh_run(target, command, timeout=20):
    """Run command on the camera over ssh; return (rc, stdout, stderr)."""
    if not target:
        return 127, "", "ssh disabled"
    try:
        p = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
             target, command],
            capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout.strip(), p.stderr.strip()
    except (subprocess.TimeoutExpired, OSError) as e:
        return 1, "", str(e)


def jct_get(target, path):
    rc, out, _ = ssh_run(target, f"jct /etc/thingino.json get {path}")
    return out if rc == 0 else None


def jct_set(target, path, value):
    rc, _, err = ssh_run(target, f"jct /etc/thingino.json set {path} '{value}'")
    return rc == 0, err


def camera_curl(target, url):
    # Long-lived streams make curl exit 28 on --max-time, but -w still
    # prints the status code, so trust the output over the exit status.
    _, out, _ = ssh_run(
        target, f"curl -s -o /dev/null --max-time 4 -w '%{{http_code}}' '{url}'")
    return int(out) if out.isdigit() else None


def check_protected(host, port, base_headers, expect, label):
    for path in PROTECTED_PATHS:
        url = f"http://{host}:{port}{path}"
        code = http_status(url, headers=base_headers, timeout=4)
        record(code == expect, f"{label} {path}", f"status={code} want={expect}")


def check_loopback(target, port, label):
    for path in PROTECTED_PATHS:
        url = f"http://127.0.0.1:{port}{path}"
        code = camera_curl(target, url)
        record(code == 200, f"{label} {path}", f"status={code} want=200")


def check_osd(host, port, expect, label):
    code = http_status(f"http://{host}:{port}{OSD_PATH}", timeout=4)
    record(code == expect, f"{label} {OSD_PATH}", f"status={code} want={expect}")


def check_websocket(target, host, ws_port):
    """The libwebsockets server (if present) must gate its HTTP media on
    websocket.http_secured; the token lives in /run/prudynt/websocket_token."""
    enabled = jct_get(target, "websocket.enabled")
    if enabled not in ("true", "1"):
        print(f"skip websocket:{ws_port} (disabled in config)")
        return
    secured = jct_get(target, "websocket.http_secured")
    record(secured in ("true", "1"),
           "websocket.http_secured is true",
           f"value={secured}")
    if secured not in ("true", "1"):
        return
    _, token, _ = ssh_run(target, "cat /run/prudynt/websocket_token")
    for path in ("/preview.jpg", "/ch0.mp4"):
        code = http_status(f"http://{host}:{ws_port}{path}", timeout=4)
        if code is None:
            print(f"skip websocket:{ws_port}{path} (not listening)")
            continue
        record(code == 403, f"ws no-token {path}", f"status={code} want=403")
        if token:
            code = http_status(
                f"http://{host}:{ws_port}{path}?token={token}", timeout=4)
            record(code == 200, f"ws with-token {path}",
                   f"status={code} want=200")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="camera address")
    ap.add_argument("--port", type=int, default=8080, help="HTTP API port")
    ap.add_argument("--ssh", default=None,
                    help="ssh target (default root@<host>; empty disables)")
    ap.add_argument("--api-key", default=None,
                    help="API key (default: read /etc/thingino-api.key over ssh)")
    ap.add_argument("--no-mutate", action="store_true",
                    help="do not touch webui.auth_bypass_ips")
    ap.add_argument("--ws-port", type=int, default=None,
                    help="libwebsockets port to check (e.g. 8089)")
    args = ap.parse_args()

    ssh = args.ssh if args.ssh is not None else f"root@{args.host}"
    base = f"http://{args.host}:{args.port}"

    key = args.api_key
    if not key and ssh:
        _, key, _ = ssh_run(ssh, "cat /etc/thingino-api.key")
    key = (key or "").strip()
    record(bool(key), "have API key",
           "from --api-key" if args.api_key else "from /etc/thingino-api.key")

    client_ip = ""
    if ssh:
        _, out, _ = ssh_run(ssh, "echo $SSH_CLIENT")
        client_ip = out.split()[0] if out else ""
    record(bool(client_ip) or args.no_mutate, "client IP known",
           client_ip or "ssh disabled")

    original = None
    mutated = False
    try:
        # -- current config: whatever the camera is set to -------------------
        check_osd(args.host, args.port, 200, "untrusted(ish)")

        # -- untrusted leg ---------------------------------------------------
        if ssh and not args.no_mutate and client_ip:
            original = jct_get(ssh, "webui.auth_bypass_ips")
            ok, err = jct_set(ssh, "webui.auth_bypass_ips", UNTRUSTED_SENTINEL)
            record(ok, "set bypass to sentinel", err)
            mutated = ok

            check_protected(args.host, args.port, {}, 401, "untrusted no-key")
            check_osd(args.host, args.port, 200, "untrusted no-key")
            # A valid key must still work from an untrusted address.
            check_protected(args.host, args.port, {"X-API-Key": key}, 200,
                            "untrusted with-key")

            # -- trusted leg (client IP in the bypass list) ------------------
            ok, err = jct_set(ssh, "webui.auth_bypass_ips", client_ip)
            record(ok, "set bypass to client IP", err)
            check_protected(args.host, args.port, {}, 200, "trusted no-key")
            check_osd(args.host, args.port, 200, "trusted no-key")
        elif args.no_mutate:
            print("skip untrusted/trusted legs (--no-mutate)")

        # -- loopback leg ----------------------------------------------------
        if ssh:
            check_loopback(ssh, args.port, "loopback no-key")

        # -- websocket server (optional) -------------------------------------
        if args.ws_port and ssh:
            check_websocket(ssh, args.host, args.ws_port)
    finally:
        if mutated and original is not None:
            ok, _ = jct_set(ssh, "webui.auth_bypass_ips", original)
            print(("restored bypass to " if ok else
                   "FAILED to restore bypass, current value is ") + repr(original))
        elif mutated:
            jct_set(ssh, "webui.auth_bypass_ips", "")

    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
