# SUMH 第三十九批实施与验证记录

日期：2026-10-19（Asia/Shanghai）。对应 [54 项计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)、[进度账本](SUMH-implementation-progress-2026-10-16.md)与[场景 31](../zh/guide/sumh-usage-scenarios.md#31-在-manager-检查诊断包并保存副本当前源码已实现)。本批推进 **S03/M04/F08 的 Manager 单场景诊断包生命周期**；相关任务及整体目标仍部分实施。使用步骤见[诊断包指南](../zh/guide/sumh-diagnostic-packages.md#manager-操作)。

## 1. 已实现的范围

| 文件 | 行为与边界 |
| --- | --- |
| `userspace/common/diagnostic_package_request.hpp` | 六类严格 schema 1 请求；拒绝重复/未知字段、非法 UTF-8、ID/revision 和超限输入。完整请求 8192 字节，内嵌诊断 4096 字节，精确私有 argv；紧凑序列化不扩大容量。 |
| `common/maps_preview_transport.*`、`sumhp_jni.cpp`、`Natives.kt` | 专用 DiagnosticPackages 服务；使用 Manager 打包的 ksud，不在 app_process 中 self-exec。独立包预约 fd3，嵌套 doctor 保留自身 fd3 并携带包预约 fd4，复制源位于所有固定目标之上。沿用 pidfd、超时隔离及不重放；仅此服务使用 208 KiB 响应/432 KiB 外壳预算。 |
| `core/diagnostic_package.*`、`embedded.cpp`、`command.cpp` | 独立 capabilities 不触发通用 daemon 恢复；Manager create/list/preview/export/remove 复用生产控制器、namespace、存储和隐私模型。成功 preview/export 仅移除冗余 `package` 对象，保留精确 `export_bytes`；公共 CLI 仍直接导出原字节。 |
| `ui/sumh/util/SUMHDiagnosticPackages.kt` | 专用适配器、逐动作能力、严格传输/请求绑定、状态/模式/版本校验、字节数和 SHA256 核对；后台解析，不展示原始解析器/传输错误。Kotlin 不声称独立重做整个嵌套脱敏模型。 |
| `SUMHDiagnosticPackageOperations.kt`、`SUMHDiagnosticPackageStorage.kt` | Activity 持有任务和小草稿，创建/删除前先持久登记包 ID；私有固定检查点不含报告、请求正文或可重放命令。文件同步、原子替换、目录同步及读回，未来/损坏格式保留。进程重建只恢复草稿与待查询身份；手动查询并确认后继续。 |
| `SUMHDiagnosticPackageExport.kt` | 被审查内容仅存在内存；选择目的地后先带 revision 查询并比较完整内容，之后才打开输出流。写入/flush/close 在 IO 线程完成；失败保留原包。丢失会话必须重新预览，重复回调不能重复写入。 |
| `SUMHDiagnosticPackagesScreen.kt`、`WorkspaceScreens.kt`、中英文资源 | 诊断页新增入口、四场景表单、身份保留单独确认、列表、完整内容按块显示、查询恢复和条件删除。通过系统 CreateDocument 保存副本，不创建公共缓存或自动分享。复用 Material/Yuki 主题。 |

原有诊断包容量维持最多 8 个、每份精确导出 96 KiB（含 LF）、bank 与固定 stage 各 1 MiB。保存成功、证据完整度、持久化和读回分别解释；包不证明普通应用视图。新私有通道是 Manager 内部实现，未作为通用 shell 工作流发布。

## 2. 审查和修正

本批改动前保存 **1211 份 before-image**，位于 `out/batch39-before`，清单为 `out/batch39-before-manifest.json`。未覆盖或重跑已经冻结的 Batch34–38 检查器。

`out/batch39-ui-review.md` 由 Kotlin 作者审查 root 的页面与导出实现，发现原生 `json::dump` 添加逗号空格，使 Kotlin 接受的临界 4096 字节请求在原生层超限。已改成诊断标量对象的紧凑序列化，并添加 4095/4096/4097 字节、有无 operation ID 及字符串空格保留检查。未放大任何限制。

`out/batch39-kotlin-independent-review.md` 由原生作者审查 Kotlin，发现多个 Activity 的旧状态可以覆盖或清除另一个操作的恢复 ID。已增加同锁条件登记/清除和进程内 tryLock 协调器：从登记前保持到原生结果返回，另一个 owner 的变更/查询/确认即时拒绝，不排队。恢复先读回当前检查点；不符时采用当前 ID 并撤销旧查询结果。root 进一步要求覆盖“登记后、派发前”的查询/清除间隙。新增真实文件双 owner 和受控间隙测试；取消的 owner 通过 UNDISPATCHED、ensureActive 和 finally 释放预先获得的协调权。不是跨应用进程的锁或抵抗特权改写的保证。

root 复核请求/响应字段、隐私解释、条件导出、native fd 生命周期和文档。首轮 Manager 编译暴露 Android 公开 SDK 不提供 `OsConstants.O_DIRECTORY`，改为既有项目做法：只读/不跟随链接打开后 fstat 验证目录，再 fsync。首轮 CLI 路由测试缺少新 dispatch 的测试替身导致链接失败，已补齐实际 main/embedded fixture 和六类操作路由覆盖。

`out/batch39-transport-independent-review.md` 独立检查包/采样预约继承、fd 源提升、异常与超时、能力及作用范围，未发现阻断源码问题。预约只覆盖同一个网络 namespace，不宣称跨网络 namespace 全设备排他。首轮 APK 的强制 clang-tidy 拒绝资源管理类缺少显式 move 声明、公开成员和嵌套条件；随后改为私有描述符/访问器、禁止 copy/move、const 作用域对象和普通分支。未关闭检查或增加抑制，修正后又经静态复核。

多 owner 审查的取消覆盖建议已转为两项针对性测试：真实取消已加载 ViewModel 的 scope，核对无登记/派发且协调权可重用；在原生调用挂起时取消，核对其他 owner 持续被拒绝，结果完成后仍保留 ID 并只能手动恢复。这不模拟真实 Android 进程被终止；执行结果列入最终 Manager 回归。

独立文档复核 `out/batch39-doc-review.md` 指出创建/删除不会自动更新列表。指南已明确：待查询 ID 的成功查询会提供当前预览，可以直接使用；需更新列表时再次手动读取。中文导航也同步为 31 个场景。

界面沿用现有主题与平台控件，应用 ui-ux-pro-max 的通用可访问性规则；与此原生界面无关的网页设计建议未采用。已目视检查 Robolectric 320dp 中文浅色/2 倍字号与 760dp 横屏深色/2 倍字号截图，文本换行和滚动操作可达。截图使用合成包，不是设备真实报告。TalkBack、软键盘、系统文件提供者、实际旋转/进程终止及 reduced-motion 设备验收未执行。

## 3. 执行记录

运行器 `out/batch39-run-native.py`、`out/batch39-run-manager.py`、`out/batch39-cross-transport.py`、`out/batch39-build-apk.py` 为本批独立文件。各次日志、输入摘要、结果与产物使用独立目录；失败记录保留。

| 实际执行 | 结果与证据范围 |
| --- | --- |
| Manager preliminary-a1 | 编译失败：公开 SDK 不存在 O_DIRECTORY。未执行测试，不记为通过。 |
| Manager preliminary-a2 | 5 个专项 suite、35 项测试通过；包括严格 parser、checkpoint、workflow、导出和 Compose 界面。 |
| Native host-a1 | 控制器、采样、模型、存储、传输/worker、query worker、CI 单元通过；CLI fixture 缺失 manager_dispatch 替身导致链接失败。整体尝试未通过。 |
| Native host-a2 | 全部选定主机套件通过，输入稳定。649 项 package 控制器/语法；原 doctor 165/57/99；模型 354、存储 9795；152 项诊断 main/CLI/embedded 路由及既有相邻功能回归。传输含 78 项 package/嵌套预约检查，JNI worker 使用提取的生产代码和故障替身。CI 单元 7 项通过。 |
| Native build-a1 / a2 | NDK 30.0.16248370、API 31、ARM64 Release 构建成功；a2 在路由测试修正后重新核对稳定输入。产物相同。未运行或安装。 |
| Native host-a3 / build-a3 | 资源管理类规范修正后重复相关主机检查及 ARM64 构建均成功，输入稳定。ksud 构建没有启用 clang-tidy，Manager APK 的必需原生检查另列，不互相替代。 |
| Native host-a4 / build-a4 | 补齐访问器 `[[nodiscard]]` 后，全部选定主机套件和 ARM64 Release 构建通过，输入稳定。控制器/语法 649 项，采样/输入/实际隐藏集成分别 165/57/99 项，隐私模型 354 项，存储 9795 项，诊断 main/CLI/embedded 路由 152 项；相邻回归、生产 JNI worker 提取体及 query worker 通过，CI 单元 7 项通过。传输通用故障模型 324 项，diagnostics/package/temporary Maps 专项分别 81/78/21 项单列，不作为互不重叠的总计。Windows syscall 替身不等于 POSIX 运行。最终 ksud 与 a3 二进制一致。 |
| POSIX cross-a1 / a2 / a3 | portable transport/worker 及 Maps、doctor、package 生命周期用例交叉编译为 x86_64 Linux musl 成功；Windows 上未执行 POSIX 进程/FD 用例。保留 musl CMSG 宏符号比较警告。 |
| POSIX cross-a4 | 最终源码的 portable transport/worker 及真实 exec/fork 生命周期 fixture 交叉编译通过，输入稳定；仍仅为 compile-only，未执行 Linux 程序。musl `CMSG_NXTHDR` 的符号比较警告保留。 |
| Manager final-a1 | 已换入本批 ksud。688 项：686 通过、2 项既有符号链接用例因 Windows 权限跳过，无失败。该结果早于多 Activity 恢复保护修正，不能单独作为最终证据。 |
| Manager final-a2 | CAS/协调器修正后 692 项：690 通过、2 项同类跳过，无失败。随后补入两项取消测试，并修正原生 lint，因此最终结果另列。 |
| APK-a1 | 强制 clang-tidy 报告七项资源管理/条件表达式规范问题而失败，未生成本批成功 APK；保留失败日志。 |
| APK-a2 | 新访问器还需 `[[nodiscard]]` 的强制 lint 诊断，构建失败；已直接补齐属性，未添加抑制或改变执行行为。 |
| Manager final-a3 | 最终 694 项：692 通过、2 跳过、0 失败、0 错误，输入稳定。五个本批诊断 suite 共 41 项通过，包含四项多 owner 保护和两项真实 ViewModel scope 取消测试。两个跳过项为既有 Windows 符号链接权限假设，详见下文。 |
| APK-a3 | `assembleDebug` 成功，必需的原生 clang-tidy 通过，输入稳定，未安装。包内 `libruntime.so` 与最终 ksud 的 SHA256 一致，`assets/runtime/payload.txt` 指向该文件；实际 JNI 动态导出符号检查通过。 |

最终证据为 `out/batch39-host-a4/manifest.json`、`out/batch39-build-a4/manifest.json`、`out/batch39-cross-a4/manifest.json`、`out/batch39-manager-final-a3/manifest.json` 和 `out/batch39-apk-a3/manifest.json`。以上执行均通过且输入在各自运行期间保持稳定；早期失败和中间结果完整保留，不用后续通过覆盖原记录。

两个跳过项为 `SUMHHideTransferStorageTest.symlinkScratchAndDestinationAreRejectedWhenSupported` 和 `SUMHOperationStorageTest.symbolicDirectoryDocumentAndScratchAreRejectedWhenHostSupportsLinks`，均因主机不能建立所需符号链接而跳过，未计为通过。最终回归保存的两张截图与前述目视检查版本哈希一致，不能替代设备上的 TalkBack、旋转或进程终止验证。

最终 ksud 产物为 **4,221,472 字节**，SHA256 `810f926fb1fffa606540f785030af3b91a2efd9c08dc31d3ed1a6a07681afb09`，Build ID `cfaeb5079b55dfd39a33a18518d2897e578323dc`。`out/batch39-artifact-inspection-a4` 核对 AArch64、RELRO/NOW、编译单元、保留符号及协议字符串。初次检查错误要求 LTO 后仍保留可内联的 manager_dispatch 独立符号；原日志与脚本保留在 `out/batch39-artifact-inspection`，修正检查方法后通过，不把缺少该独立符号解释为功能缺失。

最终 APK 位于 `out/batch39-apk-a3/app-debug.apk`，**42,538,412 字节**，SHA256 `ce8f8f34c378de27fcdb58309d071cd036948bd9d43b2e5b4e5491db4f1a2dcc`。`out/batch39-jni-inspection/manifest.json` 核对 APK 中 `libkernelsu.so` 的实际动态定义导出 `Java_com_zying_zysu_Natives_sumhDiagnosticPackages`；JNI 库 SHA256 为 `323073183ae538d038f11ff9831aebd05d2adc2a49b40adc06afe07d41115638`。这些是构建与二进制检查结果，未在 Android 上加载或运行。

## 4. 仍需完成

本批不构成完整安全审计或设备验收。未执行 Android JNI/root/SELinux 实际调用、文件提供者断连、真实进程被终止、Linux namespace/pidfd/预约生命周期、断电耐久性、设备性能/能耗与普通应用/isolated/root 观察者验证。模拟 syscall、Robolectric 和交叉编译分别标注，不能互相替代。

保留 S03 全量报告/原始 doctor 隐私和备份期限，M04 完整组件/KMI/live-owner 覆盖，F08 资源重试、恢复点关联和完整恢复向导；U05 全部语言、U06 全页面状态统一仍未完成。既有 JNI 清空日志的单链接差异不在本批修复。F06/F11/F12 的 owner/事务和设备门禁不变。

仅修改共享工作区；未提交、推送、安装、发布或向外发送诊断材料。整体 54 项目标继续进行。
