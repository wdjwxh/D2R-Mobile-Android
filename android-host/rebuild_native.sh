#!/bin/sh
# Load local Windows/Git Bash build tools.
. "$(dirname "$0")/toolchain.sh"

[ -f "$OUT/base.apk" ] && [ -f "$OUT/compute_catalog.inc" ] || { echo "Run build_apk.sh first" >&2; exit 1; }

JAVA_SOURCE=$ROOT/android-host/apk/java/local/d2rm/MainActivity.java
if [ "$JAVA_SOURCE" -nt "$OUT/classes.dex" ]; then
    echo '== Refresh Java/dex =='
    mkdir -p "$OUT/obj"
    "$JAVAC" -encoding UTF-8 -source 8 -target 8 -classpath "$PLATFORM" -d "$OUT/obj" "$JAVA_SOURCE"
    "$JAVA" -cp "$BT/lib/d8.jar" com.android.tools.r8.D8 --release --lib "$PLATFORM" --output "$OUT" "$OUT"/obj/local/d2rm/*.class
    (cd "$OUT" && "$JDK/bin/jar.exe" -uf base.apk classes.dex)
fi

echo '== 1. 编 native 库（含 JNI 桥，不含 CLI main）=='

python "$ROOT/android-host/prepare_touch.py"
python "$ROOT/android-host/prepare_host.py"
python "$ROOT/android-host/prepare_rift.py"
python "$ROOT/android-host/build_timestamp.py" "$OUT"
"$NDKC" -O2 -Wall -Wextra -Wno-unused-parameter -mbranch-protection=none -shared -fPIC -Wl,--no-undefined \
    -I"$OUT" \
    -o "$OUT/jniLibs/arm64-v8a/libdmhost.so" \
    "$ROOT/android-host/src/loader.c" "$ROOT/android-host/src/gates.c" "$ROOT/android-host/src/i18n_android.c" "$ROOT/android-host/src/touch_game_android.c" "$ROOT/android-host/src/hud_android.c" \
    "$ROOT/android-host/generated/rift/dm_rift.c" "$ROOT/android-host/generated/rift/dm_bossrush.c" "$ROOT/android-host/generated/rift/rift_time.c" \
    "$ROOT/android-host/src/tsd.c" "$ROOT/android-host/src/hle_svc.c" "$ROOT/android-host/src/hle_thread.c" "$ROOT/android-host/src/hle_fs.c" "$ROOT/android-host/src/bridge.c" \
    "$ROOT/android-host/src/entry.S" "$ROOT/android-host/src/hle_tact.c" "$ROOT/android-host/src/hle_hid.c" "$ROOT/android-host/generated/dm_townwp.c" "$ROOT/android-host/generated/hle_private_aim.c" "$ROOT/android-host/src/hle_vi.c" "$ROOT/android-host/src/hle_account.c" "$ROOT/android-host/src/hle_net.c" "$ROOT/android-host/src/hle_oe.c" "$ROOT/android-host/src/display.c" "$ROOT/android-host/src/vk_backend.c" "$ROOT/android-host/src/hle_nv.c" "$ROOT/android-host/src/hle_nvn.c" "$ROOT/android-host/src/nvn/nvn_tex.c" "$ROOT/android-host/src/nvn/nvn_dirty.c" "$ROOT/android-host/src/tex/nvn_deswizzle.c" "$ROOT/android-host/src/hle_friends.c" "$ROOT/android-host/src/hle_misc.c" "$ROOT/android-host/src/hle_audio.c" "$ROOT/android-host/src/hle_codec.c" "$ROOT/android-host/src/hle_movie.c" "$ROOT/android-host/src/hle_swkbd.c" "$ROOT/android-host/src/hle_charname.c" "$ROOT/android-host/src/guest_clock.c" \
    "$ROOT/d2r-mobile/dm/third_party/libdeflate/lib/deflate_decompress.c" "$ROOT/d2r-mobile/dm/third_party/libdeflate/lib/zlib_decompress.c" "$ROOT/d2r-mobile/dm/third_party/libdeflate/lib/adler32.c" "$ROOT/d2r-mobile/dm/third_party/libdeflate/lib/utils.c" "$ROOT/d2r-mobile/dm/third_party/libdeflate/lib/arm/cpu_features.c" \
    -lm -llog -lz -landroid -lvulkan -laaudio

echo '== 5. 组包（dex + so）=='
cd "$OUT"
mkdir -p lib/arm64-v8a
cp jniLibs/arm64-v8a/libdmhost.so lib/arm64-v8a/
"$JDK/bin/jar.exe" -uf base.apk lib/arm64-v8a/libdmhost.so

echo '== 6. 对齐 + 签名 =='
python "$ROOT/android-host/normalize_apk_paths.py" "$OUT/base.apk"
"$BT/zipalign.exe" -f 4 base.apk aligned.apk
KS=$ROOT/android-host/debug.keystore
[ -f "$KS" ] || "$KEYTOOL" -genkeypair -keystore "$KS" -storepass android -keypass android \
    -alias androiddebugkey -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -keysize 2048 -validity 10000
cmd //c "$(cygpath -w "$BT/apksigner.bat")" sign --ks "$KS" --ks-pass pass:android --out d2rm-research.apk aligned.apk
ls -la d2rm-research.apk
cmd //c "$(cygpath -w "$BT/apksigner.bat")" verify --print-certs d2rm-research.apk | head -3
echo "出包完成: $OUT/d2rm-research.apk"
