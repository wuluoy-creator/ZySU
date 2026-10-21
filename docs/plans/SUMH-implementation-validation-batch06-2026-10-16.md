# SUMH 第六批实施与验证记录

日期：2026-10-16（Asia/Shanghai）。对应[54 项主计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)及[实施账本](SUMH-implementation-progress-2026-10-16.md)。本批推进 C01、C02/C03、K07、F01 的部分范围；不表示全计划或这些任务已完整验收。已有改动保留，未提交、推送或部署。

## 已实现

1. 内核新增独立 `rule_inventory` 能力 bit 20 和 slot 52 查询。固定宽度、固定 8,352 字节页；七类登记定义独立分页，含 instance/generation、cursor、末页标记和支持掩码。非法输入、过期 token、复制或分配错误不改变规则；查询不触发绑定、初始化或控制变更记录。
2. 真实 owner 仅用于受管 Maps 和用户隐藏。其他记录明示无归属；legacy storage_managed 不是所有权。旧记录 ID 只在类别、实例及 generation 内标识位置，不作为删除句柄。overlay xattr/kstat/vnode 明确不支持。绑定健康和文件视图不属于定义快照。
3. 用户态严格验证回包，错误不覆盖旧页面，不自动重试；新增 `ksud sumhp api inventory` 和版本化单页 JSON。64 位值用字符串，路径有效 UTF-8 原样转义、其他字节用 hex，避免行拆分、trim 或替换字节。旧内核保留原始文本并标为不完整。完整性只针对所选类别；末页不能证明调用者收齐前页。
4. Manager 分别协商 state/runtime schema，严格解析类型、重复字段、整数、容量和核实身份。接入实际状态／设置页，展示保存值、configured、active、未知／历史、最后操作和诊断标识。可选查询失败保留已读取配置；非零响应中合法状态与错误同时保留。旧采样明确过期，未知结果先查询，不自动重放。
5. 使用指南补充分页步骤、路径编码、ID/owner、部分结果和撤销边界。主计划保留全部 54 项；F02–F09、F11–F13 未实现命令继续标为草案，不由本批查询接口代替。

## 验证证据

| 检查 | 结果与范围 |
| --- | --- |
| 用户态 SUMH runner | 最终会话 94821，退出 0；16 个程序通过，包含真实 client/header 的分页和 JSON 测试、实际 CLI 参数表。Windows 符号链接 fixture 不可用，未计作通过。日志 `out/batch06-native-final.log` |
| 新内核分页生产函数提取 | 通过；40 条映射、160,000 路径字节、跨桶和跨类别、同目标 MERGE、受管／临时 Maps、Unicode/CR/LF/末尾空格、无损长度、空／末页／越界、reserved/output 字节、token 冲突及内存/复制故障。使用宿主内核 API 替身，不是真实内核执行 |
| 固定 ABI 编译 | 新分页结构 ARM32、ARM64 的大小、对齐、偏移及 ioctl 编码通过；不代表开放 compat ioctl |
| 内核已有相关回归 | 分工复跑 runtime-state、feature-readiness、Maps/Maps-LP64 通过；完整四 KMI 与设备仍未执行 |
| ARM64 Release ksud | 会话 17762，退出 0，生产 CMake 编译与链接通过；未在 Android 上运行产物 |
| 同提交 CI 门禁 | 7 项宿主用例通过；新增 inventory runner 后列表共 28 个 native 入口。本批未运行整套 Linux native 或远端 Actions |
| Manager | 最终全包会话 38365，退出 0；29 类、186 项测试，零失败/错误/跳过。含 17 项新解析测试、9 项生产 Compose 测试及既有回归，真实 StatusTab/SettingsTab 接线参与测试。XML 与中文 2 倍字号窄屏/横屏截图归档 `out/sumh-batch06-manager-full` |
| 独立审查 | 用户态无阻断发现；针对实际实现的最大转义探针通过：结构化 JSON 49,925 字节、保守 envelope ≤58,332 字节；旧文本 JSON 393,682 字节、envelope ≤459,381 字节，均小于既有 1 MiB 响应限制。相同预算检查已加入常规原生用例并再次定向通过。主代理审查 Manager 实际解析/接线并独立查看两张大字体截图，无阻断问题；冗余原始原因码已移入诊断明细 |
| 文档 | 54 项唯一 ID 与账本一致，15 份文档的 191 个本地链接及 git diff --check 通过；Git 仅提示原有换行风格转换 |

用户态专门检查超 64 KiB 累计路径、每页预算、超过 2^53 的身份、无效 UTF-8/Unicode/换行、所有权伪造、长度/尾字节/未使用字段污染、过期成功回包、旧能力回退和 CLI 非法参数。没有向实际内核发送修改请求。

Manager 首轮定向测试因 Robolectric qualifier 顺序使一个大字体用例未能启动，修正配置后通过。首次全包中一个既有 Maps fixture 仍期待旧重试提示且未提供新状态，已改为真实后端 drift fixture，并保留“仅有一个共享应用入口”的检查；最终全包包含该修正和诊断文案调整后全部通过。宿主状态恢复和截图不能替代真机进程终止、TalkBack 或实际观察者验收。

## 仍待实施或验证

- 新查询按序号重扫，完整枚举累计可为 O(N²)。固定页只限制单次内存，不承诺吞吐或延迟改善；调用者仍需累计容量与时间预算。T04 设备性能未测。
- C01 尚缺 overlay xattr/kstat/vnode 类别、Manager 规则列表完整消费、全部用户态集合合同及真实设备矩阵。现有文本展示保留兼容，不能用于破坏性归属推断。
- `api state` 尚未结合新枚举重新解析 Maps 路径、核对源 inode、生产 Planner 或模块 owner。模块、Maps 集合与 xattr 保持未知；不能把路径字符串相同当成 applied revision。
- Manager 面板是控制面采样，保存值另行读取；不是全局原子快照。全入口 typed mutation result、操作 show/wait、端到端期限、完整历史及 ViewModel 工作仍在计划中。
- 新 ioctl 自身只读；完整 CLI 可能涉及 daemon 冷启动和旧能力查询，不能据此宣布 F02 纯预览边界完成。
- 真实内核/KMI 构建、设备生命周期、普通应用/isolated/work profile 观察、TalkBack、性能、掉电以及远端发布门禁均未在本批执行，不标为通过。

源码入口：[分页 UAPI](../../uapi/sumh.h)、[内核查询](../../kernel/sumh/control/sumh_ioctl.c)、[受管隐藏定义](../../kernel/sumh/core/sumh_hide_rules.c)、[用户态客户端](../../userspace/ksud/sumhp/src/mount/sumh_client.cpp)、[JSON 合同](../../userspace/ksud/sumhp/src/core/rule_inventory.hpp)、[Manager DTO](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHControlState.kt)、[生产状态面板](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHControlStatePanel.kt)。实际用法见[运行状态指南](../zh/guide/sumh-runtime-state.md)。
