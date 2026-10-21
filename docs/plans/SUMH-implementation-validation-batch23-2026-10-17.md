# SUMH 第二十三批实施与验证记录

日期：2026-10-17。对应[54 项账本](SUMH-implementation-progress-2026-10-16.md)。本批推进 F13/T03 的独立观察者工具链，整体目标继续进行；完整设备矩阵、独立测试规则 owner 和布置/回收尚未完成。

## 实现范围

- [独立 Android 测试项目](../../tests/sumh-observer/android/app)：Java 平台界面、普通进程采样、非导出 isolated service、只读 FD 委托、SAF 精确请求/样本交换、单任务与 session token、超时及进程重建恢复。debug/testOnly、ARM64/API 31+，不依赖生产 Manager。
- [原生采集器](../../tests/sumh-observer/native/collector.cpp)：与 JNI/helper 共用实现，固定合成范围、标记/类型/内容验证、路径与委托 FD 分开、原始字节十六进制、64 位数字字符串、stat/fstat、只读私有 mmap、进程自身 proc/namespace/boot、协作式预算及 RAII 清理。helper 检查真实和有效 root UID；不调用 ksud 或 shell。
- [离线比较器](../../tests/sumh-observer/host/compare.py)：有界严格 JSON、精确请求摘要、build/fixture/角色绑定、稳定 namespace 与可选直接 boot 绑定、显式逐探针期望及基线验证、JSON/Markdown 报告与稳定退出码。
- [协议](../../tests/sumh-observer/protocol/schema-v1.md)、[构建说明](../../tests/sumh-observer/README.md)及[场景指南](../zh/guide/sumh-observer-kit.md)：清楚区分当前工具、宿主合成数据和未来设备/规则工作。

样本不是原子快照。配置 revision 为 `supplied_unverified`；直接读取失败不得由 root 替填。采集完成不等于期望匹配，已打开的委托 FD 不等于 fresh path 成功。缺失结论要求成功基线及 ENOENT；EACCES、EPERM、截断和未执行不算隐藏成功。

## 本地验证

| 检查 | 实际结果与证据 |
| --- | --- |
| 原生协议和有界 I/O | 实际 `protocol.cpp` 宿主可执行程序 **97 个断言通过**：重复键/未知键、UTF-8/容量、canonical user/路径/context、精确整数版本 token、SHA-256 已知向量、分段读取/EOF/EINTR/截断、Maps 地址与挂载祖先筛选。`out/batch23-native-tests-final.log`。 |
| 完整原生收集器模型 | 实际 `collector.cpp` 在有界 syscall 替身下 **13,291 个断言通过**；覆盖 72 个普通、80 个委托、29 个创建故障位置及逐次资源检查。另覆盖标记/类型/硬链接/符号链接、uint64 inode、负 size 拒绝、map 内容/只读 FD、原 FD/偏移保留、独占创建和超时。与协议组共 2 个宿主可执行组；不是 13,291 个独立设备用例。 |
| 离线证据 | **34 项测试通过**，含 2981 个确定的嵌套字段/类型替换、精度/错误/缺角色/基线/namespace/boot/重复 invocation/顺序、输入别名保护、期望生成及真实子进程退出码。`out/batch23-evidence-fifth.log`。 |
| CI 入口 | 离线比较器 runner 注册到 `sumh_ci.NATIVE_CHECKS`，调度与同提交门禁 **7 项自测通过**；`out/batch23-evidence-ci.log` 中 missing.py 是预期负向用例。未执行远端 Actions；新的原生模型套件另用显式 runner，不冒充已接入全量 CI。 |
| 跨语言集成 | 实际原生收集器模型输出的 ordinary/isolated/root 样本，进入真实期望生成器与比较 CLI；**11 项声明检查匹配**，严格 direct_boot/namespace/请求/类型校验无问题。`out/batch23-native-model-comparison.json` 和 `.md`。所有输入在 `out/batch23-native-model-evidence/` 标明宿主模型来源。 |
| Android 生命周期 | **34 项测试通过，无失败/错误/跳过**：Coordinator 11、Protocol 17、Wire 6；含实际 Activity recreate、超时/晚回调/旧样本、进程恢复、服务断开、Binder 容量、过期导出、排队与活动任务的 FD 所有权。`out/batch23-android-fourth.log` 和 app `build/test-results/testDebugUnitTest/`。 |
| Android 构建和 lint | assembleDebug、testDebugUnitTest、lintDebug 通过；**0 错误、1 条 ChromeOsAbiSupport 提示**，与本批仅 ARM64 的支持范围一致，未隐藏该提示。原生最终冻结后另行 assembleDebug 更新 APK，Java/资源未变，不重复测试。 |
| 原生目标构建 | 真实 NDK ARM64/API 31 helper 和 JNI 编译链接通过；CMake 3.22.1、NDK 30.0.16248370 Clang 21、c++_static、固定 mbedTLS 3.6.2。`out/batch23-native-build-final.log`。 |
| 产物与文档 | 最终 APK/helper/JNI 均包含同一 collector source ID，另存 SHA-256；检查最终 APK 的 testOnly/debuggable、min31/target37、非导出 isolated service、无权限/shared UID，ELF64/AArch64、PIE/RELRO/非执行栈和 JNI 导出。54 项与账本/本地链接/diff 校验见 `out/batch23-final-docs-artifacts.log`，完整指纹为 `out/batch23-final-manifest.json`。 |

工具及命令：宿主 Python 使用捆绑 3.12，原生宿主编译使用 Zig 0.14.1 的 C/C++ 模式；Android 使用 JDK 21.0.11、Gradle 9.8.0、AGP 9.4.1、SDK 37、build-tools 36.1.0。SDK/NDK 构建日志与 source ID 分开保存；source ID 不是完整工具链或二进制可复现性证明。首轮构建与失败日志保留，不以最终通过掩盖中间错误。

```sh
python tests/sumh-observer/native/run_native_tests.py --zig out/sumh-validation/tools/zig-x86_64-windows-0.14.1/zig.exe --export-model out/batch23-native-model-evidence
python scripts/ci/test_sumh_observer_evidence.py
python -m unittest discover -s scripts/ci -p test_sumh_ci.py -v
```

原生 runner 需要已由固定 CMake 下载的 mbedTLS 3.6.2；也支持显式 `--mbedtls-source`。必须同时存在 codec 和 collector 模型源文件，缺失不能静默跳过。Android 命令为 `manager/gradlew.bat -p tests/sumh-observer/android --no-daemon --console=plain :app:assembleDebug :app:testDebugUnitTest :app:lintDebug`，最终 assemble 日志为 `out/batch23-android-final-assemble.log`。构建环境按[README](../../tests/sumh-observer/README.md)配置。

最终 collector source ID 为 `sha256-v1:4fee291cb21fa48d87bbea999d418afd8d4156a2c11bc907bdc73982394ac51b`。APK 位于 `tests/sumh-observer/android/app/build/outputs/apk/debug/app-debug.apk`，SHA-256 为 `cb92b112147fd4cf6b77580f9758ca7046366b47e8b3fd27a5f6db8936f02cea`；helper 位于 `out/batch23-native-android/sumh-observer`，SHA-256 为 `0a0df276de597c7e6383e248ec7a671ef64386e6a2ca18912dbdafd2399440c3`。APK v2 单一 debug 签名验证通过；APK 内 stripped JNI 与直接 Debug JNI 的二进制哈希不同，源码 ID 相同。最终检查保留 54 项、40 份文档的 495 个本地链接及 55 份源码指纹。

## 实现复核与修正

原生和 Android 最初对 `1.0`/`1e0` 及 user ID 位数接受范围不同，已统一为字面整数 `1` 和最多十位 canonical user；原生只在完整严格 schema 校验之后作数字 token 检查，未修改共享生产 JSON 解析器。JNI 保留标准 UTF-8 字节，导出不添加换行。

委托文件打开增加非阻塞，避免 FIFO 在 fstat 之前阻塞；Android 公共 API 无 O_DIRECTORY，使用 no-follow/非阻塞打开后 fstat 验证目录，再继续固定 FD 锚点。超时和中断按 actor/request 写入最近错误，旧有效样本保留；未开始的 isolated 任务取消时关闭 FD，已经运行的任务由 finally 在原生借用结束后关闭。新请求证据损坏时先清空旧 actor 内存，防止旧样本被误关联到新摘要。

新增测试发现 `AtomicFile.finishWrite` 的 rename 失败只写日志，宿主恢复仍读到旧错误。保存改为同目录 staged 文件、FD.sync 和公开 NIO 原子替换，发布失败向调用方传播，不提供非原子回退；这不宣称父目录落盘或真实掉电恢复已验收。另一处失败是 Robolectric Windows pipe 的合成 FD 不保证非负，测试改为核对原 handle 保留及最后关闭。Python 类型替换测试曾因预期样本数高于实际 2981 失败，校正计数阈值，产品断言未弱化。

实现设计与审查分别留存 `out/batch23-contract.md`、`out/batch23-root-integration-notes.md`、`out/batch23-cross-{android,native,protocol}-review.md`。逐观察者超时归属、排队 FD 清理和解析合同已修正并复审关闭，本批范围没有剩余的阻塞发现。普通/isolated/root 身份来自各自采样；role 字符串不用于认证 SUMH 授权。

## 证据边界

宿主原生替身和 Python 合成数据不是 Android 实测。Robolectric 使用宿主 Android 模型，不证明真实 Binder/isolated UID/SELinux/存储提供者行为。NDK/Gradle 构建不证明真实 KMI/CFI 或设备启动。

没有安装/运行设备、改变 SUMH 规则/Maps/模块/授权、执行 global clear、提交或推送。本工具只创建自己的固定合成文件，保留证据，清理通过明确清除专用测试应用数据；现有内核所有权不能直接冒充独立测试所有权。

后续仍需可恢复设备上的普通/授权/root/isolated/共享 UID/工作资料、多用户、重启与故障注入、写入入口/旧 FD/原子替换、性能及完整支持矩阵。F13/T03 状态保持部分实施。
