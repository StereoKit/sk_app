# sk_app

A small cross-platform library for windows / vulkan surfaces / input / OS functionality. Targets Win32, Linux, Android, and MacOS.

## Building

From the project root:

### Linux

```sh
# To configure
cmake -B build

# To build
cmake --build build -j8

# To run
./build/examples/test_window/test_window
```

### Windows (Cross-compile with MinGW)

```bash
# To configure
cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release

# To build
cmake --build build-mingw -j8

# To run with Wine
wine ./build-mingw/examples/test_window/test_window.exe
```

### Android

Prerequisites:

- ANDROID_HOME or ANDROID_SDK_ROOT environment variable set
- ANDROID_NDK environment variable (optional - auto-detected from SDK)
- Java JDK installed for signing APKs

```sh
# Quick build for arm64 (default)
./build_android.sh
# Install and run on connected device or simulator
cmake --build build-android --target test_window-run
```

```sh
# OR x86_64
./build_android.sh x86
# Install and run on connected device or simulator
cmake --build build-androidx86 --target test_window-run
```

#### Android manual build

```sh
# arm64 (default)
cmake -B build-android \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-32 \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build-android -j8
# Build the APK
cmake --build build-android --target test_window-apk
# Install and run on connected device or simulator
adb install -r build-android/examples/test_window/test_window.apk
adb shell am start -n net.stereokit.test_window/net.stereokit.sk_app.SkAppActivity
```

```sh
# OR x86_64
cmake -B build-androidx86 \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=x86_64 \
    -DANDROID_PLATFORM=android-32 \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build-androidx86 -j8
# Build the APK
cmake --build build-androidx86 --target test_window-apk
# Install and run on connected device or simulator
adb install -r build-androidx86/examples/test_window/test_window.apk
adb shell am start -n net.stereokit.test_window/net.stereokit.sk_app.SkAppActivity
```

```sh
# Install and run on connected device or simulator
adb install -r build-android/examples/test_window/test_window.apk
adb shell am start -n net.stereokit.test_window/net.stereokit.sk_app.SkAppActivity
```

#### Filtering Android logcat

```sh
# Filtered logcat of the app
adb logcat -v color --uid `adb shell pm list package -U net.stereokit.test_window | cut -d ":" -f3`
```

### Available commands

   ESC       - Exit application
   T         - Show virtual keyboard
   M         - Maximize window
   N         - Minimize window
   R         - Restore window
   H         - Hide window (2 seconds)
   P         - Set window position
   S         - Set window size
   SPACE     - Rename window title
   C         - Toggle cursor visibility
   V         - Toggle relative mouse mode
   W         - Warp mouse to center
   Mouse     - Move and click
   Wheel     - Scroll
