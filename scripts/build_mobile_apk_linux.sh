#!/usr/bin/env bash
# Linux build for the PocketVoice Android app (CPU variant).
# Mirrors scripts/build_mobile_apk.bat --cpu using the host Android SDK.
#
# Requirements:
#   ANDROID_SDK_ROOT (or ANDROID_HOME) pointing to an SDK with
#   build-tools/34.0.0, platforms/android-34 and ndk/ (r27 recommended).
#   third_party/sherpa-onnx-src + third_party/lib/android/arm64-v8a prepared.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/tools/android-sdk}}"
NDK="${ANDROID_NDK_ROOT:-${NDK_PATH:-$SDK/ndk/27.2.12479018}}"
BT="$SDK/build-tools/34.0.0"
PLATFORM="$SDK/platforms/android-34"
PROJECT_DIR="$ROOT_DIR/src/mobile/app"
OUTPUT_DIR="$ROOT_DIR/build/mobile-linux"
NATIVE_BUILD_DIR="$OUTPUT_DIR/native-cpu"
STAGE_DIR="$OUTPUT_DIR/apk-stage-cpu"
FINAL_APK="$OUTPUT_DIR/PocketVoice-Android-cpu.apk"

for tool in "$BT/aapt2" "$BT/d8" "$BT/zipalign" "$BT/apksigner" "$PLATFORM/android.jar" "$NDK/build/cmake/android.toolchain.cmake"; do
    if [ ! -e "$tool" ]; then
        echo "[ERROR] Missing required tool: $tool"
        exit 1
    fi
done

# javac: prefer the bundled JDK 17 under $HOME/tools/jdk17, else fall back to PATH.
JAVAC_BIN="javac"
if [ -x "$HOME/tools/jdk17/usr/lib/jvm/java-17-openjdk-amd64/bin/javac" ]; then
    JAVAC_BIN="$HOME/tools/jdk17/usr/lib/jvm/java-17-openjdk-amd64/bin/javac"
elif ! command -v javac >/dev/null 2>&1; then
    echo "[ERROR] javac not found (install openjdk-17-jdk or set up \$HOME/tools/jdk17)"
    exit 1
fi
echo "  javac: $JAVAC_BIN"

echo "=== [1/3] Build native library (CPU) ==="
cmake -S "$PROJECT_DIR/src/main/cpp" -B "$NATIVE_BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
    -DCMAKE_BUILD_TYPE=Release -DSTT_USE_QNN=OFF
ninja -C "$NATIVE_BUILD_DIR"

echo "=== [2/3] Assemble APK ==="
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/compiled" "$STAGE_DIR/obj" "$STAGE_DIR/lib/arm64-v8a" "$STAGE_DIR/assets/ui"

echo "  Resources..."
"$BT/aapt2" compile --dir "$PROJECT_DIR/src/main/res" -o "$STAGE_DIR/compiled"
"$BT/aapt2" link -I "$PLATFORM/android.jar" \
    --manifest "$PROJECT_DIR/src/main/AndroidManifest.xml" \
    -o "$STAGE_DIR/resources.ap_" --auto-add-overlay \
    $(find "$STAGE_DIR/compiled" -name '*.flat' | tr '\n' ' ')

echo "  Java..."
find "$PROJECT_DIR/src/main/java" -name '*.java' -print0 | xargs -0 \
    "$JAVAC_BIN" --release 17 -d "$STAGE_DIR/obj" -cp "$PLATFORM/android.jar"

echo "  DEX..."
"$BT/d8" --release --output "$STAGE_DIR" $(find "$STAGE_DIR/obj" -name '*.class')

echo "  Native libs..."
cp "$NATIVE_BUILD_DIR/libstt_native.so" "$STAGE_DIR/lib/arm64-v8a/"
cp "$ROOT_DIR/third_party/lib/android/arm64-v8a/libsherpa-onnx-c-api.so" "$STAGE_DIR/lib/arm64-v8a/"
cp "$ROOT_DIR/third_party/lib/android/arm64-v8a/libonnxruntime.so" "$STAGE_DIR/lib/arm64-v8a/"

cp "$STAGE_DIR/resources.ap_" "$STAGE_DIR/app.apk"
( cd "$STAGE_DIR" && "$BT/aapt" add app.apk classes.dex >/dev/null )
( cd "$STAGE_DIR" && "$BT/aapt" add app.apk \
    lib/arm64-v8a/libsherpa-onnx-c-api.so \
    lib/arm64-v8a/libonnxruntime.so \
    lib/arm64-v8a/libstt_native.so >/dev/null )
if [ -f "$PROJECT_DIR/src/main/assets/ui/index.html" ]; then
    cp "$PROJECT_DIR/src/main/assets/ui/index.html" "$PROJECT_DIR/src/main/assets/ui/styles.css" "$PROJECT_DIR/src/main/assets/ui/app.js" "$STAGE_DIR/assets/ui/"
    ( cd "$STAGE_DIR" && "$BT/aapt" add app.apk \
        assets/ui/index.html assets/ui/styles.css assets/ui/app.js >/dev/null )
fi

echo "=== [3/3] Align + sign ==="
"$BT/zipalign" -v 4 "$STAGE_DIR/app.apk" "$STAGE_DIR/aligned.apk" >/dev/null
KEYSTORE="$HOME/.android/debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    mkdir -p "$HOME/.android"
    keytool -genkeypair -v -keystore "$KEYSTORE" -storepass android -keypass android \
        -alias androiddebugkey -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA \
        -keysize 2048 -validity 10000 >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --out "$FINAL_APK" "$STAGE_DIR/aligned.apk"

echo
echo "========================================"
echo "  Build complete!"
echo "  Output: $FINAL_APK"
echo "========================================"