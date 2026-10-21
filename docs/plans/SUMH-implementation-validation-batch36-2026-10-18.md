# SUMH 第三十六批实施与验证：日志阅读工作流

日期：2026-10-18 开始，2026-10-19 完成本批主机验证（Asia/Shanghai）。对应[主计划 U04](SUMH-comprehensive-improvement-plan-2026-10-16.md)及[进度账本](SUMH-implementation-progress-2026-10-16.md)。本批仅推进日志页面；全部 54 项目标继续进行，U04 部分实施，S03 存储预算和隐私、F08/M04 诊断恢复及 C06/F06/F11/F12 门禁均不因此关闭。

## 静态检查与实施范围

实施前 `SUMHConfig.kt` 在组合期间遍历全段日志筛选并生成单个 AnnotatedString，使用单个可滚动 Text；原 VERBOSE 色对控制台背景对比度约 2.64:1。`SUMHWorkspace.kt` 每次刷新先清空日志，使同来源阅读状态丢失。JNI 守护进程读取已有 256 KiB/1,000 行尾部上限，内核读取为最多 1 MiB、再保留最多 1,000 条 SUMH 行；此次不调整原生读取或持久文件政策。

- `SUMHLogsTab.kt` 承接原日志页入口和清空确认，使用可取消的 Default 后台解析/筛选、按行 LazyColumn、稳定行 key 与显式跟随/暂停。复制使用当前完整筛选结果，准备期间禁用；不自动轮询。
- `SUMHLogReader.kt` 封装实际 Workspace 读取状态：同来源刷新保留上次成功快照，来源变化立即隔离，取消或过期读取不能发布。失败单独呈现，空成功不等同失败。
- `SUMHLogView.kt` 负责严格等级识别、行身份和过滤/复制。保留原始行文本及空白，分隔符按既有复制行为归一为 LF；正文中的等级单词不推断为等级。
- 控制台语义配色、14 sp 等宽正文和文字等级保留，配合中英文界面文案。界面仅保存搜索、过滤、跟随和列表小状态，不把完整日志存进恢复 Bundle。
- [场景指南第 28 节](../zh/guide/sumh-usage-scenarios.md#28-保持阅读位置并检查最新日志当前源码已实现)记录入口、步骤、快照边界、失败与撤销方式。

这些是源码行为及预期合同，不代替构建、测试或设备运行结果。基线为 `out/batch36-before-manifest.json`，766 个文件的不可覆盖副本保留于 `out/batch36-before/`；以前批次的最终检查器和记录未重跑。

## 本批执行记录

最终执行为 [logs-a4](../../out/batch36-manager-logs-a4/manifest.json)：**6 类、60 项测试全部通过，0 失败、0 错误、0 跳过**。生产 Kotlin 和测试编译通过；本批未构建可安装 APK 或重新编译原生内核。`out/batch36-run-manager.py` 为每次执行创建不同目录，保存命令、执行前输入指纹、日志、当次 XML 和新截图，并核对运行期间输入是否变化。最终运行的 706 个 Manager 输入无变化。

在 `manager/` 下执行的最终命令：

```powershell
.\gradlew.bat :app:testDebugUnitTest `
  --tests 'com.zying.zysu.ui.sumh.*SUMHLog*' `
  --tests 'com.zying.zysu.ui.sumh.SUMHRuleInteractionTest' `
  --tests 'com.zying.zysu.ui.sumh.SUMHKernelBuildEditorTest' `
  --tests 'com.zying.zysu.ui.sumh.SUMHSmallDraftTest' `
  --console=plain --no-daemon -PABI=arm64-v8a
```

运行环境为 Windows、JDK 21.0.11、Android SDK 35 的 Robolectric/Compose 测试。`-PABI` 是本次 Gradle 参数，不代表已经完成 ARM64 设备或内核运行验证。

| 验证范围 | 实际结果及覆盖 |
| --- | --- |
| `SUMHLogViewTest` | 23 项通过：正式 daemon/内核前缀、正文误判拒绝、前缀截断边界、CR/LF/CRLF、空行/尾空格/Unicode、字面大小写搜索、多等级、重复行独立 key、追加/尾部轮转/替换和长行取消。 |
| `SUMHLogReadTest` | 3 项通过：实际 Workspace 使用的读取 helper，刷新失败保留旧快照、成功空值替换、来源切换拒绝不可协作取消的迟到结果、离页取消不显示错误、不自动轮询。 |
| `SUMHLogsTabTest` | 14 项通过：来源切换、读取/错误/空状态、筛选与精确复制、清空确认、保存小状态恢复、阅读锚点、跟随和拖动暂停、等级文字语义、色板对比度及大小字体布局。 |
| 相邻入口回归 | 20 项通过：规则交互 12、Kernel Build 编辑 4、小草稿预算 4；验证从原配置文件拆出日志页后这些入口仍可用。 |

保留所有尝试，不覆写失败证据：`logs-a1` 共 39 项、4 项失败（三项读取测试未推进 Android Handler，一项横屏 qualifier 顺序错误）；修正测试后 `logs-a2` 39 项全部通过。截图复核发现大字体来源省略过度，改为大字体下独占一行并允许换行后，`logs-a3` 59 项全部通过。最后补上截断前缀不能伪造行尾的回归，`logs-a4` 60 项全部通过。每次运行均未发生输入变化。

根代理和独立审阅者均检查了新截图。320 dp 中文、2 倍字号、暗主题的[守护进程来源](../../out/batch36-manager-logs-a4/logs-narrow-large-text.png)和[内核来源](../../out/batch36-manager-logs-a4/logs-narrow-kernel-large-text.png)完整显示；测试还检查来源文字无溢出。[760×360 dp 英文亮主题横屏](../../out/batch36-manager-logs-a4/logs-landscape-light.png)的操作、跟随状态和正文可见。内核截图包含复制成功的暂时提示，未遮挡示例日志或来源。它们是宿主渲染证据，不代替系统最大字号和设备实测。

[独立审阅记录](../../out/batch36-review-report.md)跟踪了长空白前缀取消检查和窄屏来源标签两项修正；最终模型又覆盖截断边界。控制台各等级及普通文字与背景的对比度断言均达到 4.5:1。最终日志、XML、截图和输入哈希保留于 `out/batch36-manager-logs-a4/`；本批独立收口检查器为 `out/batch36-final-check.py`，输出 `out/batch36-final-manifest.json`，核对 766 份基线、最终源码/证据、54 个任务编号、资源占位符和文档文件链接。没有重跑第三十四、三十五批历史检查器。

## 未执行与剩余验收

- 真实设备日志读取、IPC、内核及守护进程行为；安装和发布。
- TalkBack 实际朗读、键盘/辅助输入、旋转和真实进程重建；Android 真机亮/暗、窄屏横屏和最大系统字号。
- 帧时间、滚动卡顿、输入延迟、内存峰值等设备性能测量。后台筛选/懒加载的结构变化不能当作性能实测结论。
- UI 尚未注入可控的 Default 工作线程边界来确定性验证快速连续筛选时的复制禁用竞态；当前请求校验与取消发布已做源码审阅，模型取消和读取迟到有测试，不能写成该 UI 竞态测试已通过。
- U04 的 256 条长路径编辑列表检查；S03 文件总大小/频率和脱敏方案。
- Linux 实际诊断进程生命周期、KMI/CFI、native kernel 与观察者设备门禁仍沿用各批次未完成状态。

本批未操作设备或 live rules，未提交、推送或安装。清空日志的既有确认语义与规则管理范围保持独立。
