# SUMH 目录缓存发布与打开设计（第四十批）

本文件记录 C06 的静态设计与源码复核；实际执行证据单独保存在[第四十批验证记录](SUMH-implementation-validation-batch40-2026-10-19.md)。它补充[第三十四批归属设计](SUMH-dirhijack-ownership-design-2026-10-17.md)，不改写当时的检查结论，也不代表完整模块事务已经实现。

## 1. 要解决的行为

原生目录查找可能在 SUMH 首次安装前开始、在失效处理后才返回；仅检查已经 attached 的条目，也会漏掉 PREPARED 期间的原生缓存。旧 atomic_open 对映射仍调用原生打开，可能打开或创建原生目标。旧重验证只区分“是否 SUMH vnode”，不能区分替换前后的两个源对象。

本批限定目标是：首次安装等待遵守父 inode 锁的原生回调结束；之后经当前挂接回调返回的目标缓存必须受当前观察者策略管理或被移出缓存；选中的映射通过 SUMH 投影完成 lookup/atomic_open，分配或发布失败明确返回，不转为原生创建。

## 2. 实现合同

| 环节 | 源码行为 | 边界 |
| --- | --- | --- |
| 准备 | `sumh_dh_prepare_observed` 先准备 claim/payload/备用 slot，再按父 inode→DH 顺序安装影子操作表和 PREPARED slot；父锁释放前移除当前同名缓存 | PREPARED 不改变可见规则。安装目录/FOP/SOP 本身仍可在可睡眠锁内分配；不宣称锁内无分配 |
| 原生完成 | `sumh_dh_native_complete` 在回调完成后查询 indexed slot，包含 PREPARED；检查真正返回的输入或 alias，失效/不匹配时移出缓存 | 保留原生返回状态、flags/mode 和已打开文件；不拦截文件系统内部短暂的 hash→return 窗口 |
| 选择与物化 | `sumh_dh_capture_projection` 持有不可变 payload 引用；lookup 和选中的 atomic_open 共用 `sumh_dh_materialize` | 捕获失败与透明原生路径分开；选中后分配失败返回 ENOMEM，发布过期返回错误，不静默降级 |
| 原子打开 | 映射输入先移出哈希，再通过 `finish_no_open` 返回物化结果；隐藏和不可见 su 返回 ENOENT | alias 引用只移交一次；不手动结束 lookup，不伪造 FMODE_CREATED/OPENED。普通打开、排他创建和截断检查仍由 VFS 完成 |
| 身份与重验证 | 元数据不可变地记录 projected、synthetic_negative 和 payload；安装、重用与重验证核对捕获身份 | payload 引用阻止指针被释放后复用为另一身份；受管隐藏的 NULL payload 仍结合隐藏状态和观察者判断 |
| 发布后检查 | `d_add/d_splice_alias` 后再次核对实际结果的元数据、当前 slot 和 payload；失败移出缓存并异步退休 | 防止撤销先恢复 d_op、后到结果又被缓存。不能在活动 SRCU 回调中同步等待自身 |
| 旧回调 | 一次 acquire-load 取得当前 i_op，并由同一读值得到元数据和原始回调 | 避免两次读值间重新安装造成错误回退；恢复后的旧入口仍可委托当前原生回调 |

## 3. 锁与引用

准备者不得已经持有目标父 inode 锁、config 或 DH。普通回调依次使用父锁、DH、dentry 锁；读 slot 时再使用 slot 自旋锁。commit/detach 不反向获取父 inode 锁。缓存引用的 dput 与退休等待移到准备者释放父锁和 DH 之后。

调用点复核覆盖 ioctl 的 ADD/HIDE、MERGE 物化、受管隐藏 resolver 和 su bind。ADD/HIDE 在 config 前准备；MERGE 先结束源目录迭代再安装。受管隐藏和 su 的生产者互斥锁可以先持有，因为被等待的查找/观察者路径不获取这些互斥锁；这属于源码检查，仍需真实 lockdep 验证。

payload 初始由 claim 持有；解析捕获和发布的 d_op 元数据各持有独立引用。claim 的 RCU 释放、解析结束、元数据的 SRCU 退休分别释放自己的引用。目录、dentry 与原始操作表沿用既有生命周期保护；已有目录 FD 继续使用捕获的源对象，不重新解析路径来刷新。

应用策略可在负项重验证和 atomic_open 之间改变，父 inode 锁不序列化应用策略。选中投影后，分配失败、缺少 payload 或初次元数据安装失败，也必须移出输入缓存并安排旧元数据退休；不能只清理已进入 splice 的路径。安装失败时仍可撤销刚取得的非最终 inode/superblock 引用，调用者持有的有效父 path 独立保持它们存活；这与延后可能最终释放的缓存/源 path 和退休等待不同。

## 4. 明确保留的问题

1. FUSE READDIRPLUS 可直接读取/更新缓存及 FUSE inode 元数据，需要专门的入口与来源验证，普通 lookup 修复不能替代。
2. 纯虚拟目录的后代正项、负项及中间目录仍需要自己的规则/源身份协议。
3. 原生文件系统内部发布到回调返回之间，快速无锁读取者可能看到尚未治理的结果。
4. 完整模块 owner、expected generation、整组原子发布、外部 owner 隔离和精确恢复尚未完成。
5. 真实 KMI/CFI、文件系统并发、lockdep/KASAN、设备观察者和性能验证未完成。

因此 C06 保持部分实施，F06/F11/F12 继续保留门禁。本批不增加用户命令、UAPI 或新能力声明。

## 5. 依据与后续验收

生产依据为 [dirhijack 实现](../../kernel/sumh/core/sumh_dirhijack.c)、[调用合同](../../kernel/sumh/core/sumh_dirhijack.h)、[MERGE 物化](../../kernel/sumh/overlay/sumh_overlay.c)、[受管隐藏](../../kernel/sumh/core/sumh_hide_rules.c)及 [su 绑定](../../kernel/feature/sucompat_vfs.c)。VFS 对照使用已固定来源/哈希的本地 Android 6.12、6.18 namei、dcache、open 参考，不以当前网络最新版替换历史依据。

专用测试使用完整生产 DH/FOP/SOP 函数体及显式调度的原生服务，分别检查准备屏障、原生 alias、透明准备阶段、投影身份、分配失败、原子打开和退休。保留旧实现与定向变异必须编译成功，并在指定语义断言失败。模型对象的 ARM 编译只检查这些模型的类型/代码生成；后续仍须具备真实内核树、配置、符号版本和可恢复设备后执行 KMI 与观察者矩阵。
