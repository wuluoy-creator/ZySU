# SUMH 四场景只读诊断

当前源码新增 `doctor` CLI，帮助解释隐藏定义、模块后端规划、受管 Maps 和启动保护状态。它只采集一个场景、输出 JSON，不执行重试、应用或恢复。需要配套控制器及已授权 root shell；Manager 向导仍待接入。代码与实际验证范围见[第三十五批记录](../../plans/SUMH-implementation-validation-batch35-2026-10-18.md)。

## 入口与使用步骤

先查看 `api meta` 中的 `diagnostics_schema`、`diagnostics_scenarios` 及只读/容量描述。使用精确入口 `/data/adb/ksud sumhp doctor --input <绝对文件或->`；不要通过 `daemon call`、全局 `--verbose` 包装或私有 `manager --channel` 调用。

请求 schema 1，一次只选一个场景，最大 4096 字节。下面示例在已有 root shell 运行，只向诊断进程传入 JSON：

```sh
printf '%s' '{"schema_version":1,"scenario":"boot-protection"}' |
  /data/adb/ksud sumhp doctor --input -

printf '%s' '{"schema_version":1,"scenario":"maps-failure"}' |
  /data/adb/ksud sumhp doctor --input -

printf '%s' '{"schema_version":1,"scenario":"module-fallback","module_id":"example.module"}' |
  /data/adb/ksud sumhp doctor --input -

printf '%s' '{"schema_version":1,"scenario":"rule-not-effective","definition_id":"0123456789abcdef0123456789abcdef"}' |
  /data/adb/ksud sumhp doctor --input -
```

模块名和定义 ID 是占位值，必须换成当前查询所得的真实值。隐藏 ID 来自 `hide definitions/status`，不使用内核临时 rule ID。可在任意请求加 `operation_id`，其值来自已有操作记录，不能使用 Maps submission ID 或 module rebuild ID。文件输入要求绝对路径、普通单链接文件，最终路径不能是符号链接；stdin 支持管道、socket 或普通文件。

保存 stdout JSON 和进程退出码，先读顶层 status，再看 evidence、findings、next_steps、unchecked。建议中的 `argv_prefix` 和 `arguments` 是独立参数，不应拼接成未经转义的 shell 代码；本入口从不执行输入文本或自动运行建议。

## 四种场景的期望观察

| 场景 | 目的与适用条件 | 期望观察、失败处理与撤销 |
| --- | --- | --- |
| 隐藏定义未生效 | 已知稳定定义 ID，需要解释保存的 enabled 与登记/绑定状态 | 精确选中定义，不受第一页限制；停用、待移除、全局暂停、绑定失败各自解释。未知或过期先刷新；不把未登记等同普通应用可见。诊断无需撤销，后续显式重试见[隐藏定义指南](sumh-user-hide-definitions.md)。 |
| 模块后端回退 | 已知模块 ID，需要理解当前配置如何选择后端 | 使用生产 Planner，区分全局覆盖、模块配置、探测与回退；这是推断，不证明当前挂载后端。缺项只指当前可挂载集合。重建结果未知先看[重建记录](sumh-module-rebuild.md)，不重放历史计划。查询无需撤销。 |
| Maps 失败 | 已保存受管路径定义，需要检查当前身份、碰撞与历史提交 | 对完整集合计数，最多展示 16 行摘要；余数明确标出。配置 revision、Maps 定义 revision 和历史提交分开；旧成功不证明当前匹配。读取失败保留原配置；显式重新应用及其撤销见[Maps 指南](sumh-maps-rules.md)。 |
| 启动保护 | 挂载被启动保护阻止或计数记录异常 | 当前采样 boot 与记录来源 boot 分开，计数/禁用标记不可读时保留未知。automatic_mount_allowed 只解释恢复存储门禁，不证明安全模式、外部 owner 或后端可用。显式 reset 改变计数/标记，之后重跑诊断确认；它不恢复配置或运行挂载。查询无需撤销。 |

## 如何解释结果

| status / 退出码 | 含义 |
| --- | --- |
| observed / 0 | 有限诊断范围的证据已完整取得；即使发现全局暂停或启动保护，也可能返回此状态，不能当作“功能正常”。 |
| invalid / 2 | 请求或参数非法，未开始诊断；保留原输入，按固定 reason 修正。 |
| partial / 3 | 有可用证据，也有未知、读取失败或不完整部分。 |
| stale / 4 | 采样时配置、boot 或 namespace 等上下文变化；当前结论及操作建议失效，需重新读取。 |
| unavailable / 5 | 主要证据、传输或协议不可用；不等同内核明确不支持。 |

`saved` 表示持久配置，`observed` 表示本次有限查询，`historical` 表示留存记录，`inferred` 表示按输入推断。时间来自 collector 对查询前后的采样，并非所有资源的原子快照。报告中的 `observer_verified:false`、`atomic:false` 和 `unchecked` 必须一起解释。

近期操作最多 4 条。同命令族只是候选；用户指定 operation ID 也不证明该记录确实操作了当前资源。未保留不等于从未执行，成功记录不等于现在生效。未解决或不可读取的相关记录会阻止生成修改建议，仍可按 ID 查询；不会自动重复变更。

报告最多 64 KiB，省略完整路径/备注/计划/日志及原始错误文本，仍含经校验的资源 ID 和运行身份；向他人提供前可自行检查。只读建议带可执行参数，修改建议只有固定说明。配置恢复与当前运行恢复属于不同操作，[未启用方案](sumh-profiles.md)尚不提供 activate。

## 超时、并发与观察者边界

需要留存并审查导出内容时，使用另一个明确入口[私有诊断包](sumh-diagnostic-packages.md)。它会重新采样指定场景并默认替换身份为包内别名；随后预览、导出和删除只访问已保存包。原 `doctor --input` 输出保持原行为，不能因为新增打包功能便将它视为脱敏或匿名报告。

输入等待预算 1 秒，父传输等待预算 8 秒；这不是所有系统调用的硬实时期限。超时或 busy 后不自动重试。后代仍运行时会持有同 network namespace 的独立预约，阻止新诊断执行链；不会清空规则来解除 busy，也不保证关闭终端立即终止整棵进程树。

诊断在 init mount namespace 读取控制面状态，不证明普通应用、授权应用、isolated process 或既有 FD 的实际视图。需要实测时使用[专用观察者工具包](sumh-observer-kit.md)，只处理自有合成资源。当前本地测试和交叉构建不替代 Linux 生命周期运行、Android 设备验证或性能测量。
