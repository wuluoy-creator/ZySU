# SUMH 第三十五批实施与验证记录

日期：2026-10-18（Asia/Shanghai）。对应[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)的 F08/M04 与[54 项账本](SUMH-implementation-progress-2026-10-16.md)。本批实现 standalone CLI 的四场景只读诊断；静态设计另见[诊断设计](SUMH-diagnostics-design-2026-10-18.md)，用法另见[诊断指南](../zh/guide/sumh-diagnostics.md)。**F08/M04 仍部分实施，全部 54 项目标保持进行，C06/F06/F11/F12 门禁保留。**

## 已实现行为

公开精确入口为 `ksud sumhp doctor --input <绝对文件或->`，一次选择隐藏定义、模块回退、受管 Maps 或启动保护中的一个场景。schema 1 请求最多 4096 字节，重复键、未知字段、未来版本、错误类型、非法 UTF-8/NUL 和超限均拒绝。文件输入为普通单链接文件且不跟随最终符号链接；stdin 使用有界非阻塞读取并恢复共享 flags。通用包装和 daemon 路由拒绝 doctor，不作为另一条采样入口。

隐藏场景读取完整保存定义，再用稳定 ID 定位精确 cursor/revision，复用真实 `query_status`；缺失前后复读，避免把不在第一页当作不存在。登记、绑定、停用待移除和全局 SUSPENDED 分别解释。模块场景复用生产 Planner 及重建历史：当前输入的选择属于 inferred，不证明实际挂载后端；缺模块只指当前可挂载集合。Maps 复用实际预览与提交历史，对完整最多 256 行计数、仅保留 16 行摘要。启动保护直接读取旧存储查询，明确只解释计数/标记门禁。各场景再读近期操作，用户可附 operation ID；同命令族只作候选，不能把 Maps submission 或 module rebuild ID 当作 operation ID。

报告最多 65536 字节，分 saved/observed/historical/inferred，保留来源、boot、namespace 和单调时钟区间。observed/0、invalid/2、partial/3、stale/4、unavailable/5 表示证据完整度，不表示功能健康；缺失/null 布尔值保留未知。未知结果或不可读取历史抑制修改建议，boot/namespace 变化撤销当前结论；只有只读建议带独立 argv。报告不复制路径、备注、完整计划、原始错误或日志，不执行重试、清理、reset、恢复或应用。

采集由独立 exec 子进程执行，在 init mount namespace 中核对上下文，不是 query_worker 回调。父进程 spawn 前绑定同 network namespace 的独立 abstract reservation，CLOEXEC source FD 复制至至少 4，再传给 child channel→0、guard→3。子进程在 READY 前验证 FD3 精确类型/名称/长度，pidfd+ACK 后才调用 collector。固定 `/proc/self/exe` 配合 argv0 `ksud`，避免 multi-call 将 exe 当 BusyBox applet。fork sampler 保留 reservation，最后引用释放前新链返回 busy；不宣称跨 network namespace 串行或整棵进程树立即取消。

源码范围为 [共享请求](../../userspace/common/diagnostics_request.hpp)、[模型](../../userspace/ksud/sumhp/src/core/diagnostics_model.cpp)、[采集/公开报告](../../userspace/ksud/sumhp/src/core/diagnostics.cpp)、[输入读取](../../userspace/ksud/sumhp/src/core/diagnostics_io.cpp)、[传输](../../userspace/common/maps_preview_transport.cpp)、[原始 CLI](../../userspace/ksud/src/cli.cpp)、[通用 parser](../../userspace/ksud/src/cli_args.cpp)及 [embedded](../../userspace/ksud/sumhp/src/embedded.cpp)。隐藏/重建仅抽出类型化只读响应，保留旧打印和退出语义；CMake、帮助、`api meta` 与 CI 同步注册。没有修改本批 kernel、UAPI 或 Manager。

## 已执行检查与证据范围

修改前 469 个文件保持在 `out/batch35-before/`，SHA-256 为 `out/batch35-before-manifest.json`；没有重捕获或覆盖。第三十四批历史证据保持封存。本批请求/模型、传输/路由、根采集集成分别留存输入指纹、命令与日志，最终核对另存 `out/batch35-final-manifest.json`。

| 检查 | 实际结果与限制 |
| --- | --- |
| 纯模型/严格请求 | 471 项通过；编译实际生产 module/Maps/operation/recovery serializer 片段与模块名 validator，使用受控数据，未执行设备查询。最终 `out/batch35-model-run3.log`、`out/batch35-model-run3-fingerprints.json`、`out/batch35-model-final-manifest.json`。 |
| 实际 collector 编排与公开报告 | 165 项通过；实际函数体，来源/传输/OS 服务为替身，检查场景选择、错误、容量和无修改调用。 |
| 实际有界输入读取 | 57 项通过；文件/stdin/flags/超时/容量及非法路径，OS 为故障替身。 |
| 实际 hide/collector/model/operation-query 集成 | 99 项通过；40 个合法定义的精确 index 31、启停/全局暂停、不支持/未知、缺失/过期、boot/namespace 和未留存操作。真实类型化隐藏查询、模型与操作查询一起运行，store/ioctl/OS 为替身；模块/Maps/recovery 若误调用会中止。 |
| 相邻隐藏与重建 | 361 项隐藏控制器断言、12 组 1312 项重建检查通过。属于原行为回归，不计作新诊断场景。 |
| 实际传输体故障模型 | 246 项通过，其中 Diagnostics 81 项；包含关闭 stdio、FD3 冲突、spawn/dup/action 失败、guard/ACK/READY、strict JSON、4 KiB/64 KiB/envelope 边界。JNI 原预览 worker 回归同次通过。 |
| 实际 main/CLI/embedded 与拒绝路径 | 47 次诊断调用通过；另含实际 run_command 的 5 组 daemon 拒绝与实际 parser 的 5 组拒绝，保留 BusyBox 旧 argv0 反例。既有路由/parser/logger 回归通过，静默路径零日志文件打开。 |
| POSIX 生命周期 | 新增实际 exec/fork 同步 smoke，x86_64-linux-musl 交叉编译通过；本机无可用 Linux/WSL runner，**没有运行通过结论**。 |
| CI 辅助 | 7 项通过；包括预期的 missing.py 失败 fixture 输出，不是批次失败。新增模型/采集 runner 已注册到 SUMH CI，远端 CI 未执行。 |
| ARM64 ksud | 现有 NDK 加固目录增量编译链接通过，产物 4,104,440 字节，build ID `01b8d82d39a1c30558135835566058ad85117354`；830 条编译命令加固检查、RELRO+NOW 通过，保留同身份未剥离符号。不是完整内核、设备启动或性能验收。 |

采集最终运行器为 `userspace/ksud/tests/run_diagnostics_collector_host_tests.py --zig <Zig>`；真实隐藏集成使用已有控制器 fixture 边界，未另写一套内核状态判断。根最终证据包为 `out/batch35-root-evidence-a1/manifest.json`，包含 collector、两组相邻回归、CI helper 和 NDK 增量构建的逐条命令/退出码/日志以及运行前后源指纹。初步本地日志 `out/batch35-root-collector-final.log` 与 `out/batch35-native-final.log` 保留，不混同最终冻结包。

模型详情见 `out/batch35-model-final.md`；传输最终包为 `out/batch35-transport-evidence-a6/manifest.json`，120 个源输入前后稳定，三条命令均返回 0。传输报告 `out/batch35-transport-report.md` 保存 Python stub 未执行、fixture 失败、早期指纹遗漏及修正记录，早期失败证据未改写。主机已有 logger/CMSG 宏 warning 仍记录在日志，没有声称无警告构建。

独立验收审查与当前审查指纹保存在 `out/batch35-acceptance-review.md`。静态审阅、主机执行和实际设备运行是不同证据；最终批次核对只关闭下述受限范围，不将测试数累加成已验证的设备组合。

## 保留门禁与后续实施

- Manager 四类向导、精简诊断包、诊断历史/恢复点关联和完整显式重试/恢复/reset 流程仍待实现。JNI 的 self exe 为 app_process，后续须使用已验证的打包 ksud 路径与独立窄接口；当前 `diagnostics_manager=false`、`diagnostics_repair=false`。
- 输入等待 1 秒、父传输等待 8 秒为预算，不能保证阻塞系统调用的硬实时期限，也不能保证父退出时后代已停止。POSIX 实际进程/FD 生命周期需在 Linux runner 执行。
- 控制面不证明普通/授权应用、isolated、共享 UID、工作资料或既有 FD 视图；生产 Planner 不证明 live backend/外部 owner/最终赢家。需要专用观察者和真实设备分别验证。
- 完整 kernel/KMI/CFI、真实并发、内存/吞吐/P99 及 Android 设备验收未执行。既有 C06 owner/原子发布、源对象及缓存边界继续保留，F06/F11/F12 不因只读诊断入口而开放。
- 总体 54 项保持进行；本批只完成 CLI 只读增量及其主机/静态/原生构建验证，不将 F08/M04 标为完成。
