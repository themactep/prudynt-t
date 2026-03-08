#!/bin/bash
set -euo pipefail

usage() {
	cat <<EOF
Usage: $(basename "$0") <bundle-dir> <user@host> [password] [remote-dir]

Uploads a prudynt runtime bundle over ssh (no sftp/scp required), installs it
under the remote directory, repoints /opt/prudynt.patched to the bundle wrapper
and restarts prudynt.
EOF
}

if [[ $# -lt 2 || $# -gt 4 ]]; then
	usage
	exit 1
fi

BUNDLE_DIR="$(readlink -f "$1")"
REMOTE_HOST="$2"
REMOTE_PASS="${3:-}"
REMOTE_DIR="${4:-/opt/prudynt-bundle}"

if [[ ! -f "$BUNDLE_DIR/prudynt" || ! -f "$BUNDLE_DIR/bin/prudynt.real" ]]; then
	echo "Bundle is incomplete: expected prudynt wrapper and bin/prudynt.real under $BUNDLE_DIR" >&2
	exit 1
fi

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
if [[ -n "$REMOTE_PASS" ]]; then
	SSH_BASE=(sshpass -p "$REMOTE_PASS" ssh "${SSH_OPTS[@]}")
else
	SSH_BASE=(ssh "${SSH_OPTS[@]}")
fi

REMOTE_STAGING="${REMOTE_DIR}.staging.$$"
BACKUP_DIR="/opt/backups/agents/thingino-prudynt-runtime/$(date -u +%F_%H%M%S)"

tar -C "$BUNDLE_DIR" -cf - . | "${SSH_BASE[@]}" "$REMOTE_HOST" "
set -eu
rm -rf '$REMOTE_STAGING'
mkdir -p '$REMOTE_STAGING'
tar -xf - -C '$REMOTE_STAGING'
mkdir -p '$BACKUP_DIR'
if [ -e /opt/prudynt.patched ]; then
	cp -a /opt/prudynt.patched '$BACKUP_DIR/prudynt.patched.preinstall'
fi
if [ -d '$REMOTE_DIR' ]; then
	mv '$REMOTE_DIR' '$BACKUP_DIR/runtime.previous'
fi

find_live_pids() {
	ps w | awk '/\\/opt\\/prudynt-bundle-.*\\/bin\\/prudynt\\.real/ && !/awk/ {print \$1}'
}

wait_for_live_exit() {
	retries=\${1:-10}
	while [ \"\$retries\" -gt 0 ]; do
		if [ -z \"\$(find_live_pids)\" ]; then
			return 0
		fi
		sleep 1
		retries=\$((retries - 1))
	done
	return 1
}

current_live_exe() {
	for pid in \$(find_live_pids); do
		readlink -f \"/proc/\$pid/exe\" 2>/dev/null || true
		return 0
	done
	return 1
}

/etc/init.d/S31prudynt stop >/dev/null 2>&1 || true
if [ -n \"\$(find_live_pids)\" ]; then
	kill \$(find_live_pids) >/dev/null 2>&1 || true
fi
wait_for_live_exit 5 || true
if [ -n \"\$(find_live_pids)\" ]; then
	kill -9 \$(find_live_pids) >/dev/null 2>&1 || true
fi
wait_for_live_exit 5
rm -f /run/prudynt.pid
mv '$REMOTE_STAGING' '$REMOTE_DIR'
ln -sfn '$REMOTE_DIR/prudynt' /opt/prudynt.patched
ln -sfn /opt/prudynt.patched /usr/bin/prudynt
/etc/init.d/S31prudynt start
sleep 2

EXPECTED_EXE='$REMOTE_DIR/bin/prudynt.real'
RUNNING_EXE=\$(current_live_exe || true)
if [ \"\$RUNNING_EXE\" != \"\$EXPECTED_EXE\" ]; then
	echo \"Expected live prudynt executable \$EXPECTED_EXE but found \${RUNNING_EXE:-none}\" >&2
	exit 1
fi
"

echo "Installed bundle from $BUNDLE_DIR to $REMOTE_HOST:$REMOTE_DIR"
