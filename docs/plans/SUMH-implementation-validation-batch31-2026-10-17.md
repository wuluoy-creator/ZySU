# SUMH 第三十一批实施与验证记录

日期：2026-10-17。对应[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)和[54 项账本](SUMH-implementation-progress-2026-10-16.md)。本批继续 C06，补符号链接源捕获的后置检查。完整原子配对和模块 owner 事务仍未完成，C06 保持部分实施。

## 实施合同

普通非链接源继续一次原生 nofollow 查找，元数据及规则自己的引用均来自持有对象。符号链接保留原生 FOLLOW，在其成功后，按原路径再做一次 nofollow；比较完整 mount+dentry，不相同返回 ESTALE，查询失败原样返回错误。后置检查通过才填充条目元数据、分配 canonical 描述和取得条目引用。所有临时查询引用在对应出口释放；预期叶仍是借用引用。

SUMH 不在失败后增加重试、手写链接解析或以持有父目录作为跟随查询的根。原生路径查询内部的 RCU/refwalk 和重验证行为不变。没有新增 UAPI、能力位、动态符号或私有内核布局依赖。初始 nofollow 因末尾斜杠返回目录时，继续使用普通对象分支。

此检查只拒绝在末次查询仍能观察到的链接替换、挂载对象变化或查找错误。A→B→A、动态链接、祖先/目标链变化、进程根变化及检查之后的变化仍可能通过，不能据此证明链接和目标来自一次原子观察。持有对象不冻结文件内容或源树。MERGE 父项和此前兄弟项可能已经发布，返回错误不等于整次操作零修改，也不触发全局清理。

源码：[捕获实现](../../kernel/sumh/core/sumh_runtime.c)、[入口合同](../../kernel/sumh/core/sumh_store.h)、[MERGE 调用点](../../kernel/sumh/overlay/sumh_overlay.c)。使用边界见[模块规则](../zh/guide/sumh-module-rules.md#源对象身份与更新边界)与[模块重建记录](../zh/guide/sumh-module-rebuild.md)。

## 设计检查

`out/batch31-design.md`、`out/batch31-review-design.md`、`out/batch31-test-design.md` 分别保存实现、独立兼容性审查和反例设计。静态检查使用已缓存的 Android/common 6.1 与 6.18 namei 源码：`kern_path` / `filename_lookup` 返回最终路径，未发现可直接复用且同时返回末尾链接及目标的受支持接口；`vfs_path_lookup` 的指定目录成为解析根，会改变绝对链接和 `..` 语义。直接读取链接文本也不能等价执行原生安全策略及 magic link 跳转。该结论不代替真实 KMI 构建或运行验收。

## 验证记录

| 检查 | 实际结果与证据 |
| --- | --- |
| 实际捕获/元数据/释放 | [捕获 runner](../../scripts/ci/test_sumh_source_capture.py) **1 组、10 类场景通过**。包括 12 种持续替换组合、14 种后置错误组合、完整挂载身份、借用引用、原错误传播、失败条目不变及无提前元数据工作。原有普通源、属性回退、类型和可选分配继续通过。`out/batch31-source-capture.log`。 |
| ADD 失败保留旧规则 | [ADD/store runner](../../scripts/ci/test_sumh_source_identity.py) **通过**。七种 capture errno × 有/无旧规则共 14 种组合，实际 ioctl/store 保留原节点、源 shadow、规则数量、标志及引用余额，失败不退休旧项。该层 capture 是替身，实际 capture 在上一行执行。`out/batch31-source-identity.log`。 |
| MERGE 部分发布 | [安装 runner](../../scripts/ci/test_sumh_merge_install.py)及[用例](../../scripts/ci/test_sumh_merge_install_cases.py) **通过**。七种 checked capture errno × 有/无已发布根 × 有/无先前兄弟项共 28 种组合，错误原样上送、失败子项不存在、此前根/兄弟项保留。该结果验证部分发布边界，不是事务回滚。`out/batch31-merge-install.log`。 |
| 相邻规则/快照生命周期 | [core](../../scripts/ci/test_sumh_core.py)两种 idmap 模型 **各 30 项通过**；[MERGE 快照](../../scripts/ci/test_sumh_merge_snapshot.py)及实际消费者 **2 组通过**。`out/batch31-core.log`、`out/batch31-merge-snapshot.log`。 |
| 断言工具 | [工具自测](../../scripts/ci/test_sumh_host_harness.py) **3 个故意失败均被拒绝，1 个成功对照通过**，避免退化用例因退出码被截断误报成功。`out/batch31-host-harness.log`。 |
| 修改前反例与退化检查 | 保存的修改前捕获函数在“持续链接替换应返回 ESTALE”这一独立断言上失败，当前函数通过同一反例。runner 的 `--sensitivity` 将七种退化仅注入生成 C，**7 种均编译成功并被运行断言拒绝**，不修改生产源码。`out/batch31-source-capture-baseline.py`、`out/batch31-source-capture-baseline.log`、`out/batch31-source-capture-tests.log`。 |
| ARM 提取对象 | 同一生产捕获函数原文与测试服务模型以 `-Werror` 编译为 ARM32/ARM64，**2 个对象通过**。`out/batch31-arm-capture.py`、`out/batch31-arm-capture.log` 和 `out/batch31-arm-capture-evidence/manifest.json` 保存命令与输入/对象哈希；未链接真实内核。 |
| 独立代码复核 | `out/batch31-review-report.md` 对照修改前文件核对控制流、清理、反例和指南合同，记录实际审阅文件哈希。设计静态依据与执行日志分开保存。 |

六个宿主 runner 的最终集成命令、日志哈希及执行前后源码一致性保存在 `out/batch31-tests-manifest.json`，由 `out/batch31-run-checks.py` 生成。宿主为 Windows x86_64 freestanding 模型；工具为捆绑 Python 与 NDK 30.0.16248370 Clang 21。可从仓库根目录运行 `python scripts/ci/test_sumh_source_capture.py --cc <clang> --sensitivity`；其他五个 runner 使用 `--cc <clang>`。常规 CI 继续运行现有 capture runner 的全部行为用例，`--sensitivity` 为额外反例模式，没有新增 CI 接线。

七种退化分别为：删除后置检查、只比较 dentry、后置查询错误使用 FOLLOW、吞掉后置 errno、漏释放后置引用、错误分支漏释放 followed 引用、提前执行元数据捕获。编译/链接失败不会被计作预期断言拒绝。A→B→A 和检查之后变化的成功用例刻意保留，不能计作原子性通过。

初次新增 ADD 测试错误地要求累计 `path_get` 次数不变；实际调用会先查询可见路径并释放临时引用。已改为检查尚持有引用的余额、旧对象和发布副作用。原测试编写失败保留在 `out/batch31-source-identity-first.log`，最终通过来自独立重跑日志，未据此修改产品行为。未改写第二十八、三十批历史验证或产物。

`out/batch31-final-check.py` 核对全部 54 个任务 ID、当前文档链接、修改前副本、源码审阅指纹、宿主/ARM 输入一致性与 `git diff --check`；最终记录为 `out/batch31-docs-final.log` 和 `out/batch31-final-manifest.json`。本批没有设备测试、完整内核构建或性能通过结论。

## 剩余边界

- 后置比较不是原子链接捕获，不消除 ABA，也不固定控制器计划、源字节、目录内容、挂载策略或既有 FD。
- 宿主测试中的路径、文件系统、安全策略、分配和引用服务使用替身。ARM 提取对象编译不等于完整内核构建、链接或 KMI/CFI 验证。
- 真实 namespace/挂载/SELinux/idmap/magic link、并发 rename/revalidation、lockdep/KASAN/KCSAN 和性能测试仍待执行。
- owner、expected generation、派生状态一致发布和精确恢复仍属 C06；F06/F11/F12 和 F07 激活不因此开放。本批不修改 Manager 或 userspace，不重复执行未变更组件的构建。

下一阶段继续设计派生状态的所有权与发布生命周期；原生原子配对需有受支持的内核集成点或明确收窄的新接口合同，不能靠重复路径查询宣称完成。
