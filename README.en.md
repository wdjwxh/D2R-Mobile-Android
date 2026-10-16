# D2R-Mobile-Android

An experimental Android ARM64 port based on [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), using a Vulkan backend with a Vulkan 1.1 baseline and Qualcomm Adreno as the first hardware target.

**Bring your own legally obtained game files.** This repository contains compatibility-layer source and build tools, not the game executable, assets, translated game shaders, saves, keys, or a ready-to-play APK. APKs produced locally contain game-derived inputs and are not public release artifacts.

The Android emulator reaches the menu and camp and supports movement. Recent measurements were approximately 35 FPS in camp and 26 FPS in a busier area on one six-vCPU emulator. These are not native Adreno performance figures, full-game validation, or a guarantee of a 6 GiB total physical footprint. See [status](docs/STATUS.md).

Clone with `git clone --recurse-submodules https://github.com/wdjwxh/D2R-Mobile-Android.git`. The currently documented build workflow uses Windows, Git Bash, Android NDK r28b and JDK 17. Follow [build instructions](docs/BUILD.md) and [game-file preparation](docs/GAME_FILES.md); generated and private content stays in ignored local directories.

This work owes its foundation to **d2r-mobile**, its author **jhaizhou**, and its contributors: guest rewriting, host services, NVN semantics, shader translation, resource processing, iOS touch controls, and gameplay modules are reused or adapted from upstream. The Android work adapts that foundation to Android and Vulkan. See [acknowledgements and licenses](docs/ACKNOWLEDGEMENTS.md).

Source is MIT-licensed with original notices retained; third-party files keep their own licenses. No game-content rights are granted. This project is not affiliated with Blizzard or Nintendo.
