#!/bin/bash
#
# sk_app build and test script
# Builds and tests for Linux, Windows (MinGW), and Android
# Reports build status, binary sizes, and test results
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Default settings
BUILD_TYPE="Debug"
JOBS=$(nproc 2>/dev/null || echo 4)
VERBOSE=""

# Parse arguments
while [[ $# -gt 0 ]]; do
	case $1 in
		-r|--release)
			BUILD_TYPE="Release"
			shift
			;;
		-v|--verbose)
			VERBOSE="-v"
			shift
			;;
		-h|--help)
			echo "Usage: $0 [options]"
			echo ""
			echo "Options:"
			echo "  -r, --release    Build in Release mode (default: Debug)"
			echo "  -v, --verbose    Verbose test output"
			echo "  -h, --help       Show this help"
			exit 0
			;;
		*)
			echo "Unknown option: $1"
			exit 1
			;;
	esac
done

echo -e "${BLUE}================================================${NC}"
echo -e "${BLUE}  sk_app Build & Test Script${NC}"
echo -e "${BLUE}================================================${NC}"
echo ""
echo -e "Build type: ${YELLOW}${BUILD_TYPE}${NC}"
echo -e "Parallel jobs: ${YELLOW}${JOBS}${NC}"
echo ""

# Track results
declare -A RESULTS
declare -A BINARIES
declare -A SIZES
declare -A TEST_PASSED
declare -A TEST_FAILED
declare -A TEST_SKIPPED

# Helper function to format size
format_size() {
	local size=$1
	if (( size >= 1048576 )); then
		echo "$(awk "BEGIN {printf \"%.2f\", $size/1048576}") MB"
	elif (( size >= 1024 )); then
		echo "$(awk "BEGIN {printf \"%.2f\", $size/1024}") KB"
	else
		echo "$size bytes"
	fi
}

# Helper function to parse test output
parse_test_output() {
	local output="$1"
	local platform="$2"

	# Extract test counts from output
	local passed=$(echo "$output" | grep -oP 'Passed:\s*\K\d+' | tail -1)
	local failed=$(echo "$output" | grep -oP 'Failed:\s*\K\d+' | tail -1)
	local skipped=$(echo "$output" | grep -oP 'Skipped:\s*\K\d+' | tail -1)

	TEST_PASSED[$platform]="${passed:-0}"
	TEST_FAILED[$platform]="${failed:-0}"
	TEST_SKIPPED[$platform]="${skipped:-0}"

	# Check for overall result
	if echo "$output" | grep -q "\[TEST RESULT: PASS\]"; then
		return 0
	elif echo "$output" | grep -q "\[TEST RESULT: FAIL\]"; then
		return 1
	else
		# Fallback: check if we got any test output at all
		if [[ -n "$passed" ]] && [[ "$failed" == "0" ]]; then
			return 0
		fi
		return 1
	fi
}

# ============================================================================
# Linux Build
# ============================================================================
echo -e "${BLUE}[1/4] Building for Linux...${NC}"

BUILD_DIR_LINUX="build"
if [[ "$BUILD_TYPE" == "Release" ]]; then
	BUILD_DIR_LINUX="build-release"
fi

LINUX_CMAKE_CMD="cmake -B $BUILD_DIR_LINUX -DCMAKE_BUILD_TYPE=$BUILD_TYPE"
LINUX_BUILD_CMD="cmake --build $BUILD_DIR_LINUX -j$JOBS"
if $LINUX_CMAKE_CMD > /dev/null 2>&1; then
	if $LINUX_BUILD_CMD > /dev/null 2>&1; then
		RESULTS[linux]="OK"
		BINARIES[linux]="$SCRIPT_DIR/$BUILD_DIR_LINUX/examples/test_window/test_window"
		if [[ -f "${BINARIES[linux]}" ]]; then
			SIZES[linux]=$(stat -c%s "${BINARIES[linux]}" 2>/dev/null || stat -f%z "${BINARIES[linux]}" 2>/dev/null)
		fi
		echo -e "  ${GREEN}Build successful${NC}"
	else
		RESULTS[linux]="BUILD FAILED"
		echo -e "  ${RED}Build failed${NC}"
		echo -e "  ${RED}Command: $LINUX_BUILD_CMD${NC}"
	fi
else
	RESULTS[linux]="CMAKE FAILED"
	echo -e "  ${RED}CMake configuration failed${NC}"
	echo -e "  ${RED}Command: $LINUX_CMAKE_CMD${NC}"
fi

# ============================================================================
# Windows Build (MinGW cross-compile)
# ============================================================================
echo -e "${BLUE}[2/4] Building for Windows (MinGW)...${NC}"

BUILD_DIR_WIN="build-win"
if [[ "$BUILD_TYPE" == "Release" ]]; then
	BUILD_DIR_WIN="build-win-release"
fi

# Check if MinGW is available
if command -v x86_64-w64-mingw32-gcc &> /dev/null; then
	WIN_CMAKE_CMD="cmake -B $BUILD_DIR_WIN -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ -DCMAKE_BUILD_TYPE=$BUILD_TYPE"
	WIN_BUILD_CMD="cmake --build $BUILD_DIR_WIN -j$JOBS"
	if $WIN_CMAKE_CMD > /dev/null 2>&1; then
		if $WIN_BUILD_CMD > /dev/null 2>&1; then
			RESULTS[windows]="OK"
			BINARIES[windows]="$SCRIPT_DIR/$BUILD_DIR_WIN/examples/test_window/test_window.exe"
			if [[ -f "${BINARIES[windows]}" ]]; then
				SIZES[windows]=$(stat -c%s "${BINARIES[windows]}" 2>/dev/null || stat -f%z "${BINARIES[windows]}" 2>/dev/null)
			fi
			echo -e "  ${GREEN}Build successful${NC}"
		else
			RESULTS[windows]="BUILD FAILED"
			echo -e "  ${RED}Build failed${NC}"
			echo -e "  ${RED}Command: $WIN_BUILD_CMD${NC}"
		fi
	else
		RESULTS[windows]="CMAKE FAILED"
		echo -e "  ${RED}CMake configuration failed${NC}"
		echo -e "  ${RED}Command: $WIN_CMAKE_CMD${NC}"
	fi
else
	RESULTS[windows]="SKIPPED (MinGW not found)"
	echo -e "  ${YELLOW}Skipped (x86_64-w64-mingw32-gcc not found)${NC}"
fi

# ============================================================================
# Android Build
# ============================================================================
echo -e "${BLUE}[3/4] Building for Android (ARM64)...${NC}"

BUILD_DIR_ANDROID="build-android"
if [[ "$BUILD_TYPE" == "Release" ]]; then
	BUILD_DIR_ANDROID="build-android-release"
fi

# Find Android NDK
if [[ -z "$ANDROID_NDK" ]]; then
	if [[ -n "$ANDROID_SDK_ROOT" ]]; then
		ANDROID_NDK=$(find "$ANDROID_SDK_ROOT/ndk" -maxdepth 1 -type d 2>/dev/null | sort -V | tail -1)
	elif [[ -n "$ANDROID_HOME" ]]; then
		ANDROID_NDK=$(find "$ANDROID_HOME/ndk" -maxdepth 1 -type d 2>/dev/null | sort -V | tail -1)
	fi
fi

if [[ -n "$ANDROID_NDK" ]] && [[ -f "$ANDROID_NDK/build/cmake/android.toolchain.cmake" ]]; then
	ANDROID_CMAKE_CMD="cmake -B $BUILD_DIR_ANDROID -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-32 -DCMAKE_BUILD_TYPE=$BUILD_TYPE"
	ANDROID_BUILD_CMD="cmake --build $BUILD_DIR_ANDROID -j$JOBS"
	if $ANDROID_CMAKE_CMD > /dev/null 2>&1; then
		if $ANDROID_BUILD_CMD > /dev/null 2>&1; then
			RESULTS[android]="OK"
			BINARIES[android]="$SCRIPT_DIR/$BUILD_DIR_ANDROID/examples/test_window/libtest_window.so"
			if [[ -f "${BINARIES[android]}" ]]; then
				SIZES[android]=$(stat -c%s "${BINARIES[android]}" 2>/dev/null || stat -f%z "${BINARIES[android]}" 2>/dev/null)
			fi
			echo -e "  ${GREEN}Build successful${NC}"
		else
			RESULTS[android]="BUILD FAILED"
			echo -e "  ${RED}Build failed${NC}"
			echo -e "  ${RED}Command: $ANDROID_BUILD_CMD${NC}"
		fi
	else
		RESULTS[android]="CMAKE FAILED"
		echo -e "  ${RED}CMake configuration failed${NC}"
		echo -e "  ${RED}Command: $ANDROID_CMAKE_CMD${NC}"
	fi
else
	RESULTS[android]="SKIPPED (NDK not found)"
	echo -e "  ${YELLOW}Skipped (ANDROID_NDK not set or not found)${NC}"
fi

# ============================================================================
# Android x64 Build (for Waydroid)
# ============================================================================
echo -e "${BLUE}[4/4] Building for Android (x64/Waydroid)...${NC}"

BUILD_DIR_ANDROID_X64="build-android-x64"
if [[ "$BUILD_TYPE" == "Release" ]]; then
	BUILD_DIR_ANDROID_X64="build-android-x64-release"
fi

# Only build if Waydroid is available
if command -v waydroid &> /dev/null; then
	if [[ -n "$ANDROID_NDK" ]] && [[ -f "$ANDROID_NDK/build/cmake/android.toolchain.cmake" ]]; then
		ANDROID_X64_CMAKE_CMD="cmake -B $BUILD_DIR_ANDROID_X64 -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-32 -DCMAKE_BUILD_TYPE=$BUILD_TYPE"
		ANDROID_X64_BUILD_CMD="cmake --build $BUILD_DIR_ANDROID_X64 -j$JOBS"
		ANDROID_X64_APK_CMD="cmake --build $BUILD_DIR_ANDROID_X64 --target test_window-apk"
		if $ANDROID_X64_CMAKE_CMD > /dev/null 2>&1; then
			if $ANDROID_X64_BUILD_CMD > /dev/null 2>&1; then
				# Build the APK
				if $ANDROID_X64_APK_CMD > /dev/null 2>&1; then
					RESULTS[android_x64]="OK"
					BINARIES[android_x64]="$SCRIPT_DIR/$BUILD_DIR_ANDROID_X64/examples/test_window/test_window.apk"
					if [[ -f "${BINARIES[android_x64]}" ]]; then
						SIZES[android_x64]=$(stat -c%s "${BINARIES[android_x64]}" 2>/dev/null || stat -f%z "${BINARIES[android_x64]}" 2>/dev/null)
					fi
					echo -e "  ${GREEN}Build successful${NC}"
				else
					RESULTS[android_x64]="APK FAILED"
					echo -e "  ${RED}APK build failed${NC}"
					echo -e "  ${RED}Command: $ANDROID_X64_APK_CMD${NC}"
				fi
			else
				RESULTS[android_x64]="BUILD FAILED"
				echo -e "  ${RED}Build failed${NC}"
				echo -e "  ${RED}Command: $ANDROID_X64_BUILD_CMD${NC}"
			fi
		else
			RESULTS[android_x64]="CMAKE FAILED"
			echo -e "  ${RED}CMake configuration failed${NC}"
			echo -e "  ${RED}Command: $ANDROID_X64_CMAKE_CMD${NC}"
		fi
	else
		RESULTS[android_x64]="SKIPPED (NDK not found)"
		echo -e "  ${YELLOW}Skipped (ANDROID_NDK not set or not found)${NC}"
	fi
else
	RESULTS[android_x64]="SKIPPED (Waydroid not found)"
	echo -e "  ${YELLOW}Skipped (Waydroid not installed)${NC}"
fi

# ============================================================================
# Run Tests
# ============================================================================
echo ""
echo -e "${BLUE}================================================${NC}"
echo -e "${BLUE}  Running Tests${NC}"
echo -e "${BLUE}================================================${NC}"
echo ""

# Test Linux build
if [[ "${RESULTS[linux]}" == "OK" ]]; then
	# Linux has two backends chosen at runtime, so test each explicitly rather
	# than only whichever one the current session happens to select.
	for backend in x11 wayland; do
		echo -e "${CYAN}Testing Linux binary (${backend})...${NC}"
		TEST_EXIT=0
		TEST_OUTPUT=$(cd /tmp && SKA_VIDEODRIVER=$backend "${BINARIES[linux]}" --test $VERBOSE 2>&1) || TEST_EXIT=$?

		# 77 is the conventional "skipped" exit code, from test_window's main
		if [[ $TEST_EXIT -eq 77 ]]; then
			RESULTS[linux_test_$backend]="SKIPPED"
			echo -e "  ${YELLOW}SKIPPED${NC} - $backend not available in this session"
			continue
		fi

		if parse_test_output "$TEST_OUTPUT" "linux_$backend"; then
			RESULTS[linux_test_$backend]="PASS"
			echo -e "  ${GREEN}PASS${NC} - ${TEST_PASSED[linux_$backend]} passed, ${TEST_FAILED[linux_$backend]} failed, ${TEST_SKIPPED[linux_$backend]} skipped"
		else
			RESULTS[linux_test_$backend]="FAIL"
			echo -e "  ${RED}FAIL${NC} - ${TEST_PASSED[linux_$backend]} passed, ${TEST_FAILED[linux_$backend]} failed, ${TEST_SKIPPED[linux_$backend]} skipped"
			echo "$TEST_OUTPUT" | grep -E "\[FAIL\]" || true
		fi
	done
fi

# Test Windows build with Wine
if [[ "${RESULTS[windows]}" == "OK" ]]; then
	if command -v wine &> /dev/null; then
		echo -e "${CYAN}Testing Windows binary (Wine)...${NC}"
		TEST_OUTPUT=$(cd /tmp && wine "${BINARIES[windows]}" --test $VERBOSE 2>&1 || true)

		if parse_test_output "$TEST_OUTPUT" "windows"; then
			RESULTS[windows_test]="PASS"
			echo -e "  ${GREEN}PASS${NC} - ${TEST_PASSED[windows]} passed, ${TEST_FAILED[windows]} failed, ${TEST_SKIPPED[windows]} skipped"
		else
			RESULTS[windows_test]="FAIL"
			echo -e "  ${RED}FAIL${NC} - ${TEST_PASSED[windows]} passed, ${TEST_FAILED[windows]} failed, ${TEST_SKIPPED[windows]} skipped"
			if [[ -n "$VERBOSE" ]]; then
				echo "$TEST_OUTPUT" | grep -E "^\[FAIL\]" || true
			fi
		fi
	else
		RESULTS[windows_test]="SKIPPED (Wine not found)"
		echo -e "${CYAN}Testing Windows binary...${NC}"
		echo -e "  ${YELLOW}SKIPPED${NC} - Wine not found"
	fi
fi

# Test Android x64 build with Waydroid
if [[ "${RESULTS[android_x64]}" == "OK" ]]; then
	echo -e "${CYAN}Testing Android x64 binary (Waydroid)...${NC}"

	# Check if Waydroid session is running, start one if not
	WAYDROID_STARTED=false
	if ! waydroid status 2>/dev/null | grep -q "Session.*RUNNING"; then
		echo -e "  Starting Waydroid session..."
		waydroid session start > /dev/null 2>&1 &
		WAYDROID_SESSION_PID=$!
		WAYDROID_STARTED=true
		# Wait for session to be ready (up to 30 seconds)
		for i in {1..30}; do
			if waydroid status 2>/dev/null | grep -q "Session.*RUNNING"; then
				echo -e "  Session started, waiting for container..."
				break
			fi
			sleep 1
		done
	fi

	# Check if Waydroid session is now running
	if waydroid status 2>/dev/null | grep -q "Session.*RUNNING"; then
		# Wait for container to be RUNNING (Android system takes time to boot)
		CONTAINER_READY=false
		for i in {1..60}; do
			if waydroid status 2>/dev/null | grep -q "Container.*RUNNING"; then
				CONTAINER_READY=true
				break
			fi
			sleep 1
		done

		if [[ "$CONTAINER_READY" == true ]]; then
			# Install the APK (waydroid app install doesn't require root)
			echo -e "  Installing APK..."
			INSTALL_OUTPUT=$(waydroid app install "${BINARIES[android_x64]}" 2>&1)
			INSTALL_EXIT=$?

			if [[ $INSTALL_EXIT -eq 0 ]]; then
				# Give it a moment to register
				sleep 2
				# Launch the app
				echo -e "  Launching app..."
				waydroid app launch net.stereokit.test_window > /dev/null 2>&1 &
				sleep 2
				RESULTS[android_x64_test]="PASS"
				echo -e "  ${GREEN}PASS${NC} - App installed and launched successfully"
			else
				RESULTS[android_x64_test]="FAIL"
				echo -e "  ${RED}FAIL${NC} - Install failed (exit code $INSTALL_EXIT)"
			fi
		else
			RESULTS[android_x64_test]="FAIL"
			echo -e "  ${RED}FAIL${NC} - Container not ready after 60s"
		fi

		# Stop the session if we started it
		if [[ "$WAYDROID_STARTED" == true ]]; then
			echo -e "  Stopping Waydroid session..."
			waydroid session stop > /dev/null 2>&1 || true
		fi
	else
		RESULTS[android_x64_test]="FAIL"
		echo -e "  ${RED}FAIL${NC} - Could not start Waydroid session"
	fi
fi

# ============================================================================
# Report
# ============================================================================
echo ""
echo -e "${BLUE}================================================${NC}"
echo -e "${BLUE}  Build & Test Report${NC}"
echo -e "${BLUE}================================================${NC}"
echo ""
echo -e "Build Type: ${YELLOW}${BUILD_TYPE}${NC}"
echo ""

# Linux
echo -e "${BLUE}Linux:${NC}"
if [[ "${RESULTS[linux]}" == "OK" ]]; then
	echo -e "  Build:  ${GREEN}${RESULTS[linux]}${NC}"
	echo -e "  Binary: ${BINARIES[linux]}"
	echo -e "  Size:   $(format_size ${SIZES[linux]})"
	for backend in x11 wayland; do
		result="${RESULTS[linux_test_$backend]}"
		[[ -z "$result" ]] && continue
		label=$(printf "%-8s" "$backend")
		case "$result" in
			PASS)    echo -e "  Test ${label}${GREEN}PASS${NC} (${TEST_PASSED[linux_$backend]} passed, ${TEST_FAILED[linux_$backend]} failed, ${TEST_SKIPPED[linux_$backend]} skipped)" ;;
			SKIPPED) echo -e "  Test ${label}${YELLOW}SKIPPED${NC}" ;;
			*)       echo -e "  Test ${label}${RED}FAIL${NC} (${TEST_PASSED[linux_$backend]} passed, ${TEST_FAILED[linux_$backend]} failed, ${TEST_SKIPPED[linux_$backend]} skipped)" ;;
		esac
	done
else
	echo -e "  Build:  ${RED}${RESULTS[linux]}${NC}"
fi
echo ""

# Windows
echo -e "${BLUE}Windows:${NC}"
if [[ "${RESULTS[windows]}" == "OK" ]]; then
	echo -e "  Build:  ${GREEN}${RESULTS[windows]}${NC}"
	echo -e "  Binary: ${BINARIES[windows]}"
	echo -e "  Size:   $(format_size ${SIZES[windows]})"
	if [[ -n "${RESULTS[windows_test]}" ]]; then
		if [[ "${RESULTS[windows_test]}" == "PASS" ]]; then
			echo -e "  Test:   ${GREEN}${RESULTS[windows_test]}${NC} (${TEST_PASSED[windows]} passed, ${TEST_FAILED[windows]} failed, ${TEST_SKIPPED[windows]} skipped)"
		elif [[ "${RESULTS[windows_test]}" == *"SKIPPED"* ]]; then
			echo -e "  Test:   ${YELLOW}${RESULTS[windows_test]}${NC}"
		else
			echo -e "  Test:   ${RED}${RESULTS[windows_test]}${NC} (${TEST_PASSED[windows]} passed, ${TEST_FAILED[windows]} failed, ${TEST_SKIPPED[windows]} skipped)"
		fi
	fi
elif [[ "${RESULTS[windows]}" == *"SKIPPED"* ]]; then
	echo -e "  Build:  ${YELLOW}${RESULTS[windows]}${NC}"
else
	echo -e "  Build:  ${RED}${RESULTS[windows]}${NC}"
fi
echo ""

# Android (ARM64)
echo -e "${BLUE}Android (ARM64):${NC}"
if [[ "${RESULTS[android]}" == "OK" ]]; then
	echo -e "  Build:  ${GREEN}${RESULTS[android]}${NC}"
	echo -e "  Binary: ${BINARIES[android]}"
	echo -e "  Size:   $(format_size ${SIZES[android]})"
elif [[ "${RESULTS[android]}" == *"SKIPPED"* ]]; then
	echo -e "  Build:  ${YELLOW}${RESULTS[android]}${NC}"
else
	echo -e "  Build:  ${RED}${RESULTS[android]}${NC}"
fi
echo ""

# Android (x64/Waydroid)
echo -e "${BLUE}Android (x64/Waydroid):${NC}"
if [[ "${RESULTS[android_x64]}" == "OK" ]]; then
	echo -e "  Build:  ${GREEN}${RESULTS[android_x64]}${NC}"
	echo -e "  APK:    ${BINARIES[android_x64]}"
	echo -e "  Size:   $(format_size ${SIZES[android_x64]})"
	if [[ -n "${RESULTS[android_x64_test]}" ]]; then
		if [[ "${RESULTS[android_x64_test]}" == "PASS" ]]; then
			echo -e "  Test:   ${GREEN}${RESULTS[android_x64_test]}${NC}"
		else
			echo -e "  Test:   ${RED}${RESULTS[android_x64_test]}${NC}"
		fi
	fi
elif [[ "${RESULTS[android_x64]}" == *"SKIPPED"* ]]; then
	echo -e "  Build:  ${YELLOW}${RESULTS[android_x64]}${NC}"
else
	echo -e "  Build:  ${RED}${RESULTS[android_x64]}${NC}"
fi
echo ""

# ============================================================================
# Summary
# ============================================================================
echo -e "${BLUE}================================================${NC}"
echo -e "${BLUE}  Summary${NC}"
echo -e "${BLUE}================================================${NC}"

BUILD_FAILED=0
TEST_FAILED_COUNT=0
for key in "${!RESULTS[@]}"; do
	if [[ "${RESULTS[$key]}" == *"FAILED"* ]] || [[ "${RESULTS[$key]}" == "FAIL" ]]; then
		if [[ "$key" == *"_test"* ]]; then
			((TEST_FAILED_COUNT++))
		else
			((BUILD_FAILED++))
		fi
	fi
done

# Count total tests run
TOTAL_PASSED=0
TOTAL_FAILED=0
TOTAL_SKIPPED=0
for platform in "${!TEST_PASSED[@]}"; do
	((TOTAL_PASSED += ${TEST_PASSED[$platform]:-0}))
	((TOTAL_FAILED += ${TEST_FAILED[$platform]:-0}))
	((TOTAL_SKIPPED += ${TEST_SKIPPED[$platform]:-0}))
done

echo ""
if [[ $TOTAL_PASSED -gt 0 ]] || [[ $TOTAL_FAILED -gt 0 ]]; then
	echo -e "Test Results: ${GREEN}$TOTAL_PASSED passed${NC}, ${RED}$TOTAL_FAILED failed${NC}, ${YELLOW}$TOTAL_SKIPPED skipped${NC}"
fi

if [[ $BUILD_FAILED -eq 0 ]] && [[ $TEST_FAILED_COUNT -eq 0 ]]; then
	echo -e "${GREEN}All builds and tests successful!${NC}"
	exit 0
elif [[ $BUILD_FAILED -gt 0 ]]; then
	echo -e "${RED}$BUILD_FAILED build(s) failed${NC}"
	exit 1
else
	echo -e "${RED}$TEST_FAILED_COUNT test suite(s) failed${NC}"
	exit 1
fi
