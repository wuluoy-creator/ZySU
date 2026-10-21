# SUMH 第二十六批实施与验证

日期：2026-10-17（Asia/Shanghai）。本批推进 [54 项计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)中的 **F07 配置方案与恢复点**。已实现未启用方案工作流；激活、恢复、Manager 和真实设备验收未完成，F07 保持部分实施，整体目标继续进行。

## 实际实现

[模型](../../userspace/ksud/sumhp/src/core/profile_model.cpp)新增 schema 1 `sumh.profile`，固定 settings、modules、user_hide、maps 四项资源。设置按现有默认值物化，Maps 开关由 Maps 资源单独持有；模块保留显式 auto/空配置和原规则顺序，用户隐藏保留稳定 ID、enabled、Unicode、尾部空格及允许的 CR/LF。重复键、未知字段、未来格式、错误类型及超容量拒绝，不丢字段继续。

[存储](../../userspace/ksud/sumhp/src/core/profile_store.cpp)使用独立 `profiles.json`，库 ID 与显示名称分开，名称允许重复。提供新建、复制、改名、删除；全库版本和相关单项版本分别核对，已有 ID 不自动覆盖。非合作写入通过提交前原字节重读捕获已观察变化，不声称文件系统 CAS。耐久替换、目录同步及精确读回之后才报告 saved；替换后的同步/读回错误为 unknown，不回滚或自动重试，调用者已知 ID 可供查询。unchanged 已同步目录，但没有替换或同步方案文件，不声称新增方案耐久提交。

[控制器](../../userspace/ksud/sumhp/src/core/profile.cpp)已接入精确本地 capture/import/list/show/export/clone/rename/remove/preview/import-preview。直接进入 init mount namespace 并明确恢复；数据输入先校验，变更不放进可取消的只读 worker。嵌入 root 检查、通用解析器、daemon 拒绝及逐项 api meta 能力同步，独立于原有应用授权 `ksud profile`。没有扩大 IPC 帧、增加 Manager channel 或借用 operation 日志声称可恢复操作。

采集前后读取 config、旧 module_mode、module_rules、user_hide 四份文档；仅对确认缺失的源使用默认语义，错误和变化拒绝采集。返回各源版本、状态和 double_read_observation 标识，明确 global_atomic=false、runtime_verified=false。未固定原文件、模块载荷或 runtime inode，不能排除 ABA 和未观察到的写入。

按资源差异显示 add/remove/change、before/after、未变计数；隐藏同 ID 不同路径、同路径不同 ID 均显式标识。预览是 configuration_diff_only，can_activate=false、runtime_evaluated=false；模块/能力/owner/generation 可应用性未评估，不删不可应用定义，不冒充生产 Planner 计划或应用令牌。所有 profile 动作均不写活动配置、规则、恢复记录或启动标记。

[输入](../../userspace/ksud/sumhp/src/core/profile_input.cpp)复用现有有界模式，支持绝对普通单链接文件或 stdin，no-follow，非阻塞一秒读取，恢复共享 stdin 标志。stdout 只输出结果/裸导出文档，失败和 unknown 返回非零；输出失败不能推定未保存。

容量：单方案 8 MiB、输入 8 MiB + 4096 字节、库 16 份/32 MiB、设置 64 KiB、每方案 256 模块/总计 4096 路径。每模块/Maps 原 256 条及 56 KiB 编码限制、hide 原 4096 条/4 MiB 保持；源文件各 4 MiB，差异响应 24 MiB。达到限制拒绝，不截断后成功。

## 已执行验证

宿主工具为 Python 3.12、Zig 0.14.1 C++17；生产 Android 构建使用 NDK 30.0.16248370 / Clang 21。各组有共享边界和重复检查，不合计成互不重复断言总数。

| 验证入口 | 实际结果 | 证据与范围 |
| --- | --- | --- |
| [方案模型](../../userspace/ksud/tests/run_profile_model_host_tests.py) | **281 项检查通过** | 实际 model/user_hide 模型与生产 config/module 验证器提取；精确字符/元数据、同 ID/路径冲突、默认值、auto/空记录、格式与各级容量、失败不改输出。哈希是确定性替身。日志 `out/batch26-profile-model-final.log` |
| [方案存储](../../userspace/ksud/tests/run_profile_store_host_tests.py) | **10 组 / 1076 检查通过** | 完整生产 store namespace、实际 profile/user_hide 模型和生产资源验证器，无 parse_profile 替身。锁/读写/摘要边界注入；CRUD、库/单项冲突、未来库、16/17 份和真实 32 MiB 聚合边界、替换前后/读回故障、目录同步顺序及三层失败、活动文件哨兵。日志 `out/batch26-profile-store-directory-sync.log`，最终回归见 `out/batch26-profile-store-verified.log` |
| [控制器与输入](../../userspace/ksud/tests/run_profile_controller_host_tests.py) | **80 场景 / 567 断言，输入另 62 断言通过** | 实际 controller/argument/input 代码；source read、model/store、namespace 和摘要是计数替身。捕获变化、缺失与错误、进入/恢复失败、异常恢复、错误 stdout、unknown 退出码、完整容量、stdin 超时/恢复标志。日志 `out/batch26-profile-controller-tests.log` |
| [main→CLI→embedded](../../userspace/ksud/tests/run_maps_preview_cli_host_tests.py) | **新增 121 场景通过；既有相邻套件通过** | 实际主链、通用解析器、daemon/operation 包装拒绝、root 与元信息；查询执行为替身。日志 `out/batch26-profile-cli-tests.log`。logger 用例确认静默路径零日志文件打开 |
| [CI 执行器自检](../../scripts/ci/test_sumh_ci.py) | **7 项通过** | 新增三个 profile runner 已登记到真实执行清单。日志 `out/batch26-ci.log` 的 missing.py 是预期失败场景，不是漏跑 |
| ARM64 ksud Release | **最终生产编译、链接及强制 ELF 加固检查通过** | 目录补充后重建，827 编译命令检查、RELRO/NOW、build ID 与独立符号。日志 `out/batch26-build-verified.log`，build ID `160ab1a11959a4aeec566ef28e0285fa534c27f4`。当前构建关闭 clang-tidy，不声称 lint/设备验证通过 |

复现入口：

```sh
python3 userspace/ksud/tests/run_profile_model_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_profile_store_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_profile_controller_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_maps_preview_cli_host_tests.py --cxx g++
python3 scripts/ci/test_sumh_ci.py
```

Windows 已执行 runner 使用 `--zig out/sumh-validation/tools/zig-x86_64-windows-0.14.1/zig.exe`。这些 IO/namespace/锁替身不验证真实 POSIX 锁、真实 SHA 强度或实际文件系统掉电保证。

最终 ARM64 可执行文件为 `out/batch07-ksud-hardened/ksud`，3,933,824 字节；配套独立符号为 `out/batch07-ksud-hardened/symbols/ksud.unstripped`，20,198,104 字节。没有安装运行。`out/batch26-final-check.py` 核对 54 项 ID/账本一致、文档本地链接、当前源码/测试/日志指纹、对象更新时间、产物/符号及 whitespace；清单保存在 `out/batch26-final-manifest.json`。

## 审查修正与范围

独立审查发现 list/show/preview 在单项摘要失败时仍可能返回 complete，已改为 profile_revision_unavailable，故障测试通过。模型首轮发现非对象 hide 的错误文本为空，已补明确错误；完整模型最终通过。隐藏同 ID 修改路径现已明确身份冲突，激活时的冲突决策仍待实现。

收尾发现首次创建 `/data/adb/ksu/sumhp` 时，单文件替换仅同步叶目录，缺少新父目录项的耐久确认；已修正在持有方案锁后、替换前，按 sumhp→ksu→adb 同步固定目录层级，以安装环境已有的 `/data/adb` 为锚点。每次变更都同步，覆盖并发首次创建者；任一层失败均返回 not_saved、零方案替换。新增顺序与三层故障测试通过，未修改共用 runtime 创建函数。unchanged 文案也明确目录同步与文件提交的区别。既有 logger 测试的一个非致命 Android 变量警告保留，没有掩盖。

[主计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)、[54 项账本](SUMH-implementation-progress-2026-10-16.md)、[方案指南](../zh/guide/sumh-profiles.md)、[场景 24](../zh/guide/sumh-usage-scenarios.md#24-更新前保存配置方案并比较差异)、[验证矩阵](../zh/guide/sumh-validation.md)和[中文导航](../zh/README.md)同步。本轮实现证据独立于最初静态检查，原 54 项未减少。

未执行真实设备 namespace/SELinux、合作进程并发、断连、文件系统故障或掉电验收；未运行内核/Manager 构建，因为本批未修改其代码。本批没有激活、差异恢复、运行集合事务、方案 operation ID 或 Manager UI。缺模块/缺能力检测仍待加强，当前诚实标为未知。没有安装、设备规则操作、提交或推送，也未将完整目标标为完成。
