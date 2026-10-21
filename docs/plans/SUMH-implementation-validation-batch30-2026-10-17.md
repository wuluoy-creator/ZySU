# SUMH 第三十批实施与验证记录

日期：2026-10-17（Asia/Shanghai）。范围：F05 的 Manager 临时 Maps 工作台，消费第二十九批的分页查询与精确条件删除。对应[全面改进计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)及[54 项账本](SUMH-implementation-progress-2026-10-16.md)。本批代码、本地验证与文档已闭合；全部任务与 F05 保持进行，不将编译、宿主替身或 UI 渲染当作真实设备验收。

## 实现范围

Maps 设置新增独立临时规则入口，随父页面操作权限开放，不依赖保存的 Maps 开关或过滤代理当前就绪状态。打开不读取，显式刷新先查询能力、再读取第一页；下一页由用户明确请求。能力失败保留未知，与确实不支持分开。部分、完整、历史列表分别标注，失败保留历史行并禁止删除；累计最多 1024 条和 4 MiB 回包，不能自动重启过期分页。

新读取且查询/删除能力均明确支持时，允许从部分或完整列表选择精确键。确认展示完整 target ino/dev、spoofed ino/dev 和登记路径；无效 UTF-8 显示原始 hex。登记路径不是当前文件身份重解析。确认只提交一次，并用 instance/generation 和精确键绑定回执；成功、拒绝或未知都废止旧 token，要求显式刷新，不回退 clear、普通 daemon 或重试。查询后缺失只说明当前集合，不证明由哪次请求造成。

Activity 所有的状态对象保留进行中的动作、选择和回执，旋转、后台完成和关闭重开不重放。显式返回列表或刷新可关闭选择；进程重建只保存少量 pending/query-required 标记，不保存整份集合、token 或变更参数。未知结果先查询。界面提供中英文、主题颜色、48dp 控件、长内容滚动及状态播报语义；拒绝原因给出对应下一步。

源码：[界面](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHTemporaryMapsControls.kt)、[状态与生命周期](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHTemporaryMapsOperations.kt)、[严格模型](../../manager/app/src/main/java/com/zying/zysu/ui/sumh/util/SUMHTemporaryMaps.kt)。使用步骤与撤销见[Maps 指南](../zh/guide/sumh-maps-rules.md#manager-临时规则工作台)和[场景 26](../zh/guide/sumh-usage-scenarios.md#26-在-manager-核对并删除一条临时-maps-规则)。

## 原生合同与兼容

新增专用 `Natives.sumhTemporaryMaps` 和固定服务类型，复用有界 seqpacket/pidfd 通道。请求最多 1024 字节、原始响应 56 KiB、外层封装 224 KiB；未扩大通用 IPC。请求只允许 schema 1 的 capabilities/list/remove，拒绝未知/重复字段、非 UTF-8、NUL 和不规范的无符号十进制字符串；实例/generation 非零，inode/cursor 为 u64、device 为 u32。

内部精确 `sumhp sumh maps manager <json> --channel` 前缀在普通日志/daemon 初始化前分流，错误后缀也在此边界拒绝。root 检查先于解析和回调，私有通道 ACK 后才能执行回调；包装、全局选项、普通 daemon 和 operation 提交均不能调用。此形式不是脚本公共接口，第二十九批 list-temporary/remove 保持兼容。

JNI 返回既有 `{ok,exit_code,errno,stdout,stderr}` 传输封装；完整传输的 stdout 才包含内层 dispatch。内层可以明确拒绝或结果未知，不能由外层成功推断删除成功。不完整响应丢弃，使用现有 8 秒传输/10 秒调用期限和未结束 worker 隔离，不使用数字 PID 重试或盲目回放。

能力来自结构化 feature_capabilities，独立检查 inventory/delete 位；删除要求二者齐备。控制器 meta 增加 `maps_temporary_manager_schema:1` 和 `maps_temporary_manager_channel:true` 供发现，实际能力仍逐项读取。回调直接复用既有本地查询/删除命令、序列化和客户端；本批不修改内核/UAPI。没有每调用者 owner、持久删除日志或观察者实测声明。

源码：[请求](../../userspace/common/temporary_maps_request.hpp)、[回调](../../userspace/ksud/sumhp/src/mount/temporary_maps_manager.hpp)、[传输](../../userspace/common/maps_preview_transport.cpp)、[JNI](../../manager/app/src/main/cpp/sumhp_jni.cpp)、[精确路由](../../userspace/ksud/sumhp/src/embedded.cpp)。

## 验证记录

修复后的 Manager 全包 **60 类、613 项：611 通过、2 项因 Windows 符号链接权限跳过，零失败/错误**。本批 50 项新增模型、工作流、界面和真实 C++ 输出的跨语言检查全部通过。首轮失败保留，不用旧 XML 或历史批次结果代替本批测试。

| 检查 | 当前实际证据 |
| --- | --- |
| 原生请求/回调/客户端 | 121 项新增合同检查与 106 项既有临时规则检查通过，完整相邻客户端、状态、分页和配置 runner 通过。单独 Android/Linux errno 样本目标也通过 121 项。`out/batch30-client-host-final.log`；普通宿主 ABI 与样本目标分开，见下节。 |
| 通道/JNI worker/路由 | 165 项传输检查（其中 21 项新增），实际 worker 提取生命周期与期限检查通过；42 项 Manager 精确路由、原有 64 项临时 CLI 及普通路由回归通过。operation 211 项提交及 52 项输入 IO 检查通过。证据为 batch30-transport-host.log、batch30-cli-host.log、batch30-operation-host.log。 |
| Manager 最终全包 | `out/batch30-manager-full.log` 及同名 evidence 目录保存 60 类 XML：613 项中 611 通过、2 项权限跳过；新增 26 模型、11 工作流、9 界面和 4 跨语言测试全通过。跨语言测试消费 20 份实际原生封装，覆盖大整数、hex、部分/不支持/过期和删除结果绑定。 |
| Manager 早期失败 | 首次产品 Kotlin 编译通过，测试类型推断错误导致未执行；其误归档的旧 XML 明确作废。第二次执行 50 项，其中 48 通过、2 项模型/跨语言样本失败。修正后完整运行通过；早期日志保留。 |
| UI 渲染 | 实际 Robolectric 原生渲染产生英文确认、中文窄屏 2 倍字体、中文横屏深色 2 倍字体三张 PNG；主代理与 UI 代理查看图像，身份可滚动，确认/关闭/恢复可达。状态/回执具备 live-region 测试。图像不证明 TalkBack 或真实设备显示。 |
| ARM64 ksud | Release 编译链接与 827 条加固参数检查通过，RELRO + NOW；Build ID `4138daddb971c34937330332aaef9b4bccde5f57`，3951984 字节主文件、20277416 字节独立符号。日志 batch30-native-build-final.log。 |
| Manager JNI/APK | 实际 Release JNI CMake 首次因嵌套条件表达式违反 clang-tidy 失败；修复后编译和链接成功，未关闭检查。Debug APK 构建/打包通过，39020348 字节；ZIP 内 libruntime.so 与本轮 ksud 字节完全一致，ARM64 JNI 导出新入口，JNI Build ID `854893e566a56e629b15ee5852360eabc8368c8f`。日志 batch30-manager-jni-build-final.log、batch30-manager-apk.log，独立产物指纹见 batch30-manager-apk-manifest.json。未安装。 |
| 独立复核 | 模型代理独立审查原生增量及 UI，主代理复核跨层合同与渲染；原生审查无剩余可执行产品问题，保存 reviewed hashes。UI 代理另审查文档事实。实现者的测试报告与独立审查分开保存。 |

本地工具链为 Java 21.0.11、NDK 30.0.16248370、CMake 3.22.1、Zig 0.14.1 和捆绑 Python。Manager 命令为 `gradlew.bat :app:testDebugUnitTest --tests 'com.zying.zysu.ui.sumh.*' --console=plain --no-daemon -PABI=arm64-v8a`；专项仅选择 TemporaryMaps 类；APK 使用 `:app:assembleDebug`。日志与 XML 在 out/batch30-manager-*，生成样本保留独立指纹，未安装设备。

最终 `out/batch30-final-check.py` 核对 54 项 ID/部分状态、文档链接、当前源码/审查指纹、40 个原生输出和 20 个测试资源、13 个生成输入、测试 XML、PNG、日志及实际 ELF/APK；清单为 `out/batch30-final-manifest.json`。后续批次修改源码或共享构建目录后，应保留此历史清单，不将历史结果改写为新代码已通过。

## 发现、修正与证据区别

- R1：旧 `features()` 将查询失败折叠为零，不能用 `<0` 判断失败。新回调测试和主代理审查均发现此问题，改用 `feature_capabilities().ok/last_errno/bitmask`；失败及非法负位图均为 Unknown。旧辅助函数其他调用方保持原行为。
- R2：实际 C++ 跨语言样本使用 Windows errno，EOPNOTSUPP 为 130；Android/Linux 为 95。严格 Kotlin 解析正确拒绝。已改为单独目标 errno 的测试程序并断言常量，未扩大 Android 接受范围。普通宿主回归仍使用宿主 ABI，样本明确标注为合成目标而非 Android 执行。
- 测试输入修正：未配对 UTF-16 surrogate 在测试的默认 UTF-8 编码中先变为 `?`，并非真实畸形回包。改为注入实际 JSON 转义字节，保留生产严格校验。
- 构建修正：JNI 新容量选择使用 if/else，保留原预算，满足既有 clang-tidy。UI 测试的 nullable list 补显式类型；错误反馈按具体拒绝原因本地化。
- 记录修正：第一轮编译失败后的包装脚本误收旧测试 XML；该目录仅作失败诊断，不算本批通过。包装器现按请求类过滤，并在失败时排除旧时间戳结果。

## 未执行与后续

Windows 下未执行 POSIX exec/pidfd/继承 FD 真进程测试、符号链接权限用例和真实 fork/shared-mmap fixture。宿主 syscall、时钟、root 和 ioctl 替身不能证明 Android 通道、SELinux 或内核行为。未执行真实设备进程终止/TalkBack、实际临时规则删除、旧流并发、普通/授权/root/isolated 观察者矩阵、性能和完整内核 KMI/CFI 验收；本批无内核改动，不重跑无关提取模型。

F05 仍需持久删除历史、完整批量工作流与设备门禁；每调用者临时 owner 仍不存在，不能据此自动清理 F13 合成规则。F06/F07/F11/F12 等剩余范围按主计划继续，未实现接口仍为草案。静态审查、宿主测试、实际编译、打包和设备运行分开记账。
