# SUMH 第四十二批实施与验证记录

日期：2026-10-19（Asia/Shanghai）。对应[54 项计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)和[进度账本](SUMH-implementation-progress-2026-10-16.md)。本批推进 **C04/F05/F09：临时 Maps 精确删除的可选 CLI 操作追踪**。这些任务仍为部分实施，全部目标继续进行；第四十一批及更早封存证据未重跑或覆盖。

## 1. 已实现行为

已有直接 `sumh maps remove` 和 Manager 临时规则工作台保持原行为。新增流程使用已有公开操作命令：先 prepare，保存确认的 operation ID，再以同一输入 execute；通过 show/wait/result 查询。用法与撤销见[Maps 指南](../zh/guide/sumh-maps-rules.md#临时删除的可追踪-cli-流程)及[场景 32](../zh/guide/sumh-usage-scenarios.md#32-在脚本中追踪一次临时-maps-精确删除当前源码已实现)。

| 环节 | 本批实现与边界 |
| --- | --- |
| 请求 | 仅接受精确九参数删除，四个数字必须规范十进制；inode/dev 可为零、dev 为 u32、其他身份为 u64、instance/generation 非零。旧直连 CLI 保留前导零兼容。prepare 只登记，不查询目标。 |
| 本地执行 | 验证后的临时 Maps execute 选择专用本地 capture route；无需 daemon，不发送 daemon 请求、不触发恢复。原有修改保持原 daemon 路由；原始 daemon 包装在消费预约前拒绝临时删除。 |
| 至多一次 | 强制复用同 boot、同命令/摘要的 prepared 预约，started 耐久写入和读回确认后才调用真实 Maps helper/client。已开始、已结束、缺失、淘汰或冲突均不重新调用；无自动重试或全局 clear 回退。 |
| 类型化回执 | schema 1、`resource:temporary_maps_mutation`，真实 operation ID、`history_supported:true`，始终 `saved:false`、`observer_verified:false`。严格核对键、token、请求摘要、ID、name、origin boot 和命令 exit/errno；历史支持不等于本次留存成功。 |
| 结束记录 | 本地路径使用 record-only finish，不采样全配置/内核，不增加其他功能的 verified 记录；原有 verified 内容保留。回执和 finished 摘要分别写入，任一步不确定都不授权重放。 |
| 异常结果 | 本地捕获结果经过与 daemon 回包相同的结构校验。不完整或不匹配的本地结果转换为 outcome_unknown，执行确定性为 null，保留 ID；不再调用 capture 或 daemon。 |
| 兼容 | 直接 CLI/Manager 仍返回 `temporary_maps`、null ID、history false。能力明确宣告 tracked CLI=true、tracked Manager=false，保留普通直连 journaling=false；没有增加内核 ABI、每调用者 owner 或新规则引擎。 |

“同一预约至多调用一次”依赖共同账本、boot 与协作锁，不表示全部副作用恰好一次。当前 namespace、文件系统路径和其他高权限主体绕过锁/回滚文件不由本批固定。删除得到确认也不追溯改变旧 Maps 读取流；当前集合和普通应用视图仍需独立验证。

## 2. 回执格式与迁移

`operation_resources.json` 现在写入 schema 2，读取兼容 schema 1 的隐藏回执及 schema 2 的隐藏/临时 Maps 回执。公开 operation result 包装仍为 schema 1，原预约提交和查询协议不变。共享容量仍是 **16 条 / 512 KiB 总编码 / 56 KiB 单条封装**，不扩大 IPC 缓冲，不留存通用 stdout/stderr、原始 argv 或授权凭据。

首次覆盖有效 schema 1 前，在已有 journal 锁内重读核对原字节；拒绝冲突或不可读备份；耐久保存或重新同步相同的 `operation_resources.schema1.bak`，读回确认，再复核原库未变。未知/损坏格式及备份失败保留原库。旧二进制拒绝 schema 2，降级需保留当前文件并明确转换；旧备份不包含升级后的操作，也不能恢复运行规则。

删除可能已经确认，但回执保存失败、仅 finished 摘要成功。此时 `operation result` 返回 resource_unavailable 等相应状态，不能从 command_completed 补造资源成功。相反，finished 落盘可能已可见但同步确认失败；事后读取不证明先前持久性。跨 boot 的 observed 回执只是历史，不证明当前条目缺失或应用所见内容。

## 3. 源码复核与修正

修改前保存 83 份 before-image，记录在 `out/batch42-before-manifest.json`。根任务负责路由/捕获/公共 helper，资源作者负责 codec/ledger，测试作者独立编写测试。分工复核记录为 `out/batch42-root-product-review.md`、`out/batch42-resource-root-review.md` 和 `out/batch42-resource-independent-review.md`；资源作者自己的检查明确只算作者自查。

独立复核发现一个 P2：本地提交原先直接构造外层结果，capture 内部吞下 begin/观测序列化异常时可能返回协议不接受的状态。根任务增加统一 reply 校验和本地未知结果归一化；复核确认无第二次分派。测试同时注入 begin 前异常、started 替换后的异常，并让提交层接收确切的不完整捕获状态；定向去掉校验的编译变异可重现该缺陷。

文档复核另纠正通用场景中的 daemon 前置条件、本地/传输未知分类、Maps 首页的历史支持状态与场景数量；第十四批“尚无 ID”改为历史限制，并指向第十五批已实现的隐藏流程。原计划的 41 项和 F01–F13 全部保留。

产品源码见[公共精确参数](../../userspace/common/temporary_maps_command.hpp)、[提交路由](../../userspace/ksud/sumhp/src/core/operation_submit.cpp)、[输入校验](../../userspace/ksud/sumhp/src/core/operation_submit_input.cpp)、[执行捕获](../../userspace/ksud/sumhp/src/core/command.cpp)、[Maps 回执](../../userspace/ksud/sumhp/src/mount/maps_temporary.hpp)、[资源模型](../../userspace/ksud/sumhp/src/core/operation_resource_model.cpp)及[账本与迁移](../../userspace/ksud/sumhp/src/core/state_ledger.cpp)。

## 4. 实际执行证据

主机为 Windows，宿主测试使用 Zig 0.14.1；实际 ARM64 Release 构建使用已安装的 NDK 30.0.16248370 / CMake 3.22.1。未运行 Android 设备或 Linux 真实进程/存储测试。

| 证据 | 结果与适用范围 |
| --- | --- |
| `out/batch42-host-a1/manifest.json` | 四个宿主 runner 均退出 0，所有记录输入在执行前后哈希相同。提交协议 288 次断言、输入 IO 52 次；账本共 873 次扩展检查，其中 337 次属于新增真实调用链/codec/迁移检查，不能重复相加为独立场景数。 |
| state 实际调用链 | 提取真实 capture、Journal、feature/delete client，使用生产 Maps helper、资源 codec 和查询；只替换存储、ioctl、时钟等边界。每个 feature/delete ioctl 要求锁仍持有且磁盘模型已为 started；精确高位身份、原生错误矩阵、无重复调用、无额外快照、独立写入失败、混合隐藏/Maps 回执和备份冲突通过。 |
| 既有 SUMH 回归 | client/runtime/inventory、临时规则、Kernel Build、恢复、模块与 Maps、持久化和迁移等既有 suite 通过。临时 CLI/parser 106 个用例、Manager callback/client 121 个用例；另以 Android errno 固定值生成兼容夹具。Windows 报告 symlink fixture unavailable 的既有分支未执行，不计为通过的真实符号链接验证。 |
| 精确 CLI 路由 | 实际 main/CLI/embedded 提取测试通过，包括 operation submission 90 个路由用例、临时 Maps 64 个和临时 Manager 42 个；另覆盖既有查询、root 检查和 daemon 拒绝。日志中的权限/用法错误是预期拒绝夹具。宿主 logger 的已有 unused-variable 警告未阻止该 runner；不是 Android 运行证据。 |
| `out/batch42-legacy-compatibility.json` | 20 份旧 Manager JSON 信封逐字节相同，另生成的 20 份原始正文与旧信封 stdout 逐字节相同；未替换仓库原夹具。Android errno 是专用合成协议目标，不声称执行了 Android。 |
| `out/batch42-controls-a1/manifest.json` | 两个当前语义对照通过；去掉本地结果校验的变异、替换为本批真实旧 input parser 的对照均先编译成功，再以指定语义标签/退出码 253 失败。旧解析器对照仅证明原来拒绝新输入，不代表完整旧二进制，也不声称旧版重复删除。 |
| `out/batch42-build-a2/manifest.json` | 修正后的实际 `ksud` ARM64 Release 编译链接通过，输入哈希稳定；产物 4,235,808 字节，SHA-256 `c5b3961d78cbb6e83aec81aaef943bcaab6615b1db88ae8f394f0fb7f8345c09`。保留未剥离符号、编译命令和日志；只证明编译/链接。 |

初版 build-a1 在异常结果修正前已编译通过，保留作过程证据，最终以 a2 为准。测试开发曾修正混合 auto 类型、Windows errno 宏缺失、无效 verified seed、错误隐藏 revision 及错误拒绝既有 generic Maps clear 的测试假设；最终 suite 使用修正后的输入。测试报告为 `out/batch42-tests-report.md`。根任务首次兼容性比较错误地把生成的原始正文也当作仓库独立文件查找，调整为“20 个信封 + 20 个正文与信封 stdout”后完成精确比较，产品和夹具均未因此修改。

现有 state harness 使用替代 SHA-256 实现验证参数编码与绑定流程，不测试密码库正确性。宿主模型验证生产调用顺序和故障合同，不证明真实 fsync、锁竞争、崩溃/断电、内核并发或 SELinux。临时规则 helper 的模型注入不是设备 ioctl 执行。

可复现入口如下，工具变量替换为本机路径，输出目录须为新目录：

```powershell
& $python -B userspace/ksud/tests/run_operation_submit_host_tests.py --zig $zig
& $python -B userspace/ksud/tests/run_sumh_state_host_tests.py --zig $zig
& $python -B userspace/ksud/tests/run_sumh_host_tests.py --zig $zig --temporary-maps-fixtures out/new-maps-fixtures
& $python -B userspace/ksud/tests/run_maps_preview_cli_host_tests.py --zig $zig
& $python -B userspace/ksud/tests/run_tracked_maps_controls.py --zig $zig --baseline-root out/batch42-before --out-dir out/new-tracked-controls
```

前四个 runner 已在现有 native CI 中登记，本批没有修改 CI 注册或声称远端 Actions 已运行。最后一个旧源码/变异对照依赖本地 before-image，单独执行，不把未纳入仓库的旧基线设为 CI 前提。

## 5. 后续工作与交付状态

C04/F05/F09 保持部分实施。临时 Maps 的 Manager 持久检查点、类型化历史模型和按 ID 恢复尚待接入；不能将隐藏专用 config revision 当成 Maps generation。其他资源、持久任务队列、完整端到端期限、每调用者 owner、真实 Android 生命周期和观察者验收继续保留。

未执行 Linux 真实 flock/fsync/断电、Android/JNI/SELinux、真实内核 generation 并发、普通/isolated/root 观察者或性能验证；未重跑 KMI/DDK 构建。F06/F11/F12 的 owner/事务/设备门禁保持。本批只修改共享工作区，没有提交、推送、安装或发布；批次完成不代表整个 54 项计划完成。
