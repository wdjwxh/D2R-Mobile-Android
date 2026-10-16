# 参与开发

- 先阅读 [BUILD](docs/BUILD.md)、[STATUS](docs/STATUS.md) 与 [DERIVED](docs/DERIVED.md)。
- 优先对照固定上游实现，说明 Android/Vulkan 的差异；保持 Vulkan 1.1 基线，新增可选特性必须有能力检查和正确回退。
- 不省略真实 GPU 完成、资源退役和 CPU/GPU 可见性语义来制造性能数字。
- 性能改动说明设备、场景、工作量和测量作用域，区分模拟器与原生手机，不把采样 CB 时间当整帧 GPU 时间。
- 提交必要源码和相关文档，不加入游戏文件、生成目录、私有输入、签名文件或个人存档。运行 `python scripts/check_repository.py` 并检查 staged diff。
- Issue 只提供必要且已脱敏的文字日志、版本和复现步骤；不要公开手机 IP、ADB 配对码、token 或游戏文件。
- 上游子模块版本升级需同步审查生成器锚点、游戏版本、派生表及 shader ABI。不要只修改 Git 指针就宣称兼容。
