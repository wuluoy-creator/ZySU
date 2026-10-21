# SUMH 第十二批实施与验证记录

日期：2026-10-16。对应[54 项实施账本](SUMH-implementation-progress-2026-10-16.md)和[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)。本批推进 C04/F09 的提交前操作身份与脚本结果，补 C05 的新入口容量限制；这些任务仍为部分实施，整体目标继续进行。旧批次证据保持原范围。

## 实现范围

- 新增精确本地 `operation prepare --input <绝对文件路径|->`，校验 schema 1 JSON 的 args，生成 ID 并耐久保存 prepared 预约。不调用目标、不采样运行状态、不启动 daemon；只在确认保存与读回后返回可用 ID。
- 新增 `operation execute <id> --input ...`，只向已有 daemon 发一次请求，不自动启动/重试；服务端重新校验生产 CLI 语法，要求同启动、同名称/参数摘要且仍保留的 prepared 记录，先确认 started 再调用原命令链。started/finished、旧启动、不同输入、旧无绑定记录、缺失或已淘汰 ID 均不新执行/重建。
- 磁盘 schema 3 增加 request_revision/prepared，仍保留最多 16 条、32 KiB。schema 1/2 查询不迁移；首次显式修改前备份原始字节到版本对应文件，不覆盖不一致备份。执行历史中的预约先按 ID 提升至最近位置，保持其他记录唯一和有序。
- 查询响应升级 schema 2：prepared 为 not_started，prepared/started 没有虚构的退出码。`api state` 仍为 schema 1，投影最近的 started/finished，全部为预约时 last_operation=null；Manager 接受这种有效空历史，不把 prepared 显示为运行/完成。
- 输入限制为 48 KiB UTF-8、2–32 个参数、二次编码后的请求最多 64 KiB；严格拒绝重复/未知键、未来格式、NUL 和不支持语法。文件为单硬链接普通文件，末级不跟随符号链接；stdin 为普通文件/pipe/socket，有 1 秒总轮询预算和标志恢复检查。参数不交给 shell，摘要绑定原参数字节及版本域。
- 提交 schema 1 输出 ID、请求摘要、是否本次执行、最终记录确认、命令退出码/errno、当前观察记录及独立 stdout/stderr。新执行捕获各 64 KiB，超限明确 complete=false，非 UTF-8 用 hex；旧命令保持输出行为。客户端验证完整字段、状态不变量、身份与容量；不合格回包归为 transport_unknown。
- 预登记不固定源文件、配置 revision、Planner plan ID 或 namespace。有限账本、同 boot 和协作锁下保证同预约至多调用一次，不宣称所有资源恰好一次、全局原子事务或 root 回滚账本后的防重放。操作结果与各资源的实际生效验证分开。

源码：[输入校验](../../userspace/ksud/sumhp/src/core/operation_submit_input.cpp)、[文件与 stdin](../../userspace/ksud/sumhp/src/core/operation_submit_io.cpp)、[提交与结果](../../userspace/ksud/sumhp/src/core/operation_submit.cpp)、[有界捕获](../../userspace/ksud/sumhp/src/core/capture_buffer.hpp)、[持久状态机](../../userspace/ksud/sumhp/src/core/state_ledger.cpp)、[记录格式](../../userspace/ksud/sumhp/src/core/state_ledger_model.cpp)、[daemon](../../userspace/ksud/sumhp/src/core/daemon.cpp)、[Manager 兼容解析](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHControlState.kt)。

## 本批实际验证

| 验证 | 结果与证据 |
| --- | --- |
| ledger/snapshot/capture/query | 既有回归和 **367 项扩展断言通过**，其中 120 项为本批新增。覆盖仅登记零目标/采样调用、乱序消费、旧命令穿插、重复/旧启动/淘汰/绑定冲突、迁移备份及开始/完成持久性故障。生产函数配合 IO/hash/时钟替身，最终含捕获流作用域保护的回归为 `out/batch12-state-host-final.log`。 |
| 提交输入、结果与真实路由块 | **122 项断言通过**。生产 JSON/CLI 语法、参数规范化/摘要输入、二次转义容量、结构化结果/畸形回包、状态与观察记录关系、hex/截断、异常、一次 exchange 和实际 daemon 特殊分派块；Journal、命令与 transport syscall 为计数替身，不冒充整机端到端。`out/batch12-submission-host.log`。配置字段校验另有原配置测试，此套不替换成接受任意配置的假验证器。 |
| 输入 IO 故障 | **52 项断言通过**，真实生产读取循环配合 syscall/时钟模型：类型/硬链接/大小、EOF、EINTR/EAGAIN、poll/read 失败、固定预算、stdin 标志恢复及错误时保留调用者输出。与上项共用日志。 |
| main→CLI→embedded | 保留 29 Maps、78 模块/开发、51 操作查询场景；新增 **90 提交路由场景通过**。覆盖有效/非法参数、root 拒绝、三种程序名、文件/stdin、路径字节、包装/嵌套拒绝、返回码及异常后的日志恢复，另校验 meta 独立能力/容量。`out/batch12-cli-host.log`。 |
| Manager 定向测试 | `SUMHControlStateTest` **20 项、零失败/错误/跳过**，新增 valid+null、较早执行记录被空投影替换、非法 prepared 旧 DTO 拒绝三个回归。Gradle 编译与定向测试成功，`out/batch12-manager-control-state.log`。没有重跑全部 Manager 测试或声称设备界面验收。 |
| daemon portable | 原 socket 故障回归通过，`out/batch12-daemon-portable.log`。新 execute 的单次请求/无启动行为由提交测试直接提取生产 exchange 函数验证。 |
| Linux POSIX 交叉编译 | 新提交、输入 IO、真实文件/pipe/socket/symlink 用例与原 daemon POSIX suite 均编译通过，**未在本机执行这些 Linux 二进制**。`out/batch12-submission-linux-compile.log`、`out/batch12-daemon-linux-compile.log`。真实 IO 用例已加入 Linux CI runner。 |
| ARM64 ksud | Release 编译链接通过，`out/batch12-ksud-build.log`。沿用 batch07 构建目录，clang-tidy 在该配置中关闭；不记为本批静态分析通过。 |

宿主命令使用 `C:/Users/29941/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe` 和现有 Zig 0.14.1。入口包括 `run_sumh_state_host_tests.py`、`run_operation_submit_host_tests.py`、`run_maps_preview_cli_host_tests.py`、`run_sumhp_daemon_host_tests.py --portable-only`；Linux 编译另加 `--target x86_64-linux-musl --compile-only`。新 runner 已注册 `scripts/ci/sumh_ci.py`。

Manager 从 manager 目录执行 `gradlew.bat :app:testDebugUnitTest --tests com.zying.zysu.ui.sumh.util.SUMHControlStateTest --console=plain --no-daemon -PABI=arm64-v8a`，使用本机 JDK 21 与 Android SDK。旧 logger 的非 Android unused-local 警告和 Gradle 已有弃用/非空断言警告不计为设备失败，也不隐藏为零警告。

最终 24 项实际 ELF/编译参数加固检查、7 项 CI 门禁测试、54 项编号对应与 267 个本地链接（21 份文档）、git diff --check 均通过。记录为 `out/batch12-final-{hardening,ci,docs,diff}.log`。

最终 ksud 为 3,581,968 字节，SHA-256 `571f76064151e5f7a22986baeacadd276547acdd8362a5f4d060c7ff0b37f767`；独立符号为 18,267,144 字节，SHA-256 `45ee84ecc3e22277fcbb0a9ba09b65da8dc178ffb179931c7d9f106c7f7eb8b4`。两者 build ID 均为 `9d8c2d6d8021bd0c86ee998aab67abbf2a2f3e1f`，完整 manifest 为 `out/batch12-final-artifacts.json`。构建目录沿用 batch07 名称，产物为本批最终代码；未部署。

## 审查与未闭合范围

设计阶段提出了历史 ID 消费、prepared 与旧 Manager DTO 的冲突、前缀分类不能替代生产语法验证、daemon 冷启动会恢复规则，以及有界淘汰后不能重建执行的约束，已体现在代码与测试中。客户端结果校验补齐字段与成功条件，stdin 恢复标志失败不再报告输入读取成功，捕获流通过作用域保护恢复。

实现后复核发现客户端可能接受 `already_started_or_finished` 与 prepared 观察记录的矛盾组合。已要求重复执行结果包含 started/finished、匹配摘要和同启动观察，not_retained/journal_unavailable 不得附带记录，boot_mismatch 必须匹配绑定且为不同/未知启动；冲突允许不同绑定或旧无绑定记录。补充正反例后 122 项提交测试通过，审查者复核最终代码后关闭此项，未发现本批范围内其他可确认问题。审查者负责状态层实现，故对状态层的检查属于自检；其对 root 编写的提交/传输代码复核记录于 `out/batch12-review-findings.md`，不冒充设备独立验收。

已更新主计划、全部 54 项账本、[运行状态与预登记指南](../zh/guide/sumh-runtime-state.md#提交前取得操作-id)、[场景 15](../zh/guide/sumh-usage-scenarios.md#15-命令断连后检查近期操作)和中文导航。未实现的 profile、带 plan ID 的模块 apply、F11/F12 等继续标为草案/后续实验。

真实 Android daemon/socket/SELinux、POSIX 锁与 fsync、掉电、设备/内核、观察者视图、性能和远端 Actions 本批未执行。主机断言不能证明这些环境行为。prepare/query 使用调用者文件系统视图，execute 使用 daemon 视图，未固定 namespace；源文件/配置的后续变化不受预约摘要约束。所有修改的硬期限、完整逐资源 saved/applied/partial 结果、后台队列、Manager 历史/向导、自动恢复全接入和规范化持久逐项恢复仍待后续。

本批只修改本地工作区，未提交、推送、发布、部署或修改真实设备。
