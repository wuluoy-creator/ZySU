# SUMH 第四十三批实施与验证记录

日期：2026-10-19（Asia/Shanghai）。对应[54 项计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)和[进度账本](SUMH-implementation-progress-2026-10-16.md)。本批继续 **C04/F05/F09：Manager 临时 Maps 删除的持久追踪与历史查询**。任务保持部分实施，全部目标继续进行；先前批次的封存证据未重跑或覆盖。

## 1. 已实现行为

在原临时规则工作台中，读取、删除与追踪能力分别显示。新 Manager 只有确认追踪能力才允许删除；旧控制器仍可提供规则查看，删除提示更新，未知能力不能授权操作。旧直连 CLI、旧 Manager 临时规则协议以及隐藏操作 schema 1 保持兼容。用法见[Maps 工作台](../zh/guide/sumh-maps-rules.md#manager-临时规则工作台)和[场景 33](../zh/guide/sumh-usage-scenarios.md#33-在-manager-追踪临时-maps-删除并查询历史当前源码已实现)。

| 环节 | 本批实现与边界 |
| --- | --- |
| 原生入口 | 复用固定 JNI Operation 服务和外层 operation_dispatch schema 1；新增 Manager 请求 schema 2，必须为 temporary_maps 资源。prepare/execute 只接受既有规范九参数精确 remove，查询仅按 ID，wait 为 0–5000 ms；任意 clear/add/list/hide/config 等变更拒绝。 |
| 能力 | `maps_temporary_tracked_manager:true`、`operation_manager_temporary_maps_schema:2` 与 prepare/execute/show/wait/result、local remove、结果资源等逐项正向协商。原 operation_manager_transport_schema:1 保留。缺少支持与读取失败/未来格式分别为不支持和未知。 |
| 执行 | 确认 prepare 的 ID、摘要、原始 boot 后，先保存 prepared，再保存 dispatching，两个本地检查点均确认才 execute 一次。保留第四十二批控制器 started 门禁。任一步未知不重新预约、重放或回退直连。 |
| 结果 | 专用 temporary_maps_mutation 解析器核对 ID、精确键、instance/generation、请求摘要、origin boot、状态、errno 与 attempted/acknowledged。command_completed 本身不形成删除成功；saved/observer_verified 始终为 false。 |
| 历史 | 原始响应与查询回执分开保存，命令状态与资源结果分开展示；原始 boot 与查询 boot 分开。手动 show/wait/result 不 execute，不刷新当前列表，也不使旧 token 恢复有效。打开历史只读本地文件。 |
| 生命周期 | Activity ViewModel 保留单次操作；选择代际拒绝迟到回执重开已关闭选择。进程重建仅加载本地身份和待查询提示，不恢复 token、规则列表或批量队列。 |
| 清理 | “忘记本地记录”单独确认，只删除本机关联；不取消操作、不删除服务端历史、不恢复规则。满额仅自动腾出严格确认 Deleted 的一个记录，未知/未完成记录不自动丢弃。 |

本批没有新增内核 ABI 或每调用者 owner。身份来自规则清单与 prepare；临时枚举本身的 boot_id 仍为 null，不用隐藏 config revision 代替 Maps generation。历史删除确认不证明当前集合或普通应用视图。

## 2. 本地格式与兼容

临时 Maps 使用独立 STM1 二进制文件 `noBackupFilesDir/temporary-maps-operations/temporary-maps-v1.bin`，最多 16 条/1 MiB；隐藏 SOP1 不迁移。只存选择字段、绑定身份、阶段及有界响应/历史，不保存 ordinal、替换路径或完整可重放参数。

读写校验严格 UTF-8、字段预算、重复 ID、尾部字节与类型化回执。未来/损坏文件禁止覆盖；写入使用 create-new 暂存、文件同步、原始字节复核、原子替换、父/目标目录同步和精确读回。未确认检查点不授权执行；执行后的保存失败保留 ID 并报告未知。默认服务的文件目录解析延迟到存储读写边界，页面组合不会提前访问 App 存储。

应用记录与服务端 16 条近期摘要/独立回执各自保留，不能宣称跨文件全局事务或所有副作用恰好一次。真实 Android 文件系统、进程终止和断电行为须另验收。

## 3. 静态复核与修正

修改前保存 499 份 before-image，见 `out/batch43-before-manifest.json`。根任务负责原生允许列表、ViewModel、界面及文档；模型作者负责新模型/存储/追踪器；测试作者独立编写原生与 Kotlin 测试。`out/batch43-independent-review.md` 为独立产品复核，`out/batch43-tests-independent-review.md` 为测试源码复核；作者自查不替代独立审查。

已处理三项复核意见：历史页区分未结束/淘汰/回执缺失，显示查询启动身份；展开按钮增加中英文无障碍状态；测试作者发现默认 singleton 过早访问 App 私有目录，改为延迟存储解析。没有因查询失败新增自动删除重试。

产品源码见[请求允许列表](../../userspace/ksud/sumhp/src/core/operation_manager_input.hpp)、[类型化模型](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHTemporaryMapsOperationModels.kt)、[存储](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHTemporaryMapsOperationStorage.kt)、[追踪器](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHTemporaryMapsOperationTracker.kt)、[工作流](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHTemporaryMapsOperations.kt)、[工作台](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHTemporaryMapsControls.kt)及[历史展示](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHTemporaryMapsHistory.kt)。

## 4. 实际执行记录

主机为 Windows，使用已安装 Zig、JDK 21、Android NDK 30/CMake 3.22.1。以下是实际执行证据；按最新有效 suite 合并，不重复计算重跑项。

- 原生 ARM64 Release：`out/batch43-build-a1/manifest.json` 编译链接通过，输入哈希稳定。ksud 为 4,236,576 字节，SHA-256 `4f36f6d4c29979460f8ab0c5345564dd0805c97c18500f337518e5a6390f3fab`。
- 原生回执生成：`out/batch43-tracked-fixtures-a6/manifest.json` 通过。真实生产提交/Journal/capture/query/embedded callback 与传输信封，存储/ioctl 边界采用模型；实际 mbed TLS SHA-256、empty/abc 向量以及 Python hashlib 逐请求交叉核对。151 份请求/正文/信封/向量中，50 份信封供 Kotlin 消费。errno 显式为合成 Android 数值，不声称在 Android 执行。
- 原生主机首次执行 `out/batch43-host-a1`：submit/state/legacy 三个 runner 通过；CLI/transport 因新公共头的 Windows errno 与旧 fixture 重复定义编译失败，保留日志后修正测试适配。提交 381 次断言、输入 IO 52 次，state 873 次扩展检查包含 337 次 tracked Maps，不重复相加。

| 最终证据 | 结果与范围 |
| --- | --- |
| `out/batch43-host-a2/manifest.json` | 只重跑受测试适配影响的 CLI 与 transport/worker，均退出 0，输入未变化。44 个 operation Manager 路由用例、共享传输 336 次模型检查及 JNI worker 生命周期测试通过；其嵌套计数不另加。Windows 未执行 POSIX exec/pidfd 实测。 |
| `out/batch43-legacy-compatibility.json` | 20 份旧临时 Maps Manager 信封与 20 份正文逐字节一致，未更新旧资源目录。新 Kotlin 测试使用独立的 50 份 tracked 信封。 |
| `out/batch43-manager-a2` | 输入稳定，143 个报告项中仅 Tracker 类初始化失败，另有 2 个 Windows 符号链接权限跳过；其他选定类通过。不能把 a2 整体记为成功。 |
| `out/batch43-manager-a3` | 修正测试的 Unit 返回类型后，Tracker 单类 14 项通过，无失败/错误/跳过，输入稳定。覆盖双检查点、取消、断连、恢复不执行、满额保留未知项及默认服务延迟初始化。 |
| `out/batch43-manager-a4` | 最终中英文保存失败提示调整后，工作台及父页面 21 项通过，输入稳定。4 张新渲染图与 a2 已人工检查图片逐字节一致；包括英文确认、中文大字体窄屏/横屏及历史操作。 |
| `out/batch43-verification-summary.json` | 取 a2/a3/a4 各类最新有效 XML，合计 **11 类、156 项：154 通过、0 失败/错误、2 跳过**。两个跳过分别是隐藏和 Maps 的真实主机符号链接创建；不计为通过。不是一次全 SUMH 测试。 |
| `out/batch43-apk-a1/manifest.json` | ARM64 Manager Debug APK 构建通过，输入稳定；42,551,060 字节，SHA-256 `d768a01bc73ca9ec67aa8560014ba41dd2aa74527425894f4695720c75cb435f`。APK 内 libruntime.so 与上述 ksud 精确相同，未安装。 |
| `out/batch43-artifact-inspection/manifest.json` | 实际产物 AArch64/RELRO/NOW、协议标识与 JNI sumhOperation 导出核对通过；ksud Build ID 为 `4b676551c398e58db5ad119636683c761745e94b`。只证明构建与打包。 |

失败过程也保留：原生夹具 a1–a4 为测试工具链/提取/链接适配失败，a5 首次成功、a6补齐传递头文件指纹；Manager a1 为测试嵌套类构造引用错误，输入稳定、未执行测试。a2 的 JUnit 返回类型问题阻止整个 Tracker 类启动，a3 才提供该类执行证据。均未通过放宽产品校验解决。静态测试审查补充的“满额仅淘汰已确认成功项”组合用例也在 a3 通过。

实际图片检查见 `out/batch43-visual-review.md`，独立测试报告见 `out/batch43-tests-report.md`。文档独立复核纠正了主计划活动表中的旧历史/daemon 说明、保存失败可能已替换检查点的语义，以及关闭重开与进程重建的区别；不覆盖既有历史批次记录。

可复现的定向入口如下，输出目录必须新建，工具变量替换为本机路径：

```powershell
& $python -B userspace/ksud/tests/run_operation_submit_host_tests.py --zig $zig
& $python -B userspace/ksud/tests/run_sumh_state_host_tests.py --zig $zig
& $python -B userspace/ksud/tests/run_sumh_host_tests.py --zig $zig --temporary-maps-fixtures out/new-legacy-fixtures
& $python -B userspace/ksud/tests/run_maps_preview_cli_host_tests.py --zig $zig
& $python -B userspace/ksud/tests/run_maps_preview_transport_host_tests.py --zig $zig
cd manager
./gradlew.bat :app:testDebugUnitTest --tests 'com.zying.zysu.ui.sumh.*TemporaryMaps*' --tests 'com.zying.zysu.ui.sumh.util.SUMHOperation*' --tests 'com.zying.zysu.ui.sumh.SUMHManagedMapsControlsTest' --console=plain --no-daemon -PABI=arm64-v8a
```

原生生成器另见[脚本](../../userspace/ksud/tests/run_tracked_maps_manager_fixtures.py)的参数说明与 a6 精确命令。已登记的既有测试 runner 消费新用例；生成器用于重新生成独立协议夹具，没有声称远端 CI 已执行。

## 5. 未执行范围和交付状态

Linux/Android 实际文件锁、fsync、断电、JNI/SELinux、内核 generation 并发、真实旋转/进程死亡、TalkBack/最大字号设备表现、普通/isolated/root 观察者及性能尚未执行；没有重跑 KMI/DDK 构建。主机/Robolectric 与编译产物不能替代上述验收。

C04/F05/F09、其他资源及完整工作流继续进行，F06/F11/F12 的 owner/事务/设备门禁不变。仅修改共享工作区并构建本地产物，未提交、推送、安装或发布；批次收尾不代表整个 54 项目标完成。
