# SUMH 第二十四批实施与验证

日期：2026-10-17（Asia/Shanghai）。本批推进 [54 项计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)的 **C06 短期计划留存、journal 与失败保护**，整体目标保持进行中。C06 为部分实施，F06/F11/F12 的 owner 事务依赖没有闭合。

## 实际实现

旧启动挂载与热重建在首个修改前保存完整编译计划、模块 ID、boot ID、重建 ID 和最近全部调用确认的计划，并保存独立耐久 intent。首次 feature/reset/ADD/MERGE/HIDE 失败后停止，不继续写后续规则，不进行第二次 clear，不回放旧计划。成功前缀只在正常得到结果时保存，进程中断不补造进度。

最终回执耐久写入并读回后才移除 intent；最终文件已替换但 fsync/读回失败时仍有独立阻止标记。未解决的同启动记录拒绝再重建，未来/损坏格式与读取错误不覆盖。预检同时为成功的双份计划和保留旧计划的最大部分失败回执预留容量。格式 schema 1，完整历史 512 KiB，单计划至多 4096 个编译动作，intent 1 KiB；没有扩大模块/Maps 256 条及普通 56 KiB IPC 容量。

namespace 子进程用共享结果传递 Unchanged/Applied/PartialOrUnknown，仅确认回包后采信；丢失回包一律未知。hot 标记只有确认未修改才恢复旧选择，部分/未知或调用确认后功能失败均保留请求选择。active/replay marker 落盘失败不再清除已确认规则。内层回执与外层命令分别解释：内层已完成而外层回复丢失时，后续查询可取独立证据；当前调用不重试。

新增精确本地 `module rebuild-status` 及 `api meta` 对应能力，绕过普通日志、daemon 启动和恢复；查询不创建文件、锁或调用内核。只读结果明确无 runtime/owner 验证、无原子运行发布、无原子历史快照，admission 不构成授权。stdout 写失败返回非零。参数错误和 daemon 包装均拒绝。

生产入口：

- [记录接口](../../userspace/ksud/sumhp/src/mount/module_rebuild.hpp)、[严格格式](../../userspace/ksud/sumhp/src/mount/module_rebuild_model.cpp)、[耐久执行器](../../userspace/ksud/sumhp/src/mount/module_rebuild.cpp)。
- [namespace/重置](../../userspace/ksud/sumhp/src/mount/sumh.cpp)、[后端编排](../../userspace/ksud/sumhp/src/mount/backend.cpp)、[热选择与能力](../../userspace/ksud/sumhp/src/core/command.cpp)、[精确本地入口](../../userspace/ksud/sumhp/src/embedded.cpp)、[外层 CLI](../../userspace/ksud/src/cli.cpp)。
- [使用指南](../zh/guide/sumh-module-rebuild.md)、[场景 22](../zh/guide/sumh-usage-scenarios.md#_22-模块重建失败后先读记录)、中文导航、模块指南、状态指南和实施账本已同步。

## 已执行验证

Windows 宿主，Python 3.12、Zig 0.14.1 C++17 `-Wall -Wextra -Werror`；Android 产物使用 NDK 30.0.16248370 / Clang 21。宿主边界替身不执行实际挂载或 ioctl。

| 验证入口 | 实际结果 | 覆盖与证据 |
| --- | --- | --- |
| [新模型/存储执行器](../../userspace/ksud/tests/run_module_rebuild_host_tests.py) | **12 组、1312 项检查通过** | 实际独立模型及生产执行器提取；严格重复字段、原始路径、每个 reset/规则故障/异常、写入前后替换、读回不符、锁/随机/文件错误、同启动阻止/跨启动、容量、只读和输出失败。`out/batch24-rebuild-final.log` |
| [新 namespace/重置桥接](../../userspace/ksud/tests/run_module_rebuild_namespace_host_tests.py) | **25 场景、261 项断言通过** | 实际函数原样提取；6 个特性失败点、clear、共享结果、无回包、active marker、日志破坏 errno、无第二次清理。`out/batch24-namespace-review.log` |
| [完整 SUMH 用户态回归](../../userspace/ksud/tests/run_sumh_host_tests.py) | **17 个程序通过** | 客户端/恢复/后端/热选择/Maps/namespace/持久化/迁移/构建设置；Windows 真 symlink 与 fork smoke 明确不可用。`out/batch24-sumh-second.log` |
| [生产后端回归](../../userspace/ksud/tests/run_sumh_backend_host_tests.py) | **4 个程序通过** | 模块发现含 39 项检查；热/启动失败仅一次 reset、部分保留、标记失败、guard 前置。`out/batch24-backend-first.log` |
| [生产 Planner](../../userspace/ksud/tests/run_sumh_planner_host_tests.py) | **45 个诊断/错误场景及既有回归通过** | 模块顺序、路径/错误与现有 apply 行为。`out/batch24-planner-first.log` |
| [实际 main→CLI→embedded 路由](../../userspace/ksud/tests/run_maps_preview_cli_host_tests.py) | 既有路由/解析/日志回归通过，新增 **12 个路由用例**及 daemon 拒绝/能力检查 | root、精确命令、异常、非法后缀和包装、无普通日志/恢复；既有 host logger 有一条未使用 Android 局部变量警告。`out/batch24-cli-first.log` |
| CI 执行器自检 | **7 项通过** | 新两个 runner 注册至 [sumh_ci.py](../../scripts/ci/sumh_ci.py)，路径存在、无重复及同提交门禁。`out/batch24-ci-final.log` |
| ARM64 Release `ksud` | **编译链接和加固检查通过** | 最终 build ID `8294aad7739455040a91d506a797fcda215c2826`，3826400 字节；823 个编译参数记录检查，RELRO + NOW，与独立符号匹配。`out/batch24-arm64-final.log` |

以上用例有重叠，不将各行相加冒充互不重复测试总数。正式 CI 的 Linux 全套和远端发布未执行。

最终文档与产物核对：54 个唯一任务 ID 与账本一致，39 份文档的 525 个本地链接存在，30 个本批源码/文档指纹保存，`git diff --check` 和新增代码空白检查通过；最终 ELF64/AArch64、独立符号 build ID 及源码/产物时间核对通过。清单为 `out/batch24-final-manifest.json`，日志为 `out/batch24-docs-artifacts-final.log`。这仅证明本地检查范围。

第一次完整宿主执行发现既有 `sumh_build_config_test.cpp` 缺少新 hide 查询声明的包含头文件，导致最后一个程序编译失败；已补实际 `user_hide_input.hpp` 并完整重跑通过。保留 `out/batch24-sumh-first.log` 记录初次失败，最终结果以上表第二次日志为准。

## 独立审查与修正

两名独立审查者检查内核实际错误语义、调用顺序、记录模型及存储窗口。审查发现并修复重复 JSON 字段接受、仅预留 pending/成功容量而漏掉部分失败回执两项问题。独立探针构造 pending 524282 字节、成功记录 524287 字节、部分回执超限的边界；生产执行器在写入/reset 前拒绝，9 项探针断言通过，边界已补进仓库回归。

另明确内层持久回执与外层 namespace 回包丢失的合同，不声称两者为同一个事务。root 最终补只读输出失败处理和机器可读的非原子/非授权字段，并重新运行 1312 项及 ARM64 构建。报告保留于 `out/batch24-contract-review.md`、`out/batch24-kernel-review.md`、`out/batch24-controller-status.md`。这些是工作区证据，不是设备报告。

## 未完成范围

第一次 reset 仍是全局接口，会影响非模块状态；文件锁不阻止直接内核写入；已返回成功也可能存在旧 ioctl 未检查的目录挂接失败。未实现 module owner、expected generation、整组原子发布、内核幂等回执或源对象固定。记录不是运行快照、永久历史、配置恢复点，也没有外层 operation ID 绑定。

尚未执行 Android 真机、四 KMI 内核构建、CFI/SELinux、实际 fork/共享内存传输、真实存储/掉电、并发写入、普通/授权/isolated 观察者、源替换/旧 FD/mmap 与性能测试。本批未更改 Manager 或内核产品源码，未安装设备、改变运行规则、提交或推送。下一步继续 C06 的内核 owner/一致发布设计与其他未闭合任务，不能将此批次标成全部 54 项完成。
