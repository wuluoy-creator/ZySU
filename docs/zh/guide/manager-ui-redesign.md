# 管理器页面与操作指南

ZySU 管理器使用 Kotlin、Jetpack Compose、Material 3 和 Compose Destinations。页面采用圆角面板、胶囊导航及统一顶栏；外观选项只改变管理器显示方式，Root 权限和内核功能仍受认证、协议及运行能力约束。

## 导航结构

| 入口 | 内容 | 使用条件 |
| --- | --- | --- |
| 首页 | 安装与运行状态、认证、ksud 和 SUMH 摘要、更新提示、设备信息、重启菜单 | 可在未取得管理器权限时进入；各操作单独检查权限 |
| 授权 | 应用搜索与排序、Root 授权、应用配置、模板、授权策略和记录入口 | 需要 Root 条件；内核操作还需有效管理器身份 |
| 模块 | 已安装模块、启停/卸载、更新、配置、WebUI、模块仓库和仓库源 | 需要 Root 条件 |
| 设置 | 外观、内容与显示、应用与更新、挂载与隔离、内核策略、诊断与日志 | 设置首页可直接进入，内核相关入口按权限显示或禁用 |

授权列表的策略入口包含配置模板、SuperKey 和默认卸载模块策略；记录入口用于 Root 请求与命令记录。记录功能关闭时会提示前往诊断设置启用。

## 首页状态卡片

### ksud

首页 ksud 卡片显示版本和文件完整性状态，点击整张卡片会把 APK 内置的 ksud 同步到设备。同步后重新读取版本和完整性，只有写入成功且校验一致才报告成功。操作期间禁止重复点击，并保留必要的写入及回读流程。

同步需要有效管理器权限、匹配的内核 UAPI，以及 APK 中的 `libruntime.so`（文件名由 `manager/ksud-library-name.txt` 定义）。缺少条件时卡片显示原因。勾选或“校验一致”表示设备上的 ksud 与安装包内置文件一致，不表示守护进程正在运行。

“设置 → 应用与更新”中的自动更新 ksud 默认关闭。开启后，管理器启动时会按需同步 APK 内置的 ksud；它不从网络独立获取另一个 ksud 发行版本。

### SUMH

点击 SUMH 卡片刷新协议号和视图状态。当前协议号来自内核接口，不是 ZySU 发行版本；协议可用也不等于视图已开启。加载失败会显示失败状态，并可展开原因、再次刷新。无有效管理器权限时不能读取完整状态。

首页仅显示摘要。完整配置、规则、挂载信息和日志通过设置中的相应工作区查看；一般挂载数量不能直接视为 SUMH 视图数量。

## 设置工作区

| 工作区 | 当前内容 |
| --- | --- |
| 外观 | 语言、主题、配色、背景、显示大小及返回手势相关偏好 |
| 内容与显示 | 首页布局、主卡片图片与覆盖度、核心信息逐项显示、模块信息及内容显隐偏好 |
| 应用与更新 | 发行版更新检查、自动同步 ksud、卸载/恢复等维护操作 |
| 挂载与隔离 → 挂载 | OverlayFS / Magic Mount 后端开关、文件系统类型、挂载源、工作目录与镜像路径 |
| 挂载与隔离 → 隔离 | 进程隔离选项、内核构建信息、stealth、挂载隐藏、OverlayFS xattr、statfs、maps 及自定义隐藏路径 |
| 挂载与隔离 → 生效规则 | 已登记的规则及其状态、刷新与规则维护 |
| 内核策略 | 可配置内核功能、su 路径等高级选项以及 WebUI 引擎 |
| 诊断与日志 | 日志导出、Root 日志开关/记录、SUMH 调试选项与日志；WebUI 调试页仅在 Debug 构建显示 |

模块自身的挂载模式和路径规则从模块页的挂载配置入口管理。若挂载由外部组件接管，挂载工作区会显示接管者并禁用相关控制。

内核报告的能力决定对应开关能否使用。当前“挂载隐藏”只提供开关，管理器保存配置时使用 `normal`，没有隐藏级别选择器。内核构建信息设置详见 [SUMH 内核构建信息](sumh-kernel-build.md)。

SUMH 配置更改会保存并按操作结果回读。界面区分“已保存并应用”“已保存，需重启”和“已保存但运行时应用失败”；最后一种状态提供重试应用入口。保存期间暂时阻止页签切换和离开工作区，避免操作尚未完成时丢失反馈。不能仅凭开关已变更判断运行状态已经生效。

## 安装入口

从首页进入安装页，当前可选流程为：

- 选择本地镜像文件进行修补。
- 输入远程文件地址，探测可用启动分区后下载并修补。
- 在已有 Root 时直接安装；A/B 设备额外提供安装到非活动槽位的选项。

普通 LKM 流程会解析目标镜像或目标槽位的 KMI；使用内置 LKM 时还需与内置模块列表匹配，无法自动确定时需要手动选择。使用自定义 LKM 文件时，已识别的目标 KMI 不要求在内置列表中。安装页还提供 SuperKey、签名认证相关选项及与镜像修补有关的高级选项。

本地镜像和直接安装流程可选择将 LKM 嵌入 `boot` 内核的方式；该分支固定使用 `boot`，与普通 ramdisk LKM 流程不同。远程下载入口不走这一分支。可用入口不代表目标镜像一定可修补，仍需以实际镜像检查和执行日志为准。

安装、刷写和分区操作的前提、目标选择与恢复方法见 [安装指南](installation.md)。仅安装管理器 APK 不会使未安装 ZySU 内核的设备获得 Root。

## 更新行为

更新检查默认开启，从 [`wuluoy-creator/ZySU`](https://github.com/wuluoy-creator/ZySU/releases) 的最新正式发行版读取版本信息。解析器优先使用同一发行版中的 `update.json`，并兼容指定格式的旧 APK 文件名；只接受属于该发行版的 GitHub 资产 URL，忽略 draft 和 prerelease。

当远端版本代码高于当前管理器时，首页显示提示。点击后显示可用的更新说明，并由系统打开 APK 下载地址。当前管理器没有 CI 构建更新通道、工作流产物下载或 CI 更新设置。元数据校验与打开下载链接也不等同于管理器已下载、校验并安装 APK。

## 开发验证

在配置好项目所需 JDK、Android SDK/NDK 和依赖后，从 `manager` 目录执行：

```sh
./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

Windows 使用 `./gradlew.bat`。Gradle 检查覆盖编译、现有单元测试和静态检查；生成 APK 中是否包含可用 ksud 与内核模块，取决于构建前准备的资源，见 [构建指南](workflow-build.md)。

界面验收还需在设备上覆盖浅色/深色、窄屏/横屏、大字体、RTL、系统栏和键盘、手势返回与连续滚动。运行行为至少覆盖未安装、未认证、UAPI 不匹配、APK 缺少 ksud、校验不一致、同步失败、SUMH 查询失败及保存后应用失败。Compose 预览和单元测试不能代替这些设备操作。

主要实现：[`BottomBarDestination.kt`](../../../manager/app/src/main/java/com/zying/zysu/ui/screen/BottomBarDestination.kt)、[`WorkspaceScreens.kt`](../../../manager/app/src/main/java/com/zying/zysu/ui/screen/WorkspaceScreens.kt)、[`HomeViewModel.kt`](../../../manager/app/src/main/java/com/zying/zysu/ui/viewmodel/HomeViewModel.kt)、[`SUMHWorkspace.kt`](../../../manager/app/src/main/java/com/zying/zysu/ui/sumh/SUMHWorkspace.kt)、[`ReleaseUpdateParser.kt`](../../../manager/app/src/main/java/com/zying/zysu/update/ReleaseUpdateParser.kt)。
