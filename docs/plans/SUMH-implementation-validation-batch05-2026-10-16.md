# SUMH 第五批实施与验证记录

日期：2026-10-16（Asia/Shanghai）。对应[54 项主计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)与[实施账本](SUMH-implementation-progress-2026-10-16.md)。本记录覆盖 U02 的 Manager 草稿保护、R05/K07 的 Kernel Build 条件更新，以及 C02/C03/C04 的保存摘要、运行核对和最后操作记录增量。上述任务均保留未完成范围，设备与发布验收未闭合。

## Manager：外部刷新与保存读回保留输入

[SUMHRefreshAwareDraft](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHRefreshAwareDraft.kt) 保存每个字段的草稿、已保存基线摘要和待确认提交摘要。Kernel Build 的 Release、Version、生效阶段，以及挂载来源、临时目录、镜像路径采用同一机制：

- 未编辑的字段自动接受新的已保存值；未完成的字段遇到外部变化时保留完整输入，显示最新保存值，并要求明确选择使用该值或保留草稿。
- 每个字段独立处理；选择丢弃一个字段不改变其他字段。原值与系统日期按钮仍只填对应草稿，运行时刷新不会替换草稿。
- 观察到先前提交的保存值时推进基线，保留提交之后的新输入。回调返回、失败、读取失败及未确认结果均不直接当作保存确认。
- 路径设置在校验失败或提交之后保持编辑器打开；编辑状态也参与 SavedState 恢复。Kernel Build 首次启用得到保存确认后，保留仍在编辑的内容。

现有 Workspace 只在 `Controller.load()` 返回后发布配置，没有先把请求值作为已保存值显示。本批据此确认保存基线，**不据字符串相同推断内核运行时应用成功**，也未增加虚构的 operation ID、配置 revision 或跨 Activity 提交事务。

受管 Maps 同目标更新的读回只关闭仍与提交完全相同的草稿；已有更新输入时推进保存基线并保持编辑。后续输入与一次添加或重命名交错时，当前格式仍缺少稳定定义身份：输入会保留，可能需要处理重复目标或原目标已不存在的情况。这部分继续留待稳定身份工作流，不能宣称 Maps 全部并发编辑场景已完成。

小字段草稿最多 4095 个 UTF-8 字节，SavedState 另存固定长度摘要；既有超长 mountsource 仍可显示，但从保存配置重新读取，不把完整旧值放进 Bundle。模块/Maps 私有草稿格式和用户隐藏存储保持第四批行为，本批没有格式迁移。

## Kernel Build：条件设置与可验证回滚

[UAPI](../../uapi/sumh.h) 新增独立能力 `SUMH_FEATURE_KERNEL_BUILD_CAS`（bit 19）和 `SET_KERNEL_BUILD_CAS`（slot 51）。原协议版本仍为 1；新结构为 168 字节、8 字节对齐，字段使用固定宽度。接口支持与当前 UTS 机制可用性分别协商；不能凭协议版本或 CAS 能力位推断启用一定成功。旧 slot 45 保持兼容。

内核在最终写入口验证结构、字符串及 instance/generation。坏输入与过期请求不修改设置、错误或序列；合法且匹配的请求记录一次目标结果并推进序列。旧写入口也参与同一 SUMH 控制序列。

[客户端](../../userspace/ksud/sumhp/src/mount/sumh_client.cpp) 在运行状态查询前后夹住当前值及必要的原值读取，要求实例、序列和 configured 一致。[公共应用函数](../../userspace/ksud/sumhp/src/mount/kernel_build_apply.hpp) 在此快照中解析单字段的原值，执行条件设置，再核对同实例、准确的下一代序列及完整 Release/Version 对。

只有写入已确认、读回仍属于该次写入且内容不匹配时，才按准确 token 条件恢复之前的两字段状态，并再次读回验证。传输结果不明、实例变化、序列冲突或读回不一致时，不自动重放或无条件回滚。旧内核保留显式设置和两字段读回，取消不具备所有权证明的无条件回滚。

该序列覆盖 SUMH ioctl 写入和清理流程；其他内核组件直接改写 `init_uts_ns` 不受它约束。它不改变字符串功能的作用范围，也不构成按观察者的设备实测。

## CLI 接线：保存结果与运行结果分开

[显式 `kernel-build set/reset`](../../userspace/ksud/sumhp/src/core/command.cpp) 已接入同一公共应用函数，主任务最终原生回归及 ARM64 Release 编译链接通过。新增命令级测试验证条件接口实际被使用、回包丢失后只执行一次，以及另一写者更改保存字段时不被旧配置回滚覆盖。

`set` 先保存期望字段，再应用。应用失败、未知或运行值已回滚，不会盲目把旧字段写回配置；新保存意图仍可能在后续恢复时应用。`reset` 独立尝试恢复运行值和保存停用意图，配置解析失败不会直接阻止运行恢复尝试；任一部分无法确认时分别报告。整体格式损坏或未来格式仍受配置写入保护约束。

到达应用阶段的 JSON 保留 `ok`、`enabled`，增加 `saved`、`applied`、`runtime_result`、`detail`。其中 `enabled` 是本次请求方向，不能代替运行查询。能力不可用时，reset 可报告 `deferred`、`saved:true`、`applied:false`；这表示已保存停用意图，没有确认本次实际恢复字符串。早期能力、输入或配置错误可能只有 stderr 和非零退出码，尚不是 F09 的全入口统一结果协议。

## C02/C03/C04：保存摘要与最后操作记录

新增 [state_ledger.cpp](../../userspace/ksud/sumhp/src/core/state_ledger.cpp)、[纯模型与格式校验](../../userspace/ksud/sumhp/src/core/state_ledger_model.cpp) 和已实现的 `ksud sumhp api state`。主配置的分功能意图、用户隐藏定义、模块保存文件生成 SHA-256 摘要；使用已经链接的 mbedTLS，没有另写摘要算法。分别读取持久定义与内核事实，前后重读定义、boot ID、instance/generation；发生变化时返回未知。

applied revision 必须同时满足当前可核对状态匹配、同一保存摘要及同启动/内核实例的历史验证。不能仅凭历史 applied 字段推断运行状态；换启动、内核重新初始化、配置修改和无法查询时均不沿用。五项开关区分 configured/active；Kernel Build 停用时也核对原始两字段。模块仅摘要保存定义字节，尚未包含源树/最终映射；受管 Maps 身份与 overlay xattr 缺完整查询时保持未知。

用户隐藏登记集合与绑定健康分开说明：`verification_scope=registered_definitions`、`binding_consistency=per_rule_sampled`。绑定事件不推进控制 generation，因此逐条 BOUND 不代表所有项曾在同一时刻 BOUND；全局暂停也单独说明。此边界来自独立审查，已补元数据、原因、测试及指南，不能将其包装成普通应用可见性测试。

受记录的修改命令持有非阻塞独立锁，先耐久写 started 再执行；结束记录保留退出码、errno 和可核对功能状态。文件上限 32 KiB、固定功能 ID，未知格式或损坏记录拒绝覆盖；记录只含命令类型和摘要，不含路径/载荷/凭据。开始记录失败不执行命令；结束写入失败返回非零并要求先查询，绝不自动重试或回滚。开始阶段异常也不会误写结束记录，日志 IO 残留 errno 不计为命令自身错误。

每个保存文件采样有 4 MiB 读取预算，超限仅报告未知，原文件不变。记录缺失可查询；历史损坏时仍输出本次状态但返回非零。受记录命令遇到未来格式会停止，不能为恢复操作而静默清空历史。R03 首次祖先目录耐久性限制仍适用。

目前只保存最后命令，未形成历史列表或 `operation show/wait`。finished 非零是保守的 `failed_or_partial`；即时命令的 typed unknown 尚未完整写入操作历史。直接自动恢复也未逐一纳入；完整 CLI 仍有 daemon 冷启动及旧能力查询副作用。因此 C02/C03/C04 均只记部分实施。完整字段和使用边界见[运行状态指南](../zh/guide/sumh-runtime-state.md)。

## 已执行检查与剩余验证

| 检查 | 证据与范围 |
| --- | --- |
| Manager 全 SUMH 包 | `:app:testDebugUnitTest --tests 'com.zying.zysu.ui.sumh.*'` 最终通过：27 类、160 项、0 失败、0 错误、0 跳过，session 56765，耗时 1m 10s；生产 Kotlin 和资源编译随该任务完成 |
| Manager 报告 | 当前 XML 位于 `manager/app/build/test-results/testDebugUnitTest`，该次完整报告另存本机 `out/sumh-batch05-manager-full`；XML 汇总独立核对为 160 项 |
| 刷新与编辑回归 | [9 项生产 Compose 用例](../../manager/app/src/test/java/com/zying/zysu/ui/sumh/SUMHDraftRefreshTest.kt) 覆盖干净/单字段/双字段刷新、字段独立决策、旧提交确认与新输入、未知回调、StateRestorationTester、路径校验、首次启用及大字体横屏；[Maps 用例](../../manager/app/src/test/java/com/zying/zysu/ui/sumh/SUMHManagedMapsControlsTest.kt) 补同目标读回保留更新输入 |
| 原有能力回归 | 原值/系统日期只填草稿、运行查询失败、UTF-8 限制，以及 300k 既有 mountsource 不进入 Activity SavedState 的用例均随完整包通过 |
| 界面检查 | 查看 `draft-conflict-landscape-large-text.png`：中文横屏、2 倍字体下最新保存值及两项决策可读且可达；这不是 TalkBack 或真实旋转测试 |
| 原生测试（Kernel Build 实施者） | session 27686 返回 0，15 个 SUMH 主机程序通过，含新 `sumh_kernel_build_apply_test`；Windows symlink fixture 不可用，不计为通过。该结果先于主任务最后的显式 CLI 接线复验 |
| 内核生产函数与 ABI（Kernel Build 实施者） | Kernel Build 提取测试 58 项通过；runtime-state 实际包装器覆盖 slot 51，ARM32/ARM64 布局编译及 readiness 回归通过；并非完整内核/KMI 构建或 compat 设备执行 |
| 最终原生回归（主任务） | `run_sumh_host_tests.py --zig <Zig 0.14.1>`，session 64159，真实子进程返回 0：15 个程序通过。包含最终 set/reset 的 CAS、未知结果、外部保存修改保护和 `api state` CLI 参数校验；Windows symlink fixture 不可用，未计通过 |
| Ledger 专项（实施及独立审查） | `run_sumh_state_host_tests.py --zig <Zig 0.14.1>` 通过；生产采样、codec、journal 与命令 capture 提取，覆盖 start/finish 失败、已替换但不确定、崩溃残留、锁冲突、未来格式、配置/运行变化、boot/instance 不匹配、读取上限、登记/绑定/暂停范围及无重复执行。SHA 库函数、IO、内核及配置解析采用替身；不把本测试说成真实密码库、文件系统或设备验证 |
| 最终 Android ARM64 Release ksud | `cmake --build out/sumh-maps-native --parallel 4`，session 54095，编译链接返回 0；包括最终 C02 与 CLI 接线 |
| CI gate | `python -B scripts/ci/test_sumh_ci.py`：7 项通过；新 ledger runner 已加入清单，现共 27 个原生入口。未在本机完整运行这 27 个入口或远端 Actions |
| 独立复核及文档 | 内核实施者独立审查 ledger 并执行专项测试，未发现当前增量的阻断问题；其隐藏绑定采样建议已落实。主任务复核 Manager 状态及截图。54 个唯一任务 ID 一致，14 份文档的 173 个本地链接有效；`git diff --check` 通过，仅有换行转换提示 |

本地工具链沿用前批：JDK 21.0.11、Android SDK/NDK 30、Zig 0.14.1。Manager 完整测试前的两次定向回归分别用于草稿模型和 Maps/卡片接线，均通过；最终完整包报告作为本批 Manager 汇总证据。

尚未执行真实设备进程终止、旋转、TalkBack、权限/namespace/观察者矩阵、真实内核并发压力、四 KMI 完整构建、掉电、性能基线和远端 Actions。宿主替身、ABI 编译及 SavedState 模拟不替代这些验收。C02/C04 全链路结果、配置 revision 与跨 Activity 保存协调继续保留在计划中。
