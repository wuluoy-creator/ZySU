# SUMH 第二十七批实施与验证记录

日期：2026-10-17。对应[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)和[54 项账本](SUMH-implementation-progress-2026-10-16.md)。本批实施 C06 的源对象生命周期前置项，并更新使用说明；C06 仍部分实施，整体 54 项目标继续进行。未开放 owner 事务、F06 应用、F07 激活或 F11/F12 新接口。

## 实施与行为变化

普通 ADD 已在捕获源对象后持有独立的 followed/nofollow 路径引用，再将规则交给 store。源 inode 标记和目录视图安装使用捕获的路径、类型和虚拟 inode，不再按目标字符串重查。upsert 比较完整 mount+dentry、可选 nofollow 身份及其有效性；同一文字指向新对象或仅挂载身份变化时不会跳过父目录副作用。共享清理点释放安装期间额外取得的引用。

每条 MERGE 保存完整 `target_path`，取代仅 dentry 的引用。目录快照在 RCU 保护下获取自己的路径引用，退出读区后直接打开该对象；file 引用与独立 path 引用都在可睡眠上下文释放。规则退休沿用 RCU→回收队列→工作线程→drain，挂载和目录引用一并释放。

`sumh_capture_merge_target` 一次解析目录根，并在该目录上通过兼容查找接口检查 `.replace`。非目录、SUMH vnode 或 `.replace` 均拒绝；查找适配缺失返回 EOPNOTSUPP，目录 trylock 失败返回 EAGAIN，其他错误传播。失败输出清零并回收引用。新增错误不能解释为整个模块集合未修改。

顶层 MERGE 在发布前完成物化所需字符串分配；分配失败不再静默跳过物化并报告成功。根目录物化借用捕获引用；嵌套登记返回明确错误，真实 filldir 调用方停止并传播。已存在的相同嵌套规则先在锁内取得原根引用，不重新查找已经被替换或删除的路径。仍保留发布前第二次重复检查。顶层重复 MERGE 保持 EEXIST。

源码见 [ADD/MERGE 控制入口](../../kernel/sumh/control/sumh_ioctl.c)、[store 与回收](../../kernel/sumh/core/sumh_runtime.c)、[内部类型](../../kernel/sumh/internal/sumh_types.h)、[目录捕获、收集与物化](../../kernel/sumh/overlay/sumh_overlay.c)、[接口](../../kernel/sumh/overlay/sumh_overlay.h)及 [store 合同](../../kernel/sumh/core/sumh_store.h)。使用变化已同步[模块规则](../zh/guide/sumh-module-rules.md#源对象身份与更新边界)、[重建记录](../zh/guide/sumh-module-rebuild.md)、[场景指南](../zh/guide/sumh-usage-scenarios.md)和[验证矩阵](../zh/guide/sumh-validation.md)。

## 本批验证

| 检查 | 实际结果与证据 |
| --- | --- |
| ADD 安装与 upsert | [新 runner](../../scripts/ci/test_sumh_source_identity.py) **1 组通过**。执行真实 ioctl case 和 store；覆盖捕获后路径替换、五种源类型、符号链接、旧 FD 引用、同路径新 inode、仅 mount/nofollow 改变、未改变重复、分配失败和可选安装失败。`out/batch27-source-identity.log`。 |
| MERGE 注册与物化 | [新 runner](../../scripts/ci/test_sumh_merge_install.py) **1 组通过**。真实 capture、顶层 case、nested helper、materialize 和 filldir；覆盖根替换后 marker 检查仍锚定旧根、错误矩阵、必要分配先于发布、重复嵌套路径消失、发布后打开/迭代错误、递归登记失败及深度上限。`out/batch27-merge-install.log`。 |
| MERGE 快照与实际消费者 | [收集器 runner](../../scripts/ci/test_sumh_merge_snapshot.py) **2 组通过**。导入生产 MERGE 类型；路径在捕获前/后替换不改变已存根，显式替换规则才选新根；RCU 退出时退休和破坏原文本，快照继续有效；两条相同 dentry/不同 mount 分别保留。`out/batch27-merge-tests.log`。 |
| 回收生命周期 | [core runner](../../scripts/ci/test_sumh_core.py) **2 种 idmap 布局各 30 项通过**。实际 source release、退休、RCU 回调队列、sleepable worker/drain；检查挂载和目录引用未提前释放、未在原子区释放及未重复释放。`out/batch27-core-tests.log`。 |
| 相邻目录与查找回归 | VFS helper 4 组、目录流 1 组、名称/虚拟目录 6 组、lookup 13 组，共 **24 组宿主通过**；另有 8 项预期不兼容声明拒绝及 1 项 bootstrap 接线检查。`out/batch27-adjacent-tests.log` 和各分项日志。部分名称用例复用收集器测试，不代表独立的新增覆盖。 |
| ARM 提取代码编译 | ADD、MERGE 安装、collector、active caller 各 ARM32/ARM64，**8 个对象编译通过**，`-Werror`，断言生产函数原文存在；collector 恢复实际 unsigned-long 指针表达式。`out/batch27-arm-install.log`、`out/batch27-arm-collector.log` 及对应 manifest。未链接真实内核。 |
| CI 入口 | 两个新 runner 登记进 `scripts/ci/sumh_ci.py`；现有 CI 自测 **7 项通过**，`out/batch27-ci.log`。missing.py 是预期失败子进程，最终 OK。 |
| 独立复核 | `out/batch27-owner-review-report.md` 未发现本批范围剩余阻断问题，记录源码指纹、已关闭 R1 及剩余边界。 |

目录收集测试继续覆盖 6/8/1024 个来源、1025 拒绝、动态重复名、路径别名、字符 whiteout、空集合、每个遇到的分配失败点、打开/迭代错误、cookie 和完整输出顺序。空或半截规则路径返回 ESTALE，缺 inode 返回 ENOTDIR，无字符串回退。4 MiB 分配预算和目标/名称/记录/迭代次数限制不变；移除字符串副本后，以实际长名称工作区耗尽继续验证容量拒绝。

工具链为捆绑 Python 与 NDK 30.0.16248370 Clang 21。宿主运行使用 Windows x86_64 freestanding、无 CRT 的提取模型；路径、锁、RCU、VFS 和分配器均为替身。新安装 runner 的完整命令为 `python scripts/ci/test_sumh_source_identity.py --cc <clang>`、`python scripts/ci/test_sumh_merge_install.py --cc <clang>`；相邻 runner 同样使用 `--cc`。ARM 重现脚本为 `out/batch27-arm-install.py`、`out/batch27-arm-collector.py`，命令、源码与对象 SHA-256 保存在各证据 manifest。

## 复现与审查修正

新增 ADD 测试先在本批修改前执行，成功编译后于“目标不应再次解析/目录视图应使用捕获对象”断言失败，保留 `out/batch27-source-identity-initial.log`；修正后通过。它验证安装后身份一致性，未将捕获替身冒充真实 follow/nofollow 捕获。

R1：初版嵌套登记先捕获当前路径，导致已有规则在原路径消失或替换后无法复用其仍有效的旧根。调整为先查已有规则并取得引用，再为新条目捕获；插入前仍检查重复。真实 helper 回归强制目标查找 ENOENT，确认重复条目成功复用旧根且没有增加路径查找。又补充仅 mount、仅 nofollow 变化及未改变重复的父目录副作用计数，避免只比较 dentry 的错误漏检。

文档、54 个唯一任务 ID、链接、当前源码/日志时间和对象指纹由 `out/batch27-final-check.py` 检查，结果保留于 `out/batch27-docs-final.log`、`out/batch27-final-manifest.json`。静态审查报告与运行日志分开保存；失败的首次复现未改写为通过。

## 未完成边界

- 固定引用保证根对象身份，不冻结目录内容、文件内容或挂载策略，也不能回滚旧 FD。初次 ADD followed/nofollow 仍是分开解析，递归子项仍用生成的字符串捕获，完整源树一致性尚未实现。
- ADD 可选源标记和 dirhijack 安装保留原有尽力执行语义；返回 0 不新增“所有机制均安装成功”的保证。MERGE 发布后物化失败可能保留父规则及此前子项；不执行全局 clear 回滚。
- 旧模块首次全局 reset、owner 归属、generation replace、幂等提交查询、派生规则/注入/标志/目录视图一致发布与精确恢复仍属 C06 待办；本批没有新增能力位或改变 UAPI。
- 未进行完整内核构建/链接、四 KMI/CFI、SELinux/idmap、真实卸载/namespace、RCU/SRCU 并发、lockdep/KASAN/KCSAN、设备观察者和性能验收。没有 Manager/控制器改动，因此未重跑已关闭的第二十六批及 Gradle。

下一阶段继续设计相对源目录的子项捕获、符号链接捕获和派生状态 owner 生命周期，再完成 C06 原子发布与 F06/F07 消费。测试模型和引用修正不能替代这些工作。
