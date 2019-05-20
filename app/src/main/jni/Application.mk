# Primary is CMakeLists.txt, but this should still work

APP_PLATFORM=android-21
APP_ABI=armeabi armeabi-v7a arm64-v8a x86 x86_64
APP_PIE=true
APP_STL=gnustl_static
APP_CXXFLAGS=-Wall -Werror -fpermissive -fpic -Os -fdata-sections -ffunction-sections -s -fvisibility=hidden -Wno-error=strict-aliasing
NDK_TOOLCHAIN_VERSION=4.9
