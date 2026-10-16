#!/bin/sh
# Load local Windows/Git Bash build tools.
. "$(dirname "$0")/toolchain.sh"

python "$ROOT/scripts/check_inputs.py"
rm -rf "$OUT" && mkdir -p "$OUT/jniLibs/arm64-v8a" "$OUT/assets" "$OUT/obj"

echo '== 1. 编 native 库（含 JNI 桥，不含 CLI main）=='
python "$ROOT/android-host/build_compute.py" "$OUT"
python "$ROOT/android-host/build_timestamp.py" "$OUT"
python "$ROOT/android-host/prepare_touch.py"
python "$ROOT/android-host/prepare_host.py"
python "$ROOT/android-host/prepare_rift.py"
"$NDKC" -O2 -Wall -Wextra -Wno-unused-parameter -mbranch-protection=none -shared -fPIC -Wl,--no-undefined \
    -I"$OUT" \
    -o "$OUT/jniLibs/arm64-v8a/libdmhost.so" \
    "$ROOT/android-host/src/loader.c" "$ROOT/android-host/src/gates.c" "$ROOT/android-host/src/i18n_android.c" "$ROOT/android-host/src/touch_game_android.c" "$ROOT/android-host/src/hud_android.c" \
    "$ROOT/android-host/generated/rift/dm_rift.c" "$ROOT/android-host/generated/rift/dm_bossrush.c" "$ROOT/android-host/generated/rift/rift_time.c" \
    "$ROOT/android-host/src/tsd.c" "$ROOT/android-host/src/hle_svc.c" "$ROOT/android-host/src/hle_thread.c" "$ROOT/android-host/src/hle_fs.c" "$ROOT/android-host/src/bridge.c" \
    "$ROOT/android-host/src/entry.S" "$ROOT/android-host/src/hle_tact.c" "$ROOT/android-host/src/hle_hid.c" "$ROOT/android-host/generated/dm_townwp.c" "$ROOT/android-host/generated/hle_private_aim.c" "$ROOT/android-host/src/hle_vi.c" "$ROOT/android-host/src/hle_account.c" "$ROOT/android-host/src/hle_net.c" "$ROOT/android-host/src/hle_oe.c" "$ROOT/android-host/src/display.c" "$ROOT/android-host/src/vk_backend.c" "$ROOT/android-host/src/hle_nv.c" "$ROOT/android-host/src/hle_nvn.c" "$ROOT/android-host/src/nvn/nvn_tex.c" "$ROOT/android-host/src/nvn/nvn_dirty.c" "$ROOT/android-host/src/tex/nvn_deswizzle.c" "$ROOT/android-host/src/hle_friends.c" "$ROOT/android-host/src/hle_misc.c" "$ROOT/android-host/src/hle_audio.c" "$ROOT/android-host/src/hle_codec.c" "$ROOT/android-host/src/hle_movie.c" "$ROOT/android-host/src/hle_swkbd.c" "$ROOT/android-host/src/hle_charname.c" "$ROOT/android-host/src/guest_clock.c" \
    "$ROOT/d2r-mobile/dm/third_party/libdeflate/lib/deflate_decompress.c" "$ROOT/d2r-mobile/dm/third_party/libdeflate/lib/zlib_decompress.c" "$ROOT/d2r-mobile/dm/third_party/libdeflate/lib/adler32.c" "$ROOT/d2r-mobile/dm/third_party/libdeflate/lib/utils.c" "$ROOT/d2r-mobile/dm/third_party/libdeflate/lib/arm/cpu_features.c" \
    -lm -llog -lz -landroid -lvulkan -laaudio

echo '== 1.5 包装 guest .so（AOT 翻译路径）=='
python "$ROOT/mk_guest_so.py" "$ROOT/android-work/modules/main.android.bin" "$ROOT/android-work/dm-tables/main.fixups.tsv" "$OUT/jniLibs/arm64-v8a/libguest_main.so"
python "$ROOT/mk_guest_so.py" "$ROOT/android-work/modules/sdk.android.bin" "$ROOT/android-work/dm-tables/sdk.fixups.tsv" "$OUT/jniLibs/arm64-v8a/libguest_sdk.so"

echo '== 2. 准备资产（模块映像 + 装载表）=='
python "$ROOT/android-host/build_graphics.py" "$OUT/assets/graphics.vkpack"
cp "$ROOT/d2r-mobile/dm/gfx/nvn/pso_manifest.tsv" "$OUT/assets/pso_manifest.tsv"
cp "$ROOT/android-work/modules/main.android.bin" "$OUT/assets/"
cp "$ROOT/android-work/modules/sdk.android.bin" "$OUT/assets/"
for t in main.fixups.tsv sdk.fixups.tsv main.gates.tsv sdk.gates.tsv; do
    cp "$ROOT/android-work/dm-tables/$t" "$OUT/assets/"
done

# Reuse the installed iOS art pack; these are local game-derived assets.
mkdir -p "$OUT/assets/touch"
cp "$IOS_APP"/dm_native_*.png "$OUT/assets/touch/"
cp "$IOS_APP/dm_touch_joy.png" "$OUT/assets/touch/"
cp -R "$IOS_APP/hud_v4_ui" "$OUT/assets/touch/"
cp -R "$IOS_APP/romfs_overlay" "$OUT/assets/romfs_overlay"

echo '== 3. aapt2 编清单 =='
"$BT/aapt2.exe" link -o "$OUT/base.apk" \
    -I "$PLATFORM" \
    --manifest "$ROOT/android-host/apk/AndroidManifest.xml" \
    -A "$OUT/assets"

echo '== 4. javac + d8 出 dex =='
"$JAVAC" -source 8 -target 8 -classpath "$PLATFORM" \
    -d "$OUT/obj" "$ROOT/android-host/apk/java/local/d2rm/MainActivity.java" 2>/dev/null || \
"$JAVAC" -encoding UTF-8 -classpath "$PLATFORM" -d "$OUT/obj" "$ROOT/android-host/apk/java/local/d2rm/MainActivity.java"
cmd //c "$(cygpath -w "$BT/d8.bat")" --release --lib "$PLATFORM" --output "$OUT" "$OUT"/obj/local/d2rm/*.class 2>/dev/null || \
"$JAVA" -cp "$BT/lib/d8.jar" com.android.tools.r8.D8 --release --lib "$PLATFORM" --output "$OUT" "$OUT"/obj/local/d2rm/*.class

echo '== 5. 组包（dex + so）=='
cd "$OUT"
mkdir -p lib/arm64-v8a
cp jniLibs/arm64-v8a/*.so lib/arm64-v8a/
"$JDK/bin/jar.exe" -uf base.apk classes.dex lib/arm64-v8a/libdmhost.so lib/arm64-v8a/libguest_main.so lib/arm64-v8a/libguest_sdk.so
# aapt2's default asset filter skips dot files; the CASC overlay needs this root.
"$JDK/bin/jar.exe" -uf base.apk assets/romfs_overlay/data/.build.config

echo '== 6. 对齐 + 签名 =='
python "$ROOT/android-host/normalize_apk_paths.py" "$OUT/base.apk"
"$BT/zipalign.exe" -f 4 base.apk aligned.apk
KS=${D2R_SIGNING_KEYSTORE:-$ROOT/android-host/debug.keystore}
[ -f "$KS" ] || "$KEYTOOL" -genkeypair -keystore "$KS" -storepass android -keypass android \
    -alias androiddebugkey -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -keysize 2048 -validity 10000
cmd //c "$(cygpath -w "$BT/apksigner.bat")" sign --ks "$KS" --ks-pass pass:android --out d2rm-research.apk aligned.apk
ls -la d2rm-research.apk
cmd //c "$(cygpath -w "$BT/apksigner.bat")" verify --print-certs d2rm-research.apk | head -3
echo "出包完成: $OUT/d2rm-research.apk"
