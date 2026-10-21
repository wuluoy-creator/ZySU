# SUMH 配置方案与差异预览

第二十六批提供 **当前可用的未启用方案 CLI**：采集、命名、复制、导入、导出、删除及按资源比较。第四十六批将这些流程接入 **Manager 配置方案页**。CLI 独立于管理应用授权的 `ksud profile`，入口是 `ksud sumhp profile`。激活、恢复运行集合和服务端方案操作日志仍是 **拟新增**，不接受 `profile activate` 或 `profile restore`。

基础概念见[模块规则](sumh-module-rules.md)、[用户隐藏定义](sumh-user-hide-definitions.md)、[Maps](sumh-maps-rules.md)、[Kernel Build](sumh-kernel-build.md)和[状态与操作结果](sumh-runtime-state.md)。实现与测试边界见[第二十六批验证](../../plans/SUMH-implementation-validation-batch26-2026-10-17.md)和[第四十六批验证](../../plans/SUMH-implementation-validation-batch46-2026-10-19.md)。

## Manager 配置方案页

在“挂载与隔离”打开“配置方案”，旧 SUMH 配置入口也提供同一页面。进入页面只读取本机草稿、导出快照和待核对记录；点击读取按钮才查询控制器能力与方案库。每个操作单独协商支持情况，读取失败不能当成空库。列表失败时保留上次读取内容并标明状态未知。

保存当前配置前，先读取方案库，填写稳定 ID、名称和备注，审查后捕获。捕获始终包含四类资源，结果只保存到独立未启用方案库。名称可以重复；ID 用于查询和恢复。复制使用新 ID，改名只改变名称与备注，删除只移除选中的方案条目。这些操作均绑定库版本，针对已有条目的操作还绑定方案版本；冲突时重新读取、审查后再发起操作。

重新读取发现库版本、选中方案版本或条目存在性变化时，旧详情及对应比较失效，需要再次打开条目。刷新不替换表单和导入草稿；编辑本机草稿也不会使一次失败查询留下的旧列表重新变为最新。

导入先通过系统文件选择器复制到本机私有草稿，再手动预检；选择的资源决定比较范围，正式导入仍保存完整四资源方案。切换文件或资源后旧预检失效。方案内部隐藏规则和 Maps 的启用值保留，方案本身不会自动启用。比较展示增加、删除、改变和未变计数，另列 Maps 开关差异及隐藏身份冲突；成功预检不表示模块、能力、owner 或运行状态已经验收。

文件格式读取失败会保留原草稿。若草稿已经替换但同步或读回无法确认，页面保留原显示内容，并提供“重新读取本机草稿”；确认后只读取应用私有文件，重新核对文档和资源选择，原预检失效。取消不会替换显示内容。这一步不会查询或修改控制器的方案库。

导出先核对已审查的库版本和方案版本，再固定该份文档，最后打开系统保存位置。文件选择取消或写入失败保留固定文档，重试只保存相同内容。真实文件提供者是否完成持久写入需结合提供者行为判断；本机快照不是活动配置备份，也不包含模块源文件。

页面重建后可以直接重试已有固定导出，不必先读取方案库或重新选择详情。导出卡片显示它自己的 ID 和版本；它可能与当前选中的方案不同。重试不会自动取最新版本，也不会在进入页面时自动打开保存位置。

写操作发出前先保存本机 prepared、dispatching 两个检查点，包含动作、目标 ID、来源 ID、版本和请求摘要。页面关闭、超时或进程重建不会自动重复请求；原生子进程可能继续完成保存。此时先查询已知目标，再明确确认已查看当前状态，才能开始下一次写操作。**当前存在或缺失只是当前观察，不是先前操作的历史回执**；本地检查点也不是服务端操作日志。记录损坏或未来格式会阻止覆盖和继续写入。

大文档使用独立私有文件及 4 KiB 分帧通道，只有完整结束且子进程成功退出才接收结果。输入和输出不进入 shell、命令行大参数或 Android Bundle，也不放宽普通 SUMH IPC 缓冲。专用 `profile manager --channel` 是 Manager 内部协议，不是供脚本直接调用的命令；脚本继续使用下述 CLI。

## 方案包含什么

每份方案的 `resources` 固定包含以下四项；缺项会拒绝导入，空集合表示明确的空配置。预览必须显式选择资源，未选资源不进入差异结果。

| 资源 | 保存内容 | 比较键 |
| --- | --- | --- |
| `settings` | 完整已知设置；采集当前配置时补齐默认值，导入方案不得缺字段 | 设置名 |
| `modules` | 模块 ID、mode、完整路径规则；保留显式 auto 和空规则 | 模块 ID |
| `user_hide` | schema 1 稳定定义、路径、enabled、名称、备注、分组 | 稳定定义 ID，另报同路径不同 ID、同 ID 不同路径冲突 |
| `maps` | `enabled` 开关和受管路径 `rules` | 目标路径，开关单独比较 |

`enable_maps_spoof` 与 `maps_rules` 归 Maps 资源，不再出现在 settings。方案整体未启用，但内部 hide 的 enabled 和 Maps 的 enabled 保留期望值；导入不会把它们全部改成 false。

方案不保存授权凭据、临时 Maps 数字规则、运行 inode、boot marker、当前内核集合或模块源文件。旧方案不等于旧文件备份。未知设置、未来格式、重复 JSON 键、无法按当前严格规则表示的旧模块配置都会报错，不能靠丢字段或截断获得“成功”。

## 先查询版本，再创建

所有入口需要既有 root 授权。采用精确本地 CLI，数据通过绝对文件路径或 stdin 的 `-` 输入，输出为 JSON；不要加全局参数或经 `daemon call`、`operation execute` 包装。命令不会冷启动 daemon 或触发自动恢复。

```sh
/data/adb/ksud sumhp profile list
```

记录返回的 `bank_revision`。库不存在时返回空列表和代表“缺失”的版本，查询不创建文件。方案 ID 是调用者选定的 1–64 字节小写字母、数字、下划线或连字符，首字符必须为小写字母或数字，例如 `before-module-update`；显示名称可以重复，不能代替 ID。

把以下请求存入 `/data/local/tmp/sumh-capture.json`，将版本占位文字换成刚查询的完整值：

```json
{
  "schema_version": 1,
  "id": "before-module-update",
  "bank_revision": "<复制 profile list 返回的完整 sha256-v1:...>",
  "name": "更新模块前",
  "note": "保存配置意图；模块文件另行备份"
}
```

```sh
/data/adb/ksud sumhp profile capture --input /data/local/tmp/sumh-capture.json
/data/adb/ksud sumhp profile show before-module-update
```

capture 前后读取 config、旧模块模式、模块规则及隐藏定义四份文件；检测到变化返回失败，不保存方案。输出 `capture_consistency:double_read_observation`、来源状态和版本，以及 `global_atomic:false`、`runtime_verified:false`。两次相同读取不能排除 ABA、非合作写入或证明跨文件原子性；这只是配置语义的采集。缺失配置采用默认值，无法读取不等于缺失。

## 导出、导入预检和保存

```sh
/data/adb/ksud sumhp profile export before-module-update > /data/local/tmp/sumh-profile.json
/data/adb/ksud sumhp profile import-preview --input /data/local/tmp/sumh-profile.json --resources all
```

export 成功时输出裸方案，顶层仅有 `schema_version:1`、`kind:"sumh.profile"`、name、note、resources。导出前确认输出目标不是 SUMH 的活动配置或方案库；检查命令退出码，失败时输出的是错误对象，不能当作方案导入。

旧 CLI 的 show/export/preview 按 ID 读取当时的内容，没有“此前已审查库版本／方案版本”的输入参数。list/show 后再次 export 可能读到外部修改后的内容，不能据此声称固定了先前审查的版本；Manager 内部版本核对和固定导出提供的是另一项保证。写操作的条件版本校验仍按下文执行。

import-preview 校验完整方案，并与本机已保存配置比较，不保存。然后把裸方案作为下面请求的 `profile` 对象，使用最新 bank_revision 和一个新 ID：

```json
{
  "schema_version": 1,
  "id": "imported-example",
  "bank_revision": "<最新 bank_revision>",
  "profile": {
    "schema_version": 1,
    "kind": "sumh.profile",
    "name": "示例",
    "note": "",
    "resources": "<此处替换成导出文件的完整 resources 对象；字符串占位符不能导入>"
  }
}
```

```sh
/data/adb/ksud sumhp profile import --input /data/local/tmp/sumh-import-request.json
```

同 ID 已存在时拒绝覆盖。导入不会自动激活；即使所有嵌入功能都启用，活动配置和内核仍不变化。不存在的模块或当前未支持的能力不会使定义被丢弃；本批预览统一把模块、能力和运行所有权可应用性标为未评估，不能将差异有效理解为可立即应用。

## 按资源预览

```sh
/data/adb/ksud sumhp profile preview before-module-update --resources settings,modules
/data/adb/ksud sumhp profile preview before-module-update --resources user_hide,maps
```

可选项为 settings、modules、user_hide、maps，或单独 all；不允许空项或重复项。结果显示 add/remove/change、before/after、未变更计数，以及隐藏同路径不同 ID、同 ID 不同路径两种身份冲突。

结果明确包含 `configuration_diff_only:true`、`can_activate:false`、`runtime_evaluated:false`。预览不调用 Planner、不修改挂载，不证明源路径、能力、外部 owner、运行 generation 或观察者视图。暂未实现的激活步骤必须继续使用后续合同；不能把本输出用作应用令牌。

## 复制、改名和删除

先使用 list 获取最新 bank_revision，show 获取源方案的 profile_revision。三个操作都通过完整 JSON 请求提交：

| 操作 | 除 schema_version:1 外的必需字段 |
| --- | --- |
| clone | id（新 ID）、source_id、bank_revision、profile_revision（源方案）、name、note |
| rename | id、bank_revision、profile_revision、name、note |
| remove | id、bank_revision、profile_revision |

```sh
/data/adb/ksud sumhp profile clone --input /data/local/tmp/sumh-clone-request.json
/data/adb/ksud sumhp profile rename --input /data/local/tmp/sumh-rename-request.json
/data/adb/ksud sumhp profile remove --input /data/local/tmp/sumh-remove-request.json
```

clone 保留四类资源和隐藏定义 ID，仅产生独立的新方案；没有继承或方案叠加。rename 只修改名称、备注。remove 只删除方案库条目，不撤销活动隐藏、模块、Maps 或设置。撤销误删可把先前导出的方案用新请求导入；删除前应自行保存所需导出。

## 结果、容量和故障处理

方案库为 `profiles.json`，通过独立私有锁及版本检查串行写入。持锁后先同步固定的 sumhp→ksu→adb 目录层级；任一层失败均不替换方案文件。已确认保存还需耐久替换和读回。不要直接编辑库文件来绕过冲突。

| 状态 | 含义与后续操作 |
| --- | --- |
| saved | 方案库替换、目录同步及精确读回确认；活动配置未保存，运行集合未应用 |
| unchanged | 当前内容已匹配，目录已同步，但没有替换或同步方案文件；不声称新增方案耐久提交 |
| not_saved | 本次没有替换方案库；按 reason 处理输入、容量、冲突或读取错误 |
| unknown | 结果无法确认；先 list/show 已知 ID，不自动重复写入。原生明确返回 `replaced_durability_unconfirmed`／`replaced_readback_unconfirmed` 时表示替换已发生；Manager 传输失败或回执丢失时，替换可能发生也可能未发生 |
| complete | 查询或配置差异完成，不表示应用完成 |

输出失败、断连、namespace 恢复失败或进程退出不能反推“未保存”或“已经替换”。保留调用者 ID，通过 list/show 核对；不要自动重试。库/单项版本不匹配时重新读取并重新审查。损坏、未来版本或不可读取的库保留原文件，不能当空库重建。原始输入不作为 shell 执行，Unicode、尾部空格及入口允许的隐藏路径 CR/LF 保留；模块和 Maps 路径仍禁止 CR/LF。

单方案最多 8 MiB，输入请求最多 8 MiB + 4096 字节；方案库最多 16 份且规范编码总量最多 32 MiB。每方案最多 256 个模块、合计 4096 条模块路径，每模块保持 256 条/56 KiB 编码限制；隐藏保持 4096 条/4 MiB，Maps 保持 256 条/56 KiB，settings 最多 64 KiB。来源文件各按 4 MiB 有界读取，超限拒绝采集；不放大既有 IPC 缓冲。

文件输入限单链接普通文件，拒绝符号链接；stdin 使用一秒有界非阻塞读取，允许普通文件、管道和 socket，并恢复共享描述符标志。输入总预算不等于存储 fsync 截止时间。输出差异最多 24 MiB，超限报错，不截断后继续。上述限制属于方案工作流，不放宽原功能限制。

## 待实施与设备验收

F07 仍为部分实施：选择资源后的激活、同键冲突决策、持久配置/运行集合/重启状态分开恢复和服务端操作日志尚未实现。真实 Android namespace、SELinux、并发进程、文件系统故障、文件提供者、进程死亡、断连与掉电测试也未完成。宿主故障替身、Robolectric 测试和 ARM64 构建不能替代这些证据。
