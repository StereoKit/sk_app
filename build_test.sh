#!/bin/bash
#
# sk_app build and test script
# Builds simple_window for Linux, Windows (MinGW), and Android
# Reports binary paths and sizes
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default settings
BUILD_TYPE="Debug"
JOBS=$(nproc 2>/dev/null || echo 4)

# Parse arguments
while [[ $# -gt 0 ]]; do
	case $1 in
		-r|--release)
			BUILD_TYPE="Release"
			shift
			;;
		-h|--help)
			echo "Usage: $0 [options]"
			echo ""
			echo "Options:"
			echo "  -r, --release    Build in Release mode (default: Debug)"
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

# ============================================================================
# Linux Build
# ============================================================================
echo -e "${BLUE}[1/3] Building for Linux...${NC}"

BUILD_DIR_LINUX="build"
if [[ "$BUILD_TYPE" == "Release" ]]; then
	BUILD_DIR_LINUX="build-release"
fi

LINUX_CMAKE_CMD="cmake -B $BUILD_DIR_LINUX -DCMAKE_BUILD_TYPE=$BUILD_TYPE"
LINUX_BUILD_CMD="cmake --build $BUILD_DIR_LINUX -j$JOBS"
if $LINUX_CMAKE_CMD > /dev/null 2>&1; then
	if $LINUX_BUILD_CMD > /dev/null 2>&1; then
		RESULTS[linux]="OK"
		BINARIES[linux]="$SCRIPT_DIR/$BUILD_DIR_LINUX/examples/simple_window/simple_window"
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
echo -e "${BLUE}[2/3] Building for Windows (MinGW)...${NC}"

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
			BINARIES[windows]="$SCRIPT_DIR/$BUILD_DIR_WIN/examples/simple_window/simple_window.exe"
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
echo -e "${BLUE}[3/3] Building for Android...${NC}"

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
			BINARIES[android]="$SCRIPT_DIR/$BUILD_DIR_ANDROID/examples/simple_window/libsimple_window.so"
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
# Run Tests
# ============================================================================
echo ""
echo -e "${BLUE}Running tests...${NC}"

# Test Linux build
if [[ "${RESULTS[linux]}" == "OK" ]]; then
	echo -e "  Testing Linux binary..."
	TEST_OUTPUT=$(cd /tmp && "${BINARIES[linux]}" -test 2>&1 || true)
	if echo "$TEST_OUTPUT" | grep -q "\[CWD\]" && echo "$TEST_OUTPUT" | grep -q "\[TEST\] Exiting"; then
		RESULTS[linux_test]="OK"
		echo -e "    ${GREEN}Linux test passed${NC}"
	else
		RESULTS[linux_test]="FAILED"
		echo -e "    ${RED}Linux test failed${NC}"
	fi
fi

# Test Windows build with Wine
if [[ "${RESULTS[windows]}" == "OK" ]] && command -v wine &> /dev/null; then
	echo -e "  Testing Windows binary (Wine)..."
	TEST_OUTPUT=$(cd /tmp && wine "${BINARIES[windows]}" -test 2>&1 || true)
	if echo "$TEST_OUTPUT" | grep -q "\[CWD\]" && echo "$TEST_OUTPUT" | grep -q "\[TEST\] Exiting"; then
		RESULTS[windows_test]="OK"
		echo -e "    ${GREEN}Windows/Wine test passed${NC}"
	else
		RESULTS[windows_test]="FAILED"
		echo -e "    ${RED}Windows/Wine test failed${NC}"
	fi
elif [[ "${RESULTS[windows]}" == "OK" ]]; then
	RESULTS[windows_test]="SKIPPED (Wine not found)"
	echo -e "    ${YELLOW}Windows test skipped (Wine not found)${NC}"
fi

# ============================================================================
# Report
# ============================================================================
echo ""
echo -e "${BLUE}================================================${NC}"
echo -e "${BLUE}  Build Report${NC}"
echo -e "${BLUE}================================================${NC}"
echo ""
echo -e "Build Type: ${YELLOW}${BUILD_TYPE}${NC}"
echo ""

# Linux
echo -e "${BLUE}Linux:${NC}"
if [[ "${RESULTS[linux]}" == "OK" ]]; then
	echo -e "  Status: ${GREEN}${RESULTS[linux]}${NC}"
	echo -e "  Binary: ${BINARIES[linux]}"
	echo -e "  Size:   $(format_size ${SIZES[linux]})"
	if [[ -n "${RESULTS[linux_test]}" ]]; then
		if [[ "${RESULTS[linux_test]}" == "OK" ]]; then
			echo -e "  Test:   ${GREEN}${RESULTS[linux_test]}${NC}"
		else
			echo -e "  Test:   ${RED}${RESULTS[linux_test]}${NC}"
		fi
	fi
else
	echo -e "  Status: ${RED}${RESULTS[linux]}${NC}"
fi
echo ""

# Windows
echo -e "${BLUE}Windows:${NC}"
if [[ "${RESULTS[windows]}" == "OK" ]]; then
	echo -e "  Status: ${GREEN}${RESULTS[windows]}${NC}"
	echo -e "  Binary: ${BINARIES[windows]}"
	echo -e "  Size:   $(format_size ${SIZES[windows]})"
	if [[ -n "${RESULTS[windows_test]}" ]]; then
		if [[ "${RESULTS[windows_test]}" == "OK" ]]; then
			echo -e "  Test:   ${GREEN}${RESULTS[windows_test]}${NC}"
		else
			echo -e "  Test:   ${RED}${RESULTS[windows_test]}${NC}"
		fi
	fi
else
	echo -e "  Status: ${YELLOW}${RESULTS[windows]}${NC}"
fi
echo ""

# Android
echo -e "${BLUE}Android:${NC}"
if [[ "${RESULTS[android]}" == "OK" ]]; then
	echo -e "  Status: ${GREEN}${RESULTS[android]}${NC}"
	echo -e "  Binary: ${BINARIES[android]}"
	echo -e "  Size:   $(format_size ${SIZES[android]})"
else
	echo -e "  Status: ${YELLOW}${RESULTS[android]}${NC}"
fi
echo ""

# Summary
echo -e "${BLUE}================================================${NC}"
FAILED=0
for key in "${!RESULTS[@]}"; do
	if [[ "${RESULTS[$key]}" == *"FAILED"* ]]; then
		((FAILED++))
	fi
done

if [[ $FAILED -eq 0 ]]; then
	echo -e "${GREEN}All builds successful!${NC}"
else
	echo -e "${RED}$FAILED build(s) failed${NC}"
	exit 1
fi
