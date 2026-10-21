# SUMH 第二十八批实施与验证记录

日期：2026-10-17。对应[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)和[54 项账本](SUMH-implementation-progress-2026-10-16.md)。本批继续实施 C06 的源对象前置项，并修正 T01 宿主断言工具；C06 仍部分实施，整体目标继续进行。没有新增协议能力位、owner 事务、F06 应用或 F07 激活入口。

## 实施变化

普通源先做一次原生不跟随末尾符号链接的路径查询；非链接直接由持有对象生成元数据并取得规则自己的引用。`sumh_entry_capture_resolved` 借用调用者的完整 mount+dentry，拒绝链接输入 ELOOP。元数据填充与路径解析分离，保留源/可见属性回退、虚拟身份及可选 canonical 分配行为。EINVAL、EALREADY、缺 inode 和不支持类型不会泄漏新引用；EALREADY 保留已有条目。

符号链接继续走原生 `kern_path(..., LOOKUP_FOLLOW)`，保留进程根、绝对/相对链接、`..`、挂载及安全策略。受检查入口先比较原生 nofollow 结果与预期叶对象，不匹配返回 ESTALE；随后跟随查询仍可能遇到替换，因此这不是原子或防 ABA 的链接/目标配对。没有手写链接解析器，也没有将跟随查询限制在持有父目录的根下。

MERGE 物化分为每目录的完整名称收集和随后安装。actor 只校验、计数、分配并排队；重复调用 `iterate_dir` 至 EOF，先关闭迭代文件，再执行子项查找、规则发布和递归。非链接子项以持有父目录加单个已验证名称执行原生 nofollow 相对查找，类型取自捕获 inode，忽略未知或过期的 dirent 类型。普通子项直接捕获该对象，嵌套目录直接传入借用引用；重复嵌套继续复用原规则根。目录路径字符串作为描述与可见路径使用，不再重新选择非链接源对象。

共享预算覆盖整次递归：最多 1024 个目录、16384 个非点名称、65536 条原始记录、65536 次迭代及 4 MiB 名称队列/拼接路径，深度 0–8。已发布规则与 VFS 内部分配不在该工作区计数中。非法名称、有回调但位置不前进为 EIO；容量为 E2BIG；活动祖先循环和深度超限为 ELOOP；路径不能完整拼接时为 ENAMETOOLONG。记录包含点名称，空批次仍计迭代预算。可见目录观察只将 ENOENT/ENOTDIR 视为缺失，权限与 I/O 错误原样返回。

每个目录枚举成功才开始安装其子项，但父规则和此前子项可能已经发布。源树内容继续可变，未实现整树 prepare/commit 或失败回滚；不以全局 clear 清理局部失败。

源码：[捕获及元数据](../../kernel/sumh/core/sumh_runtime.c)、[捕获合同](../../kernel/sumh/core/sumh_store.h)、[MERGE 物化](../../kernel/sumh/overlay/sumh_overlay.c)、[接口](../../kernel/sumh/overlay/sumh_overlay.h)。使用说明同步[模块规则](../zh/guide/sumh-module-rules.md#源对象身份与更新边界)、[重建指南](../zh/guide/sumh-module-rebuild.md)、[场景指南](../zh/guide/sumh-usage-scenarios.md)与[验证矩阵](../zh/guide/sumh-validation.md)。

## 验证记录

| 检查 | 实际结果与证据 |
| --- | --- |
| 实际源捕获与元数据 | [新 runner](../../scripts/ci/test_sumh_source_capture.py) **1 组通过，含 7 类场景**。直接提取捕获、元数据填充、两个新入口、旧入口和释放函数；覆盖普通源替换、借用引用、挂载身份、旧 FD、预期叶比较、原生链接委托与错误、属性回退、类型门禁、重复初始化、可选分配及所有引用回收。`out/batch28-source-capture-tests.log`。 |
| MERGE 物化与安装 | [安装 runner](../../scripts/ci/test_sumh_merge_install.py) **1 组通过，含登记/物化/边界 3 类场景**。执行生产物化段、真实子项 ADD、store 与顶层 ioctl；目录模型支持独立 file/cookie 和迭代锁防护，验证完整 EOF、持有父目录、实际 inode 类型、嵌套重复、循环/深度、共享递归预算、可见路径错误及逐分配点清理。元数据服务是替身，实际元数据由上一组验证。`out/batch28-merge-install.log`。 |
| 相邻 ADD 与回收 | [ADD/store](../../scripts/ci/test_sumh_source_identity.py) **1 组通过**；[core](../../scripts/ci/test_sumh_core.py) **2 种 idmap 布局各 30 项通过**。`out/batch28-source-identity.log`、`out/batch28-core.log`。 |
| MERGE 读取快照与消费者 | [快照 runner](../../scripts/ci/test_sumh_merge_snapshot.py) **2 组通过**，保留已持有根、全目标收集、名称与容量、退休引用及活动消费者验证。`out/batch28-merge-snapshot.log`。 |
| 公共断言工具 | [新工具自测](../../scripts/ci/test_sumh_host_harness.py) **3 个故意失败控制均被拒绝，1 个成功/单次求值对照通过**。错误行定位也核对。`out/batch28-host-harness.log`。 |
| 相邻目录与查找 | 公共断言工具修改后重跑 VFS 4 组、目录流 1 组、名称/虚拟目录 6 组、lookup 13 组，**共 24 组通过**；另有 8 项预期不兼容声明拒绝与 1 项 bootstrap 接线检查。`out/batch28-vfs-helpers.log`、`out/batch28-adjacent-tests.log`。部分名称用例复用收集器，不重复宣称新增覆盖。 |
| ARM 提取代码编译 | 源捕获、ADD、MERGE 安装、collector、active caller 各 ARM32/ARM64，**10 个对象以 -Werror 编译通过**；断言生产函数原文存在，collector 使用实际 unsigned-long 指针表达式。`out/batch28-arm-capture.log`、`out/batch28-arm-install.log` 及各 manifest。没有链接真实内核。 |
| CI 接线 | capture 与 host-harness 两个 runner 登记到 [CI](../../scripts/ci/sumh_ci.py)，**7 项 CI 自测通过**。`out/batch28-ci.log`；missing.py 是预期失败子进程，最终结果 OK。 |
| 独立复核 | `out/batch28-source-review-report.md` 关闭 R1/R2，确认本批范围无剩余阻断问题；包含已审阅文件 SHA-256。静态复核与运行日志分别保存。 |

工具链：捆绑 Python 与 NDK 30.0.16248370 Clang 21。宿主为 Windows x86_64 freestanding 无 CRT 模型；原生 runner 使用 `python scripts/ci/<runner>.py --cc <clang>`，CI 自测使用 `python scripts/ci/test_sumh_ci.py`。ARM 重现脚本为 `out/batch28-arm-capture.py`、`out/batch28-arm-install.py`，精确编译命令、生成 C 与对象 SHA-256 见对应 `out/batch28-arm-*-evidence/manifest.json`。本批没有执行 Linux 全套或远端 CI。

最终检查由 `out/batch28-final-check.py` 核对 54 个任务 ID、文档链接、已审阅源码指纹、当前日志与对象来源，并执行 whitespace/diff 检查；结果保存于 `out/batch28-docs-final.log` 和 `out/batch28-final-manifest.json`。

## 审查与反例

实际捕获测试在旧实现中注入源 getattr 期间的路径替换，复现普通源对象与后来符号链接混配；当前实现通过同一反例。旧实现预期失败与当前通过分别保存于 `out/batch28-source-capture-baseline.log`，不改写旧批次验证记录。

审查 R1：公共宿主 CHECK 原先直接返回源码行号，POSIX 将退出状态截断为 8 位，行号为 256 倍数时可能误报成功。公共工具改用始终非零的失败码并同步候选行诊断，增加故意失败与成功对照。Windows 先前测试保留完整退出码，此发现不将其历史结果改判为通过或失败；修正后的实际运行另记本批日志。

审查 R2：首次通过的 MERGE 模型尚未覆盖新的可见路径权限/I/O 错误，也仅验证根层预算耗尽。补充父层消费最后额度、嵌套子层明确拒绝的记录/名称/次数/目录测试，核对清理后临时字节归零及已发布父项保留；同时覆盖嵌套打开失败、超长/负长度名称和消失子项。最终通过日志在这些补充之后生成。

另在生成的 C 测试输入中故意将相对持有父目录查找退回词法路径查询、将完整枚举退回单次迭代，两种退化均被真实物化断言拒绝。`out/batch28-merge-install-sensitivity.py`、`out/batch28-merge-install-sensitivity.log` 保留结果；生产源码未为此修改。这些预期失败不是内核设备测试。

## 剩余边界

- 符号链接的两次原生查询不原子，检查后变化及 ABA 仍未解决；单目录完整枚举也不冻结内容。持有对象不固定源字节、挂载策略、可见元数据或控制器计划，不撤销既有 FD。
- 普通 ADD 的可选源标记/目录挂接保留原有尽力执行行为；MERGE 子项挂接失败会传播，但此前发布不回滚。首次全局 reset、模块 owner、generation 冲突、派生状态发布和精确恢复仍属 C06。
- 宿主模型使用 VFS、目录锁、路径、分配器和引用替身。ARM 提取代码仅为对象编译，不是完整内核构建或链接；没有执行真实 KMI/CFI、SELinux/idmap、RCU/SRCU 并发、lockdep/KASAN/KCSAN、真实 namespace/卸载、设备观察者或性能验收。
- 本批无控制器/Manager 修改，不重跑已关闭的第二十六批或 Gradle。历史批次静态检查与本批测试证据分别保留。

下一阶段继续处理原生链接捕获与派生状态的 owner 生命周期，再完成 C06 一致发布及 F06/F07 消费；不通过扩大 IPC 或提前开放实验入口绕过前置工作。
