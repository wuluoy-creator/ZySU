# SUMH 第十八批实施与验证记录

日期：2026-10-16。对应[54 项实施账本](SUMH-implementation-progress-2026-10-16.md)和[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)。本批推进 F01 能力与作用范围中心及 C03 的紧凑只读采样，代码、主机验证及文档已闭合。整体 54 项目标保持进行；完整设备验收未闭合。

## 实现范围

新增精确本地 `api capabilities`、专用 JNI 传输及独立 Manager 页面。分别观察保存设置、六项内核控制、构建原值/当前值及后端能力；保存值、configured、active、错误、来源及采样身份分开。运行 owner、模块实际后端和应用观测保持未核实。页面提供明确配置/诊断导航，没有自动修改。

各段独立采样，不宣称全局原子快照。保留未知、不支持、关闭和错误的区别；读取失败时历史段携带原 boot ID/时间并标记过期。原始 Release 推导的 KMI 仅为标签，不是 ABI 探测。完整规则、路径解析与操作历史继续复用已实现的详细工作流。

- 精确 CLI 在普通解析、日志及 daemon 分发之前识别；多余参数、包装调用、非 root 和 daemon 转发拒绝。专用服务不接受任意命令。Manager 先逐项协商 schema/read-only/transport/预算，失败不回退到旧查询。
- 配置采用生产解析器、256 KiB 有界只读、文件类型/链接检查、原始字节摘要及前后读回。不存在、不可读、非法/未来格式、采样中变化分别保留，不能用空文件默认值冒充保存事实。boot ID 采用本地最多 80 字节读取和规范 UUID 校验；缺失或前后变化使整份结果未知。
- 六项控制复用运行状态合同。构建原值/当前值独立采样，有能力时核对自身控制 generation；未与 runtime 段拼成同一次原子观察。后端仅报告协议、文件系统注册或已编入策略，未执行挂载探测。
- 原始结果最多 64 KiB；传输 512 KiB、帧 4096 字节、8 秒等待、JNI 调用者 10 秒、内部查询进程 5 秒。沿用 pidfd/ACK/隔离和 init 挂载命名空间核对，不扩大通用预算。没有保存、迁移、自动恢复、模块全树扫描或规则清空。
- Activity ViewModel 手动单请求读取；离开后不接收旧会话迟到结果，原请求返回前保持单请求限制。旋转复用同一会话，进程重建不保存完整快照或自动重放。读取失败保留旧段和最新失败来源，明确不支持则替换旧值；连续失败不累积嵌套历史链。
- 页面分列保存/configured/active、支持/错误/生效阶段，展示原始构建、KMI 标签与后端能力。采样标志相同不证明配置已应用；策略说明分别解释 VIEW、HIDE、挂载/statfs、Maps、stealth 和 xattr。配置与诊断按钮只导航。

源码：[只读采样](../../userspace/ksud/sumhp/src/core/capability_snapshot.cpp)、[传输](../../userspace/common/maps_preview_transport.cpp)、[JNI](../../manager/app/src/main/cpp/sumhp_jni.cpp)、[模型与校验](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHCapabilities.kt)、[查询入口](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHManager.kt)、[工作流](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHCapabilitiesOperations.kt)、[页面](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHCapabilitiesScreen.kt)。

## 验证记录

| 检查 | 实际结果与证据 |
| --- | --- |
| 生产快照 | **44 项**文件边界/序列化场景通过：配置错误/缺失/未来格式/类型/链接/超限/EINTR、采样变化、独立能力/构建失败、非法 UTF-8、观察 Release 空格、boot 缺失/无效/超长/变化及工作进程/命名空间错误。`out/batch18-native-snapshot-byte-exact.log`；调用 `userspace/ksud/tests/run_capability_snapshot_host_tests.py`。宿主 IO/ioctl 为故障替身。 |
| 原生传输与路由 | **144 项**生产传输故障模型及 bounded JNI worker 检查通过；**18 项新增**能力精确 CLI/root/parser 场景与既有相关套件通过。`out/batch18-native-transport.log`、`out/batch18-native-cli.log`。Windows 未执行真实 POSIX exec/pidfd smoke。 |
| ARM64 构建 | ksud Release 编译/链接通过，821 项编译命令检查；继承的 ksud clang-tidy 关闭。Manager JNI/transport 改动翻译单元 clang-tidy 与 ARM64 链接通过。`out/batch18-native-build.log`、`out/batch18-manager-native.log`。 |
| Manager 全 SUMH 包 | **56 类、563 项：561 通过、2 跳过、零失败/错误**。命令 `python out/batch18-manager-check.py`；日志 `out/batch18-manager.log`、XML `out/sumh-batch18-manager`。两项既有符号链接存储测试因 Windows 无创建权限跳过，不计通过。 |
| 新增模型 | **27 项通过**：逐项能力、类型/容量/UTF-8、保存与内核独立、未知/错误/不支持、跨 boot 历史、最新失败元数据、构建读取与编辑约束及三份生产原始字节夹具。 |
| 新增工作流/界面 | **7 项工作流 + 12 项界面通过**：手动读取、单请求、迟到隔离、失败保留、模拟进程新实例、导航、过期显示、来源与策略说明、Unicode 长错误、状态恢复、中文横竖屏 2 倍字体。不是设备生命周期/TalkBack 证据。 |
| 加固与交付 | 24 项实际 ELF/编译参数加固、7 项 CI runner/gate 测试、54 个唯一任务编号与账本匹配、本地链接及 `git diff --check` 通过；`out/batch18-final-{hardening,ci,docs,diff}.log`。 |

## 复核与修正

原生和模型交叉复核见 `out/batch18-native-independent-review.md`、`out/batch18-model-independent-review.md`。修正了源标识、保存模式的字符串类型、早期错误和运行编码失败的互通合同；boot ID 从无界通用读取改成本地有界读取。观察到的构建 Release 允许空格，保存覆盖值仍按生产写入要求拒绝空格；四个观测字段继续校验非空、64 字节、UTF-8、NUL/控制字符。三份原生成功/boot 不可用/命名空间失败夹具直接保存序列化字节，哈希及来源见[夹具记录](../../manager/app/src/test/resources/sumh/capability-snapshot-v1.md)。

首次原生夹具编译缺少 include，修正后通过，失败日志保留于 `out/batch18-native-snapshot.log`。首次 Manager 编译发现旧 Settings 导航未适配可选页面参数，改用 destination 构造调用后定向 46 项通过；日志保留于 `out/batch18-manager-initial.log`、`out/batch18-manager-focused.log`。首轮全包的新增失败夹具测试误期待 guard 错误，实际生产夹具为命名空间查询错误；按来源修正预期与说明，未改生产错误值或放宽解析。首轮日志/XML 保留于 `out/batch18-manager-full-initial.log`、`out/sumh-batch18-manager-full-initial`，最终全包通过。

根代理复核工作流及 PNG，记录 `out/batch18-root-workflow-review.md`。暗色截图初次未设置测试 Surface 背景，修正测试包装后重新生成；产品页面原有 Scaffold。根代理查看 `manager/app/build/reports/sumh-layout/capabilities-{narrow,landscape}-large-text.png`：窄屏换行正常，横屏暗色文字/错误与背景可辨，滚动后操作可达。未将语义测试和截图当作 TalkBack 或真实性能通过。

最终产物见 `out/batch18-final-artifacts.json`：ksud 3,775,232 字节，SHA-256 `311cc3fff1425d8d32638f0f35bfff6ca492dc228f4251a8576a0dd64ac4b8ae`；符号 19,314,576 字节，共享 build ID `ea66a1e1158693b0ba086e28f7a114cfb5e3d8d4`。JNI 5,616,936 字节，SHA-256 `098c2af8967dbcc45385a551b9a4321ebae1c43e65e4d6bcd1baf0da36423aca`，build ID `9397023dd8ae6f41c3a2ae7d588fac46bb83b987`。仅为本地构建，未安装到设备。

## 保留范围

F01/C03/K07 继续为部分实施。外部 owner 的可靠查询、全资源快照、真实后端归属、应用进程实际视图、F13 合成自检、性能/延迟基线、真实设备生命周期与 TalkBack、四 KMI 和远端 CI 均未因此完成。没有提交、推送、部署或真实设备修改。

使用说明见[能力中心指南](../zh/guide/sumh-capabilities.md)及[场景 20](../zh/guide/sumh-usage-scenarios.md#20-区分保存设置内核状态和应用作用范围)。
