# SUMH 第四批实施与验证记录

日期：2026-10-16（Asia/Shanghai）。对应 [54 项主计划](../SUMH-comprehensive-improvement-plan-2026-10-16.md)及[实施账本](../SUMH-implementation-progress-2026-10-16.md)。本批继续 R05/K07/U01，并推进 C03 的内核运行状态部分。未提交、推送或部署；没有将主机测试写成设备验证。

## K07 / C03：真实开关状态与独立能力协商

新增 GET_RUNTIME_STATE（slot 49、240 字节）与 SET_RUNTIME_FEATURE（slot 50、48 字节），分别使用 bit 17 和 bit 18 能力位；原协议版本保持 1。固定宽度、对齐、输入零字段和输出验证使新旧协议边界可测试。GET 在 mutation/lifecycle 边界读取六项状态：supported、configured、active、degraded、last_error。带非零 instance/generation，GET 不做隐式初始化，生命周期重新初始化更换身份；旧写入口也更新序列。有效 CAS 尝试只增加一次序列；过期、坏形状与 copy 输入失败不改变 CAS 状态、错误或序列。

控制设置及错误序列化读取，外部 readiness 分别采样。DEBUG 不依赖全局 SUMH 开关，Kernel Build 直接影响全局 UTS 字符串，也不随全局视图停用而撤销；独立复核发现这两项最初被错误地按全局开关过滤，现已修正并添加断言。statfs 需要 Mount Hide；受全局开关影响的四项视图功能停用时保留 configured，active 为假。last_error 为最近 setter 结果，不推断 readiness 失败原因。

控制器新增已实现的 `ksud sumhp api runtime`，输出 schema 1、查询区间、来源、boot ID 和一致性说明；instance/generation 用字符串输出。unknown/error/unavailable/unsupported 不合成 false 值；失败保留 `runtime:null` 和错误。`api meta` 宣告控制器格式版本；`api sumh` 添加同结构字段供新客户端采用，原兼容字段未全面重构。生产客户端拒绝畸形结构且不覆盖上次有效输出。

C03 仍为部分实施：尚未统一保存 revision、模块/后端、日志/存储分区结果和 Manager 全部查询，也没有测量 root 调用数与冷热延迟。新 GET 本身纯读取；既有 GET_FEATURES 的隐藏机制初始化、daemon 冷启动恢复边界仍存在。ARM32 只做 ABI 编译，现有 compat 调用仍被拒绝。

## R05：条件写入与有依据的回滚

同时协商 GET 和 SET 后，debug、stealth、Mount Hide、Maps 开关和 statfs 按单项执行 snapshot→CAS→readback。CAS 核对 instance/generation；过期停止，无自动重试。成功设置与暂不活动分别报告。只有写入确认且精确匹配下一代状态时，才允许条件回滚；读回失败、实例变化、并发变更或 transport 结果不明时不盲目回滚。

独立审查未发现五项开关路径的阻断问题，并提出客户端允许非零 Mount Hide mode 的合同差异；已改为本地拒绝，补无 ioctl 调用断言。Maps 继续复用受管集合原子替换；集合已替换而开关失败报告 partial，结果丢失不重放。能力只有 GET 或旧内核保留旧兼容语义。

未闭合范围：Maps 集合与开关不是全局事务，集合没有 owner/generation 条件替换；Kernel Build 字符串仍使用旧 setter，其回滚还可能覆盖另一客户端的并发写入。单项 CAS 不解决这些边界，后续必须处理，R05 不标为完成。

## U01：模块与 Maps 草稿持久存储

模块完整规则草稿、单规则输入和跨 Maps 编辑器累计草稿移出 Bundle，放到进程共享的私有 noBackup 存储。4 MiB / 512 项总预算、每项 128 KiB、字段 4095 UTF-8 字节；原模块/Maps 保存配置仍遵守 256 条和 56 KiB 编码限制。用户隐藏保留第三批独立 1 MiB 文档与分批文件输入。

使用严格有界格式、同目录单临时文件、文件同步、原子替换与读回。无法读取或未来格式禁止覆盖；写入失败保留内存并可重试。超限拒绝新输入并明确提示，允许确认后逐项丢弃。多个 Activity 使用同一进程会话，旧完成回调不能删除更晚编辑；外部配置变化保留草稿，要求明确选用当前配置或保留草稿后继续。临时 Maps、设置、Kernel Build 及搜索输入有小字段上限；既有过大 mountsource 仍显示，但不进入 SavedState。

Root 独立读取存储、状态所有者、模块/Maps 接线与相关测试，未发现阻断问题；另外查看模块横屏大字体及 Maps 编辑截图。没有独立重跑整个 Manager 包，以下 Gradle 结果来自实施子任务。U02 的 Kernel Build/设置刷新保护和 typed result 仍未全部完成。磁盘写入确认前进程终止可丢失未提交输入，不承诺目录 fsync 或掉电耐久性。

## 执行证据

| 检查 | 结果与范围 |
| --- | --- |
| `test_sumh_runtime_state.py --cc <NDK clang>` | 实施者及 root 最终复验通过；实际 ioctl 包装器、选定生产分支与 lifecycle，含坏输入、错误记录、无副作用 GET、过期 CAS、清理、重初始化及两项活动语义修正；ARM32/ARM64 固定布局编译通过，非设备运行 |
| `test_sumh_feature_readiness.py`、`test_sumh_kernel_build.py`、`test_sumh_ci.py` | 实施者通过；Kernel Build 43 项、CI gate 7 项；原生 CI 清单现为 26 个入口 |
| `run_sumh_host_tests.py --zig <Zig 0.14.1>` | 最终 14 个程序通过，Python 子进程实际返回码 0（session 15216）；新增 runtime 客户端/JSON/条件写入测试及生产 apply 集成测试，旧内核分支继续覆盖；Windows symlink fixture 不可用，不计通过 |
| Android ARM64 Release ksud | 首轮及最终客户端合同修正后的编译链接通过（最终 session 82115） |
| Manager 全 SUMH 包 | 26 类、149 项、0 失败/错误/跳过（session 75450）；最后 bounded saver 调整后另有 8 项定向测试通过（session 69465），完整包未在该微调后重复执行 |
| 大草稿生命周期 | 实际 Activity SavedState 序列化 <128 KiB，私有草稿 >1.4 MiB；覆盖磁盘重建、未知格式、读写失败、刷新冲突与旧完成事件；另覆盖 300k 既有 mountsource 不进入 Bundle |
| 文档与差异检查 | 54 个唯一任务 ID 与账本一致；13 份文档的 150 个本地链接有效；`git diff --check` 通过，仅有仓库换行转换提示 |

初次主机编译发现 Windows 没有 ESTALE/ESHUTDOWN 常量，仅在测试 runner 的 Windows fake transport 中补 Linux 数值。一次 PowerShell 重定向把测试预期 stderr 当 NativeCommandError，虽然所有程序输出通过，外层退出码为 1；随后改用 Python subprocess 合并日志并明确核对真实返回码，不能把这个外层错误当作生产失败或掩盖不报。

仍未执行：四 KMI 内核完整构建、真实设备权限/namespace/观察者矩阵、真实内核并发压力、Android 进程终止与旋转、TalkBack、文件选择器设备往返、掉电、性能基线和远端 Actions。未执行项不记通过。
