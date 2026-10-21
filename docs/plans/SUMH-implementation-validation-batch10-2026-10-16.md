# SUMH 第十批实施与验证记录

日期：2026-10-16（Asia/Shanghai）。本批推进 F03 的开发预检、规范化预览与额外分区管理，依附 R03/R04/R06 和 C02/U02 的已有实现。[54 项主计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)与[实施账本](SUMH-implementation-progress-2026-10-16.md)保持完整范围，F03 仍为部分实施，未执行的设备验收不记为通过。

## 已实现行为

`module inspect <绝对模块目录>` 在有界 init 挂载命名空间查询中调用生产后端选择与 SUMH Planner。目录必须规范、没有符号链接祖先，且含真实普通 `module.prop`；不读取其内容。精确位于安装目录下的模块使用对应保存的 mode/rules，开发目录使用 `inspect.candidate` 身份和 SUMH 请求，不继承同名已安装模块的规则；两者均使用保存的全局设置与额外分区。检查单个模块时不按 disable/remove/skip_mount 过滤，明确不代表完整安装集合或下一次启动行为。生产编译器报告其实际走到的别名、whiteout/opaque、源链接、映射和错误；不保证一次枚举全部缺陷。其他后端选中、SUMH 不可用或编译不完整时不能声称完成了该后端的预检。

`module normalize <目录> --dry-run` 与实际 normalize 共用 `prepare_normalization`，列出 mkdir、move、symlink、unchanged、skip 和元数据。两次规划、配置/启动/源观察变化会使相应查询失效。预览不创建锁文件、目录或日志，不保存、恢复或挂载。独立查询复用既有私有目录非阻塞锁和 5 秒 query worker；响应上限 512 KiB，超过预算返回未知，普通 IPC 不扩容。

实际 normalize 再生成和核对同一计划，使用目录 FD、no-follow 和 `renameat2(RENAME_NOREPLACE)`，不覆盖目标、不自动递归删除或回滚。返回原有 `ok/module` 兼容字段，以及逐项结果、`effect_observed`、`durability_confirmed` 和恢复提示。失败可能已经移动目录或创建链接；`changed:false` 也不是无并发或无未知影响的证明。暂不提供持久化逐项恢复日志；现有操作账本不能恢复已丢失的逐项回包，断连后先 fresh dry-run/inspect，再明确决定是否继续。元数据检查不等于固定内容快照，也不排除 ABA；需要断电/并发真机验证。

`config discover-partitions` 通过有限目录描述符遍历读取全部已安装模块（含禁用/待移除/skip_mount），不读取 module.prop 内容或递归扫描任意文件树。候选必须符合既有名字语法，排除内建和保留目录；可新增项还需真实目标目录、可接受的平台别名，以及已观察到的受支持块文件系统挂载事实。overlay/tmpfs、挂载信息未知、冲突链接等不能推导为可挂载。扫描限制为 512 根目录项、256 模块、4096 项/观察、128 候选、512 来源、128 KiB 观察、256 KiB mountinfo、320 KiB 扫描结果；外层 512 KiB。旧 `sync-partitions` 精确本地入口现在返回同样的只读候选 JSON，不再打印误导性的 Added，也不会保存。

`config set-partitions <revision> <JSON字符串数组>` 是独立保存入口，绕过 daemon 冷启动，显式进入并恢复 init 命名空间。它使用配置锁的非阻塞模式、4 MiB 有界读、原字节 SHA256 revision 条件比较和严格格式校验。最多 128 个唯一名字、40 KiB JSON；仅更新 partitions 和必要的旧格式 schema 标记，迁移前备份原文。已有但当前无法识别的名字可保留/移除，新增项提交时重新发现并校验资格。配置变化返回 conflict；落盘使用公共三态 durable API，区分未替换、已替换但持久性未确认及完成，并读回确认。跨进程非配合 root 写入不受该锁串行化，不能承诺全局事务或未来挂载成功。该入口没有新增 operation ID/show/wait 协议。

Manager 设置中的“额外分区”提供手动发现、逐项选择、完整增删差异审查和独立“仅保存选择”。保存使用固定参数的专用 JNI/exec 私有通道，不通过 shell 或普通 daemon；共享原有 8 秒传输/10 秒 JNI 等待预算和单 pending 槽。超时或断连为未知结果，不自动重放。选择保存在有界私有草稿，外部配置变化必须重新发现、明确合并意图并再次审查；保存成功只更新本页读回，不触发其他页面的普通 daemon 刷新广播。新旧 Maps 传输保持兼容。使用了 `ui-ux-pro-max` 的表单反馈、恢复与可访问触摸交互指导。

## 测试与审查修正

- 修正了“无需写入”的 unchanged 结果与 UI 校验不一致：unchanged 要求已读回匹配，但不凭空宣称历史文件持久性已同步；真正 saved 仍要求 durable 完成。
- 分区查询失败返回完整的未知 DTO，避免缺字段造成无法显示结构化失败。模块 inspect 的 512 KiB 输出预算独立声明；全局 none/其他后端不冒充 SUMH 编译成功，摘要失败保持不完整。
- 保存原本广播通用 revision，会间接触发其他已组成页面的 daemon 查询；改为本地 receipt 更新，保留明确保存边界。
- 已选择的新增候选在重新发现时失去资格，原 UI 也会禁止取消勾选；修正为允许移除现存草稿选择，同时继续禁止重新添加无资格候选，补回归覆盖。
- 新接口接入后旧提取测试需要声明新的分派依赖；补充真实匹配函数或显式不可调用替身，保持既有 suite 覆盖。新增端到端分派测试验证包装/嵌套路径拒绝，不能仅靠 handler 单测证明入口可达。
- 首次 Manager 定向运行缺少 ANDROID_HOME；配置已有 SDK 后重跑。一个迟到响应测试在同一 UI 事件中更新外部值并返回响应，早于 Compose 观察；改为先完成外部值的重组再回包，以验证“已观察到外部变化”的合同。完整回归通过，后端 CAS 继续负责未观察到的并发更改。
- 并行工作在后期因服务额度错误终止。根任务检查已保存代码并补齐控制器、Manager 测试及集成；本批不宣称完成了独立审计或正式安全验证。

## 已执行验证

| 项目 | 当前证据 |
| --- | --- |
| 模块规范化 | 83 项生产规划/执行模型场景通过；包括安全根、别名、冲突、变化/伪造计划、no-replace、部分修改、fsync/读回失败。`out/batch10-run_module_normalize_host_tests.log`。 |
| 分区扫描 | 68 项真实扫描器配确定性 OS 边界测试通过，涵盖缺失/不完整、名字、挂载记录、符号链接、容量、异常和元数据变化。`out/batch10-run_partition_discovery_host_tests.log`。 |
| 条件保存/查询 | 54 项生产解析、配置锁、有界读取、CAS、备份、耐久/读回状态、扫描稳定性、禁止 query worker 变更及命名空间恢复测试通过。`out/batch10-run_development_host_tests.log`。 |
| 模块 Planner 控制器 | 原 71 项输入与 69 项控制器保留；新增 16 项 inspect，总计 85 项控制器通过。使用真实控制逻辑及后端/Planner 边界替身；原真实 Planner 测试证据仍见第九批。`out/batch10-run_module_preview_host_tests.log`。 |
| 实际 CLI 调用链 | 29 个 Maps + 78 个模块/开发分派场景（本批新增 35），并验证生产 parser、logger 静默、普通 daemon 拒绝和保存退出码。`out/batch10-run_maps_preview_cli_host_tests.log`。 |
| 兼容回归 | 17 个共享原生程序、44 项 Maps 观察/16 项路由、58 项固定传输及 12 项 JNI 生命周期/服务选择测试通过。对应 `out/batch10-run_*host_tests.log`。Windows 未运行真实 pidfd/fork/Android smoke。 |
| Manager | 38 类 / 280 项，零失败、错误、跳过；报告、32 张 PNG、日志及本批原生库存于 `out/sumh-batch10-manager-full/`。新增中文 2 倍字体横竖屏截图已查看，滚动区和固定按钮可访问。 |
| Manager 原生构建 | ARM64 Release 编译链接及必需 clang-tidy 通过，`out/batch10-manager-native.log`；未打包或安装 APK。 |
| ksud 与门禁 | 最终 ARM64 Release 编译链接通过，811 条编译参数/实际 ELF 加固检查及 24 项加固回归通过；7 项 CI 测试、54 个唯一任务 ID 与文档链接检查通过，普通 diff check exit 0。见 `out/batch10-{ksud-build,final-hardening,final-ci,final-docs,final-diff}.log`。新增 3 个 runner 已登记，远端 Actions 未运行。 |

最终 `ksud` 为 3,520,144 字节，SHA256 `9affae1cf9b6e4058f4f0048b287dbc43af16a6d069035b300cc3fb3453d89e4`；独立符号为 17,939,272 字节，两者 build ID 均为 `321adfc27f63459ff087a3a3f8a729cac6f5f29f`。Manager 原生库为 5,356,280 字节，SHA256 `2a492fc09be2448a08d6dfbcc1df98b3035b12b20e95c0f141e1184a24719660`，build ID `0bf27b22f233bd5d26220f3857f027461aa984c2`。完整路径及符号摘要保存在 `out/batch10-final-artifacts.json`。本批没有修改或重建 ksuinit/完整内核，不借用旧构建证据声称本批通过。

复跑新增宿主入口：

```sh
python3 userspace/ksud/tests/run_module_normalize_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_partition_discovery_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_development_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_module_preview_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_maps_preview_cli_host_tests.py --cxx g++
cd manager
./gradlew :app:testDebugUnitTest --tests 'com.zying.zysu.ui.sumh.*' -PABI=arm64-v8a
```

## 仍未完成

未执行真实设备、完整内核构建、观察者 APK/isolated service、进程终止/断电恢复、SELinux、热挂载并发、性能或远端 Actions 测试。无提交、推送、部署或设备变更。模块预检没有新的运行集合/owner 证明或应用 token；normalization 没有持久逐项恢复协议；新保存工作流不是 C04/F09 全操作协议的替代。F03、R03/R04/R06、C02/U02 与其他 54 项任务按原验收继续推进。
