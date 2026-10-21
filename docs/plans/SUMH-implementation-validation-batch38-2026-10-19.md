# SUMH 第三十八批实施与验证记录

日期：2026-10-19（Asia/Shanghai）。对应 [54 项计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)与[进度账本](SUMH-implementation-progress-2026-10-16.md)。本批推进 **S03/M04 单场景私有诊断包 CLI**，使用方法见[诊断包指南](../zh/guide/sumh-diagnostic-packages.md)和[场景 30](../zh/guide/sumh-usage-scenarios.md#30-留存预览导出和删除一份诊断包当前源码已实现)。S03、M04 和整体目标均未全部完成。

## 1. 实际实现

| 文件 | 行为与边界 |
| --- | --- |
| `userspace/ksud/sumhp/src/core/diagnostic_package_model.cpp`、`.hpp` | 新 schema 1 `sumh.diagnostic.package`。按上下文字段白名单重新构造，默认将 boot、namespace、定义、模块、操作、重建/提交、revision、rule/generation 转为按类型的包内别名；相同身份保持包内关联。显式 `--include-identifiers` 只保留校验后的允许身份。两种模式均省略路径、备注、日志、凭据、环境和命令参数/说明；绝对单调时间转为相对时间或显式省略。 |
| `core/diagnostic_package_model.cpp` | 留存包按完整闭合 schema 重建并比较，拒绝未来版本、未知字段、隐私标记变更、默认模式原身份和非规范别名。无原报告 fallback。保留 partial/stale/unavailable、非原子与非观察者验证标记。 |
| `core/diagnostic_package_store.cpp` | 最多 8 个不可覆盖 ID；精确导出内容每份 96 KiB（含 LF），bank/stage 各最多 1 MiB，固定 stage，无自动淘汰。查询不 mkdir/chmod/加锁/迁移/清理。所有包先完整校验，未来或损坏格式保留。 |
| 同上 | 已知目录链、root 所有者、0700 私有目录、0600 普通单链接文件、读取前后身份/大小/时间检查；修改使用独立非阻塞合作锁、精确原字节复核和固定暂存。rename 后同步或读回失败报告 unknown，不重试或回滚。条件删除只影响选中 ID。 |
| `core/diagnostic_package.cpp` | 精确 create/list/preview/export/remove 入口，init mount namespace 前后核对及恢复。重复 ID 在重新采样前拒绝；create 复用原诊断通道，preview/export/remove 不重新采样。预览包含完整 export_bytes、字节数和 revision；导出逐字节输出同一内容。输出失败保留包，异常只返回固定原因。 |
| `core/diagnostics.cpp`、`.hpp` | 提取 `query_diagnostics` 复用严格输入、self-exec 传输和报告校验。原 `doctor --input` 保留原报告及诊断退出码，不因此成为脱敏入口。 |
| `src/embedded.cpp`、`core/command.cpp`、`userspace/ksud/src/cli_args.cpp`、`sumhp/CMakeLists.txt` | root 精确本地路由、帮助/发现、api meta 和构建接线；通用参数/daemon 包装拒绝 doctor 子命令。采样只读与本地存储修改分开声明，Manager 能力保持 false。 |
| 三个 `diagnostic_package_*_test.cpp`、对应 runner、既有 CLI fixture、`scripts/ci/sumh_ci.py` | 生产模型、存储与控制器故障检查，实际 main/CLI/embedded 路由回归，CI 登记三个新 runner。 |

`package_revision` 为 `state::revision_for_bytes` 对规范化包 JSON 加末尾 LF 的 SHA256；没有附加域前缀。store fixture 的 digest 是测试替身，只验证传入字节和条件比较；生产 mbedtls 实现进入 ARM64 链接，未把替身测试称为密码学验收。

包 ID 由调用方预先保存，格式为 32 位小写十六进制。创建回包丢失先按 ID 预览；重复 create 不覆盖或重新采样。读回和耐久性确认分别报告；unknown 时 saved/deleted 为 false 表示未确认，不能据此断言文件未改变。无独立持久操作日志。删除仅影响本地留存，不撤销已导出的副本。

## 2. 静态检查与复核

本批先保存 498 份 before-image，位于 `out/batch38-before`，清单为 `out/batch38-before-manifest.json`。未覆盖 Batch34–37 的既有检查器与证据。

范围审查记录在 `out/batch38-scope-review.md`：采用独立采样和受限投影，未调用 Manager 的 `LogEvent.kt` 全量 bugreport。该报告仍包含广泛系统数据，原 doctor 仍带真实身份，不能泛称所有诊断已脱敏。包内别名保留关系和状态特征，不保证匿名。

`out/batch38-final-review.md` 对存储、控制器、doctor 重构和路由进行了跨作者复核，未发现具体阻断缺陷；该审查者自己编写了隐私模型，其模型复核明确是自审。隐私模型另由存储作者检查，`out/batch38-model-independent-review.md` 同样未发现具体阻断问题，不把任何一次审查称为完整安全审计。独立模型复核建议后续增加真实 `diagnose` partial/stale 产物直接输入用例：当前这些状态的动态检查是修改 observed fixture 的状态/退出码，真实分支兼容性由源码复核确认，不能冒充已执行的全分支集成测试。

根 agent 复核了源报告字段/状态、模型投影、存储的读取和提交路径、指南命令与真实行为。已知父目录没有在整个操作期间全部固定 dirfd；不承诺抵抗任意并发特权写入。标准流失败、namespace 恢复与存储模型的检查不等于实际 Linux/Android 执行。

## 3. 本批实际执行

统一运行器为 `out/batch38-run.py`；所有尝试使用独立目录并保存命令、日志、输入前后摘要。以下命令通过已配置 Python 执行，宿主编译器为 Zig 0.14.1 C++：

```text
python -B out/batch38-run.py preliminary a1
python -B out/batch38-run.py host a1
python -B out/batch38-run.py host a2
python -B out/batch38-run.py build a1
python -B out/batch38-artifact-inspect.py
```

| 实际执行 | 结果与证据范围 |
| --- | --- |
| preliminary-a1 | 原 doctor 采样/输入/隐藏集成通过；新控制器 fixture 使用了错误 `sumhp::json::parse` 名称导致编译失败。修正为 `json::parse` 后进入新尝试；保留失败日志。不是产品行为测试失败，也不将此尝试标为通过。 |
| host-a1 | 模型、存储、控制器、原 collector 和 CI 单元全部通过，输入摘要未变。 |
| host-a2 | 加入实际 main/CLI/embedded 路由、通用 parser 和日志静默回归后全部通过，输入摘要未变。最终主机证据以此为准。 |
| 包隐私模型 | 6 组、354 项始终启用检查；四种生产报告 fixture、真实 unavailable 输出、包内别名/同值关联、嵌套 canary、命令字段省略、身份保留、时钟/未来格式/重复键/UTF-8/容量及严格留存解析通过。fixture 生产者本身 471 项检查通过，两处 runner 各生成一次，不累加成不同测试数。 |
| 包存储 | 5 组、9,795 项 portable 检查；链接实际隐私模型，替换 POSIX 调用/类型/常量及摘要函数。覆盖精确导出、不可覆盖 ID、8 项容量、1 MiB 输入界限、不安全类型/权限/链接、未来格式保护、固定暂存、短读写/EINTR/零写入、加锁/同步/关闭/rename/读回故障、条件删除与其他记录保留。 |
| 包控制器 | 309 项检查，执行生产控制器；namespace、存储、query 和 model 为替身。覆盖精确参数、已存在 ID 不采样、partial/unavailable 留存、投影失败、精确导出、过期 revision、查询/删除不采样、namespace 进入/恢复/变化、保存后异常与输出损坏。 |
| 原 doctor | 165 项采样/CLI orchestration、57 项输入 IO、99 项真实隐藏/collector/model/operation query 集成检查通过。覆盖本批 query helper 重构。 |
| 实际 CLI 路由 | 81 个诊断/诊断包 main/CLI/embedded 用例与既有 Maps/module/profile/operation 等相邻回归通过。命令行为和 OS/root 为 fixture，真实 parser、入口函数和元数据生成参与；额外 parser 与生产日志静默检查通过。保留既有 logger 的非 Android `android_level` 未使用警告。 |
| CI runner 单元 | 7 项通过；其中刻意缺失脚本的失败输出是负例，整体 unittest 为 OK。新 runner 已登记，但未执行远端 Actions。 |
| ARM64 Release | NDK 30.0.16248370、API 31、CMake 3.22.1，使用本地既有依赖，830 步编译链接成功，输入摘要未变。构建脚本检查 834 条 compile_commands；最终 AArch64 动态产物含 RELRO+NOW，新模型/存储/控制器编译单元和符号已核对。 |

ARM64 产物：`out/batch38-build-a1/ksud`，4,206,352 字节，SHA256 `a436754ef926e85acb300f454ef7c302cd0481aa1633fb94bd954884fe32a930`；Build ID `ae17b0ba52698db66b42293c46bcc81b26c97e1f`。未安装、启动或发布。未剥离符号、编译数据库和 CMakeCache 同目录留存；`out/batch38-artifact-inspection` 记录 ELF/符号核对。

最终 `out/batch38-final-manifest.json` 关联本批文件、before-image、各尝试日志/产物、最终已测输入和文档链接，确认两份表均保留原 54 项 ID。其 passed 只针对该限定交付核对，不代表 S03/M04 或设备验收完成。

## 4. 未执行与后续

- 未执行实际 Linux flock/namespace/fork/exec/pipe 生命周期、并发写入、文件系统故障、崩溃或断电测试；没有 Android SELinux、真实采样、观察者视图、磁盘/IO/能耗和性能结论。
- 本批未修改 Manager/JNI；诊断包预览/分享/删除 UI、进程重建和可访问性验收待实施。既有 JNI 日志清空缺单链接检查的差异未修正。
- M04 的完整组件身份、原始 KMI、实际 owner、全部预算及状态覆盖未完成。包成功留存不代表证据完整或功能健康。
- 原 doctor 和 Manager 全量 bugreport 隐私保持原行为；全量报告的权限/生命周期、备份期限和删除政策继续待办。包库无自动淘汰；固定 stage 不等于总磁盘只占 1 MiB，旧打开 inode 和文件系统元数据也不在路径上限内。
- 整体 54 项继续推进。F06/F11/F12 的 owner、事务和真实观察者门禁保留，未新增热应用或全局 clear 工作流。工作区未提交、推送或发布。
