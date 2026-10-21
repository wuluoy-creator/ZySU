# SUMH 第四十六批实施与验证记录

开始日期：2026-10-19；基本闭合日期：2026-10-21（Asia/Shanghai）。对应[54 项计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)和[进度账本](SUMH-implementation-progress-2026-10-16.md)。本批推进 **F07 Manager 未启用配置方案库**，同时完善 U01/U02/U03/C05 的大文档、草稿和传输边界。按用户要求停止功能扩展，先完成源码、必要回归和构建的基本闭合，交接[实际测试清单](SUMH-basic-test-handoff-2026-10-21.md)。F07 保持部分实施，激活、运行恢复与服务端方案操作日志尚未开放；本机通过不等于设备验收。

## 1. 已实现范围

独立方案页从“挂载与隔离”及旧 SUMH 配置入口进入。进入页面只加载本机草稿、固定导出和待核对记录；读取方案库、比较及导入预检由用户手动发起。复用第二十六批四资源方案、生产验证器和独立方案库，不启动 daemon，不改变活动配置或内核规则。使用步骤见[方案指南](../zh/guide/sumh-profiles.md)和[场景 24](../zh/guide/sumh-usage-scenarios.md#24-更新前保存配置方案并比较差异)。

| 环节 | 行为与边界 |
| --- | --- |
| 方案库 | 按稳定 ID 区分条目，名称允许重复；读取失败保留旧列表并标记当前状态未知。 |
| 捕获与管理 | 命名捕获、导入、复制、改名、删除均保存为未启用方案；操作绑定库版本，已有方案还绑定单项版本。 |
| 差异 | 手动选择 settings/modules/user_hide/maps，显示增删改、未变计数、Maps 开关及隐藏身份冲突；只比较配置，不评估运行可应用性。 |
| 导入 | 系统文件读取到独立私有草稿；完整格式和资源规则由原生验证器检查。资源选择仅影响差异，导入仍保存全部四资源。 |
| 导出 | 在同一库快照核对已审查版本，固定文档后再选择系统保存位置。取消或写入失败保留固定内容。 |
| 写入中断 | 本机 prepared/dispatching 检查点先于派发；超时不杀写入子进程、不自动重放。按已知 ID 查询当前状态，再明确确认处理待核对记录。 |
| 恢复语义 | 本机检查点不构成服务端操作回执。当前存在或缺失不能证明过去那次保存/删除的结果，不声称跨文件原子或恰好一次完成。 |

## 2. 原生协议与容量

新增固定内部路由 `ksud sumhp profile manager --channel`，只接受精确参数和 root 授权。它不是通用 shell 或脚本命令包装；原 CLI 保持兼容，普通 CLI export 仍输出裸方案。

JNI 在提升权限和启动异步线程前复制私有输入/输出 FD，检查 UID、普通文件、单链接、访问模式、独立 inode 和空输出。超时后任务持有自身描述符，不使用可能已复用的 Java FD。专用传输采用私有 seqpacket、4 KiB 帧、自进程 pidfd READY/ACK、输入 DONE/EOF、输出 DONE/EOF 与成功退出核对；普通 IPC 预算不变。

单方案仍限 8 MiB，请求 8 MiB + 4096，方案库 16 份/32 MiB，差异输出 24 MiB。只有小完成回执返回 JNI，完整原始 JSON 存入独立暂存 inode。未完成文件不能作为结果。等待期限不意味着内核 IO 或 fsync 可被强行中断；卡住的子进程保持隔离，禁止无限排队。跨进程 guard 的范围是同一网络命名空间。

服务端逐项协商 Manager 能力；`profile_activate`、`profile_restore`、`profile_operation_journal` 继续为 false。Manager 内部 show/export/preview 请求的期望版本在选择目标的同一个库快照中核对，过期时不替换用户审查的文档。旧 CLI 按 ID 读取当时内容，不提供此前审查版本的输入参数。命名空间恢复失败保留原写入结果并在 Manager 标记未知。

## 3. 静态复核

修改前保存 1326 份 before-image（17,864,619 字节），见 `out/batch46-before-manifest.json`。原生、模型/存储与界面分工实现，根任务负责 JNI、导航、集成和文档；已有大范围共享改动保留。冻结的第四十至四十五批证据不重跑或覆盖。

首轮集成复核记录在 `out/batch46-parent-review-1.md`，独立模型复核见 `out/batch46-model-independent-review.md`。已移除重复生产验证规则的近似实现，增加真实原生协议样本核对；导入失败保留原草稿，固定导出可在页面重建后重试；成功回执绑定提交前库版本，失败的选中查询使旧列表失效，重新读取变化后的库会清除旧详情。大文档摘要和精确比较移至工作线程，数字精度与指数有界，避免短输入扩展为无界字符串。

草稿文件替换后同步／读回失败时，保留显示中的旧草稿，并提供明确的本机重读操作；草稿、检查点和导出损坏分别阻止对应操作。模型用例覆盖故障后重读再编辑及当前状态核对后继续操作。JNI 独立复核见 `out/batch46-jni-independent-review.md`，没有发现阻塞问题；描述符和权限仍需真机验证。

指南独立复核见 `out/batch46-guide-independent-review.md`，已修正文档中三项范围：Manager 的结果未知不证明方案库已经替换；旧 CLI 不绑定此前审查版本；只有采集当前配置会补默认设置，导入方案必须包含完整字段。原始审查与失败测试记录保留，不改写成通过。

复制对话框的诊断记录在 `out/batch46-ui-debug-a1/`：屏幕只重组两次，表单测量却在 544／624 px 间反复切换，至少 131072 次。改为显式可用宽度、24 dp 外边距及 560 dp 上限后，debug-a2 的原测试通过，测量稳定。独立复核见 `out/batch46-model-ui-debug-findings.md`；最终移除临时计数和日志，完整 a4 仍通过，未延长测试超时或关闭空闲检查。

## 4. 实际执行记录

主机为 Windows，使用现有 Zig、JDK 21、Android SDK/NDK 和 CMake。不将运行期间源码变化的测试记为最终验收。

| 证据 | 实际结果 |
| --- | --- |
| `out/batch46-worker-a2/manifest.json` | 提取生产 JNI worker 的 28 项主机检查通过，输入稳定。验证描述符独立持有、权限/类型拒绝、有限等待、不排队及迟到结果。没有加载真实 JNI 或 Android root。 |
| `out/batch46-native-final-a1/manifest.json` | 原生控制器 120 场景/828 断言及 62 项输入 IO 断言通过；专用传输 63 场景/75990 断言通过。ARM64 Linux musl 编译通过，未执行 POSIX smoke。后续集成修正需新证据，不覆盖此记录。 |
| `out/batch46-native-final-a2/manifest.json` | 最终原生控制器 132 场景/907 断言、62 项输入 IO 断言及专用传输 63 场景/75990 断言通过，输入稳定。后者包含逐帧容量循环，断言数不等于独立测试数。23 组生产原生请求／响应样本可精确重新生成，使用实际 mbedTLS SHA-256，Python 独立核对摘要。 |
| `out/batch46-regressions-a2/manifest.json` | 现有方案模型 281 项检查、方案存储 10 组/1076 项检查和 CI 注册 7 项测试通过，源码稳定。较早 a1 整体指纹因原生修正变化，保留但不作为最终基线。 |
| `out/batch46-manager-a1/manifest.json` | 编译成功；28 项测试中 27 通过、1 项复制/改名界面测试等待超时。模型源码在运行期间变化，因此整个运行不记为最终通过。 |
| `out/batch46-manager-a2/manifest.json` | 编译成功；50 项测试中 49 通过、1 项相同界面超时；输入变化，不作为最终验收。 |
| `out/batch46-manager-a3/manifest.json` | 源码稳定；62 项中 61 通过、1 项相同界面超时。42 项模型／存储／工作流／协议测试通过，界面布局候选修正未解决超时，继续保留失败。 |
| `out/batch46-ui-debug-a1/`、`out/batch46-ui-debug-a2/` | 针对实际测量循环定位，保留失败及修正后的 1 项复制／编辑／改名交互通过记录，测试源码未改变，诊断期间计数不作为正式产品代码。 |
| `out/batch46-manager-a4/manifest.json` | 最终 Kotlin／界面源码稳定，62 项全部通过、无错误或跳过：20 项界面、14 项解析、7 项原生样本兼容、9 项存储、12 项工作流。4 张截图已复核：复制编辑、中文窄屏／大字体、横屏深色删除确认，无遮挡或不可达按钮。 |
| `out/batch46-ksud-a1/manifest.json` | Android ARM64 ksud 编译链接、24 项实际产物/编译选项加固检查、源码/依赖/工具链来源核对、嵌入字节证明通过。仅含原有 installer.sh/shellrc.sh，不包含 ksuinit/KMI 载荷；属于编译与字节一致性证据，不能当作完整安装包。 |
| `out/batch46-apk-a1/manifest.json` | 首次 APK 构建被项目默认 clang-tidy 拦截：容量常量、析构类特殊成员、声明排版及任务封装等。没有关闭静态检查；保留失败产物记录，临时载荷已还原。 |
| `out/batch46-native-lint-followup/manifest.json`、`out/batch46-worker-a4/manifest.json` | 局部修正后，63 场景传输模型及 ARM64 musl 编译通过，28 项 JNI worker 检查通过，输入稳定。POSIX siginfo_t 的平台 union 读取使用单行说明抑制，其余警告在源码修正。 |
| `out/batch46-ksud-a2/manifest.json` | 最终源码再次完成 ARM64 编译、24 项加固检查、来源及嵌入字节核对，全部步骤退出 0；产物 4,252,840 字节，仍仅含两个 shell 资产。 |
| `out/batch46-apk-a2/manifest.json` | 最终 Manager assembleDebug 成功，默认 C++ 静态检查通过，输入稳定。APK 39,590,976 字节，内嵌控制器字节、JNI 导出符号和能力标识已核对。无 v1／APK signing block，apksigner 按预期拒绝未签名包；没有签名或安装。 |
| `out/batch46-apk-stage-a2/manifest.json` | 构建临时载荷已按备份字节精确恢复；产物保留在 out，工作区原 libruntime.so 未被交付载荷覆盖。 |

最终 Kotlin 测试之后仅修改两个 C++ 文件以满足静态检查：显式常量宽度／括号、禁止 FD 移动、nodiscard、拆分声明、任务成员私有化及 POSIX union 注释。传输和 JNI 由新回归及实际编译覆盖，控制器／存储／协议样本输入未变；证据清单明确记录这两份输入的后续版本，不把旧指纹写成相同。

基本闭合清单为 `out/batch46-final-manifest.json`，保存修改文件、before-image 核对、最终证据摘要及既有第四十至四十五批清单校验；源码留档在 `out/batch46-final-source/`。具体测试产物、哈希及步骤见[交接文档](SUMH-basic-test-handoff-2026-10-21.md)。这次闭合不新增实际设备通过记录。

## 5. 保留的验收范围

未执行真实 Android namespace/SELinux、文件提供者、进程终止/系统重启、文件锁及 fsync 故障/掉电、跨命名空间并发、TalkBack 和设备性能测试。没有运行真实 KMI/DDK 或部署到设备。主机模型、Robolectric 和编译只能证明各自覆盖的行为，不能替代这些验收。

本批交付未启用方案管理，F07 的激活/差异恢复/运行恢复、F09 服务端方案操作历史以及 F06/F11/F12 owner/事务门禁保持待完成。全部 54 项任务继续按依赖推进，批次收尾不代表整个目标完成。
