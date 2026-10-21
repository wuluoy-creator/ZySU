# SUMH 第四十一批实施与验证记录

日期：2026-10-19（Asia/Shanghai）。对应[54 项计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)和[进度账本](SUMH-implementation-progress-2026-10-16.md)。本批补齐 **S03 的 Manager 当前日志清空合同**；完整 S03、C04 全链期限以及整体目标继续进行。此前第四十批封存记录未重跑或覆盖。

## 1. 实际改动

产品改动仅在 `manager/app/src/main/cpp/sumhp_jni.cpp` 的 `clear_log`。沿用当前 JNI、确认对话框和失败反馈，没有新增 CLI、ABI 或内核能力。

| 步骤 | 当前行为 |
| --- | --- |
| 打开协作锁 | `.lock` 使用 CREATE/RDWR/NONBLOCK/CLOEXEC/NOFOLLOW；打开和 fstat 的系统错误分别保留。检查实际描述符为普通文件、链接数恰好 1、大小非负。 |
| 取得锁 | `LOCK_EX \| LOCK_NB`；占锁即失败，不打开当前日志，不等待写入器释放后自动执行。持锁后将合法锁权限收紧为 0600。 |
| 打开当前日志 | WRONLY/NONBLOCK/CLOEXEC/NOFOLLOW，不使用 CREATE 或 TRUNC；ENOENT 是成功的无变更结果。 |
| 校验与截断 | fstat 检查同一描述符的普通文件、单链接和非负大小，再设置 0600，最后调用一次 ftruncate。前置错误不会进入截断。 |
| 返回与清理 | 真实系统 errno 在异常展开前保存为错误文字；结构错误为 EINVAL。RAII 先关闭日志再关闭锁；JNI 沿已有异常通道反馈，Kotlin 显示现有失败提示并复位忙状态。成功才请求刷新，无自动重试。 |

历史 `.old`、marker、stage、配置和规则不在清空范围内。不 unlink/rename，不修复异常文件。权限已收紧后出现后续错误，不承诺恢复原权限；不执行 fsync、不承诺断电耐久性，已清除文字没有软件撤销入口。详细使用步骤见[日志指南](../zh/guide/sumh-log-storage.md#清空当前守护进程日志)。

## 2. 源码复核与前提

改动前冻结 `out/batch41-before-manifest.json` 和 `out/batch41-before/` 的 15 个文件。源码作者与测试作者分工；另有未修改产品的代理复核描述符校验、协作锁、错误与 Kotlin 接线。初审记录为 `out/batch41-review-initial.md`，最终复核为 `out/batch41-review-final.md`。静态复核与下节执行证据分开。

生产 logger 的轮转、裁剪与追加使用同一个稳定锁文件；清空在锁内打开、校验并截断同一文件描述符。这是已有私有目录中的协作约定，不是对其他高权限主体的强制隔离。替换锁 inode、改动祖先目录、在校验后新增硬链接或绕过锁的外部操作，仍可能破坏该约定。本批不将这些边界标为已解决，也不据此做未经验证的漏洞定级。

NONBLOCK 和 LOCK_NB 解决本入口的命名管道打开等待与合作写入器争锁；root 授权、共享 worker、其他文件系统调用及整体 JNI 期限仍有独立边界。当前 UI 使用通用“操作失败”，尚不按占锁/权限/结构/IO 分类展示。

## 3. 实际执行证据

主机为 Windows；便携 C++ syscall 模型使用本机 Zig 0.14.1，生产 JNI 构建使用已安装 NDK 30.0.16248370 / CMake 3.22.1 的 ARM64 Release 配置。未安装或运行到 Android 设备。

| 证据 | 结果与限制 |
| --- | --- |
| `out/batch41-tests-a3/manifest.json` | 8 组场景、450 次运行检查通过；1 个编译后的定向变异和 6 次旧源码执行分别在指定语义断言失败，退出码 253。实际 JNI 调用接线静态检查及断开接线反例通过；源码/测试/旧基线的前后指纹一致。 |
| `out/batch41-build-a1/manifest.json` | 实际 JNI 库编译链接通过，`libkernelsu.so` 为 5,677,416 字节，SHA-256 为 `065bfc69538c9c9f6880956d0d82d91708b66bba52b22c32a4d41fca57621e9d`。导出符号含 `sumhClearLog`；输入指纹在构建期间保持不变。这不是完整 APK 或设备验证。 |
| `out/batch41-adjacent-a1/manifest.json` | 生产 logger 文件系统替身通过 45,474 次检查；RepeatFilter 的 15 个用例、317 次检查通过；实际生产函数提取的 daemon stdio 118 次检查通过。计数是断言数，不是同等数量的独立场景。 |
| `out/batch41-ci-a1/manifest.json` | CI runner / 发布门禁的 7 个单元测试通过。日志中的 missing.py 是预期失败夹具，不是漏执行。本条不代表全套 native 回归或远端 Actions 通过。 |
| `out/batch41-tests-cross-a3/manifest.json` | 当前函数、定向变异与旧函数的模型均可交叉编译/链接为 x86_64 Linux musl；明确未运行，仅作补充编译证据。 |

早期 a1 的 421 次检查和 a2 的 435 次检查及对应模型交叉编译均保留。根任务复核先补了异常 inode 权限未变断言和 JNI 接线检查；随后独立复核发现 FIFO 模型错误地将 O_RDWR 打开也视为等待对端，使旧源码的 FIFO 反例停在不符合 Linux 行为的锁打开分支。a3 改为区分访问模式：O_RDWR 不等待对端，O_WRONLY 无 reader 时 NONBLOCK 返回 ENXIO，阻塞模式记录等待；另补 reader 已存在时的 fstat 拒绝。最终旧源码由目标日志的 O_WRONLY 等待断言检出。产品源码无需修改，最终验收使用 a3，a1/a2 不再作为该反例的最终证据。修正要求见 `out/batch41-review-correction.md`，测试作者记录见 `out/batch41-tests-report.md`；根任务核对提取代码、最终指纹、实际输出和文档。

新测试入口 `scripts/ci/test_sumh_log_clear.py` 提取当前生产 `Fd`、`failure` 和 `clear_log`，仅将系统调用重定向到内存模型；不复制一套决策算法。覆盖正常清空、缺失无变更、链接数 0/2/17、普通文件以外的类型、负大小、各阶段 syscall 失败、忙锁、权限调整和描述符生命周期。还使用删除单链接检查的定向变异，以及本批真实 before-image 作为反例，要求编译成功后在对应语义断言失败；编译失败不计为捕获缺陷。

便携模型不会操作真实日志、文件系统或进程锁。它模拟的截断失败没有修改内容，只能证明调用顺序和错误处理，不能推导所有实际 IO 故障、持久性或断电结果。

可复现入口如下；将工具变量换为本机已安装路径，证据目录必须是新目录：

```powershell
& $python -B scripts/ci/test_sumh_log_clear.py --zig $zig --out-dir out/new-log-clear-run
& $python -B userspace/ksud/tests/run_log_storage_host_tests.py --zig $zig
& $python -B userspace/ksud/tests/run_log_policy_host_tests.py --zig $zig
& $python -B -m unittest discover -s scripts/ci -p test_sumh_ci.py
```

`--baseline-root out/batch41-before` 仅用于本地保留旧源码的反例验证；CI 默认不依赖未纳入仓库的基线文件。新回归已登记在 native CI 入口，远端 CI 尚未运行。

## 4. 文档与剩余工作

主计划和台账保留全部 54 项，并修正当前“JNI 清空单链接差异尚未修复”的状态；历史批次记录不改写。日志指南和场景 29 补清空步骤、失败后的手动核对、权限变化、无撤销和真实并发限制，同时纠正“尚无默认脱敏诊断包”的旧当前状态，链接第三十八、三十九批已经实现的四场景诊断包。原始日志、doctor 和全量报告没有因此变为脱敏。

未执行 Linux 真实 flock/FIFO、多进程轮转/清空、读者保留旧 inode、崩溃/断电、SELinux、Android UI、权限路径或设备存储/耗电性能测试。未重新执行内核/KMI 验收。本批没有改变内核或模块事务，F06/F11/F12 的 owner、generation 与原子替换门禁保持。

当前仅修改共享工作区，没有提交、推送、安装或发布。本批完成不代表完整 S03 或整体计划已经完成。
