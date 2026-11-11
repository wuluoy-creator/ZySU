# 应用环境可观察面

本文说明当前源码对启动属性、SELinux 和挂载视图的处理范围，以及如何验证普通应用实际看到的结果。管理器中的配置值、内核报告的运行状态和应用的观测结果是三个不同层面的信息。

## 启动属性

`hide_bootloader` 默认关闭。启用后，`ksud` 在以下启动阶段调用内置 `resetprop`：

1. `post-fs-data`：模块脚本、`system.prop`、挂载及 `post-mount` 脚本执行之后，同步写入属性，再返回 init。
2. `services`：重试较晚出现或此前写入失败的属性，然后执行 service 脚本。
3. `boot-completed`：再次重试，然后执行 boot-completed 脚本。

每次执行都重新读取内核开关；不存在或为空的属性不会被创建。写入使用 `resetprop -n` 的进程内入口，不触发 init 属性事件，并回读确认。处理的属性包括 verified boot / 锁定状态、debuggable、build type / tags，以及源码列出的部分厂商属性。完整列表位于 [`hide_bootloader.cpp`](../../../userspace/ksud/src/core/hide_bootloader.cpp)。

这是软件属性的修改，不会锁定 Bootloader，也不会改变硬件密钥证明。晚加载、进程已缓存旧值、后续脚本重新写入属性仍会影响结果。关闭开关只会停止后续改写，不会主动恢复本次启动中已经改写的值。

## SELinux 配置与运行状态

`selinux_hide`（feature 4）表示请求启用的配置意图。内核即使暂时无法完成 hook，也会保留该意图，以便保存配置后在下一次启动尝试启用。只读的 `selinux_hide_status`（feature 108）单独报告运行状态：

| 值 | 状态 | 含义 |
| --- | --- | --- |
| 0 | disabled | 没有请求启用 |
| 1 | active | 策略 hook 已启动，状态页及其 open hook 已准备 |
| 2 | pending reboot | 初始化返回需要重试的状态，通常为原始策略备份不可用；保存配置后重启再检查 |
| 3 | failed | hook 或状态页尚未准备成功 |

管理器在相关开关下显示运行状态，并在切换后刷新。旧内核不提供该查询或查询失败时，应按“状态不可用”处理。通过已获授权的 root shell 可读取：

```sh
/data/adb/ksud feature get selinux_hide
/data/adb/ksud feature get selinux_hide_status
```

前一条命令同时显示配置和运行状态。运行状态不参与保存、恢复，也不能通过 `feature set` 设置。`active` 只代表上述初始化条件满足，不代表 SELinux 的所有可观察接口或第三方应用验证均已通过。

状态页在完整初始化后通过 release/acquire 发布给无锁读取路径；如果首次打开才创建源状态页，该次打开也会使用刚生成的快照。初始化失败时保留原有回退行为，底层打开失败时保留错误返回。已准备好的状态页读取不增加互斥锁或内存分配。

`access` 查询直接在事务缓冲区内拆分两个上下文，省去原先两次 `size + 1` 字节的临时分配及对应复制，随后仍执行上下文转换、权限计算和原有整数解析。此处减少的是单次请求的分配与复制开销；实际设备延迟需要另行测量。

## 挂载、namespace 与文件系统视图

当前管理器的“挂载隐藏”只有开关，保存配置时使用 `normal` 模式。内核保留原生 mount namespace 身份，不再提供只改写 namespace 链接文本的 `aggressive` 能力；直接向内核请求该模式返回 `EOPNOTSUPP`。

用户空间仍兼容读取旧配置中的 `aggressive`。恢复时仅在内核明确返回“不支持”时记录日志并回退到 `normal`，其他错误继续作为失败处理。管理器随后保存 SUMH 配置时会把模式规范为 `normal`。

| 可观察面 | 当前范围 |
| --- | --- |
| 普通应用的模块策略 | 默认非 root 配置保留模块；可在授权策略或应用配置中调整卸载策略 |
| 挂载隐藏、maps、statfs | 默认关闭，各自依赖开关和内核报告的能力；功能支持与实际启用需分别检查 |
| `/proc/.../ns/mnt` | 保留原生 namespace 对象身份，应联合检查 `readlink`、`stat` 和已打开句柄的 `fstat` |
| `mountinfo` / `mounts` | 支持相应挂载视图处理，但不能据此推断所有文件接口都已同步处理 |
| `statx` / `fdinfo` | 保留模块覆盖文件时，仍可能看到与被过滤挂载相关的真实挂载 ID；当前没有统一跨接口的挂载 ID 投影 |
| `mountstats` | 未提供对应过滤；应用是否可读取取决于设备权限和 SELinux 策略 |
| 管理器包信息 | 包名、签名、安装记录的可见性遵循 Android 包可见性规则，不由上述开关隐藏 |

管理器公开入口使用 `app.entry.App` 和 `app.entry.Launcher`，内置 ksud 使用 `manager/ksud-library-name.txt` 定义的 `libruntime.so`。这些调整移除了旧入口类名和载荷文件名，未改变包名、签名或内核授权规则，也不阻止应用枚举、APK 内容分析或其他指纹匹配。是否仍被检测需要在目标设备上复测，不能以文件改名代替验证。

升级没有保留旧启动组件别名。从应用列表首次打开新版时，管理器会尝试迁移已有的模块动作及 WebUI 快捷方式；普通桌面图标或已被桌面删除的快捷方式可能仍需重新添加。完整回归应覆盖启动、通知点击、模块快捷方式、ksud 同步与校验、模块安装及内置工具调用；打包回归可运行 `python3 scripts/ci/test_manager_payload_packaging.py`。

挂载 ID 不连续本身不是异常的充分证据。应比较同一进程、同一 namespace、同一文件句柄在多个接口中的关系。模块保留、卸载与视图处理也应分别测试。

## 可复现验证

在仓库根目录的 Linux 环境中运行相关主机回归，需要 Python 3、Clang 和 C++ 编译器：

```sh
python3 scripts/ci/test_selinux_hide_status.py --cc clang
python3 scripts/ci/test_selinux_hide_access.py --cc clang
python3 scripts/ci/test_selinux_hide_context.py --cc clang
python3 scripts/ci/test_selinux_hide_setprocattr.py --cc clang
python3 scripts/ci/test_sumh_native_namespace.py --cc clang
python3 scripts/ci/test_sumh_mount_views.py --cc clang
python3 userspace/ksud/tests/run_hide_bootloader_host_tests.py --cxx clang++
python3 userspace/ksud/tests/run_sumh_host_tests.py --cxx g++
```

这些测试使用生产函数和模拟内核、属性或 ioctl 接口，检查状态转换、错误分支及恢复逻辑。它们不执行真实 SELinux hook，也不等同于设备兼容性或第三方检测结论。

设备验证应记录源码提交、管理器和 ksud 版本、KMI、Android 版本、模块列表、应用授权/卸载策略及启用的功能。使用测试应用的主进程和 `isolatedProcess` 分别收集结果：

1. 关闭相关功能并冷启动，记录属性、SELinux 可读接口、namespace 身份、挂载视图和模块文件是否存在。
2. 每次只修改一项配置，核对保存结果及运行状态，再冷启动应用重复采样。
3. 重启设备，检查启动阶段日志、属性回读和配置恢复。对 `pending reboot` 应在重启后再次查询。
4. 打开模块覆盖文件，将 `statx`、`fdinfo`、`mountinfo` 的结果关联比较；同时覆盖保留模块和卸载模块两种策略。
5. 测试旧 `aggressive` 配置恢复，检查日志中的回退及实际模式；确认 namespace 文本与 inode 身份一致。

root shell 适合读取诊断状态，但它的文件视图不能代替普通应用的结果。权限拒绝也应作为观测结果记录，不能当作功能已隐藏该接口。

SELinux 专项复验还应覆盖：启动时首次打开状态页、多个应用同时打开、开关切换后重新打开、状态页分配失败后的恢复，以及不同长度和空白分隔的 `access` 请求。分别记录首次与后续请求的延迟及分配次数；主机测试的调用计数不能换算为真机性能提升比例。

实现入口：[`init_event.cpp`](../../../userspace/ksud/src/init_event.cpp)、[`selinux_hide.c`](../../../kernel/feature/selinux_hide.c)、[`sumh_ioctl.c`](../../../kernel/sumh/control/sumh_ioctl.c)、[`sumh_proc_read_hooks.c`](../../../kernel/sumh/hooks/sumh_proc_read_hooks.c)。相关回归范围见 [SUMH 验证指南](sumh-validation.md)。
