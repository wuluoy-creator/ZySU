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
