# SUMH 第二十批实施与验证记录

日期：2026-10-16。对应[54 项实施账本](SUMH-implementation-progress-2026-10-16.md)和[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)。本批推进 K08 活动目录包装器的长路径和准备错误传播，代码、主机验证及文档已闭合。整体目标、K08 和 K02 完整验收均未闭合。

## 实现合同

活动目录包装器按需申请 `PATH_MAX` 大小的堆缓冲，预算为 4096 字节（含结尾 NUL），保留路径原始字节。只在需要 VIEW 注入且存在规则时分配；仅隐藏过滤不申请路径缓冲，无关目录保持直接通过。分配在普通 RCU 临界区之外进行，不扩大内核栈。

`sumh_iterate_prepare_wrapper` 区分三种结果：有效包装器、确实无需包装的 NULL、准备失败的 ERR_PTR。分配失败返回 ENOMEM；首选 `d_absolute_path` 的错误原样上报，仅在该函数不可用时使用保留挂载上下文的 `d_path`。不再因绝对路径过长而改用文件系统根相对的短路径。非法返回指针或非绝对路径返回 EIO，缓冲内没有 NUL 返回 ENAMETOOLONG。

唯一活动调用方检查准备错误后退出并释放 SRCU，不调用原始或专用目录枚举，也不向用户 actor 发出条目。失败保留进入调用时的位置，包括通用注入 cookie 和失去专用 client 的旧 cookie。成功结束才发布位置；独立释放函数处理路径、目标 dentry 引用及包装器，错误清理不发布位置。

源码：[容量定义](../../kernel/sumh/internal/sumh_base.h)、[包装器结构](../../kernel/sumh/internal/sumh_types.h)、[准备及清理](../../kernel/sumh/hooks/sumh_vfs_hooks.c)、[调用合同](../../kernel/sumh/hooks/sumh_vfs_hooks.h)、[活动调用方](../../kernel/sumh/features/sumh_fop_override.c)。实现锁定合同与研究记录保存在 `out/batch20-contract.md`、`out/batch20-paths-research.md`、`out/batch20-tests-research.md`。

## 验证记录

| 检查 | 实际结果与证据 |
| --- | --- |
| VFS 辅助回归 | `scripts/ci/test_sumh_vfs_helpers.py` **4 组通过**。目录夹具读取生产结构及容量，覆盖 255/256/510/511/512/513/4094/4095 字节、4096 拒绝、UTF-8/原始高位字节、返回指针和 NUL 边界、嵌套包装、分配失败及四目标引用清理。`out/batch20-vfs-final.log`。 |
| 活动调用链 | `scripts/ci/test_sumh_directory_stream.py` **1 组通过**。84 种错误组合核对错误码、不调用 native/client/actor、原游标和 SRCU 释放；另外验证有/无 client 的仅隐藏过滤 ENOMEM，及真实准备/发出/过滤/结束链路的路径边界、续读和组合行为。最终 `out/batch20-stream-verified.log`。 |
| 名字与虚拟目录回归 | `scripts/ci/test_sumh_directory_names.py` **6 组通过**，既有 K02 虚拟目录失败断言保持。`out/batch20-directory-names.log`。这组既有 merge actor 测试仍描述旧吞错行为，不能作为其已修复的证据。 |
| ARM 提取代码编译 | `out/batch20-arm-compile.py` 对 helper 和活动调用方夹具分别编译 ARM32/ARM64，**4 组对象编译通过**。先恢复生产 `unsigned long` 表达式，并断言完整 prepare 函数原文存在；使用 `-Werror`。`out/batch20-arm-compile.log`。这是带内核 API 替身的提取代码，未链接真实内核。 |
| 交付检查 | 54 个唯一任务 ID 与账本一致、本地文档链接及 `git diff --check` 检查记录为 `out/batch20-final-{docs,diff}.log`；`out/batch20-final-sources.json` 保留五个实现文件与两个测试文件的源码指纹，不是内核产物。 |

共 **11 组宿主执行、4 组 ARM 提取代码对象编译**，重复运行同一组不另计。Python 为 `C:/Users/29941/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe`；前三个脚本均使用 `--cc C:/Users/29941/.codex/tools/android-sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe`。详细命令、初始通过日志及补充验证见 `out/batch20-tests-status.md`。本批没有 Manager、用户态或 CI runner 接线改动，不重复运行相应构建或无关套件。

## 独立复核和证据限制

独立实现/测试源码复核未发现本批范围内的可操作缺陷，见 `out/batch20-independent-review.md`。根代理另补退休专用 cookie 的恢复、仅隐藏过滤 ENOMEM、全部四个保留引用退出和 ARM 指针模型检查。较早的独立测试说明仅写单引用模型；最终夹具已扩展到生产四槽位，但并非五个以上来源验证。

测试提取真实 prepare/free/finish、活动 fop 调用方和相关发出/过滤函数；分配器、路径解析、锁及 VFS 行为由替身提供。Windows LLP64 宿主执行对指针宽度使用类型适配，ARM 对象编译恢复实际表达式。guard 字节与计数验证模型中的释放和越界，不等于真实内核内存压力或并发证明。

活动 stream 夹具中的 `sumh_populate_injected_list` 仍提供确定名称；它验证本批包装链路，未执行真实通用收集器。路径用例是合法长度组件组成的字符串，未实际创建深层目录或执行 symlink/openat。四目标 CLEAR 是明确时序的引用模型，不是同时运行的 RCU 测试。

## 兼容边界与剩余工作

旧指针 ioctl 的 `strndup_user(..., PAGE_SIZE)` 不变，16 KiB 页内核历史可接收范围不在此批暗中缩窄。4095 字节是本批活动目录路径的内容上限，不保证所有文件系统、挂载别名或单个文件名均可解析；单组件仍受 NAME_MAX 等限制。模块及受管文本路径的既有校验、Maps 显示字段 255 字节、256 条和 56 KiB 传输预算不变。

MERGE 去重仍只持有前四个目标，注入收集则另行扫描规则。五个以上来源的动态条目去重、别名匹配、目标替换及 CLEAR 期间统一快照仍需后续实现。通用 `sumh_populate_injected_list` 及 merge actor 仍会吞掉部分收集错误；本批只修复包装准备阶段，不能推导所有目录枚举都已具备完整错误传播。第二批 K02 的已验证范围是虚拟目录 lookup/收集及 vnode 消费，本批据此收窄当前账本描述，保留历史测试记录。

下一步应让注入与去重共享一次调用的完整目标快照，完整收集后再发布条目，并对每处资源、解析和迭代错误返回明确失败。需要至少六来源、第五/六来源新增子项与重复名、同 inode/别名、CLEAR 引用退出及小缓冲续读用例；不能仅扩大固定数组。跨多次 getdents 的 generation/cookie 稳定性仍属 K03。

真实内核构建、四 KMI、4/16 KiB 页设备、普通应用/isolated 观察者、真实 openat/符号链接、lockdep/KASAN/并发故障和性能验收未执行。未提交、推送、部署或修改真实设备。
