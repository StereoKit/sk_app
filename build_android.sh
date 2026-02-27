#!/bin/bash
# Build script for Android APK

set -e  # Exit on error

# Parse command line arguments
if [ -n "$1" ] && [ "$1" != "x86" ]; then
	echo "Error: Invalid ABI parameter. Use 'x86' or leave empty for arm64."
	exit 1
fi

if [ "$1" = "x86" ]; then
	TARGET_ABI="x86_64"
	BUILD_DIR="build-androidx86"
else
	TARGET_ABI="${ANDROID_ABI:-arm64-v8a}"
	BUILD_DIR="build-android"
fi

# Check for Android SDK
if [ -z "$ANDROID_HOME" ] && [ -z "$ANDROID_SDK_ROOT" ]; then
	echo "Error: ANDROID_HOME or ANDROID_SDK_ROOT must be set"
	exit 1
fi

# Set Android SDK root
ANDROID_SDK=${ANDROID_HOME:-$ANDROID_SDK_ROOT}

# Check for Android NDK
if [ -z "$ANDROID_NDK" ]; then
	# Try to find NDK in SDK
	if [ -d "$ANDROID_SDK/ndk" ]; then
		# Get the latest NDK version
		ANDROID_NDK=$(ls -d $ANDROID_SDK/ndk/* | sort -V | tail -1)
		echo "Found NDK: $ANDROID_NDK"
	else
		echo "Error: ANDROID_NDK not set and not found in SDK"
		exit 1
	fi
fi

# Build configuration
ABI="$TARGET_ABI"
API_LEVEL="${ANDROID_API_LEVEL:-32}"

echo "========================================="
echo "Building sk_app for Android"
echo "========================================="
echo "NDK:       $ANDROID_NDK"
echo "SDK:       $ANDROID_SDK"
echo "ABI:       $ABI"
echo "API Level: $API_LEVEL"
echo "========================================="

# Configure
cmake -B $BUILD_DIR \
	-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
	-DANDROID_ABI=$ABI \
	-DANDROID_PLATFORM=android-$API_LEVEL \
	-DCMAKE_BUILD_TYPE=Release \
	-DANDROID_STL=c++_shared

# Build
cmake --build $BUILD_DIR -j$(nproc)

# Build APK
echo "========================================="
echo "Building APK..."
echo "========================================="
cmake --build $BUILD_DIR --target simple_window-apk

# Show result
if [ -f "$BUILD_DIR/examples/simple_window/simple_window.apk" ]; then
	echo "========================================="
	echo "SUCCESS! APK built:"
	echo "$BUILD_DIR/examples/simple_window/simple_window.apk"
	ls -lh "$BUILD_DIR/examples/simple_window/simple_window.apk"
	echo "========================================="
	echo ""
	echo "To install and run:"
	echo "  adb install -r $BUILD_DIR/examples/simple_window/simple_window.apk"
	echo "  adb shell am start -n net.stereokit.simple_window/net.stereokit.sk_app.SkAppActivity"
	echo ""
	echo "Or use the convenience target:"
	echo "  cmake --build $BUILD_DIR --target simple_window-run"
else
	echo "Error: APK not found!"
	exit 1
fi