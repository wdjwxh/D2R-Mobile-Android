#!/bin/sh
# Windows + Git Bash, matching the currently verified Android build workflow.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS=${ANDROID_TOOLS_ROOT:-/e/android-tools}
NDK=${ANDROID_NDK_HOME:-$TOOLS/android-ndk-r28b}
NDK=$(cygpath -u "$NDK")
NDKC=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang
BT=${ANDROID_BUILD_TOOLS:-$(find "$TOOLS/build-tools" -mindepth 1 -maxdepth 1 -type d | sort | tail -1)}
JDK=${JAVA_HOME:-$TOOLS/jdk17/jdk-17.0.13+11}
JDK=$(cygpath -u "$JDK")
if [ ! -f "$JDK/bin/java.exe" ] && [ -z "${JAVA_HOME:-}" ]; then
    JDK=$(find "$TOOLS/jdk17" -mindepth 1 -maxdepth 1 -type d -name 'jdk-17*' | sort | tail -1)
fi
PLATFORM=${ANDROID_PLATFORM_JAR:-$(find "$TOOLS/platform34" -name android.jar | sort | tail -1)}
JAVA=$JDK/bin/java.exe
JAVAC=$JDK/bin/javac.exe
KEYTOOL=$JDK/bin/keytool.exe
for executable in "$NDKC" "$JAVA" "$JAVAC" "$BT/aapt2.exe"; do
    [ -f "$executable" ] || { echo "Missing build tool: $executable" >&2; exit 1; }
done
[ -f "$PLATFORM" ] || { echo 'Set ANDROID_PLATFORM_JAR to API 34 android.jar' >&2; exit 1; }
export JAVA_HOME="$(cygpath -w "$JDK")"
export D2R_SHADER_TOOLS=${D2R_SHADER_TOOLS:-$(cygpath -m "$NDK/shader-tools/windows-x86_64")}
export PATH="$JDK/bin:$PATH"
OUT=$ROOT/android-host/apk-out
IOS_APP=$ROOT/private/ios-app
