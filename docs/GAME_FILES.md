# 游戏文件、来源与手机安装

## 内容边界

你必须拥有正版游戏。本仓库不提供游戏程序、美术、音频、文本表、数据表、游戏着色器、存档、主机密钥或这些文件的下载地址。不要在 Issue、PR 或 Release 中上传它们。详见 [DERIVED.md](DERIVED.md)。

当前版本相关接口与改写依赖 **Switch 1.0.34.0，对应游戏内部/PC 版本号 3.3.94012，包含 Reign of the Warlock（术士君临）资料片**；其他版本未验证。Switch 与 PC 资源不可互换。

## 自己获取原始文件

参照上游的 [GAME_FILES.zh-CN.md](../d2r-mobile/docs/GAME_FILES.zh-CN.md)：从自己持有的主机与已购买游戏导出更新版 ExeFS、RomFS。上游说明链接到 [nxdumptool](https://github.com/DarkMatterCore/nxdumptool)，并提供 `dm/tools/extract_game.sh` 处理自己的整包导出；需要密钥时只能使用自己的密钥，留在本机。

原始文件至少包含：

```text
Update/
  ExeFS/main
  ExeFS/sdk
  ExeFS/subsdk0
  RomFS/data/build_manifest.psv
  RomFS/data/*.data
```

不要从第三方索取游戏包或把上游不含游戏的源码/壳误当完整游戏。

## 当前 Android 构建输入路线

目前 Android 工具不是一个独立的 Switch 原包到 APK 的一键打包器。已实现的路线是：

1. 使用固定版本上游及自己的游戏文件，在本机生成上游 iOS App/IPA 的内容。
2. 如使用 Windows，阅读上游 [NO_XCODE.zh-CN.md](../d2r-mobile/docs/NO_XCODE.zh-CN.md)，用对应版本的不含游戏内容的 shell build input 与 `dm/tools/d2rm_pack.py`，生成自己的 IPA。壳和源码必须版本匹配；上游的最新壳不保证适配本仓库固定提交。若旧壳不可获得，请按上游说明构建对应版本，而不是略过校验。
3. 在本机解压自己生成的 IPA，找到 `Payload/DmIOS.app`。Android 无需安装或签名这个 IPA。
4. 按 [BUILD.md](BUILD.md)运行 `scripts/prepare_inputs.py`。它只导入必要装载表、派生表、触控素材及覆盖层，并从 Guest.framework 重建模块，再改写 Android TLS 访问。

这些步骤会在本机产生游戏衍生文件，不能上传仓库。当前输入路线仍依赖上游打包链，不宣称已完成无需 iOS 中间产物的独立 Android 打包流程。

## 资源放到手机哪里

安装自己构建的 APK 并启动一次，使应用建立目录。把基础 `data` 文件夹复制到：

```text
/sdcard/Android/data/local.d2rm/files/romfs/data/
```

用同一局域网电脑的 PowerShell 执行；替换两个变量，避免使用已过期的固定端口：

```powershell
$device = "手机IP:当前无线调试端口"
$gameData = "D:\MyGame\Update\RomFS\data"
adb connect $device
adb -s $device push $gameData /sdcard/Android/data/local.d2rm/files/romfs/
adb -s $device shell ls -la /sdcard/Android/data/local.d2rm/files/romfs/data
```

USB ADB 可省去 connect，仍建议用 `-s` 明确设备。无线首次配对使用 `adb pair 手机IP:配对端口`，按提示输入配对码；配对端口与调试端口不同，不要把配对码写进脚本。

基础资源是上游列出的 18 个档案（约 5.6 GB）：`00-00000001.data` 至 `00-00000006.data`，以及前缀 `01`、`02`、`03`、`04`、`05`、`17` 各自的 `00000001` 和 `00000002` 档案。不要多套一层资源包名称。生成的覆盖层 `.build.config` 与附加档案由自己的 APK 提供，应用从私有覆盖层和基础目录组合读取；不要用别人的覆盖档案替换不匹配版本。

`adb push` 合并目录，无需先清空。与手机同一 Wi-Fi 的电脑可直接传输，不必经远程调试机中转。

## 存档、更新与离线运行

- 当前应用 ID 是 `local.d2rm`。更新使用 `adb -s $device install -r <本机APK路径>`，不要先卸载；卸载可能删除存档和外部资源目录。
- 存档位于应用私有文件目录的 `save/`，不放入 `romfs/data`。覆盖安装前建议用 Android 调试工具自行备份；不要把备份纳入仓库。
- 游戏按上游离线路线运行，不需要在线游戏账号，不提供联机功能。离线改写的来源归属见 [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md)，这不表示 Android 已验证所有离线流程。
