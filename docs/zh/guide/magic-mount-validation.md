# Magic Mount：保留模块内容与挂载观察

本页描述当前实现可以检查的行为，不把宿主模拟测试当作设备通过证明。Magic Mount 在启动阶段把模块文件绑定到实际分区路径，并在需要时建立 tmpfs 目录骨架；模块选择 Magic Mount 后，文件内容由实际绑定挂载提供。

## 后端选择

模块后端由 SUMHP 的全局 `mount_backend` 和保存的模块模式共同决定：全局值不是 `auto` 时覆盖单模块设置；否则使用单模块模式，默认 `auto`。模式与路径规则现在一并保存在 `module_rules.json`，旧 `module_mode.json` 作为升级输入。`auto` 的顺序是 OverlayFS → Magic Mount → none。模块在分区根放置直属文件（例如 `system/build.prop`）时，OverlayFS 不会替换分区根，规划器会选择 Magic Mount；显式 Overlay 也检查布局条件。显式请求不可用后端时，规划器会尝试回退到可用后端。

```sh
ksud sumhp api backends
ksud sumhp module set-mode example.module magic
ksud sumhp module list --all
```

`module set-mode` 只保存计划。OverlayFS/Magic Mount 在启动挂载阶段运行；切换后端请重启。`sumhp module mount-all` 仅供启动流程使用，启动完成后拒绝手动执行；SUMHP 还以每次启动标记和三次未确认启动保护避免重复/危险挂载。

## Magic Mount 的实际边界

- 模块树中的分区根直属条目不能改变原分区根的整体类型；实现会逐项绑定文件，拒绝不支持的分区根替换或类型变更。
- `.replace`、Overlay whiteout 属性、符号链接、目录骨架和原目录内容需要按模块树遍历；目录骨架可能临时挂到工作 tmpfs，再移动到最终位置。
- 工作 tmpfs 在成功提交后被卸载，但保留的子挂载仍然存在。每个最终挂载都写入卸载登记；登记失败会使事务失败并进入回滚。
- 失败路径会尝试按登记顺序卸载并保存残留状态。日志写入失败、卸载失败或存储状态损坏都应当视为需要人工检查的错误。
- 模块目录带有 `disable`、`remove`、`skip_mount` 或本次启动的 `hot_unmounted` 标记时不会进入挂载枚举。
- 挂载后端只改变文件视图；它不保证隐藏文件的设备号、inode、内容哈希或其他内核可见身份。

## SUMH 挂载观察与 statfs

内核中的 `mount_hide` 是独立于文件内容路由的观察策略。启用后，普通应用和隔离进程会按照各自打开的 `/proc/self/mountinfo`、`/proc/self/mounts` 所属命名空间生成过滤视图；root、allowlist 中的管理器/服务继续使用管理视图。过滤使用挂载身份、来源和目录关系，不应根据应用名称或固定 `/apex`/`/vendor` 路径推断结果。

```sh
ksud sumhp sumh mount-hide normal
ksud sumhp sumh statfs-spoof on
```

实现保留原生挂载 ID、父 ID 和传播字段，不为追求连续编号而重排。每个打开的 proc 文件维护自己的快照，成功从头 seek 会重新生成；快照构建或成对过滤失败时应返回错误，不应默默返回未经处理的内容。内核对快照大小有约 1 MiB 的实现上限，真实设备上的挂载表超过此限需要记录错误并调整布局。

上述直接命令只改变运行时状态；需要持久化时，保存 `enable_mount_hide`、`enable_statfs_spoof` 和 `mount_hide_mode` 到 SUMHP 配置，再执行 `config apply`。当前内核不再支持 `aggressive` 模式；旧配置应用时会回退到 normal，保留原生命名空间身份。

`statfs-spoof` 要求同时开启挂载隐藏，仅在内核能够找到对应可见上层挂载并成功生成视图时替换完整 `kstatfs` 结果（类型、fsid、容量、inode 计数和标志）。它不是对所有路径或所有文件系统的通用伪装。`mount-hide` 和 `statfs-spoof` 都需要包含 SUMH 内核改动的模块；仅替换 ksud 不会安装这些 hook。

### 挂载视图整理与 mount ID 缺口

`ksud` 在挂载流程、卸载配置恢复及 `post-mount` 脚本完成后发送 `EVENT_MODULE_MOUNTED`，允许内核在后续应用启动时执行模块卸载。挂载流程部分失败时也必须发送，因为可能已有挂载需要清理；安全模式、UAPI 不匹配和 Magisk 接管启动的跳过路径不发送。遗漏通知会使内核一直认为模块未挂载，即使开启“挂载视图整理”也会提前返回。

复验时需开启“内核卸载”和“挂载视图整理”，并在目标应用策略中启用模块卸载；默认保留模块的应用不会重建命名空间。更新包含修复的 `ksud` 后重启设备，再强行停止并重新打开检测应用，同时检查内核日志有无 `on_module_mounted!`、`handle umount for uid` 或 `unshare_mnt: ... failed`。

命名空间重建发生在卸载之后、应用返回用户态之前。它保留真实挂载身份，不能保证全局分配的 mount ID 连续。单独的 ID 缺口并不证明挂载损坏；应结合启动与运行时挂载表，以及 `statx`/`fdinfo` 的 ID 一致性复验。

## 设备复验步骤

在目标固件上记录 KMI、提交号、实际挂载布局和配置，然后：

1. 安装包含内核 SUMH 改动及对应 ksud 的完整包，重启，执行 `ksud sumhp api backends` 与 `api mounts`。
2. 用含分区根文件、替换文件、目录、符号链接、`.replace` 和 whiteout 的测试模块分别选择 `magic`、`overlay` 和 `none`；检查启动日志的计划、登记和回滚结果。
3. 在关闭模块卸载的普通应用中读取模块文件、遍历新增目录并加载原生库；记录 `/proc/self/mountinfo` 与 `/proc/self/mounts`，确认仅观察视图变化而文件内容仍来自绑定挂载。
4. 分别对模块文件、骨架目录和未修改系统路径调用 `statfs`/`fstatfs`，比较 f_type、fsid、容量、inode 计数和挂载标志；关闭 spoof 后复验原生结果。
5. 覆盖多用户应用、隔离进程、外部存储、应用专属挂载、重新打开、seek、分段 read、readv、splice 和 poll。使用同一进程、同一命名空间和同一时刻的原始表/过滤表比较挂载 ID；ID 不保证连续。
6. 触发初始化失败、日志或卸载失败、重复 mount-all 与 bootloop recovery，确认日志明确报告失败且不会把部分事务报告为成功。root/管理器路径应保持管理视图。

## 宿主回归入口

这些脚本提取生产函数并模拟系统调用，不执行真实挂载或 Android 命名空间：

```sh
python3 userspace/ksud/tests/run_magic_mount_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_mount_alias_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_overlay_mount_host_tests.py --cxx g++
python3 scripts/ci/test_sumh_mount_views.py --cc clang
python3 userspace/ksud/tests/run_boot_mount_notify_host_tests.py --cxx g++
python3 scripts/ci/test_sumh_vfs_helpers.py --cc clang
python3 scripts/ci/test_sumh_native_namespace.py --cc clang
```

可验证事务回滚、登记、遍历计数、分区别名、挂载表成对过滤和 statfs 视图逻辑；执行结果应以当前命令输出为准。宿主测试、Android 编译或内核辅助脚本通过都不能替代设备上的启动、命名空间、VFS 和 SELinux 复验。

实现入口：[后端规划](../../../userspace/ksud/sumhp/src/mount/backend.cpp)、[Magic Mount](../../../userspace/ksud/sumhp/src/mount/magic_mount.cpp)、[挂载表视图](../../../kernel/sumh/features/sumh_fake_mountinfo.c)、[应用观察策略](../../../kernel/sumh/policy/sumh_path_policy.c)。
