# SUMH 第八批实施与验证记录

日期：2026-10-16（Asia/Shanghai）。本批推进 F05/U03/C02/C04/C05，继续[54 项主计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)，进度见[实施账本](SUMH-implementation-progress-2026-10-16.md)。原静态审查与历史批次保持原样；本文件只记录本批代码和实际取得的证据。

## 实现范围

精确 `/data/adb/ksud sumhp sumh maps preview` 在最外层 CLI 的普通日志、通用参数解析和 daemon 路由前分派，保留 su/sh 与多调用程序行为，仍要求 root。错误尾参数就地拒绝；全局参数包装或 daemon 嵌套调用不能绕入普通恢复。只读控制器复用真实应用解析器，分别呈现保存定义、本次路径身份、完整受管登记集合和历史提交；前后核对定义、boot、历史及运行 token，变化时标为 stale/unknown。不会保存、恢复、规范化或应用。

专用传输用固定 posix_spawn argv 和私有 SOCK_SEQPACKET，不经过 shell。子进程自行交付 pidfd，父进程确认后才开始工作；使用 pidfd 信号和 P_PIDFD 非阻塞回收，没有数字 PID 回退。传输预算 8 秒、原始 JSON 512 KiB；JNI 使用单任务、root 锁 try-lock 和 10 秒调用者等待预算。未回收/卡住的任务保留槽位，不能无限创建线程或子进程。跨进程预览 guard 在网络命名空间内共享，并由查询子进程继承。4 MiB 只用于容纳外层 JSON 转义后的封装，普通控制器 IPC 缓冲和定义预算未扩大。

实际 mutation callback 解析后的 payload 在 ioctl 前写 durable pending，确认后写 acknowledged，记录 latest 与 confirmed。写入前后的 fsync/读回失败分别保留未修改、部分完成或未知结果，不采样未来路径伪造历史。空集合旧内核兼容 no-op 不作为真实提交。记录保留路径、实际身份、boot、namespace、定义摘要及可用的前后 generation；这些 generation 不是提交 token，记录也不作为重放配置。同启动 pending 阻止自动恢复的开关、清理及新提交，且提交锁内再次检查；显式新应用通过 `supersedes_pending_id` 关联被取代的未知提交。旧启动 pending 不永久阻止新启动恢复。

Manager 手动预览使用专用 native API，无 meta/daemon 回退。严格区分 complete/partial/unknown/stale，历史不可用与当前事实分开；观察到保存定义或开关改变后使旧/迟到结果失效，需手动重读。JSON 交换仅包括受管路径定义，支持文件或文本输入、合并/替换差异、同目标冲突选择及选中复制导出。点击保存并应用前只有私有草稿；失败保留输入，读取/保存完成不覆盖更新的编辑。Unicode 和末尾空格原样保留，拒绝未来格式、未知/运行字段和容量超限。导入文档上限 64 KiB，定义仍为 256 条 / 双层编码 56 KiB。此功能不构成完整 profile，也没有新增临时规则按键删除。

## 审查发现与修正

1. 同启动 pending 原先可被 daemon 自动恢复覆盖并再次提交。独立故障脚本已复现；现增加自动/用户触发贯穿调用链、恢复前检查和记录锁内复验，显式新提交保留取代关联。CLI 增加 `runtime_result` / `detail`，未知/部分完成提示先 preview，避免无条件推荐重复应用。
2. 内层 embedded 路由测试原先没有覆盖 `cli_run → parse_cli`，真实外层解析会拒绝新命令。现增加早期精确分派和实际 main/CLI/parser/embedded 调用链回归；普通邻接命令、su/sh、BusyBox 路由保留。底层 driver 失败日志也会写 `/dev/log/main`，现使用进程内 OFF 作用域，采样子进程继承静默，返回后恢复原日志级别；生产 logger 替身检查确认零日志文件打开。
3. Manager 最初使用普通 1 MiB 外层 JSON 限制，不能容纳大预览的转义封装；现为专用入口设置 4 MiB 外层 / 512 KiB 内层独立上限。补齐 complete、非零身份、历史 phase/errno、latest/confirmed、一致性及 supersession 验证。
4. 导入对话框初次测试发现反复布局导致 idle 超时；使用明确宽度后相关生产 Compose 测试通过。独立审查另指出预览配置变化后仍显示旧比较、旋转后导出子集重置，现加入配置指纹失效及有界选择恢复，并补回归。

## 已执行验证

| 项目 | 结果与证据范围 |
| --- | --- |
| 原生共享回归 | pending 修正后 17 个程序、state ledger 及历史故障套件通过，session 26539，`out/batch08-root-native.log`；客户端 lint 修正后 17 个程序再次通过，session 77454，`out/batch08-client-style-native-final.log`。覆盖真实提交顺序、耐久/读回故障、同启动阻断、显式取代、旧启动恢复、未来/不可读记录和空集合兼容；Windows 符号链接 fixture 仍不可用。 |
| 预览控制器 | 44 个观察场景及 16 个内层路由检查通过；root 最终复跑 session 38750，`out/batch08-root-preview.log`。使用生产代码提取/模型，非真实 Android namespace 证据。 |
| 专用传输/JNI | 最终 native lint/const 修正后 50 个 syscall 模型检查和 8 个真实宿主线程生命周期检查通过，`out/batch08-final-transport-jni.log`。真实 POSIX smoke 已交叉编译为 x86_64-linux-musl，未运行，`out/batch08-transport-lint-posix.log`。 |
| 最外层路由 | 最终实际 main + cli_run + parser + embedded 提取测试 29 个分派场景及 parser 合同、现有 cli_args_test、生产 logger 静默/恢复检查通过，并经独立复跑；`out/batch08-preview-cli-tests.log`。负例的 root/参数错误输出为预期结果。 |
| ksud 生产构建 | 最终路由静默及 lint 修正后 ARM64 Release 编译链接和构建后加固检查通过，session 78557，`out/batch08-ksud-final-build.log`；806 条编译记录。最终实际产物/参数回归 24 项通过，`out/batch08-ksud-hardening-final.log`。 |
| Manager 最终测试 | 全包 34 类 / 248 项，零失败、错误、跳过，session 96173；root 与独立 reviewer 均重新汇总 XML。日志 `out/batch08-manager-final-gradle.log`，XML 和布局截图归档 `out/sumh-batch08-manager-full/`。 |
| Manager 原生构建 | ARM64 Release CMake 编译链接通过，最终 tool chunk f2774a / exit 0，`out/batch08-manager-native-final.log`。首次失败的必需 clang-tidy 诊断经源码修正，未关闭检查；POSIX siginfo_t 必需 union 访问保留一处有说明的定向 NOLINT。产物与日志归档同上。此证据是 native library 构建，未生成/安装完整 APK。 |
| 布局检查 | 已查看 360dp / 2 倍中文字体预览历史、横屏大字体导入截图，滚动后内容与操作可达，按钮文字无截断。Compose 生命周期回归包含迟到预览失效、导出子集恢复及保存后新输入保护；Robolectric 不等于设备/TalkBack/真实进程死亡。 |
| CI / 文档 / 差异 | 7 项门禁测试通过（缺少 missing.py 是预期负例），`out/batch08-ci-final.log`。新增四个 runner 后为 33 个 native 入口；文档 54 个唯一任务 ID 与账本一致、17 份文档 218 个本地链接通过。普通 `git diff --check` exit 0，仅 Windows LF/CRLF 提示，`out/batch08-git-diff-check.log`；未改换行配置。远端 Actions 未执行。 |

最终 ksud：3,257,368 字节；独立符号 16,453,048 字节；两者 build ID 均为 `cb40af5f32ad5f6eff29fef49865b7df21098358`。ksud SHA256 为 `73d4bac2a094098a7e76e59efc1238e38b9982627c2843623057d9fe299656f1`。Manager `libkernelsu.so` 为 5,317,992 字节，SHA256 `3edcea1345413c0c47c0e576efdf128c4b7e4688e532581e9271d309f5e1a6d7`；ELF64 DYN AArch64。具体路径、符号 hash 与产物清单为 `out/batch08-final-artifacts.json`。

ksud 构建使用 NDK 30.0.16248370 / Clang 21、Android API 31、CMake 3.22.1、Release/LTO。宿主使用 Zig 0.14.1。ksuinit 本批未改动，不重复构建或将第七批证据记为本批新结果。独立 receipt/route/Manager 源码复核发现均已修正，记录分别保存在 `out/batch08-history-review.md`、`out/batch08-end-to-end-route-review.md`、`out/batch08-manager-independent-review.md`；最终文档复核保存在 `out/batch08-final-doc-review.md`。

复跑入口：

```sh
python3 userspace/ksud/tests/run_sumh_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_sumh_state_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_maps_history_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_maps_preview_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_maps_preview_transport_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_maps_preview_cli_host_tests.py --cxx g++
python3 -B -m unittest discover -s scripts/ci -p 'test_sumh_ci.py'
cd manager
./gradlew :app:testDebugUnitTest --tests '*SUMH*'
```

## 兼容、撤销和剩余验收

没有新增内核 ABI。保存定义格式和旧命令继续兼容；新的历史文件是诊断记录，未知格式禁止覆盖。历史损坏需保留原文件并使用匹配版本排查，不通过删除历史或全局 clear 绕过保护。撤销保存规则应恢复原路径定义并明确应用；预览、预检和导出本身没有运行变更。关闭 Maps 保留定义并清理受管集合，临时数字规则保留。

历史只记录受管 Maps 提交，不是全功能操作历史，不包含第三方/全局 clear 的所有变更。集合替换与开关仍是两个请求，C06 全局事务尚未实现。前后采样不能排除 ABA；文件内容、旧 FD/mmap、应用观察者视图不由数字匹配证明。既有修改 namespace helper 仍可能阻塞，不能把新预览的等待预算扩展为所有操作的期限保证。

独立复核还记录了抽象 socket guard 的可用性限制：同网络命名空间的非特权进程若 SELinux 等策略允许，可能抢占名称并让诊断持续 EBUSY。当前忙状态严格为未知且不会触发恢复/清理；私有所有权隔离留作 S02/C04 后续，未宣称已解决。静默 scope 是 CLI 进程级，不是可并发嵌入任意线程的隔离日志系统。

未执行真实 Linux POSIX smoke、Android fork/mmap/pidfd、SELinux、文件系统并发/掉电、设备启动恢复、四 KMI、4/16 KiB 页设备、普通/授权/root/isolated/共享 UID/工作资料观察矩阵、TalkBack、真实进程终止、性能/内存或远端 Actions。没有安装、提交、推送、发布或设备变更。F05/U03/C02/C04/C05 为部分实施，整体 54 项目标继续进行。
