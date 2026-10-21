# SUMH 第十三批实施与验证记录

日期：2026-10-16。对应[54 项实施账本](SUMH-implementation-progress-2026-10-16.md)和[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)。本批实现 F04 的控制器/存储/恢复基础，同时推进 C02/C04/C05 的定义状态与逐资源结果。F04 仍为部分实施：Manager 的定义、分组和停用项编辑界面及真实设备验收尚未完成。整体 54 项目标继续进行。

## 实现范围

- 用户隐藏文件新增 schema 1 六字段定义，保存稳定 ID、原始路径、enabled、name/note/group。旧路径数组只读兼容，重复精确路径按首次顺序合并；首次显式修改前耐久备份原始字节，不在查询/启动恢复时迁移。未知格式、读取失败、重复键、非法 UTF-8、容量超限均保留文件并拒绝破坏性保存。
- 新增带完整文件 revision 的 create/annotate、按 ID enable/disable/retry/remove-id、按组 enable-group/disable-group。定义修改和自动恢复共用非阻塞文件协作锁，持有至内核同步结束。恢复仅投影 enabled 路径；停用保留定义，受管删除指定路径及采样的内核 ID，不用全局 clear，不写 SUSPENDED。
- 新的运行变更要求受管所有权能力。保存失败或未知不继续本次内核变更；保存确认后按选中项同步并读回，分别统计确认、失败和未知，不盲目回滚。结果的 applied 只表示注册成员关系，绑定状态、全局暂停及观察者实测独立报告。元数据修改只保存，不查询内核。
- 精确本地 hide status 绕过 daemon 启动/恢复，返回 schema 1 保存定义与采样运行状态、revision、boot ID 和分页信息。每页最多 16 条，动态控制转义后整个回包小于 56 KiB；再次核对文件版本才发布结果。没有跨页内核运行原子快照保证。
- 存储最多 4096 定义、4 MiB；名称/组名各 128 UTF-8 字节，备注 1024 字节。变更回包最多 64 个逐项结果，同时给出全量计数和完整性。operation prepare/execute 复用这些新命令的生产语法和 journal 分类，保留既有 48 KiB 输入、64 KiB 外层请求约束。
- 保留旧 path add/remove/list。旧 add 已有启用定义仍触发重新绑定；disabled 定义不能被 add 隐式启用。新增 retry-path 在锁内复核定义仍存在且启用，再注册/重试；Manager 的单条和批量 Retry 均使用此入口。当前 Manager 保存列表为 enabled 投影，完整停用定义使用新 CLI 管理。
- api state 的用户隐藏 desired revision 继续表示 enabled 路径集合，元数据/顺序不影响该运行成员关系摘要；detail 标注范围和 enabled/disabled 数量。完整定义版本由 hide status 提供，不混同两个 revision。

源码：[定义模型](../../userspace/ksud/sumhp/src/core/user_hide_model.cpp)、[存储与迁移](../../userspace/ksud/sumhp/src/core/user_hide_store.cpp)、[输入合同](../../userspace/ksud/sumhp/src/core/user_hide_input.hpp)、[控制器](../../userspace/ksud/sumhp/src/core/user_hide.cpp)、[恢复](../../userspace/ksud/sumhp/src/mount/sumh.cpp)、[状态采样](../../userspace/ksud/sumhp/src/core/state_ledger.cpp)、[Manager 重试](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHManager.kt)。

## 本批实际验证

| 验证 | 结果与证据 |
| --- | --- |
| 定义模型/存储 | **177 项断言通过**，生产 codec、Transaction 和兼容读写包装，IO/hash/锁/耐久提交为替身。覆盖稳定旧 ID、重复路径合并、元数据/disabled 保留、未来格式、UTF-8 字节及 4 MiB 边界、原字节备份冲突/故障、源变化、未提交/未知区别和锁持有。`out/batch13-storage-host.log`。 |
| 控制器 | **190 项断言通过**，实际控制器 namespace 与生产模型，Transaction 和 ioctl 为计数/故障模型。覆盖条件版本、能力、单条/组启停、定向重试、旧 add 重新绑定、disabled/已删除保护、部分组失败、读回未知、结果截断和极端 JSON 转义分页。独立模块 HIDE fixture 仅证明调用受管删除边界，没有证明真实内核隔离。`out/batch13-controller-host.log`。 |
| 状态账本 | 原有回归及 **375 项扩展断言通过**；实际定义 codec 接入 enabled 集合、停用残留漂移、元数据/顺序不改变 desired revision、未来格式未知；新增采样不写入/调用命令。`out/batch13-state-host.log`。 |
| 原生核心回归 | **17 个测试程序通过**，包括恢复锁覆盖、反复 boot/event 恢复不添加 disabled、持久读取兼容、迁移保留丰富定义及未来格式。Windows 符号链接/fork fixture 限制仍按日志跳过对应平台行为，不冒充已测试。`out/batch13-core-host.log`。 |
| main→CLI→embedded | 新增 **37 个隐藏定义路由/root/参数场景通过**；保留 29 Maps、78 模块/开发、51 操作查询及 90 提交场景。实际通用 CLI parser、只读精确入口、包装拒绝、异常日志恢复和 capability metadata；查询/daemon 为计数替身。`out/batch13-cli-host.log`，另有原 parser/logger 回归通过。 |
| 脚本提交 | **147 项提交断言、52 项输入 IO 断言通过**。新增稳定定义命令及 retry-path 接受，查询/错误 ID/错误 revision/非法字段/重复键拒绝；未执行目标。`out/batch13-submission-host.log`。 |
| Manager 定向回归 | **4 类、23 项、零失败/错误/跳过**：SUMHHideBatchTest、SUMHHideRemovalTest、SUMHUserHideSnapshotTest、SUMHUserHideRulesTest。Gradle 编译与测试成功，`out/batch13-manager-hide.log`，XML 存于 `out/sumh-batch13-manager-targeted`。这些既有测试验证批量/移除/列表合同，不宣称在设备执行了新的 retry-path。 |
| ARM64 ksud | Release 编译与链接通过，`out/batch13-ksud-build.log`。实际编译新 store/model/controller 和集成源；沿用 batch07 构建目录，clang-tidy 仍关闭，不记为本批静态分析通过。 |

宿主 runner 使用现有 Python 与 Zig 0.14.1：`run_user_hide_host_tests.py`、`run_user_hide_controller_host_tests.py`、`run_sumh_state_host_tests.py`、`run_sumh_host_tests.py`、`run_maps_preview_cli_host_tests.py`、`run_operation_submit_host_tests.py`。两个新 runner 已注册到 `scripts/ci/sumh_ci.py`。Manager 从 manager 目录运行 `gradlew.bat :app:testDebugUnitTest --tests <上述各类完整名称> --console=plain --no-daemon -PABI=arm64-v8a`，使用 JDK 21 和本机 Android SDK。

本批没有重跑整个 Manager 测试包，也没有重新声称第十二批的 Linux POSIX 二进制执行通过。已有非 Android logger 未使用变量和 Gradle 弃用警告保持可见。

最终 **24 项实际 ELF/编译参数加固检查、7 项 CI 门禁测试、54 项编号对应、本地文档链接与 git diff --check 均通过**。日志为 `out/batch13-final-{hardening,ci,docs,diff}.log`；文档检查覆盖本批新增指南和验证记录。

最终 ksud 为 3,646,544 字节，SHA-256 `45abe42717cb0011e093d0035fab865e175beb4289a6a9bed53`；独立符号为 18,627,648 字节，SHA-256 `b9c79a2688ec7a9bc6a87d8ad24b1c1814c732ab348d45f8c9e14ba2b97fd603`。两者 build ID 为 `a668b5741537821b410a56416b00b93958e24e48`；完整 manifest 为 `out/batch13-final-artifacts.json`。构建目录沿用 batch07 名称，产物是本批最终代码，未部署。

## 审查与交付边界

集成审查发现 generic CLI 将 string_view 直接放入 vector<string> 初始化列表导致 libc++ 编译失败；已显式构造 string，并重跑全部 17 个核心程序通过。根代理进一步核对内核 UPSERT 实际会增加 generation 并安排绑定，修正旧 add 被优化成无操作的回归；Manager 重试改用锁内检查的 retry-path，补齐缺失/停用、旧内核兼容、失败及部分组结果测试。

协作代理复核控制器及最后重试差异后没有剩余可确认问题，记录为 `out/batch13-review-findings.md`。其负责存储层实现和部分集成测试，因此存储检查属于自检；对控制器的复核也不替代真实设备验收。所有权、锁和耐久接口在宿主用例中有边界替身，不能据此证明实际 POSIX fsync、内核并发或掉电恢复。

使用指南和本验证记录另经源码/日志对照复核，未发现范围内不一致，记录为 `out/batch13-doc-review.md`。该复核没有重新执行测试，不增加设备验证结论。

已更新主计划、全部 54 项账本、[用户隐藏定义指南](../zh/guide/sumh-user-hide-definitions.md)、[场景 16](../zh/guide/sumh-usage-scenarios.md#16-按用途分组并暂停用户隐藏定义)和中文导航。主计划保留原任务，不将控制器切片标为 F04 整体完成。当前没有自动降级转换命令，原始迁移备份也不是之后全部定义编辑的恢复点。

设备重启、实际 daemon/JNI/socket/SELinux、真实内核所有权、绑定事件与删除并发、普通/授权/root/isolated 观察者、实际文件系统锁/fsync/掉电、性能和远端 Actions 本批未执行。协作锁不防止任意 root 绕过接口改文件/内核；配置提交与内核同步不是全局原子事务。运行成员关系确认不等同隐藏效果确认。

本批仅修改本地工作区，未提交、推送、发布、部署或修改真实设备。
