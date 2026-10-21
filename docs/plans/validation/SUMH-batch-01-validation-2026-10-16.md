# SUMH 首批实施验证记录

日期：2026-10-16（Asia/Shanghai）。范围：R01、R02、T01、F10。关联[实施账本](../SUMH-implementation-progress-2026-10-16.md)。

基线 HEAD 为 `2a5ce441e9c923b8b0e89779f5c9ec95e6a97e46`，验证对象是该基线加共享工作区的未提交改动，不是单独提交的发布产物。原有 `.github/workflows/sumh-tests.yml`、`init_event.cpp`、Magic Mount 指南和 boot mount notify 用例已保留。

## 1. 已执行验证

| 验证 | 结果 | 能证明什么 / 不能证明什么 |
| --- | --- | --- |
| SUMH host runner，Zig 0.14.1 C++17，`-Wall -Wextra -Werror` | 13 个测试程序成功退出；包括新恢复持久化故障、R01 策略、内部自动 Maps 请求及 config apply 隐藏失败用例 | 使用生产源码提取和替身依赖；Windows 的符号链接 fixture 显示 `symlink fixtures unavailable: Invalid argument`，该子场景未执行；不等于 Linux/Android 设备测试。 |
| daemon portable socket fault runner | 成功退出 | 覆盖传输故障替身；本次未运行真实 POSIX socket/生产 daemon 重启。 |
| ARM64 Android Release ksud CMake 构建 | 编译并链接成功，产物 `out/sumh-maps-native/ksud` | 编译了 `command.cpp`、`daemon.cpp`、`recovery.cpp`、`sumh.cpp`、`embedded.cpp`、`backend.cpp` 等真实源文件。第三方 mbedTLS 有字符串初始化告警，未将告警写成新增代码错误，也未做设备执行。 |
| CI 门禁 Python 测试 | 新增 7 项全部通过 | 覆盖成功、失败、取消、跳过、缺任务/证据、异 SHA；不等于远端 workflow 运行。 |
| CI Python discovery（T01 子任务执行） | 57 项中 3 项已有 POSIX 用例跳过，其余通过 | 保留跳过边界；`unittest discover` 不会执行仅在 `main()` 中启动的 native runner，工作流另有显式 23 项清单。 |
| 工作流静态校验（T01 子任务执行） | actionlint 1.7.12 检查 build/release/sumh-tests 三份工作流通过 | YAML 和 Actions 静态检查，未触发实际发布。 |
| F10 Gradle 验证（F10 子任务执行，父任务核对 XML） | 7 项 Kotlin + 4 项 Compose/Robolectric，0 failure/error；生产 Kotlin/资源编译成功 | 测试编辑器草稿、失败、刷新、保存回调、UTF-8、状态保存恢复及中文横屏大字体；未实际进行 Activity recreate、设备旋转/进程终止或 TalkBack。 |
| 工作区差异检查 | `git diff --check` 通过 | 换行格式提醒不是 whitespace 错误；不代替功能测试。 |

## 2. 本地复现入口

在仓库根目录运行；下列命令只做构建或宿主验证，不向设备写入规则。

```powershell
$sumhPython = 'C:\Users\29941\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
$sumhZig = 'C:\Users\29941\Desktop\ZySU\out\sumh-validation\tools\zig-x86_64-windows-0.14.1\zig.exe'
& $sumhPython userspace/ksud/tests/run_sumh_host_tests.py --zig $sumhZig
& $sumhPython userspace/ksud/tests/run_sumhp_daemon_host_tests.py --zig $sumhZig --portable-only
& $sumhPython -m unittest discover -s scripts/ci -p test_sumh_ci.py -v
& 'C:\Users\29941\.codex\tools\android-sdk\cmake\3.22.1\bin\cmake.exe' --build out/sumh-maps-native --parallel 4
```

原生构建复用了工作区已有 CMake 配置，目标为 Android `arm64-v8a` Release；该路径依赖已有 NDK 与第三方源码缓存。全新构建应按[构建指南](../../zh/guide/workflow-build.md)准备环境，不能仅复制 build 命令便认为配置可复现。

F10 命令在 `manager` 目录运行，JDK 21.0.11，SDK 编译平台 API 37.0、Build Tools 36.1.0；Gradle Wrapper 9.8.0、AGP 9.4.1，Robolectric 模拟 SDK 35：

```powershell
$env:JAVA_HOME = 'C:\Program Files\Java\jdk-21.0.11'
$env:ANDROID_HOME = 'C:/Users/29941/.codex/tools/android-sdk'
./gradlew.bat :app:testDebugUnitTest `
  --tests com.zying.zysu.ui.sumh.util.SUMHKernelBuildTest `
  --tests com.zying.zysu.ui.sumh.SUMHKernelBuildEditorTest `
  --console=plain --no-daemon
```

报告路径（本地构建产物，未加入版本控制）：

- `manager/app/build/test-results/testDebugUnitTest/TEST-com.zying.zysu.ui.sumh.SUMHKernelBuildEditorTest.xml`：4 项，UTC 时间 `2026-10-15T18:26:12.472Z`。
- `manager/app/build/test-results/testDebugUnitTest/TEST-com.zying.zysu.ui.sumh.util.SUMHKernelBuildTest.xml`：7 项，UTC 时间 `2026-10-15T18:26:24.834Z`。

该目录其他旧 XML 不用于本次通过数量。编辑器状态恢复使用 `StateRestorationTester`；保存失败用例模拟“回调后配置未获确认”，未覆盖整个保存后端异常链。外部已保存配置改变会更新对应字段草稿，并发冲突保护仍归 U02。

## 3. 尚未执行

- Android 真机安全模式首次查询、daemon 终止重启、boot-completed、外部 MetaModule owner 组合。
- 真机 ENOSPC/EIO/EROFS、实际目录同步故障、掉电和连续未确认启动；mock 故障不等于真实文件系统证明。
- Linux 全部 23 个 native runner、完整 Manager SUMH 测试包及真实 GitHub Actions/tag 失败阻断。
- 四 KMI 内核构建、内核功能设备测试、性能与功耗基线。
- 完整卡片生命周期、TalkBack、设备旋转/进程重建和全部支持语言。

未执行项不记为通过。首批没有启动设备变更、发布或推送；下一批验证记录应按新增改动独立追加，避免将原静态检查与后续测试混为一份历史。
