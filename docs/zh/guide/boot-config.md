# 启动强制配置与 Shell 提权限制

ZySU 的内核使用 C，ksud 使用 C++17，管理器使用 Kotlin。以下行为参考 KernelSU 提交
`6e386044`、`f895b80a`、`0a1f3ff7`、`aaa14a34`，并适配本地固定 SUMH 模式和直接 LKM 注入流程。

## 强制功能值

两个修补命令均接受可重复的 `--force-feature 功能名=值`，也可使用功能 ID。
值为无符号 64 位十进制或 `0x` 十六进制数；布尔功能以零表示关闭、非零表示开启。

```sh
# 替换为固件匹配的 KMI；这里只生成镜像，不写分区。
ksud boot-patch --boot init_boot.img --kmi android15-6.6 --out patched \
  --force-feature shell_su_adb_only=1 --force-feature sulog=1

ksud boot-patch-v2 --boot boot.img --output patched-boot.img \
  --force-feature shell_su_adb_only=1 --force-feature sulog=1
```

| 名称 | ID |
| --- | --- |
| `kernel_umount` | 1 |
| `sulog` | 2 |
| `adb_root` | 3 |
| `selinux_hide` | 4 |
| `webview_zygote_umount` | 5 |
| `shell_su_adb_only` | 6 |
| `hide_bootloader` | 104 |
| `unshare_mnt` | 107 |
| `unicode_path_guard` | 109 |

本地 ID 5 仍在使用，不能复用于 Shell 限制。`su_compat`、SUMH、增强安全、默认
NO_NEW_PRIVS 和只读状态 ID 不接受强制配置，避免改变固定策略或锁住 SUMH 的路径重绑定。

ramdisk 修补通过 `/ksu_config` 传递配置；直接 LKM 注入使用本地 512 字节
ImgPatch 配置块，当前版本为 4，不改变现有引导胶囊格式。版本 3 引入强制功能与模块屏蔽，
版本 4 增加 `--no-preinit`，参见[模块 preinit](module-preinit.md)。旧版 2/3 配置块仅接受其支持的选项，
启用新选项时须更新 LKM；修补器会拒绝不兼容配置，不会静默丢弃。

内核在 SELinux 初始化后应用强制值，post-fs-data 再重试尚未成功的配置；晚加载也会应用。
`feature check` 返回 `forced`，`feature list/get` 显示强制标记。运行时修改被拒绝，管理器开关禁用。
配置加载跳过强制项，保存时保留原来的用户设置；强制开启 sulog 仍会启动日志守护程序。
SELinux 隐藏状态尚未激活时会继续保留重试标记。

重新修补时省略强制选项会清除镜像中的旧覆盖，下一次启动恢复使用保存的用户设置。

## 屏蔽内核模块

`--block-module` 可使用逗号列表，也可重复传入。默认不屏蔽任何模块。

```sh
ksud boot-patch --boot init_boot.img --kmi android15-6.6 --out patched \
  --block-module oplus_secure_guard
```

管理器仅把 `/proc/modules` 中已加载的已知模块列作建议，**默认不勾选屏蔽选项**。
只有主动勾选后才显示、校验并传递列表，取消勾选后即使字段保留旧文本也不传递。
每个名称仅允许字母、数字和下划线，长度 1–64，完整列表最多 255 个字符。
过长输入直接拒绝，不截断为其他模块名；重新修补时省略选项会清除旧列表。

屏蔽系统依赖的模块可能导致卡开机或反复重启，例如部分联发科设备上的 `mkp`。
应事先确认设备兼容性；恢复方式是刷回匹配固件的原厂 boot / init_boot。
机制使用内核原生 `module_blacklist`，保留原有条目，只影响后续加载，不能卸载已经加载的模块。
为避免并发模块加载读取失效内存，卸载 ZySU 时本次启动的合并列表仍保留。

## Shell 来源限制

`shell_su_adb_only` 默认关闭。开启后 UID 2000 除了已有 Root 授权，还须在最多 128 层
祖先链中找到 adbd 或 ZySU Root 域进程。它同时覆盖 su 路径和 `GRANT_ROOT` 请求，
不影响管理器查询 Shell 本身是否获授权，也不会自动授予权限。

```sh
ksud feature set-save shell_su_adb_only 1
ksud feature check shell_su_adb_only
```

普通 ADB 会话、由 ZySU Root 守护进程派生的 Shell 可以通过；脱离会话并重新挂到 init
下的进程会被拒绝。LADB、Shizuku 无线调试等经 adbd 建立的会话无法与其他 ADB 会话区分。
此检查也不替代 SELinux 对进程注入的限制。

## 验证

```sh
python3 scripts/ci/test_boot_config.py --cc clang
python3 userspace/ksud/tests/run_boot_config_host_tests.py --cxx clang++ --sanitize
python3 userspace/ksud/tests/run_host_tests.py --cxx clang++ --sanitize
python3 userspace/ksud/tests/run_boot_patch_host_tests.py --cxx clang++ --sanitize
python3 userspace/ksud/tests/run_boot_patch_v2_flash_host_tests.py --cxx clang++ --sanitize
```

Windows 的上述脚本也接受 `--zig /path/to/zig.exe`。管理器回归包含
`BlockModuleOptionsTest`，覆盖未勾选、非法名称、长度边界和建议列表。
主机测试使用真实解析器和核心函数，替换内核 API、设备 I/O 等依赖；它们不能替代
设备上的 LKM 加载、模块屏蔽、首次启动和恢复验证。
