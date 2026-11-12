# 管理器界面改进对照

参考版本：[FinaSU 67a56dc](https://github.com/LyraVoid/FinaSU/tree/67a56dc2a0a286fa0dec51825611f735f7e5e0d7)，核对日期：2026-11-13。

本轮将参考仓库的界面组织方式应用到 ZySU 已有的管理器功能，保留 ZySU 的品牌、Root 授权与 SUMH 操作语义。主题商店、主题下载和市场接口不在范围内。

| 范围 | FinaSU 参考实现 | ZySU 的实现与补充 |
| --- | --- | --- |
| 首页层级 | `ui/screen/home/HomeStatusCard.kt`、`HomeFactsCards.kt` | 安装状态、守护进程、SUMH 状态各司其职；每个技术字段只分配一个展示位置。版本、内核、构建指纹等默认显示简短摘要，点按打开可选择复制的完整详情。 |
| 系统概览 | `HomeMetricComponents.kt`、`data/SystemMetrics.kt` | 聚焦与统计布局显示电池、温度、内存、存储、CPU 可用读数、运行时长；统计布局绘制实际采集的内存与 CPU 温度趋势。不可用读数保持空缺；隐藏概览、切换到不使用指标的布局或离开前台时停止轮询。 |
| 首页布局 | `HomeLayoutStyle.kt`、`Home*Layout.kt`、`HomeCountCards.kt` | 五种真实结构：经典单列、状态与计数双列、聚焦四面板、宽幅仪表盘、统计监控。选择器提供示意图并即时保存；旧自适应迁移为网格，旧列表迁移为经典；窄屏与大字号自动堆叠。 |
| 导航样式 | `ui/component/bottombar/BottomBarMaterial.kt`、`BottomBarControl.kt` | 保留默认悬浮玻璃导航，新增 Material 通栏导航并配套切换顶部栏。两种样式共用导航状态、离开拦截、徽标、安全区与键盘隐藏逻辑；颜色和文字沿用页面主题。 |
| 应用列表 | `ui/screen/superuser/SuperUserMaterial.kt` | 应用组摘要、全部／已授权／未授权／自定义配置筛选、次级 Android 用户标记；保留共享 UID 提示、批量操作和授权确认。 |
| 模块列表 | `ui/screen/module/ModuleItemCard.kt`、`ModuleSortMenu.kt` | 保留已有模块卡片层级，增加总数／启用／待重启摘要和筛选、名称／ID 排序及方向；明确加载、失败、无匹配和未安装状态；筛选变化回到列表起点。 |
| 设置导航 | `ui/screen/settings/SettingsCategoryContent.kt`、`settings/search/` | 分类保留；搜索可按分类、摘要和功能名找到外观、显示、更新、安全、挂载及诊断入口。多关键词和大小写／重音归一化；结果打开所属分类。 |
| 基础外观 | `ui/screen/colorpalette/`、`ui/screen/font/`、`settings/` | 保留深浅色、动态色、色板和 DPI；增加纯黑背景、系统／衬线／等宽字体、85–130% 应用文字缩放、12–32dp 卡片圆角、预览和重置。系统辅助字号继续生效。 |
| 壁纸 | `wallpaper/`、`ui/screen/wallpaper/` | 保留选择、裁剪、更换和移除；增加 Android 12+ 图片模糊；修正透明度、暗度、模糊的持久化和移除后状态同步。 |
| 显示选项 | `HomeDisplaySettings.kt`、`settings/SettingsCategoryContent.kt` | 按模块信息、首页布局、信息可见性分组；保留精简显示、版本、Seccomp、Zygisk、元模块、链接卡片与标签控制。 |
| 交互与可访问性 | 分组行、状态标签、自适应组件 | 状态不只用颜色表示；筛选换行，较多的页签可滚动；选择语义、48dp 触控区域、标题语义和安全区；卡片使用共享主题圆角。 |

参考仓库的其他内核机制、插件运行时、多媒体播放、在线素材、可导入主题包及任意界面素材替换没有作为本轮依赖引入。系统指标仅从公开 Android API 和可读的设备节点取得，不为界面显示额外开启 Root shell。

## 验证

- 与 GitHub 工作流一致的 Gradle 任务：`:app:testDebugUnitTest lintDebug lintRelease assembleRelease -PABI=arm64-v8a`。
- WebUI 桥接回归：`node --test js/tests/webui.test.mjs`。
- 新增回归覆盖数值边界、外观保存与重置、首页偏好实时更新与监听释放、模块筛选和排序、设置搜索。
- `PresentationLayoutTest` 用 Robolectric 原生图形渲染生产组件，验证 375dp 浅色／深色、2 倍字号、800×375dp 横屏的滚动、筛选选择和预览可达性，并输出 `out/fina-ui/` 下的截图。
- `HomeNavigationLayoutTest` 渲染五种实际首页结构与两种上下导航，覆盖中文手机、2 倍字号深色及横屏；检查字段不重复、原始长值按需查看和计数入口。截图输出到 `out/home-nav-ui/`。偏好测试覆盖布局迁移、选择保存、导航实时切换与监听释放。
- 本机未连接 Android 实机，截图验证不等同于实机上的 Root、传感器和系统手势测试。

新增文案提供简体中文和英文；其他现有语言按 Android 资源机制回退到英文。
