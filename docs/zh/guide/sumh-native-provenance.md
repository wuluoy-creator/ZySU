# 原生构建来源与发布字节验证

S04 的目标是把源码、依赖、工具链和最终产物关联起来。第 44 批接入
`ksud` 和 `ksuinit` 原生构建，记录实际输入、产物、有限范围的原生 SBOM
和许可证证据。第 45 批补齐证据迁移验证、最终 ELF 嵌入字节证明，以及
APK → ksud → 选定 ksuinit/KMI 下载文件的字节关联。

这些工具检查记录和字节是否一致。它们不认证构建者或签名，迁移验证也不会
重新读取原机器上的源码和工具。Manager/Gradle、每个 LKM/DDK 的源码构建
来源、完整依赖图及可复现构建仍需后续工作，不能把字节关联称为完整来源证明。

## 固定来源和实际来源

[`native-dependencies.lock.json`](../../../cmake/native-dependencies.lock.json)
保存已选择版本的不可变来源。miniz 与备用 LZ4 使用完整 Git commit；mbedTLS
保留官方发布压缩包及其 SHA-256。Git 子模块继续由仓库 gitlink 固定。
集成的 ksud 构建直接选择 MagiskbootAlone 源文件，不等于执行其独立 CMake 项目。

CI 的 Action 引用和 DDK 镜像另由
[`ci-dependencies.lock.json`](../../../.github/ci-dependencies.lock.json)记录。
已解析的 11 类 Action 使用核实后的完整 commit，版本注释便于升级审查；
Gradle Action 使用注解标签指向的 commit，保留原子目录入口。四个既有 KMI
按 `KMI + ddk_release` 离线选择固定镜像 digest，未知组合拒绝构建，不回退
到可变 tag。新增 DDK release 必须先审查并补入锁定清单。

设备登记流程中的 `Kernel-SU/actions-comment-on-issue@master` 本次无法查询
来源，仍保留为两处明确例外。检查器只允许它出现在既有 `add-device.yml`，
不能用于构建或发布工作流，也不会把检查结果标为“全部执行依赖已固定”。
DDK 的 `--privileged` 需求仍未验证；固定镜像不等于完成权限隔离。

CMake 配置阶段输出 `native-dependencies.json`，记录实际依赖目录和来源声明。
正常下载使用固定来源；指定 `FETCHCONTENT_SOURCE_DIR_*` 时使用调用者提供的
源码目录，该行为明确记录为本地覆盖。离线构建仍可使用已有源码，但不能因为
配置里有 `URL_HASH` 就声称覆盖目录已通过那个压缩包摘要验证。

## 一次新构建的记录步骤

目的：核对一次原生构建前后的相关输入，保留产物及诊断符号的精确摘要。
适用条件：已准备好 Android NDK、CMake、Ninja、Python 和必要依赖；使用新的构建目录。
不需要安装 APK 或在设备上运行 SUMH。

可先离线检查 CI 引用；该检查要求独立行的 `uses:` 字段，同时需要 actionlint
验证 YAML 和 GitHub Actions 结构，不把文本检查器当成完整 YAML 解析器：

```sh
python3 scripts/ci/ci_dependencies.py check
python3 scripts/ci/ci_dependencies.py resolve-ddk --kmi android14-6.1 --release 20260828
```

先使用当前项目 CMake 完成 configure，并明确指定 `-G Ninja`。来源工具目前
要求 `build.ninja` 及其规则文件，以及当前 CMake 输出的 `native-toolchain.json`；
其他生成器尚不支持这套记录步骤。然后在仓库根目录执行下列命令。
`out/native-build` 代表该次配置的构建目录，不能拿已有二进制补写成新构建证据：

```sh
python3 scripts/ci/native_provenance.py snapshot \
  --repo . --target ksud --build-dir out/native-build \
  --out out/native-evidence \
  --artifact ksud=out/native-build/ksud \
  --artifact ksud.unstripped=out/native-build/symbols/ksud.unstripped \
  --dependencies out/native-build/native-dependencies.json

cmake --build out/native-build --target ksud

python3 scripts/ci/native_provenance.py finalize \
  --snapshot out/native-evidence/source-snapshot.json
python3 scripts/ci/native_provenance.py verify \
  --manifest out/native-evidence/build-manifest.json
```

`ksuinit` 使用相应 target 和文件名，并省略 `--dependencies`。快照时声明的
产物必须尚不存在；完成构建后才能 finalize。若构建期间相关源文件、依赖或
工具链身份发生变化，不能把它们与这次产物关联为通过，应保留失败记录并以
新的目录重新构建。验证失败不能通过修改清单中的摘要来“修复”。

finalize 只允许成功执行一次；再次调用会报错并保留原证据，不代表本次操作
成功。脚本必须检查退出码，且消费或上传前执行 verify；不能仅凭旧清单存在
就认定本次构建通过。不同构建使用不同证据目录。

## 如何阅读结果

| 输出 | 能说明什么 | 不能说明什么 |
| --- | --- | --- |
| `source-snapshot.json` | 构建前的相关源码、Git 状态、依赖和配置身份 | 完整系统镜像、未记录的环境与远端服务 |
| `build-manifest.json` | 输入前后核对、实际产物大小和 SHA-256、声明的工具链与资产 | 构建者身份认证、签名证明、设备运行正确性 |
| `native-sbom.cdx.json` | 记录范围内原生源码组件及其实际身份 | 完整 Gradle、NDK/sysroot 或内核依赖图 |
| `licenses/` | 实际来源中保存的许可证材料 | 自动法律结论或所有文件受同一许可证覆盖 |

本地 dirty 源码和覆盖目录有各自的内容摘要，不能只凭 HEAD 相同就称为相同
输入。嵌入的 ksuinit、LKM 等资产以实际文件摘要记录；只有资产摘要还不足以
证明它们自身的源码和工具链来源。许可证材料缺失必须保留缺失状态。

CI 在构建前 snapshot、硬化检查后 finalize/verify，通过后单独上传
`ksud-provenance-<target>` 与 `ksuinit-provenance-<target>`。既有二进制、
诊断符号和签名产物名称保持原约定，既有签名和发布门禁继续生效。
上面的 `native_provenance.py verify` 会重新核对当前本地输入，需要对应源码、
依赖和工具路径；迁移后的接收方使用下面的独立入口。

## 在另一目录或机器上验证原生证据

目的：核对收到的原生证据及全部声明产物，不依赖原构建目录。
保留整个证据目录中的 `build-manifest.json`、`source-snapshot.json`、
`native-sbom.cdx.json` 和 `licenses/`，同时准备可执行文件与未剥离符号文件。
`COMMIT_SHA` 必须由本次预期构建的可信上下文提供，为完整的 40 或 64 位小写
十六进制 commit；不能从待验证清单抄出一个值，再声称独立确认了构建来源。

例如下载文件采用当前 CI 目录结构时，在仓库根目录执行：

```sh
python3 scripts/ci/native_evidence.py verify \
  --manifest artifacts/ksud-provenance-aarch64-linux-android/build-manifest.json \
  --target ksud --commit "$COMMIT_SHA" \
  --artifact ksud=artifacts/ksud-aarch64-linux-android/ksud \
  --artifact ksud.unstripped=artifacts/ksud-symbols-aarch64-linux-android/ksud.unstripped

python3 scripts/ci/native_evidence.py verify \
  --manifest artifacts/ksuinit-provenance-aarch64-linux-android/build-manifest.json \
  --target ksuinit --commit "$COMMIT_SHA" \
  --artifact ksuinit=artifacts/ksuinit-aarch64-linux-android/ksuinit \
  --artifact ksuinit.unstripped=artifacts/ksuinit-symbols-aarch64-linux-android/ksuinit.unstripped
```

每个声明角色必须明确提供一次，包括 `.unstripped`；没有只验主程序而略过
符号的隐式降级。工具核对目标、外部 commit、快照和输入表摘要、许可证、
SBOM 投影及实际文件摘要。导入的原始源码、依赖、工具和产物路径均为观察
数据，不会被打开或执行，也不用于寻找缺失的产物。

成功返回 `status: consistent` 和 `coverage: portable_evidence_consistency`，
并明确 `source_revalidated: false`、`toolchain_revalidated: false` 和
`embedded_bytes_verified: false`。后者由下一步独立证明。迁移验证不修改证据，
失败时保留收到的文件并核对缺失角色、文件变化及来源声明，不手动改摘要。

## 证明最终 ksud 中实际存在嵌入字节

目的：把暂存资产和生产生成器输出绑定到最终 ELF 中的压缩字节。
在构建机器完成本地 finalize/verify 后、清理暂存资产前执行。下面继续使用
前面的 `out/native-build` 和 `out/native-evidence`；输出的 `embedded` 目录
必须尚不存在。

```sh
python3 scripts/ci/embedded_payloads.py create \
  --native-manifest out/native-evidence/build-manifest.json \
  --binary out/native-build/ksud \
  --generated-cpp out/native-build/generated/assets_data.cpp \
  --assets-dir userspace/ksud/assets \
  --output-dir out/native-evidence/embedded

python3 scripts/ci/embedded_payloads.py verify \
  --proof out/native-evidence/embedded/manifest.json \
  --binary out/native-build/ksud \
  --native-manifest out/native-evidence/build-manifest.json
```

`create` 严格读取生产生成器的数组和注册表，核对暂存文件，保存准确的压缩流
到 `embedded/payloads/`，并记录 `embedded/manifest.json`。普通资产使用 zlib，
KMI 共用 XZ/CRC32 solid pack，成员偏移必须连续且摘要一致。shell 的原始文件
与 LF 换行规范化后的内容分别记录；ksuinit/KMI 必须保持原始字节身份。

校验只接受完整位于 ARM64 ET_DYN ELF 的文件承载、仅 PF_R 的 PT_LOAD 段内
的准确压缩流，拒绝尾部数据、可写/可执行段、重复位置、相互冲突的虚拟映射
和 BSS 覆盖。解压受大小和内存上限约束，不靠重新压缩推测字节是否匹配。
结果说明这些已分配字节存在，不代表运行时注册表一定使用它们或设备提取成功。

迁移后，`verify` 只需要上述 proof 目录、对应原生清单及快照、实际 ksud；
不需要生成的 C++ 或原始暂存目录。仍应先运行上一节的完整原生证据验证，
因为嵌入证明本身不代替 SBOM/许可证等全部检查。CLI 会交叉核对 proof 与
原生 manifest/snapshot 的身份。新建失败只清理本次暂存目录，不覆盖既有证明。

## 严格组装并复验发布目录

目的：把最终 APK、独立原生文件、选定 KMI 和可迁移证据绑定在同一发布清单中。
当前目标为 `aarch64-linux-android` / `arm64-v8a`。输入采用以下当前 CI 角色：

| 输入目录 | 必需文件 |
| --- | --- |
| `Manager-arm64-v8a/` | 一个直接位于该目录的最终 `.apk` |
| `ksud-aarch64-linux-android/`、`ksuinit-aarch64-linux-android/` | 对应的 `ksud`、`ksuinit` |
| `ksud-symbols-aarch64-linux-android/`、`ksuinit-symbols-aarch64-linux-android/` | 对应的 `ksud.unstripped`、`ksuinit.unstripped` |
| `ksud-provenance-aarch64-linux-android/`、`ksuinit-provenance-aarch64-linux-android/` | 两个完整原生证据目录；ksud 还须包含 `embedded/` |
| `<KMI>-lkm/` | 对应的 `<KMI>_kernelsu.ko` |

KMI 选择必须来自构建计划，不能按下载目录中碰巧有哪些文件推断。目前支持
`android14-6.1`、`android15-6.6`、`android16-6.12`、`android17-6.18`；
`--kmi` 接受其中唯一、非空的逗号分隔列表，`all` 表示这四项。缺少、重复或
多出的 KMI、角色和歧义 APK/签名名称会拒绝。已有可选 `.sig` 文件仍可携带，
这里核对其字节身份，签名真实性由独立签名流程认证。

先确认最终 APK 中唯一的运行库标记与 payload 等于独立 ksud。当前库名为
`libruntime.so`，修改时必须与 `manager/ksud-library-name.txt` 保持一致：

```sh
python3 scripts/ci/release_provenance.py apk \
  --apk artifacts/Manager-arm64-v8a/final.apk \
  --ksud artifacts/ksud-aarch64-linux-android/ksud \
  --library-name libruntime.so
```

`final.apk` 是示例文件名，使用实际唯一 APK。生产流程在签名完成后检查最终
文件；重打包直接使用已验证的 ksud，CI 不再额外 strip 后改变 canonical 字节。

严格组装 CLI 需要 Android SDK 的 `apkanalyzer`，以及构建流程已经解析出的
`MANAGER_VERSION_NAME`、`MANAGER_VERSION_CODE`、`KSUD_VERSION_NAME`、
`KSUD_VERSION_CODE`、`KERNEL_VERSION_NAME`、`KERNEL_VERSION_CODE` 环境变量。
Manager 版本必须与最终 APK Manifest 一致，应用 ID 必须为 `com.zying.zysu`。
本例假定这些环境变量、外部 `COMMIT_SHA` 和实际 `EXPECTED_KMI` 已由对应构建
上下文提供，`release-assets` 尚不存在：

```sh
python3 scripts/ci/prepare_release.py \
  --artifacts artifacts --output release-assets \
  --commit "$COMMIT_SHA" --kmi "$EXPECTED_KMI"

python3 scripts/ci/release_provenance.py verify \
  --release-dir release-assets
```

CLI 始终使用严格模式，库名从共享配置读取。它检查两套完整原生证据和符号，
交叉绑定嵌入 proof，要求资产集合恰好是两个 shell、ksuinit 和外部选定 KMI，
再核对 APK payload 与 ksud、嵌入 ksuinit/KMI 与对应可下载文件的原始及解压
身份。Python `package_release` 为历史调用者保留的无上下文分支不提供这些
证明；传入任意严格上下文字段就必须完整提供三项合法上下文。

组装先验证输入，在新建的同级暂存目录复制并完整复验，再移到尚不存在的
最终目录；保留已有输出，不宣称断电或并发写入下的全局事务。输出保留旧 APK
文件名、已有二进制/签名名称和 `update.json` schema 1，增加独立
`ksuinit-aarch64-linux-android`、`release-manifest.json`、`native-evidence.zip`
以及 update.json 的 `provenance` 字段。ZIP 包含两套原生证据、两个符号文件
和压缩 proof；不把运行 inode、设备授权或 SUMH 规则混入发布证明。

复制完整发布目录到另一位置后，可以再次使用上面的 `verify --release-dir`。
它核对精确文件/ZIP 成员清单，在私有临时目录仅写入有界且明确允许的证据
成员，然后复验内部关联；不读取清单中原来的源码或构建路径。这个入口使用
发布清单内声明的 commit/KMI 检查自洽性，接收方仍需独立核对期望 commit 和
签名来源，不能把自洽性当成来源认证。单独的 `apk` 和 `verify` 子命令也不
替代组装 CLI 的 APK Manifest 版本检查。

## 容量与失败边界

当前上限是输入合同，超限会失败，不通过忽略条目或放大 IPC 缓冲继续处理。
容量单位为二进制 MiB/GiB：

| 检查范围 | 当前主要上限 |
| --- | --- |
| 可移植原生证据 | 每份 JSON 64 MiB，单文件 512 MiB；每源文件表 50,000 项 / 2 GiB，源码与依赖表合计 300,000 项；声明产物 32 个、依赖组件 128 个；单许可证 16 MiB、许可证合计 128 MiB；JSON 深度 32、节点 2,000,000。 |
| ELF 嵌入证明 | ELF 和生成 C++ 各 512 MiB；单资产 128 MiB、解压合计 512 MiB、压缩流合计 256 MiB；资产/组各 32 个、程序头 128 个；proof JSON 1 MiB，原生 JSON 64 MiB；XZ 解码内存 64 MiB。 |
| 发布验证 | 单文件 512 MiB、JSON 16 MiB；APK 最多 8,192 个成员、展开合计 2 GiB；证据 ZIP 最多 1,024 个成员、展开合计 512 MiB。 |

未来格式、重复 JSON 键、类型不符、路径别名/穿越、符号链接或 reparse 路径、
缺失文件、摘要变化和解压边界错误均应保留失败记录。原生迁移、嵌入和发布
验证 CLI 成功时向 stdout 输出 JSON，校验失败向 stderr 输出结构化错误并
返回非零；脚本需检查退出码。严格组装 CLI 还会报告参数、SDK/Manifest 或
构建版本错误，不能假定所有入口都有同一错误 JSON 格式。

## 更新、失败处理与撤销

更新依赖时记录原版本、目标版本、不可变 commit/摘要和来源，检查变更与
许可证，修改 lock 后运行相关原生测试和构建。不要为消除下载错误换成另一
个未审查版本，也不要用当前工作目录冒充上游干净源码。

清单、SBOM 或嵌入 proof 生成失败会阻止本次 CI 正常上传步骤；严格发布校验
失败会阻止后续发布步骤。保留错误和输入，判断是来源变化、缺少产物、无效
路径还是容量限制；不要复用此前目录里的成功文件。
撤销一次来源更新应恢复经过审查的旧 lock/代码，并创建新的构建记录。
本工具不修改设备配置，也没有运行规则的恢复或全局清理操作。

## 仍需完成的范围

S04 保持部分实施。APK、ksud、ksuinit 和选定 KMI 已有统一字节清单，但
Manager/Gradle 与各 LKM/DDK 的真实源码构建来源、完整工具与 NDK sysroot
依赖、未解析评论 Action、DDK 最小权限、签名认证和两次受控干净构建差异
分析仍要分别验收。远端 Actions、真实 KMI、Android/内核运行、许可证全面
合规审查和性能测试不能由本机脚本测试替代。

第 45 批实际验证包含新建目录中的 ARM64 ksud/ksuinit 构建、四个合成 ARM64
ET_REL 对象，以及已有 Debug APK 的未签名重打包；后两者不构成真实内核模块、
新 Gradle 构建或签名验收。构建、静态审查、脚本测试和未执行项分别见
[第 44 批记录](../../plans/SUMH-implementation-validation-batch44-2026-10-19.md)与
[第 45 批记录](../../plans/SUMH-implementation-validation-batch45-2026-10-19.md)。
全部 54 项计划继续推进，F06/F11/F12 的 owner、事务和真实设备前置门禁保持。
