# SUMH 第十七批实施与验证记录

日期：2026-10-16。对应[54 项实施账本](SUMH-implementation-progress-2026-10-16.md)和[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)。本批推进 F02 Manager 生产 Planner 预览与路径解释，代码、主机验证和文档已闭合。整体 54 项目标仍在进行；完整设备验收未闭合。

## 实现范围

- 模块设置新增显式预览窗口，打开不读取。用户可读取保存配置、比较当前模块的完整未保存模式/规则，或填写原始路径后手动解释。未完成单条编辑先完成或取消；冲突/非法候选不妨碍读取保存配置。候选不覆写全局设置，其他模块保持保存值；空规则列表表示完整清除候选例外。
- 新 typed JNI 与精确私有子进程路由复用生产 `module_preview_json`，输入内联且不可变，不创建暂存文件或走普通 daemon。结构/容量在父子两端校验，完整配置语义交给原生产校验器。公共 `module plan/explain --input` 保持兼容；私有路由不是新增公开 CLI。
- 单独协商 Manager 预览传输 schema、保存读取、内联候选及路径解释能力。能力未知、不支持或调用失败时保留错误，不回退到可能启动 daemon 的旧查询。预览不保存、迁移、normalize、恢复或应用规则。
- 原生沿用 init 命名空间内两次编译、源元数据与配置/boot/别名/运行上下文核对。响应维持 schema 1；完整仅表示当前可挂载集合的 SUMH 编译贡献。外部 owner、跨后端最终赢家、完整运行集合、源固定/内容哈希、下次启动完整集合和观察者视图均未证明；`module_apply=false`、`apply_token=null`、不授权热应用。
- 64 KiB 候选和 2 MiB 原始响应仍是 Planner 专用预算，模块规则维持 256 条/56 KiB 编码限制。ModulePreview 服务单独使用 2 MiB，其他服务保持 512 KiB，帧 4096 字节、传输 8 秒、JNI 调用者 10 秒、内部查询 5 秒。模块原始响应禁止裸控制字节，stdout 字符串封装最多 `2 × 2097152 + 256 = 4194560` 字节；普通 IPC 和公共解析器容量不扩大。
- Kotlin 专用严格解析器以 2 MiB、16 层、262144 节点及各数组限制校验，拒绝重复/未知字段、错类型、非法 UTF-8、错误摘要/路径/目标完整定义及互相矛盾的成功标志。旧保存规则的非规范/重复/超过 256 项仍按读取合同展示。保留 utf-8/hex、空诊断、部分结果、早期无样本错误和未知运行身份，不把缺数据伪造成空计划。
- 单个 Activity ViewModel 保存一个在途请求和一个观察，后台编码/解析，完整输入和结果不进 Bundle。冻结候选字节、路径和本地上下文；实际编辑递增 epoch，因此改后还原仍会过期。关闭/重开或换模块隔离迟到响应，前次原生读取真正结束前不并发启动新读取。刷新清除旧证据，失败保留模块草稿；进程重建不恢复观察或自动读取。
- 界面分保存/候选/差异以及后端、配置、有序贡献、路径事件、完整 trace 和诊断。所有路径以精确引用或 hex 展示；原生顺序不重排，ADD/MERGE 的写透源和隐藏祖先明确标注。LazyColumn 使用包含侧别/章节/位置的键，长诊断按不拆 UTF-16 字符的 2048 字符块显示；大字体下控件可滚动，关闭按钮始终可用。

源码：[私有请求合同](../../userspace/common/module_preview_request.hpp)、[有界传输](../../userspace/common/maps_preview_transport.cpp)、[生产 Planner](../../userspace/ksud/sumhp/src/mount/module_preview.cpp)、[JNI](../../manager/app/src/main/cpp/sumhp_jni.cpp)、[模型与校验](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHModulePreview.kt)、[工作流](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHModulePreviewOperations.kt)、[入口与草稿](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHMountConfigDialog.kt)、[结果界面](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHModulePreviewDialog.kt)。

## 实际验证

| 检查 | 结果与证据 |
| --- | --- |
| 原生传输 | 143 项故障模型及生产 bounded JNI worker 检查通过。null/空候选、精确 64 KiB、Unicode/元字符/制表符/尾空格、2 MiB、溢出、最坏封装、裸控制字符、ACK/期限/隔离；`out/batch17-native-transport.log`。Windows 未执行真实 POSIX exec/pidfd smoke。 |
| 生产输入/Planner | 71 项候选输入及 91 项控制器场景通过，包含内联摘要/完整替换/no-op、文件与内联互斥、语义拒绝和源变化失效；原文件/stdin及 256 条/56 KiB 边界通过。`out/batch17-native-planner.log`。 |
| CLI 边界 | 新增 26 项私有 Manager 路由检查及既有 Maps/module/development/hide/operation/parser/logger 套件通过。非法/包装/非 root 路由拒绝，预览日志路径零文件打开；`out/batch17-native-cli.log`。 |
| ARM64 构建 | ksud Release 编译/链接通过，820 项编译命令校验；继承构建中 ksud clang-tidy 关闭。Manager JNI/transport 改动翻译单元 clang-tidy 和链接通过；`out/batch17-native-build.log`、`out/batch17-manager-native.log`。 |
| 原生加固 | 24 项真实 ELF/逐项编译参数加固检查通过；`out/batch17-native-hardening.log`。 |
| Manager 全 SUMH 包 | **53 类、517 项：515 通过、2 跳过、零失败/错误**。日志 `out/batch17-manager.log`，XML `out/sumh-batch17-manager`。两项跳过为既有操作存储/隐藏交换存储在 Windows 无符号链接权限；本批新增测试均通过，未以跳过冒充通过。 |
| 新增模型 | 31 项通过，含完整/部分/过期/早期错误、摘要/路径/完整定义绑定、无请求配置改变、legacy 重复与超过 256 条、接近 2 MiB 高密度配置、600 个事件和重复解释、UInt64/UTF-8/hex、逐项能力、封装/深度/节点边界。 |
| 新增工作流/入口 | 10 项 ViewModel 与 5 项编辑器集成通过：打开零请求、完整未保存规则、保存读取与冲突隔离、改单再还原、上下文变化、关闭/重开与迟到响应、单请求限制、无自动重放、输入失败保留和实际 Activity SavedState 小于 32 KiB。真实 OS 生命周期仍待设备验证。 |
| 新增界面 | 13 项通过，含状态优先级、冻结/新路径分离、空事件的未知含义、所有来源/别名/隐藏祖先、1024 个重复目标懒加载及末项可达、长文本精确保留、页面状态恢复无读取、中英文与中文横竖屏 2 倍字体。语义检查不替代 TalkBack。 |
| 交付核对 | 24 项实际原生加固、7 项 CI 门禁单元、54 个唯一编号与账本一致、本地文档链接及 `git diff --check` 通过；`out/batch17-final-{hardening,ci,docs,diff}.log`。最终产物摘要 `out/batch17-final-artifacts.json`。 |

## 审查、修正与剩余边界

原生通道和 Kotlin/生产合同的交叉独立复核未发现阻断问题，记录 `out/batch17-native-independent-review.md`、`out/batch17-model-independent-review.md`。另一次工作流只读复核未发现会话隔离、单请求、冻结输入、过期 epoch 或 Compose 接线的阻断问题，根代理将最终意见记录于 `out/batch17-root-workflow-review.md`。

首轮有两项界面文本断言失败，改为明确的子串匹配；另一个集成用例在同步编辑后等待 SideEffect 过期状态时超时。先隔离各用例模块身份并保证中间模式不同、断言 editEpoch 精确增加 2；单用例通过但完整运行仍暴露重组同步不足。补 `compose.waitForIdle()` 等待编辑器重组后再断言，28 项界面/工作流定向测试及最终全包均通过。未改产品 dispatcher、放宽时限或删除原有断言。失败与定位日志分别保留 `out/batch17-manager-initial.log`、`out/batch17-manager-second.log`、`out/batch17-manager-epoch-isolated.log`、`out/batch17-manager-workflow-synchronized.log`。

首轮截图发现中文窄屏标题最后一个字独占一行，使用 Heading 换行后重新生成。根代理查看 `manager/app/build/reports/sumh-layout/module-preview-{narrow,landscape}-large-text.png`：窄屏标题分两行，内容可滚动，深色横屏错误说明及固定关闭按钮无重叠。大响应、真实设备渲染性能没有因此被标为通过。

最终 ksud 为 3,730,608 字节，SHA-256 `d7859090379826c183f930d86e68f0dea36ea469a25d396f7a37634b0c7d914f`；匹配符号 19,086,152 字节，共享 build ID `5a6ba504d43ee48c53d6eb4fe0762beeff903f1a`。Manager JNI 5,614,992 字节，SHA-256 `be26e18372261ca1d6f201c880d306bf045e27c8a439d057a64688d364db9a39`，build ID `605d107aff64d8adaf815f512d0b729701a36975`。这些是本地构建产物，不是已安装或设备运行验证的发布包。

使用说明合并到[模块预览指南](../zh/guide/sumh-module-rules.md#manager-计划预览与路径解释)、[场景 19](../zh/guide/sumh-usage-scenarios.md#19-保存前比较完整模块草稿并解释路径)、中文导航、主计划及账本。其他未实现命令仍标为草案，本批不增加 F06 立即重应用、F07 统一方案或新的内核 ABI。

真实设备的 Activity 旋转/进程死亡/后台完成、TalkBack、JNI/root/socket/SELinux/pidfd、挂载命名空间、普通/授权/root/isolated 观察者、重启、2 MiB 最坏设备性能、四 KMI 与远端 CI 均未执行；宿主测试和静态复核不替代这些证据。没有提交、推送、部署、发布或修改真实设备。F02 仍为部分实施。
