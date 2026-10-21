# 替换运行时内核构建信息

SUMH 的 `kernel_build_spoof` 能力可以替换内核初始 UTS 命名空间中的 Release 和 Version。它改变读取到的字符串，不改变内核代码、模块 ABI 或设备实际支持的 KMI。能力取决于内核能否解析并使用所需 UTS 符号，不能仅凭管理器版本判断。

| 字段 | 读取方式 | 示例 |
| --- | --- | --- |
| Release | `uname -r` | `6.1.75-android14-example` |
| Version | `uname -v` | `#1 SMP PREEMPT Fri Aug 23 03:08:10 UTC 2024` |

示例仅说明格式。Version 应填写完整构建字符串。每个自定义字段最多 64 个 UTF-8 字节，不接受换行或 ASCII 控制字符；用户空间还拒绝 Release 中的 ASCII 空格。配置中空字段表示使用原值，启用时至少填写一个字段。

## 在管理器中设置

管理器的 SUMH 隔离设置提供 **Spoof Kernel Build** 卡片，显示保存的配置、运行时状态及原值。保存定义后立即尝试应用；关闭会尝试恢复本次启用前记录的原值，保留有效草稿。查询失败时应查看错误并重试，不能把配置保存成功当作内核应用成功。

编辑时可以使用“使用原始内核版本”和“使用原始构建版本”分别填充对应字段；“使用系统构建日期”仅更新 Version，保留 Release 与生效时机。所有辅助按钮只修改草稿，点击保存才会应用。也可以将单个字段留空，使该字段继续使用原值；启用时至少保留一个非空自定义字段。

控制器的 `kernel-build show` 会读取 `ro.build.date`，在识别到原始构建前缀和日期格式时返回 `suggested_version` 与 `suggestion_source`。管理器仅在来源是 `ro.build.date` 且建议通过长度及字符校验时启用日期按钮。该日期来自当前 ROM，不代表原厂内核的真实编译时间。无法识别、缺少属性或超过长度限制时显示无有效建议，仍可手工填写；读取失败时原值按钮也不可用，可以重试读取。

保存前的预览分别显示 Release 和 Version 将报告的值，以及它们是否与当前运行值相同；下方保留原值与当前值的对照。“重新读取内核信息”更新对照数据，不覆盖尚未保存的草稿。从原始 Release 识别出的 KMI 标识单独展示，不随草稿或当前替换字符串改变；它是版本字符串解释结果，不是 ABI 探测，也不能单独用于选择待刷入的内核。

已保存配置发生外部变化时，未编辑的字段自动更新；仍有未保存输入的字段显示最新保存值，并提供“使用已保存值”或“保留草稿”的明确选择。Release、Version 和生效时机分别处理，不会因另一个字段变化而丢弃输入。保存请求尚未得到读回确认时保留草稿；如果确认的是较早提交，之后的新输入仍保留。选择保留草稿或填入原值本身都不会保存。首次启用确认后，编辑器也会保留仍在编辑的内容。

这些行为已通过宿主 Compose/SavedState 回归，真实设备旋转、进程终止及 TalkBack 仍待验证，见[第五批验证记录](../../plans/SUMH-implementation-validation-batch05-2026-10-16.md)。保存值读回只确认配置，不替代内核运行状态验证。

开机恢复时机由配置控制：

| 值 | 行为 |
| --- | --- |
| `post-fs-data` | 默认值，开机早期应用，启动完成时再次恢复或重试。 |
| `boot-completed` | 启动完成时应用，早期阶段不启用替换。 |

手动保存、CLI `set`、`config apply` 仍立即应用。恢复流程独立于模块挂载后端；安全模式会跳过，SUMH 接口不可用或不兼容时也不能完成恢复。

## 命令行

在已授权的 root shell 中运行。下面使用完整路径，避免依赖 PATH：

```sh
/data/adb/ksud sumhp sumh kernel-build show
/data/adb/ksud sumhp sumh kernel-build set \
  '6.1.75-android14-example' \
  '#1 SMP PREEMPT Fri Aug 23 03:08:10 UTC 2024'
/data/adb/ksud sumhp sumh kernel-build set default \
  '#1 SMP PREEMPT Fri Aug 23 03:08:10 UTC 2024'
/data/adb/ksud sumhp sumh kernel-build set '6.1.75-android14-example' default
/data/adb/ksud sumhp sumh kernel-build reset
```

`set` 的两个参数中，`default` 表示该字段使用原值；两个字段都恢复应使用 `reset`，不能使用 `set default default`。JSON 配置以空字符串表示原值，字符串 `"default"` 没有特殊意义。

`show` 返回 JSON：`enabled`、`release`、`version` 表示当前状态，`original_release`、`original_version` 表示原值。`suggested_version` 是日期辅助填充值，`suggestion_source` 为 `ro.build.date`；无建议时这两个字段为空字符串。

`set` 先保存期望配置，再尝试应用。内核设置失败、结果未知或运行值已回滚时，控制器不会盲目写回旧配置；新保存的意图仍可能在后续 `config apply` 或开机恢复时应用。需要撤销该意图时，应明确保存原设置或执行 `reset`，然后分别检查保存与运行结果。

`reset` 分别尝试恢复运行时和关闭持久化配置。无法解析配置不会直接阻止运行恢复尝试；但能力或原值查询失败仍可能使运行恢复无法执行。它可修复已知的非法构建字段，不会覆盖整体损坏、未来格式或存在未知字段的配置。某一部分失败时，另一部分仍可能已完成。

到达应用阶段后，`set/reset` 的 JSON 包含以下字段；早期能力、输入或配置错误可能只返回 stderr 和非零退出码。

| 字段 | 含义 |
| --- | --- |
| `ok` | 本次命令是否满足其成功条件；不能替代下面各阶段结果。 |
| `enabled` | 本次请求的启用或停用方向，不是独立运行查询。 |
| `saved` | 本次期望配置是否保存成功。 |
| `applied` | 是否完成本次运行设置并读回确认。 |
| `runtime_result` | 如 `applied`、`deferred`、`failed`、`rolled_back`、`rollback_failed` 或 `outcome_unknown`。 |
| `detail` | 运行阶段的原因与边界说明。 |

例如，能力不可用时 reset 可能已保存停用意图，但返回 `applied:false`、`runtime_result:"deferred"`；此时不能把 `enabled:false` 或 `ok:true` 当作本次恢复了原字符串的证据。返回 `outcome_unknown` 时先查询状态和错误记录，不自动重复变更。

新内核同时支持运行状态查询和 Kernel Build 条件设置时，控制器按 instance/generation 查询、设置和读回完整的 Release/Version 对。只有确认属于本次写入的状态才允许条件回滚；并发变更、确认丢失或读回不一致时不会无条件覆盖其他写入。旧内核仍进行两字段读回，但不执行无法证明归属的回滚。这只协调 SUMH 写入口，不能排除其他内核组件直接修改 UTS 字符串。`show` 的多项信息也不承诺是跨读取原子快照。

上述显式 CLI 接线已在当前源码中实现；最终集成测试记录见[第五批验证记录](../../plans/SUMH-implementation-validation-batch05-2026-10-16.md)，未执行的设备检查不算通过。

## 配置字段

原值记录、单字段实验及恢复步骤见[场景化使用指南](sumh-usage-scenarios.md)。配置保存、运行时应用和恢复阶段应分别检查，不以构建字符串选择实际 KMI 产物。

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `enable_kernel_build_spoof` | `false` | 是否启用。 |
| `kernel_build_release` | `""` | Release，空值使用原值。 |
| `kernel_build_version` | `""` | 完整 Version，空值使用原值。 |
| `kernel_build_apply_stage` | `"post-fs-data"` | 后续开机恢复时机，也可为 `"boot-completed"`。 |

启用状态和自定义字段放在同一次更新中，再显式应用：

```sh
/data/adb/ksud sumhp config merge-json '{"enable_kernel_build_spoof":true,"kernel_build_release":"","kernel_build_version":"#1 SMP PREEMPT Fri Aug 23 03:08:10 UTC 2024","kernel_build_apply_stage":"boot-completed"}'
/data/adb/ksud sumhp config apply
```

运行时清空 SUMH 规则也会重置此功能，但不等同于关闭持久化配置；后续 `config apply` 或开机恢复可以重新应用已保存值。

## 生效范围与验证

实现位于 [sumh_kernel_build.c](../../../kernel/sumh/features/sumh_kernel_build.c)。它修改初始 UTS 命名空间，影响使用该命名空间的进程所读取的 `uname`、`/proc/sys/kernel/osrelease`、`/proc/sys/kernel/version` 及 `/proc/version`，不是按 UID 生效的规则。

已有独立 UTS 命名空间不会同步修改；启用期间新建的命名空间可能复制修改后的值，关闭也不会回写这些副本。已经生成的启动日志不在修改范围内。

ksud 的镜像修补使用原始 Release 接口来自动判断 KMI。不能把查询失败简单回退为当前伪装字符串；明确不支持接口时才允许使用普通读取路径，权限或 seccomp 等不明确错误可能导致自动检测失败。

设备验证时先保存以下输出，再分别检查设置、仅改一个字段、恢复、两种开机时机和重启后的结果：

```sh
uname -r
uname -v
cat /proc/sys/kernel/osrelease
cat /proc/sys/kernel/version
cat /proc/version
```

同时检查普通应用和 root 读取结果、UTF-8 长度边界、非法输入、原值读取及不支持能力时的行为。成功替换字符串不代表其他检测或完整性校验能够通过。

仓库已有主机回归入口，从仓库根目录运行：

```sh
python3 scripts/ci/test_sumh_kernel_build.py --cc clang
python3 userspace/ksud/tests/run_sumh_host_tests.py --cxx g++
```

[SUMH 工作流](../../../.github/workflows/sumh-tests.yml) 调用这些测试。内核测试使用 UTS/锁替身，用户空间测试使用模拟内核接口，覆盖状态、配置、CLI、恢复和原值读取等逻辑；它们不能替代完整内核构建、实际设备启动或 UI 操作验证。本页不声称这些验证已经在当前设备或当前提交上完成。
