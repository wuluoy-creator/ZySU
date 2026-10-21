# Native third-party sources

ksud uses **git submodules** in this directory (`userspace/ksud/third_party/`) and **FetchContent** for some dependencies (see ../CMakeLists.txt).

## Submodules (this directory)

### MagiskbootAlone

- **Path**: `userspace/ksud/third_party/MagiskbootAlone`
- **Purpose**: Boot image unpack/repack/split-dtb; compiled **into ksud** as a multi-call binary.
- **Usage**: `ksu/bin/ksud` is the main binary; `ksu/bin/magiskboot` should be a **symlink to ksud**. When invoked as `magiskboot`, ksud dispatches by `argv[0]` to `magiskboot_main()`.
- **Init/update**: `git submodule update --init userspace/ksud/third_party/MagiskbootAlone`

### bootctlAlone

- **Path**: `userspace/ksud/third_party/bootctlAlone`
- **Purpose**: Compiled **into ksud** as multi-call (like magiskboot). `ksu/bin/bootctl` → symlink to ksud; argv0 dispatch to `bootctl_main()`.
- **Init/update**: `git submodule update --init userspace/ksud/third_party/bootctlAlone`

### resetpropAlone

- **Path**: `userspace/ksud/third_party/resetpropAlone`
- **Purpose**: Compiled **into ksud** as multi-call. `ksu/bin/resetprop` → symlink to ksud; argv0 dispatch to `resetprop_main()`.
- **Init/update**: `git submodule update --init userspace/ksud/third_party/resetpropAlone`

### ndk-busybox

- **Path**: `userspace/ksud/third_party/ndk-busybox`
- **Purpose**: Compiled **into ksud** as multi-call. `ksu/bin/busybox` → symlink to ksud; argv0 dispatch to `busybox_main()`.
- **Init/update**: `git submodule update --init userspace/ksud/third_party/ndk-busybox`

### readelf_toyboxAlone

- **Path**: `userspace/ksud/third_party/readelf_toyboxAlone`
- **Purpose**: Android 16 Toybox `readelf` compiled **into ksud** as a namespaced multi-call applet, plus a reentrant structured ELF-header API shared by ksud and the CLI. `ksud readelf` works without a ROM-provided executable.
- **License**: 0BSD
- **Init/update**: `git submodule update --init userspace/ksud/third_party/readelf_toyboxAlone`

## FetchContent and provenance

PicoSHA2 is no longer an input of this build. The integrated build uses mbedTLS
for hashes and cryptographic operations, miniz for ZIP handling, and the LZ4
vendored in MagiskbootAlone when present (otherwise the locked upstream LZ4).
mkbootfsAlone is also an integrated submodule for ramdisk filesystem creation.
The authoritative FetchContent identities are in
[`../../../cmake/native-dependencies.lock.json`](../../../cmake/native-dependencies.lock.json).

### Management

Initialize submodules with `git submodule update --init --recursive`. Gitlinks
pin their expected commits; the actual checkout and local modifications are
recorded separately. The integrated build selects MagiskbootAlone sources
directly; its standalone CMake project is a separate build surface.

The configure-time `native-dependencies.json` records actual source directories,
declared immutable identities and local source overrides. A
`FETCHCONTENT_SOURCE_DIR_*` override bypasses normal download verification.
Provenance labels that fact and fingerprints the actual files; an offline
source override is not verified by the archive hash.

Native CI emits a build manifest, partial native CycloneDX SBOM and copied
license evidence. These describe observed inputs and outputs; they are not
signed attestations, a complete Android/Gradle graph or a determination that a
top-level license covers all vendored files. Retain component-specific notices,
including source headers where those contain the notice.

To update a dependency, resolve the intended upstream version to an immutable
commit or release archive digest, update the lock and review source/license
differences. Run dependency/provenance regressions and affected native builds.
A second clean build and device acceptance are separate checks. Dependabot
currently handles GitHub Actions and Gradle here; it does not update the native
lock. See the [provenance guide](../../../docs/zh/guide/sumh-native-provenance.md)
for commands, evidence boundaries and remaining S04 work.
