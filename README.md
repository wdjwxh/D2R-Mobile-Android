# D2R-Mobile-Android

基于 [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) 的《暗黑破坏神 II：狱火重生》Android ARM64 兼容层移植，图形后端使用 **Vulkan，最低 API 基线为 Vulkan 1.1**，优先适配高通 Adreno。

**开发预览版。仓库不包含游戏程序、游戏资源、翻译后的游戏着色器或可直接游玩的安装包。你需要拥有正版游戏，并在本机生成这些内容。**

[English](README.en.md) · [构建](docs/BUILD.md) · [游戏文件与安装](docs/GAME_FILES.md) · [移植架构](docs/ARCHITECTURE.md) · [验证状态](docs/STATUS.md) · [内容边界](docs/DERIVED.md) · [致谢与许可](docs/ACKNOWLEDGEMENTS.md)

## 当前能做到什么

- Android 横屏外壳，ARM64 程序装载与系统服务适配。
- NVN 命令录制和回放到 Vulkan；图像、计算、绘制、呈现和真实 GPU 完成同步。
- 共享 CPU/GPU guest backing、资源缓存、批量提交及图像内存分块。
- 复用上游 iOS 触控桥、HUD 和部分玩法模块，接入 Android 输入及键盘。
- 模拟器已验证主菜单、营地、触控移动和相机滚动。最近同一 6 核模拟器实例约 **35 FPS 营地、26 FPS 较繁忙区域**，详见[测量边界](docs/STATUS.md)。

这不是上游 iOS 版本全部功能、60 FPS 或全流程通关的 Android 保证。当前没有完成高通真机性能、所有触控操作、换幕及长时间稳定性验收，也没有证明全进程物理占用始终低于 6 GiB。

## 获取与构建

```bash
git clone --recurse-submodules https://github.com/wdjwxh/D2R-Mobile-Android.git
cd D2R-Mobile-Android
```

已经克隆但没有子模块时：

```bash
git submodule update --init --recursive
```

先按[游戏文件说明](docs/GAME_FILES.md)准备自己的文件，再按[构建说明](docs/BUILD.md)生成 APK。当前整理的是经过开发验证的 **Windows + Git Bash + NDK r28b** 路线；不宣称 Linux/macOS 构建已验证。

构建出的 APK 包含你在本机生成的游戏衍生内容，应留在本机，不应上传为公开 Release。更新应用使用覆盖安装，避免卸载造成资源或存档丢失。

## 目录

```text
android-host/src/      Android 宿主、Vulkan 后端、接口与输入适配
android-host/apk/      Android Manifest 与 Java 外壳
android-host/*.py      上游适配生成器、着色器工具与出包工具
scripts/              私有输入准备与仓库边界检查
docs/                 构建、资源、架构、状态、许可说明
d2r-mobile/           固定提交的上游 Git 子模块
```

`private/`、`android-work/`、`android-host/generated/`、`android-host/apk-out/` 是本机输入、缓存和生成目录，均不进入版本库。调试转储、截图、旧 APK、签名文件、个人存档与原工作区历史交接文件也不随仓库发布。

## 感谢

本项目建立在 **d2r-mobile 的大量工作之上**，包括程序改写、宿主服务、NVN 接口、着色器翻译链、资源处理、iOS 触控设计及玩法模块。Android 移植尽量复用这些实现，把平台外壳和 Metal 图形执行适配到 Android/Vulkan；这些上游成果不应被描述为 Android 项目的独立原创。

特别感谢原作者 **jhaizhou** 与所有上游贡献者。完整来源、第三方许可和离线改写归属见[致谢](docs/ACKNOWLEDGEMENTS.md)。本项目与 Blizzard、Nintendo 没有关联或授权关系；游戏内容及商标归各自权利人。

## 许可与参与

移植源码沿用上游 MIT 许可，保留原版权声明；第三方文件遵守各自文件头或许可文件，见 [LICENSE](LICENSE)。MIT 不授予游戏内容的再分发权。

提交 Issue/PR 前阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。不要附带游戏文件、密钥、包含游戏内容的 APK 或未清理的私人日志。
