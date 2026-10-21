# SUMH 第二十九批实施与验证记录

日期：2026-10-17。对应[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)和[54 项账本](SUMH-implementation-progress-2026-10-16.md)。本批推进 F05/K05，增加临时 Maps 数字规则的独立分页查询与条件定向删除。F05/K05 和整体目标继续进行；C06 模块 owner 事务没有在本批完成。

## 实施变化

新增两个已实现的精确本地 CLI 入口，均先校验 root，不启动 daemon、不恢复规则、不读取或保存持久路径配置。参数错误仍在本地拒绝；全局参数、普通 daemon、operation 包装及 Manager 通道不支持这两个命令。

```sh
# 查询第一页；后续页保持同一组内核 token
/data/adb/ksud sumhp sumh maps list-temporary
/data/adb/ksud sumhp sumh maps list-temporary INSTANCE GENERATION CURSOR

# 大写参数均为占位值，只删除查询确认过的精确临时键
/data/adb/ksud sumhp sumh maps remove TARGET_INO TARGET_DEV --instance INSTANCE --generation GENERATION
```

查询复用 inventory schema 1，每页最多一条，按 `next_cursor` 读至 `at_end`。64 位身份使用十进制字符串；`boot_id:null` 明确此入口没有读取文件系统 boot ID。旧内核不支持时返回 unsupported 和 `legacy:null`，不读取无关旧文本冒充临时集合。

删除同时协商 `temporary_maps_delete` 与 `rule_inventory` 能力。新增 UAPI bit 21、command 53（`0x40285335`）、version 1；请求结构为 40 字节，包含 size/version、expected instance/generation 和 target inode/device。保留协议版本和既有数字入口语义，inode/device 零值仍可作为精确键；device 限 u32，inode 必须可由内核原生 unsigned long 表示。

内核在生命周期保护和变更锁内校验非零 token、精确匹配临时项、准备不可变集合并发布。不存在或只有同键受管条目时返回 ENOENT；不回退通配匹配，也不清空集合。同键受管项、其他临时键及排序保持不变。错误不发布、不推进 generation；成功只推进一次，发布后不进行回复 usercopy。最后一条且无受管项时可直接发布空集合。分配或共享预算不足时保留旧集合，旧读取流继续持有旧快照。清理能力不依赖当前 proc 过滤代理就绪。

删除结果区分 `deleted`、`not_applied` 和 `outcome_unknown`。`saved:false`、`operation_id:null`、`history_supported:false`、`observer_verified:false` 保留实际边界。提交后的 EIO/EINTR 等不确定错误、输出失败或断连均不能证明未删除；先查询再决定新的显式请求，不自动重试，不回退全局 clear。该入口没有增加每调用者 owner 或持久操作历史。

源码：[UAPI](../../uapi/sumh.h)、[不可变集合](../../kernel/sumh/core/sumh_maps_store.c)、[ioctl 与能力](../../kernel/sumh/control/sumh_ioctl.c)、[客户端](../../userspace/ksud/sumhp/src/mount/sumh_client.cpp)、[本地命令](../../userspace/ksud/sumhp/src/mount/maps_temporary.hpp)、[分页输出](../../userspace/ksud/sumhp/src/core/rule_inventory.hpp)、[精确路由](../../userspace/ksud/sumhp/src/embedded.cpp)。使用方式、撤销与输出字段同步[Maps 指南](../zh/guide/sumh-maps-rules.md#临时数字规则的定向删除)和[场景 25](../zh/guide/sumh-usage-scenarios.md#25-撤销一条临时-maps-覆盖并保留受管规则)。

## 验证记录

| 检查 | 实际结果与证据 |
| --- | --- |
| 实际删除源码 | [新 runner](../../scripts/ci/test_sumh_maps_delete.py) 的 **2 种宿主宽度模型通过**，提取生产 store、删除 wrapper、generation 及外层 ioctl；覆盖 16 种精确/通配和 temporary/managed 组合及两种插入顺序、零值/最大值、旧 token、失败保留、满额 1024+256、原样保留其他条目、旧快照及最终空集合。`out/batch29-maps-delete.log`。 |
| ARM 提取模型与 ABI | **ARM32/ARM64 共 4 个对象编译通过**；保留对象的证据脚本使用 -Werror，校验结构大小、对齐、偏移、命令与能力位。生成源、对象、精确命令和 16 项输入 SHA-256 保存在 `out/batch29-kernel-tests-evidence/manifest.json`。这些不是完整内核构建。 |
| 相邻内核源码回归 | Maps store/filter **4 组通过**，runtime state 与 inventory 各 **1 个宿主组及 ARM32/64 ABI 编译通过**，feature readiness **通过**，包含代理缺失和关闭后的清理能力。日志分别为 `out/batch29-maps-existing.log`、`out/batch29-runtime-state.log`、`out/batch29-rule-inventory.log`、`out/batch29-feature-readiness.log`。 |
| 实际客户端与输出 | [新客户端测试](../../userspace/ksud/tests/sumh_maps_temporary_test.cpp) **106 项通过**；执行实际 client/header/serializer，ioctl 为替身。完整 `run_sumh_host_tests.py` 相邻套件通过，最终日志 `out/batch29-client-host-final.log`。Windows 不可用的 symlink 和真实 fork/shared mmap fixture 仍未验收。 |
| 完整 CLI 路由 | [路由测试](../../userspace/ksud/tests/maps_preview_cli_test.cpp) **新增 64 项通过**，验证精确入口、root、错误后缀、包装拒绝及无日志文件打开；相邻 parser 与路由回归通过。测试执行真实 main/cli/embedded，服务为替身，实际服务由上一组覆盖。`out/batch29-cli-host.log`。存在既有 Android logger 局部变量未使用警告，不影响退出结果。 |
| operation 拒绝与输入 | [提交测试](../../userspace/ksud/tests/operation_submit_test.cpp) 加入两个拒绝案例；**209 项提交断言、52 项输入 IO 断言通过**。`out/batch29-operation-host.log`。这不表示本地删除已接入持久历史。 |
| CI 登记 | 新 runner 登记到 [CI](../../scripts/ci/sumh_ci.py)，**7 项 CI 自测通过**。`out/batch29-ci.log` 中 missing.py 是预期失败子进程，最终 OK。 |
| ARM64 ksud | 修复输出处理后 **Release 构建及链接通过**，强制加固核对 **827 条编译命令、RELRO + NOW**。`out/batch29-native-build-final.log`；Build ID `ae6e72ac19f7f7b5a6c1da71b8e6ec4a065d3f6e`，主文件 3940448 字节，独立符号文件 20221424 字节。未安装或启动设备。 |
| 独立审查与反例 | 独立审查确认本批范围无剩余阻断，R1 已关闭。`out/batch29-review-report.md` 与 `out/batch29-review-hashes.json` 保存结论及 30 个产品/测试文件指纹；文档整合单独核对。测试代理的 5 个故意回归及审查者独立的 2 个变体均被断言拒绝，详见下节。 |

工具链使用捆绑 Python、Zig 0.14.1、NDK 30.0.16248370 Clang 与 CMake 3.22.1。内核源码 runner 使用 `python scripts/ci/<runner>.py --cc <clang>`，客户端使用 `python userspace/ksud/tests/run_sumh_host_tests.py --zig <zig>`；ARM64 复用 `out/batch07-ksud-hardened` 构建目录。初次日志保留，批次结论以名称带 final 的修复后客户端和构建日志为准。

最终一致性检查由 `out/batch29-final-check.py` 核对 54 项任务、状态、文档链接、审查指纹、测试输入与生成源/对象、最终 ELF 和日志，并执行 `git diff --check`；结果保存在 `out/batch29-docs-final.log` 与 `out/batch29-final-manifest.json`。静态审查不代替测试，历史批次结果不改写成本批证据。

## 审查发现与反例

R1：共享 inventory 输出原先只根据查询状态返回，输出缓冲刷新失败时可能误报成功。现显式 flush 并检查流状态，失败返回非零和 EIO；删除结果也显式刷新。新增已知列表、不支持列表和“删除成功后输出失败”三类测试，最后一项确认只提交一次且不重放。修复后的 106 项客户端测试和 ARM64 构建均通过。

测试代理在生成源中分别引入通配回退、删除 managed 保护缺失、过期 token 检查缺失、重复 generation 推进、发布后 usercopy 五种故意错误，全部被拒绝。证据为 `out/batch29-tests-evidence.py`、`out/batch29-tests-evidence.log`。审查者另行运行通配删除与准备失败仍推进 generation 两种变体，均被拒绝，见 `out/batch29-review-negative-probes.log`。生产源码不为反例修改，这些预期失败不算设备验收。

## 剩余边界

- 未执行完整内核构建/链接、KMI/CFI、RCU/锁并发、lockdep/KASAN/KCSAN、SELinux、真实 namespace、设备观察者或性能验证。宿主服务替身与 ARM 对象不能证明这些项目通过。
- 没有新增 Manager 临时规则控件、持久删除历史或每调用者临时 owner。精确删除不等于 F05 全工作台完成，也不能据此自动放置或回收 F13 规则。
- instance/generation 只约束该内核集合变更，查询不是文件路径身份重解析；删除不会改变已固定的旧流，也不承诺消除旧 FD。撤销须先核对当前身份，再显式重建单项，不能盲目重放旧 inode。
- 本批无 Manager 修改，未重跑 Gradle。C06 的链接捕获、源内容变化、模块 owner 及一致发布仍按原计划推进。

后续将按相同能力与未知结果合同补齐 Manager 和历史流程，并通过真实设备验证再关闭对应验收项。
