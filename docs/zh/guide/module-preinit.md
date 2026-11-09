# 模块 preinit

模块可以提供 `preinit/preinit.conf`，在 SELinux 策略加载和 init.rc 解析之前运行少量启动操作。
ksud 将已启用模块的 `preinit/` 复制到 metadata 分区；下次启动时，内核内嵌的静态 runner
拦截 PID 1 的 `/system/bin/init selinux_setup`，执行操作后移交原始 init。

## 配置

```text
/data/adb/modules/<模块目录名>/
└── preinit/
    ├── preinit.conf
    ├── kmod/example.ko
    └── bin/fixup
```

每行一个动作，空行和以 `#` 开头的行会被忽略。参数以空白分隔，双引号保留参数中的空白；
支持 `\"`、`\\`，不做变量展开。相对路径以该模块的 preinit 目录为基准。
配置文件最多 64 KiB，单行最多 4 KiB。

| 动作 | 语法 | 行为 |
| --- | --- | --- |
| 加载内核模块 | `insmod <文件.ko> [参数...]` | 查询内核镜像符号并加载模块 |
| 执行静态程序 | `exec [-t <毫秒>] <程序> [参数...]` | 等待退出，默认超时 5000 毫秒 |
| 写入现有文件 | `write <路径> <内容>` | 写入内容，不附加换行 |

动作名后缀 `?` 表示失败可忽略。普通动作失败会停止当前模块的剩余动作，继续下一个模块。

```text
insmod? kmod/example.ko debug=1
exec -t 10000 bin/fixup --flag
write /proc/sys/kernel/printk_devkmsg on
```

模块按目录名排序。执行程序需要在 `customize.sh` 中设置执行权限，例如：

```sh
set_perm "$MODPATH/preinit/bin/fixup" 0 0 0755
```

这个阶段尚无属性服务和 ueventd，`/apex` 也尚未挂载，因此只能运行静态链接程序。
程序以 root 运行，工作目录是其 preinit 目录；环境提供 `KSU=true`、`KSU_PREINIT=1`、
`MODID`、`PREINIT_DIR` 和 `PATH=/system/bin:/vendor/bin`。标准输入接 `/dev/null`，
输出进入内核日志。整个阶段最多 30 秒，结束时会清理 worker 进程组。

## 同步、状态与恢复

支持该接口的内核优先使用 `/metadata/vold/ksu`，其次是 `/metadata/watchdog/ksu`，最后是
`/metadata/ksu`。文件使用相应的 SELinux 标签，属主为 root，移除 group/other 的写权限。
符号链接和非普通文件不参与复制。

模块安装、启停、卸载和 post-fs-data 会同步下一次启动的内容。`modules_update/` 优先于
`modules/`；任一副本的 disable/remove 标记都会阻止执行。以目录名识别模块，避免
`module.prop` 的显示 ID 与目录不同造成状态错配。没有变化的文件不会重新复制。

preinit 的总预算为 4 MiB，metadata 至少预留 8 MiB。放不下的模块整体跳过。
复制失败会清理对应的旧动作，避免继续执行已经失效的配置。

```sh
ksud initrc refresh             # 同时刷新 modules.rc 和 preinit 副本
ksud module preinit status      # 查询下一次启动的准备状态
ksud module preinit reset       # 手动清除失败启动计数
```

管理器和 `ksud module list` 显示 `preinit` 状态：`ok`、`no_space`、`error`、`bootloop` 或
`unsupported`。这是准备状态，不表示每个动作已经在真机上执行成功；执行结果查看内核日志。

连续三次启动未到达 boot-completed 后，runner 跳过 preinit 并保持禁用。
成功启动仅清除尚未停用的计数；停用状态需要内容发生变化或手动 reset 才会解除。
关机充电模式跳过执行。安全模式禁用模块后，ksud 会清理它们的副本，影响下一次启动。

两个镜像修补命令均支持 `--no-preinit`。普通修补写入模块参数 `preinit=0`；直接 LKM 注入
写入版本 4 的 ImgPatch 配置块。旧版 2/3 LKM 不支持此新选项时会拒绝修补。
late-load、`preinit=0` 和未构建 runner 的内核均报告 `unsupported`，ksud 会清理预存副本。

必要时可从 recovery 删除实际使用目录下的 `preinit/`，或恢复固件匹配的原始启动镜像。

## 验证

```sh
python3 userspace/ksud/tests/run_preinit_host_tests.py --cxx clang++ --sanitize
python3 userspace/ksud/tests/run_boot_config_host_tests.py --cxx clang++ --sanitize
```

Windows 可使用 `--zig <zig.exe>`。这些测试覆盖同步、空间边界、复制失败、状态和启动计数，
不能代替设备的首次启动、模块执行、充电模式和恢复验证。
