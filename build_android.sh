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
	-DANDROID_STL=c++_static

# Build
cmake --build $BUILD_DIR -j$(nproc)

# Show results
APK_COUNT=0
echo "========================================="
for apk in $(find $BUILD_DIR/examples -name "*.apk" 2>/dev/null); do
	APK_COUNT=$((APK_COUNT + 1))
	name=$(basename "$apk" .apk)
	echo "APK built: $(ls -lh "$apk" | awk '{print $5}') $apk"
	echo "  adb install -r $apk"
	echo "  cmake --build $BUILD_DIR --target ${name}-run"
	echo ""
done

if [ "$APK_COUNT" -eq 0 ]; then
	echo "Error: No APKs found!"
	exit 1
fi
echo "========================================="