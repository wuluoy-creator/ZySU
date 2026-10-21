# SUMH 第三十二批实施与验证记录

日期：2026-10-17。对应[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)与[54 项账本](SUMH-implementation-progress-2026-10-16.md)。本批继续 C06，修复目录遍历缓存的 inode/superblock 生命周期前置问题。代码、主机验证与独立复核已完成；C06 保持部分实施，真实内核和设备验收继续保留。

## 实施合同

每个新发布的 FOP inode 缓存条目独立持有一个 inode 引用及一个 superblock 活跃引用。安装调用者仍须持有原生活跃路径或等价保护；在 FOP spinlock 外取得 `s_umount` 读锁，检查 `SB_BORN` 并用 `atomic_inc_not_zero` 取得活跃引用，拒绝从零复活。未出生或活跃计数已为零返回 ESHUTDOWN。之后取得 inode 引用，进入原发布流程；重复竞争、就绪门关闭、原操作表变化和其他未发布出口均平衡引用。

失败回收与正常退休先 `iput(inode)`，再对条目保存的精确 superblock 执行 `deactivate_super`；两者均在 FOP spinlock 和 `s_umount` 外执行，不重新解析路径。没有持久 mount 引用，也没有新增动态符号、UAPI 或能力位。

保留已有退休合同：先撤下条目并恢复仍属于自己的 inode 操作表，再等待 Tasks RCU、RCU 与目录客户端 SRCU，最后释放 inode/superblock。运行 CLEAR 保留仍有专用客户端的条目及其引用；解绑后由后续既有清理/停止边界退休。共享模板按原操作表身份复用，模板及原操作表所属模块引用继续保留至 provider 退出，供旧文件句柄使用。外部已经替换的操作表不被覆盖。

源码：[实现](../../kernel/sumh/features/sumh_fop_override.c)、[内部入口合同](../../kernel/sumh/features/sumh_fop_override.h)。该修复不增加用户操作；既有重建、失败处理和撤销范围见[模块重建指南](../zh/guide/sumh-module-rebuild.md)。

## 静态检查与范围选择

初始注入归属探索分别记录于 `out/batch32-kernel-design.md`、`out/batch32-tests-design.md`、`out/batch32-review-design.md`；这些是后续设计，不是已实现能力。审查发现 retained FOP 只有 inode 引用，尚不能独立保证 superblock 在退休前存活，因而先拆出本批修复。冻结合同为 `out/batch32-contract.md`。

当前调用者来自成功原生路径查找后的注入标记和 dirhijack 安装；所有安装路径在调用期间保留路径或对应活跃引用。运行清理在 config 锁外依次清理 dirhijack、FOP 和 SOP；停止顺序先关闭准入，再退出相关 provider。调用者与锁序静态记录为 `out/batch32-root-lifetime-trace.md` 和 `out/batch32-review-callers.md`。这些检查不代替目标内核 lockdep 或卸载验证。

## 验证记录

修改前文件保存在 `out/batch32-before/`，清单为 `out/batch32-before-manifest.json`；历史第三十一批证据不重写。当前主机测试与对象编译结果如下。

| 检查 | 实际结果与证据 |
| --- | --- |
| FOP 生命周期实际函数体 | [测试入口](../../scripts/ci/test_sumh_fop_lifetime.py)移除内核 include 后编译完整生产翻译单元，使用生产回调 typedef；[服务替身](../../scripts/ci/test_sumh_fop_lifetime_model.py)追踪 inode/superblock/module 引用和释放上下文。[25 个场景](../../scripts/ci/test_sumh_fop_lifetime_cases.py)通过，含分配失败、关闭准入、重复发布、共享 superblock/模板、活动客户端、旧 FD、外部操作表替换及 1000 次实际安装/CLEAR 循环。日志 `out/batch32-fop-final.log`。 |
| 退化检出与修改前反例 | 17 个仅修改临时生成源码的行为变异全部编译成功，并被执行断言拒绝；编译/链接失败不算检出。修改前生产函数体在调用者放下最后一个原生引用后触发缓存 inode 仍存活、superblock 已关闭的模型反例；当前函数体通过对应场景。 |
| 相邻回归 | 目录流实际回调、VFS 辅助逻辑 4 组、两种 idmap 布局各 30 项 core 检查通过；宿主断言工具拒绝 3 个负对照并接受 1 个正对照。四个入口的命令、源码指纹、退出码和日志见 `out/batch32-adjacent-manifest.json`。 |
| CI 接入 | 新生命周期入口登记到 [SUMH CI runner](../../scripts/ci/sumh_ci.py)，默认运行行为变异；7 项 CI 辅助测试通过。`missing.py` 输出来自既有预期失败子进程用例，最终 unittest 为 OK。日志 `out/batch32-fop-ci-registration-final.log`。 |
| ARM 对象 | 完整生产 FOP 函数体加服务替身以 `-Werror` 分别编译 ARM32/ARM64 对象；最终测试替身调整后重新生成并编译。输入、生成源和对象指纹见 `out/batch32-arm-fop-evidence/manifest.json`。这仅是模型翻译单元的对象编译，没有内核链接、KMI 或设备结论。 |

生命周期命令为 `python scripts/ci/test_sumh_fop_lifetime.py --cc <clang> --baseline out/batch32-before/kernel/sumh/features/sumh_fop_override.c`；CI 辅助测试使用 `python scripts/ci/test_sumh_ci.py`。实际 Python/Clang 路径、完整命令与边界记录在 `out/batch32-tests-report.md`；最终输入和日志指纹见 `out/batch32-tests-hashes.json`。

初次模型编译因 Windows LLP64 的指针转 unsigned long 警告失败；仅在 Windows/Clang 测试替身中抑制此诊断，完整指针比较仍决定条目身份，产品类型未改动。初次以根目录 `-m unittest` 调用 CI 辅助测试因同目录导入路径失败，改用既有直接脚本入口通过。这两份失败日志保留为 `out/batch32-fop-first.log` 与 `out/batch32-fop-ci-registration.log`，不冒充产品测试失败或最终通过证据。

独立源码审查与测试审查未发现本批范围内的阻断问题，分别记录在 `out/batch32-review-report.md` 与 `out/batch32-review-tests.md`；根任务整合复核见 `out/batch32-root-review.md`。最终检查核对 54 个唯一任务 ID、C06 部分实施状态、本地链接、修改前备份、受审源码/测试指纹、实际日志和 ARM 对象来源，并执行 `git diff --check` 与新增源码空白检查。结果清单为 `out/batch32-final-manifest.json`。

## 剩余边界

- FOP inode 缓存仍保留至既有清理/停止边界；活跃引用可能延迟文件系统最终 shutdown。不增加 vfsmount 引用不等于保证所有卸载都成功，也不等于按规则即时回收。
- 注入路径索引、`DIR_HAS_INJECT` 标志、dirhijack 子项及虚拟祖先的完整规则归属未实现。现有注入入口仍可能忽略 FOP 安装错误，路径清理及部分发布限制继续保留。
- 没有实现模块 owner 事务、generation 条件替换、原子源树或精确恢复；F06/F11/F12 与 F07 激活不因此开放。
- 本批没有设备测试、完整内核构建或性能通过结论。真实 KMI/CFI、普通及延迟卸载、freeze、SELinux、旧 FD 并发、lockdep/KASAN/KCSAN 和资源峰值仍待验收。
- Manager/userspace 未修改，不重复构建未变更组件。所有 54 项原范围保留，C06 仍为部分实施。
