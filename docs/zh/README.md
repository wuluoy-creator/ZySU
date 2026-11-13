# ZySU 中文文档

[项目首页](../../README.md) · [English](../README.md) · [日本語](../ja/README.md) · [Türkçe](../tr/README.md) · [Русский](../ru/README.md)

ZySU 是基于 KernelSU、从 SukiSU-Ultra 分叉的 Android root 方案，包含 ARM64 内核模块、C++17 userspace 和 Android Manager。本文档描述仓库当前实现；具体发布版的组件版本以下载附件和运行时状态为准。

## 安装与构建

| 文档 | 内容 |
| --- | --- |
| [安装指南](guide/installation.md) | 设备要求、镜像修补、直接安装、升级和恢复 |
| [GitHub Actions 与本地构建](guide/workflow-build.md) | 组件依赖、环境、签名和产物 |
| [LKM 集成](guide/how-to-integrate.md) | 内核配置、DDK、自定义模块与管理器认证 |
| [发布与版本管理](releases.md) | 本仓库发布、版本覆盖、更新元数据与迁移 |

现成构建面向 ARM64 Android GKI，内核最低为 Linux 6.1；Manager / userspace 最低为 Android 12（API 31）。默认 KMI 为 `android14-6.1`、`android15-6.6`、`android16-6.12`、`android17-6.18`，仅支持 `CONFIG_KSU=m`。这些是构建范围，不保证每个厂商设备都能启动。

## 组件与接口

| 文档 | 内容 |
| --- | --- |
| [模块 preinit](guide/module-preinit.md) | SELinux 加载前动作、metadata 同步与启动失败恢复 |
| [ksud 命令行](../ksud-cli.md) | 命令树、参数、权限、模块与启动镜像操作 |
| [Ramdisk 编辑协议](../ramdisk-editor-protocol.md) | Manager 与 ksud 的二进制协议、请求和错误处理 |
| [Userspace 架构与验证](../userspace-optimization.md) | 原生组件、构建约束、主机回归入口 |
| [Manager 界面结构](guide/manager-ui-redesign.md) | 当前导航、设置、安装入口和状态呈现 |

SUMHP 已内置于 ksud。自动挂载按适用条件选择 OverlayFS 或 Magic Mount；SUMH 后端需要显式选择。外部 MetaModule 接口仍保留，后端选择、实际生效状态和恢复条件应结合诊断结果判断。

## 验证与诊断

| 文档 | 内容 |
| --- | --- |
| [SUMH 行为验证](guide/sumh-validation.md) | 内核能力、运行状态、规则与真机检查 |
| [Maps 路径规则](guide/sumh-maps-rules.md) | 路径配置、持久化、原子应用和启动恢复 |
| [Kstat 路径规则](guide/sumh-kstat-rules.md) | 按路径指定元数据来源、应用和恢复 |
| [模块路径规则](guide/sumh-module-rules.md) | 模块隐藏、模式范围、原子保存与规则状态 |
| [内核构建信息替换](guide/sumh-kernel-build.md) | Release / Version 字符串、配置、生效时机与验证 |
| [Unicode 路径防护](guide/unicode-path-guard.md) | CVE-2025-48567 原理、SUMH 防护开关、边界与验证 |
| [Magic Mount 验证](guide/magic-mount-validation.md) | 挂载树、生命周期、回归和设备检查 |
| [OverlayFS 验证](guide/overlayfs-validation.md) | 目录分层、能力限制和回归入口 |
| [环境状态与可观测性](guide/environment-observability.md) | 设置值、有效值、运行时证据与错误定位 |
| [Manager 特权隔离验证](guide/manager-privilege-validation.md) | 辅助进程、空闲 shell 回收与普通/隔离应用观测 |
| [启动配置与 Shell 来源限制](guide/boot-config.md) | 强制功能值、模块屏蔽默认关闭、配置兼容和回归验证 |
| [Manager 识别与扫描验证](guide/manager-scan-validation.md) | 内核侧管理器发现、APK 签名缓存与扫描调度 |
| [KernelSU 21 项提交适配记录](guide/upstream-20261109.md) | 逐项处理、本地兼容、回归结果与验证边界 |

验证文档提供可复现步骤，不表示这些步骤已在读者设备上执行。报告问题时附上固件与内核信息、组件版本、所选后端、完整操作步骤和相关日志。

## 下载与反馈

- [发布版下载](https://github.com/wuluoy-creator/ZySU/releases)：Manager 正式更新也读取此仓库。
- [源码与工作流](https://github.com/wuluoy-creator/ZySU)：Actions 的最终 APK 产物为 `Manager-arm64-v8a`。
- [问题反馈](https://github.com/wuluoy-creator/ZySU/issues)与[安全问题说明](../../SECURITY.md)。

源码、工作流和正式发布统一在公开的 `wuluoy-creator/ZySU` 仓库。旧 APK 若仍检查 `ZySU-Releases`，需要手动安装一次包含新更新地址的 APK，详见[迁移说明](releases.md#从独立发版仓库迁移)。

许可证、项目来源和完整目录介绍见[根目录 README](../../README.md)。
