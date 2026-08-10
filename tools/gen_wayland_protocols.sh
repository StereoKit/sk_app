#!/bin/bash
#
# Regenerates the Wayland protocol sources checked into src/wayland/.
#
# The output is committed rather than generated at build time. Protocols move
# between unstable, staging, and stable as they mature, and distros ship a wide
# spread of wayland-protocols versions, so building from the local XML made the
# available protocol set depend on the build machine. Generating once and
# committing the result means every build compiles identical code, needs no
# wayland-scanner, and cross-compiles without a target-side protocol package.
#
# Run this when adding a protocol or picking up upstream protocol changes, then
# commit the result. Needs wayland-scanner and a recent wayland-protocols.
#
# Usage: tools/gen_wayland_protocols.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/../src/wayland"
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

# name:candidate paths, in preference order. A protocol promoted to stable
# keeps its interface names, so either spelling generates equivalent code and
# only the file name differs.
#
# Only protocols the backend actually uses belong here. Each one costs real
# source: the marshalling tables in the .c plus a header of inline wrappers.
# Add a line when the code that needs it lands, not before.
PROTOCOLS=(
	"xdg-shell:stable/xdg-shell/xdg-shell.xml"
	"xdg-decoration-unstable-v1:unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
	"viewporter:stable/viewporter/viewporter.xml"
	"fractional-scale-v1:staging/fractional-scale/fractional-scale-v1.xml"
	"relative-pointer-unstable-v1:unstable/relative-pointer/relative-pointer-unstable-v1.xml"
	"pointer-constraints-unstable-v1:unstable/pointer-constraints/pointer-constraints-unstable-v1.xml"
	"cursor-shape-v1:staging/cursor-shape/cursor-shape-v1.xml"
)

# Interfaces to drop before generating, space separated, keyed by protocol base
# name. Only whole interfaces can go, and wl_prune.py refuses any that another
# surviving interface still references, so this list cannot silently corrupt the
# message tables. wl_callback, wl_touch, and wl_region look unused but are
# referenced by wl_display, wl_seat, and wl_compositor respectively.
prune_list() {
	case "$1" in
		wayland-core)    echo "wl_shell wl_shell_surface wl_subcompositor wl_subsurface wl_fixes" ;;
		cursor-shape-v1) echo "wp_cursor_shape_manager_v1.get_tablet_tool_v2" ;;
		*)               echo "" ;;
	esac
}

# Still to come, with the protocol each needs:
#   clipboard           primary-selection-unstable-v1
#   window raise        xdg-activation-v1
#
# cursor-shape-v1 costs two interfaces rather than the tablet protocol as well:
# its only reference to zwp_tablet_tool_v2 is get_tablet_tool_v2, which is the
# last request on the manager, so pruning it renumbers nothing.

command -v wayland-scanner > /dev/null || { echo "error: wayland-scanner not found"; exit 1; }
pkg-config --exists wayland-protocols || { echo "error: wayland-protocols not found"; exit 1; }
pkg-config --exists wayland-scanner   || { echo "error: wayland-scanner pkg-config not found"; exit 1; }

PROTO_DIR=$(pkg-config --variable=pkgdatadir wayland-protocols)
CORE_XML=$(pkg-config --variable=pkgdatadir wayland-scanner)/wayland.xml
PROTO_VER=$(pkg-config --modversion wayland-protocols)
SCANNER_VER=$(wayland-scanner --version 2>&1 | head -1)

echo "wayland-protocols $PROTO_VER"
echo "$SCANNER_VER"
echo "output: $OUT_DIR"
echo

mkdir -p "$OUT_DIR"
# Named explicitly, not globbed: wayland-client.h lives here too and is
# hand-written, so a wildcard delete would take it out.
rm -f "$OUT_DIR/wayland-protocols.c" "$OUT_DIR/wayland-protocols.h"

# Order matters: the merged output keeps this order, and every other protocol
# refers to core interfaces, so core has to be declared first.
HEADERS=()
SOURCES=()
SOURCE_LIST=""

generate() {
	local base="$1"
	local xml="$2"
	local source_xml="$xml"
	local drop
	drop=$(prune_list "$base")
	if [[ -n "$drop" ]]; then
		python3 "$SCRIPT_DIR/wl_prune.py" "$xml" "$TMP_DIR/$base-pruned.xml" $drop
		xml="$TMP_DIR/$base-pruned.xml"
	fi
	wayland-scanner client-header "$xml" "$TMP_DIR/$base-client-protocol.h"
	wayland-scanner private-code  "$xml" "$TMP_DIR/$base-protocol.c"
	HEADERS+=("$TMP_DIR/$base-client-protocol.h")
	SOURCES+=("$TMP_DIR/$base-protocol.c")
	SOURCE_LIST="$SOURCE_LIST $base"
	printf "  %-34s %s\n" "$base" "${source_xml#$PROTO_DIR/}"
}

# The core protocol is generated too, so sk_app owns every wl_*_interface
# object and only functions ever need dlsym. Named "wayland-core-" so it does
# not collide with the system wayland-client-protocol.h.
generate "wayland-core" "$CORE_XML"

missing=0
for entry in "${PROTOCOLS[@]}"; do
	name="${entry%%:*}"
	paths="${entry#*:}"
	found=""
	IFS=',' read -ra candidates <<< "$paths"
	for candidate in "${candidates[@]}"; do
		if [[ -f "$PROTO_DIR/$candidate" ]]; then
			found="$PROTO_DIR/$candidate"
			break
		fi
	done
	if [[ -z "$found" ]]; then
		echo "  MISSING: $name (tried: $paths)"
		missing=1
		continue
	fi
	generate "$name" "$found"
done

if [[ $missing -ne 0 ]]; then
	echo
	echo "error: some protocols were not found. Update wayland-protocols and re-run."
	exit 1
fi

# One header and one source, comments stripped. The scanner emits a file pair
# per protocol and two thirds doc comment, which is a lot of generated prose to
# carry in the repo for no benefit.
BANNER="/* Generated by tools/gen_wayland_protocols.sh. DO NOT EDIT.
   protocols:$SOURCE_LIST
   wayland-protocols $PROTO_VER, $SCANNER_VER
   Comments stripped and files merged; re-run the script to regenerate. */"

python3 "$SCRIPT_DIR/wl_minify.py" "$OUT_DIR/wayland-protocols.h" "$BANNER" "${HEADERS[@]}"
python3 "$SCRIPT_DIR/wl_minify.py" "$OUT_DIR/wayland-protocols.c" "$BANNER" "${SOURCES[@]}"

echo
echo "Generated 2 files, $(cat "$OUT_DIR"/wayland-protocols.[ch] | wc -l) lines. Commit them."
