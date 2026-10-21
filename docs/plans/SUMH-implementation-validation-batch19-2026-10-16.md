# SUMH 第十九批实施与验证记录

日期：2026-10-16。对应[54 项实施账本](SUMH-implementation-progress-2026-10-16.md)和[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)。本批推进 K01 的快照、流缓冲及工作区统一预算，代码、主机验证及文档已闭合。整体目标保持进行，K01 完整内核/设备验收尚未闭合。

## 实现范围

共用 16 MiB 请求分配字节预算，覆盖快照对象、两份快照缓冲、Maps/分组流缓冲及临时过滤/归一化数组。计费叶子锁只保护固定长度统计，不分配、释放或调用原生文件操作。扩容先预留整个新容量，旧分配释放后再退账；分配器失败释放自身预留，最终引用释放真实持有容量。开关和 generation 不重置存活计数。

Maps 与分组视图终止性失败清理持有缓冲，保留既有错误/重试边界；用户复制 `EFAULT` 保留可重试数据。既有 hooks 文本增加 requested-memory 统计和明确范围，未新增 UAPI 或扩大响应预算。

源码：[共享计费](../../kernel/sumh/core/sumh_proc_memory.c)、[接口](../../kernel/sumh/core/sumh_proc_memory.h)、[快照与工作区](../../kernel/sumh/features/sumh_fake_mountinfo.c)、[流缓冲](../../kernel/sumh/hooks/sumh_proc_read_hooks.c)、[诊断](../../kernel/sumh/control/sumh_ioctl.c)。用法见[proc 内存预算](../zh/guide/sumh-proc-memory.md)。

## 验证记录

| 检查 | 实际结果与证据 |
| --- | --- |
| 新增生产函数测试 | `scripts/ci/test_sumh_proc_memory.py` **7/7 组通过**：共享账本/获取顺序模型、流分配/完整扩容峰值、快照真实读取增长与引用回滚、公开流读取错误/EFAULT、临时工作区、诊断格式边界、最终 close 与 fops 交接。`out/batch19-tests-final.log`。 |
| 挂载视图回归 | `scripts/ci/test_sumh_mount_views.py` **4/4 组通过**；共享内存合同接入既有夹具，新增实际 read/read_iter/splice 的 ENOMEM/EIO/缺失快照分支，未调用原生读取或 splice。`out/batch19-mount-verified.log`。 |
| 能力/Maps/状态回归 | native namespace 1 组、feature readiness 1 组、Maps 2 组、runtime 1 组、inventory 1 组执行通过；runtime/inventory 各 2 组 ARM32/ARM64 ABI 编译通过。`out/batch19-{namespace-final,readiness,maps,runtime,inventory}.log`。 |
| CI 接线 | 新 runner 已登记到 `scripts/ci/sumh_ci.py`；7 项 CI 自检通过，`out/batch19-ci-tests.log`。其中 missing.py 输出是预期负向用例。 |
| 交付检查 | 54 个唯一任务 ID 与账本一致、本地文档链接及 `git diff --check` 通过；`out/batch19-final-{docs,diff}.log`。`out/batch19-final-sources.json` 记录 11 个实现/测试文件的字节数与 SHA-256，明确是源码指纹，没有内核产物。 |

共 **17 组宿主执行和 4 组 ARM ABI 编译**，不含真实内核构建。各原生脚本使用 `--cc C:/Users/29941/.codex/tools/android-sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe`；Python 来自工作区已有运行时。Windows 宿主以 `-Werror` 编译提取的生产函数并执行。具体命令和替身范围见 `out/batch19-tests-status.md`。本批未修改 Manager，不重复运行第十八批已通过的 Gradle 套件；用户态构建也不能证明这些内核文件已编译。

## 复核、修正与测试边界

两路独立源码复核没有发现本批可操作的正确性缺陷：`out/batch19-memory-independent-review.md`、`out/batch19-proc-independent-review.md`。检查了分配前预留、释放后退账、原缓冲完整峰值、两次数组分配的部分失败、最后引用清理、错误保持、锁范围和诊断剩余容量；不把未运行的真实锁竞争记为通过。

首轮新增测试夹具遗漏 stream-reserve 适配器，补齐提取后通过；splice 夹具使用了错误的锁变量名，修正后 4 组通过。相关日志分别保留 `out/batch19-tests-initial.log`、`out/batch19-mount-splice.log`。namespace 回归发现旧夹具没有同步运行状态等能力位，改为读取真实 UAPI 常量；未改产品能力值，初次失败保留 `out/batch19-namespace.log`。逐步扩展的中间通过记录也保留，最终结果以表中日志为准。

新测试使用真实计费 helper、结构体/容量常量和所验证的生产函数；追踪分配器在每次分配/释放回调核对预留及峰值。快照组执行真实 render/read/get/put，filter/normalize 在该组以返回边界替身注入，另在工作区组执行其真实函数。流读取组的 Maps 格式器和原生读取是故障替身。最终释放组执行真实 proxy release、snapshot put 与 stream free，SRCU/list/fops/VFS 是模型。诊断 formatter 使用宿主 scnprintf 替身并检查 0–514 字节 canary。锁获取顺序模型、模拟最终 fops_put 和 ARM ABI 编译均不能替代真实内核并发、模块引用或四 KMI 构建。

## 保留范围

K01 仍部分实施。逐文件 proxy 对象暂未计费：现有 void `fd_install` 入口忽略安装失败，直接新增预算拒绝会保留原生视图；需要先设计可靠拒绝及原始 release/fops/模块引用生命周期。本批保留安装路径，不宣称完整 fail-closed 入口。预算为 SUMH 请求容量，排除分配器取整/元数据、原生 VFS/seq 与 proxy 对象，不等于物理内存总上限。

真实内核构建、四 KMI、设备多读者/普通应用/isolated 观察者、lockdep/KASAN/真实并发、页大小和性能基线尚未执行。未提交、推送、部署或修改真实设备。
