# SUMH 第二十二批实施与验证记录

日期：2026-10-17。对应[54 项账本](SUMH-implementation-progress-2026-10-16.md)。本批推进 K09 的目录查找接口兼容，已完成实现及以下本地验证。整体目标和 K09 完整内核/设备验收仍未完成。

## 实现合同

保留内部可为空的三参数 `sumh_lookup_one_len` 及调用方的锁、引用和错误合同。6.16 之前按头文件核对并解析精确旧符号；6.16 及之后核对新 `lookup_one(mnt_idmap, qstr, parent)` 与身份映射单例对象类型，解析两个精确依赖后才公开旧签名的适配函数。任一依赖缺失则保持不可用，并在启动日志说明。没有把新函数强转成旧函数签名，没有使用后缀变体或不检查权限的查找入口。

适配器原样传入名称指针和长度，以新的栈上 qstr 接受上游哈希处理；负长度在转换前返回 EINVAL。函数仍执行旧接口的身份映射与父目录权限语义，返回的正/负 dentry 或错误指针不改写，不额外持锁、引用、分配、改变凭据或重试。外层与内层调用保留普通 CFI 插桩。初始化仅在现有启动生命周期中、视图公开之前执行，不提供运行中刷新。

代码限于 [runtime 实现](../../kernel/sumh/core/sumh_runtime.c)、[内部声明](../../kernel/sumh/core/sumh_runtime.h)及[启动接线](../../kernel/sumh/core/sumh_bootstrap.c)。MERGE 分类、vnode 子项与 rename 调用方保持原控制流。详细兼容矩阵及故障处理见[目录查找兼容指南](../zh/guide/sumh-kmi-compatibility.md)。

## 研究与设计复核

固定 Android Common 6.1/6.6/6.12/6.18 源码和上游 6.15/6.16 头文件表明，接口变化始于 6.16；不能仅在 6.18 才切换。6.18 的 `nop_mnt_idmap` 是内核自身对象，按指针身份识别，不能构造替身。当前通用解析器允许编译器后缀候选，故本批只对目录查找依赖改用精确解析；其他解析器消费者保持待办。

研究和独立设计复核分别保存在 `out/batch22-adapter-design.md`、`out/batch22-design-review.md`，锁定合同为 `out/batch22-contract.md`。头文件匹配不证明运行内核的 KMI/CFI 一致；任意 idmapped mount 的权限 API 改造不包含在此范围。

## 验证记录

| 检查 | 实际结果与证据 |
| --- | --- |
| 查找接口与调用方 | `scripts/ci/test_sumh_lookup_compat.py` **13 组宿主执行通过**：实际精确解析器 1 组、六版本适配 6 组、四版本 MERGE/vnode 调用方 4 组、6.12/6.18 完整快照集成 2 组。`out/batch22-lookup-final.log`。 |
| 错误声明拒绝 | **8 组预期拒绝通过**，涵盖旧函数返回类型/长度/缺失，新函数返回类型/const qstr/旧参数形状/user_namespace/错误对象声明。每项核对具体生产 guard 或缺失声明错误，完整源码与 stderr 保留在 `out/batch22-declaration-rejections/`。 |
| 既有 MERGE 与虚拟目录 | 真实收集器 **2 组**、名称与虚拟目录 **6 组**通过；未修改第二十一批期望。`out/batch22-{snapshot,names}-final.log`。 |
| CI 与启动接线 | 新 runner 已注册，CI **7 项自测通过**；另以源码检查验证 bootstrap 只有一次专用初始化且不再次覆盖指针。`out/batch22-ci-final.log`；missing.py 为有意的负向子进程用例。 |
| ARM 对象编译 | **24 组通过**：六版本适配、四版本真实调用方、两版本完整快照，各编译 ARM32/ARM64，保留生产表达式并断言未使用 Windows 指针替换。`out/batch22-arm-final.log`。 |
| KCFI 检查 | ARM64 普通及整数规范化 **2 种模式通过**，检查 wrapper 的外部类型、内部间接调用类型、错误签名对照、间接调用/陷阱指令及无直接 helper/data 重定位。IR、对象和反汇编保存在 `out/batch22-arm-evidence/`。这是编译和元数据检查，没有执行目标内核。 |
| 文档与交付 | `out/batch22-final-{docs,diff}.log` 检查 54 项、账本、文档链接及差异格式；`out/batch22-final-sources.json` 记录源码指纹，非真实内核产物。 |

合计 **21 组宿主执行、7 项 CI 自测、8 组预期声明拒绝、24 组 ARM 对象编译、2 种 KCFI 检查**，另有启动源码接线检查。各类证据分别计算，复用语义不当作全新覆盖，不累加为“执行过的内核测试”。完整命令、版本和源码清单见 `out/batch22-tests-status.md`、`out/batch22-tests-final-sources.json`。工具链仍为捆绑 Python 与 NDK 30 Clang；未运行无关 Manager/用户态套件。

依赖组合验证包括精确函数/对象存在或缺失、相似符号及另一版本函数干扰、启动成功后缺失再恢复；公开指针在依赖查找中始终为空。参数与返回检查覆盖非 NUL 结尾的名称跨度、嵌入 NUL/高位字节、零和负长度、真实单例地址以及正/负 dentry、EACCES/ENOENT/EIO/ENOMEM。名称合法性与权限由上游替身返回测试结果，不能当作真实权限执行证据。

实际调用方提取覆盖 MERGE whiteout、类型/身份变化、缺能力/锁忙与错误清理；vnode 子项成功时保留父锁和引用给调用方，错误不泄漏锁；rename 的第一次/第二次查找错误、负源/负目标、trap、最终失败/成功核对引用、挂载写保护及模块引用。新适配还接入真实完整收集器，验证后期拒绝时清空结果、释放此前来源，以及 `.replace` 在缺能力时仍只覆盖名称。rename 的锁算法和最终 VFS 委托仍为替身。

## 证据限制与复核

初始调用方夹具因既有 vnode 的 qstr 无符号字节指针及嵌套柔性数组模型无法编译；只在未改动的 vnode 提取代码周围限定 pointer-sign 诊断适配，并修正测试存储。完整快照夹具随后因 qstr 模型布局变化中的两个位置初始化报错，改为命名字段初始化。初始失败日志保留；没有改动生产适配器或降低断言来让这些用例通过。

Windows LLP64 的宿主夹具只对地址传递作指针宽度适配，ARM 保留实际表达式；二者仍使用声明模型而非完整真实内核头文件。KCFI 类型值仅适用于本次编译器/配置，不能作为跨内核固定值。独立审查已核对实现、测试、ARM/KCFI 证据及最终源码指纹，本批范围内无未解决的阻塞问题，记录在 `out/batch22-independent-review.md`。本批代码、局部验证和文档已闭合；设计研究与测试结果分别保存。

完整内核/KMI 构建、设备装载、实际 CFI、DAC/SELinux、挂载/idmap、锁和并发均未执行。通用快照仍有跨调用和嵌套锁边界，SUMH vnode 目标及已投影字符项限制仍在。未提交、推送、部署或修改真实设备。
