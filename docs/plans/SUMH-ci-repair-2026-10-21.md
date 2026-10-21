# SUMH 首次推送后的 CI 修正

针对提交 `ccdd9ac7c1b6094deee2a5aa9c437bdefdb6ed8d` 的实际 GitHub Actions 结果收敛兼容问题，未扩展新的 SUMH 功能。

## 失败依据

- [SUMH regression tests](https://github.com/wuluoy-creator/ZySU/actions/runs/39526754975)：9 个用户态主机入口在 Ubuntu/GCC 编译阶段失败，分别涉及 `const` 转换目标、glibc 函数属性、`CLONE_NEWNS` 宏冲突和遗漏的 `<climits>`。其余内核及用户态入口通过；Manager 子任务通过，汇总门禁正确拒绝放行。
- [Build Manager](https://github.com/wuluoy-creator/ZySU/actions/runs/39526755120)：829 项单元测试中 2 项隐藏规则编辑测试在点击后找不到编辑器节点，构建因测试失败停止。控制器、启动器及 android14-6.1、android15-6.6、android16-6.12、android17-6.18 模块构建通过。
- 同一提交的 clang-format、clang-tidy、ShellCheck、独立用户态回归、Manager discovery 工作流均通过。此前本机通过结果仍保持其原有平台边界，不能代表这些首次远端失败已经通过。

## 修正

1. 两个传输实现先排除负返回值，再以 `std::size_t` 比较报文长度，避免将 `const auto` 推导出的限定符带入转换目标。短报文、错误、ACK、超时及 FD 收尾逻辑保留。
2. logger 使用无状态 callable 关闭文件，避免将 glibc `fclose` 的函数属性作为模板实参；保留原有所有权及静默日志行为。
3. 测试夹具显式包含 `<climits>`，只在系统未定义时提供 `CLONE_NEWNS` 替身；Linux 使用系统宏，Windows 使用原替身。配置方案样本生成器内部变量避开 POSIX `index` 名称。原有断言、警告强度和 Linux 执行入口不变。
4. 重新执行配置方案的真实原生协议样本生成，23 组请求／响应与原资源逐字节相同，仅更新生产源码及生成器的溯源哈希。
5. 隐藏规则页的编辑入口需要同时满足规则读取、编辑草稿与导入导出状态就绪。测试同步等待独立 transfer ViewModel；用受控忙碌状态覆盖点击入口的禁用与恢复，避免将 Compose 空闲误认为所有外部 IO 已结束。

隐藏规则竞态的新增用例先在旧产品条件下运行，并在“行编辑入口应禁用”的断言稳定失败，验证记录未覆盖。修正只调整入口的启用条件，选择模式和复制继续可用；不通过重复点击、固定 sleep、跳过测试或放宽原断言规避失败。

## 验证记录

- 6 个受影响的测试夹具入口在 Windows 执行通过，并以实际 Linux 头文件交叉编译、链接通过；生成器也完成 Linux musl 编译及 Windows 样本生成。
- Maps／配置方案传输故障模型、完整 CLI 和 logger 回归通过，相关 POSIX 二进制交叉编译通过。Linux 交叉编译没有被标为实际执行。
- 修正后的 Android ARM64 `ksud` 和 JNI 在默认 clang-tidy 开启时重新构建、链接通过；`ksud` ELF 加固检查通过。未签名、安装或替换工作区载荷。
- 隐藏规则页 13 项测试全部通过；与 Build Manager 相同的 4 组过滤条件合计 830 项，827 通过、0 失败／错误、3 项既有 Windows 符号链接权限跳过，运行期间输入稳定。证据为 `out/batch46-manager-ciRepairManagerRegressionAfter/` 和 `out/batch46-manager-ciRepairManagerFinal/`。
- 原生格式检查 504 个文件通过。工作流检查没有被跳过或降级。

原始失败日志、下载的测试报告、修正验证保存在本机 `out/ci-repair-*`；原生完整构建记录为 `out/prepush-ksud-evidence-ci1/` 与 `out/prepush-jni-evidence-ci1/`。GitHub 复验结果须按修复提交 SHA 核对，真实设备、namespace／SELinux 和掉电恢复仍需单独验收。

## 第二轮：真实 Linux 运行与 Manager 后续门禁

提交 `cec4e24738c872a727252be2542bb7c905f43e5a` 的远端复验消除了首轮 GCC 编译错误，并通过格式、clang-tidy、独立用户态回归及四个 KMI 模块构建，但暴露了此前未到达的运行与 Lint 问题：

- [SUMH 回归](https://github.com/wuluoy-creator/ZySU/actions/runs/39530278014) 的 Linux 主机测试在真实进程通信首个请求失败。Linux `recvmsg` 会在输出 `msg_flags` 保留 `MSG_CMSG_CLOEXEC`，Maps 和 Profile 传输原先要求 EOF 的标志全部为零，因此将正常结束误判为 `EPROTO`。两处仅允许该回传标志，同时要求零字节、无描述符、无控制数据；截断、未知标志及异常 ancillary 数据继续拒绝。
- 两个传输模型补入 Linux 的标志回传行为，均先证明旧实现失败，再通过修正；各补 5 个 EOF 边界场景。真实 POSIX 测试仍保留，并在首个请求失败时输出错误 envelope，方便定位。
- 同一 SUMH 工作流的 797 项 Manager 测试有 6 项等待错误，涉及模块规则展开、分区发现和旧隐藏草稿的首次加载。测试现在明确等待真实 IO 的主线程续体与草稿就绪，再断言按钮可用并单次点击；保留原有超时及业务断言。
- [Build Manager](https://github.com/wuluoy-creator/ZySU/actions/runs/39530278090) 的单元测试通过，后续 debug/release Lint 各报告 709 项错误，尚未生成可交付的最终 APK。这与 SUMH 专项工作流的测试结果分别记录，不能混为一次通过。

Lint 修正沿用现有仓库约定：新增 SUMH 文案保持英语与已有简体中文，尚未翻译的其他语言（包括新增文案的繁体中文）使用 Android 英语回退；仅对应资源声明 `MissingTranslation` 例外并核对简中键与格式参数，没有关闭项目级检查或修改 `warningsAsErrors`。实际计数文案改用复数资源，多项统计改为独立计数标签，确认无引用的资源及其翻译一并移除。

两个工作流 ViewModel 的 `SavedStateHandle` 改由生命周期工厂注入，移除生产代码调用测试专用默认构造器的路径；直接构造的测试显式传入测试 handle，并增加真实 Activity 工厂构建与旋转保留验证。其他修正包括配置感知的剪贴板标题、Compose 原始数值状态和 URI 扩展调用。

第二轮本机传输验证记录位于 `out/ci-repair-transport-eof-a3/`，含实际生产模型、CLI、Linux 交叉编译与 clang-tidy；旧实现失败记录为 `out/ci-repair-transport-eof-before-*.log`。交叉编译不代表 Linux 执行通过，最终仍须核对后续修复 SHA 的完整 GitHub 工作流。

第二轮提交前验证：

- 默认 clang-tidy 开启的 ARM64 `ksud` 与 JNI 构建通过，`ksud` 加固检查通过，原嵌入载荷未改动。记录为 `out/prepush-ksud-evidence-ci3/` 与 `out/prepush-jni-evidence-ci3/`。
- SUMH 专项 798 项、Build Manager 完整过滤集 831 项均为 0 失败／错误，各有 3 项既有 Windows 符号链接权限跳过；两种过滤集分别执行并保留记录。debug/release Lint 均通过。记录为 `out/ci-repair-manager-round2-validation/`。
- 191 个资源 XML 解析通过，保留的 668 个英语／简中资源键及格式参数一致；8 个真实复数资源和 4 个多项计数标签完成转换，11 个无引用资源的 63 处定义移除。
- `out/ci-repair-round2-precommit-evidence.json` 核对原生构建、Manager 两组测试及 Lint 的源码哈希与最终工作区一致，`git diff --check` 通过。以上仍是本机证据，远端工作流及真实设备验收分别记录。

## 第三轮：草稿测试同步与最终 APK 校验

提交 `3ccc04b482a025989b8181b3ccea94457993733c` 的远端结果确认：Linux 主机 SUMH 回归已实际执行通过；Build Manager 的 Manager 测试、debug/release Lint、APK 编译，以及 ksud、ksuinit 和四个 KMI 构建均通过。仍有两个独立失败：

- [SUMH 回归](https://github.com/wuluoy-creator/ZySU/actions/runs/39533929111) 的 Manager 子任务有 2 项 Maps 工作流测试，在输入前遇到仍禁用的 `maps_import_json`。共享草稿由真实 IO 加载并回到 Android 主线程，Compose 空闲不足以证明草稿加载和初次注册完成。
- [Build Manager](https://github.com/wuluoy-creator/ZySU/actions/runs/39533929262) 已完成重打包与签名，但最终载荷校验在扫描 ZIP 目录时拒绝大小写变体，尚未比较 ksud 字节，最终 APK 因此没有上传。

Maps 测试统一等待共享 store 的 `loaded` 和对应草稿键，并推进 Robolectric 主线程，随后断言输入框可用并单次输入。模块模板的模块／模板两层草稿及隐藏编辑页采用相同的明确就绪条件；生命周期和操作历史测试中真实共享 store 的首次加载也推进主线程。没有修改生产业务、削弱业务断言、增加重试或延长已有超时。

实际下载的 Gradle APK 有 656 个条目，没有同名重复，但有 24 组合法的 Android 资源大小写变体，例如 `res/2F.xml` 与 `res/2f.xml`。修正仅在 APK 检查中允许精确 `res/` 命名空间内的文件大小写变体，并继续按 ZIP 原名读取，不解压资源到宿主文件系统。相同全名重复、其他命名空间的大小写别名、路径穿越、容量限制、ABI、marker 和 canonical ksud 字节检查均保持；可解压证据包仍拒绝大小写别名。

新回归先在旧实现上复现拒绝合法资源的问题，再验证修正；负例覆盖资源重复、`RES/` 别名、marker／载荷／assets 别名、穿越及证据包。244 项 CI Python 测试执行完成，0 失败、10 项环境条件跳过。使用上述真实中间 APK 和同次运行的 canonical ksud 在本机执行未签名重打包，载荷字节校验通过，482 个资源内容全部保持原样；没有本机签名或安装。证据位于 `out/ci-repair-round3-*`，独立只读复核记录为 `out/ci-repair-apk-round3-review.md`。

第三轮提交前，5 个受影响测试类的 41 项定向测试、SUMH 专项 798 项、Build Manager 完整过滤集 831 项均为 0 失败／错误；两组完整过滤集各有 3 项既有 Windows 符号链接权限跳过。debug/release Lint 通过，运行期间输入稳定。记录分别为 `out/batch46-manager-ciRepairManagerRound3Targeted/` 和 `out/ci-repair-manager-round3-validation/`，提交前源码一致性核对为 `out/ci-repair-round3-precommit-evidence.json`。原嵌入载荷未变，未修改工作流或关闭检查。远端完整 Build Manager（含最终重打包、载荷校验和上传）仍须按修复提交 SHA 单独复验；上述本机记录不代替真机验收。
