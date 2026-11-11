# SUMH Kstat 路径规则

受管 Kstat 规则按目标路径绑定属性覆盖。来源路径模式中，SUMHP 在 init 挂载命名空间通过内核原始 Kstat 查询取得来源文件元数据，再把快照与目标路径一起交给内核；内核按目标的真实 inode 和设备号绑定规则。规则仅在 SUMH 既有视图策略允许的进程中生效，不改变文件内容、访问权限或全局 VFS 解析。

`identity` 只替换设备号与 inode；`full` 替换受支持的九类字段：设备号、inode、链接数、大小、访问时间、修改时间、状态变更时间、块大小和块数。`custom` 可逐字段指定值，未指定的字段保留原值，显式 `"0"` 也会生效。其他 `stat` 字段不属于此规则。来源元数据在应用时读取，此后来源文件变化不会自动更新快照。

```sh
/data/adb/ksud sumhp api features
/data/adb/ksud sumhp sumh kstat add-path /system/bin/target /system/bin/source identity
/data/adb/ksud sumhp sumh kstat list-paths
/data/adb/ksud sumhp sumh kstat status
/data/adb/ksud sumhp sumh kstat apply
/data/adb/ksud sumhp sumh kstat remove-path /system/bin/target

# 高级规则：只覆盖 inode、大小和修改时间
/data/adb/ksud sumhp sumh kstat add-custom /system/bin/target \
  '{"ino":"123","size":"0","mtime":{"sec":"-1","nsec":"0"}}'
```

管理器的 Kstat 分类页可添加、编辑和删除同一组规则。添加和编辑时直接填写自定义字段，不再选择元数据范围或填写元数据来源路径。`dev`、`ino`、`nlink`、`size`、`atime`、`mtime`、`ctime`、`blksize`、`blocks` 全部必填；三个时间字段各自的 `sec` 和 `nsec` 也都必须填写。字段不能留空，显式填写 `0` 是有效值。

命令行和配置格式仍支持来源路径模式及仅指定部分字段的 `custom` 规则。来源路径规则使用 `{"target_path":"/system/bin/target","source_path":"/system/bin/source","mode":"identity"}`；部分自定义字段规则可使用 `{"target_path":"/system/bin/target","mode":"custom","fields":{"size":"0"}}`。已有规则可以继续读取和应用，在管理器编辑后保存时须填写全部自定义字段。数值均为规范十进制字符串，避免大整数在 JSON 中丢精度。`dev` 是内核编码的设备号，即 `major << 20 | minor`。

`dev`、`nlink`、`blksize` 的范围为 0 到 2^32-1；`ino`、`blocks` 为 0 到 2^64-1；`size` 为 0 到 2^63-1；时间秒数使用有符号 64 位，纳秒为 0 到 999999999。当前受管提交 ABI 使用原生 `unsigned long`，仅支持项目构建的 arm64 控制器，不提供 32 位 compat。

路径必须为绝对路径，不含 NUL、回车或换行；目标长度小于 256 字节，来源长度小于 4096 字节。最多 32 条，同一目标路径只能出现一次。旧内核未报告 `managed_kstat` 能力时不能安装非空规则。

保存与应用是两步：命令结果中的 `saved` 表示定义已写入配置，`applied` 表示内核接受本次替换。应用会先校验并读取全部来源，再通过一次 ioctl 原子替换受管规则；失败时旧快照保留。提交空数组会清空受管规则，不影响旧的临时 `ADD/UPDATE` 规则。匹配顺序为临时精确规则、受管精确规则、临时 inode 通配规则；旧规则仍遵循零值表示不覆盖的语义。

可分别读取保存的定义与内核活动状态：

```sh
/data/adb/ksud sumhp sumh kstat list-paths
/data/adb/ksud sumhp sumh kstat status
# 示例输出：{"active_count":1,"generation":"3"}
```

`list-paths` 返回配置中的规则数组；`status` 直接查询内核已安装的规则快照，仅返回 `active_count` 和字符串形式的 `generation`。代次从本次模块加载开始计数，成功替换规则时递增；失败的提交不会改变代次，`CLEAR_ALL` 移除非空受管快照时也会递增。代次不是跨重启的持久标识。Manager 分别显示保存条数、活动条数和代次；这些都是快照级信息，即使条数相同，也不能证明每条保存规则与活动规则逐项一致。SUMH 被关闭或目标 inode 已变化时，已安装的规则也不一定实际生效。两条命令的读取也不是同一原子快照。查询失败时活动状态显示为未知，命令返回非零退出码。

配置应用、模块重放后以及 boot-completed 会重新应用保存的路径规则；安全模式跳过启动恢复。目标或来源文件在挂载、替换后发生变化，应再次应用。管理器显示规则定义和内核活动快照计数，不提供活动规则逐项回读。真实设备仍需验证不同文件系统、硬链接、冷启动、模块重放及并发 `stat` 的行为。

运行中不能直接逐 inode 释放 Kstat hook：临时规则、受管规则和重定向共用同一个包装器，并发 VFS 线程可能已读取其函数指针。成功安装过的目标 inode 包装器保留到模块退出；频繁切换大量不同目标可能累积引用。
