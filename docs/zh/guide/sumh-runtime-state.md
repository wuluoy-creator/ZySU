# SUMH 运行状态查询

模块启动/热重建另有精确本地 `module rebuild-status`，记录候选与最近确认计划、成功调用前缀及未解决意图。它不属于 `operation result` 的隐藏资源回执，不证明当前运行集合。见[模块重建记录与失败处理](sumh-module-rebuild.md)。

本页描述仓库新增的控制器接口；使用前核对 ksud 与内核版本。能力与普通应用所见结果不同，实际视图仍按 [SUMH 行为验证](sumh-validation.md)检查。

第十八批增加独立的[能力与作用范围中心](sumh-capabilities.md)及精确本地 `api capabilities`。它只读采样保存设置、六项内核控制、构建字符串和后端能力，不启动 daemon 或恢复；与下面的通用查询区别见该指南。全量定义、规则和操作历史继续使用本页相应入口。

## 查询入口

```sh
/data/adb/ksud sumhp api meta --control-only
/data/adb/ksud sumhp api runtime
/data/adb/ksud sumhp api state
```

`api meta` 的 `runtime_status_schema:1` 表示控制器能输出该格式，不代表内核实现了所有能力。控制器再分别检查内核的 `runtime_state` 与 `runtime_set` 支持位；不从协议版本 1 推断支持。`api sumh` 也附带同一格式的 `runtime_status`，原字段保留兼容。判断未知状态时优先使用新字段，不以旧字段中的 `false` 推断关闭。

`api runtime` 输出 schema 1 JSON。`known` 表示读到有效结构，`unsupported` 表示能力查询成功但没有新查询接口；`error` 表示能力或状态读取失败，`unavailable` 表示传输不可用或协议不匹配。后三种均返回 `runtime:null`，不会编造关闭值。前两种退出码为 0，后两种为非零；JSON 仍保留原因和 errno。

| 字段 | 解释 |
| --- | --- |
| `source` / `observation` | `kernel_ioctl` / `control_plane_only`，仅为控制面采样，没有执行观察者测试 |
| `sample_started_monotonic_ms` / `sample_finished_monotonic_ms` | 此次查询区间，单调时钟毫秒字符串，不是墙上日期 |
| `boot_id` | 当前 `/proc/sys/kernel/random/boot_id`；读不到时为 `null` |
| `runtime.instance_id` / `runtime.generation` | 内核运行实例和控制变更序列，均用十进制字符串避免 JSON 数字精度损失 |
| `runtime.consistency` | 控制设置与错误在同一锁下读取；外部就绪状态分别采样，不承诺全局原子快照 |
| `runtime.features[].supported` | 该功能所需机制在采样时可用，不是仅凭配置或版本推断 |
| `configured` | 实际内核开关，**不是持久配置文件中的保存值** |
| `active` / `degraded` | 开关、机制及所需依赖决定的活动状态；已配置但未活动时 degraded 为真，包含主动全局暂停 |
| `last_set_errno` | 最近一次对应 setter 的负 errno；成功后为 0。不是当前降级原因，也不是普通应用测试结果 |

六项功能为 debug、stealth、mount hide、Maps spoof、statfs spoof 和 Kernel Build。四项视图功能依赖全局 SUMH 开关；statfs 还依赖 Mount Hide。调试日志仅依赖自身开关，Kernel Build 直接更新全局 UTS 字符串，不能因为关闭全局视图就显示它们已经撤销。

generation 只跟踪控制变更及记录的尝试，不是模块规则 generation、配置 revision 或 readiness 的变更版本。外部 hook readiness 可以在 generation 不变时改变。instance 用于区分重新初始化；不能跨启动重放旧 token。ARM32 的固定布局编译测试不代表开放了 compat 调用，现有 compat 入口仍拒绝 SUMH 操作。

## 应用与失败处理

正常使用原有配置保存与明确应用入口。新内核同时支持查询和条件写入时，控制器对 debug、stealth、mount hide、Maps 开关及 statfs 采用“查询旧值→带 instance/generation 写入→查询确认”。状态过期返回失败，不自动重试。只有能够确认是本次写入且没有后续变更时，才用另一次条件写入回滚该项。

配置值符合请求但因全局暂停或依赖暂不活动时，会保留已应用的设置并报告原因，不把配置写入误判为失败。无法读取结果或检测到其他写入时报告未知，不盲目覆盖。旧内核继续走兼容路径，缺少读回的失败仍为未知；仅支持 GET 的内核不被当成具备条件写入。

当前开关条件接口只接受 normal Mount Hide；配置层保留原来的 aggressive 能力检查和回退。第五批另为 Kernel Build 字符串新增独立 `kernel_build_cas` 能力，公共配置应用和显式 `kernel-build set/reset` 使用一致快照、条件设置及双字段读回。旧内核不再无条件回滚字符串。条件写入仍不是跨功能原子事务；Maps 受管集合替换与开关切换是两个步骤，集合已替换但开关失败会报告部分完成，集合没有 CAS 删除/替换。启动阶段的直接恢复入口尚未全部纳入操作日志，不能据此声称所有写入已形成统一事务。修改来源文件造成的数据变化不会由规则回滚恢复。

查询异常时保留有效配置，记录 JSON、组件版本与最近动作，再重读状态；不要用全局 clear 作为通用修复。撤销时恢复事先记录的配置并按原生效阶段应用，分别核对持久配置、内核运行状态和目标观察者。

新 GET ioctl 自身不初始化或恢复规则；上述完整 CLI 请求仍可能先启动 daemon 或查询旧 GET_FEATURES，不能宣称端到端无副作用。精确 `sumh maps preview`、`module plan/explain/inspect`、normalize dry-run、分区发现、本页的 `operation list/show/wait` 和第十三批 `hide status` 已有独立分派，可绕过 daemon 恢复；具体范围分别见对应指南。

## 保存版本、运行差异与最后操作

`api state` 是已实现的 schema 1 查询入口，`api meta` 用 `state_ledger_schema:1` 宣告格式。它同时返回当前控制面采样、每类保存定义的摘要，以及最后一项已记录操作。它不执行配置恢复或写入记录；完整 CLI 启动边界仍受上面的限制。

第十三批用户隐藏定义改为版本化 ID/元数据/enabled。`api state` 的隐藏 desired revision 仍只比较去重排序的 **enabled 路径集合**，detail 标明 `revision_scope:enabled_path_set_only` 和启用/停用数量；名称、备注、分组及条目顺序不会改变这个运行成员关系摘要。第十四批 Manager 默认定义列表完整读取启用/停用及元数据，通过专用有界通道调用 status 和条件变更；普通修改仍走 daemon/journal；第十四批当时尚未接入提交前唯一 operation ID，第十五批已为稳定隐藏工作流补齐[预登记和按 ID 恢复](#按-id-恢复资源结果)。超时后不得自动重放或直接认领最新同名记录。条件修改需要的 **整个定义文件 revision** 来自 [hide status](sumh-user-hide-definitions.md#先读取状态和版本)，不能用 api state 的摘要替代。

| 字段 | 解释 |
| --- | --- |
| `features.<名称>.desired_revision` | 该类保存意图的 `sha256-v1:` 摘要；无法读取或解析时为 `null`。摘要不是可按大小比较的版本号，也不是可用于写入 CAS 的 token |
| `status` / `reason` | `matches`、`drift`、`unknown`、`unsupported` 或 `deferred`，分别表示采样匹配、差异、未知、所需机制不可用或等待启动阶段；具体原因单独给出 |
| `last_verified` | 历史上一次确认的摘要、boot ID、instance 和 generation。历史记录不代表现在仍然生效 |
| `applied_revision` | 仅在本次采样匹配、摘要相同，且历史验证属于同一启动/内核实例时给出；否则为 `null`。表示可查询的控制设置，不表示普通应用实测，也不是全部资源的应用证明 |
| `history.status` | `valid`、`missing`、`unreadable` 或 `corrupt_or_unsupported`。历史不可用不阻止输出本次可读取的状态 |
| `history.last_operation` | 保留记录中最近一次 started/finished 命令；仅有 prepared 时为 null，不把预约当执行。持久化记录，含随机操作 ID、命令类型、启动身份、阶段、退出码和 errno；不保存路径、配置正文或授权凭据 |
| `history.last_operation.result` | `completed` 表示命令返回成功，`failed_or_partial` 表示失败或部分完成，`outcome_unknown` 表示只有开始记录；均需结合逐功能状态判断 |

五项开关比较保存意图与内核 configured，并单独显示 active。Kernel Build 比较完整两字段，停用也核对原始字符串。用户隐藏比较受管路径集合并列出待绑定、错误和全局暂停数量；待绑定不冒充已生效，`SUSPENDED` 仍只是全局停用状态。其 `verification_scope=registered_definitions` 只核实登记定义，`binding_consistency=per_rule_sampled` 表示绑定健康逐条采样，不保证所有路径在同一时刻均已绑定；全局暂停也会有明确原因。

支持结构化分页的内核上，`api state` 还会核对受管 Maps：在 init 挂载命名空间读取完整受管集合，用与实际应用相同的规则解析保存路径，对比 target/spoofed 的 inode、设备号及替换路径。文件身份采样两次，分页前后使用同一 instance/generation。原地修改内容通常保留身份；替换 inode 后旧登记会显示差异。缺文件、非普通文件、不可表示的身份或查询失败显示未知；硬链接目标碰撞显示差异。这里仅核对登记身份，Maps 开关和 active 单独显示，也不证明应用中的实际 Maps 视图。

关闭 Maps 时保留保存定义，不解析这些路径，仅核对受管集合是否清空。临时数字规则不参与这项匹配，仍可能优先影响观察结果。核对最多读取 256 条受管记录；超限、中途失败或 token 变化不会把部分列表当成完整集合。字段 `verification_scope=registered_managed_maps_vs_current_path_identities` 和 `identity_consistency=twice_sampled_not_pinned` 明确其范围：没有固定 inode 生命周期，不能排除两次采样间的 ABA 变化或之后的文件替换。

这项核对使用独立只读子进程，父进程按单调时钟限制等待为 5 秒，超时请求结束该采样并返回未知，不接受部分共享结果。仅通过子进程交付的 pidfd 发送终止信号，并用 P_PIDFD 非阻塞回收；没有数字 PID 回退，不改动应用规则的子进程执行语义。未退出子进程最多保留一个，未回收前后续采样返回 EBUSY。内核或策略不支持 pidfd/P_PIDFD 时无法完成这项核对；无法取得进程句柄的槽位保留到控制器退出，不反复创建子进程。进程创建、系统调度及其他控制器查询仍不受完整硬期限保证，C04 的端到端期限尚未完成。

模块目前只摘要保存定义的字节，尚未验证源文件树或最终映射；overlay xattr 也尚无结构化枚举，因此这些项目保持 `unknown`，不会因为某次命令返回 0 就填入 applied revision。模块字段注明其摘要范围，语义预检仍需生产 Planner。各配置文件每次采样有 4 MiB 读取预算；超限保留文件并报告未知，不截断后声称完整。Maps 的保存摘要同时包含启用状态和路径规则，不沿用只摘要路径的历史验证记录。

查询先读取定义，再采样运行状态和必要的字符串/隐藏信息，最后重读定义与 instance/generation。检测到变化时报告未知，不自动应用。此过程是带前后检查的采样，不能排除外部文件先改后还原，也不能锁住独立 readiness、事件绑定或第三方直接修改内核 UTS；不是全局原子快照。内嵌 runtime 的时间字段仅对应其自身查询区间。

受记录的修改命令在执行前耐久写入 `started`，完成后保存命令结果与可验证状态。记录上限为 32 KiB，独立文件锁不无限排队；开始记录失败时不执行命令，结束记录失败时返回非零并提示先查询，不自动重复操作。进程中断留下 `started`，查询显示未知；如果写入已替换但耐久性未确认，记录可能已在当前文件系统中可读，仍不能当作掉电保证。未知格式不被自动覆盖。记录不可读或未来版本阻止新的受记录修改，需先保留原文件并使用匹配版本排查；不要删除全局规则来修复日志。

`api state` 继续只返回最后一项命令及逐功能最近验证，保持 schema 1 兼容。第十一批的独立历史查询及第十二批的预登记执行见下节；方案恢复、完整资源结果及 Manager 全部入口的 typed result 仍为计划项。自动恢复入口也未逐一记录；启动后当前采样仍会重新核对，旧启动的 applied revision 不会直接沿用。

退出码为 0 表示查询得到可输出的记录格式，**不保证每个功能都匹配**；历史损坏或读取失败返回非零，但仍输出当前状态 JSON。遇到未知结果先读取此状态和原命令错误，再决定重试或按原生效阶段撤销。撤销配置、恢复运行集合和恢复来源文件内容是不同操作。

## 近期操作历史与等待

第十一批已实现以下精确本地命令。必须在授权 root shell 中直接运行，不加全局选项包装或 `daemon call`；这些查询不启动 daemon、不创建日志/锁/目录、不迁移账本，也不采样配置或内核状态。

```sh
/data/adb/ksud sumhp operation list
# 从本次列表选择要检查的 id；下面的 ID 是占位符
/data/adb/ksud sumhp operation show 0123456789abcdef0123456789abcdef
/data/adb/ksud sumhp operation wait 0123456789abcdef0123456789abcdef 5000
```

`api meta` 分别宣告 `operation_query_schema:2`、`operation_list/show/wait:true`、`operation_history_limit:16` 和 `operation_wait_max_ms:10000`。`state_ledger_storage_schema:3` 是磁盘格式；原来的 `state_ledger_schema:1` 仍指 `api state` 响应格式。

列表按新到旧排列，保留最多 16 条、编码后的账本不超过 32 KiB，超过任一预算即淘汰最旧记录，并持续标记 `history_pruned:true`。未完成和 prepared 预约记录也受此保留上限约束；预约记录另外保存命令参数摘要，不保存参数正文。`history_complete:false` 始终提醒这不是所有变更的完整审计；额外分区直接保存和部分自动恢复目前不使用此账本。历史仅含随机 ID、命令类型、boot ID、阶段与结果说明，不存目标路径、配置正文或凭据。

| 查询结果 | 退出码与含义 |
| --- | --- |
| `observed` | 0，成功读取列表或目标记录；不表示被查询命令成功。列表为空时结合 `ledger_status:missing` 判断记录文件不存在。 |
| 参数无效 | 2，stderr 给出用法，不访问账本。ID 必须为 32 位小写十六进制；等待时间为 0–10000 的规范十进制整数。 |
| `not_retained` | 3，当前没有保留此 ID；可能未记录、已淘汰或文件缺失，不能推断“从未执行”。 |
| `wait_timeout` | 4，预算耗尽仍只看到 prepared 或 started；不取消命令，也不允许自动重发。 |
| `unavailable` / `context_changed_or_unknown` | 5，账本损坏/未来格式/读取失败，或 boot ID 读取失败/变化；保留原因，不把失败当成空历史。 |

第十二批查询升级为 schema 2。每条 `records[].result` 为 `not_started`（prepared 预约）、`outcome_unknown`（started）、`command_completed` 或 `failed_or_partial`。`request_revision` 是预约参数摘要，旧命令为 null。prepared 尚未执行，prepared/started 都没有可确认的退出码，JSON 中 `exit_code/errno` 为 null；即使来自上次启动，也不能据此断言失败、仍在执行或已撤销。`same_boot` 仅比较 boot ID。完成记录不验证当前规则、持久配置或应用视图；`runtime_verified:false` 明确这一限制。

wait 每 100 毫秒左右重新读取账本，以单调时钟限制轮询预算；0 毫秒只读一次。读到 finished 立即返回 0，需再看记录的退出码；中途淘汰或读取失败立即返回对应状态，不重试变更。此预算不是可强制中断磁盘 IO 的硬期限，系统调度与单次 IO 仍可能延迟。查询使用调用者文件系统命名空间，未固定或比较 namespace 身份，不能把不同 namespace 中的读数视为同一视图。

磁盘 schema 1/2 只读兼容，首次显式预约或受记录修改先把原始字节耐久备份到对应 `state_ledger.schema1.bak` / `state_ledger.schema2.bak`，再升级到 schema 3。schema 1 保留旧 last 并标记较早历史未知；schema 2 保留已有历史。普通旧记录没有预约绑定，不能拿来 execute。相同备份会再次同步；不同内容、未知字段、重复 JSON 键、无效 UTF-8 或未来格式均阻止写回，原文件保留。降级前应保留当前文件并采用明确转换；旧程序不应直接覆盖 schema 3，备份也不是配置或运行规则恢复点。

命令开始记录成功后，原始 daemon 响应信封新增 `operation_id`；`operation_recorded:true` 仅表示最终记录的耐久写入和读回均已确认。最终保存失败仍携带 ID，但 recorded 为 false；磁盘可能已经显示 finished，因此查询固定报告 `durability_confirmation:not_observable_from_read`，不从可读字节推断先前 fsync 已被确认。普通 CLI 保持原 stdout/stderr 输出，尚不暴露这两个信封字段，可用 `operation list` 找候选 ID。普通旧 CLI 没有提交前 ID；多条同名操作或整份响应丢失时，列表不能唯一关联某次提交，**不要自动选最新一条并重放原命令**。

此查询没有需要撤销的变更。遇到未知结果先保留命令错误和历史，再按资源查询保存/运行状态；查询状态、配置恢复及撤销运行规则是不同流程。第十五批已提供用户隐藏的 Manager 历史和独立留存资源结果；后台完整任务队列、其他资源及端到端期限仍待实现。

## 提交前取得操作 ID

第十二批新增精确本地 `operation prepare --input <绝对文件路径|->` 与 `operation execute <id> --input <绝对文件路径|->`。`api meta` 分别宣告 `operation_prepare`、`operation_execute`、`operation_reserved_id` 和 `operation_submission_schema:1`；仍为 `operation_caller_id:false`，ID 由控制器生成。通常需先在正常操作流程中明确建立 daemon 可用状态，其启动可能按原策略恢复保存规则。prepare 与 execute 自身都不启动 daemon；原有修改命令仅向已有 daemon 发送一次请求，不自动重试。第四十二批新增临时 Maps 精确 remove 的本地执行分支，无需 daemon，不触发恢复；步骤见[Maps 可追踪 CLI](sumh-maps-rules.md#临时删除的可追踪-cli-流程)。

输入文件格式如下，`args` 为 `sumhp` 之后的独立参数，内容不交给 shell：

```json
{"schema_version":1,"args":["sumh","maps","apply"]}
```

```sh
# 使用事先保存的同一输入文件；先检查 prepare 的 JSON 和退出码
/data/adb/ksud sumhp operation prepare --input /data/local/tmp/sumh-operation.json
# $operation_id 必须取自刚才 reservation_confirmed:true 的成功回包并保留
/data/adb/ksud sumhp operation execute "$operation_id" --input /data/local/tmp/sumh-operation.json
# 断连或结果未知时只查询这个预先保存的 ID
/data/adb/ksud sumhp operation show "$operation_id"
/data/adb/ksud sumhp operation wait "$operation_id" 5000
```

也可将同一 JSON 通过 stdin 传入 `--input -`。文件必须为普通、单硬链接且末级非符号链接文件；stdin 接受普通文件、pipe 或 socket，拒绝终端和设备。最多 48 KiB UTF-8、2–32 个字符串参数，拒绝 NUL、重复/未知 JSON 字段、未来 schema。读取有 1 秒总轮询预算，单次文件系统 IO 不保证硬可取消。外层 daemon 请求经过二次转义后最多 64 KiB；超限在预约之前拒绝，不通过扩大 IPC 缓冲绕过。

原有命令两端复用生产 CLI 语法校验；第四十二批另接受恰好九个参数、十进制规范拼写的临时 Maps 精确 remove，普通直连仍兼容前导零。除此之外，只接受原有受操作日志保护的修改命令；不接受查询、daemon 控制、内部 automatic 参数、嵌套 operation 或额外分区直接保存。目标存在性、模块 owner、能力及资源语义仍在实际执行时检查，prepare 不是 Planner 预检。摘要为 `sha256-v1:`，绑定版本域与紧凑 args 数组；JSON 排版/字段顺序变化不改变绑定，参数内的 Unicode、空格、末尾空格及入口允许的换行保留。可选 `--` 分隔符也参与绑定，执行时由生产 parser 去除。

prepare 仅写入 schema 3 的 prepared 预约，包含随机 ID、命令类型、请求摘要与 boot ID，不查询/修改目标配置或运行规则。确认耐久保存并读回后才返回 `reservation_confirmed:true`。预约属于 16 条/32 KiB 近期账本，会被后续操作淘汰；prepare 回包丢失可能留下未执行预约，再 prepare 会生成另一个 ID，不会执行原命令。

execute 在同一排他锁下找到**仍保留、同启动、名称和摘要相同的 prepared 记录**，耐久写入并读回 started 后才调用命令；完成后保存 finished。历史中的预约会移至最近位置。started/finished 永不重复调用，缺失/淘汰 ID 不重建，旧启动或不同输入拒绝。该保证是共享同一账本、boot 与协作锁条件下的“同一预约至多调用一次”，不表示所有副作用恰好一次或全局原子成功。任意 root 回滚账本、路径/inode 固定、源文件和配置 revision、Planner plan ID、namespace 固定均未包含在绑定中。prepare/query 使用调用者文件系统视图，原有 execute 使用 daemon 视图，临时 Maps 本地 execute 使用调用者视图；视图不一致时可能找不到 ID，不能偷偷新建预约补救。

stdout 只输出 schema 1 的 `sumh.operation.submission` JSON；日志或用法错误不混入结果。`status` 为稳定分类，`error` 和捕获的 stderr 为诊断文本，不能按其文字编写重试规则：

| status | 退出码与处理 |
| --- | --- |
| `prepared` | 0，仅登记成功，保留 operation_id 和 request_revision。 |
| `command_completed` | 0，本次命令返回 0 且最终记录确认；仍需查询各资源和实际观察者。 |
| `failed_or_partial` | 1，命令已调用并返回非零；可能已有部分修改。 |
| `already_started_or_finished` | 1，本次未执行；observed_record 给出保留记录，旧输出不重放。 |
| `not_retained` / `request_conflict` / `boot_mismatch` | 1，本次未执行；不能把未保留解释成从未执行。 |
| `journal_unavailable` / `reservation_unconfirmed` | 1，无法确认开始/预约提交，本次没有调用目标；当前文件可能已经替换，先查询，不能假定没有记录。 |
| `outcome_unknown` / `transport_unknown` | 1，完成记录未确认、执行异常或请求/回包不确定；保持原 ID 查询，禁止自动重发或改用新 ID 重放。 |
| `rejected` | 2，本地输入读取/校验失败；用法参数错误也为 2，stderr 给出用法。 |

`command_executed_this_attempt` 为 true/false；传输或无法判明的执行异常为 null。`command_exit_code/command_errno` 只在本次确实观察到命令结束时给出。`final_record_confirmed` 指最终记录的耐久提交和读回；`runtime_verified:false` 明确未据命令结果证明实际生效。客户端严格核对版本、ID、摘要、类型、状态不变量及输出字节数；daemon 的不完整或不匹配回包归为 transport_unknown，本地临时 Maps 捕获结果不完整或不匹配则归为 outcome_unknown；两者均保留 ID 并禁止自动重放。

stdout/stderr 捕获各最多 64 KiB，以 `{encoding,value,byte_length,complete}` 输出；超出部分丢弃但不打断已开始的修改，complete=false。截断到 UTF-8 中间或原输出非 UTF-8 时用 hex 保留捕获字节。`command_completed` 与输出完整性独立，脚本解析命令文本前必须检查 complete。普通旧命令输出行为保持原样，其全链容量仍待 C05 完成。

未执行预约不需要撤销运行规则，保留到自然淘汰即可；当前没有取消/删除单条预约接口。预约和历史文件不是配置备份或恢复点，不纳入方案重放。已执行变更按资源类型恢复并重新验证，不能通过清空账本或全局 clear 撤销。

## 结构化规则分页

第六批新增 `api inventory`，用于读取内核已登记定义。`api meta` 的 `rule_inventory_schema:1` 宣告控制器格式；内核独立协商 `rule_inventory` 能力（bit 20）。仍兼容协议 1，不能仅凭协议号认为此接口存在。

```sh
# 查询支持的类别和当前集合身份；不返回规则
/data/adb/ksud sumhp api inventory
# 读取受管 Maps 或模块映射类别的第一页
/data/adb/ksud sumhp api inventory maps_managed
/data/adb/ksud sumhp api inventory mapping
```

| 类别 | 返回内容与边界 |
| --- | --- |
| `mapping` | `visible_path` 是可见目标，`backing_path` 是实际来源；type 保留内核记录值。不能从来源路径推断模块所有权或最终后端胜出关系 |
| `legacy_hide` | 隐藏路径及 storage_managed 存储标记；该标记不代表用户受管所有权 |
| `merge` | 可见目录和来源目录的已登记关系，包含物化的嵌套定义；不枚举目录当前内容 |
| `maps_managed` / `maps_temporary` | 分开返回受管与临时数字规则，保留 target/spoofed ino/dev 和 spoofed_path；不会反查已保存路径或重新解析文件 |
| `inject` | 内部目录注入记录；不是可供独立删除的受管规则 |
| `user_hide` | 登记路径、真实内核 rule ID 和定义 generation；不包含异步绑定健康，健康仍需原隐藏查询 |
| `overlay_xattr` / `kstat` / `vnode` | 已保留名称，但当前不支持；返回 unsupported，不能解释成完整空集合 |

每页最多一条记录，输出有 `page.instance_id`、`page.generation`、`page.cursor`、`page.next_cursor` 和 `page.at_end`。所有 64 位整数使用十进制字符串，调用者不能转成精度不足的浮点数。后续页面使用上次返回的 token 和 next_cursor：

```sh
# 三个变量必须来自同一次有效分页的返回值
/data/adb/ksud sumhp api inventory mapping "$instance_id" "$generation" "$next_cursor"
```

从 cursor 0 开始，保留同一 instance/generation，连续读取至 `at_end:true` 才能组成该类别的完整集合。跨类别核对也必须复用同一 token，并将新类别 cursor 设为 0。控制变更会使旧 token 失效，返回 `stale`/ESTALE；丢弃该次尚未完成的集合后，由调用者明确决定是否重新开始，不自动重放任何修改。超出末尾的 cursor 返回错误。调用者需限制累计记录、字节、用时并允许取消；达到预算时保留“不完整”状态。

顶层 `complete` 只在**单次请求从 cursor 0 读完所选非 metadata 类别**时为真。后续末页的 `at_end:true` 不证明调用者读过前面所有页，也不代表所有类别、运行开关或实际文件视图完整。metadata 中 supported_categories 当前是 `0x7f`，第 n 类对应 bit n−1；未支持类别不能补成空数组。

路径字段采用 `{encoding,value,byte_length}`：有效 UTF-8 原样保留（JSON 转义控制字符），无效 UTF-8 使用 `hex` 保留全部字节。不要 trim、按换行拆分或用替换字符重建路径。查询不会导出内核地址。当前只有 user_hide 和 managed_maps 标明对应受管 owner，其余为 `unattributed`。`generation_ordinal` ID 只标识本类别、本实例、本 generation 的位置；user_hide ID 也是内核运行 ID，与第十三批 F04 的持久定义 ID 分开。所有记录都标记 `mutation_handle:false`，不能据此执行泛化删除或全局 clear。

未协商到新能力时输出 `unsupported`，保留旧文本为 `legacy.raw` 并明确 `complete:false`，即使文本为空也不宣称完整。旧文本读取失败保留 errno。能力不存在的兼容响应退出 0；类别不支持、过期或查询失败退出非零，JSON 仍说明原因。客户端拒绝格式、长度、尾字节、所有权或 token 不符合合同的回包，不覆盖此前有效页面。

新 ioctl 每次只分配固定 8,352 字节页，跨页可返回超过旧 64 KiB 的集合。但当前按序号重新遍历，完整扫描累计可能为 O(N²)，**没有经过设备性能验证**。它只对定义提供一致性，既有 FD、文件替换、来源内容、绑定健康和普通应用观察均不在此快照中。新 ioctl 本身无恢复、重绑或变更记账；完整 CLI 的 daemon 冷启动边界仍同前述限制。

## Manager 中的保存与运行状态

第七批的“运行规则”页可手动读取结构化登记集合，并按类别筛选、搜索和查看详情。默认不在普通刷新时扫描。每次最多 1,024 条记录、4 MiB JSON 和 30 秒预算，跨类别使用同一集合身份并在最后重新核对；不支持的类别保留未知。超预算、过期、失败或取消时已读部分仍可查看，但不会显示“完整空集合”。刷新立即清除上次完整集合状态。

路径以带 JSON 转义的 UTF-8 或明确标注的十六进制显示；详情可复制精确文字或原始字节的十六进制，保留控制字符和末尾空格。序号不提供删除权限，用户隐藏的绑定健康仍去对应页面查询。旧版列表与原始兼容数据始终标为不完整。同步 JNI 请求已经在途时，取消只能阻止后续页面，界面会保留“等待当前请求返回”的提示；30 秒不是强制中断底层传输的承诺。页面重建不会自动重放扫描，完整结果只代表采样结束时核实过的登记集合。

第六批在状态页与设置页接入保存／运行状态面板。按功能查看保存值、内核 configured、active、机制是否可用、未活动原因和最近设置错误；隐藏的登记匹配与绑定健康分开。诊断明细展示 boot、instance、generation、采样区间、摘要和最后操作 ID。

第七批源码：[Maps 身份采样](../../../userspace/ksud/sumhp/src/mount/maps_snapshot.cpp)、[只读查询 worker](../../../userspace/ksud/sumhp/src/mount/query_worker.cpp)、[Manager 分页解析与读取](../../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHInventory.kt)、[运行集合页面](../../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHInventoryRules.kt)；已执行测试与设备限制见[第七批记录](../../plans/SUMH-implementation-validation-batch07-2026-10-16.md)。

Manager 分别协商 state/runtime schema。可选状态读取失败不清掉已读取配置；非零命令中合法的部分状态仍会显示并保留错误。刷新失败后保留的旧采样明确标为过期；历史核实不冒充当前成功。保存值来自另一配置读取，不宣称与状态查询全局原子一致。

支持新接口时，页面依据后端差异提示应用或先查询，结果未知不自动重试；只有旧控制器使用明确标注的页面待确认标记。点击“应用已保存的运行设置”仍执行现有 config apply 流程，涉及其他符合条件的功能及受管 Maps，不保证热重编译模块。完整操作历史、精确重试及所有入口的 typed result 仍在计划中。

第八批已增加精确 `sumh maps preview` 与 Manager 手动预览，独立读取保存路径、当前解析、登记集合和实际提交记录。提交历史由真实 mutation callback 记录实际 payload，区分 latest/confirmed，不由运行 inode 反推。pending 表示未知；同启动自动恢复会暂停，先查询后由用户明确发起新的应用并保留取代关联。历史变化或页面已观察到配置变化时不维持当前比较结论；不自动刷新或应用。定义导入只先产生可审查草稿，选中导出只包含路径。完整格式、等待预算、撤销和边界见[Maps 指南](sumh-maps-rules.md)，测试证据见[第八批记录](../../plans/SUMH-implementation-validation-batch08-2026-10-16.md)。

源码：[UAPI](../../../uapi/sumh.h)、[内核状态与分页](../../../kernel/sumh/control/sumh_ioctl.c)、[分页客户端](../../../userspace/ksud/sumhp/src/mount/sumh_client.cpp)、[分页 JSON](../../../userspace/ksud/sumhp/src/core/rule_inventory.hpp)、[运行状态序列化](../../../userspace/ksud/sumhp/src/core/runtime_status.hpp)、[开关应用与回滚](../../../userspace/ksud/sumhp/src/mount/runtime_apply.hpp)、[Kernel Build 应用](../../../userspace/ksud/sumhp/src/mount/kernel_build_apply.hpp)、[状态采样与存储](../../../userspace/ksud/sumhp/src/core/state_ledger.cpp)、[记录格式与结果模型](../../../userspace/ksud/sumhp/src/core/state_ledger_model.cpp)、[Manager 解析](../../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHControlState.kt)、[状态面板](../../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHControlStatePanel.kt)。验证范围见[第四批记录](../../plans/validation/SUMH-batch-04-validation-2026-10-16.md)、[第五批记录](../../plans/SUMH-implementation-validation-batch05-2026-10-16.md)和[第六批记录](../../plans/SUMH-implementation-validation-batch06-2026-10-16.md)。


## 按 ID 恢复资源结果

第十六批完整隐藏定义导入的每条 `import-id` 也使用同一预登记、私有检查点和资源回执流程。一次导入只提交最多 16 项，剩余项须重新读取与预检；保留草稿不表示会自动续传。丢失回包先按原 operation ID 查询，再检查当前定义，不能因导入列表仍显示该项而直接重发。详见[完整定义交换](sumh-user-hide-definitions.md)。

第十五批新增精确本地只读命令 `operation result <id>`。能力逐项读取：`operation_result:true`、`operation_resource_result_schema:1`、`operation_resource_result_resources:["user_hide_mutation","temporary_maps_mutation"]`（后一类型由第四十二批加入）。Manager 私有结构化通道另宣告 `operation_manager_transport_schema:1`；不能仅用协议大版本推断支持。公开 prepare/execute schema 1 和 show/wait schema 2 保持不变。

**目的与条件：** 在预约的稳定用户隐藏修改或临时 Maps 精确删除回包丢失后，取得该次操作实际捕获的类型化资源结果。需要此前保存的精确 operation ID、保留中的 finished 摘要以及匹配的独立回执。普通旧 CLI、未预约操作、其他资源不会因此自动产生可恢复回执。Maps 的键与 token 还须能重建原请求摘要；历史回执不证明当前集合或观察者视图。

```sh
# operation_id 来自已经确认的 prepare 回包，不能使用“最新同名记录”代替
/data/adb/ksud sumhp operation show "$operation_id"
/data/adb/ksud sumhp operation result "$operation_id"
```

**步骤与期望：** 先 show；prepared/started 时可 wait 最多 5000 ms，再显式 result。查询不启动 daemon，不调用内核，不迁移、修复或写文件。返回 schema 1、resource=`operation_resource_result`，始终 `read_only:true`、`runtime_verified:false` 和 `durability_confirmation:not_observable_from_read`。只有 observed 才含 result 对象；将摘要、资源回执和重新查询的当前规则分别保存。旧 boot 的 observed 是历史证据，不证明这次启动的运行状态。

| status / 退出码 | 含义与处理 |
| --- | --- |
| observed / 0 | 回执与 finished 摘要的 ID、name、request revision、origin boot、exit/errno 全部匹配。读取资源内的 saved、status 和逐项结果；命令成功不代替实际观察者验证。 |
| not_retained / 3 | 摘要缺失或已淘汰；不能证明没有执行。 |
| not_finished / 4 | 预约仍为 prepared/started，没有可确认的最终资源结果。 |
| resource_unavailable / 6 | 已有 finished 摘要，但资源回执未保存、不支持或已被裁剪。不得从摘要补造资源成功。 |
| unavailable / 5 | 账本/回执读取、格式或身份核对失败；保留文件调查。 |
| context_changed_or_unknown / 5 | 读取期间 boot/账本身份变化或无法取得上下文；没有资源 payload，重新查询。 |

控制器在现有 journal 锁内捕获完整类型化回执，先写独立 `operation_resources.json`，再写 finished 摘要。第四十二批存储写入升级为 schema 2，读取兼容 schema 1 的隐藏回执与 schema 2 的隐藏/临时 Maps 回执。首次覆盖 schema 1 前先耐久备份原字节到 `operation_resources.schema1.bak`，读回确认并复核原库未变；冲突或失败保留原库。未来/损坏格式不覆盖。仍最多 16 条、总编码 512 KiB、单封装 56 KiB；不缓存通用 stdout/stderr、原始 argv 或授权数据。旧二进制拒绝 schema 2，降级须保留当前文件并明确转换；旧备份不包含后续操作，不能用它恢复运行状态。回执保存失败不阻止命令摘要正常结束、不回滚或重放目标修改。查询在读回执后再次核对账本和 boot，若发生裁剪/变化则放弃 payload。一次事后读取无法证明之前 fsync 已确认。

**Manager 使用：** 用户隐藏→操作历史→选定 ID→查询结果，或最多等待 5 秒后再查询。第四十三批临时 Maps 也接入同一预约和结果协议：Maps 设置→管理临时 Maps 规则→本地删除历史→按 ID 查询，具体步骤见[Maps 工作台](sumh-maps-rules.md#manager-临时规则工作台)。两者分开保存本地记录；隐藏仍是 SOP1，临时 Maps 为独立 STM1，各自最多 16 条/1 MiB，置于 noBackup 私有目录。

页面分开当时收到的结果、手动查询的命令状态和留存资源回执。查询不自动轮询或 execute，不使旧规则 token 重新有效；关闭历史后需刷新当前规则。进程重建不恢复变更队列，也不清除编辑草稿。临时 Maps 新入口须明确支持 schema 2/temporary_maps 及逐项追踪能力，旧 schema 1 隐藏通道保持原合同；查询失败或不支持不会回退直连删除。服务端回执、本地跟踪及当前运行集合各自有边界，不构成跨文件全局事务。

**失败处理与撤销：** 回执缺失、版本冲突或不完整先核查当前保存配置与运行集合，再显式决定是否发起新操作。服务端处于 started 时不能用再次 execute 解锁。移除本地记录只移除关联，不取消服务器工作；恢复定义需使用当前 revision 的对应资源命令，不使用全局 clear。历史不是配置备份或恢复点。

代码与宿主证据见[第十五批验证](../../plans/SUMH-implementation-validation-batch15-2026-10-16.md)。真实 Android 杀进程、掉电、root/JNI/socket 和观察者验证仍需专用设备。
