# SUMH 第九批实施与验证记录

日期：2026-10-16（Asia/Shanghai）。本批推进 F02/F03/R06，继续[54 项主计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)，进度见[实施账本](SUMH-implementation-progress-2026-10-16.md)。完整任务范围保持不变；原静态审查和历史批次证据不改写。

## 实现范围

精确 `ksud sumhp module plan [--input <绝对文件或->]` 与 `module explain <绝对路径> [--input <绝对文件或->]` 在最外层 CLI 的普通日志、通用参数解析和 daemon 恢复之前分派，仍检查 root；保留 su/sh/多调用程序行为。错误参数在本地拒绝，包装和嵌套调用不能转到可恢复状态的替代路径。普通 daemon handler 拒绝此预览，query 分类不记录变更操作；不承诺手动启动 daemon 本身无恢复效果。

候选 schema 1 / `sumh.module.plan.input` 可修改限定全局选项及指定模块的完整 mode/rules，但仅作用于副本。总输入 64 KiB、最多 256 个模块替换；单模块规则仍为 256 条 / 双层 JSON 56 KiB。严格拒绝未知字段/版本、重复键/模块、无效 UTF-8 和路径，不执行 shell。原保存格式和旧命令保持兼容。保存文档通过实际加载器共用的纯解析函数读取，不调用创建锁文件的持久配置加载器。

后端选择与生产应用共用结构化决定，区分未探测、不支持和读取失败；Planner 诊断与真实 prepare 共用编译器，保留反向模块发出顺序、最长前缀、none 嵌套例外、hide 祖先、别名及原有路径后端合并语义。输出保存/候选配置、有序 ADD/MERGE/HIDE 贡献和差异、实际匹配来源、直接映射被覆盖关系、MERGE 回退及源写透提示。路径解释筛选真实事件，不实现另一套规则匹配器，不推断跨后端或重叠 MERGE 的最终赢家。

范围为**当前可挂载模块的假设编译**，保留禁用/移除/skip_mount/热暂停过滤；不等于完整下次启动集合或当前运行规则。响应显式列出 runtime reconciliation、观察者视图、完整 owner、源生命周期及应用事务等未检查项。`complete` 只指声明范围内 SUMH 编译完整，`comparison_valid` 控制差异有效性；缺模块、失败/超限/未知不能当成成功空集合。保守要求重启，`apply_token` 为空，`hot_apply_authorized` 为 false。

预览使用既有 root 私有运行目录的非阻塞 flock，不创建目录、锁文件或配置；查询子进程继承锁并验证目录身份及 init 挂载命名空间。既有 query worker 提供 5 秒等待预算、pidfd 确认及单 pending 槽位；只有确认子进程成功才接受匿名共享内存响应。配置/模块文档单份最多 4 MiB，候选及 boot marker 64 KiB，模块发现最多 512 个目录项；诊断为 2048 事件、4096 元数据、共享 512 KiB 及 4096 动作，超限立即停止，独立响应最多 2 MiB。普通 IPC 未扩容。

前后读取原始配置、候选文件、模块发现、boot/运行 token 和启动标记；baseline/candidate 分别两次编译比较元数据与计划，并重复解析 explain 别名。变化使比较 stale，清除有效差异；摘要失败不能成为 complete。元数据涵盖编译实际访问的源，包括 MERGE 后代；重复相等不是固定 FD、内容哈希、原子快照或 ABA 保证。外部 owner 明确为未观察，预览不调用旧的无界发现函数，也不授权应用。

R06 补 checked module discovery、后端元数据及 Planner 的遍历/链接读取错误传播，失败保留已有结果；实际 mount/refresh 在发现失败时停止，不把空或残缺集合替换进去。现有 conflict 检查也报告发现失败，但深层冲突扫描和其他后端的全部错误处理仍未完成。

Manager 增加全部使用、排除、仅使用、排除父目录保留子项、隐藏五类模板。先审查相对当前草稿的完整差异，再明确替换草稿，原保存按钮独立；输入保存在有界私有草稿。未完成单项编辑、保存冲突或观察到草稿/全局选择改变时要求重新审查；失败和刷新不覆盖输入。中文/英文资源已补。此界面不调用模块预览，也未实现任意模块开发预检或 normalize dry-run。

## 审查发现与修正

1. 候选 JSON 原使用兼容解析器，重复字段会被后一个值静默覆盖。共享 Parser 增加默认关闭的严格选项，仅新候选开启；每层按解码后的键拒绝重复，旧保存格式保留原语义。独立复现与修复复核见 `out/batch09-manager-status.md`。
2. Planner 匹配系统符号链接的分支仍写普通日志。诊断模式现在抑制该条日志，生产 apply/prepare 原日志保留；回归断言诊断不调用普通 logger。
3. explain 最初把所有 POLICY 事件作为全局策略，混入无关路径的后端转换。现按事件实际路径筛选，后端决定独立呈现。
4. 旧 external-owner helper 可遍历全部模块并无界读取 module.prop，绕过预览预算。预览已移除该调用，owner 明确 unknown/not_observed 并列入 unchecked；以后需有界共享查询协议才能补齐，不能用空字符串冒充没有冲突。生产 owner 行为不变，见 `out/batch09-controller-independent-review.md`。
5. 共享 embedded 路由扩展使旧 Maps-only 测试缺少新入口声明；该 fixture 增加明确禁止使用的无关入口替身，实际 full-chain 测试继续编译真实模块参数代码，旧 Maps 回归重新通过。

## 已执行验证

以下是本批最终代码的本地证据，不能替代设备或发布验收。

| 项目 | 已获得证据 |
| --- | --- |
| Planner | 45 个生产诊断/错误场景及原有 Planner/whiteout 回归通过；诊断日志修正后复跑。真实临时源树和边界故障替身覆盖顺序、别名、none/hide、MERGE 回退、元数据变化、IO 故障、容量及旧计划保留。`out/batch09-planner-host.log`。 |
| 模块候选/控制器 | 最终 71 项输入/参数 + 69 项控制器场景通过，并经另一 reviewer 独立复跑。编译实际候选实现和完整控制器提取，配置及规则解析来自生产代码；IO/namespace/摘要/后端/Planner 边界使用确定性替身。涵盖输入/预算/无损路径、缺失/失败/变化、比较失效、解释筛选、零 owner-helper 调用、guard、namespace、子进程失败后丢弃响应和容量。`out/batch09-module-preview-host.log`、`out/batch09-independent-controller.log`。 |
| 后端/发现/保存解析 | 4 个生产程序通过，checked discovery 在 Windows 上执行 39 项；选择来源/未知能力、canonical/legacy 纯解析等价、读取失败保留运行/重放集合。`out/batch09-backend-host.log`。Windows symlink fixture 不可用；Linux 编译证据不算执行。 |
| CLI 实际调用链 | 29 个 Maps + 43 个模块分派场景、parser/daemon 拒绝合同、原 cli_args_test 和生产 logger OFF/恢复检查通过。`out/batch09-preview-cli-tests.log`。sampler 在此 suite 为替身。 |
| 原共享及 Maps 回归 | 17 个共享程序通过，`out/batch09-root-regression.log` 的首个 runner exit 0；其后 Maps fixture 编译失败已修复，独立复跑 44 观察 + 16 路由检查通过，`out/batch09-existing-maps-tests.log`。 |
| Manager JVM/Compose | 36 类 / 264 项，零失败、错误、跳过，`out/batch09-manager-full-gradle.log`；XML 与 30 张布局截图归档 `out/sumh-batch09-manager-full/`。两张新中文 2 倍字体横竖屏截图已经查看，滚动后错误、差异和按钮可达。 |
| Manager 原生构建 | 共享 JSON 变化后 ARM64 Release 编译/链接及必需 clang-tidy 通过，`out/batch09-manager-native.log`；未创建或安装完整 APK。 |
| ksud 最终生产构建 | 最终路由、能力元数据、严格 JSON、无日志诊断和 owner 修正后 ARM64 Release 编译/链接与构建后加固检查通过，session 84263，`out/batch09-ksud-build.log`。808 条编译记录；实际产物/编译参数加固回归 24 项通过，`out/batch09-final-hardening.log`。 |
| CI/文档/差异 | 7 项门禁测试通过，35 个 native 入口包含新 backend/module-preview runner；仅本地测试，远端 Actions 未执行。54 个唯一任务 ID 与账本一致、18 份文档 227 个本地链接有效；普通 `git diff --check` exit 0，仅 LF/CRLF 提示。证据为 `out/batch09-final-{ci,docs,diff}.log`。 |

测试使用 Zig 0.14.1、JDK 21、Robolectric；Android 用户态使用 NDK 30.0.16248370 / Clang 21、API 31、CMake 3.22.1。Manager 264 项后未更改 Kotlin。真实命名空间、内核 ioctl 和观察者视图不是这些宿主测试的证据。

最终 ksud 为 3,367,224 字节，SHA256 `2c0a9fea49b371a7238559952100c02ccb847d9eebf88fded9b65c9f210956e2`；独立符号为 17,071,664 字节，两者 build ID 均为 `289cbfae308c67db963270128c7dc55b5e954e04`。Manager `libkernelsu.so` 为 5,318,960 字节，SHA256 `c0f10f0a82ac569936c4e405567e28506966f3781b31139a0b0d2ad8d1cd8148`。真实路径、符号摘要和 build ID 记录于 `out/batch09-final-artifacts.json`；Manager 归档与实际构建输出一致。ksuinit/内核本批未改动，不复用旧证据声称本批重新构建。

复跑入口：

```sh
python3 userspace/ksud/tests/run_sumh_planner_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_sumh_backend_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_module_preview_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_maps_preview_cli_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_maps_preview_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_sumh_host_tests.py --cxx g++
python3 -B -m unittest discover -s scripts/ci -p 'test_sumh_ci.py'
cd manager
./gradlew :app:testDebugUnitTest --tests 'com.zying.zysu.ui.sumh.*' --no-daemon -PABI=arm64-v8a
```

## 撤销与剩余范围

CLI 预览没有持久或运行变更，丢弃候选即可。Manager 取消/关闭仍保留模板输入和主编辑草稿；当前“放弃草稿”仅针对单项编辑，不是整份模块草稿重置。替换前应记录完整 mode/rules，保存前撤销需在编辑器中手动恢复该定义；已保存则恢复原完整定义并按现有流程重启。没有新增内核 ABI、应用事务 token 或 profile 格式。checked discovery/Planner 的失败传播是实际挂载路径修正，不能通过全局 clear 绕过。

F02 仍需 Manager 接入、完整运行映射和有界 owner 核对；F03 仍需任意目录 inspect、normalize 变更预览、额外分区发现/选择/保存闭环；R06 仍需其余后端和标签错误处理。`module apply/inspect`、normalize dry-run 等未实现入口继续标草案。使用方法见[模块规则指南](../zh/guide/sumh-module-rules.md)及[场景指南](../zh/guide/sumh-usage-scenarios.md)。

未执行真实 Linux/POSIX smoke、Android open/flock/setns/pidfd/mmap 生命周期、SELinux、并发源替换/旧 FD、四 KMI 内核、启动恢复/掉电、普通/授权/root/isolated/共享 UID/工作资料观察矩阵、TalkBack/真实进程终止、性能/内存基线或远端 Actions。没有安装、提交、推送、发布或设备变更。F02/F03/R06 仍为部分实施，整体 54 项目标继续进行。
