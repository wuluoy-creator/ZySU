# ZySU 中文文档

[项目首页](../../README.md) · [English](../README.md) · [日本語](../ja/README.md) · [Türkçe](../tr/README.md) · [Русский](../ru/README.md)

ZySU 是基于 KernelSU、从 SukiSU-Ultra 分叉的 Android root 方案，包含 ARM64 内核模块、C++17 userspace 和 Android Manager。本文档描述仓库当前实现；具体发布版的组件版本以下载附件和运行时状态为准。

## 安装与构建

| 文档 | 内容 |
| --- | --- |
| [安装指南](guide/installation.md) | 设备要求、镜像修补、直接安装、升级和恢复 |
| [GitHub Actions 与本地构建](guide/workflow-build.md) | 组件依赖、环境、签名和产物 |
| [原生依赖与构建来源](guide/sumh-native-provenance.md) | 固定来源、构建输入/产物清单、有限原生 SBOM、许可证证据与升级边界 |
| [LKM 集成](guide/how-to-integrate.md) | 内核配置、DDK、自定义模块与管理器认证 |
| [发布与版本管理](releases.md) | 本仓库发布、版本覆盖、更新元数据与迁移 |

现成构建面向 ARM64 Android GKI，内核最低为 Linux 6.1；Manager / userspace 最低为 Android 12（API 31）。默认 KMI 为 `android14-6.1`、`android15-6.6`、`android16-6.12`、`android17-6.18`，仅支持 `CONFIG_KSU=m`。这些是构建范围，不保证每个厂商设备都能启动。

## 组件与接口

| 文档 | 内容 |
| --- | --- |
| [ksud 命令行](../ksud-cli.md) | 命令树、参数、权限、模块与启动镜像操作 |
| [Ramdisk 编辑协议](../ramdisk-editor-protocol.md) | Manager 与 ksud 的二进制协议、请求和错误处理 |
| [Userspace 架构与验证](../userspace-optimization.md) | 原生组件、构建约束、主机回归入口 |
| [Manager 界面结构](guide/manager-ui-redesign.md) | 当前导航、设置、安装入口和状态呈现 |

SUMHP 已内置于 ksud。自动挂载按适用条件选择 OverlayFS 或 Magic Mount；SUMH 后端需要显式选择。外部 MetaModule 接口仍保留，后端选择、实际生效状态和恢复条件应结合诊断结果判断。

## 验证与诊断

| 文档 | 内容 |
| --- | --- |
| [SUMH 行为验证](guide/sumh-validation.md) | 内核能力、运行状态、规则与真机检查 |
| [SUMH 运行状态查询](guide/sumh-runtime-state.md) | api runtime/state/inventory、操作预登记/执行/查询、Manager 有界集合读取及未知结果边界 |
| [SUMH 模块重建记录](guide/sumh-module-rebuild.md) | 旧模块重建的计划留存、首次失败停止、同启动未知结果保护与只读查询 |
| [SUMH 配置方案与差异预览](guide/sumh-profiles.md) | CLI/Manager 未启用方案库、资源差异、固定版本导出、条件保存与未知结果核对 |
| [SUMH 能力与作用范围中心](guide/sumh-capabilities.md) | 独立只读采样、保存/内核状态分离、构建原值、后端能力、过期证据与观察者说明 |
| [SUMH 四场景只读诊断](guide/sumh-diagnostics.md) | doctor CLI、隐藏定义/模块规划/Maps/启动保护、操作候选、部分结果与只读边界 |
| [SUMH 私有诊断包](guide/sumh-diagnostic-packages.md) | CLI 与 Manager 创建、完整预览、系统文件保存、条件删除与中断后查询 |
| [SUMHP 日志保留与重复消息](guide/sumh-log-storage.md) | 当前/历史容量、尾部裁剪、重复计数、后台输出、清空与失败边界 |
| [SUMH proc 视图内存预算](guide/sumh-proc-memory.md) | 快照/流缓冲/临时工作区/Maps 所有存活代共用预算、扩容峰值、失败回收与统计范围 |
| [SUMH 目录查找接口兼容](guide/sumh-kmi-compatibility.md) | 四版本声明依据、精确符号与权限语义、缺能力处理及真实 KMI 验收边界 |
| [SUMH 按观察者采样与比较](guide/sumh-observer-kit.md) | 独立测试 APK、isolated service、root helper、精确请求绑定、原始样本和离线期望比较 |
| [SUMH 全面改进计划（2026-10-16）](../plans/SUMH-comprehensive-improvement-plan-2026-10-16.md) | 源码检查结论、54 项改进与功能任务、分层里程碑、接口草案和验收标准 |
| [SUMH 分批实施进度](../plans/SUMH-implementation-progress-2026-10-16.md) | 全部 54 项状态、各批代码改动、本地验证记录和待验收范围 |
| [SUMH 第五批验证](../plans/SUMH-implementation-validation-batch05-2026-10-16.md) | 字段刷新草稿保护、Kernel Build 条件更新及宿主/设备证据边界 |
| [SUMH 第六批验证](../plans/SUMH-implementation-validation-batch06-2026-10-16.md) | 结构化分页、路径字节保真及 Manager 保存／运行状态接入 |
| [SUMH 第七批验证](../plans/SUMH-implementation-validation-batch07-2026-10-16.md) | Maps 身份与采样期限、Manager 运行集合及原生产物加固 |
| [SUMH 第八批验证](../plans/SUMH-implementation-validation-batch08-2026-10-16.md) | Maps 独立预览、实际提交记录、pending 恢复保护与定义导入导出 |
| [SUMH 第九批验证](../plans/SUMH-implementation-validation-batch09-2026-10-16.md) | 生产 Planner 只读预览、路径解释、五类模块模板及读取错误传播 |
| [SUMH 第十批验证](../plans/SUMH-implementation-validation-batch10-2026-10-16.md) | 单模块预检、规范化预览/逐项结果、额外分区条件保存及草稿保护 |
| [SUMH 第十一批验证](../plans/SUMH-implementation-validation-batch11-2026-10-16.md) | 近期操作历史、迁移备份、独立查询/等待与未知结果边界 |
| [SUMH 第十二批验证](../plans/SUMH-implementation-validation-batch12-2026-10-16.md) | 提交前 ID、参数绑定、至多一次调用、结构化结果与输入容量 |
| [SUMH 第十三批验证](../plans/SUMH-implementation-validation-batch13-2026-10-16.md) | 隐藏稳定定义、分组启停、迁移/恢复锁、分页与逐项结果 |
| [SUMH 第十四批验证](../plans/SUMH-implementation-validation-batch14-2026-10-16.md) | Manager 稳定定义、分组启停、私有草稿、分页与有界原生通道 |
| [SUMH 第十五批验证](../plans/SUMH-implementation-validation-batch15-2026-10-16.md) | 隐藏预登记操作、执行前检查点与按 ID 资源结果恢复 |
| [SUMH 第十六批验证](../plans/SUMH-implementation-validation-batch16-2026-10-16.md) | 完整/选中隐藏定义交换、冲突预检、私有快照与每组条件提交 |
| [SUMH 第十七批验证](../plans/SUMH-implementation-validation-batch17-2026-10-16.md) | Manager 生产 Planner 预览、完整草稿绑定、路径解释与过期保护 |
| [SUMH 第十八批验证](../plans/SUMH-implementation-validation-batch18-2026-10-16.md) | 独立能力中心、有界只读采样、保存/内核状态分离、历史身份及作用范围导航 |
| [SUMH 第十九批验证](../plans/SUMH-implementation-validation-batch19-2026-10-16.md) | proc 快照/流缓冲/工作区共享计费、终止性错误清理与代理入口剩余边界 |
| [SUMH 第二十批验证](../plans/SUMH-implementation-validation-batch20-2026-10-16.md) | 活动目录长路径、准备错误传播、游标保留及通用 MERGE 剩余边界 |
| [SUMH 第二十一批验证](../plans/SUMH-implementation-validation-batch21-2026-10-16.md) | 通用 MERGE 完整收集、共享名称去重、容量与错误语义及验证边界 |
| [SUMH 第二十二批验证](../plans/SUMH-implementation-validation-batch22-2026-10-17.md) | 目录查找 typed 适配、精确依赖解析与调用方验证记录 |
| [SUMH 第二十三批验证](../plans/SUMH-implementation-validation-batch23-2026-10-17.md) | 独立观察者工具链、协议/生命周期测试、ARM64 APK/helper 构建与未执行的设备验收 |
| [SUMH 第二十五批验证](../plans/SUMH-implementation-validation-batch25-2026-10-17.md) | Maps 不可变索引、整流规则快照、所有存活代预算、宿主对比及未完成的设备性能门禁 |
| [用户隐藏定义与启停](guide/sumh-user-hide-definitions.md) | Manager 定义工作流、分组启停、完整文件交换、冲突预检、条件提交与旧格式迁移 |
| [SUMH 第二十六批验证](../plans/SUMH-implementation-validation-batch26-2026-10-17.md) | 四资源方案模型、独立存储、只读差异与本地路由；激活和设备验证仍待完成 |
| [SUMH 第二十七批验证](../plans/SUMH-implementation-validation-batch27-2026-10-17.md) | ADD/MERGE 源对象引用一致性、嵌套错误传播、宿主与 ARM 对象证据及事务剩余边界 |
| [SUMH 第二十八批验证](../plans/SUMH-implementation-validation-batch28-2026-10-17.md) | 普通源单次捕获、MERGE 锁外物化与递归预算、宿主断言工具修正及源树一致性边界 |
| [SUMH 第三十批验证](../plans/SUMH-implementation-validation-batch30-2026-10-17.md) | Manager 临时 Maps 工作台、独立能力与单次条件删除、生命周期恢复及验证边界 |
| [SUMH 第三十一批验证](../plans/SUMH-implementation-validation-batch31-2026-10-17.md) | 原生链接跟随后对象核对、错误清理、替换反例及未解决的原子配对边界 |
| [SUMH 第三十二批验证](../plans/SUMH-implementation-validation-batch32-2026-10-17.md) | 目录缓存的 inode/superblock 独立引用、失败回收、共享客户端与真实内核验证边界 |
| [SUMH 第三十三批验证](../plans/SUMH-implementation-validation-batch33-2026-10-17.md) | 注入目录路径键/精确 inode 归属、同步撤销、错误传播与集成验证进度 |
| [SUMH 目录挂接归属设计](../plans/SUMH-dirhijack-ownership-design-2026-10-17.md) | 第三十四批静态发现、全部入口归属范围及缓存兼容性后续边界；不是测试通过记录 |
| [SUMH 第三十四批验证](../plans/SUMH-implementation-validation-batch34-2026-10-18.md) | 目录挂接归属、独立源码/测试审查与主机验收；原生内核及设备门禁保留 |
| [SUMH 诊断设计](../plans/SUMH-diagnostics-design-2026-10-18.md) | 四场景来源、请求/报告、独立进程与预约合同；静态设计与运行证据分开 |
| [SUMH 第三十五批验证](../plans/SUMH-implementation-validation-batch35-2026-10-18.md) | 四场景 CLI、真实生产查询集成、传输/路由及 ARM64 构建；Linux 生命周期与设备未验收 |
| [SUMH 第二十九批验证](../plans/SUMH-implementation-validation-batch29-2026-10-17.md) | 临时 Maps 独立分页与精确键条件删除、受管/旧流保留、输出错误和验证边界 |
| [SUMH 第三十六批验证](../plans/SUMH-implementation-validation-batch36-2026-10-18.md) | 日志后台解析、按行阅读、读取状态、跟随/暂停及主机和设备验收边界 |
| [SUMH 第三十七批验证](../plans/SUMH-implementation-validation-batch37-2026-10-19.md) | 日志存储故障注入、重复聚合、标准流与启动确认描述符、ARM64 构建及剩余验收 |
| [SUMH 第三十八批验证](../plans/SUMH-implementation-validation-batch38-2026-10-19.md) | 诊断包隐私模型、存储故障、命令路由、ARM64 集成与未执行边界 |
| [SUMH 第三十九批验证](../plans/SUMH-implementation-validation-batch39-2026-10-19.md) | Manager 诊断包、精确导出、持久检查点、原生接线与验证边界 |
| [SUMH 第四十批目录缓存设计](../plans/SUMH-dirhijack-cache-design-2026-10-19.md) | 父 inode 屏障、PREPARED 缓存、投影身份、atomic_open 与未闭合边界 |
| [SUMH 第四十批验证](../plans/SUMH-implementation-validation-batch40-2026-10-19.md) | 生产代码模型、旧实现反例、故障变异、相邻回归和未执行测试 |
| [SUMH 第四十一批验证](../plans/SUMH-implementation-validation-batch41-2026-10-19.md) | 当前日志清空的文件合同、非阻塞争锁、失败传播、主机回归与设备边界 |
| [SUMH 第四十二批验证](../plans/SUMH-implementation-validation-batch42-2026-10-19.md) | 临时 Maps 的可选本地预约、回执存储迁移、至多一次执行和旧入口兼容 |
| [SUMH 第四十三批验证](../plans/SUMH-implementation-validation-batch43-2026-10-19.md) | Manager 临时 Maps 的双检查点、单次执行、独立历史和按 ID 查询恢复 |
| [SUMH 第四十四批验证](../plans/SUMH-implementation-validation-batch44-2026-10-19.md) | 原生依赖、Action/DDK 固定身份、工具链和构建输入/产物证据、独立复核及未完成范围 |
| [SUMH 第四十五批验证](../plans/SUMH-implementation-validation-batch45-2026-10-19.md) | 可移植原生证据、最终 ELF 嵌入字节、APK/下载产物关联、严格发布暂存及验证范围 |
| [SUMH 第四十六批验证](../plans/SUMH-implementation-validation-batch46-2026-10-19.md) | Manager 配置方案库、独立文件传输、写入检查点、固定导出与主机/设备证据边界 |
| [SUMH 基本闭合与实际测试交接](../plans/SUMH-basic-test-handoff-2026-10-21.md) | 当前测试基线、产物限制、逐步操作、预期结果、故障恢复和反馈清单 |
| [SUMH 场景化使用](guide/sumh-usage-scenarios.md) | 33 类现有能力和测试工具用法、适用条件、操作步骤、失败处理与撤销边界 |
| [Maps 路径规则](guide/sumh-maps-rules.md) | 路径配置、只读预览、提交历史、定义交换、启动恢复与每流规则快照 |
| [模块路径规则](guide/sumh-module-rules.md) | 模块隐藏、五类草稿模板、Manager/CLI 只读计划与路径解释、开发预检和额外分区条件保存 |
| [内核构建信息替换](guide/sumh-kernel-build.md) | Release / Version 字符串、配置、生效时机与验证 |
| [Unicode 路径防护](guide/unicode-path-guard.md) | CVE-2025-48567 原理、SUMH 防护开关、边界与验证 |
| [Magic Mount 验证](guide/magic-mount-validation.md) | 挂载树、生命周期、回归和设备检查 |
| [OverlayFS 验证](guide/overlayfs-validation.md) | 目录分层、能力限制和回归入口 |
| [环境状态与可观测性](guide/environment-observability.md) | 设置值、有效值、运行时证据与错误定位 |
| [启动配置与 Shell 来源限制](guide/boot-config.md) | 强制功能值、模块屏蔽默认关闭、配置兼容和回归验证 |
| [Manager 识别与扫描验证](guide/manager-scan-validation.md) | 内核侧管理器发现、APK 签名缓存与扫描调度 |

验证文档提供可复现步骤，不表示这些步骤已在读者设备上执行。报告问题时附上固件与内核信息、组件版本、所选后端、完整操作步骤和相关日志。

## 下载与反馈

- [发布版下载](https://github.com/wuluoy-creator/ZySU/releases)：Manager 正式更新也读取此仓库。
- [源码与工作流](https://github.com/wuluoy-creator/ZySU)：Actions 的最终 APK 产物为 `Manager-arm64-v8a`。
- [问题反馈](https://github.com/wuluoy-creator/ZySU/issues)与[安全问题说明](../../SECURITY.md)。

源码、工作流和正式发布统一在公开的 `wuluoy-creator/ZySU` 仓库。旧 APK 若仍检查 `ZySU-Releases`，需要手动安装一次包含新更新地址的 APK，详见[迁移说明](releases.md#从独立发版仓库迁移)。

许可证、项目来源和完整目录介绍见[根目录 README](../../README.md)。
