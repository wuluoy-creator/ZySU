# SUMH 第四十批实施与验证记录

日期：2026-10-19（Asia/Shanghai）。本批推进 C06 的目录缓存发布与打开一致性，保留全部 54 项任务。源码设计见[目录缓存设计](SUMH-dirhijack-cache-design-2026-10-19.md)。本批已完成下列源码、主机模型和交叉对象检查；完整任务及设备验收尚未完成。

## 已实施范围

`sumh_dirhijack.c/.h` 增加父 inode 准备屏障、PREPARED 原生缓存治理、共享投影物化、受管 atomic_open、带引用保护的源身份及发布后核对。原生非目标操作保留调用与返回语义；选中映射失败不再静默转向原生目标。CI 注册新的生产代码缓存测试。

相邻测试增加必要 API/类型适配，仍运行实际生产函数体。原先依赖 vpath 临时复制的失败用例改为当前虚拟投影分配失败语义；生产代码现在由 payload 直接保持 vpath 生命周期。旧批次输出和结论保持原样。

## 执行记录

| 检查 | 实际结果 | 证据 |
| --- | --- | --- |
| 新缓存生产代码套件 | 24 个命名场景分别通过 6.12/6.18 d_revalidate 签名模型；12 个定向变异、10 个旧实现反例均编译成功并在唯一指定语义断言返回 253 | [cache-a3 manifest](../../out/batch40-cache-a3/manifest.json) |
| 原有归属/生命周期 | 8 个完整生产代码视图、1,000 次模型复用循环、28 个定向变异通过 | [ownership-a3 manifest](../../out/batch40-ownership-a3/manifest.json) |
| 相邻回归 | 13 个程序通过：注入归属、源捕获/身份、MERGE 安装/快照、FOP 生命周期、目录名称/流、查找兼容、运行状态、规则枚举、功能就绪及 CI；CI 内含 7 项门禁测试 | [adjacent-a3 manifest](../../out/batch40-adjacent-a3/manifest.json) |
| ARM 模型对象 | 10 个正向视图的 ARM64 对象与 8 个适用视图的补充 ARM32 对象编译通过；SU/bootstrap 的 bit-63 产品代码不作为 ARM32 支持目标 | [arm-a1 manifest](../../out/batch40-arm-a1/manifest.json) |
| 源码复核 | 主任务复核、作者自审、独立测试作者交叉审查；发现并修正早期失败元数据引用泄漏 | [主任务审查](../../out/batch40-root-final-review.md)、[作者自审](../../out/batch40-independent-review.md)、[独立产品复核及修正验证](../../out/batch40-tests-independent-product-review.md) |

最终缓存场景明确区分发布前与发布后撤销，覆盖普通 lookup/atomic_open、输入/alias、PREPARED 正负项、原生错误及哨兵、O_CREAT/O_EXCL/O_TRUNC 传递、源 A→B 同 inode 编号、payload 引用/内存复用，以及观察者改变后 EBUSY/ENOMEM 的定向清理。模型只验证 SUMH 回调合同，不声称实际执行了 Linux 的完整创建、截断、文件系统或并发流程。

上述最终 manifest 核对生产/测试输入的前后指纹；缓存 suite 另外核对保留的旧源码。交叉编译读取已通过主机运行的生成 C 文件，并核对其哈希。ARM32 是补充模型检查，产品 Kbuild 仍只支持 ARM64。没有执行 Manager/APK 构建：本批未改动 Manager 或用户态产品代码。

本批前置快照包含 323 个文件，见 [before manifest](../../out/batch40-before-manifest.json)，其 SHA256 为 `22a6b008d70b64637e1581b29c3892c18439d67dbdffca1b288b6afa46595f81`。最终范围与证据哈希见 [Batch40 收尾清单](../../out/batch40-final-manifest.json)。Batch34–39 已冻结证据没有重跑或覆盖。旧的 a2 结果保留为历史，它不包含最后两个新场景及 P2 修正，不能代替 a3。

## 失败与修正记录

- 独立测试作者复核确认 P2 引用泄漏：应用策略在原生负项重验证与 atomic_open 之间变化，早期 ENOMEM/不可变元数据 EBUSY 返回可能留下已移出缓存的 dget。产品已统一早期失败的异步退休路径，并补针对性回归；规则仍附着时即可释放旧元数据，避免依赖后续 clear。
- 新缓存套件 a1 在 Windows 主机发生访问异常。测试把 Linux 的 `-1UL` 哨兵直接移到 LLP64 环境，生成了非负的指针宽度值；改为 `ERR_PTR(-1)` 后对应回归通过。产品没有因此修改。
- ownership a1 的 su 模型重复定义 inode 锁服务；统一到公共 API 适配后 a2 通过。
- adjacent a1 的四个程序因读取新增 payload 引用字段而缺少测试模型 `refcount_t` 定义，属于编译失败，未计为通过。补齐 stream/snapshot 两处类型适配后重新执行。
- 开发 smoke-a3 的 payload 测试在已完成退休之后检查元数据。修正为真实 commit 后延迟退休的调度，并验证最后引用及内存复用；失败尝试保留。

## 未执行与剩余门禁

未执行真实内核/KMI 构建链接、CFI/lockdep/KASAN、FUSE/其他真实文件系统并发、设备安装、普通应用/授权应用/root/isolated 观察者、重启恢复、内存压力和性能采样。当前机器没有已验证可用的 Linux/adb 环境或已准备内核构建输入。

FUSE READDIRPLUS、纯虚拟后代缓存和原生 hash→return 快速读取窗口不在本批闭合范围。完整模块 owner 事务和原子替换仍未实现，已有 FD 仍固定源身份。C06 保持部分实施，F06/F11/F12 不开放，整体目标继续进行。
