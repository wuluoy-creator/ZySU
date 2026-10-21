# SUMH 第二十一批实施与验证记录

日期：2026-10-16。对应[54 项账本](SUMH-implementation-progress-2026-10-16.md)和[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)。本批完成 K02/K08 通用 MERGE 目录收集与去重一致性的代码、主机回归、独立复核及文档。整体 54 项目标和 K02/K08 完整验收仍未完成；以下不代表真实内核或设备验收通过。

## 实施合同

通用目录包装器为每次调用创建完整采样：从已打开的源目录取得路径形式，在 RCU 内复制匹配的规则数据，在外部解析并打开每个来源，完整收集名称后才向用户输出。注入与去重使用同一份收集结果，替代前四个目标的 dentry 缓存查询。目标打开后的替换不改变本次持有的对象，后续调用重新解析；这不是文件内容冻结或跨多次读取的固定视图。

保留现有规则遍历顺序、先进入的同名有效条目、exact/client 组合及输出列表顺序。字符设备 whiteout 和 `.replace` 可以抑制同名原目录项而不发出自身；后续来源的普通同名项仍可参与原有选择。`.replace` 的这一读取行为不表示已支持 opaque 目录替换。源树、子项或 live hide/client 状态在采样期间变化，仍不能宣称全局原子观察。

资源、路径解析、打开、迭代、白化分类和容量错误须在输出前返回并释放所有部分结果；活动调用方沿用第二十批的游标保护。只有完整采样才能进入用户输出和原目录读取；原目录在输出开始之后报错仍遵循原有系统调用行为，不提供回滚已输出条目的承诺。

已实施的单次调用预算为：1024 个目标、16384 个发出/抑制名称节点合计、65536 个原始回调记录、65536 次目标迭代调用、4 MiB 请求分配峰值。按分配请求计费，包括中间数据与同时存活的双份名字、路径；不包含 VFS 内部分配、分配器开销或多个并发调用的全局上限。超出预算返回 E2BIG，不返回成功的前半份列表。旧 UAPI、模块/Maps 的条数和传输预算保持不变。

源码：[完整收集器与 API](../../kernel/sumh/overlay/sumh_overlay.c)、[共享名称消费者](../../kernel/sumh/hooks/sumh_vfs_hooks.c)、[容量](../../kernel/sumh/internal/sumh_base.h)、[包装结构](../../kernel/sumh/internal/sumh_types.h)、[已投影条目生产者](../../kernel/sumh/core/sumh_dirhijack.c)。活动 fop 继续复用第二十批的错误退出与游标恢复。

## 设计复核导致的调整

1. 目标流可能已经包含 SUMH 专用 client 发出的可见 inode。新增明确的单条来源标识，原生 inode 只投影一次，已投影的值原样保留，不以数值范围或稍后查询推断来源。
2. SUMH vnode 目标可能委托回外层已锁定的源目录并改变 actor。当前批次在迭代前明确拒绝此目标，返回 EOPNOTSUPP；完整 backing-chain/cycle 支持仍需后续设计。同一原生 inode 的直接或别名目标在进入嵌套迭代前作为无操作跳过。
3. 只限制回调条数不能约束“无名称但游标持续变化”的迭代器，因此另外限制调用次数。游标不按数值大小解释；无进展且无回调才是本轮 EOF，有回调却无位置进展返回 EIO。
4. 原始名称拒绝嵌入 NUL、斜杠和非法组件长度，其他原始字节保留。精确规则复制到名称索引前也校验组件，不能通过 strlen 静默缩短。

依据为 `out/batch21-{merge-design,lock-decision,design-review,test-design}.md`；锁定合同 `out/batch21-contract.md`。研究不是测试结果。

## 锁与能力边界

目标读取保留 `iterate_dir` 的权限、安全和通知路径。原有“外层源目录锁→目标目录锁”关系没有通过改用原始 fop 绕过；交叉 MERGE 与并发写者的潜在锁等待仍需独立架构处理和 lockdep 验证。每次原生游标续读重新采样会增加该路径调用及成本，不能声称没有锁或性能影响。

白化检查移到目标迭代结束后，基于持有的目标目录和准确子项名称执行。独占 trylock 失败返回 EAGAIN，不循环重试；辅助查找能力缺失返回 EOPNOTSUPP；原生字符项身份或类型变化返回 ESTALE。已投影字符项暂不具备可证明的原始身份，明确返回 EOPNOTSUPP。所检查 Android 6.18 分支已无旧 `lookup_one_len`，不能把新的 `lookup_one` 强转为旧签名；完整 KMI 适配仍待 K09。

## 验证记录

| 检查 | 实际结果与证据 |
| --- | --- |
| 真实收集器及活动调用链提取 | 新增 `scripts/ci/test_sumh_merge_snapshot.py`，**2 组通过**。真实 Part10b、名称索引、专用子项生产者及 prepare→emit/filter→活动 fop 链路；最终 `out/batch21-snapshot-verified.log`。 |
| VFS 辅助回归 | **4 组通过**，保留路径字节、返回指针、分配失败和嵌套所有权检查，适配不透明快照 API。`out/batch21-vfs-final.log`。 |
| 目录流回归 | **1 组通过**，保留 84 种准备失败组合、2 项 HIDE-only OOM、路径和 client/cookie 回归。这里的内容快照为替身，真实收集器由新脚本单独执行。`out/batch21-stream-final.log`。 |
| 名称与虚拟目录 | **6 组通过**，移除旧的吞错成功期望，通用 MERGE 组复用真实收集器的 whiteout 用例，既有名字索引和 4 组虚拟目录用例保留。`out/batch21-names-verified.log`。 |
| CI 注册 | 新 runner 已加入 `scripts/ci/sumh_ci.py`；`test_sumh_ci.py` **7 项自测通过**。日志中的 missing.py 为有意的失败子进程用例，最终结果 OK。`out/batch21-ci.log`。 |
| ARM 提取代码编译 | helper、stream、collector、active-caller 各编译 ARM32/ARM64，**8 组对象编译通过**。恢复原始 unsigned-long 表达式、断言生产函数原文存在，使用 `-Werror`；`out/batch21-arm-verified.log`。未链接真实内核。 |
| 文档与源码指纹 | `out/batch21-final-{docs,diff}.log` 检查 54 个唯一任务 ID、账本一致、相对链接和差异格式；`out/batch21-final-sources.json` 为源码 SHA-256，不是编译内核产物。 |

合计 **13 组宿主执行、7 项 CI 自测、8 组 ARM 对象编译**。重复运行不另计；13 组中两个 runner 复用 whiteout 用例，不代表 13 类完全不同的覆盖。Python 和编译器仍为第二十批记录的捆绑 Python 与 NDK 30 Clang；完整命令及文件指纹见 `out/batch21-tests-status.md`、`out/batch21-tests-final-sources.json`。当前范围没有 Manager/用户态改动，未重复运行 Gradle 或无关套件。

核心覆盖包括六个/八个目标、第六来源动态新增、第五/六来源重复名、冷 dcache、手写完整输出顺序、exact 优先、原生及已投影 inode。真实收集器的集成用例验证单记录分页、零容量拒绝以及八个保存 cookie 的全部预期后缀；原生续读重新解析目标，专用 client 续读保留快速路径。

故障检查遍历成功场景的每个分配点，并注入后期目标解析/打开/迭代失败、无进展回调、畸形组件、六类次级路径异常、whiteout 能力缺失/锁竞争/身份变化。错误须清空输出指针、释放此前目标/名字/原始记录和引用，保留进入活动调用方的游标且没有用户或源目录输出。实际生产容量检查覆盖 1024/1025 目标、65536/65537 回调、迭代上限、16384 名称节点，以及完整链路内存耗尽和计费函数 cap/cap+1/SIZE_MAX。

CLEAR 模型在捕获后的 RCU 退出点破坏规则自己的文本与 exact payload，验证副本仍可解析与输出；持有期间和释放后的 dentry/mount/文件/凭据计数都检查。路径替换在解析前选新对象，在打开后仍使用旧对象。这些是明确时序的 API 模型，不能替代真正的 RCU 并发、挂载或权限测试。

## 复核与修正

独立审查发现原始 `.replace` 字符设备先进入 whiteout 分类，会因缺能力或锁竞争让整个读取失败。新增用例先复现失败，再将标记识别提前；四个原生/投影、辅助函数有/无组合均只覆盖名称，不进入 lookup 或 trylock。显式 exact 规则中的 `.replace` 保持历史语义，这不是新支持 opaque 目录替换。

审查同时补齐 CLEAR 文本失效、次级路径异常、引用存活与完整顺序断言；根代理要求的真实收集器零容量、单记录分页与保存 cookie 后缀也已补齐并重新执行。`out/batch21-independent-review.md` 的最终结论为本批范围内无未解决的实现或测试源码问题。早期夹具缺少 LIST_HEAD/dir_emit 的编译失败和修正前 marker 复现日志均保留，未改写成通过。

宿主执行为 Windows x86_64 freestanding 提取模型，只在夹具中适配 LLP64 指针宽度；ARM 编译使用实际生产表达式。RB 替身保留搜索拓扑而非内核平衡算法，hash 用确定性单桶；引用、锁、路径及 VFS 接口仍为替身。真实内核构建、四 KMI、设备观察者、冷缓存文件系统、并发 CLEAR/rename、lockdep/KASAN/KCSAN、内存压力和性能项目均未执行。未提交、推送、部署或修改真实设备。
