# SUMH 第七批实施与验证记录

日期：2026-10-16（Asia/Shanghai）。范围：C01/C02/F01 的继续实现、S01 原生构建加固及采样等待修正。对应[54 项主计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)和[实施账本](SUMH-implementation-progress-2026-10-16.md)。本文件记录代码、测试和已知限制，不替代原静态检查证据。

## 已实现行为

### Manager 手动读取运行登记集合

运行规则页接入七类结构化分页，先协商 schema，再使用同一 boot/instance/generation/mask 读取各类别并做末次 metadata 核对。最多 1,024 条、4 MiB JSON、30 秒预算；时间及取消在请求之间检查，不能强制中断既有同步 JNI 调用。刷新立即取消旧完整集合的权威性；失败、过期、取消、未知类别或超预算均不声称完整，不自动重新扫描或重放修改。

新增分类、搜索、详情及精确 UTF-8／十六进制复制。路径不 trim、不按换行拆分、不用替换字符恢复；u64 保留十进制字符串。所有权只使用真实字段，序号不提供变更权限；用户隐藏的绑定健康仍独立显示。旧文本视图与原始兼容结果保留并明确不完整。重建页面不自动恢复扫描，采样结果不写入 SavedState。

### 受管 Maps 当前身份核对

实际应用与核对共用 `read_maps_identity`：跟随符号链接、要求普通文件，验证 inode 与内核 12:20 设备编码。`api state` 在 init 挂载命名空间读取最多 256 条完整受管记录，对保存目标／替换路径采样两次，核对 target/spoofed inode、设备号及替换路径。硬链接目标碰撞显示差异；路径缺失、类型错误、身份不可表示、分页失败或采样变化显示未知。不会发布可被误当完整集合的前缀。

停用 Maps 时保留定义，只检查受管集合是否为空，不解析已停用路径。临时数字规则单独查询，不参与受管身份匹配。保存摘要现在包含开关和规则，旧的“仅路径摘要”历史记录不被错误沿用。身份匹配只证明当前登记关系；不固定 inode 生命周期，不验证文件内容、普通应用视图或临时规则优先级的最终效果。

### 原生产物加固与符号

ksud、ksuinit 及其 C/C++ 子目录启用 stack-protector-strong；优化配置启用 FORTIFY 2。链接启用 RELRO/NOW 和 SHA1 build ID；先保存独立 `.unstripped` 符号与源码行表，再裁剪发布 ELF。构建后强制核对真实产物和编译参数，不只检查 CMake 字符串。符号作为独立 CI 附件，不打包进 APK／执行文件附件。缺失符号 byproduct 会触发重建。现有 UPX 开关改为明确拒绝，避免压缩后的装载布局绕过验证。

动态 ksud 验证 PIE、NOW、RELRO、非可执行栈和无 RWX LOAD。ksuinit 保持静态 ET_EXEC；不宣称静态 PIE/ASLR 或动态 NOW。当前 NDK 的静态 libc 初始化反汇编有 RELRO mprotect 路径，这是静态证据，不等于设备运行权限已验证。

## 审查发现及处理

1. 新 Maps 采样原本会在 namespace 子进程阻塞时无限等待，且 `Journal::finish` 会调用它。现已接入独立只读 worker：父进程按单调时钟预算 5 秒，只接受收到成功回复且确认正常退出的共享结果。超时通过子进程交付的 pidfd 终止，P_PIDFD 非阻塞回收，不使用数字 PID 信号／回收回退。未回收的一个子进程会阻止新采样；没有安全句柄时保留槽位到控制器退出。pidfd/P_PIDFD 不支持或被策略拒绝时返回未知，属于查询可用性限制。修改操作的既有 namespace helper 保持原语义；系统调度、fork 及完整 CLI/JNI 期限仍不能宣称为硬实时保证。
2. 原生检查器原先按原始命令正则检查，漏掉分隔式 `-D/-U` 和带引号参数。现改为解析参数 token、优先使用 arguments 数组、按顺序计算有效宏与保护选项，并拒绝未展开 response file。补入实际产物对应的参数回归；当前产物没有发现保护被关闭。

## 已执行验证

| 验证 | 已取得结果与边界 |
| --- | --- |
| SUMH 原生宿主套件 | 最终接线后 17 个程序通过，含新生产 Maps 解析／采样／比较提取测试；Windows worker 为模型，真实 fork/mmap 分支未在本机执行；原符号链接 fixture 仍不可用。日志 `out/batch07-root-native-final.log`，最终 session 12191。 |
| 状态账本 | 生产采样、比较及 ledger 故障测试通过；覆盖 Maps matches/drift/unknown、停用摘要差异、失效历史 applied revision 和外层 generation 变化，与最终宿主日志合并保存。 |
| 只读查询 worker | 39 项生产代码故障模型通过，并经独立复跑；覆盖超时、持续 EINTR、异常退出、句柄缺失、重复／多个描述符、PID 复用、回收失败和单槽预算。实际 `.cpp` 与 POSIX smoke 成功交叉编译为 x86_64-linux-musl，未执行真实 POSIX 分支。日志 `out/batch07-query-worker-model.log`、`out/batch07-query-worker-posix-compile.log`。 |
| Manager | 全包 31 类、210 项，零失败／错误／跳过；新增 16 项 parser/reader、8 项生产 Compose 测试。日志/XML/截图在 `out/sumh-batch07-manager-full/`；定向 session 66702、全包 session 56731。 |
| 布局检查 | 已查看窄屏 360dp、2 倍中文字体及横屏取消截图，滚动后按钮／详情／复制／关闭可达。Robolectric 渲染不是设备、TalkBack 或真实进程死亡验证。 |
| 加固检查 | 最终两个 ARM64 Release 构建通过，802+7 条 C/C++ 编译记录与 ELF／符号通过；修正参数解析后最终产物回归为 ksud 24、ksuinit 23 项通过。ksud 最终增量构建 session 21911；构建日志 `out/batch07-root-{ksud,ksuinit}-final.log`，最后的 ksud 参数回归日志 `out/batch07-root-ksud-checker-final.log`。 |
| CI | 7 项 SUMH 门禁回归通过，ksud／ksuinit／SUMH 工作流 actionlint 通过；查询 worker runner 已加入 29 个原生入口。新增符号附件与产物回归步骤尚未在远端 Actions 运行。 |
| 文档与差异检查 | `out/check_batch03_docs.py` 通过：主计划与账本的 54 个唯一任务 ID 一致，16 份文档的 203 个本地链接有效。普通 `git diff --check` 返回 0，仅有 LF/CRLF 转换提示；日志 `out/batch07-git-diff-check.log`。未更改仓库换行设置或批量重写文件。 |

构建使用 NDK 30.0.16248370 / Clang 21、Android API 31、CMake 3.22.1、Release/LTO。宿主 C++ 使用 Zig 0.14.1；静态检查、模型测试、交叉编译和设备运行分别记账。加固参数与 query worker 的独立复核均已闭合，没有剩余阻断发现；这不是完整安全审计结论。

### 最终产物

| 产物 | 可执行文件字节 | 独立符号字节 | build ID |
| --- | ---: | ---: | --- |
| ksud | 3,193,144 | 16,077,536 | `3c5793812043ba2b71377cb119b121462b8b5ac4` |
| ksuinit | 317,616 | 8,949,576 | `d5c25ebf578e72fd75b1cb43dcde111991fbb791` |

最终 hash/大小清单为 `out/batch07-final-artifacts.json`。ksud SHA256：`8768ce4adf10be9ee6b6b37ff9d4a2e7365d3d6a8249b8aba29a6490fc92d00b`；ksuinit SHA256：`14af1c4191d5d128d98e427a236a66ef963c2beefcf4c508ede8d1582cbd2c69`。同输入的 ksuinit 重链接与缺失符号重建检查通过，符号能定位源码行；不据此声称跨工具链／跨工作区整体构建可复现。

与本批保留的 ksud 基线 3,098,640 字节相比增加 94,504 字节（约 3.05%）。该差异同时包含 Maps 核对、查询 worker 和加固，不能归因于加固的独立成本。没有可比较的 ksuinit 旧产物基线，启动时间／内存／性能尚未测量。

### 复跑入口

```sh
python3 userspace/ksud/tests/run_sumh_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_sumh_state_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_query_worker_host_tests.py --cxx g++
python3 -B -m unittest discover -s scripts/ci -p 'test_sumh_ci.py'
# 对已有 Android 构建目录检查；两个组件工作流已接入同一脚本
python3 scripts/ci/test_native_hardening.py --build-dir userspace/ksud/build \
  --target ksud --linkage dynamic --readelf "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
```

本机原生入口使用上述 runner 的 `--zig` 参数；Linux 的真实 fork/pidfd smoke 若平台拒绝所需能力会明确报告不可运行，不能计为运行通过。生产编译命令及本机具体路径保存在对应日志。未安装或执行这些 Android 产物，也未提交、推送或发布。

## 兼容、撤销与剩余范围

保留旧命令及旧内核兼容读取，未修改内核 ABI。没有新增 Maps apply、全局 clear 或启动恢复入口；旧内核无法核对完整集合时仍为未知。撤销规则仍使用原对应 owner 的路径操作，查询序号不能删除。构建撤销需重建配套可执行文件；符号必须与产物 build ID 匹配。

F05 的逐路径解析预览、上次提交目标路径关联和批量工作台仍未实现。完整 CLI 冷启动 daemon 可能按已有策略恢复，不能把新查询当作严格纯预览。模块 Planner／源树／owner、其他枚举类别、全功能事务、typed 操作结果和全链 deadline 仍待后续。设备启动、四 KMI、4/16 KiB 页设备、真实 FUSE 阻塞、Maps/smaps、应用身份矩阵、性能／内存、远端 Actions 和发布未执行，均未标为通过。整体 54 项目标继续进行。
