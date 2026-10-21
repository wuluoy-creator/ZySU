# SUMH 第二十五批实施与验证

日期：2026-10-17（Asia/Shanghai）。本批推进 [54 项计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)的 **K05 Maps 查询结构**，整体目标保持进行中。源码与宿主验证已落地，真实内核、并发和设备性能门禁未闭合，K05 保持部分实施。

## 实际实现与兼容变化

新增 [sumh_maps_store.c](../../kernel/sumh/core/sumh_maps_store.c) 和[接口合同](../../kernel/sumh/core/sumh_maps_store.h)，以不可变有序数组保存全部临时和受管规则。索引按 inode、device、来源排序，查询先查精确设备，再查 device 0 通配；同键临时规则优先于受管规则。两类定义均保留供枚举，不只存最终赢家；legacy inode 0、空替换路径及高位 inode 保留原语义。

本批选择短 spinlock 获取引用/交换当前指针，替代原计划的 RCU 回收候选。每 VMA 不加全局锁、不增减引用，使用二分查找；最终释放在可睡眠路径执行，不引入模块卸载后的 RCU 回调或回调中 kvfree。所有 writer 的准备到发布仍依赖既有 control mutation mutex 与 lifecycle 边界，shutdown clear 使用排他生命周期。短锁方案没有被宣称为真实内核并发验证通过。

[proc 代理](../../kernel/sumh/hooks/sumh_proc_read_hooks.c)在第一次有效非零读取固定规则快照，显式记录“已采样空集合”。同一文件对象跨 VMA、原始块、ENOSPC 重滤、短读和用户复制失败重试使用相同集合；EOF 持有至关闭，终止性流错误和最终关闭释放。Maps seek 仍返回 ESPIPE，重新打开并读取才使用新集合。规则替换或清理不追溯撤销旧流，源 VMA 本身仍为动态数据；现有读取入口的开关/scope 检查保持不变，不承诺撤销已开始的 read 或记录读取间未观察到的关开变化。

临时规则新增 1024 个不同键上限，受管 256 条额度独立，合计最多 1280 条；满额仍可更新现有临时键。当前、准备和旧 FD 保留的所有代都计入既有 16 MiB 请求分配预算，新增 maps 分类；受管 ioctl 原始副本计入 workspace。先预留、后分配，实际释放后才退还容量。分类统计仍不包括分配器取整、原生 VFS 或 proxy 对象开销。

[ioctl](../../kernel/sumh/control/sumh_ioctl.c)在完整校验/分配之后发布：legacy 回复复制失败时丢弃准备集合；受管替换保留临时集合；预算、分配和输入错误均保留原集合。临时超额返回 ENOSPC，预算/分配失败返回 ENOMEM。全局 Maps clear 无需分配，但旧流仍占用旧快照；空受管替换若需保留临时项，仍可能因分配失败而拒绝。普通 CLEAR_ALL 没有被改成清空 Maps。枚举仍使用既有 generation/分页合同，排序后的 ordinal 不是稳定 ID。

UAPI 布局与功能协商位没有新增；控制器/Manager 的受管 256 条及 56 KiB 编码限制没有放大。本批未新增临时规则按键删除、Maps 集合与开关事务、永久定义所有权或新应用策略。

## 已执行验证

Windows 宿主使用 NDK 30.0.16248370 / Clang 21；算法基准使用 Zig 0.14.1 的 C 编译器与 `-O2`。锁、refcount、分配器和用户复制为宿主替身；生产函数按当前源码提取。下面的组数有重叠，不合计成互不重复的断言数。

| 验证入口 | 实际结果 | 覆盖与证据 |
| --- | --- | --- |
| [Maps store/ioctl/parser/filter](../../scripts/ci/test_sumh_maps.py) | **4 组通过** | 两组分别在 Windows 32 位 long 与模拟 LP64 执行。16 种精确/通配×临时/受管组合及两种插入顺序；高位/零 inode、空路径、双来源同键、完整容量、各分配/输入/回复故障、持有旧代至预算耗尽及回收后重试。真实 parser/filter 在两条 VMA 之间发布新规则、ENOSPC 后重滤、空集合固定。`out/batch25-maps-final.log` |
| [proc 内存与读取生命周期](../../scripts/ci/test_sumh_proc_memory.py) | **7 组通过** | 实际完整 store 与 read/read_iter/stream_fill/释放函数；初始空/非空、发布发生于重滤、第二/第三原始块、单字节短读、EFAULT、EOF、终止错误、最终关闭和重复释放。格式输出用受控 adapter 注入时序，真实 parser/filter 由上一行验证。`out/batch25-review-memory.log` |
| [规则分页](../../scripts/ci/test_sumh_rule_inventory.py) | **1 组执行及 ARM32/ARM64 ABI 编译通过** | 实际枚举/外层生命周期与 generation 路径，快照 API 替身检查每次获取释放、双来源同键和高位身份。`out/batch25-review-inventory.log` |
| [mount/proxy 回归](../../scripts/ci/test_sumh_mount_views.py) | **4 组通过** | 保留策略、mount 快照、安装时不预取 Maps pin、部分读取与错误。`out/batch25-review-mount.log` |
| ARM 提取对象编译 | **8 组通过** | 实际 store + mutation/filter/stream/close，分别编译 ARM32/ARM64 对象；使用内核服务替身，不是完整内核链接或设备执行。脚本 `out/batch25-arm-compile.py`，最终日志 `out/batch25-arm-compile-final.log` |
| store 独立语法核对 | **ARM32/ARM64 通过** | 完整实际 store 配合声明替身，`-Wall -Wextra -Werror`。`out/batch25-store-compile.log` |
| 核心、guard、能力、状态、kstat 定向回归 | **通过** | core 两种布局各 28 检查；guard 20,000 次空检查零锁；feature-readiness；runtime 执行及 ARM ABI；kstat 两种位宽。`out/batch25-test_sumh_*.log` 与 `out/batch25-kstat.log` |
| [CI 执行器自检](../../scripts/ci/test_sumh_ci.py) | **7 项通过** | Maps、memory、inventory、mount runner 已在既有 CI 清单，新增 C fixture 经对应 runner 使用。日志中的 missing.py 为预期失败测试。`out/batch25-ci.log` |

测试入口示例：

```sh
python3 scripts/ci/test_sumh_maps.py --cc clang
python3 scripts/ci/test_sumh_proc_memory.py --cc clang
python3 scripts/ci/test_sumh_rule_inventory.py --cc clang
python3 scripts/ci/test_sumh_mount_views.py --cc clang
python3 scripts/ci/benchmark_sumh_maps.py --cc cc --output out/maps-benchmark.csv
```

第一次 Maps runner 发现测试提取器误选前置声明，导致 filter fixture 编译失败；已改为识别函数定义的提取器。独立测试审查又发现 Windows 模拟 LP64 的 `~0UL` 仍只有 32 位，已改为类型转换表达式并断言高位值实际超过 32 位，再完整重跑四组通过。初次失败保留于 `out/batch25-maps-first.log`，不当作通过证据。

## 宿主算法基线

[可复现基准](../../scripts/ci/benchmark_sumh_maps.py)保留改动前的实际链表 selector 及源码/函数 SHA，比较当前生产二分查找。旧 selector 只改符号别名与遍历计数；新 selector 只在实际二分循环加入探测计数。先比较返回规则，再测相同混合命中/未命中/通配负载；确认索引查询不获取集合锁。

共 8 档规则数（0、1、16、64、256、512、1024、1280）× 3 档 VMA 数（128、1024、4096），每行重复至 262,144 次查询。CSV、编译参数与源指纹为 `out/batch25-maps-benchmark.csv` / `.json`；原算法单独基线为 `out/batch25-old-maps-baseline.*`。

以下为其中 4096 VMA 档的单次宿主结果，时间包含插桩且分辨率有限：

| 规则数 | 旧链表节点访问 | 索引循环探测 | 旧/新宿主计时 |
| --- | ---: | ---: | --- |
| 1 | 262,144 | 524,288 | 0.001 / 0.001 秒 |
| 256 | 67,108,864 | 3,521,024 | 0.064 / 0.004 秒 |
| 1024 | 268,435,456 | 4,375,680 | 0.306 / 0.012 秒 |
| 1280 | 335,544,320 | 4,679,936 | 0.393 / 0.011 秒 |

小集合不保证减少探测或时间；这些数字不含真实内核 mutex 争用、规则发布成本、完整 proc 格式化和设备调度，不能换算为 Android 延迟提升百分比。T04 的同设备多次 A/B、噪声控制、P50/P95/P99、吞吐/资源/能耗仍待实施。本批没有提交、合并或部署，性能上线门禁仍然保留。

## 独立审查、文档与剩余范围

独立审查覆盖全部 store、ioctl、枚举、首次读取、重滤、终止/关闭与 teardown 路径，目前未发现阻断缺陷；测试审查与生产审查分别保存于 `out/batch25-test-review.md` 和 `out/batch25-independent-review.md`。最终文档与源指纹清单为 `out/batch25-final-manifest.json`，检查日志为 `out/batch25-docs-final.log`。

已同步 [Maps 指南](../zh/guide/sumh-maps-rules.md)、[共享预算](../zh/guide/sumh-proc-memory.md)、[场景 23](../zh/guide/sumh-usage-scenarios.md#23-maps-更新后分别验证旧流与新流)、设备验收表、中文导航、主计划及 54 项账本。指南分别说明当前登记集合、旧流快照、保存定义和开关，不以清理当前规则冒充立即回收或逐 FD 撤权。

尚未执行真实 Linux 内核构建/链接、四 KMI、CFI、lockdep/KASAN、真实多读者与写入竞争、SELinux/普通/授权/isolated 观察者、设备 P99/吞吐或完整 Linux 远端 CI。宿主确定性交错不构成调度器并发证明；请求预算不覆盖全部物理内存。K01 的可靠代理安装失败拒绝入口、C06 owner 事务及全部 54 项剩余范围持续保留。本批未修改 Manager/控制器产品代码，未安装设备或改变设备规则。
