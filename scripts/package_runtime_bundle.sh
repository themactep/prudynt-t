#!/bin/bash
set -euo pipefail

usage() {
	cat <<EOF
Usage: $(basename "$0") <binary-path> <output-dir>

Creates a self-contained prudynt runtime bundle with:
- the target binary under bin/prudynt.real
- required shared libraries under lib/
- a wrapper script that sets LD_LIBRARY_PATH relative to itself
EOF
}

if [[ $# -ne 2 ]]; then
	usage
	exit 1
fi

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY_PATH="$(readlink -f "$1")"
OUTPUT_DIR="$2"
LIB_DIR="$ROOT_DIR/3rdparty/install/lib"

if [[ ! -f "$BINARY_PATH" ]]; then
	echo "Binary not found: $BINARY_PATH" >&2
	exit 1
fi

if [[ ! -d "$LIB_DIR" ]]; then
	echo "Library directory not found: $LIB_DIR" >&2
	exit 1
fi

declare -A SKIP_NEEDED=(
	["ld-musl-mipsel.so.1"]=1
	["libalog.so"]=1
	["libaudioProcess.so"]=1
	["libc.so"]=1
	["libdl.so.0"]=1
	["libgcc_s.so.1"]=1
	["libimp.so"]=1
	["libm.so.6"]=1
	["libpthread.so.0"]=1
	["librt.so.1"]=1
	["libstdc++.so.6"]=1
	["libsysutils.so"]=1
)

if [[ -n "${PRUDYNT_BUNDLE_SKIP_LIBS:-}" ]]; then
	for lib in ${PRUDYNT_BUNDLE_SKIP_LIBS}; do
		SKIP_NEEDED["$lib"]=1
	done
fi

declare -A SONAME_TO_PATH=()
declare -A REALPATH_TO_DEST=()
declare -A QUEUED=()
QUEUE=()

register_library() {
	local candidate="$1"
	local real_path
	real_path="$(readlink -f "$candidate")"
	[[ -f "$real_path" ]] || return 0

	local base_name
	base_name="$(basename "$candidate")"
	SONAME_TO_PATH["$base_name"]="$real_path"

	local real_name
	real_name="$(basename "$real_path")"
	SONAME_TO_PATH["$real_name"]="$real_path"

	local soname
	soname="$(readelf -d "$real_path" 2>/dev/null | awk -F'[][]' '/SONAME/{print $2; exit}')"
	if [[ -n "$soname" ]]; then
		SONAME_TO_PATH["$soname"]="$real_path"
	fi
}

while IFS= read -r -d '' candidate; do
	register_library "$candidate"
done < <(find "$LIB_DIR" -maxdepth 1 \( -type f -o -type l \) -name 'lib*.so*' -print0)

enqueue_needed() {
	local object_path="$1"
	local dependency
	while IFS= read -r dependency; do
		[[ -n "$dependency" ]] || continue
		[[ -n "${SKIP_NEEDED[$dependency]:-}" ]] && continue
		if [[ -z "${QUEUED[$dependency]:-}" ]]; then
			QUEUED["$dependency"]=1
			QUEUE+=("$dependency")
		fi
	done < <(readelf -d "$object_path" 2>/dev/null | awk -F'[][]' '/NEEDED/{print $2}')
}

mkdir -p "$OUTPUT_DIR/bin" "$OUTPUT_DIR/lib"
cp -f "$BINARY_PATH" "$OUTPUT_DIR/bin/prudynt.real"

enqueue_needed "$BINARY_PATH"

copy_library() {
	local needed="$1"
	local source_path="${SONAME_TO_PATH[$needed]:-}"
	if [[ -z "$source_path" ]]; then
		echo "Unable to resolve shared library: $needed" >&2
		exit 1
	fi

	local source_real
	source_real="$(readlink -f "$source_path")"
	local source_base
	source_base="$(basename "$source_real")"
	local dest_real="$OUTPUT_DIR/lib/$source_base"

	if [[ -z "${REALPATH_TO_DEST[$source_real]:-}" ]]; then
		rm -f "$dest_real"
		install -m 0644 "$source_real" "$dest_real"
		REALPATH_TO_DEST["$source_real"]="$dest_real"

		local file_name
		file_name="$(basename "$source_path")"
		if [[ "$file_name" != "$source_base" ]]; then
			ln -sfn "$source_base" "$OUTPUT_DIR/lib/$file_name"
		fi

		local soname
		soname="$(readelf -d "$source_real" 2>/dev/null | awk -F'[][]' '/SONAME/{print $2; exit}')"
		if [[ -n "$soname" && "$soname" != "$source_base" ]]; then
			ln -sfn "$source_base" "$OUTPUT_DIR/lib/$soname"
		fi

		enqueue_needed "$source_real"
	fi

	if [[ "$needed" != "$source_base" ]]; then
		ln -sfn "$source_base" "$OUTPUT_DIR/lib/$needed"
	fi
}

while [[ ${#QUEUE[@]} -gt 0 ]]; do
	needed="${QUEUE[0]}"
	QUEUE=("${QUEUE[@]:1}")
	copy_library "$needed"
done

cat > "$OUTPUT_DIR/prudynt" <<'EOF'
#!/bin/sh
set -eu

resolve_self() {
	target="$1"
	while [ -L "$target" ]; do
		dir=$(dirname "$target")
		link=$(readlink "$target")
		case "$link" in
			/*) target="$link" ;;
			*) target="$dir/$link" ;;
		esac
	done
	printf '%s\n' "$target"
}

SELF=$(resolve_self "$0")
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$SELF")" && pwd)
export LD_LIBRARY_PATH="$ROOT_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ -f "$ROOT_DIR/lib/libmuslshim.so" ]; then
	export LD_PRELOAD="$ROOT_DIR/lib/libmuslshim.so${LD_PRELOAD:+ $LD_PRELOAD}"
fi
exec "$ROOT_DIR/bin/prudynt.real" "$@"
EOF
chmod 755 "$OUTPUT_DIR/prudynt"

# Thingino cameras on musl often expose only /lib/libc.so while vendor libs
# still declare glibc-style SONAMEs such as libc.so.0 and libpthread.so.0.
# Provide compatibility symlinks inside the bundle so bundled live555/json/etc.
# can coexist with platform IMP libs without touching system libraries.
ln -sfn /lib/libc.so "$OUTPUT_DIR/lib/libc.so.0"
ln -sfn /lib/libc.so "$OUTPUT_DIR/lib/libpthread.so.0"
ln -sfn /lib/libc.so "$OUTPUT_DIR/lib/libdl.so.0"
ln -sfn /lib/libc.so "$OUTPUT_DIR/lib/libm.so.0"
ln -sfn /lib/libc.so "$OUTPUT_DIR/lib/librt.so.1"

{
	echo "binary=$(basename "$BINARY_PATH")"
	echo "created_at=$(date -u +%FT%TZ)"
	find "$OUTPUT_DIR/lib" -maxdepth 1 \( -type f -o -type l \) | sort | sed "s#^$OUTPUT_DIR/##"
} > "$OUTPUT_DIR/MANIFEST.txt"

echo "Runtime bundle created at $OUTPUT_DIR"
