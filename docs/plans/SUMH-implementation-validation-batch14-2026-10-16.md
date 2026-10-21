# SUMH 第十四批实施与验证记录

日期：2026-10-16。对应[54 项实施账本](SUMH-implementation-progress-2026-10-16.md)和[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)。本批继续 F04，将第十三批定义模型接入 Manager，同时推进 C02/C04/C05/U01/U02 的状态、期限、结果与草稿消费。本批代码、宿主回归和文档整合已收尾；F04 保持部分实施，整体 54 项目标继续进行。

## 实现范围

- 用户隐藏默认列表使用稳定定义 ID，启用/停用项同时可见，搜索路径及元数据、按状态/精确分组筛选；复用原有列表、多选、复制及进度/停止容器。支持新建、编辑 name/note/group、逐条/多选启停、定向重试、删除及整组启停。已有稳定定义 path 只读，改变路径需要显式新建并核对后删除旧定义。
- 定义 enabled、用户注册、全局暂停和绑定分别展示；applied 只表示注册成员关系，不是应用观察结果。受管能力未知/不支持关闭运行变更，完整定义仍可编辑元数据。查询失败不自动退回仅 enabled 的旧列表；路径导入/兼容工具必须明确切换，并提示路径编辑的新 ID 与元数据丢失边界。
- 页读取检查 schema、类型、状态、分页边界、重复 ID/路径、revision、boot ID、总量和受管能力；最多 4096 条、累计 8 MiB，逐次请求前后检查 30 秒预算，完成后再请求空尾页核对文件版本。失败保留部分结果或上次条目，但撤销可修改资格；没有把每条采样误称跨页内核原子快照。
- 专用 JNI 通道仅接受生产隐藏定义语法和 48 KiB JSON 参数，父端/子端独立校验后执行固定 ksud argv，不解释 shell。status 本地只读，不启动 daemon；修改通过正常 daemon/journal 一次调用。每个输出流最多 64 KiB，完整性显式标记；沿用 512 KiB 通信上限、pidfd 清理与隔离槽。有界 root worker 不排队等待已有无界工作；JNI 等待预算 10 秒，子通信最多 8 秒，超时结果未知，不自动重放，也不保证 daemon 原命令已经停止。
- 回包校验 action、previous/revision、boot、单项 ID、计数、逐项绑定及结果完整性，拒绝元数据操作带运行成功等矛盾结果。整组确认锁定审查时的版本/boot/完整成员，结果核对范围；多选按确认回包推进版本，部分/未知/错误立即停止并保留未尝试项。30 秒批量预算在每项前检查，不冒充当前命令的取消期限。
- 草稿进入现有应用私有的 4 MiB/512 条有界存储，支持失败保留、外部刷新冲突的显式选择、删除定义不自动重建和旧完成不清除新稿。重建后的活动界面观察草稿清理，解决旧 composition 回调无法关闭当前编辑框的问题。双语资源、语义标签、可滚动表单及宽度限制沿用现有 Material 组件。

源码：[定义模型与消费者](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHHideDefinitions.kt)、[Manager 绑定](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHManager.kt)、[列表工作流](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHHideDefinitionsScreen.kt)、[编辑/结果界面](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHHideDefinitionEditor.kt)、[草稿](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHHideDefinitionDraft.kt)、[JNI](../../manager/app/src/main/cpp/sumhp_jni.cpp)、[有界传输](../../userspace/common/maps_preview_transport.cpp)、[控制器分派](../../userspace/ksud/sumhp/src/core/user_hide.cpp)。

## 本批实际验证

| 验证 | 结果与证据 |
| --- | --- |
| 原生传输/worker | **90 个故障模型场景通过**，生产 JNI worker 回归通过。隐藏服务/载荷配对、槽竞争、授予 root 阻塞后不启动过期修改、参数/超时/容量/清理覆盖；原始 `out/batch14-native-transport.log`，最终预分配修改后再跑 `out/batch14-native-transport-final.log`。Windows 未执行 POSIX exec/pidfd smoke，不冒充设备链路。 |
| main→CLI→embedded | 新增 **38 个精确 Manager channel 场景通过**，保留 37 隐藏定义、29 Maps、78 模块/开发、51 查询、90 提交场景及 parser/logger 回归。非法参数/包装拒绝，status 本地、合法修改只调用正常 daemon 一次，字面组名 `--channel` 保留为数据。`out/batch14-native-cli.log`。 |
| 控制器 | **212 项断言通过**，原 190 项加 dispatch 捕获/独立截断/边界容量/异常/非零状态/流恢复。实际控制器与替身 IO/ioctl；`out/batch14-native-controller.log`。 |
| Manager | **41 类、326 项、零失败/错误/跳过**，全 SUMH 包通过。本批新增 43 项：parser/collector/batch 25、私有定义草稿 6、Compose 工作流 12。`out/batch14-manager.log`，XML 存于 `out/sumh-batch14-manager`。实际 native 调用使用注入替身；第一轮两项 malformed JSON 异常类型断言过窄，已修正为验证确实拒绝，未放宽产品解析。 |
| Manager 原生库 | ARM64 Release 编译链接通过，改动翻译单元的 clang-tidy 检查通过。首次构建报告 argv vector 循环未预分配，已添加 reserve 后通过；`out/batch14-manager-native.log`。不代表全仓库 tidy 或设备运行。 |
| ksud | ARM64 Release 构建链接通过，最终 `out/batch14-ksud-final-build.log`；沿用 batch07 加固构建目录，本构建 clang-tidy 仍关闭。最终 manifest 为 `out/batch14-final-artifacts.json`，实际 ELF 加固核对通过。 |

Manager 使用 JDK 21、现有 Android SDK，从 manager 目录运行 `gradlew.bat :app:testDebugUnitTest --tests com.zying.zysu.ui.sumh.* --console=plain --no-daemon -PABI=arm64-v8a`。原生 host runner 使用 Python 和 Zig 0.14.1。已有非 Android logger 未使用变量、Gradle 实验选项及弃用警告保持可见。

生命周期测试使用 StateRestorationTester 验证 composition/SavedState 重建后成功关闭、失败保留和新稿不被旧完成清除，并使用 Robolectric ActivityScenario.recreate 验证真实 Activity 实例重建时 ViewModel 保留、编辑框恢复及成功后关闭且只调用一次。首次 Activity 测试在两个不同 composition 调用点安装内容，导致保存位置身份不同；已统一测试安装点后通过。没有以该宿主测试宣称手机旋转或 Android 杀进程恢复通过。选择/分组/开关使用可访问语义，实际 TalkBack 尚未验证。

最终 **24 项实际 ELF/编译参数加固检查、7 项 CI 门禁测试、54 项编号与账本对应、本地文档链接及 git diff --check 通过**；证据为 `out/batch14-final-{hardening,ci,docs,diff}.log`。CI 测试中故意缺失 runner 的错误输出是验证失败关闭的 fixture，最终 7 项均成功；不是运行远端 Actions。

本批没有改动定义存储/恢复或内核语义，不重跑第十三批不变的存储/核心/状态全套；引用上一批证据不增加本批测试计数。未执行远端 CI、真实内核构建或部署。

最终 ksud 为 3,652,272 字节，SHA-256 `6b697272da4d9b4f57ba0973f3e2fc928b464fa3a77c07b7fb0e2215a0ddf787`；独立符号为 18,662,328 字节，SHA-256 `f1f36336ff3624f6f0626eaa26b8450cbefd87ed6d2ebe0c8d203d2684e44952`，共享 build ID `fba1fda5dbe958738da594b6c489442866a72dd8`。Manager 原生库为 5,525,000 字节，SHA-256 `08a7ca894880e7d4193bf4a99b9e1e105cabf362941a38d8be680f0a58515abf`，build ID `a3199a978bc8a6cf1817a71ccee6e5782bcfb309`。这些是本地编译产物，未签发发布 APK、未部署。

## 复审与剩余边界

协作审查发现两项问题：Activity 重建后旧回调清理草稿但不能关闭当前编辑框；回包允许 annotate/applied 以及零成员整组 applied 矛盾。根代理已修正并补测试，同时绑定确认范围、清除不相关旧编辑错误、让批量遇到身份不符的回包显示未知而非成功。审查记录 `out/batch14-kotlin-review.md`；worker 同时实现原生部分和模型测试，因此这些部分属于自检。独立复审的最终结论以同一记录末尾为准。根代理检查中文 360dp 竖屏暗色编辑框、英文 760×360dp/2 倍字体横屏暗色编辑框及亮色多选列表截图，未见横向溢出，表单可滚动、保存/取消及批量按钮可达；截图位于 `manager/app/build/reports/sumh-layout/hide-definition-{portrait,landscape}.png` 和 `hide-definitions-selection-landscape.png`。

尚未实现 Manager 提交前唯一 operation ID、与预约执行共用的完整队列、进程死亡后的持久结果消费及完整定义格式交换。普通 daemon 历史仍受保留上限和同名并发关联限制，不能凭最新记录唯一认领超时操作。进程被杀后草稿可读回，但不承诺还原当前操作进度/结果。分组配置保存与逐项内核同步不是全局原子事务，已有 FD 或其他观察者不由本批验证。

真实手机旋转/后台/杀进程、TalkBack、完整多语言、JNI/root/socket/SELinux、真实内核所有权、重启/安全模式/绑定事件、普通/授权/root/isolated 视图、POSIX 耐久性/掉电、性能和远端 Actions 未执行。宿主布局/生命周期模型与字段验证不能替代这些证据。

使用方法已合并到[用户隐藏定义指南](../zh/guide/sumh-user-hide-definitions.md#manager-定义工作流)、[场景 16](../zh/guide/sumh-usage-scenarios.md#16-按用途分组并暂停用户隐藏定义)、中文导航、主计划与全部 54 项账本。新增命令是否已实现按具体指南标识，不能把本批 UI 切片等同于全部 54 项完成。

本批仅修改本地工作区，未提交、推送、发布、部署或修改真实设备。
