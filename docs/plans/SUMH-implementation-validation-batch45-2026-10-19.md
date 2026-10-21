# SUMH 第四十五批实施与验证记录

日期：2026-10-19（Asia/Shanghai）。对应[54 项计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)和[进度账本](SUMH-implementation-progress-2026-10-16.md)。本批继续 **S04：可移植原生证据、最终 ELF 嵌入字节和发布产物关联**。S04 保持部分实施，全部目标继续进行；第 40–44 批封存证据只读保留。

## 1. 已实现行为

| 范围 | 实现与边界 |
| --- | --- |
| 原生证据迁移验证 | [native_evidence.py](../../scripts/ci/native_evidence.py)接收明确的 manifest、外部预期 target/commit 和全部产物路径，包括符号文件；验证快照、输入表、许可证、SBOM 投影及产物身份。导入记录中的原始源码、依赖和工具路径只作观察数据，不访问或执行。原 [native_provenance.py](../../scripts/ci/native_provenance.py)的本地源码重验合同保持不变。 |
| ELF 嵌入字节证明 | [embedded_payloads.py](../../scripts/ci/embedded_payloads.py)严格解析生产生成器输出，保存准确压缩流；在最终 ARM64 ELF 的文件承载、仅 PF_R 的 PT_LOAD 范围内定位并独立有界解压。核对 XZ solid pack 的成员偏移、校验和及普通 zlib 资产，区分 shell 换行规范化前后的身份；不重新压缩来推测匹配。 |
| 发布字节关联 | [release_provenance.py](../../scripts/ci/release_provenance.py)要求最终 APK 内 payload 等于可下载 ksud，嵌入 ksuinit/KMI 的原始与规范化身份等于对应下载文件，嵌入集合等于外部选定 KMI 加必需资源。核对两个原生 bundle 及符号、proof 与 manifest/snapshot 的交叉绑定。 |
| 严格打包 | [prepare_release.py](../../scripts/ci/prepare_release.py)CLI 必须提供 commit/KMI 上下文；先验证，再在新的同级暂存目录复制，完整复验后移至尚不存在的输出目录。拒绝覆盖已有发布目录。保留旧 APK 文件名、二进制/签名名称和 update.json schema 1；新增 `release-manifest.json`、`native-evidence.zip`、独立 ksuinit，以及 update.json 的 provenance 字段。 |
| 可移植发布验证 | 迁移后的发布目录可单独复验。ZIP 成员与文件清单精确对应，只将有界、明确允许的成员写入私有临时目录，不使用 extractall。符号、许可证和压缩 proof 都随证据 ZIP 提供。可验证自洽关系，不认证构建者或签名。 |
| CI 接入 | ksud 在 finalize/verify 后生成和验证嵌入 proof，再签名上传；repack 使用确定的单个 APK 和 canonical ksud，移除冗余 `--strip`，最终签名后再核对 payload；release 消费实际选定 KMI，严格打包并复验后才进入原有发布步骤。已有证书、GPG、同 commit 回归、tag 和 draft 门禁保留。 |

`package_release` 的无上下文 Python 兼容分支仍供历史调用者使用，不生成来源证明；只要传入任何严格上下文，就必须提供完整合法上下文。实际 CLI 总是严格模式。发布验证不代替 APK Manifest 版本/应用 ID 检查或外部签名认证。

证据明确标记 Manager/Gradle 和 DDK 内核的源码构建记录缺失，不能由父仓库 commit 补造。观察到相同字节不代表运行时注册表确实使用它们，也不证明内核兼容、观察者作用范围或可复现构建。容量与完整命令见[来源指南](../zh/guide/sumh-native-provenance.md)。

## 2. 静态复核、失败与修正

修改前保存 213 份 before-image，含未修改的 `repack_apk.py`，见 `out/batch45-before-manifest.json`。原生证据、嵌入 proof、发布聚合分别实施并交叉审阅；发布测试作者使用自行构造的 schema/SBOM、ARM64 ELF 和 ZIP 夹具，没有导入其他测试的构造器。

实际发现并修正：

- `PurePosixPath('.')` 的空 parts 让路径检查错误放行单点名称；portable-native 和 release 都明确拒绝，保留初次失败记录。
- Python 对 `True == 1` 的相等判断使未检查类型的 SBOM、许可证或发布摘要投影可能被接受；改为规范 JSON 的精确比较，覆盖布尔 version、零/一字节许可证大小及 dirty 状态的整数替换。
- JSON 完成大小检查后重新无界读取，可能读到增长后的文件；发布读取改为最多 MAX_JSON+1 字节并核对先前身份，解析同一批有界字节。
- ELF 文件偏移不重叠不代表虚拟地址不重叠；补查虚拟内存区间、BSS 边界、权限/映射冲突和地址溢出。相同物理映射的合法重复头另有接受用例。

独立报告：`out/batch45-portable-independent-review.md`、`out/batch45-embedded-independent-review.md`、`out/batch45-release-independent-review.md`。两项独立实际复现分别由“错误接受”变为“拒绝”，原结果未覆盖。测试作者自行运行测试属于实现验证，与另一作者的源码审查分开记录。

发布暂存保证本次检查通过前不产生最终目录，失败只清理本次创建的暂存目录；不宣称断电、并发写入或跨系统的全局事务。导入证据的所有声明仍是未签名数据，能重写全部证据的主体可以构造另一份自洽声明。

## 3. 实际脚本验证

主机 Windows，使用已有 Python、Android NDK 30.0.16248370、CMake 3.22.1/Ninja 和 actionlint 1.7.12。按最终有效套件计数，不重复累计早期尝试。

| 证据 | 实际结果 |
| --- | --- |
| `out/batch45-portable-tests-a5.log` | 37 项：36 通过、1 项因 Windows 无符号链接创建权限跳过。完整身份/角色/容量、投影、硬链接、原路径惰性和读取中变更覆盖。 |
| `out/batch45-embedded-tests-a3.log` | 25 项：24 通过、1 项同类权限跳过。使用生产生成器和独立 ELF/压缩预期，覆盖 shell 规范化、XZ 成员、映射冲突、解压边界和不可变发布。 |
| `out/batch45-release-tests-a3.log` | 44 项全部通过。精确单/全 KMI、全角色、迁移后不启动进程、不同源码范围、ZIP/内层篡改、版本兼容、大小边界、暂存和失败清理。 |
| `out/batch45-release-versions-a1.log` | 既有版本与兼容打包 9 项全部通过。 |
| `out/batch45-integration-a1/manifest.json` | payload 打包 11 项、构建选择 5 项、测试门禁 7 项全部通过；证书合同 14 项中 13 通过、1 因当前 PATH 未提供 Java keytool 跳过。14 个工作流 actionlint 通过，未启用 shellcheck/pyflakes。CI lock 检查通过，仍明确保留两处评论 Action 例外。 |
| `out/batch45-existing-apk-a1.log` | 只读核对第 43 批真实 Debug APK 与第 44 批真实 ksud，payload 精确相等。此项单独不覆盖 ksuinit/KMI 嵌入。 |
| `out/batch45-portable-real-a1/manifest.json` | 第 44 批两个真实原生 bundle、主产物及符号复制到新路径后验证通过；后续严格类型修正有单独回归。未重验旧源码或执行导入工具。 |

新增三套共 **106 项：104 通过、2 跳过、0 失败/错误**。既有集成共 **46 项：45 通过、1 跳过**；不将跳过项写成通过。证书测试使用夹具或 mock 的部分不等同于本批 APK 签名认证。

早期失败保留：portable a1 是单点路径实际缺陷，a3 是读取中变更测试注入点错误；release-boundaries a1 的两项真实缺陷修正后通过。中间通过尝试后因新增覆盖或交叉审查修正而重跑，最终计数以上表为准。

## 4. 实际构建与完整字节链

执行入口为 `out/batch45-populated-chain.py` 和 `out/batch45-run-native.py`，完整命令、退出码和日志摘要保存在对应 manifest。新构建使用实际 dirty 工作区，未退回 HEAD，也未改写旧构建证据。

| 证据 | 实际结果和范围 |
| --- | --- |
| `out/batch45-ksuinit-a1/manifest.json` | 新目录 ARM64 Release；configure、snapshot、build、23 项真实硬化检查、finalize、本地 verify 均退出 0。 |
| `out/batch45-ksud-a1/manifest.json` | 新目录 ARM64 Release；六阶段及 24 项真实硬化检查通过；再由生产生成的 assets_data.cpp 和最终 stripped ELF 创建并验证 proof，两步退出 0。本地 mbedTLS/miniz 离线覆盖明确记录，UPX 关闭。 |
| `out/batch45-populated-chain-a1/manifest.json` | 四个明确标记的自有合成 C 文件编译成 ARM64 ET_REL 对象，每份 1,240 字节，以 KMI 资源名测试打包；不是 DDK 构建或可加载内核模块。fresh ksuinit 与四份夹具暂存进实际构建输入。 |
| `out/batch45-ksud-a1/provenance/embedded/manifest.json` | 七项资产核对通过：installer.sh、shellrc.sh、真实 fresh ksuinit、四份合成 KMI；四组压缩流位于 ELF 的只读 load index 2。solid XZ 解压 4,960 字节，四个成员偏移为 0/1240/2480/3720。 |
| `out/batch45-populated-chain-a1/portable-release/` | 用未修改的生产 repack helper 把 fresh ksud 放入第 43 批真实 Debug APK，禁用 strip。没有重新 Gradle 构建、签名、安装或发布。AAPT 读取应用 ID/版本，严格 package API 完成验证与暂存；输出仍按兼容约定保留 `-release.apk` 后缀，名称不改变其测试/未签名性质。 |
| `out/batch45-populated-chain-a1/portable-release-verify.log` | 暂存的五个资产核对身份后移除，输入库存改名，最终目录移动后执行独立 portable CLI，退出 0。原暂存资产路径和输入库存不再可用，验证仍成功。 |

| 真实原生产物/本地测试包 | 字节数 | SHA-256 |
| --- | ---: | --- |
| fresh ksud | 4,390,304 | `e27d814e85a6f5907dd797de7f79d02d3c9ab68119f77d87297be7008e46148c` |
| ksud.unstripped | 21,876,512 | `82ef1a1c8c1dbff4862dd8e97764c4e8b68b8b613115b327dc81b2cf034f5085` |
| fresh ksuinit | 317,616 | `14af1c4191d5d128d98e427a236a66ef963c2beefcf4c508ede8d1582cbd2c69` |
| ksuinit.unstripped | 8,949,576 | `e88e6b2701751df36d1bd770761f530ddb410c79079613303620b522b6ab8f47` |
| 未签名重打包 APK | 35,665,839 | `a94c540ba03ee3b8df678bfc28ce6a3b221c39729b604f38b711dc9836de82ef` |
| native-evidence.zip | 9,457,681 | `3212bc84f22e6dc92710907ee60d346ff70d469916eae1e828c40247e21aeb12` |

精确库存见 `release-files.json`；`scope.json` 明确记录 APK 来自旧 Debug 包、四个 KMI 是合成对象，以及测试专用 kernel 元数据 `0.0.1-fixture / 1`。不能用这些字段或父 commit 冒充实际 DDK/KMI 来源。可移植报告中的 `consistent` 表示证据自洽，发布 CLI 的 AAPT/版本门禁在本次测试中没有被完整执行；本次调用的是严格 Python package API，AAPT 另行读出实际 APK 信息。

清理记录为 `asset-cleanup.json`，源 assets 目录恢复为原有两个 shell 文件。构建时本地 verify 已通过；清理后不得继续声称“当前源目录仍与该 snapshot 相同”，也不为消除这个预期差异重写快照。迁移验证检查保留产物和证据，不重验原始源码路径。

该流程验证生产生成器、真实原生链接、最终 ELF 字节和发布聚合之间的关联；没有实际装载四种 KMI，没有 Android/观察者测试、签名认证、远端 CI 或同目标双干净构建。原生构建的 47 项硬化检查与脚本测试分别计数。独立产物检查见 `out/batch45-populated-independent-review.md`，父代理的源码/测试/证据复核另存 `out/batch45-final-root-review.md`。

## 5. 未完成范围与后续

S04 继续部分实施。尚缺完整 Gradle/运行工具/NDK sysroot 依赖、Manager 和每个 LKM 的实际源码构建来源、未解析评论 Action、DDK 最小权限、签名认证、两次受控同目标干净构建及差异分析。远端 CI、四个真实 KMI、设备和性能验收不能由本机字节链测试替代。

F06/F11/F12 的 owner、事务和真实设备前置门禁保持；其他功能与全部 54 项任务继续按依赖推进。本批未提交、推送、安装、签名或发布，保留共享工作区既有修改。
