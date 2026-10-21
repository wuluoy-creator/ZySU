# SUMH 目录查找接口兼容与故障边界

本页记录第二十二批新增的目录子项查找适配。它用于通用 MERGE 的字符设备 whiteout 检查，以及有源目录 vnode 的创建、删除和重命名等操作。源码已按下表所列头文件声明适配；实际内核构建、装载和设备验收须单独完成，不能把版本号或宿主测试当作支持证明。实施证据见[第二十二批验证](../../plans/SUMH-implementation-validation-batch22-2026-10-17.md)。

## 版本与接口

| 目标源码 | 当前选择的接口 | 身份与权限语义 | 实际目标验收 |
| --- | --- | --- | --- |
| Android Common 6.1 | 精确 `lookup_one_len(name, parent, len)` | 上游旧函数使用 `init_user_ns` 并检查父目录执行权限 | 待完成 |
| Android Common 6.6 | 精确 `lookup_one_len(name, parent, len)` | 上游旧函数使用 `nop_mnt_idmap` 并检查父目录执行权限 | 待完成 |
| Android Common 6.12 | 精确 `lookup_one_len(name, parent, len)` | 同上 | 待完成 |
| Android Common 6.18 | 精确 `lookup_one(idmap, qstr, parent)` 与 `nop_mnt_idmap` 对象 | 适配器传入原始名称字节和长度，保留旧接口的身份映射与权限检查 | 待完成 |

上游接口变化始于 **6.16**；6.15/6.16 分支边界用于代码选择，不新增两个已经验收的产品 KMI。编译时会核对实际头文件的函数与对象类型；厂商回移或改动导致声明不符时明确构建失败，需要单独适配。即使类型相同，也仍需使用目标内核的配置、工具链及 CFI 设置完成构建和执行验证。

运行时只查找完整符号名，不将 `.isra`、`.constprop`、`$` 等编译器变体当作可调用接口，也不回退到不检查权限的 `lookup_noperm`。新接口还需要内核自己的 `nop_mnt_idmap` 单例对象，不能用本地零值对象代替；它通过实际对象地址传入，不解释为保存另一个地址的指针变量。

源码：[适配器和初始化](../../../kernel/sumh/core/sumh_runtime.c)、[内部合同](../../../kernel/sumh/core/sumh_runtime.h)、[启动接线](../../../kernel/sumh/core/sumh_bootstrap.c)。适配只在视图公开前初始化，不能用于运行中的热刷新。现有调用方继续负责目录锁、挂载写保护与 dentry 引用；成功查找返回的负 dentry 对创建操作有效，不会被改写为 ENOENT。

## 遇到不支持或读取失败

先记录实际内核/KMI、错误码、失败路径、所用后端与本次启动日志。`uname` 显示字符串可能经过 Kernel Build 替换，应结合[能力中心](sumh-capabilities.md)中的原始构建信息及已知镜像来源判断；不能用显示字符串证明 ABI 一致。

启动日志会说明目录查找缺失的精确依赖。旧分支缺 `lookup_one_len`，或新分支缺 `lookup_one` / `nop_mnt_idmap` 任一项时，该能力保持不可用。相关操作返回 EOPNOTSUPP；适配器不会因找到相似名称、另一个版本的函数或无权限检查入口而宣称可用。完整能力中心尚未逐项报告所有私有符号，不能根据其总开关推断该辅助函数存在。

通用 MERGE 普通字符项的检查还可能返回 EAGAIN（父目录 trylock 忙）、ESTALE（类型或身份变化）及权限/查找错误。先确认原因再重新读取，不自动循环重试。原始 `.replace` 标记不进入该分类器；已投影字符项与 SUMH vnode 目标仍有明确的支持限制，详见[目录读取边界](sumh-module-rules.md#长路径和目录读取失败)。补齐查找接口不会解除这些限制。

这项初始化与读取不修改持久配置，不需要全局 clear 来“恢复”。缺少精确符号须使用配套内核或增加经验证的适配；改变 Kernel Build 显示值不会增加符号或改变 ABI。现有源目录 mutation 的权限、idmapped mount 行为及嵌套锁架构仍需独立验收，本次没有扩大按应用规则或可写范围。

## 来源与待验收

声明及实现检查固定在以下 Android Common 提交，避免以移动分支代替来源：

- [6.1：b44b71789840](https://android.googlesource.com/kernel/common/+/b44b71789840c9c4cff65d56ffe46fbd224518be/include/linux/namei.h)
- [6.6：b9d5e9a3a59c](https://android.googlesource.com/kernel/common/+/b9d5e9a3a59c79fd6ec703c9780ab38ad1c0505d/include/linux/namei.h)
- [6.12：0a5243953af0](https://android.googlesource.com/kernel/common/+/0a5243953af061cc6a2e71cb769082aa1617d276/include/linux/namei.h)
- [6.18：70a231d0d37b](https://android.googlesource.com/kernel/common/+/70a231d0d37bc782fef784fc8a367822ed2098a1/include/linux/namei.h)

每个实际发布 KMI 仍需要记录头文件/配置/编译器身份、完整模块构建与符号检查、装载及 CFI 执行、DAC/SELinux 拒绝、负 dentry 创建、重命名、whiteout、错误清理和真实挂载测试。宿主替身只验证适配与调用方控制流；对象编译与 CFI 元数据检查不等于真实内核接受该模块。K09 的其他动态符号、私有布局与状态位风险仍在[54 项账本](../../plans/SUMH-implementation-progress-2026-10-16.md)中。
