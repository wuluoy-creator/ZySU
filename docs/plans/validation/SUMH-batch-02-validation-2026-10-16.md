# SUMH 第二批实施验证记录

日期：2026-10-16（Asia/Shanghai）。关联[实施账本](../SUMH-implementation-progress-2026-10-16.md)。范围 R03/R04/K02；已执行与未执行证据分别记录。基线仍为 `2a5ce441e9c923b8b0e89779f5c9ec95e6a97e46` 加共享工作区改动。

## K02 虚拟目录错误语义

- `sumh_rule_vpath_child` 的路径分配失败返回 `-ENOMEM`，lookup 调用方传播负错误，不能将其缓存为负 dentry。
- `sumh_rule_vpath_emit` 的节点、名字或虚拟子路径分配失败停止收集，返回 `-ENOMEM`；不跳过失败条目、不生成占位 inode。错误时已收集列表仍由调用方负责释放。
- `sumh_vnode_dir_iterate_virtual` 在发出点目录项、推进 cursor 之前完成收集。收集失败释放所有节点并保持位置；缓冲区已满属于正常分段输出，下一次继续从已有 cursor 读取。
- 这次保留 RCU 下的 `GFP_ATOMIC` 收集和既有规则顺序；并发变更的稳定 generation、目录索引和减少每次重建属于 K03，不将本项错误修正描述成性能优化。

执行环境：Windows，Android NDK `30.0.16248370` 的 Clang，以 `x86_64-pc-windows-msvc` 编译独立 C fixture 并执行。使用生产函数提取、替身内核 API，未运行真实内核。

```powershell
$sumhPython = 'C:\Users\29941\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
$sumhClang = 'C:\Users\29941\.codex\tools\android-sdk\ndk\30.0.16248370\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe'
& $sumhPython scripts/ci/test_sumh_directory_names.py --cc $sumhClang
& $sumhPython scripts/ci/test_sumh_directory_stream.py --cc $sumhClang
& $sumhPython scripts/ci/test_sumh_vfs_helpers.py --cc $sumhClang
```

结果：名字索引、合并去重、虚拟目录收集、虚拟目录分页、规则 lookup、vnode lookup 共 **6 组通过**；目录 stream **1 组通过**；xattr-policy/xattr/statfs/directory **4 组通过**。新增用例覆盖各分配点、失败后的释放、无新增输出、读取位置保持、小缓冲完整续读、重试及“缺项”与“内存不足”的区分。原有“分配失败后跳项仍成功”的用例已改为显式失败断言。

待验收：四 KMI 内核构建、真实 getdents 小缓冲/seek、内核内存故障注入及并发规则更新。主机测试不能证明这些层面的正确性。

## R03 耐久存储与 R04 配置合同

R03 新增 `durable_file.hpp`：`replace_file_durable` 返回 `NotCommitted`、`ReplacedDurabilityUnconfirmed` 或 `Complete`，同时携带 errno；`write_file_durable` 将阶段写入具体错误。原有 atomic writer 不变，SUMHP 配置/用户规则/模块规则/恢复记录/迁移/启动标记/热操作标记/挂载 journal/日志轮转记录已接入公共接口。重置涉及的删除仍保留原来的独立同步/读回，不宣称跨文件事务。

R04 仅对主 `config.json` 建立 schema 1 合同。旧无版本配置可读，首次显式写入前备份原始字节；备份失败停止覆盖。未来版本不应用、不写入；未知字段可供检查，但旧写入入口拒绝改写。文件与 patch 对所有已知字段共用校验，失败保留旧配置对象，解析不触发日志设置变化。用户隐藏定义和多资源方案的版本化另在 F04/F07 实施。

独立复核随后发现旧 CLI 合法的空 `fs_type`、`mount_backend`、`mount_hide_mode` 会被新加载器拒绝。第三批已仅对无版本文档兼容归一并补回归，首次保存仍备份原文；详见[兼容修正与复验](SUMH-batch-03-validation-2026-10-16.md)。R03/K02 独立复核未发现阻断问题。

| 验证 | 结果与限制 |
| --- | --- |
| `run_runtime_host_tests.py --zig ... --portable-only` | capture pipe 与新 durable_file 测试通过。覆盖目录打开、临时文件创建、短写后错误/零写、chmod、file-fsync/close、rename、directory-fsync/close、EINTR、错误保持及完整旧/新值模型。模型不是实机掉电试验。 |
| `run_runtime_host_tests.py --zig ... --target x86_64-linux-musl --compile-only`（R03 子任务执行） | Linux runtime 测试编译通过，未执行该二进制。 |
| `run_overlay_mount_host_tests.py --zig ...`（R03 子任务执行） | 通过，包含新的 journal 存储替身。 |
| 合并后的 `run_sumh_host_tests.py --zig ...` | 13 个程序通过，含配置 schema、每个已知字段类型、NUL/相对路径/非法分区、未知字段/未来版本、备份失败、日志状态和旧配置保留；符号链接 fixture 因 Windows 权限/能力跳过。 |
| 合并后的 Android ARM64 Release ksud | 完整生产编译链接通过，包含公共 utils、config、migration、command、runtime、recovery 与挂载调用方；没有执行设备二进制。 |
| CI 清单和门禁 | 加入 runtime 后共 24 个入口。集成时测试发现并移除了重复入口；修正后 7 项门禁回归通过。 |
| Manager SUMH 全包 | Gradle 实际执行 21 个测试类、126 项测试，0 failure/error/skipped；`BUILD SUCCESSFUL in 41s`。XML 时间为 UTC `2026-10-15T18:58`–`18:59`（北京时间 10 月 16 日）。此为 JVM/Compose/Robolectric，不是设备测试。 |
| 差异检查 | `git diff --check` 通过。 |

复现命令与首批的 Python/Zig/CMake 路径相同；runtime 新测试使用：

```powershell
$sumhZig = 'C:\Users\29941\Desktop\ZySU\out\sumh-validation\tools\zig-x86_64-windows-0.14.1\zig.exe'
& $sumhPython userspace/ksud/tests/run_runtime_host_tests.py --zig $sumhZig --portable-only
& $sumhPython userspace/ksud/tests/run_sumh_host_tests.py --zig $sumhZig
& 'C:\Users\29941\.codex\tools\android-sdk\cmake\3.22.1\bin\cmake.exe' --build out/sumh-maps-native --parallel 4
```

Manager 全包验证在 `manager` 目录，使用首批相同 JDK/SDK：`./gradlew.bat :app:testDebugUnitTest --tests 'com.zying.zysu.ui.sumh.*' --console=plain --no-daemon -PABI=arm64-v8a`。只统计此次 `TEST-com.zying.zysu.ui.sumh.*.xml`，不混入其他旧测试报告。

尚待验证：真实文件系统故障、掉电、设备启动读取、Manager 错误呈现及实际降级恢复。耐久接口要求父目录预先存在；首次创建父目录及祖先目录的耐久性不包含在此次 replace 保证内。跨资源保存和运行集合恢复仍由后续 C/F 任务完成。
