# SUMH 按路径管理 maps 规则

SUMH 的 maps 路径规则保存两个文件路径：需要匹配的文件，以及提供替换身份的文件。应用时，SUMHP 在 init 挂载命名空间中读取两者当前的 inode/device，将替换文件的路径和身份交给已有的 maps/smaps 过滤器。重启后重新解析路径；提交记录中的旧 inode 只用于诊断，不能作为重放配置。

本功能仅调整 SUMH 既有策略范围内的 maps/smaps 文本，不重定向文件读取，也不改变 stat、文件内容或访问权限。SUMH 虚拟映射本身已有的身份投影仍由原来的 vnode 机制处理。

文件更新后重新应用、区分各来源及撤销步骤见[场景化使用指南](sumh-usage-scenarios.md)。第八批已实现手动路径解析预览、实际提交身份记录，以及 Manager 的定义导入预检和选中导出；第二十九批增加临时数字规则的独立分页查询和条件删除。第三十批增加下述 Manager 临时规则工作台，第四十二批增加可选 CLI 持久删除历史，第四十三批将 Manager 删除接入操作 ID、本地检查点和手动历史查询。F05 完整工作台及设备验收仍在[全面改进计划](../../plans/SUMH-comprehensive-improvement-plan-2026-10-16.md)中。

## 管理器与 CLI

管理器的模块与 Maps 编辑草稿共用应用私有存储，累计最多 512 项 / 4 MiB，不再把累计草稿写入 Activity Bundle。读取失败时暂停编辑并可重试；保存失败保留输入，超限不截断路径，可确认后丢弃选定草稿。外部规则刷新会提示冲突，不直接覆盖草稿。保存确认前进程被终止仍可能丢失未提交输入，未承诺掉电恢复。持久配置的 256 条 / 56 KiB 限制保持不变。

管理器在 SUMH 的 Maps 设置中显示已保存的路径规则，支持添加、编辑、删除和重新应用。编辑目标路径或替换文件只提交一次配置，再整批替换受管规则，不需要先删除旧规则；编辑时不把原规则算作重复项，达到 256 条上限后仍可修改已有规则。各条规则的草稿分别保留，保存失败可继续修改。路径规则需要内核报告 `managed_maps` 能力；已有数字规则入口仍可用于当前运行时。

以下示例中的文件需按设备情况替换，且两者都必须是可解析的普通文件：

```sh
# 检查内核能力
/data/adb/ksud sumhp api features

# 保存规则并尝试应用；相同 target-path 会更新其 replacement-path
/data/adb/ksud sumhp sumh maps add-path \
  /data/local/tmp/maps-target.so /system/lib64/liblog.so

# 查看保存的定义（不表示已经生效）
/data/adb/ksud sumhp sumh maps list-paths

# 独立只读预览：不启动 daemon，不恢复或应用规则
/data/adb/ksud sumhp sumh maps preview

# 开启 maps 过滤并保存开关
/data/adb/ksud sumhp config merge-json '{"enable_maps_spoof":true}'
/data/adb/ksud sumhp config apply

# 文件就绪、替换或重新挂载后，重新解析并应用
/data/adb/ksud sumhp sumh maps apply

# 删除一条保存的定义并重新应用剩余规则
/data/adb/ksud sumhp sumh maps remove-path /data/local/tmp/maps-target.so
```

路径必须是绝对路径，不含 NUL、回车或换行；空格和 Unicode 原样保存。目标路径的 UTF-8 长度小于 4096 字节，替换路径小于 256 字节。最多保存 256 条规则，同时规则数组经双层 JSON 编码后的总大小不得超过 56 KiB，为现有控制器请求的包装及其他设置保留空间；长路径、引号和反斜杠会更早达到总大小限制。同一目标路径只能保存一次；不同目标路径若解析到相同 inode/device（例如硬链接或路径别名），整批应用会失败，避免隐含的覆盖顺序。

配置沿用 SUMHP 的配置文件和原子写入流程：

```json
{
  "enable_maps_spoof": true,
  "maps_rules": [
    {
      "target_path": "/data/local/tmp/maps-target.so",
      "replacement_path": "/system/lib64/liblog.so"
    }
  ]
}
```

## 保存、应用与恢复

保存定义与应用内核规则是两步。CLI 添加/删除成功保存后返回 `saved: true`；`applied` 和新增的 `runtime_result` / `detail` 分别报告本次结果及具体状态。后者区分 `applied`、`failed`、`partial` 和 `outcome_unknown` 等结果。失败保留定义；先查询预览，再决定新的明确应用。列表中的“已保存”不等同于“已生效”。

SUMHP 先解析、校验全部路径，再通过一次 ioctl 原子替换受管 maps 规则。任一路径不存在、不是普通文件、身份重复或内核拒绝请求时，已安装的受管规则保持原样，不会先清空再逐条添加。配置损坏或读取失败也不会被当成空列表。删除后若剩余路径无法解析，旧规则同样保留，应修复路径并重新应用；可保存空 `maps_rules` 后应用以移除全部受管规则。

确认消息丢失与确定拒绝不同：规则可能已经提交，但子进程回包丢失或完成状态未知。此时应用报告 `outcome_unknown` 或提交可能已完成的错误，不保证仍是旧集合，不自动启用、回滚或重放。先检查运行规则和错误记录；旧 ABI 列表不能确认完整性时保留未知结论，不能把失败退出码当作“没有改变”。

开启过滤后的配置应用、boot-completed，以及启动完成后的控制器重启按恢复策略重新解析路径。若同次启动仍有 `pending` 提交记录，自动恢复会暂停，避免覆盖未知结果并重复提交；缺少启动身份、历史损坏或不可读也会阻止自动恢复。先预览当前集合和历史，再明确应用；新的显式提交通过 `supersedes_pending_id` 关联被取代的待确认提交。上一启动的 pending 只作为历史，不永久阻断下一启动。启动前不抢先读取尚未完成挂载的文件；安全模式跳过自动恢复。使用外部 MetaModule 时仍可以恢复 maps 定义，因为它们不负责模块挂载。运行期间文件再次变化后需手动重新应用；本功能不持续监控文件替换。

关闭 Maps 开关会停止过滤并保留定义，配置应用时清除内核中的受管规则，不会因已停用规则的文件缺失而阻止关闭。`maps apply` 遵循保存的开关：开启时重新解析、安装并启用过滤，关闭时停用过滤并清除受管规则。添加/删除路径命令仍会立即尝试更新受管部分，不更改过滤开关。

配置应用、CLI 的 `maps apply` 和控制器启动恢复共用同一应用流程，先解析并安装规则，再启用过滤。普通配置应用中，路径解析或规则替换被确定拒绝时保留原 Maps 开关与规则，不关闭其他已应用的功能。模块重放仍需先重建模块视图；若重建后 Maps 应用失败，保留已重建的模块规则，不二次清空模块。规则替换和开关设置是两个内核请求：若规则替换成功但开关设置失败，新规则已安装，会报告部分完成；完成消息丢失则可能为未知。先预览/查询，再决定新的明确操作，不承诺只重试失败开关。管理器的“应用已保存配置”复用现有配置应用流程。

原有 `maps add <target-ino> <target-dev> <spoof-ino> <spoof-dev> <path>` 仍是临时数字规则：同一 inode/device 再次添加会更新规则。匹配时精确 device 优先于 `target-dev=0` 通配规则，同一匹配键下临时规则优先于受管规则。数字 device 使用内核的 `major << 20 | minor` 编码；路径入口由控制器完成转换。

`maps clear` 清空当前内核中的所有 maps 规则，不删除保存的路径定义；它们会在下次应用或启动恢复时重新安装。受管规则的添加、删除及重试只替换受管部分，保留临时数字规则。

## 规则快照与读取中的更新

第二十五批的内核实现用不可变有序集合替代逐 VMA 的全局互斥锁和链表扫描。匹配仍按“精确设备临时规则 → 精确设备受管规则 → 通配临时规则 → 通配受管规则”的顺序；`target-dev=0` 表示通配。规则排序只影响同一 generation 内的枚举顺序，不产生稳定规则 ID，也不省略被覆盖的定义。

每个已经安装过滤代理的 maps/smaps 文件对象，在第一次有效、非零长度读取时固定当时的规则集合。此后多次短读、不同原始数据块、输出扩容重滤和用户缓冲复制失败重试都使用同一集合；初始为空也保持为空。单纯 open 或零字节 read 不提前固定规则。`dup` 和继承同一文件对象的 FD 共享读取位置与快照；Maps 不支持 seek，查看新规则需要关闭后重新打开并读取。

成功替换或清理当前集合后，已经开始读取的流仍使用旧集合；尚未采样的流使用新集合。该保证只固定 **SUMH Maps 规则**，不固定进程本身的 VMA、文件内容或其生命周期。每次读取入口仍检查当前开关和观察者策略；检查发现关闭或作用范围变化时，原代理标记失效并返回 `EIO`，后续不恢复原生读取。这不是对已经开始的一次 read 的追溯撤权，也不追踪两次读取之间未观察到的关开变化。正常 EOF 保留快照至关闭；终止性流错误会释放，用户复制 `EFAULT` 保留可重试状态。

受管规则仍最多 256 条，保存定义仍受 56 KiB 编码限制。临时数字规则新增 **1024 个不同键**的内核上限，二者合计最多 1280 条；临时集合满时仍可更新已有键，且不占用受管 256 条额度。超过临时键数返回 `ENOSPC`，不驱逐其他定义。

当前集合、准备发布的集合、旧读取流持有的集合，以及 ioctl 输入副本，全部纳入既有 [16 MiB 请求分配预算](sumh-proc-memory.md)。预算或分配失败返回 `ENOMEM`，不发布半套集合。清理全部 Maps 规则无需分配，但旧流占用的快照要等其引用释放才回收；清空受管部分而保留临时部分仍可能需要分配，失败时保留原集合。不要用全局 clear 撤销单项或强行回收其他读者的快照。

这是仓库实现的行为合同；真实 KMI 构建、并发压力和设备 P99/吞吐验收仍待完成，不能据此宣称目标设备已经更快。

## 核对保存路径与运行登记

支持新状态和规则分页接口时，可查询 `/data/adb/ksud sumhp api state` 的 `maps_rules`。控制器在 init 挂载命名空间比较保存路径当前解析出的 inode/device、替换身份及路径与完整受管集合；匹配、差异和未知分开报告。关闭 Maps 时保留定义，只检查受管集合是否为空，不要求保存路径仍存在。硬链接碰撞、文件替换和采样变化也不会被悄悄忽略。该核对不修改规则；完整命令冷启动 daemon 仍遵循既有恢复策略，不能当作 F05 的严格纯预览入口。

`api inventory maps_managed` 与 `api inventory maps_temporary` 分别提供两类登记定义的连续分页查询。临时规则可能覆盖受管规则，因此受管身份匹配不等于实际应用视图已验证。内核分页自身没有原保存目标路径关联；第八批的独立提交记录保存了真正交给 ioctl 的路径及身份，不能从当前运行 inode 反查替代。完整字段、分页方法和采样限制见[运行状态查询](sumh-runtime-state.md)。第二十九批临时规则的精确本地查询和删除见下节。

## 临时数字规则的定向删除

第二十九批控制器提供以下**已实现直连 CLI**，必须在已授权 root shell 中以精确入口执行；不支持全局选项或 daemon call。两条命令都不冷启动 daemon、不触发规则恢复、不读取或保存路径配置。直连删除只影响当前内核集合，不记录持久操作历史。第三十批另有 Manager 专用入口；第四十二批新增下节的可选预约执行流程，普通直连命令的回执保持原样。

```sh
# 第一页；每页最多一条，读取的是临时规则定义
/data/adb/ksud sumhp sumh maps list-temporary

# 将上一页返回的 instance_id、generation、next_cursor 原样作为十进制参数
/data/adb/ksud sumhp sumh maps list-temporary INSTANCE GENERATION CURSOR

# 仅当实际查询到这个临时键并审查其身份后执行；大写参数均为占位值
/data/adb/ksud sumhp sumh maps remove TARGET_INO TARGET_DEV --instance INSTANCE --generation GENERATION
```

分页返回复用 inventory schema 1，`status:known` 才有可用 page；保持同一组 token 读取到 `at_end:true`，任一页 stale 则放弃该次集合并重新查询。`complete` 仅表示从 cursor 0 一次返回了整个类别，不能把后续页的 `complete:false` 当作传输截断。该本地入口的 `boot_id:null` 表示未采集文件系统 boot ID，内核 instance/generation 仍精确传递。旧内核不支持分页时报告 unsupported，`legacy:null`，不读取其他类别的旧文本来冒充临时集合。

删除要求独立内核能力 `temporary_maps_delete` 与 `rule_inventory`；仅协议版本相同不代表支持。请求使用完整无符号十进制，64 位身份作为 JSON 字符串返回，不能先转成浮点数。删除精确匹配 `(target_ino,target_dev)` 的临时条目，**不会按查询时的通配规则回退**；`target_dev=0` 只删除真正登记的通配键。旧数字入口允许的 inode 0 也可删除。同键受管条目保留，删除临时覆盖后可能重新成为匹配项；不存在或只有受管项返回 `temporary_key_not_found` / `ENOENT`。

实例或 generation 变化返回 `snapshot_changed` / `ESTALE`。这里的 generation 是全部受管控定义的控制序列，其他控制操作也可能使它过期；客户端不会自动查询新 token 后重放删除。内核在同一变更锁中校验和准备集合，仅成功发布后推进一次 generation，拒绝不改变集合或 token。原有库存 ordinal ID 仍不是删除句柄。

删除 JSON 的 schema 为 1，`status` 为 `deleted`、`not_applied` 或 `outcome_unknown`。只有 `acknowledged:true` 表示收到内核确认；`saved:false`、`operation_id:null`、`history_supported:false` 和 `observer_verified:false` 明确本次范围。能力查询失败或旧内核不支持不会发出删除；从不回退到 clear。内核确定拒绝如 ESTALE、ENOENT、EINVAL、EOVERFLOW、ENOMEM 保留旧集合；提交后的通信类错误如 EIO/EINTR 按未知处理。

结果未知、输出失败或进程断连时，先独立查询当前集合再决定是否发出新请求；本地退出非零不证明删除未执行。查询和删除都检查输出刷新错误。单项删除通常要分配新的不可变集合，预算不足时保留旧集合；最后一条且无受管条目可发布空集合而不分配。过滤机制当前不可用时仍允许清理。删除不追溯改变已固定的旧读取流；旧快照等最后引用释放后才回收。

撤销删除需先核对当前源身份和原规则，再通过已有数字 add 显式重建该单项；inode 已替换时不能照抄旧身份。新入口没有替换既有全局 `maps clear` 语义，也不能把清空当作撤销或释放别人的旧 FD。源码见[条件删除与查询](../../../userspace/ksud/sumhp/src/mount/maps_temporary.hpp)、[内核集合](../../../kernel/sumh/core/sumh_maps_store.c)及[批次验证](../../plans/SUMH-implementation-validation-batch29-2026-10-17.md)。

## 临时删除的可追踪 CLI 流程

第四十二批已实现可选的 `operation prepare/execute/show/wait/result` 流程。它复用原精确删除；只多记录这次请求及其结果，不保存规则定义、不新增 owner，也不自动重试。第四十三批的 Manager 接入同一追踪流程，见下方工作台说明；旧直连入口仍兼容。

**能力与条件：** `api meta` 宣告 `operation_local_temporary_maps_remove:true`、`maps_temporary_tracked_cli:true`，资源列表包含 `temporary_maps_mutation`，`operation_resource_store_schema:2`。第四十三批另宣告 `maps_temporary_tracked_manager:true` 和 `operation_manager_temporary_maps_schema:2`。原 `maps_temporary_operation_journal:false` 描述普通直连入口。删除仍逐项要求内核的查询与精确删除能力；控制器支持预约不代表内核一定支持删除。

**操作步骤：** 先按 `list-temporary` 完整查询并审查所选键，保留原规则与 instance/generation。在普通单链接文件中保存 schema 1 请求；下列大写字符串均为占位值，须换成该次查询的无前导零十进制字符串。inode/dev 可以是 `0`，instance/generation 必须非零，64 位身份不能转浮点数。

```json
{"schema_version":1,"args":["sumh","maps","remove","TARGET_INO","TARGET_DEV","--instance","INSTANCE","--generation","GENERATION"]}
```

```sh
/data/adb/ksud sumhp operation prepare --input /data/local/tmp/maps-remove.json
# 仅从 reservation_confirmed:true 的成功回包取得并保存 operation_id
/data/adb/ksud sumhp operation execute "$operation_id" --input /data/local/tmp/maps-remove.json
# 中断或未知结果后，只查询事先保存的同一个 ID
/data/adb/ksud sumhp operation show "$operation_id"
/data/adb/ksud sumhp operation wait "$operation_id" 5000
/data/adb/ksud sumhp operation result "$operation_id"
```

文件/stdin 的既有输入容量和校验不变，见[操作协议](sumh-runtime-state.md#提交前取得操作-id)。预约不查询或删除目标；execute 在调用者进程本地执行，无需 daemon，且不发送 daemon 请求或触发恢复。原始 daemon 包装会在消费预约之前拒绝这个命令。只有相同 boot、相同请求摘要、仍保留的 prepared 记录，在 started 耐久写入并读回确认后才允许调用删除。缺失、过期、已开始/已结束的 ID 均不会重新执行。请求摘要绑定原键与 token；另一次查询得到新 generation 时，必须重新审查并明确创建新请求，不能更新原文件后复用原预约。

**结果解释：** 外层仍是 submission schema 1，内层 stdout 的 schema 1 / `temporary_maps_mutation` 回执含真实 operation ID、`history_supported:true`，并始终保留 `saved:false` 和 `observer_verified:false`。history_supported 只表示支持留存，不证明这次留存成功。`acknowledged:true` 表示实际删除得到内核确认；回执留存与 finished 摘要各自可能失败。外层 `command_completed` 不保证资源回执存在，内层 `outcome_unknown` 也不能被外层已结束状态解释成确定拒绝。按原 ID 查询 result，只有 `observed` 才返回与摘要绑定的历史回执。

**失败处理：** begin 写入/读回失败时不调用删除，磁盘却可能已经是 started；finished 写入失败可能已删除。异常或不完整本地结果统一报告未知，保留 ID，不自动再次 execute 或改用直连命令。记录被淘汰、回执缺失或格式未知时，先查询当前临时集合；当前缺失不能证明由哪次操作删除。跨 boot 查询的回执只是历史证据。记录结束不额外采样全配置或内核，不将临时删除写成其他功能的已验证状态。

**存储与撤销：** 回执与隐藏共用 16 条 / 512 KiB 总量 / 56 KiB 单条上限；schema 1 升级前在 journal 锁内备份原字节到 `operation_resources.schema1.bak` 并确认同步与读回。冲突、不可读、未来格式或备份失败均保留原库，可能只有操作摘要而没有本次回执。新旧记录不构成跨文件事务。旧程序无法读取 schema 2；降级需保留当前文件并做明确转换，旧备份不包含升级后的操作，不能直接替换活动库来恢复规则。撤销删除仍需核对当前源身份后明确 add 原单项；删除历史或全局 clear 都不能撤销它。

源码见[提交路由](../../../userspace/ksud/sumhp/src/core/operation_submit.cpp)、[执行捕获](../../../userspace/ksud/sumhp/src/core/command.cpp)、[回执校验](../../../userspace/ksud/sumhp/src/core/operation_resource_model.cpp)和[记录存储](../../../userspace/ksud/sumhp/src/core/state_ledger.cpp)。本机验证与未执行的设备项目见[第四十二批记录](../../plans/SUMH-implementation-validation-batch42-2026-10-19.md)。

## 手动解析预览与提交记录

执行完整的 `/data/adb/ksud sumhp sumh maps preview`，或在 Manager Maps 区域点击手动预览。该精确入口在普通日志初始化和 CLI/daemon 路由前分派，仍要求 root；不保存配置、创建提交记录、规范化、恢复或应用规则。不能加全局参数或通过 `daemon call` 包装成同一只读入口。Manager 使用专用原生传输，无普通 daemon 回退；打开页面或普通刷新不自动预览。

schema 1 的 `kind` 为 `sumh.maps.preview`。整体 `status` 可为 `complete`、`partial`、`unknown`、`stale`；退出 0 只表示成功输出 JSON，不证明全部规则有效。`saved` 是保存定义，`rows` 是本次路径解析，`sample` 是当前登记集合核对，`last_submission` 是历史提交。逐行保留缺失、非普通文件、身份不可表示、硬链接碰撞、当前身份与运行登记差异。关闭 Maps 后仍可查看路径解析，但运行比较按关闭状态检查受管集合为空，不把保留的定义当作应启用规则。

历史记录区分 `latest` 与 `confirmed`：前者可能为 `pending`、`rejected` 或 `acknowledged`，后者仅为最后一次被内核接受的实际 payload。提交前耐久写入 pending；内核接受后写入确认。确认持久化失败时可能已改变运行集合，返回部分完成；子进程完成未知则仍为结果未知。旧内核的空集合兼容成功不伪造确认。前后 generation 只是历史采样，不是提交 token；缺历史、损坏、上次启动记录和变化中的记录均不冒充当前成功。记录不作为重放配置，也不覆盖全局 clear 或第三方修改的完整历史。

预览前后核对保存定义、boot 和提交记录；文件身份采样两次，未固定 inode 生命周期，不能排除 ABA、采样后替换或旧 FD/mmap 继续使用原对象。它不核验文件内容或普通应用实际视图。Manager 已观察到定义或开关改变时，旧结果和迟到结果会标为过期，需手动重读。

定义继续限制为 256 条 / 56 KiB；历史最多 256 KiB，预览原始 JSON 最多 512 KiB，超限报告未知而不截断为完整。专用传输等待预算为 8 秒，Manager 原生调用者等待预算为 10 秒；pidfd 失败、超时或未回收时保留单槽限制，不无限创建任务。跨进程预览 guard 按网络命名空间限制并发。以上不构成系统调用、调度或修改操作的硬实时保证。

guard 当前使用可预测名称的抽象 Unix socket；同一网络命名空间的其他进程若策略允许，也可能抢占该名称而使预览返回忙。忙只表示这次诊断不可用，不证明规则失效，也不触发恢复或清理。私有所有权保护仍需后续加强。

## 定义导入、差异审查与选中导出

Manager 可从文件或粘贴文本读取以下版本化格式：

```json
{"schema_version":1,"kind":"sumh_managed_maps","rules":[{"target_path":"/data/local/tmp/maps-target.so","replacement_path":"/system/lib64/liblog.so"}]}
```

导入先产生草稿，选择合并或替换，再查看新增、删除、变更和同目标冲突；冲突需明确保留已保存值或使用导入值。只有点击“保存”才执行既有配置保存/应用流程，解析和预检不会变更运行状态。保存失败保留输入；保存列表变化后需要重新审查。文件读取和较早保存的完成通知不覆盖后续编辑。

输入文档最多 64 KiB，解析后仍遵循 256 条 / 56 KiB 限制；严格 UTF-8，保留 Unicode 和末尾空格。未来版本、未知字段、重复目标以及运行身份等非定义字段拒绝导入。勾选已保存规则后可复制导出 JSON；只含路径定义，不包含开关、inode、boot marker、授权或提交记录，也不导出临时数字规则。这不是 F07 的完整配置方案。

撤销导入前可丢弃草稿；保存后恢复原定义并明确应用，再分别核对保存、运行集合和观察者。预览与导出本身无需撤销。不要使用全局 clear 撤销一条规则。

源码：[预览控制器](../../../userspace/ksud/sumhp/src/mount/maps_preview.cpp)、[真实提交记录](../../../userspace/ksud/sumhp/src/mount/maps_history.cpp)、[专用传输](../../../userspace/common/maps_preview_transport.cpp)、[Manager 预览解析](../../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHMapsPreview.kt)、[定义交换模型](../../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHMapsTransfer.kt)。验证与限制见[第八批记录](../../plans/SUMH-implementation-validation-batch08-2026-10-16.md)。

## 与参考项目的能力对应

设计参考 [susfs4ksu-module，提交 c6a1c065](https://github.com/sidex15/susfs4ksu-module/tree/c6a1c065efff9eaead049acf529a10bc93d34e75) 的能力与使用场景，未引入其脚本或可执行文件。主要参考其 [boot-completed.sh](https://github.com/sidex15/susfs4ksu-module/blob/c6a1c065efff9eaead049acf529a10bc93d34e75/boot-completed.sh) 中按路径配置和启动恢复的用法。

| 参考能力 | SUMH 中的对应与本轮处理 |
| --- | --- |
| 路径隐藏、等待路径出现 | 复用现有受管隐藏、绑定状态和内核事件重试 |
| 按模块选择路径行为 | 补齐已有模块规则的隐藏入口；模式与完整规则一次原子保存，详见[模块路径规则](sumh-module-rules.md) |
| maps 自定义规则与启动恢复 | 本轮补齐路径定义、持久化、重新解析、原子替换及管理器入口 |
| 重定向、目录合并 | 继续使用 SUMH 的虚拟文件与 vnode 身份投影；通用用户重定向的归属管理不在本轮开放 |
| uname release/version | 复用已有 kernel-build 设置、原值查询与应用阶段 |
| 挂载、statfs、OverlayFS xattr | 继续使用既有视图与挂载控制器 |
| 静态 kstat | 修正既有接口的设备绑定、显式更新重绑定和 RCU 替换；保留零值语义，未新增独立管理入口 |
| cmdline、avc、legacy sus_su | 未为对齐开关数量加入专用实现 |

功能归属保持明确：Maps 路径规则负责 proc 文本，虚拟文件映射负责实际文件视图，显式 kstat 负责独立的属性规则。不会给每条虚拟映射额外生成 kstat 规则，也不会另建用户重定向引擎。现有模块映射表按路径管理，开放通用用户重定向前仍需补齐规则归属、冲突拒绝和模块重放保护。

模块路径规则使用现有模块配置和编译流程，用户隐藏路径继续使用受管隐藏池。规则页区分保存定义与内核记录，已绑定隐藏路径去重展示；不以重复入口或重复计数增加功能数量。参考项目的全局 loop 扫描未直接移植：原生 SUMH 不依赖镜像 loop 挂载，Overlay/Magic 的存储应按控制器已有的挂载归属处理。

既有 kstat 的 `ADD/UPDATE` 共用同一规则存储。提供路径时，以该路径当前解析到的设备和 inode 为准，返回更新后的 `target_ino`；路径缺失或另一条路径规则已占用相同身份时拒绝更新，保留旧规则。未提供路径的数字规则保留 inode 通配语义，精确路径规则优先；数字规则自身不安装 inode hook，只对已经进入现有属性处理流程的文件生效。每次更新发布完整新对象，旧对象在 RCU 读者退出后回收。旧文件仅残留 hook 标记时不会继续套用通用属性投影，删除虚拟映射也不会清掉仍被显式属性规则共用的标记。原 ABI 的零值仍表示保留原字段（时间为零值对），`is_static` 仅保留兼容字段；文件替换后需要显式重新提交，不提供自动跟踪或新的持久化入口。

## 验证

```sh
python3 scripts/ci/test_sumh_maps.py --cc clang
python3 scripts/ci/test_sumh_maps_delete.py --cc clang
python3 scripts/ci/test_sumh_kstat.py --cc clang
python3 userspace/ksud/tests/run_sumh_host_tests.py --cxx g++
cd manager
./gradlew :app:testDebugUnitTest --tests '*SUMH*'
```

主机测试覆盖规则替换失败的原子性、匹配优先级、配置校验、设备编码、路径重新解析、客户端协议和管理器配置流。设备验证还需覆盖 maps/smaps、应用策略范围、冷启动、文件替换后的重新应用、重启恢复、安全模式，以及使用旧内核时的能力提示。主机模拟测试不等于真实内核或设备兼容性验证。

## Manager 临时规则工作台

第三十批在 Maps 设置中增加“临时 Maps 规则”，第四十三批接入持久操作追踪。入口随页面操作权限开放，查询、清理和追踪能力独立协商，不要求 Maps 开关开启或过滤代理当前可用。打开页面只读取应用私有历史，不查询控制器或内核；点击“刷新规则”后读取能力和第一页。可明确加载下一页，最多保留 1024 条及 4 MiB 回包。部分列表、完整列表与历史列表分别标注；错误或过期不会显示成空集合。

在新读取、内核支持删除且控制器明确支持追踪的列表中选择一条，核对目标 inode/device、替换 inode/device 和登记路径，再确认删除。无效 UTF-8 路径以原始十六进制显示；该路径是登记文本，不证明当前文件身份。新 Manager 对缺少追踪能力的旧控制器保留查看、提示更新；能力未知时禁止删除，不静默改用旧直连入口。

确认后依次取得 prepared 操作 ID 和原始 boot、确认应用私有 prepared 检查点、确认 dispatching 检查点，然后 execute 一次；任一检查点不确定均不执行。响应完整性、精确键、instance/generation、请求摘要、ID 和 boot 必须匹配。删除成功、明确拒绝、回包丢失或超时后，所有旧 token 均停止用于操作，必须显式刷新；当前条目缺失不能证明由哪次操作删除。

展开“本地删除历史”，按 ID 选择“查询本次操作”或“最多等待 5 秒”。这只调用 show/wait/result，不执行或预约删除，也不会刷新当前列表。界面分开呈现当次收到的响应、手动查询的命令状态和保留回执；command_completed 本身不能证明删除成功。跨启动的匹配回执只说明原启动的历史，原始启动与最新查询启动分别显示。回执缺失、淘汰、未结束、查询失败保留对应状态；没有自动轮询或重试删除。

旋转、关闭重开或后台完成不重放操作，Activity 存续期间保留选择和结果；明确返回列表不会被迟到结果重新打开。进程重建只加载私有操作关联和“需要查询”提示，不恢复整份列表、token 或队列。历史使用独立 STM1 文件，置于 `noBackupFilesDir/temporary-maps-operations`，最多 16 条/1 MiB；不改变隐藏的 SOP1 格式。仅已确认成功的类型化记录允许自动腾出一个位置，未完成/未知记录满额须明确处理。未来/损坏格式不覆盖；写入不确定时可能留下旧检查点或已替换的新检查点，需重新读取，不清空重置。两个检查点都确认前不执行删除；真实设备耐久性仍需验收。

“忘记本地记录”需单独确认，仅删除应用本地关联和回执，不取消操作、不删服务端历史、不恢复规则。需要后续 CLI 查询时先复制 ID；确认删除前仍应保留原条目，历史只保存四个执行选择字段及关联证据，不保存完整替换路径/身份作为重放配置。恢复规则须核对当前身份后明确添加单项，全局 clear 不作撤销。

Manager 复用固定的 Operation 原生通道，新增 schema 2 且必须指定 temporary_maps 资源；schema 1 仍仅允许既有隐藏流程。`api meta` 分别宣告 `maps_temporary_tracked_manager:true` 和 `operation_manager_temporary_maps_schema:2`，旧 transport schema 1 不变。内部 manager/channel 参数不是公共脚本命令；旧 Manager/旧直连 CLI 兼容入口保留。源码见[工作流](../../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHTemporaryMapsOperations.kt)、[追踪器](../../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHTemporaryMapsOperationTracker.kt)、[类型化模型](../../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHTemporaryMapsOperationModels.kt)和[第四十三批验证](../../plans/SUMH-implementation-validation-batch43-2026-10-19.md)。普通应用与 root 的实际视图仍需分别验证。
