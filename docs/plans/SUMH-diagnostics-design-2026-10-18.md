# SUMH 四场景只读诊断设计

日期：2026-10-18。对应[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)的 F08/M04。本文保存第三十五批静态检查与接口决策；执行结果另存[第三十五批验证记录](SUMH-implementation-validation-batch35-2026-10-18.md)。

## 范围与源码依据

第一阶段实现 standalone ksud CLI 的单场景诊断，复用现有生产查询，不接入 Manager、不执行修复、不落盘诊断历史。F08/M04 仍部分实施。控制器诊断不证明普通应用、isolated process、已有 FD 或跨后端的实际可见性。

| 场景 | 复用来源 | 必须保留的语义 |
| --- | --- | --- |
| 隐藏规则未生效 | `core/user_hide.cpp` 的类型化 `query_status` 与完整定义存储 | 用稳定 ID 找到精确 cursor/revision；缺失前后复读，不能因第一页没有目标就判不存在。登记、绑定和应用可见性分开。SUSPENDED 是全局暂停。 |
| 模块后端回退 | `mount/module_preview.cpp` 的真实 Planner、`mount/module_rebuild.cpp` 的历史及 intent | Planner 决策属于当前输入推断，不是 live backend；缺模块仅指当前可挂载集合。历史 admission 不授权热应用。 |
| Maps 失败 | `mount/maps_preview.cpp` 的路径解析、集合核对与提交历史 | config_revision、maps_revision 和历史 definition_revision 各有范围；历史成功不能证明当前身份，提交 ID 不是 operation ID。 |
| 启动保护 | `mount/recovery.cpp` 的直接只读存储查询 | 旧无版本响应通过专用适配器；记录所属 boot 与本次采样 boot 分开。存储门禁不能替代安全模式、外部 owner 或后端判断。 |
| 最近操作 | `core/operation_query.cpp` 的 List/可选 Show | 不自动 wait、读取每项 result 或重放；同命令族只算候选关联，未保留不证明从未执行。 |

## 请求与报告合同

公开命令为 `ksud sumhp doctor --input <绝对文件或->`，请求 schema 1，最多 4096 字节。四种 scenario 分别为 `rule-not-effective`、`module-fallback`、`maps-failure`、`boot-protection`；前两种分别要求稳定 definition_id、module_id，所有场景可选 operation_id。重复键、未知字段、未来版本、错误类型、非法 UTF-8、解码后的 NUL 和超限都拒绝，失败保留调用方请求。

报告 schema 1，紧凑 JSON 最多 65536 字节；不截断 JSON。状态与退出码固定为 observed/0、invalid/2、partial/3、stale/4、unavailable/5，表示有限范围内证据完整度，不能视为功能健康度。证据分类为 saved、observed、historical、inferred；缺失或 null 布尔值不得变成 false。当前 boot/namespace 不明或变化时，当前推断与操作建议失效。

每份最多 16 条 evidence、32 条 findings、16 条 next_steps、4 条近期操作摘要、16 条 Maps 行摘要；Maps 对完整集合计数并披露 omitted_count。报告省略路径、备注、完整计划、日志、凭据和原始错误文本，保留校验后的 ID、固定原因及范围。只有 read_only_query 建议带独立 argv 参数；显式修改建议只有说明。所有建议引用证据，不提供“修复全部”、全局 clear 或自动恢复。

## 进程与期限边界

公开入口只读输入并请求专用子进程。固定执行 `/proc/self/exe`，argv0 必须为 `ksud`：生产 multi-call 启用 BusyBox 时，argv0 使用该路径会把 basename `exe` 当作 applet。精确私有参数是 `doctor manager <严格 JSON> --channel`。通用 parser、包装调用和 daemon 不接受该诊断入口。

父进程在 spawn 前绑定独立 abstract socket `sumh.doctor.sample.v1`，在本 network namespace 内保留一个诊断执行链。源描述符全部 CLOEXEC、复制至 FD 4 以上，再映射 child channel→0、reservation→3；不清除父描述符的 CLOEXEC。子进程在 READY 前检查 FD3 的 AF_UNIX、精确名称/长度和 SOCK_STREAM，并为将来的 exec 设置 CLOEXEC。先传 pidfd、收到 ACK 才运行 collector；不按数字 PID 回退终止。

collector 进入并核对 init mount namespace，记录 dev/ino、boot 和单调时钟起止，再调用一个主来源；模块场景另读重建记录，各场景另读操作摘要。它不是 query_worker callback，Maps/Planner 的现有 helper 不会形成递归 query mutex。结束再次核对 namespace 与 boot。

输入等待预算 1 秒，父传输等待预算 8 秒。阻塞的文件系统/系统调用和不可中断进程不具备硬实时终止保证；父进程取消也不表示整棵进程树已退出。fork 的 sampler 后代继续持有 reservation，即使父或 collector 退出，也阻止同 network namespace 新建执行链，直到最后引用释放。不同 network namespace 不共享该限制。

Manager 的 `/proc/self/exe` 是 app_process，不能复用 standalone 自执行入口。后续接入必须使用已验证的打包 ksud 路径及独立窄接口，本批能力明确返回 diagnostics_manager=false、diagnostics_repair=false。

## 验收设计与保留工作

针对严格输入、真实生产输出、稳定 ID 跨页选中、未知/过期状态、历史范围、容量和无修改调用做主机验证；传输覆盖关闭 stdio、FD3 冲突、失败清理、READY/ACK 门禁、BusyBox 分派反例。另提供 Linux 实际 exec/fork、父退出及后代保留 reservation 的同步测试。编译通过与运行通过分别记录。

Manager 四类向导、可选恢复点关联、按功能重试/配置恢复/重置确认、设备观察者、真实并发与性能仍待实施或验收。C06 与 F06/F11/F12 的 owner、事务、真实内核门禁继续保留。
