# 替换运行时内核构建信息

SUMH 的 `kernel_build_spoof` 能力可以替换内核初始 UTS 命名空间中的 Release 和 Version。它改变读取到的字符串，不改变内核代码、模块 ABI 或设备实际支持的 KMI。能力取决于内核能否解析并使用所需 UTS 符号，不能仅凭管理器版本判断。

| 字段 | 读取方式 | 示例 |
| --- | --- | --- |
| Release | `uname -r` | `6.1.75-android14-example` |
| Version | `uname -v` | `#1 SMP PREEMPT Fri Aug 23 03:08:10 UTC 2024` |

示例仅说明格式。Version 应填写完整构建字符串。每个自定义字段最多 64 个 UTF-8 字节，不接受换行或 ASCII 控制字符；用户空间还拒绝 Release 中的 ASCII 空格。配置中空字段表示使用原值，启用时至少填写一个字段。

## 在管理器中设置

在 **设置 → 挂载与隔离 → 标准功能** 中点击 **内核伪装**，即可编辑 Release 和 Version。条目显示当前内核值，编辑框以原始值作为提示；没有独立的启用开关或应用阶段选择。保存后立即应用并持久化，后续开机在 `post-fs-data` 阶段恢复。

留空的字段使用原值，两个字段都清空后保存会恢复原始内核信息并清除自定义值。查询失败时可重试；应用失败时管理器会提示并提供重新应用入口，不能把配置保存成功当作内核应用成功。

旧配置和命令行仍兼容以下开机恢复时机，管理器保存时统一使用默认阶段：

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

`set` 会保存配置并应用，内核运行时设置失败时尝试回滚保存值。`reset` 分别尝试恢复运行时和关闭持久化配置，任一步失败都会报告错误；它可修复非法构建字段，但不会覆盖整体不是有效 JSON 对象的配置文件。

## 配置字段

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
