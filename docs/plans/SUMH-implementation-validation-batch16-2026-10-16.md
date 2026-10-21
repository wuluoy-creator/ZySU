# SUMH 第十六批实施与验证记录

日期：2026-10-16。对应[54 项实施账本](SUMH-implementation-progress-2026-10-16.md)和[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)。本批推进 F04/U03 完整用户隐藏定义交换，复用 C04/F09 预登记操作。本批代码、主机验证及文档已闭合；整体 54 项目标仍在进行，设备验收未完成。

## 实现范围

- 完整或所选定义导出采用 schema 1 `sumh_user_hide_definitions`，每项仅含 id/path/enabled/name/note/group。保留 Unicode、LF/CR/TAB、末尾空格和原始启用意图；不导出内核临时 ID、boot、运行 inode 或授权信息。严格拒绝重复字段、重复 ID/路径、未知格式、额外字段、错误类型和无效 UTF-8，不启用旧格式猜测。
- `hide definitions [revision cursor]` 是精确本地只读分页，每页最多 64 项且动态限制小于 56 KiB；读取后复核配置 revision 与 boot。Manager 跨页核对数量、身份、上下文，再读末尾空页；部分集合不得导出。`hide export` 先生成完整文档并复核源 revision，再输出 stdout。导出不查询内核、不启动 daemon、不迁移或恢复；boot 缺失不阻止配置导出，但阻止修改预检。
- 私有 hide 通道新增精确 `api meta --control-only`，仅读取控制器能力，不泛化 API 白名单。交换、分页和导入 schema 分别协商，未声明或未来版本不回退到旧变更入口。
- 导入首先写入本地未启用草稿，只做合并，不隐式删除其他定义。默认保留已有 enabled、新项停用；明确勾选后才采用导入 enabled。ID/路径冲突分别选择保留、采用本地身份、显式新身份或跳过；交叉冲突只允许跳过。显式复制映射在重新预检时仅允许精确无变更识别，不能借此写入交叉冲突。
- `hide import-id` 在已有 Transaction 锁下核对全局 revision、absent/same_identity 和不可变路径，再检查完整候选容量、受管能力和运行查询。持久保存确认后才调用用户 owner 的修改与读回。直接公共 CLI 保留现有 daemon 启动语义；Manager 使用第十五批 prepare/checkpoint/execute，向已有 daemon 至多发送一次，不自动启动或重放。
- 草稿原文上限 4 MiB + 256 字节，规则上限 4096，规范化存储仍限 4 MiB；不扩大 48 KiB 输入和 64 KiB 帧。专用私有文件各限 5 MiB，单草稿与单导出快照，原子替换、同步和有界读回；CAS 拒绝旧文件回调覆盖新草稿，未来/损坏格式不覆盖，scratch 不截断硬链接。导出在文件选择器打开前冻结内容与选择，回调按 token 读取，不取更新后的当前值。
- 每次预检绑定原文摘要、配置 revision、boot、冲突决策、启用策略和候选；检查最终集合、每个中间状态及逐项请求容量。每组最多 16 次实际修改，30 秒预算在下一项开始前检查；确认匹配 typed receipt 才推进 revision。失败、未知或停止均保留原稿和决策，剩余项需显式重新预检，无自动续传。保存相同不代表运行注册已修复，需要单独查询或重试。
- 大文件在后台解析和计算，差异列表使用 LazyColumn；完整文档和 4096 个选择不进入 Activity Bundle。文本粘贴单独限制 64 KiB。文件选择器返回早于加载时保留 URI/快照 token，加载且空闲后才接收；写文件错误明确可能存在部分目标文件，不宣称回滚。

源码：[生产定义与导入](../../userspace/ksud/sumhp/src/core/user_hide.cpp)、[存储与交换模型](../../userspace/ksud/sumhp/src/core/user_hide_model.cpp)、[输入校验](../../userspace/ksud/sumhp/src/core/user_hide_input.hpp)、[Kotlin 交换模型](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHHideTransfer.kt)、[定义分页](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHHideExportPages.kt)、[私有存储](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHHideTransferStorage.kt)、[操作生命周期](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHHideTransferOperations.kt)、[入口与文件回调](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHHideTransferControls.kt)、[冲突审查界面](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHHideTransferDialog.kt)。

## 实际验证

| 检查 | 结果与证据 |
| --- | --- |
| 原生定义/存储 | 209 项断言通过，含原文往返、严格格式、4096 条、精确 4 MiB canonical 边界及封装开销；`out/batch16-native-store.log`。 |
| 控制器 | 361 项断言通过，含 ID/路径冲突、版本、容量、保存失败零内核写入、部分运行结果、owner 隔离、分页/导出及读回变化；`out/batch16-native-controller.log`。 |
| 预约与输入 | 207 项提交及 52 项有界 IO 断言通过，最坏单项转义仍满足旧输入/帧容量；`out/batch16-native-submit.log`。 |
| 状态与回执 | 536 项扩展合同检查及原 snapshot/journal/capture 检查通过；import-id 精确回执可留存/恢复，错误选择器和伪 metadata-only 成功被拒绝；`out/batch16-native-state.log`。 |
| 传输与路由 | 108 项故障模型及生产有界 worker 检查通过；新增 29 个交换/能力路由用例及既有 CLI 套件通过；`out/batch16-native-transport.log`、`out/batch16-native-cli.log`。Windows 未执行 POSIX exec/pidfd smoke。 |
| ARM64 构建 | ksud Release 编译链接通过，3,725,728 字节，匹配符号 19,063,720 字节，共享 build ID `c14405923d90e04a6409209dd69ac5ffab56ba39`；Manager Release 原生库改动翻译单元 clang-tidy 与链接通过。日志 `out/batch16-native-ksud.log`、`out/batch16-manager-native.log`。继承 ksud 构建仍关闭 clang-tidy。 |
| Manager 全 SUMH 包 | **49 类、458 项：456 通过、2 跳过、零失败/错误**。日志 `out/batch16-manager.log`，XML `out/sumh-batch16-manager`。两项跳过均为 Windows 缺少符号链接权限，分别来自已有操作存储及本批交换存储；硬链接保护实际通过。 |
| 模型与容量 | 33 项交换模型测试通过，覆盖所有冲突、复制映射重检、默认/显式启用策略、4096 条、精确 4 MiB、增长先于缩小的中间溢出、原文 Unicode/控制字符、未来格式和逐项 receipt/revision 链。 |
| 分页与持久存储 | 新增 8 项分页/能力测试及 19 项交换存储测试（18 通过、1 符号链接跳过），覆盖末尾读回变化、缺 boot、部分集合、陈旧 CAS、4 MiB + 256 原文/4096 决策、不可读/损坏/未来、写入同步和读回故障、导出旧 token。另新增 import-id 执行前检查点与类型化选择器回执集成用例。 |
| 工作流与布局 | 11 项测试通过：导入/预检零变更、新项停用、陈旧/损坏输入保留原稿、策略变化重新预检、能力/版本拒绝零提交、成功后原稿保留、新 owner 不恢复、丢包停止且保留 ID、所选导出冻结、加载前/加载中回调不被消费，以及中文竖屏/英文横屏 2 倍字体。宿主回调接收合同不替代实际 OS 文件提供者验收。 |
| 交付核对 | 24 项实际 ELF/编译参数加固、7 项 CI 门禁单元、54 项唯一编号与账本对应、本地文档链接及 git diff --check 通过。日志 `out/batch16-final-{hardening,ci,docs,diff}.log`，最终摘要 `out/batch16-final-artifacts.json`。 |

## 审查修正与边界

独立原生复核未发现新阻塞问题，并纠正公共 `hide import-id` 与预留操作通道的 daemon 启动说明。Manager 复核发现选择器回调可能早于草稿加载导致丢失、身份冲突没有展示原路径；已加入待接收 URI/token 与接收结果，并显示所有冲突路径。复制映射再次预检的严格无变更识别补入模型测试。报告保存于 `out/batch16-native-independent-review.md`、`out/batch16-root-ui-review.md`。

首轮新增分页测试缺少 Robolectric 的 JSONObject 实现，已补相应 runner；大字体横屏回归发现新增独立按钮行挤占列表，已改用既有工具栏内的紧凑菜单。原生首轮 transport 并发运行遇到 30ms fixture 调度期限，未修改产品/测试，顺序重跑通过；CLI 新测试最初提取了 stub，已改为生产拒绝入口并重跑。各次失败日志与最终成功证据分开保留。

新增工作流测试初次等待只推进 Compose 时钟，没有排空 Robolectric 暂停的 Main Handler，导致 IO/Default 返回后的协程挂起。测试使用仓库既有的 `ShadowLooper.idleMainLooper()` 模式后通过；未修改产品 dispatcher、放宽期限或跳过断言。失败日志分别保留 `out/batch16-manager-initial-tests.log`、`out/batch16-manager-workflow-initial.log`、`out/batch16-manager-workflow-scheduler.log`。

根代理查看 `manager/app/build/reports/sumh-layout/sumh-hide-transfer-{portrait,landscape}.png`：长说明和规则可滚动，主要操作可见，无水平溢出；4096 项不会全部创建 UI 节点。未将语义及布局测试解释为实际 TalkBack 通过。

最终本地产物：ksud SHA-256 `89d51259df6f8e35495d675660568e1255ec42004f5d919b29964b93a923c73c`；匹配符号 SHA-256 `25fced7bf4aba079ace2e7f0c1aefd3817a8e7a55b9024e093c79d06fdc84892`。Manager 原生库 5,566,936 字节、SHA-256 `2ac8740cd01a5a6e2c0aa89a6a79a42b04c387b69dc49af81a6837f73f16ed2c`、build ID `3c9fe731c0cba650ea76117cca5369230355ccbe`。这些是编译产物，不是已安装或已验证运行的发布包。

使用方法合并至[完整定义交换指南](../zh/guide/sumh-user-hide-definitions.md)、[场景 18](../zh/guide/sumh-usage-scenarios.md#18-备份迁移并合并完整隐藏定义)、[中文导航](../zh/README.md)、主计划及 54 项账本。新增已实现命令已标明当前可用，其他计划命令继续保留草案标识。

本批不提供统一 F07 方案、跨资源回滚、全局原子事务、永久 4096 项操作审计或自动恢复整批队列。保留最近 16 项操作的既有查询能力，不能补造已裁剪记录。隐藏定义、内核注册、操作账本和本地文件是不同状态。

真实 Android SAF 文件提供者、旋转/进程死亡/后台完成、JNI/root/socket/SELinux、内核事件/设备重启/安全模式、普通/授权/root/isolated 观察者、POSIX 耐久/掉电、四 KMI、性能和远端 CI 均未执行。本批没有提交、推送、发布、部署或修改真实设备；F04/U03 等仍保留未闭合验收。
