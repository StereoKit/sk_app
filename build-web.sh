#!/bin/bash
# Build script for Web (Emscripten/WASM)
#
# Usage:
#   ./build-web.sh              # Debug build
#   ./build-web.sh --release    # Release build
#   ./build-web.sh --test       # Build, then run test_window in headless Firefox
#   ./build-web.sh --run NAME   # Build, then open an example (e.g. minimal_window)

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Parse command line arguments
BUILD_TYPE="Debug"
BUILD_DIR="build-web"
RUN_TEST=false
RUN_TARGET=""

while [[ $# -gt 0 ]]; do
	case $1 in
		-r|--release)
			BUILD_TYPE="Release"
			BUILD_DIR="build-web-release"
			shift
			;;
		-t|--test)
			RUN_TEST=true
			shift
			;;
		--run)
			RUN_TARGET="$2"
			if [ -z "$RUN_TARGET" ]; then
				echo "Error: --run requires an example name (e.g. --run minimal_window)"
				exit 1
			fi
			shift 2
			;;
		-h|--help)
			echo "Usage: $0 [options]"
			echo ""
			echo "Options:"
			echo "  -r, --release    Build in Release mode (default: Debug)"
			echo "  -t, --test       Run test_window --test in headless Firefox after building"
			echo "  --run NAME       Open an example in the default browser after building"
			echo "  -h, --help       Show this help"
			exit 0
			;;
		*)
			echo "Unknown option: $1"
			exit 1
			;;
	esac
done

# Find Emscripten: use emcc if already on PATH, otherwise source emsdk_env.sh
# from $EMSDK or a couple of common install locations
if ! command -v emcc &> /dev/null; then
	for emsdk_dir in "$EMSDK" "$HOME/SK/emsdk" "$HOME/emsdk"; do
		if [ -n "$emsdk_dir" ] && [ -f "$emsdk_dir/emsdk_env.sh" ]; then
			source "$emsdk_dir/emsdk_env.sh" > /dev/null 2>&1
			break
		fi
	done
fi

if ! command -v emcc &> /dev/null; then
	echo "Error: Emscripten not found. Install emsdk and either put emcc on PATH,"
	echo "set EMSDK to the install directory, or install to ~/emsdk:"
	echo "  git clone https://github.com/emscripten-core/emsdk.git"
	echo "  ./emsdk/emsdk install latest && ./emsdk/emsdk activate latest"
	exit 1
fi

echo "========================================="
echo "Building sk_app for Web (Emscripten)"
echo "========================================="
echo "emcc:      $(emcc --version | head -1)"
echo "Type:      $BUILD_TYPE"
echo "Build dir: $BUILD_DIR"
echo "========================================="

# Configure. --emrun instruments pages so emrun can capture stdout/exit codes
# (used by --test); it is inert when the page is served any other way.
emcmake cmake -B $BUILD_DIR \
	-DCMAKE_BUILD_TYPE=$BUILD_TYPE \
	-DCMAKE_EXE_LINKER_FLAGS=--emrun

# Build
cmake --build $BUILD_DIR -j$(nproc)

# Show results
PAGE_COUNT=0
echo "========================================="
for page in $(find $BUILD_DIR/examples -name "*.html" 2>/dev/null); do
	PAGE_COUNT=$((PAGE_COUNT + 1))
	wasm="${page%.html}.wasm"
	if [ -f "$wasm" ]; then
		echo "Page built: $(ls -lh "$wasm" | awk '{print $5}') (wasm) $page"
	else
		echo "Page built: $page"
	fi
	echo "  emrun $page"
	echo ""
done

if [ "$PAGE_COUNT" -eq 0 ]; then
	echo "Error: No pages found!"
	exit 1
fi
echo "========================================="

# Optionally open an example in the default browser
if [ -n "$RUN_TARGET" ]; then
	PAGE="$BUILD_DIR/examples/$RUN_TARGET/$RUN_TARGET.html"
	if [ ! -f "$PAGE" ]; then
		echo "Error: $PAGE not found"
		exit 1
	fi
	emrun "$PAGE"
fi

# Optionally run the test suite in headless Firefox
if [ "$RUN_TEST" = true ]; then
	if ! command -v firefox &> /dev/null; then
		echo "Error: --test needs Firefox for a headless browser run"
		exit 1
	fi

	echo ""
	echo "Running test_window --test in headless Firefox..."
	TEST_OUTPUT=$(timeout 180 emrun --port 6941 \
		--browser firefox --browser_args="-headless" \
		--kill_exit --timeout 120 \
		$BUILD_DIR/examples/test_window/test_window.html -- --test 2>&1 || true)

	echo "$TEST_OUTPUT" | grep -E "^\[INFO\]|^\[ERROR\]" | sed 's/^/  /' | tail -12

	if echo "$TEST_OUTPUT" | grep -q "\[TEST RESULT: PASS\]"; then
		echo "Web test suite: PASS"
	else
		echo "Web test suite: FAIL"
		exit 1
	fi
fi
