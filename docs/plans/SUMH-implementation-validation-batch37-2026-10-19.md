# SUMH 第三十七批实施与验证记录

日期：2026-10-19（Asia/Shanghai）。对应 [54 项计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)与[进度账本](SUMH-implementation-progress-2026-10-16.md)。本批推进 **S03 日志存储与重复消息**，使用说明合并到[日志指南](../zh/guide/sumh-log-storage.md)及[场景 29](../zh/guide/sumh-usage-scenarios.md#29-核对日志保留重复错误与清空范围当前源码已实现)。S03 和整体目标均未全部完成。

## 1. 实际改动

| 文件 | 行为与边界 |
| --- | --- |
| `userspace/ksud/sumhp/src/core/log.cpp`、`log.hpp` | 当前/历史日志各 1 MiB；超限保留最多 512 KiB 窗口内完整记录和保留提示。启动 P/R 恢复先于裁剪；共用一个有界 `.stage`，维护失败停止本次追加。普通追加不逐条 fsync。 |
| `userspace/ksud/sumhp/src/core/log_repeat.cpp`、`log_repeat.hpp` | 每进程最多 32 个精确 Warning/Error 身份，30 秒单调窗口，首次立即写，重复计数在匹配到期/淘汰/关闭时汇总；失败保留计数，时钟异常直接输出，无定时线程。 |
| `userspace/ksud/sumhp/src/core/daemon.cpp` | 后台原始 stdin/stdout/stderr 与受管日志隔离；明确启动错误写结构化日志，关闭时刷新汇总。readiness socket 两端在 daemon 化前提升到 fd ≥ 3，部分失败关闭当前两端。 |
| `userspace/ksud/sumhp/CMakeLists.txt`、`scripts/ci/sumh_ci.py` | 生产链接 repeat 实现；native CI 登记两个新 runner。实际完整远端 CI 尚未运行。 |
| `userspace/ksud/tests/log_*_test.cpp`、`run_log_*_host_tests.py` | 生产重复过滤器、提取的生产描述符函数和完整生产日志 namespace，分别通过内存 sink/描述符/文件系统替身验证。没有复制一套存储决策算法。 |

组件最多 256、正文最多 8,192 个编码字节，包含截断提示；保留完整有效 UTF-8，CR/LF 转义。重复身份按编码截断后的文字比较，Info/Debug 不汇总，Debug 沿用已有开关。外部写入、清空、启动/进程或观察文件变化重置聚合；异常退出可能丢失尚未写出的计数。

三个受管内容路径成功维护后的合计上限为 3 MiB，另有 marker/锁。此声明不包含文件系统元数据、其他日志/备份、被读者持有的旧 inode、外部写入和维护失败的既有超大文件。损坏或未知 marker 不再默默重置，而是保留并停止写入。Manager 读取范围仍为当前文件尾部 256 KiB/1,000 行，JNI 清空没有在本批修改，不能据 writer 检查声称 clear 的单链接/竞态问题已解决。

## 2. 静态复核与修正

实现前冻结 `out/batch37-before-manifest.json` 及 `out/batch37-before/` 的 **488 份**文件。范围审查在 `out/batch37-scope-review.md`，重复模型和存储测试分别由独立分工交付；最终静态复核为 `out/batch37-final-review.md`。这些报告是源码审查，不是运行证据。

复核指出：原始后台 stdout/stderr 长期开着日志追加 FD，会绕过容量控制并占有旧 inode；新的 detach 必须同时保留启动确认通道。`daemonize_process` 自身会先重定向 stdio，因此不能等返回子进程后再提升 readiness。最终实现将两端提升放在 socketpair 之后、daemonize 之前，补全八种标准流关闭组合和第一/第二次提升失败清理。独立复核确认 R1 已关闭，无剩余静态审查阻断。

按复核建议补了 stage/directory close 失败、R-marker 写入失败后重试、部分追加后的第二次换行边界及 UTF-8 精确容量回归。最终检查结果如下。

## 3. 已实际执行

执行主机为 Windows，Python 使用本机捆绑运行时，C++ 替身用 Zig 0.14.1；Android 使用 NDK 30.0.16248370、CMake 3.22.1、ARM64/API 31 Release。均未安装到设备。

| 最终证据 | 实际结果 |
| --- | --- |
| `out/batch37-host-a3/policy.log` | 生产 RepeatFilter **15 个用例、317 次检查、0 失败**；生产提升/重定向函数 **118 次描述符检查通过**。 |
| `out/batch37-host-a3/storage.log` | 完整生产日志 namespace 的文件系统故障模型 **45,474 次检查通过**；这是断言计数，包含逐字节/短 IO 循环，不能称为同等数量的独立用例。 |
| `out/batch37-host-a3/transport.log` | daemon 便携 socket 故障回归通过。 |
| `out/batch37-host-a1/ci-runner.log` | CI runner **7 个单元测试通过**；其中 missing.py 错误来自预期失败夹具，不是漏执行。其后 CI runner 源码未变。 |
| `out/batch37-build-a2/configure.log`、`link.log` | 最终 ARM64 ksud 编译链接通过；产物 **4,118,880 字节**，保留符号 **21,118,088 字节**，Build ID `0225e9b88ccf91d9be7584a9a4b6bef2a5d45ac5`；构建检查 831 条编译命令，RELRO + NOW。 |

`out/batch37-host-a3/manifest.json`、`out/batch37-build-a2/manifest.json` 保存命令、耗时、选定源码/测试输入的前后 SHA-256 及日志指纹；两次最终执行期间捕获的输入均无变化。构建证据目录保存 ksud、符号、编译数据库与 CMakeCache。它不是包含所有第三方依赖的可复现构建封包，不能由这些指纹推导真实设备已启动。

早期证据保留：host-a1 的策略测试先通过 15 个模型用例，随后因 Windows `STDIN_FILENO` 宏与测试枚举重名导致夹具编译失败；改为条件定义后 host-a2 通过。存储 a1 为旧覆盖集 40,769 次检查。build-a1 是修复 readiness 前的编译通过记录；最终验收采用 host-a3/build-a2，不覆盖或冒用旧记录。

本地复现入口（占位变量请替换为已安装工具路径；这些是已有测试入口）：

```powershell
& $python -B userspace/ksud/tests/run_log_policy_host_tests.py --zig $zig
& $python -B userspace/ksud/tests/run_log_storage_host_tests.py --zig $zig
& $python -B userspace/ksud/tests/run_sumhp_daemon_host_tests.py --zig $zig --portable-only
& $python -B -m unittest discover -s scripts/ci -p test_sumh_ci.py
```

`out/batch37-run.py` 保存了实际路径和 CMake 配置；新复验必须选新的 attempt 目录，不覆盖本批记录。冻结前的最终核对脚本为 `out/batch37-close.py`，结果在 `out/batch37-final-manifest.json`，检查基线、改动、证据指纹、54 项 ID 与文档链接，不重跑或改写 Batch34/35/36 的封存记录。

## 4. 已覆盖的行为

- 容量阈值前/等于/后一字节，旧超大当前/历史、无完整尾行、首尾残片、有界读取和固定暂存；跨启动 P 的各阶段、R 保存失败与安全重试。
- 当前/历史/锁/marker/暂存的链接与非普通文件拒绝，0600、无跟随/非阻塞打开，errno 保留和描述符回收。
- 短读写、EINTR、零写、ENOSPC、create/chmod/stat/flock/fsync/close/rename 和目录同步/关闭失败；rename 前保留目标，rename 后不虚报回滚；部分追加之后插入 LF。
- 组件/正文 UTF-8 精确预算、跨字符截断点、转义编码预算，关闭 Debug 无文件访问；用户清空后重复状态重置。
- 首次事件、精确 30 秒窗口、不同等级/组件/文本、32 项 LRU、容量淘汰、失败保留、flush/reset、时钟缺失/倒退、uint64 饱和和极限时间；重复汇总不吞掉不同的新错误。
- 标准流关闭的全部八种组合，readiness 两端在实际生产 stdio-reset helper 和 SUMHP detach 后仍指向原端点；open/fcntl/dup2 失败和部分提升后清理。

## 5. 未执行与后续范围

没有 Linux 实际 flock/多进程并发/真实 fork 生命周期测试；没有 Android 安装/启动、SELinux、读者持有旧 inode、JNI 清空竞态、进程崩溃点、断电、长期磁盘写入/耗电或性能测量。故障模型通过和 ARM64 链接通过均不能代替这些验收。本批不改变内核，未执行四 KMI 构建或内核设备验证。

S03 的默认脱敏、用户主动且可预览/删除的精简诊断导出、备份保留期限仍待实现；对未知结果先查询及 F06/F11/F12 的 owner/事务门禁保持。仅修改共享工作区，未提交、推送、发布、连接或改动设备。
