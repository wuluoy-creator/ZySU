# SUMH 第十一批实施与验证记录

日期：2026-10-16。对应[54 项实施账本](SUMH-implementation-progress-2026-10-16.md)和[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)。本批推进 C04/F08/F09 的操作历史与查询基础；三项仍为部分实施，整体目标继续进行。第十批结果保留，不重复记作本批测试。

## 实现范围

- 复用 `state_ledger.json`，磁盘格式从 schema 1 扩为 schema 2，保留最后操作及较早记录；总数最多 16 条，编码后最多 32 KiB，按最旧优先淘汰并持续标记。原来 started/unknown 不再被下一条操作直接覆盖，仍受容量限制。
- schema 1 查询不迁移、不创建文件；首次受记录修改前将原始字节耐久备份到 `state_ledger.schema1.bak`。相同备份再次同步，不同或不安全备份拒绝覆盖。未来格式、未知字段、重复 JSON 键/ID、无效 UTF-8 和超限输入保留原文并拒绝写回。
- 新增精确本地 `operation list`、`operation show <id>`、`operation wait <id> <0..10000-ms>`，绕过普通 parser/logger 与 daemon 冷启动，异常参数也在该边界拒绝。仅读取账本与 boot ID；不查询内核、配置、Maps 或模块，不构造 Journal。通用包装/daemon call 路径拒绝这些查询。
- schema 1 查询输出区分 observed、not_retained、wait_timeout、unavailable、context_changed_or_unknown。started 的 exit/errno 为 null；命令完成不证明运行生效或持久性已经确认。等待只轮询，不取消/重放修改；预算以单调时钟计算，单次 IO 不是硬可取消期限。
- Journal 只允许一次 begin/finish，检测保留 ID 碰撞；命令执行必须在 begin 的耐久写入与读回成功后进行。`CommandResult` 与原始 daemon 信封增加 operation_id、operation_recorded，后者仅表示最终记录写入/读回确认。最终写失败后 ID 保留，磁盘可能出现 started 或 finished，不能自动回滚或再执行。
- 原 `api state` 及 Manager 的 schema 1 最后操作合同保持兼容；`api meta` 分开宣告响应格式、磁盘格式、查询支持与容量。普通 CLI 继续消费原 stdout/stderr/退出码，新增 ID 元数据目前在原始 daemon 响应中；列表只能提供候选，不保证丢失响应后的唯一关联。

实现源码：[历史模型](../../userspace/ksud/sumhp/src/core/state_ledger_model.cpp)、[持久账本](../../userspace/ksud/sumhp/src/core/state_ledger.cpp)、[查询输入](../../userspace/ksud/sumhp/src/core/operation_query_input.cpp)、[查询执行](../../userspace/ksud/sumhp/src/core/operation_query.cpp)、[命令结果](../../userspace/ksud/sumhp/src/core/command.cpp)、[daemon 信封](../../userspace/ksud/sumhp/src/core/daemon.cpp)、[精确分派](../../userspace/ksud/sumhp/src/embedded.cpp)。

## 本批实际执行的验证

| 验证 | 结果与证据 |
| --- | --- |
| 生产 ledger/snapshot/capture 回归与操作合同 | 既有回归通过，新增 **247 项合同断言通过**。覆盖原始备份、迁移失败/读回、未完成历史保留、条数与转义后字节淘汰、未知格式/重复键/重复 ID/无效 UTF-8、碰撞及 Journal 单次生命周期。`out/batch11-state-host.log`。 |
| 生产查询输入及轮询 | 同一 247 项内验证 list/show/wait，0/250/10000 毫秒注入时钟、完成/淘汰/读失败/boot 改变、反复 EINTR 的读取预算，以及持有写锁时查询。断言零写入、零创建目录/锁、零配置/内核/命令回调。仅替换 IO/时钟边界，实际生产函数参与测试。 |
| 实际 main→CLI→embedded 分派 | **51 项新增操作场景通过**，含有效入口、严格参数、root 校验、异常后日志级别恢复、包装/嵌套拒绝与 su/sh 优先级；另有生产信封序列化、meta 和 daemon dispatch 拒绝验证。既有 29 项 Maps、78 项模块/开发分派及 parser/logger 回归通过。`out/batch11-cli-host.log`。 |
| daemon 传输故障替身 | Windows portable socket 故障回归通过，`out/batch11-daemon-portable.log`。 |
| POSIX daemon 测试交叉编译 | x86_64-linux-musl 编译通过，**未执行**该 POSIX 二进制，`out/batch11-daemon-linux-compile.log`。 |
| ksud ARM64 Release | 编译、链接及构建期产物加固检查通过，813 条编译记录；`out/batch11-ksud-build.log`。该复用构建目录的 ksud clang-tidy 配置为关闭，不将其记为本批静态分析通过。 |
| 最终产物与门禁 | 24 项实际 ELF/编译参数加固检查、7 项 CI 门禁测试、54 项编号对应与本地文档链接检查、git diff --check 全部通过。记录为 `out/batch11-final-{hardening,ci,docs,diff}.log`。 |

宿主入口为 `run_sumh_state_host_tests.py`、`run_maps_preview_cli_host_tests.py`、`run_sumhp_daemon_host_tests.py --portable-only`，使用仓库外现有 Zig 0.14.1；POSIX 交叉编译另加 `--target x86_64-linux-musl --compile-only`。两个扩展 runner 早已接入 `scripts/ci/sumh_ci.py`，本批没有新增遗漏的 CI 入口。

这些是 Windows 宿主测试与 ARM64 编译证据。实际 socket/SELinux/文件系统 fsync、掉电、设备启动、Manager 生命周期、内核构建、观察者视图和性能未执行。Manager 本批没有改动，未重跑其界面测试，不把第十批 280 项记成本批结果。测试中 SHA 库、IO 和时钟有替身，不等于设备持久化或硬期限验证。

最终 ksud 为 3,540,496 字节，SHA-256 `3c84679a662880bf46cb4bafd8b9a66a57b746ee72287cb02f5c2e312e599dd0`；独立符号为 18,050,888 字节，SHA-256 `e9cf7d99bd72c95d572030c95445a38014b694f9858d3431d0ba36746ade4580`。两者 build ID 均为 `91ad8ac512d99a61bc7f73c09e9e56d1c15bd4e6`，完整 manifest 为 `out/batch11-final-artifacts.json`。构建目录沿用 batch07 名称，但文件来自本批最终源代码；未部署。

## 审查、文档与剩余边界

实施前只读审查发现并要求保留的关键区别：最终写失败后可能已经替换 finished；普通 CLI 不输出新增信封字段；同名历史不能唯一关联丢失的响应。本批按此修正注释、结果字段和指南。测试作者在实现后复核实际调用链，未发现可确认的新正确性问题；这不是原独立审查者对最终代码的第二次独立验收。过程记录为 `out/batch11-independent-review.md`、`out/batch11-state-tests-status.md` 和 `out/batch11-final-review.md`。

已更新主计划、54 项账本、[运行状态指南](../zh/guide/sumh-runtime-state.md#近期操作历史与等待)、[场景指南第 15 项](../zh/guide/sumh-usage-scenarios.md#15-命令断连后检查近期操作)及中文导航。当前命令与后续草案继续分开，历史静态审查和旧批次测试记录保持原证据范围。

查询明确使用调用者文件系统命名空间，未固定或采样 namespace 身份。16 条历史不是完整审计，也不是配置恢复点。额外分区直接保存、部分自动恢复尚未统一记录；调用者预分配 ID、跨断连唯一关联、typed 变更结果、后台队列、Manager 历史/向导、所有修改入口期限及规范化持久逐项恢复仍待后续实施。C04/F08/F09 与全 54 项目标不因本批通过而完成。

本批仅修改本地工作区，未提交、推送、发布或操作真实设备。
