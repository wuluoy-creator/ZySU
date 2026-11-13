# Manager 特权隔离与普通应用观测验证

本验证分别回答两个问题：Manager 的应用进程是否直接持有提权凭据，以及没有授权的普通应用实际能读到什么。普通应用读取被拒绝只表示当前权限下不能读取，不表示目标进程不存在，也不能据此声称不可检测。

## 实现与主机回归

Manager 的完整性校验、安装、工具链接修复和原 SUMHP JNI 特权操作通过 APK 内附的 ksud `manager-helper` 子进程执行。JNI 本身不再申请 root。辅助进程在提权前核对内核认可且已经认证的 Manager 身份及 UAPI，单次处理一个有界请求后退出；通信通过继承的私有 socketpair，没有监听端口或任意 shell 命令接口。首次安装不依赖 `/data/adb/ksud` 已存在。

Manager 与内附 ksud 必须一起重新构建、打包。辅助进程通信协议不兼容或鉴权失败时返回错误，不回退到应用内线程提权。原有第三方 su 和模块接口不变。请求发送后的超时不表示操作未执行；写操作不自动重放，应先查询实际状态再决定是否重试。

辅助进程拥有独立进程组与 60 秒 watchdog，由特权侧终止超时任务及尚未独立守护化的子进程。Manager 失败路径不阻塞等待已提权子进程。普通和全局挂载 shell 都按需创建，最后一个任务释放后闲置 30 秒回收；认证刷新会淘汰缓存，但等待在途任务完成后才关闭旧实例。WebUI 自有会话和 RootService 按原有生命周期管理。

Linux 主机回归：

```sh
python3 userspace/ksud/tests/run_manager_helper_host_tests.py --cxx clang++ --sanitize
cd manager
./gradlew :app:testDebugUnitTest --tests com.zying.zysu.ui.util.IdleResourcePoolTest
```

辅助进程测试使用生产协议与分发代码，替换内核授权和安装行为，覆盖拒绝调用、UAPI 不匹配、未知操作、截断/超大请求和断开的通信端。它不会实际提权、安装 ksud 或启动守护程序。

## 独立观测 APK

`manager/visibility-probe` 是单独的 Android 应用，包名为 `dev.zysu.visibilityprobe`，没有权限声明、共享 UID、网络连接、su 调用或 Manager 依赖。它在普通进程和 `isolatedProcess` 服务中分别采样，并通过 Android 文件选择器导出 JSON。它不能获取自己的 Root 授权状态；请在 ZySU 中确认该包未被授权，也未使用自定义提权 profile。

GitHub Actions 的完整 Manager 构建会同时运行应用单元测试和观测应用 lint，并提供 **VisibilityProbe-release** 产物，内含可安装的 `visibility-probe-release.apk` 和 `SHA256SUMS`。该 release APK 不可调试，使用每次运行单独生成的测试证书，完全独立于 Manager 签名。跨工作流更新观测 APK 时可能需要先卸载旧版；同一轮前后对比应保留同一个 APK。

构建独立的 release APK：

```powershell
cd manager
.\gradlew.bat :visibility-probe:assembleRelease
```

构建环境与 Manager 相同：JDK 21、Android SDK 37、Build Tools 36.1.0。产物为 `visibility-probe/build/outputs/apk/release/visibility-probe-release-unsigned.apk`。使用专门的测试密钥，通过 Android SDK 的 `zipalign` 和 `apksigner` 对该 APK 签名；不要使用 Manager 的发布密钥。这样可避免共享签名权限或 Manager 身份对观测造成影响。安装签名后的 APK，应用名为 **ZySU Visibility Probe**。

也可以执行 `:visibility-probe:assembleDebug` 做初步调试；debug APK 的报告会标记 `debuggable: true`，可能使用开发机默认 debug 签名。最终对比应使用同一个、独立签名且不可调试的 release APK。

## 采样与结果

1. 在 Scenario 中标注源码提交、Manager/ksud/内核版本、KMI、模块状态和本轮场景。设备型号、固件指纹、Android 版本、安全补丁、内核 release、观测 APK 版本及签名 SHA-256 会自动记录。
2. 可填写最多 8 个已知 PID，优先尝试读取这些目标；留空则按实际可枚举内容取样。已知 PID 是额外输入，不表示普通应用自己能够发现该 PID。
3. 点击 **Capture**，等待普通进程和隔离进程各完成一次采样，再点击 **Export JSON**。两次采样是先后进行的，不是原子快照。
4. 每个观察者记录实际 `uid/euid/gid/egid`、PID/TID、`/proc/self/status`、`/proc/thread-self/status` 及当前线程 SELinux 标签。跨进程只读取 `status` 的凭据相关字段和 `attr/current`，不读取命令行、进程内存或应用数据。

单个快照最多选择 128 个 PID，每次文件读取最多 8 KiB、JSON 快照预算 128 KiB，枚举和跨进程读取共享 2 秒采样预算。上限、截断标记和实际耗时写入报告。隔离服务连接最多等待 15 秒，失败会留下明确状态。采样结束立即解绑隔离服务。

结果含义：

| outcome | 含义 |
| --- | --- |
| `readable` | 此次调用能读取；查看值及截断标记 |
| `permission_denied` | DAC、SELinux 或其他访问检查拒绝，保留 errno（可取得时） |
| `missing` | 路径不存在或进程已退出；也可能是该视图没有此目标，不能单独判定原因 |
| `error` | 其他错误，查看 errno 和异常类别 |
| `timeout` / `bind_failed` 等 | 隔离服务未完成；不能按“无可见特征”处理 |

`/proc/self/status` 描述进程主线程；`/proc/thread-self/status` 描述实际采样线程。二者不同应保留原始结果。不要只以 `Uid: 0` 或非空 capabilities 认定安装了 root 方案，原厂系统服务也可能持有这些权限。

## 对比矩阵

同一设备、同一固件、同一观测 APK，分别测试旧版与修改后的 Manager：

| 场景 | 关注结果 |
| --- | --- |
| 重启后，Manager 未打开 | 系统与模块的基线，普通/隔离身份是否正确 |
| 打开 Manager 后保持空闲 | 是否产生无任务的长期 root shell；真实可读项是否变化 |
| 校验、安装或修复 ksud | 首次安装、匹配、不匹配和权限拒绝是否正确返回；Manager 线程凭据是否改变 |
| 查询 SUMHP、保存配置、读取日志 | 操作结果与旧版一致；辅助进程退出、失败及错误传递正确 |
| 模块安装、长脚本、WebUI 操作正在执行 | 空闲回收不能中断任务或长期服务绑定 |
| 操作结束、Manager 转到后台 | 无任务的辅助进程及 shell 是否按设计退出 |
| 模块关闭/开启、按应用卸载策略不同 | 分别记录，避免把挂载策略差异归因于进程隔离 |

若普通 APK 无权枚举 Manager 线程，用独立的已授权诊断环境检查 `/proc/<manager-pid>/task/<tid>/status` 与对应 `attr/current`，覆盖主线程及所有工作线程。将该检查单独标为“特权验证”，不能替代普通 APK 的可见性结果。应连续观察任务前、任务中、任务后，单次任务完成后的快照可能漏掉短暂提权。

回归还应覆盖第三方 `su -c`、交互 shell、退出码和标准输出/错误、App Profile、模块启动脚本、RootService 绑定，以及拒绝授权或 SuperKey 尚未认证的冷启动。硬件密钥证明、设备完整性和第三方检测结果需要另外验证；本 APK 不将任何一项观测转换成“已 root/未 root”的结论。
