# OverlayFS：扩展属性过滤与挂载登记

本页针对两个独立功能：OverlayFS 负责合并模块文件，SUMH 内核的 `overlay_xattr_hide` 负责在目标观察视图中隐藏 OverlayFS 控制属性。属性隐藏是固定内建行为，不设启停开关；过滤目标随实际挂载登记。

## 实际挂载规划

SUMHP 的 `auto` 规划按 OverlayFS → Magic Mount → none。OverlayFS 适用于不需要替换分区根的模块树；分区根直属文件会使规划器回退到 Magic Mount。`module set-mode <ID> overlay` 的显式选择也检查这个布局条件；无法形成叶目录挂载时使用已有回退，并在日志中说明。模块扫描、SUMH、OverlayFS 和 Magic Mount 共用内置分区与额外分区的去重集合。

OverlayFS 实现：

- 按分区和相对路径选择模块层，保留层优先级；遇到 opaque / `.replace` 祖先时截断更低层。
- 优先使用 `fsopen`/`fsconfig`/`fsmount`/`move_mount`，失败时回退到传统 `mount(2)`。
- 原分区内容被 opaque 截断且仅剩一个模块层时，添加空的第二 lower 层。当前叶目录挂载使用只读 lower 层，存储层准备 upper/work 不代表这些挂载已启用写入。
- 每个实际 OverlayFS 挂载都登记可卸载身份，包括后续重建的独立子挂载；恢复原生 bind 挂载不会被误记为 OverlayFS。
- 挂载期间读取当前命名空间挂载表，按路径索引查找子挂载；索引不跨调用缓存，读取不完整时不发布覆盖挂载。

挂载失败时按挂载身份回滚，避免卸载已经被替换的其他挂载。不能卸载的本模块挂载继续写入现有日志，供后续清理；已经消失的挂载会清理对应卸载登记。日志保存失败仍是操作失败，不能把仅打印错误当成残留挂载已经可恢复。

扩展属性隐藏不读取配置，也不接受配置修改。它需要内核导出 `overlay_xattr_hide` 能力；即使 OverlayFS 已启用，对应 hook 也可能不可用。用 `ksud sumhp api sumh`、`api features` 和日志确认实际能力。

```sh
ksud sumhp config show
ksud sumhp api features
ksud sumhp config apply
```

OverlayFS 挂载时自动登记过滤目标，`config apply` 根据已登记挂载重建这些目标。目标身份的登记和清理由挂载生命周期管理，不提供用户修改入口。登记目标必须位于 OverlayFS，不能假设 `/system` 根本身就是 overlay。

内核过滤只处理 `trusted.overlay.*` 与 `user.overlay.*` 名称，在已登记 OverlayFS superblock、当前观察策略允许且功能可用时，将原先成功或返回 `ERANGE` 的匹配 `getxattr` 结果改成 `ENODATA`，并从 `listxattr` 中删除；其他原生错误保持原义。size-only 查询、短缓冲区、未登记文件系统、root/allowlist 进程和缺少 hook 的情况都必须分别检查。

## 设备复验

准备同时包含父目录和独立子挂载的测试模块，并记录设备 KMI、内核/ksud 提交号、配置和实际 `mountinfo`：

1. 冷启动后执行 `ksud sumhp api mounts`，确认父、子 OverlayFS 都按计划挂载，登记数量与实际挂载一致；失败或回滚时检查控制器日志。
2. 在关闭模块卸载的普通应用中读取模块内容和合并目录，确认 OverlayFS 行为不因 xattr 过滤改变。
3. 在同一应用中对父、子 OverlayFS 目录调用 `getxattr`、`listxattr`、size-only 和短缓冲区查询，分别测试 `trusted.overlay.*`、`user.overlay.*` 与普通属性。记录返回码和列表长度。
4. 确认旧配置中的关闭值无法影响属性过滤，配置修改请求被拒绝；root、获 root 授权应用和原生文件系统不应被误过滤。
5. 重启、刷新 SUMH 规则并触发一次挂载失败，确认父/子挂载的登记和清理与成功路径一致，不把恢复的原生 bind 挂载报告成 OverlayFS。
6. 分别验证挂载隐藏、statfs 视图与属性隐藏的实际结果；三项均为固定行为，但能力与目标范围不同。任何应用环境检测仍可能从设备号、inode、挂载关系或其他信号识别修改。

当前实现不承诺所有内核版本的 OverlayFS 属性 ABI 完全一致。测试报告必须保留内核日志、返回码和实际挂载表，不能仅写“通过”。

## 宿主回归入口

```sh
python3 userspace/ksud/tests/run_overlay_mount_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_mount_alias_host_tests.py --cxx g++
python3 scripts/ci/test_sumh_vfs_helpers.py --cc clang
python3 scripts/ci/test_sumh_mount_views.py --cc clang
```

这些入口模拟生产挂载和内核/VFS 边界，覆盖层选择、父子登记、失败回滚、路径别名及过滤逻辑；不会执行真实 OverlayFS、xattr 或 Android 命名空间。设备验证仍需完整内核构建、安装和重启。

实现入口：[OverlayFS 后端](../../../userspace/ksud/sumhp/src/mount/overlayfs.cpp)、[VFS 观察 hook](../../../kernel/sumh/features/sumh_vfs_view.c)、[属性名称过滤](../../../kernel/sumh/features/sumh_xattr_filter.h)。
