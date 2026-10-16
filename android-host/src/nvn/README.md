# NVN guest ABI

Vendored from `jhaizhou-ops/d2r-mobile` commit
`944e199c8a4833853ce6c6e7c73751fa5840811c`:

- `nvn.h`: NVN types and enums; original MIT notice retained.
- `nvn_protos.h`: upstream generated function signatures.
- `names.txt`: upstream 523-entry API inventory.
- `missing.inc`: regenerate with `python android-host/src/nvn/generate_missing.py`.

`hle_nvn.c` checks implemented signatures against these typedefs at compile time.
Its device/queue methods and packed state objects follow upstream semantics;
execution belongs to the Android Vulkan backend. Recognizing a name is not an
implementation: unresolved methods terminate with the exact API name.

Custom multisample positions follow upstream's setter behavior, which discards
them. Vulkan execution and verification limits are documented in
`docs/ARCHITECTURE.md` and `docs/STATUS.md` at the repository root.
