# 仓库内容与生成内容的边界

参考上游 [DERIVED.zh-CN.md](../d2r-mobile/docs/DERIVED.zh-CN.md)。本页描述 Android 仓库本身，不代表对所有未来提交的自动保证。

## 跟踪的内容

- Android/Vulkan 兼容层源码、Java 外壳和构建/转换工具。
- `android-host/shaders/counter.comp`、`timestamp.comp` 是宿主自行编写的计数/时间戳转换工具 shader 源码，不是从游戏翻译出的 shader。
- 接口声明、函数名、槽位、地址、shader 键、反射规则等兼容所需的事实与元数据；少量指令字用于明确的改写/版本核对，未嵌入游戏程序映像。
- 上游 MIT 版权声明与第三方文件内的原有许可说明。
- 固定到公开上游提交的 Git 子模块引用，不是带私有构建输入的整份工作目录快照。

## 不跟踪的内容

- 游戏程序、Guest.framework、改写后模块与装载表生成物。
- 原始/翻译游戏 shader、SPIR-V、着色器包、graphics.vkpack 与 shader catalog 生成物。
- RomFS/CASC 档案与覆盖层、美术、字体、触控 atlas/HUD 素材、文字及数据表。
- IPA/APK、SDK、工具二进制、签名证书/密钥、主机密钥、token、存档、截图、运行转储及个人路径交接记录。

这些内容由开发者从自己的输入在本机生成，放在 Git 忽略目录。忽略规则不能撤销已经提交过的文件，也不能识别所有重命名的游戏内容。提交前运行：

```bash
git add <明确选择的源码或文档>
python scripts/check_repository.py
git diff --cached --stat
git diff --cached
```

检查器审查 Git 索引中的路径、大小和常见凭据形态，**不宣称逐字节证明“没有任何游戏内容”或代替人工审查**。不要用强制 add 绕过私有目录。当地 APK 包含游戏衍生内容；当前仅准备发布源码，不能把它当成无游戏内容的壳分发。
