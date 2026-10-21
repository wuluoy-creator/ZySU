# SUMH 第十五批实施与验证记录

日期：2026-10-16。对应[54 项实施账本](SUMH-implementation-progress-2026-10-16.md)和[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)。本批推进 C04/U02/F04/F08/F09：稳定用户隐藏修改使用预登记 ID、执行前私有检查点和精确资源结果恢复。其他资源、完整持久队列和真实设备验收仍未闭合，整体 54 项目标保持进行中。

## 实现范围

- Manager 稳定定义修改改用 prepare→确认本地 prepared/dispatching 检查点→execute。文件同步、原子替换、父目录与当前目录同步、读回均完成后才能发送 execute；任一检查点失败不发送目标修改。execute 只向已有 daemon 发送一次，不冷启动、不自动重放。旧路径兼容工具保留原行为并单独标注。
- 本地 SOP1 文档位于 noBackup 私有目录，最多 16 条/1 MiB，保存 ID、请求摘要、原 boot、动作/选择器、配置 revision、阶段、原回包及查询结果。未来/损坏格式禁止覆盖；临时文件固定单槽，拒绝符号链接，以 CREATE_NEW 重建，残留普通 scratch 先 unlink，不截断硬链接的其他名字。容量满只自动淘汰原回包明确成功的条目；未知或仅恢复条目需显式移除。
- 进程重建加载原 ID；查询仅 show/wait/result，不 resume execute，不猜最新同名操作，不恢复未尝试批量。响应丢失或本地结果保存失败保留 ID，显示未知。原回包、命令摘要、留存资源回执和当前规则分别展示。恢复历史不清除草稿，旧 boot 明确标记历史，不能作为当前运行或应用可见性的证明。
- 控制器新增精确本地 `operation result <id>`、schema 1 `operation_resource_result` 和独立能力声明。只保留预约稳定隐藏修改的完整类型化结果；资源 aggregate schema 1 最多 16 条/512 KiB，单封装 56 KiB，不扩大原 journal 16 条/32 KiB、输入 48 KiB 或 daemon 帧 64 KiB 的限制。
- 回执在现有 journal 锁内、捕获命令输出后、finished 摘要前保存。回执失败仍正常写摘要，不回滚目标或重放。查询核对 ID/name/request revision/origin boot/exit/errno 和 finished 状态，读回执后再核对账本与 boot；变化/裁剪/不匹配丢弃 payload。查询不启动 daemon、不调用内核、不迁移或修复，不能用事后读取证明之前的持久写入已获确认。
- Manager 私有结构化对象通道复用同一生产预约和查询逻辑；父子两端均严格校验、固定 argv、无 shell。沿用有界 worker、ACK、pidfd 和隔离槽；JNI 等待 10 秒、子通信最多 8 秒，不表示 daemon 原命令已停止。公共 file/stdin 接口不变，默认定义入口没有超时 fallback。
- typed parser 验证 UTF-8、重复键、schema/精确字段、摘要、启动身份、状态/代码/计数和资源 ID。序列化提交结果最多 512 KiB 是已有 64 KiB 双流转义后的消费预算，不是增加 IPC；每个原始流仍最多 64 KiB。原始 args 不持久保存用于重放。

源码：[解析与绑定](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHOperations.kt)、[私有检查点](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHOperationStorage.kt)、[跟踪与恢复](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHOperationTracker.kt)、[Manager 绑定](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHManager.kt)、[历史界面](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHOperationHistory.kt)、[原生请求校验](../../userspace/ksud/sumhp/src/core/operation_manager_input.hpp)、[回执模型](../../userspace/ksud/sumhp/src/core/operation_resource_model.cpp)、[锁内保存与只读查询](../../userspace/ksud/sumhp/src/core/state_ledger.cpp)、[提交核心](../../userspace/ksud/sumhp/src/core/operation_submit.cpp)。

## 实际测试与构建

| 验证 | 结果与证据 |
| --- | --- |
| Manager 全 SUMH 包 | **45 类、386 项：385 通过、1 跳过、零失败/错误**。新增 parser 29、tracker 11、storage 16、Compose 4；日志 `out/batch15-manager.log`，XML `out/sumh-batch15-manager`。唯一跳过是 Windows 无创建符号链接权限，硬链接测试实际通过。 |
| 操作跟踪 | 预执行两次写入失败零 execute、损坏格式零 prepare、响应丢失的新 owner 仅 show/result、prepared 不自动恢复、摘要成功不冒充资源成功、结果存储失败停止批量、容量满保留未知、旧 boot、身份冲突、并发点击/取消不重放、多字节错误预算通过。 |
| 私有存储 | 编码/所有截断位置/未来/重复/容量、父目录同步失败后重试、子目录同步失败仍抛错、读回不一致、固定 scratch 恢复、硬链接不截断、双 store 互斥替换通过。同步函数为注入故障模型，不是 Android 文件系统耐久测试。符号链接场景如上跳过。 |
| 生命周期与布局 | StateRestorationTester + 新 tracker owner 重载已保存 ID/选中详情，不自动查询/执行、不清新稿；中文竖屏、英文横屏 2 倍字体的详情滚动、查询和关闭按钮可达。Robolectric 注入结果，不是手机杀进程/TalkBack 证明。 |
| 控制器状态/回执 | **531 项扩展合同检查通过**（375 既有、156 新增），另含原 snapshot/codec/journal/capture 检查。`out/batch15-native-state.log`。覆盖顺序、故障写入/读回、failed summary、裁剪/并发上下文、只读查询、格式/身份/类型、负 runtime errno 和边界。 |
| 预约提交及输入 IO | **187 项提交断言 + 52 项输入 IO 断言通过**，`out/batch15-native-submit.log`。包含原协议回归及字面 option-looking 分组名称，生产隐藏语法先于通用选项解析。 |
| 原生 transport/worker | **97 个故障模型场景及生产有界 root worker 检查通过**，`out/batch15-native-transport.log`。Windows 未执行 POSIX exec/pidfd smoke。 |
| CLI/embedded | 新增 **26 个 operation channel 场景通过**，保留 38 hide channel/37 definitions/90 submission/51 query/78 module-development/29 Maps 与 parser/logger 回归；`out/batch15-native-cli.log`。固定私有形态/包装拒绝/root/ACK 分派分别核查。 |
| 原生构建 | ARM64 Release ksud 编译链接通过，`out/batch15-native-ksud.log`，继承构建中 ksud clang-tidy 关闭。Manager ARM64 Release 原生库构建及改动翻译单元 clang-tidy 通过，`out/batch15-manager-native.log`。未发布 APK 或部署。 |

Manager 命令为 `gradlew.bat :app:testDebugUnitTest --tests com.zying.zysu.ui.sumh.* --console=plain --no-daemon -PABI=arm64-v8a`，使用 JDK 21；native host runner 使用 Zig 0.14.1。首次编译修正 Android 公开 OsConstants 缺少 O_DIRECTORY（改为 nofollow open 后 fstat 核对目录再 fsync）；添加字符串时恢复同文件已有的保存提示资源。首次全包两个测试尝试直接定位 LazyColumn 未创建的远端节点，改为容器 performScrollToNode 后通过，未放宽产品验证。

最终 **24 项实际 ELF/编译参数加固、7 项 CI 门禁、54 项唯一编号与账本对应、337 个本地链接（25 份文档）及 git diff --check 均通过**，日志为 `out/batch15-final-{hardening,ci,docs,diff}.log`。CI 负向 fixture 的预期错误不等于真实运行失败，也不代表远端 Actions 已运行。

最终 manifest 为 `out/batch15-final-artifacts.json`：ksud 3,704,480 字节、SHA-256 `2208b1754831da9a3874aa5cea2e3258fb38e9986e579facabb125dca9b56e61`；独立符号 18,944,136 字节、SHA-256 `ea43c3a9cc0cceecbad81a99d79c76bfbb92851c02618e1df1f7c8e8021275df`，共享 build ID `8b990443a39b3dfc2bab3542da5f70b047d3b889`。Manager 原生库 5,556,576 字节、SHA-256 `f4b1b816f48dc3143fd9d80ac02ec96a59cedf2dc2699b75087dc7a4dc72c21c`，build ID `827801be2894040eae3c3c734015c057699c681c`。均为本地编译产物。

## 审查、文档与剩余边界

独立 Kotlin 复核发现初次 mkdir 的父目录同步失败后，下一次写入会跳过父同步；已改为每次检查点重复父同步并增加故障回归。精确字段、多字节错误截断、临时文件残留进一步收紧。`out/batch15-kotlin-review.md` 和存储测试记录保存来源。

独立原生复核发现正常 pending/binding_error 的嵌套 runtime errno 为负，留存模型原先误用非负校验，导致真实待绑定回执不能恢复。已限定为有限整数 -4095..4095，逐项 syscall errno 保持非负；生产 capture→store→query 覆盖 -2/-13、两端边界及非法值，并与 Manager 的 generation=0 合同对齐。`out/batch15-native-independent-review.md` 已复核关闭；worker 最后 generation 对齐后的 build ID 以最终 manifest 为准，审查记录内较早 ID 是中间产物。

根代理已检查 `manager/app/build/reports/sumh-layout/sumh-operation-history-{portrait,landscape}.png`：历史来源分开，内容可纵向滚动，未见水平溢出，查询/等待/关闭可达。语义按钮和宿主重建测试不能替代实际 TalkBack、大字体系统设置和设备生命周期验收。

本批首个编译助手误用了 batch14 日志路径；该次失败日志已另存 `out/batch15-manager-initial-compile.log`，第十四批原始成功文本日志被覆盖，**其归档 XML `out/sumh-batch14-manager` 未丢失，仍是 41 类/326 项成功的可复查证据**。第十五批后续全部使用独立日志/归档，不把首个失败日志解释为第十四批最终结果。

使用步骤合并到[定义指南](../zh/guide/sumh-user-hide-definitions.md#manager-定义工作流)、[资源结果恢复](../zh/guide/sumh-runtime-state.md#按-id-恢复资源结果)、[场景 17](../zh/guide/sumh-usage-scenarios.md#17-恢复隐藏修改丢失的回包)、中文导航、主计划和 54 项账本。公开 result 已实现，不再称草案；仍未实现的 F 命令保留原标识。

本批不持久化整批未执行意图，不提供跨资源全队列、方案恢复点或定义导入导出。未知结果只能查询和人工决定，历史被裁剪后不能补造回执。隐藏配置、内核注册、回执、摘要与 Manager 文件不是一个全局原子事务；旧 FD、观察者可见性和其他资源不由本批证明。

真实 Android 进程死亡/旋转/后台、JNI/root/socket/SELinux、真实内核/重启/安全模式/绑定事件、普通/授权/root/isolated 观察者、POSIX 文件系统耐久性/掉电、性能、完整多语言与远端 Actions **均未执行**。未提交、推送、发布、部署或修改真实设备；整体任务继续进行。
