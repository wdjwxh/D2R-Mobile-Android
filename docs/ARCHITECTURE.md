# 移植架构

本项目复用 d2r-mobile 的兼容层思路，替换平台相关实现，而不是重新实现游戏玩法。

1. 自有游戏程序经上游改写，再适配 Android TLS 访问，包装为 ARM64 ELF；装载器应用 relocation、gate 与运行时服务。
2. Android 系统服务、文件系统、线程、音频、键盘和输入提供游戏所需接口；上游 iOS C 触控桥及部分 HUD/玩法源码在构建时生成 Android 适配副本。
3. NVN 接口将命令录入游戏命令内存，按 FIFO 在翻译线程回放成 Vulkan；CPU 翻译完成不等同于 GPU 完成。
4. 游戏 shader 在本机离线翻译为 Vulkan 1.1 SPIR-V，运行时创建/缓存设备管线。Metal 编译缓存不能直接用于 Vulkan。
5. guest backing 由 Vulkan 分配并映射给 CPU/GPU 共享；资源可见性、读写冲突与 fence 生命周期仍需维护。共享内存不意味着可以省去同步。

## Vulkan 路径

- 批量 command buffer/FIFO 提交，复用 pipeline、descriptor、sampler/view 和资源准备结果。
- 普通 optimal 图像按 memory type 分配 32 MiB 内存块；需要/偏好 dedicated 或大图使用独立分配。退役后再回收，避免把在途 GPU 资源提前释放。
- 普通游戏帧不做 CPU framebuffer 回读；确实需要 CPU 观察结果的路径仍执行正确同步。
- 低版本设备保留 fence 完成路径，不把 Vulkan 1.2/1.3 特性当作无条件要求。
- 记录 Vulkan 自有分配，与 CPU RSS、驱动私有内存分开解释；目前约 6 GiB 是规划目标，不是已实现的全进程硬上限。

## 与上游的差异

上游 Metal 的 render-pass store action、tile 读取、GPU 地址与触控外壳不能逐 API 名字机械替换。例如 Metal 未写附件的省写回不能简单改为 Vulkan `STORE_OP_DONT_CARE`：后者允许丢弃旧内容。正确路径需要设备能力和完整通道的依赖/写入证明。

FUSE/HOIST 相关原型保留在源码中，但当前普通配置未启用昂贵的预扫描规划；不要把“代码存在”写成真实游戏已覆盖。完整触控、i18n、玩法功能与原生硬件表现仍按 [STATUS.md](STATUS.md) 分项验证。
