# SUMH 第四十四批实施与验证记录

日期：2026-10-19（Asia/Shanghai）。对应[54 项计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)和[进度账本](SUMH-implementation-progress-2026-10-16.md)。本批推进 **S04：原生依赖锁定与构建来源记录**。S04 保持部分实施，全部目标继续进行；先前批次的封存证据未重跑或覆盖。

## 1. 已实现行为

| 范围 | 实现与边界 |
| --- | --- |
| 原生依赖 | [原生 lock](../../cmake/native-dependencies.lock.json)固定 miniz 3.0.2、备用 LZ4 及预声明子项目回退的完整 commit；mbedTLS 保留官方压缩包及既有 SHA-256。完整 Git commit 不使用浅克隆。集成 ksud 直接选择 MagiskbootAlone 源文件，不执行其独立 CMake；未消费的回退依赖不冒充实际构建组件。 |
| 配置元数据 | 两个目标输出实际 `native-toolchain.json`；ksud 另输出 `native-dependencies.json`。记录真实配置变量、依赖来源目录、不可变身份、覆盖/离线状态及许可证路径。只记录指定字段，不导出完整环境。 |
| 构建前快照 | [来源工具](../../scripts/ci/native_provenance.py)在 configure 后、首次 build 前 snapshot；要求声明产物尚不存在。绑定相关一方源码、dirty/untracked/ignored 内容、六个 gitlink 和实际子模块源码、依赖、生产嵌入资产、版本生成器输入、实际工具二进制/链接身份及配置。Ninja 的 include/subninja 规则图递归纳入指纹。 |
| 完成与核验 | finalize 再次核对输入，要求产物非空，生成版本化 `build-manifest.json`、有限原生 CycloneDX 1.5 SBOM 和许可证证据。verify 检查原快照绑定、产物、许可证、SBOM 和当前原始输入；重验不会执行导入快照里的工具路径。 |
| Actions | [CI lock](../../.github/ci-dependencies.lock.json)记录 11 类已解析 Action 的完整 commit；实际 54 处远程引用中 52 处已固定。两处 `Kernel-SU/actions-comment-on-issue@master` 查询返回 Repository not found，保留为仅限既有设备登记流程的例外，不允许扩展到构建/发布。检查器明确返回 `all_execution_dependencies_pinned:false`。 |
| DDK | 四个既有 KMI 的 `20260828` 镜像按实际 registry manifest/config 摘要核实；`KMI + release` 离线解析固定 digest，未知组合失败，不回退可变 tag。只查询镜像元数据，未下载运行镜像层；既有 `--privileged` 保留待评估。 |
| CI 接入 | ksud/ksuinit 在构建前 snapshot、硬化检查后 finalize/verify，再签名及上传。来源记录分别上传为 `ksud-provenance-<target>` 和 `ksuinit-provenance-<target>`；既有二进制/符号产物结构及发布门禁保留。`prepare_release.py` 未改，独立 CI 证据尚不是 release 附件。 |

命令、失败处理、升级和撤销见[使用指南](../zh/guide/sumh-native-provenance.md)。finalize 为一次性操作：拒绝覆盖已成功证据，失败退出不授权继续消费；必须检查退出码并执行 verify。清单是未签名的本地观察记录，尚需原始源码、依赖和工具路径进行核验，不是可移植签名认证或可复现构建证明。

本地 `FETCHCONTENT_SOURCE_DIR_*` 覆盖记录实际内容摘要，不声称由 lock 中压缩包摘要认证。许可证材料只作证据，不推断顶层许可证覆盖所有 vendored 文件。完整 sysroot、Gradle 及内核依赖图仍在范围外。

## 2. 静态复核与修正

修改前保存 192 份 before-image，见 `out/batch44-before-manifest.json`。原生依赖/元数据、来源工具、CI 锁定与集成分别实施；独立测试作者检查 CI 引用，并复核签名及发布流程与批前内容。来源工具由原生代理独立审查，其自行实现的元数据 helper 不计作独立审查自身代码。

已发现并修正的实际问题：

- CI 引用检查最初漏扫 `.yaml`，且未拒绝重复 action/version 指向不同 commit、已解析与未解析项重叠；补齐扫描、唯一性约束和回归测试。仅支持独立行 `uses:`，不支持的 inline/quoted key 明确拒绝；同时保留 actionlint 的结构检查。
- NDK 编译器和 launcher 可能是 CMake 普通变量，未进入缓存或与缓存不同；改由 configure 输出实际变量，缺少实际 launcher 不回退旧缓存。
- 只记录 `build.ninja` 会漏掉实际编译命令所在的 `CMakeFiles/rules.ninja`；补齐有界递归规则图，动态或越界 include 拒绝。
- 二次 finalize 的约定统一为保留不可变旧证据并报错，消费必须检查退出码和 verify；没有删除旧证据来掩盖失败。

审查材料为 `out/batch44-ci-review.md`、`out/batch44-provenance-independent-review.md` 和各作者报告。Action/DDK 来源查询另存 `out/batch44-workflow-research.json`；原生 ref 和许可证查询材料独立保留。查询身份不等于审计全部第三方实现。

文档独立复核见 `out/batch44-document-review.md`；已补明指南的 Ninja 配置前提，并限定本次构建实际包含的嵌入资产，避免把夹具覆盖写成完整发布包验证。

## 3. 实际执行记录

主机为 Windows，使用已安装 Python、Android NDK 30.0.16248370、CMake 3.22.1/Ninja 和 actionlint 1.7.12。以下按各套件最终有效结果列出，不重复累计重跑次数。

| 证据 | 实际结果 |
| --- | --- |
| `out/batch44-native-tests-a3.log` | 10 项：9 通过、1 项因 Windows 无符号链接创建权限跳过；包含实际 CMake 普通变量/缓存差异夹具。 |
| `out/batch44-provenance-tests-a4.log` | 44 项：42 通过、2 项因同类权限跳过。覆盖输入/工具/Ninja/资产/版本变化、缺失及空产物、未来/重复/越界格式、许可证/SBOM 绑定、覆盖语义、不可变 finalize 和重验不执行导入工具。 |
| `out/batch44-ci-tests-a2.log` | 24 项全部通过；离线锁定、例外限制、DDK 选择与注入边界。 |
| `out/batch44-integration-a1/manifest.json` | 14 个工作流 actionlint 通过（未开启 shellcheck/pyflakes）；实际 CI lock 检查通过并保留两处例外。测试 runner 门禁 7 项、构建选择 5 项、发布打包 9 项全部通过。 |
| `out/batch44-ksud-a1/manifest.json` | 新构建目录 ARM64 Release；configure、snapshot、build、24 项真实产物/编译命令硬化检查、finalize、verify 均退出 0。使用明确标记的本地 mbedTLS/miniz 离线源码覆盖，UPX 关闭。 |
| `out/batch44-ksuinit-a1/manifest.json` | 独立新目录 ARM64 Release；相同六阶段均退出 0，23 项真实产物/编译命令硬化检查通过，UPX 关闭。 |

三套新增脚本最终共 78 项，75 通过、3 跳过、0 失败/错误。21 项既有集成测试和 47 项硬化检查单列，不把模型测试、编译检查和设备验收混为一个总数。

| 实际产物 | 字节数 | SHA-256 |
| --- | ---: | --- |
| ksud | 4,236,576 | `4f36f6d4c29979460f8ab0c5345564dd0805c97c18500f337518e5a6390f3fab` |
| ksud.unstripped | 21,722,208 | `f85ca49589a9c2ba2d685824b725f7f0104d8134d38c12b44333521c6ff97b8e` |
| ksuinit | 317,616 | `14af1c4191d5d128d98e427a236a66ef963c2beefcf4c508ede8d1582cbd2c69` |
| ksuinit.unstripped | 8,949,576 | `e88e6b2701751df36d1bd770761f530ddb410c79079613303620b522b6ab8f47` |

两个目录分别记录各自目标的一次构建，不是同一目标的两次可复现性实验。ksud 与先前批次二进制摘要相同也不替代受控双构建分析。对应 `provenance/` 中保存 source-snapshot、build-manifest、native-sbom 和 licenses；实际构建绑定内容以这些文件为准。

独立复核另以 hashlib 核对产物、快照、SBOM、许可证及关键工具/源码文件，见 `out/batch44-provenance-final-inspection.json`。ksud 记录 493 份一方文件、9 个源码组件和 14 份许可证证据；ksuinit 为 165 份一方文件及 1 份根许可证证据。本次 ksud 仅暂存 installer.sh 和 shellrc.sh，没有内嵌 ksuinit 或 KMI 载荷；相关捕获路径有夹具覆盖，尚未以本批构建验证完整内嵌发布包。

失败过程保留：原生测试 a1 为 Windows 解码/默认生成器适配错误，a2 首次通过后补元数据测试形成 a3；CI 测试 a1 的 22 项中 4 项失败，修正真实缺口后 a2 通过。来源测试 a1–a4 为增加覆盖的连续通过记录，不能重复相加。全部测试和本地构建均未安装或启动 Android 产物。

定向复核入口如下，原生构建的精确命令保存在上述 manifest，新的验证必须使用新目录：

```sh
python3 scripts/ci/test_native_dependencies.py
python3 scripts/ci/test_native_provenance.py
python3 scripts/ci/test_ci_dependencies.py
python3 scripts/ci/ci_dependencies.py check
python3 scripts/ci/test_sumh_ci.py
python3 scripts/ci/test_plan_build.py
python3 scripts/ci/test_release_versions.py
```

## 4. 未完成范围与交付状态

未执行远端 Actions、DDK 镜像构建、完整 Gradle/NDK sysroot 依赖审计、签名认证、同目标双干净构建、设备/KMI 运行或性能测试。Windows 跳过的符号链接用例不能标为 Linux 通过。运行工具安装、包管理器及 runner 等执行依赖尚未全部固定，评论 Action 例外仍待解决。

后续继续完整 APK → ksud → 各 LKM 的统一发布清单、可移植验证、签名认证、DDK 最小权限和解释构建差异。S04 保持部分实施；F06/F11/F12 的 owner、事务和设备门禁不变。共享工作区既有改动保留，未提交、推送、安装或发布；本批收尾不代表全部 54 项完成。
