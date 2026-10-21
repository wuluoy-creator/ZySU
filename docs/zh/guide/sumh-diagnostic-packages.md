# SUMH 私有诊断包

当前源码提供单场景诊断包 CLI 和 Manager 入口：显式采样并保存，预览留存内容，按 revision 导出同一份内容，最后删除选中的本地记录。默认移除或替换诊断中的身份信息。CLI 需要配套控制器和已授权 root shell；Manager 使用随应用打包的配套 ksud，并逐项读取能力。

诊断包基于[四场景只读诊断](sumh-diagnostics.md)，可用于整理隐藏规则未生效、模块后端回退、Maps 失败或启动保护的证据。每个包只包含一个场景的有限诊断数据。源码与验收进度见[第三十八批记录](../../plans/SUMH-implementation-validation-batch38-2026-10-19.md)；主机模型测试不代表 Linux 生命周期、Android 真机或性能测试通过。

## 隐私范围

默认 `privacy.policy` 为 `redacted`。模块、隐藏定义、操作、submission/rebuild、boot、namespace、规则及 revision 等经选择的身份，使用带类型的包内顺序别名，例如 `module-1`、`boot-1`。同一包中同类型、相同原身份保持相同别名；不同包中的同名别名不保证是同一个资源。包中不保存反向映射，也不以原始身份的再次散列代替脱敏。

保留的信息包括固定状态和原因码、证据分类、布尔值、计数、分组关系和有限建议代码。时间使用有效的相对偏移或时长，不能计算时明确省略。路径、备注、原始日志、凭据、环境、命令参数、命令说明、原始单调时间和未知字段均不进入包。脱敏后的建议不包含可执行命令；需要真实参数时，在设备本地重新查看原始 `doctor` 结果。

创建时可以显式选择 `--include-identifiers`，将 policy 改为 `identifiers_included`，保留该诊断投影允许且校验过的身份。它仍然不加入路径、日志、凭据或命令参数，也不是完整系统报告。隐私模式写入包，创建后不能原地切换；需要另一种模式时，应使用新 ID 显式创建另一份包。

`redacted` 表示按此规则处理，不保证匿名。场景、数量、状态组合及包内关系仍可能具有识别性。分享前应检查完整预览，决定是否需要提供这份数据。CLI 向 stdout 导出；Manager 通过系统文件选择器保存副本，不会自动分享或发送给他人。目标文件由用户选择的文件提供者管理。

## Manager 操作

入口为 **Manager → 诊断 → SUMH 诊断包**。打开页面只读取应用私有的恢复检查点，不采样系统、不自动查询控制器。点击“读取能力与已保存的包”后，各操作只在控制器明确声明对应能力时开放；读取失败不等于不支持或空列表。

1. 选择启动保护、Maps 失败、模块后端回退或隐藏规则未生效。后两者填写相应模块 ID 或隐藏定义稳定 ID；操作 ID 为可选关联条件。默认使用包内身份别名；选择保留身份时还需确认本次创建。
2. 点击创建。应用先在 `noBackupFilesDir` 的专用子目录中持久保存调用方包 ID，再请求独立采样和留存；无法确认检查点写入时不发起变更。包已保存与诊断证据完整是不同结果。
3. 创建或删除之后，使用“查询这个诊断包 ID”，检查当前存在或不存在的结果，再点击“确认查询结果并继续”。这也适用于响应丢失或进程重建；查询不会重新执行变更。当前不存在不能证明从未创建、删除或导出。
4. 第 3 步查询到包时，页面已提供该包的当前预览，可直接展开全文；也可以再次点击“读取能力与已保存的包”，然后从刷新后的列表选择预览。检查隐私模式、诊断状态、字节数和内容版本，确认已检查后点击“保存副本…”并选择目标位置。写入前会带已审查 revision 再查询，并比较完整 UTF-8 字节；不一致或无法确认时不写入。
5. 删除时确认所选包 ID 和本地删除范围。应用使用已预览 revision 条件删除，再按第 3 步查询；需要更新列表时手动读取，旧列表行不会随删除自动刷新。删除本地包不会删除任何导出副本。

操作由 Activity 状态层持有，返回页面可以查看后台结果。旋转保留进行中的任务；进程重建只恢复有界表单和本地待查询 ID，不恢复报告内容、预览确认或变更执行权限。文件选择器返回时如果已丢失被审查内容，需要重新预览并重新选择保存位置。检查点不包含请求正文、报告或可重放命令，也不会进入自动备份。

同时打开多个页面时，创建/删除、恢复查询和确认由同一进程内的协调器保护；占用期间另一页面立即返回忙碌，不排队执行。检查点采用条件登记和条件清除，旧页面不能覆盖或清除新的待查询 ID；发现记录变化会重新读取该身份并撤销旧查询确认。该机制针对应用同一进程的 Activity，不宣称抵抗其他特权进程修改私有文件。

取消选择位置不导出；校验、写入、flush 或 close 失败时原包保留，目标提供者可能已产生空文件或不完整文件，应检查或手动删除。应用不创建公共缓存副本，不调用完整 bugreport，不自动打开分享菜单。界面的导出成功表示目标输出流写入和关闭完成，不承诺远端提供者已经同步完成或断电耐久性。

Manager 的接口、生命周期及验证边界见[第三十九批记录](../../plans/SUMH-implementation-validation-batch39-2026-10-19.md)。Kotlin 核对响应归属、版本、隐私元数据、精确字节数和摘要；完整嵌套包的 schema 与脱敏投影仍由原生存储和模型验证。主机模拟界面与状态测试不能替代 Android 文件提供者、SELinux、TalkBack 或真实进程终止验收。

## 入口、输入和容量

精确入口如下，参数顺序固定：

```text
/data/adb/ksud sumhp doctor package list
/data/adb/ksud sumhp doctor package create <id> --input <绝对文件或-> [--include-identifiers]
/data/adb/ksud sumhp doctor package preview <id>
/data/adb/ksud sumhp doctor package export <id> --revision <package_revision>
/data/adb/ksud sumhp doctor package remove <id> --revision <package_revision>
```

`id` 由调用方提供，为 32 位小写十六进制字符串。它标识留存包，不是 definition、operation、submission 或 rebuild ID。每次新采样选择未使用的新 ID；已有 ID 不可覆盖。`package_revision` 是 `sha256-v1:` 加 64 位小写十六进制字符，取自本次预览，不应使用配置 revision 替代。

create 使用与原始 doctor 相同的 schema 1 请求，一次一个场景，输入最多 4096 字节，源诊断最多 64 KiB。文件输入须为绝对路径的普通单链接文件，最终路径不能是符号链接；`--input -` 支持 stdin。场景选择、模块 ID、definition ID 和可选 operation ID 的约束见[诊断指南](sumh-diagnostics.md)。输入是数据，不进行 shell 展开或执行。

留存容量为最多 8 个包、每个精确导出 payload 最多 96 KiB（包含末尾 LF）、整个 bank 最多 1 MiB。到达容量后拒绝新建，不自动淘汰旧包。写入使用一个固定、最多 1 MiB 的 staging 文件，因此不能把 bank 的 1 MiB 上限解释为全过程总磁盘占用上限。

## 创建、检查和导出

下面示例在已有 root shell 中运行。包 ID 是占位示例；新建前换成调用方自行保存的、未使用的 ID。

```sh
PACKAGE_ID=0123456789abcdef0123456789abcdef

printf '%s' '{"schema_version":1,"scenario":"boot-protection"}' |
  /data/adb/ksud sumhp doctor package create "$PACKAGE_ID" --input -
```

创建会先检查该 ID 是否已留存，再通过已有诊断采样通道取得报告。创建 receipt 的 `saved:true` 只确认诊断包已保存；同时检查 `diagnostic_status`，它可以保留源报告的 `partial`、`stale` 或 `unavailable`。成功保存不意味着问题已修复，也不意味着证据完整或当前生效。

如果本次确实需要保留允许的身份，使用新 ID 显式创建：

```sh
printf '%s' '{"schema_version":1,"scenario":"maps-failure"}' |
  /data/adb/ksud sumhp doctor package create fedcba9876543210fedcba9876543210 \
    --input - --include-identifiers
```

然后预览实际留存包：

```sh
/data/adb/ksud sumhp doctor package preview "$PACKAGE_ID"
```

预览成功返回 `status:observed`，其中：

- `package` 是解析后的完整包，包含 `privacy` 和 `diagnostic`。
- `export_bytes` 是将被导出的完整字符串。JSON 展示时换行和引号会转义；这不是删减后的摘要。
- `export_byte_count` 是该字符串的 UTF-8 字节数。
- `package_revision` 覆盖精确 `export_bytes`：规范化包 JSON 加一个末尾 LF，没有额外前缀。

读取完整包和隐私模式后，记录预览返回的 `package_revision`。导出只读取这份包，不重新采样、不再次脱敏、不改变时间或身份。revision 不匹配时返回冲突，不输出包内容。

```sh
# 替换为刚才预览返回的完整值。
PACKAGE_REVISION='sha256-v1:这里替换为预览返回的64位小写十六进制值'

/data/adb/ksud sumhp doctor package export "$PACKAGE_ID" \
  --revision "$PACKAGE_REVISION"
```

成功导出的 stdout 直接是 `export_bytes`，不带 receipt 外壳。需要落盘时，由调用方选择已存在、root 所有、0700 权限的私有目录和新的目标文件，并在重定向前设置 `umask 077`。不要让 shell 重定向覆盖已有重要文件或符号链接。stdout 出错、管道中断或目标写入失败时退出码为 5；已留存包仍可再次预览。

只有退出码为 0 的 export 输出才是成功导出的包。失败时 stdout 可能是错误 receipt；重定向产生的文件不能仅凭 `.json` 后缀或存在就当作有效诊断包。工具不跟踪调用方导出文件，也不会删除或发送它。

## 列表、删除与丢失回复

```sh
/data/adb/ksud sumhp doctor package list

/data/adb/ksud sumhp doctor package remove "$PACKAGE_ID" \
  --revision "$PACKAGE_REVISION"
```

list 只列出包 ID、package revision、隐私模式、场景、诊断状态和导出字节数，不展开原始资源身份。preview、export 和 list 不触发采样、目录创建、迁移、清理 staging 或恢复。

remove 只在选中 ID 的 revision 相符时移除该记录。其他包的增加或删除不会替代对该包内容的检查。删除成功后可通过 preview 得到 `not_found`，或在 list 中确认。它不删除已导出的副本、输入文件、原始日志、配置、规则、操作记录、备份或接收者保存的内容；也不改变运行规则。

create 或 remove 的回复丢失时，先查询调用方已知的 ID。不要因为没有看到 stdout 就使用新 ID 自动重采样，也不要自动重复 remove。已有 ID 的 create 会返回冲突且不重新采样；preview 可取回已保存内容。若 ID 不存在，再结合 receipt、退出码和当前需求决定下一次显式操作。

## 结果与失败处理

| status / 退出码 | 含义和下一步 |
| --- | --- |
| `saved` / 0 | 新包已替换写入，持久化和读回均确认。继续完整预览；不等同源诊断健康。 |
| `deleted` / 0 | 选中记录的删除已持久化并读回确认。需要时再查询该 ID。 |
| `observed` / 0 | 列表或包已读取。该状态不表示重新验证了设备运行情况。 |
| `invalid` / 2 | 参数、请求、ID 或 revision 不符合要求。修正输入；不自动执行建议。 |
| `not_found` / 3 | 该 ID 当前未留存。它不证明过去从未创建或导出。 |
| `conflict` / 4 | ID 已存在、reviewed revision 不符或操作期间 bank 改变。重新预览，保留已留存数据。 |
| `not_saved` / 5 | 本次未完成 bank 替换，例如 busy、容量不足、格式不可读或 staging 写入失败。原 bank 保留；先检查固定 reason，不把它解释为内核不支持。 |
| `unavailable` / 5 | 只读查询无法可靠读取 bank 或计算 revision。不可把未知结果当成空列表。 |
| `unknown` / 5 | 替换可能已发生，但持久化、读回、namespace 恢复或回复完成情况不能确认。先查询已知 ID，不回滚或自动重放。 |

receipt 将 `durability_confirmed` 和 `readback_confirmed` 分开报告。rename 后目录同步失败时可能已读回新内容，但持久化仍未确认；持久化成功而读回失败也会是 unknown。结果未知时 `saved`/`deleted` 不会冒充成功，package revision 也可能为空。输出断连可能让调用方连 receipt 都无法取得，因此稳定 ID 是后续查询依据。

存储位于控制器私有目录的 `diagnostic_packages.json`，使用独立的 `diagnostic_packages.lock` 和固定的 `diagnostic_packages.stage`。查询检查父目录、所有者、权限、文件类型、链接数和读取稳定性；未来格式、损坏数据或不安全文件类型会阻止重写，不自动迁移或清空。遇到这些情况保留原文件供本地排查，不把删除整个 bank 当作通用恢复步骤。

## 解释边界

包仍保留源诊断的 partial/stale/unavailable、`atomic:false`、`observer_verified:false` 和未检查事项。保存、脱敏或导出不会提高证据强度。模块后端推断、历史成功记录和控制面状态都不能代替普通应用、授权应用、isolated process、旧 FD 或 mmap 的实际视图。需要实测时使用[专用观察者工具包](sumh-observer-kit.md)，仅操作自有合成资源。

原始 `doctor --input` 输出方式与 Manager 完整 bugreport 保持原有行为。此处的默认脱敏只作用于新诊断包投影；不能据此把原始 doctor JSON 或 Manager bugreport 称为已脱敏。完整报告的隐私、分享、备份保留期限及真实设备运行仍需单独实现和验证。
