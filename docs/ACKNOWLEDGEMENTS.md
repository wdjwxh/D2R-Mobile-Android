# 致谢与许可来源

## d2r-mobile

特别感谢 [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)、原作者 **jhaizhou** 和所有贡献者。本项目复用其程序改写和装载设计、系统服务、NVN 接口语义、着色器翻译链及接口事实表、资源生成工具、iOS 触控/HUD 设计和玩法模块。

固定参考提交：`17845634d71341cca1debf43ad79ac25c6a3a97a`。子模块提供可追溯版本。Android 下的 `prepare_touch.py`、`prepare_host.py`、`prepare_rift.py` 从该版本生成适配副本；生成目录不提交。

根目录 [LICENSE](../LICENSE) 保留 `Copyright (c) 2026 jhaizhou` 的 MIT 原文。复制或改编的文件保留其文件头。新贡献可以在相应文件中注明自己的版权，不删除上游版权。MIT 仅涵盖有权许可的源码，不涵盖 Blizzard/Nintendo 游戏内容。

## 第三方

| 来源 | 用途与许可入口 |
|---|---|
| Ryujinx shader translator | 经上游复制、局部适配生成 Android SPIR-V；参照 [上游依赖](../d2r-mobile/docs/DEPENDENCIES.md) 与对应 [源码目录](../d2r-mobile/dm/third_party/ryujinx) 中的许可 |
| libdeflate，Eric Biggers、Google 等贡献者 | 解压，MIT；[COPYING](../d2r-mobile/dm/third_party/libdeflate/COPYING) |
| NVN 接口声明 | `android-host/src/nvn/nvn.h`，保留文件头 MIT 作者与许可；原移入提交另见同目录 README |
| Khronos Vulkan / SPIR-V 工具 | 构建使用 SDK/工具，不随本仓库打包；各工具遵循自己的许可 |
| nxdumptool / hactool 等上游资源准备工具 | 由用户从各官方项目获取，遵循各自许可，本仓库不分发这些二进制 |

第三方许可不会因项目根目录使用 MIT 而改变。发布第三方二进制前须独立核对其许可要求。

## 离线改写归属

上游 [DERIVED.zh-CN.md](../d2r-mobile/docs/DERIVED.zh-CN.md) 说明其 `patch_nso.py` 中 7 处离线运行改写来自社区 **LFS Mod Offline Fix**，并非上游或本项目原创；上游没有确切的最初作者/首发记录。本项目保留该说明，不擅自归为自己的工作。如有可核实的作者信息，应同步补充并告知上游。

游戏代码、素材、商标及相关内容仍归各自权利人。本项目不提供游戏内容下载，也不代表权利人的官方移植。
