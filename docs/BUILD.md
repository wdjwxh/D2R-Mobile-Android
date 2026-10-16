# 构建 Android 开发包

当前整理并验证的宿主环境是 Windows + Git Bash；使用 Android NDK r28b、API 34 Build Tools/Platform、JDK 17、Python 3.10+ 和 .NET 10 SDK。其他宿主平台未验收。

## 1. 源码和工具

```bash
git submodule update --init --recursive
```

上游固定到 `17845634d71341cca1debf43ad79ac25c6a3a97a`，不要直接切到上游最新分支。触控生成器会核对派生表哈希，翻译器适配也有明确源代码锚点。

从官方安装 [Android NDK/SDK](https://developer.android.com/ndk/downloads)、[JDK 17](https://learn.microsoft.com/java/openjdk/download)、[Python](https://www.python.org/downloads/)、[.NET SDK](https://dotnet.microsoft.com/download)。需要的上游资源生成工具依赖遵循其 [DEPENDENCIES](../d2r-mobile/docs/DEPENDENCIES.md) 与 [NO_XCODE](../d2r-mobile/docs/NO_XCODE.zh-CN.md)。不要提交 SDK 或下载缓存。

Git Bash 示例环境（路径按本机修改；含空格的路径保留引号）：

```bash
export ANDROID_NDK_HOME='E:/android-tools/android-ndk-r28b'
export ANDROID_BUILD_TOOLS='E:/android-tools/build-tools/android-14'
export ANDROID_PLATFORM_JAR='E:/android-tools/platform34/android-34/android.jar'
export JAVA_HOME='E:/android-tools/jdk17/jdk-17.0.20.1+1'
export D2R_SHADER_TOOLS='E:/android-tools/android-ndk-r28b/shader-tools/windows-x86_64'
export DOTNET='E:/android-tools/dotnet10/dotnet.exe'
export PYTHONUTF8=1
```

`D2R_SHADER_TOOLS` 目录需要 `glslc.exe`、`spirv-val.exe`。构建脚本支持这些环境变量；未配置时保留原开发工具布局的默认值。`.NET` 构建需联网恢复其开源依赖，游戏文件本身不上传。

## 2. 准备私有输入

先阅读 [GAME_FILES.md](GAME_FILES.md)，得到自己构建的同版本 `DmIOS.app`：

```bash
python scripts/prepare_inputs.py --ios-app 'D:/MyBuild/Payload/DmIOS.app'
```

输出：`private/ios-app/`、`android-work/dm-tables/`、`android-work/modules/`。这些目录全部被 Git 忽略。已有同版本输入时不要混入另一个版本；重新导入不同版本时使用新的本地工作副本，脚本不承担清除旧素材的工作。

## 3. 提取和翻译着色器

以下路径指向自己的 RomFS 基础 data 目录，覆盖层由步骤 2 导入：

```bash
python tvfs_get.py list private/ios-app/romfs_overlay 'D:/MyGame/Update/RomFS/data' '*'
```

从 TVFS 提取上游 shader_map.tsv 指向的着色器包，可使用本项目的精确文件名提取脚本，避免依赖未知的通配路径：

```bash
python scripts/prepare_shaders.py --data 'D:/MyGame/Update/RomFS/data'
python android-host/build_android_xlate.py --dotnet "$DOTNET"
python spv_build.py android-work/shaders/packs android-work/modules/main.android.bin d2r-mobile/dm/gfx/nvn android-work/tools/dmsh_xlate_android/dmsh_xlate.exe android-work/shaders/spv-adreno
python android-host/validate_spirv.py android-work/shaders/spv-adreno --validator "$D2R_SHADER_TOOLS/spirv-val.exe"
```

不要换成上游为 Metal 准备的 SPIR-V，也不要只重新生成一部分后混用旧 guarded-uniform ABI 的结果。`build_android_xlate.py` 在忽略目录复制并适配翻译器，不修改固定的上游子模块。

## 4. 完整构建

```bash
sh android-host/build_apk.sh
```

先检查私有输入路径，再生成 compute/timestamp catalog、上游触控/HUD/玩法适配、Android native 库、guest ELF 包装、图形着色器档案、Java/dex，最后对齐并用本机调试证书签名。

输出为 `android-host/apk-out/d2rm-research.apk`。**这个文件含本机生成的游戏衍生内容，不是可公开上传的源码仓库产物。** 本机 `debug.keystore` 会自动生成，不要共享，也不要每次更新随意替换；签名不同将无法直接覆盖旧应用。

为覆盖已经安装过的测试包，设置 `D2R_SIGNING_KEYSTORE` 为原调试 keystore 的本机路径；该文件不要复制进公开仓库。当前脚本使用 Android 标准调试签名参数，变量不用于处理商店发行密钥。包名维持 `local.d2rm`，预览包 versionCode 为 101，资源目录仍为 `Android/data/local.d2rm/files/romfs/data/`。

只修改 native/Java、已经有正确的完整出包目录时可以：

```bash
sh android-host/rebuild_native.sh
```

shader/manifest/assets 或生成 ABI 变化后必须完整构建。完整构建会重建专用 `apk-out/`，不清空原始私有输入和手机资源。

## 5. 安装

```bash
adb -s '<设备序列号或IP:端口>' install -r android-host/apk-out/d2rm-research.apk
adb -s '<设备序列号或IP:端口>' shell am start -n local.d2rm/.MainActivity --ez boot true
adb -s '<设备序列号或IP:端口>' logcat -s dmhost
```

随后按[资源说明](GAME_FILES.md)复制基础档案。开发包启用了 Android debuggable；当前仓库没有独立的公开发行签名/商店发布流水线。

## 故障定位

- 子模块文件缺失：执行 `git submodule update --init --recursive`。
- `descriptor_index_sha256` 或源锚点不匹配：上游、派生表、程序或素材版本不一致，不能删断言继续。
- 无 shader/reflection：先完成步骤 3，不要拿空 catalog 出包。
- `INSTALL_FAILED_UPDATE_INCOMPATIBLE`：通常是签名不同。先备份数据并找回原签名，不要直接卸载。
- 取日志可以用 `adb -s ... shell run-as local.d2rm cat files/dmhost.log`；发布前清理设备地址、用户名、文件路径等私人信息。
