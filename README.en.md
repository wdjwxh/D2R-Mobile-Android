# D2R-Mobile-Android

## Archived: upstream now officially supports Android

2026-10-23: [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) now provides official Android support and APK releases. Independent port development here has ended; this repository is archived to preserve its implementation history, build documentation, and acknowledgements.

**New users should use upstream:**

- [Upstream README](https://github.com/jhaizhou-ops/d2r-mobile)
- [Android installation and import-pack instructions](https://github.com/jhaizhou-ops/d2r-mobile/blob/main/dm/android/README.md)
- [Official releases](https://github.com/jhaizhou-ops/d2r-mobile/releases)

Using the official v2.0.0 APK and a matching Android import pack generated from our existing local materials, we reached the Rogue Encampment on a Xiaomi 17 Max / Adreno 840, with Chinese UI and upstream touch controls. This validates the official route, not the performance of this repository's historical port; it is not full-game or long-term stability validation.

**The local recovery and packaging workflow is retained.** Archiving ends routine development on GitHub; it does not delete local recovery scripts, packaging tools, resources, or save backups. These remain available for future local builds and installation preparation. Private resources and generated import packs are not included in this documentation update.

Thank you to **jhaizhou, d2r-mobile, and all upstream contributors** for the porting foundation and official Android support. Follow upstream for future platform maintenance and releases.

---

The remaining text describes the historical independent port, including its previous measurements and limitations.

An experimental Android ARM64 port based on [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), using a Vulkan backend with a Vulkan 1.1 baseline and Qualcomm Adreno as the first hardware target.

**Bring your own legally obtained game files.** This repository contains compatibility-layer source and build tools, not the game executable, assets, translated game shaders, saves, keys, or a ready-to-play APK. APKs produced locally contain game-derived inputs and are not public release artifacts.

The Android emulator reaches the menu and camp and supports movement. Recent measurements were approximately 35 FPS in camp and 26 FPS in a busier area on one six-vCPU emulator. These are not native Adreno performance figures, full-game validation, or a guarantee of a 6 GiB total physical footprint. See [status](docs/STATUS.md).

Clone with `git clone --recurse-submodules https://github.com/wdjwxh/D2R-Mobile-Android.git`. The currently documented build workflow uses Windows, Git Bash, Android NDK r28b and JDK 17. Follow [build instructions](docs/BUILD.md) and [game-file preparation](docs/GAME_FILES.md); generated and private content stays in ignored local directories.

This work owes its foundation to **d2r-mobile**, its author **jhaizhou**, and its contributors: guest rewriting, host services, NVN semantics, shader translation, resource processing, iOS touch controls, and gameplay modules are reused or adapted from upstream. The Android work adapts that foundation to Android and Vulkan. See [acknowledgements and licenses](docs/ACKNOWLEDGEMENTS.md).

Source is MIT-licensed with original notices retained; third-party files keep their own licenses. No game-content rights are granted. This project is not affiliated with Blizzard or Nintendo.
