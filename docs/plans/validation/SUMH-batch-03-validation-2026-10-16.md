# SUMH 第三批实施与验证记录

日期：2026-10-16（Asia/Shanghai）。对应 [54 项主计划](../SUMH-comprehensive-improvement-plan-2026-10-16.md)及[实施账本](../SUMH-implementation-progress-2026-10-16.md)。本批推进 R05、K07、U01 的第一阶段，并修正第二批独立审查发现的旧配置兼容问题。未提交、推送或部署。

## R04 兼容修正

旧 CLI 曾允许空 `fs_type`、`mount_backend`、`mount_hide_mode`，旧运行逻辑分别按 `auto`、`auto`、`normal` 处理。现在只有无版本文档在解析时兼容归一；schema 1 文档及 patch 继续拒绝空值。只读加载不迁移文件，首次显式保存仍备份原字节并写入 schema 1。

新增逐字段、混合旧配置、只读无写入、备份原文及保存后读回回归。修正后 SUMH 原生 runner 的 13 个程序通过；Windows 符号链接 fixture 仍不可用，不计为通过。

## K07 最终 ioctl 能力边界

- Maps 启用需要就绪的 proc proxy；未就绪返回 `EOPNOTSUPP`。关闭及受管空集合清理仍可执行，不影响临时规则。
- Mount Hide 查询和开启统一检查 proxy 与挂载信息 renderer；支持位与配置开关分开。
- proxy 退出时先撤回就绪状态，再 unregister/drain；合法 Kernel Build 关闭在功能不可用时保持幂等。
- ABI 编号与结构未变。支持位不是普通应用或其他观察者实际生效的证明。

实现子任务与独立复核分别运行：`test_sumh_feature_readiness.py`、`test_sumh_maps.py`（2 组）、`test_sumh_kernel_build.py`（43 项）、`test_sumh_native_namespace.py`，均通过。测试编译生产函数和 ioctl 分支，注入缺符号、注册失败、缺 renderer、copy 失败、重复开关和共享引用等条件。CI 原生清单为 25 个入口；Python discovery 57 项通过、3 个既有 POSIX 条件跳过，discovery 不等于执行所有原生入口。

独立复核未发现阻断问题。完整 `supported/configured/active/degraded/last_error` DTO、CLI/Manager 同步呈现和设备观测继续归 K07/C03。

## R05 功能应用失败隔离

基础功能能力查询失败时不修改该组功能；单项失败后继续其他独立功能。结果区分 applied、deferred、unsupported、failed、rolled_back、rollback_failed、partial、outcome_unknown。Kernel Build 有结构化旧值及读回，可定向回滚；旧 ABI 的 debug/stealth/mount/maps/statfs 无可靠旧值查询，不猜测关闭状态，也不执行全局关闭代替回滚。

CLI、启动和热刷新保留已经提交的模块映射，分别尝试基础功能、Maps、xattr 和用户 hide。模块规划/构建事务本身原有的清理边界保留。Maps 继续使用现有受管集合原子替换，不从保存路径推测旧运行 inode 集合。

独立复核复现了“子进程已经提交 Maps，但结果消息丢失”的情况。修复后 namespace helper 区分未启动、确认完成/拒绝和未知结果，Maps 在回包丢失、截断、读取失败或完成状态丢失时报告 `outcome_unknown`，不继续启用、回滚或重放；只有确定拒绝才报告失败。旧内核未执行受管替换时不再声称集合已替换。Kernel Build 单字段覆盖会先解析另一字段的原值，读回比较两个字段，取原值失败时不修改该功能。

实现者最终运行 13 个原生测试程序通过，另重跑配置/生命周期/Maps 和 namespace 相关回归；生产 namespace helper 经 Maps apply 完整链路注入回包故障，断言提交一次、未继续切换且结果未知。两项 Linux target 仅交叉编译通过，未在 Linux 运行。最终 Android ARM64 Release ksud 编译链接通过（session 51044）。

修正后独立静态复核确认三项发现均已闭合、未见剩余阻断问题；这次复核没有重复执行原生测试，以上测试和构建来自实施者执行记录。

## U01 用户隐藏草稿与分批文件输入

用户隐藏新增/批量/逐规则草稿及批量失败重试路径改存应用 `noBackupFilesDir/sumh-drafts`，不会进入 Android 备份或传给 root daemon。进程内共用一个会话和串行写入队列，Activity 更换不会先读到旧磁盘草稿；SavedState 保留原有的小型页面状态与有上限的结果摘要。

存储采用有版本标记的有界格式、严格 UTF-8 解码、文件同步、同目录原子替换和读回。单一临时文件槽避免进程中断产生无限临时文件。无法读取、未来格式、错误长度或损坏会暂停编辑；保存失败保留内存输入并提供重试。当前实现不承诺父目录 fsync 或掉电耐久性，进程在保存确认前被终止仍可能丢失尚未提交的输入。

每条路径最多 4095 个 UTF-8 字节；批量文本最多 1024 行、256 KiB；所有用户隐藏草稿最多 1 MiB、1024 条编辑定义。预算包含键、编码开销及预留失败重试路径，不截断输入；超限保留原草稿并就地提示。独立复核发现的“失败后重复存储路径超预算”已改为输入阶段预留容量，并补接近上限的全部失败/磁盘恢复用例。

文件导入通过系统选择器，按最多 256 行、64 KiB 分块预览，保留 Unicode、末尾空格和顺序；UTF-8 错误、超长行明确失败。先完成或清空当前批次再读取下一批。当前批次持久保存，文件余下部分不持久化；离开页面或重启后重新选择文件，已保存路径由现有预览去重。单路径内的 LF/CR 不转成文件行导入格式；无损交换另由 U03 实施。

U01 尚未迁移模块规则、Maps 等其他编辑器，也未完成真实进程终止、旋转、TalkBack 及全部布局验收，因此账本标记部分实施。

## 验证过程与未测范围

新增测试使用真实草稿 codec、宿主文件系统、故障 storage 和 Compose/Robolectric。首次全包 135 项中有 2 项失败：宿主 Windows 下 AtomicFile 替换未覆盖旧文件；读回检查阻止了假成功，随后改为显式原子移动。后续生命周期等待未驱动 Robolectric 主 Looper，已显式驱动测试 Looper，11 项草稿专项通过。加入 2 项窄屏/大字交互检查后发现后台页面与弹窗重复呈现状态提示，已改为弹窗开启时仅在弹窗内显示，避免重复语义播报。失败运行保留在记录中，不作为通过证据。

独立复核已确认 U01 的重试容量和临时文件修复，未发现剩余阻断问题。专项测试验证超过 750 KiB 的草稿及全部失败路径精确从磁盘恢复，实际 Activity `onSaveInstanceState` 序列化小于 128 KiB；这不是 Android 进程真正被杀后重建的设备测试。

最终 Manager 全包通过（session 96313，`BUILD SUCCESSFUL in 1m 8s`）：23 个测试类、139 项测试，0 failure/error/skipped。新增 13 项覆盖文件/codec/预算/失败恢复、完整重试身份、临时文件中断、SavedState 体积和窄屏/大字输入反馈。已人工查看生成的中文 360dp 竖屏和英文 760dp 横屏 2 倍字号测试截图，提示、保留输入和操作按钮可见；文件选择按钮在当前批次清空后可达。未执行真实 TalkBack、系统文件选择器往返或设备键盘遮挡检查。

复现 Manager 测试：在 `manager` 目录设置 JDK 21 和已有 Android SDK 后，执行 `./gradlew.bat :app:testDebugUnitTest --tests 'com.zying.zysu.ui.sumh.*' --console=plain --no-daemon -PABI=arm64-v8a`。核对 `manager/app/build/test-results/testDebugUnitTest/TEST-com.zying.zysu.ui.sumh.*.xml`；截图位于 `manager/app/build/reports/sumh-layout/draft-limit-portrait.png` 与 `draft-limit-landscape.png`，属本地生成产物。

没有执行真实设备、内核/KMI 构建、真实断电、性能功耗、远端 Actions/tag 发布或普通应用/isolated/工作资料观测。R05/K07 的完整状态合同、U01 其他编辑器仍有实现工作，不仅是测试未完成。
