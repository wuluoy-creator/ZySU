# SUMH 回归验证指南

SUMH 是内核中的 VFS 视图引擎；`ksud` 内置的 SUMHP 控制器负责配置、模块挂载规划、规则恢复及诊断接口。管理器通过 JNI 和控制器呈现状态及执行配置操作。本文给出当前仓库已有的回归入口和设备验证范围，不作为某个设备或发行版的通过证明。

普通进程、isolated service 与 root 的实际样本可使用[独立观察者工具链](sumh-observer-kit.md)采集，再按声明期望离线比较。该工具已经有源码与本地构建/宿主测试；设备安装、SUMH 规则布置、独立测试 owner 和完整设备矩阵尚未完成。权限拒绝、截断或仅采集完成都不能当作隐藏测试通过。

## 验证对象

第二十七批增加 ADD/MERGE 安装源身份、完整挂载/目录引用和回收验证；[实施记录](../../plans/SUMH-implementation-validation-batch27-2026-10-17.md)分别列出实际源码宿主测试、ARM 对象编译与未执行的完整内核/设备门禁。新入口未开放 owner 事务或 F06 应用命令。

第二十八批补实际捕获/元数据/释放函数测试，以及 MERGE 完整收集、锁外安装、相对持有父目录捕获、递归共享预算和失败清理测试；[实施记录](../../plans/SUMH-implementation-validation-batch28-2026-10-17.md)单独记录测试工具退出码修正、反例与当前证据，不改写旧批次结果。

第三十一批针对链接跟随后 nofollow 检查，补持续替换、mount 身份变化、查询失败、引用清理及明确的 ABA/检查后变化反例。实际运行与静态设计分列于[实施记录](../../plans/SUMH-implementation-validation-batch31-2026-10-17.md)；宿主结果不能证明原子配对或真实 namei/安全策略语义。

| 层次 | 实现入口 | 重点 |
| --- | --- | --- |
| 内核协议 | [`uapi/sumh.h`](../../../uapi/sumh.h)、[`sumh_ioctl.c`](../../../kernel/sumh/control/sumh_ioctl.c) | 协议、能力查询、参数边界及错误返回 |
| 内核规则和 VFS | [`kernel/sumh`](../../../kernel/sumh) | 规则生命周期、引用回收、目录视图、属性及进程可见性 |
| 用户空间 | [`userspace/ksud/sumhp`](../../../userspace/ksud/sumhp) | ioctl 客户端、配置校验/持久化、恢复、挂载后端、daemon 传输 |
| 管理器 | [`ui/sumh`](../../../manager/app/src/main/java/com/zying/zysu/ui/sumh) | 能力判断、保存/应用结果、状态刷新与错误显示 |

当前 SUMH 协议版本为 1。内核引擎初始化、控制器可用、某项能力受支持及某项视图已启用，必须分别验证。管理器摘要中的“协议 1”不能证明全部功能已经启用。

## 主机回归

模块重建新增两组生产函数执行测试，分别验证记录/存储故障与 namespace/热状态结果传递。宿主模型不代替真实 fsync、fork、内核 ioctl 或设备掉电；本批证据见[第二十四批验证](../../plans/SUMH-implementation-validation-batch24-2026-10-17.md)。

```sh
python3 userspace/ksud/tests/run_module_rebuild_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_module_rebuild_namespace_host_tests.py --cxx g++
```

以下命令从仓库根目录运行。推荐使用与 CI 一致的 Linux 环境，安装 Python 3、Clang 和支持 C++17 的 G++。

### 内核函数与接口

```sh
python3 scripts/ci/test_sumh_core.py --cc clang
python3 scripts/ci/test_sumh_host_harness.py --cc clang
python3 scripts/ci/test_sumh_source_capture.py --cc clang
python3 scripts/ci/test_sumh_source_identity.py --cc clang
python3 scripts/ci/test_sumh_merge_install.py --cc clang
python3 scripts/ci/test_sumh_merge_snapshot.py --cc clang
python3 scripts/ci/test_sumh_vfs_helpers.py --cc clang
python3 scripts/ci/test_sumh_directory_names.py --cc clang
python3 scripts/ci/test_sumh_directory_stream.py --cc clang
python3 scripts/ci/test_sumh_mount_views.py --cc clang
python3 scripts/ci/test_sumh_native_namespace.py --cc clang
python3 scripts/ci/test_selinux_hide_status.py --cc clang
python3 scripts/ci/test_sumh_guards.py --cc clang
python3 scripts/ci/test_sumh_kernel_build.py --cc clang
python3 scripts/ci/test_sumh_rules.py --cc clang
python3 scripts/ci/test_sumh_maps.py --cc clang
python3 scripts/ci/test_sumh_kstat.py --cc clang
```

| 测试脚本 | 覆盖范围 |
| --- | --- |
| `test_sumh_core.py` | 规则回收、引用所有权、setattr 和错误分支 |
| `test_sumh_host_harness.py` | 故意失败的断言不能被 POSIX 8 位退出码截断为成功，成功对照与错误行定位 |
| `test_sumh_source_capture.py` | 实际普通源单次捕获、借用引用、原生链接委托及后置 mount+dentry 检查、错误/元数据/类型与引用释放；保留 ABA 和检查后变化边界，不声称原子配对 |
| `test_sumh_source_identity.py` | 实际 ADD 控制入口及 store 对完整挂载/目录身份、可选 nofollow 身份和副作用的处理 |
| `test_sumh_merge_install.py` | 实际 MERGE 登记、至 EOF 收集、迭代锁外安装、持有父目录解析、递归共享预算和部分发布错误 |
| `test_sumh_merge_snapshot.py` | 通用 MERGE 读取快照及实际消费者、全部目标、旧根引用、完整名称收集和容量错误 |
| `test_sumh_vfs_helpers.py` | xattr 边界、目录遍历、无处理需求时的快速路径 |
| `test_sumh_directory_names.py` | 目录名称索引、合并和虚拟目录去重、首条优先与输出顺序、非 NUL 结尾名称及分配失败恢复 |
| `test_sumh_directory_stream.py` | 小缓冲目录续读、注入与原生目录 cookie 边界、合并规则与单独文件规则组合、负 dentry 及过滤后空批次 |
| `test_sumh_mount_views.py` | 保留模块时的 proc 视图策略与过滤函数 |
| `test_sumh_native_namespace.py` | 已撤除的 namespace 文本投影能力及 aggressive 模式拒绝行为 |
| `test_selinux_hide_status.py` | 配置意图与实际 SELinux 运行状态、初始化失败 |
| `test_sumh_guards.py` | 递归保护、哈希冲突、嵌套任务标记和空桶锁获取 |
| `test_sumh_kernel_build.py` | 内核构建字段校验、状态更新和恢复语义 |
| `test_sumh_rules.py` | 规则导出的分配与清理失败分支 |
| `test_sumh_maps.py` | 受管规则替换、失败原子性、临时规则与精确设备匹配优先级 |
| `test_sumh_maps_delete.py` | 实际 store/删除 ioctl/外层控制：精确键与 managed 保留、token 校验、失败不推进、旧代引用及满额/预算；ARM ABI/模型编译单列 |
| `test_sumh_kstat.py` | 显式属性规则的设备身份、路径重绑定、RCU 替换、失败保留与残留 hook 标记 |

这些脚本读取生产 C 函数或源文件，以 VFS、RCU、工作队列、锁等测试替身执行关键分支。它们不加载内核模块，也不进行真实并发压力测试。分支内分配或锁计数的改善不能换算为设备延迟提升比例。

目录名称去重使用单次枚举内的红黑树索引，将该阶段的名称比较从平方级降为 O(N log N)，保留原列表顺序与首条规则优先级；这不是完整目录读取的耗时保证。文件视图的续读测试模拟 `getdents64` 将 offset 写入上一条记录的行为，并验证注入与原生目录之间的边界。设备上仍需在 ext4、f2fs 和 tmpfs 验证小缓冲 `getdents64`、`telldir`/`seekdir`、已有文件句柄下清空或重装规则，以及并发目录遍历。主机替身不能验证各文件系统的 `llseek` 和真实 SRCU 时序。

kstat 设备回归需覆盖：两个文件系统中的同号 inode、精确规则与数字通配规则共存、文件替换后重新提交路径、硬链接别名冲突、更新失败保留旧规则，以及持有旧文件句柄时更新规则。并发读取 stat 时应只观察到完整的新或旧属性集；旧 inode 若没有剩余显式规则或有效虚拟映射，其残留 hook 应保留原始属性。`is_static` 不提供自动重绑定，零值字段仍遵循原 ABI，不能把它们当成新增的静态属性管理功能。

### 用户空间与挂载控制

```sh
python3 userspace/ksud/tests/run_sumh_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_sumhp_daemon_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_mount_alias_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_sumh_planner_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_magic_mount_host_tests.py --cxx g++
python3 userspace/ksud/tests/run_overlay_mount_host_tests.py --cxx g++
```

SUMH 用户空间测试覆盖客户端响应长度/文本边界、驱动探测、规则恢复、持久化失败、配置迁移、内核构建设置及 CLI。daemon 测试包含真实 POSIX socket 的分帧、截止时间和重试处理，但不会启动 Android 上的生产 daemon。挂载测试覆盖分区别名、挂载规划以及 Magic Mount、OverlayFS 的相关生产逻辑，不会在宿主机实际修改 Android 挂载树。

挂载规划回归还覆盖规则前缀的分区别名、解析后的模式冲突、whiteout 拆分为既有隐藏操作，以及准备失败时不发送挂载请求。生命周期回归检查热挂载标记写入/恢复失败、重建前配置校验、Overlay 挂载身份变化、回滚残留记录和挂载信息读取失败。设备上仍需验证真实命名空间、whiteout 属性和挂载系统调用。

自动化入口为 [`.github/workflows/sumh-tests.yml`](../../../.github/workflows/sumh-tests.yml)。工作流存在或曾执行过，不代表当前提交所有目标的测试已通过；验证报告应记录具体提交、命令、编译器和结果。

与 CI 相同的 Linux 原生回归集合可统一执行：

```sh
python3 scripts/ci/sumh_ci.py host --report out/sumh-ci/native.json
```

该入口逐个执行 24 个测试脚本的 `main()`，包括启动挂载完成通知、daemon、耐久文件写入和两种挂载后端。单项失败后继续收集其他结果，最后返回失败退出码；JSON 记录提交、命令、退出码和耗时。`unittest discover` 只导入这些脚本，不能替代原生入口的编译和执行。门禁自身的回归可用 `python3 -B -m unittest discover -s scripts/ci -p 'test_sumh_ci.py'` 验证。

独立工作流同时运行 `com.zying.zysu.ui.sumh.*` 包中的 Manager JVM/Compose 测试，上传 JUnit XML 和 HTML 报告。主分支、dev、feat 分支及 PR 中的 Manager、用户态、共享接口、内核和构建依赖变化都会触发；tag 发布通过本地可复用工作流调用同一套检查。发布必须等待 native 和 Manager 两个 job 成功，且测试、汇总和构建提交完全一致；失败、取消、跳过、缺少任务或提交不一致都不能放行。普通完整 Manager 构建也包含 SUMH 包测试。

CI 的 `SUMH-native-results`、`SUMH-Manager-results` 和 `SUMH-gate-result` 分别保存原生结果、Manager 报告及汇总状态。任务未执行或环境准备失败时，汇总记录对应 job 的失败/跳过状态，不把缺失报告视为通过。首次接通后仍需在实际 Actions 运行中核对报告，并用失败分支验证发布停止；本地主机门禁测试不能作为远端 Actions 已通过的证据。

Windows 下，内核脚本可通过 `--cc` 指定 NDK 的 `clang.exe`，使用同目录 `ld.lld.exe` 构建本机测试。SUMH 用户空间测试可使用 `--zig` 指定 Zig。daemon 测试的 `--portable-only` 仅执行可移植故障注入部分；`--target x86_64-linux-musl --compile-only` 仅交叉编译，不运行 Linux socket 测试。Windows 模拟传输头不能验证 Android ioctl ABI。

## 管理器与完整构建

从 `manager` 目录执行：

```sh
./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

这验证管理器编译、JNI 构建、现有单元测试及静态检查。完整内核还应按 [构建指南](workflow-build.md) 对发布所需的目标 KMI 编译，不能用提取函数的主机测试替代。是否完成 APK/ksud/LKM 打包也需检查实际产物。

### 功能管理

挂载与隔离页的隔离选项直接展示全部 SUMH 功能，并按挂载可见性、文件系统和 Maps 分组；功能数量较少，因此不再提供搜索和筛选控件。内核构建参数默认收起，通过“编辑参数”展开。数量和开关反映保存的配置及当前内核能力，不表示实际运行状态；能力未读到和内核不可用也不等同于确认不支持。规则页仍保留路径搜索和状态筛选，用于在规则较多时快速定位。

切换内核后，可以逐项关闭之前保存的设置。内核可用且能力已确认时，还可通过“清理不支持的设置”一次关闭不支持的已开启功能；确认框列出目标，内核构建字段草稿和其他设置保留。

保存功能设置会尝试立即应用；挂载后端、挂载源、文件系统和路径等设置在重启后生效；控制器日志选项只保存配置。挂载或日志设置不会清除先前的内核应用失败提示。保存成功后若读取状态失败，应分别显示保存结果和读取错误，并在成功刷新前禁用依赖旧配置的操作。

重点回归：功能页完整分组展示、大字体换行、未知能力下禁止批量清理、逐项关闭不支持的功能、保留内核构建草稿、保存失败与应用失败、保存成功后刷新失败、切回前台后刷新；规则页搜索与状态筛选仍需覆盖。相关管理器测试可单独执行：

```sh
./gradlew :app:testDebugUnitTest --tests '*SUMH*'
```

### 自定义隐藏规则的批量管理

“内核规则”提供路径搜索和全部、文件映射、隐藏路径、功能四种类型筛选。搜索同时匹配可见路径与来源路径，保留空格和 Unicode，不规范化输入。映射卡片分别展示两个路径，点击后可查看和复制完整值；旧协议无法可靠分割的路径对仍按原文展示与复制。筛选后的数量只统计已读取记录，不完整列表的提示继续显示。控件与列表共同滚动，便于横屏和大字体使用；路径详情使用独立滚动区域。

回归时覆盖来源路径搜索、组合筛选、无匹配后清除筛选、长路径详情、复制末尾空格、模糊路径对，以及横屏大字体与深色主题。对应测试为 `SUMHRuntimeRuleDisplayTest` 和 `SUMHRuntimeRuleFilterTest`；截图输出到 `manager/app/build/reports/sumh-layout`。

“规则”页默认显示“隐藏路径”，可按路径搜索，并按已生效或需要处理的状态筛选；“内核规则”查看实际内核记录和已绑定的隐藏路径，等待绑定的保存定义不计入内核记录。同路径隐藏项去重，功能项单独标识；不完整的文本列表会显示提示，无法可靠分割的路径对按原文展示。点击已保存的路径即可直接编辑，保存时先添加新路径，再删除原路径；添加失败不会删除原规则，部分完成时保留输入并提示刷新检查新旧路径。

“多选”模式下才显示批量操作，可复制路径、重试或删除；长按任意规则也可进入多选，返回键先退出多选。全选、取消和反选仅影响当前筛选中的路径，隐藏的选择保持不变。删除确认框列出实际选中路径，筛选不会把已经选中的其他路径偷偷替换成当前列表。列表使用独立滚动区域，修改搜索或状态筛选后回到第一个匹配项，无匹配时可直接清除筛选。新增、批量导入和每条规则的编辑分别保留草稿，关闭窗口或切换规则不会重置草稿，可用“恢复原路径”放弃该条修改。

“更多操作 → 批量导入”接受每行一个绝对路径，在输入框下显示重复项及无效行，可直接添加有效项；已完成的路径从草稿中移除，未完成和无效行保留供修改。单条新增直接提交，无需再次预览。管理器只检查控制器要求：以 `/` 开头、不含 NUL、UTF-8 长度小于 4096 字节；空格、Unicode、根目录、点路径段和重复斜杠原样传递，由控制器及内核返回实际处理结果，不在界面中擅自规范化路径。

批量操作没有管理器人为数量上限，逐项执行并显示进度，可停止尚未发送的操作；当前请求会完成，已经完成的修改不会回滚。某条失败时保留具体错误并继续处理其他条目，可再次处理未完成项。操作完成表示控制器接受请求，是否已绑定仍以刷新后的内核状态为准。批量操作只涉及选中的用户规则，不调用全局清空。托管规则从配置删除后若内核删除失败，仍显示为仅在内核中存在，便于继续清理；复制保存规则时不包含这类残留项。

控制器连接失败、请求超时或内核状态读取发生 I/O 错误时停止发送剩余请求，并将这些路径标为未执行，避免重复等待超时。超时请求的结果可能未知，应先刷新状态，再明确重试；已成功的条目仍保留在结果中。

已完成批次的汇总和失败项可随页面状态恢复；超大结果仅保存限定体积的失败详情，恢复后明确提示未保留的数量，重试只针对完整保留的路径。汇总栏可收起，通过“更多操作 → 最近操作结果”重新查看。批量导入中，已保存但应用失败的路径仍可通过“继续未完成项”重试，不会因为配置中已存在就被跳过；已完成路径不再提交。

旧内核的通用删除接口会解析路径别名，且无法区分用户规则与模块规则。内核可用但未提供托管用户规则能力时，管理器拒绝删除并保留配置，提示使用支持托管规则的内核；不能从可能截断、未转义的文本规则列表推断删除安全。内核不可用时仍可删除已保存的定义。每条删除前重新检查能力和归属，避免兼容内核在上一条删除后失去托管模式时回退到通用删除。

回归时覆盖：直接编辑、末尾空格、重复路径、超过 100 条输入、UTF-8 长路径、有效与无效行混合导入、部分失败后继续、停止剩余操作、失败项重试、隐藏选中项的删除确认、内核不可用时保存/删除、刷新失败保留操作结果、模块规则不被批量删除，以及批量过程中旋转屏幕或切到后台。Compose 界面测试覆盖点击编辑、多选行为、导入及窄屏大字体下的保存按钮；截图输出到 `manager/app/build/reports/sumh-layout`。这些主机测试不替代设备输入法及真实内核验证。

Maps 数字规则的 inode/device 输入使用无符号十进制值，负数、溢出和无效文本应在提交前阻止；伪装路径按内核 256 字节字段限制校验，回车/换行不能进入 proc 文本。数字规则仅在当前运行时有效。新的路径规则保存目标文件和替换文件路径，启动完成后重新解析身份并原子替换受管部分；路径缺失或整批校验失败时保留原规则。验证配置持久化、旧内核能力门控、原地编辑与重命名、失败草稿和旋转恢复、满额时编辑、重复与编码预算校验、精确 device 优先、临时规则保留和 saved/applied 错误反馈。还需从实际配置应用入口验证 Maps 失败不会关闭其他功能，待应用提示出现时不重复显示应用按钮。详见 [Maps 路径规则](sumh-maps-rules.md)。

模块路径编辑还需覆盖 `hide`、`none` 的不同效果、最长前缀匹配与父目录隐藏的边界、旧 Overlay/Magic 路径模式、非法路径和重复项、旋转恢复、失败后保留草稿，以及单次保存模式和规则的原子性。读取损坏配置时，应拒绝保存与重放并保留旧运行状态。详见 [模块路径规则](sumh-module-rules.md)。

## 设备基线采集

### 配置格式与故障检查

控制器的新配置包含 `schema_version: 1`。没有版本字段的旧配置仍可读取；读取和预检不会写回。首次明确保存旧配置前，控制器会将原始字节备份到同目录的 `config.json.pre-schema-1.bak`，备份未完成则不覆盖配置。后续保存当前版本不会覆盖这份迁移备份。`schema_version` 由控制器管理，不能通过普通配置补丁修改。

文件加载和 CLI/Manager 更新共用字段类型、枚举、整数范围、路径及分区名校验。分区名接受 1–255 字节的英文字母、数字、下划线和连字符；工作目录须为绝对路径，镜像目录允许留空使用自动配置。无效字段返回具体错误，失败时不更新调用者持有的配置；仅校验不会切换日志状态。Kernel Build reset 仍可修复其已知字段并关闭保存开关，但不再顺带改写存在未知字段的文件。

兼容例外只用于无版本旧文档：空 `fs_type` 和 `mount_backend` 读作 `auto`，空 `mount_hide_mode` 读作 `normal`，与旧版一致；schema 1 和新 patch 仍拒绝空值。只读加载不修改原文，首次显式保存才迁移并备份原文件。

`config show` 返回原配置内容，保留未知字段和原始数字。当前版本包含未知字段时可以读取已知设置，但合并、生成默认配置及 Kernel Build 配置重置会拒绝写入；未来版本号会拒绝加载和写入。应保留原文件，使用理解该格式的控制器或经过验证的格式转换，不能靠旧客户端重新保存来降级。迁移备份仅恢复持久配置，不代表运行规则或源文件已恢复。

主机回归覆盖全部已知字段的错误类型、NUL、相对路径、非法枚举与分区名、旧版本、未来版本、未知字段、UTF-8 路径、备份失败和失败时原值保留。设备上还需验证磁盘故障、启动读取、CLI/Manager 提示和恢复后的实际规则；主机通过不代表这些设备用例已通过。

先记录源码提交、设备型号、Android 版本、内核 KMI、管理器/ksud 版本、加载方式、模块列表和文件系统。在已获授权的 root shell 中读取控制器状态：

```sh
/data/adb/ksud sumhp config show
/data/adb/ksud sumhp api system
/data/adb/ksud sumhp api features
/data/adb/ksud sumhp api hooks
/data/adb/ksud sumhp api mounts
/data/adb/ksud sumhp api backends
/data/adb/ksud sumhp sumh list
/data/adb/ksud sumhp hide list
```

配置表示保存的意图；能力、hooks、挂载状态和规则反映不同的运行信息。报告时同时保留这些输出，不要仅截取管理器中的成功提示。

## 设备验证矩阵

| 场景 | 检查内容 |
| --- | --- |
| 挂载后端 | 分别使用 SUMH、OverlayFS、Magic Mount，检查模块优先级、覆盖文件、链接、读写、属性与目录内容；以运行状态确认实际选用后端 |
| 规则并发 | 持续遍历目录时添加、删除和清空测试规则，检查内核日志、引用生命周期及目录位置是否异常 |
| 目录缓存生命周期 | 在专用测试挂载验证注入独有缓存、共享目录客户端、普通卸载/延迟卸载、freeze、CLEAR/stop、旧目录 FD 和外部 i_fop 替换；记录 inode→superblock 释放顺序、最终 shutdown 时机、lockdep/KASAN/KCSAN 及内存峰值。宿主测试与 ARM 提取对象见[第三十二批记录](../../plans/SUMH-implementation-validation-batch32-2026-10-17.md)，不代表实际内核卸载或 CFI 通过；不得用全局 clear 作为生产环境单条规则撤销 |
| 注入目录归属 | 专用测试环境验证两个映射共用父目录、映射与 MERGE 共用 inode、分区别名、同键换 inode、相同 ADD、部分物化、重复 MERGE、派生键 DEL 的 EBUSY 与零副作用；延迟旧回调跨替换/CLEAR/重装后，核对索引、计数、精确 inode 标记和引用。补原生路径/渲染/分配/FOP 故障、最后使用者退出、缓存与声明两种释放顺序、卸载/freeze/旧 FD 及 lockdep/KASAN/KCSAN。代码与主机证据边界见[第三十三批记录](../../plans/SUMH-implementation-validation-batch33-2026-10-17.md)；测试规则尚无独立完整 owner 时只能在可恢复的专用环境布置 |
| 映射源对象 | 普通源单次捕获、捕获后 rename/替换、同 dentry 不同挂载、持有父目录的非链接子项解析、DT_UNKNOWN/错误 d_type、源根替换与根内子项替换、原地写入、旧 FD/新 open/mmap；真实迭代锁外安装、分页至 EOF、cookie 不前进、递归预算/循环和引用回收。链接后置检查分别测试持续替换、ABA、动态链接和检查后变化，不能当成原子源树；另测 marker 缺失/锁竞争、权限/I/O 错误、部分发布和重建拒绝重放 |
| Maps 快照与预算 | 旧/新/零字节/初始空流、跨块与扩容重滤、复制故障、临时 1024 + 受管 256、旧代保留及预算拒绝后旧集合完整性；按[场景 23](sumh-usage-scenarios.md#23-maps-更新后分别验证旧流与新流)核对，并采多读者/写入并发下的 P99、吞吐和内存峰值 |
| Manager 临时 Maps | 按[场景 26](sumh-usage-scenarios.md#26-在-manager-核对并删除一条临时-maps-规则)验证能力未知/不支持、手动分页/过期、历史行禁删、精确确认、单次提交、所有结果刷新、旋转/后台/关闭重开/进程恢复；英文/中文、大字体、横屏与 TalkBack 分开记录。[第三十批记录](../../plans/SUMH-implementation-validation-batch30-2026-10-17.md)区分本地渲染、编译和未执行设备门禁 |
| 临时 Maps 条件删除 | 按[场景 25](sumh-usage-scenarios.md#25-撤销一条临时-maps-覆盖并保留受管规则)核对精确键/通配/managed、旧实例/过期 generation、无过滤入口清理、失败保留/满额/最后一项/旧流 pin；输出失败和断连不自动重放；root/普通应用观察分开。宿主与 ARM 模型结果见[第二十九批记录](../../plans/SUMH-implementation-validation-batch29-2026-10-17.md)，真实设备未执行 |
| 文件属性 | 只读/可写源挂载、chmod、chown、truncate、已有文件句柄以及目标内核使用的 idmap 接口 |
| xattr | size-only 查询、短缓冲区、大列表、OverlayFS 属性过滤和原生错误返回 |
| 持久化/恢复 | 规则文件缺失、损坏、不可读、恢复正常及控制请求短暂失败后的重试；读取失败不能被当作清空规则 |
| 未启用配置方案 | [方案工作流](sumh-profiles.md)的真实 namespace/SELinux、文件/stdin 输入、合作并发、未知结果查询、fsync/掉电故障；所有 profile 操作前后核对活动配置、内核和启动标记无变化。激活和运行恢复尚未实现，不能验成通过 |
| 管理器操作 | 刷新与保存互斥、能力不可用、保存失败、保存成功但应用失败、重试及离开页面后的反馈 |
| 普通应用视图 | 主进程和 isolatedProcess 的模块保留/卸载策略、mountinfo、maps、statfs、namespace 身份及已打开文件句柄 |
| 重启 | post-fs-data 与 boot-completed 恢复、应用冷启动、不同加载方式下的能力变化 |

规则清空及后端变更应在可恢复的测试环境中执行，保留原配置和测试模块。OverlayFS、Magic Mount 的专用场景分别见 [OverlayFS 验证](overlayfs-validation.md) 和 [Magic Mount 验证](magic-mount-validation.md)。

挂载隐藏目前使用 `normal`。内核拒绝旧 `aggressive` 模式；用户空间恢复旧配置时仅对“不支持”错误回退到 normal，管理器保存相关功能时会规范该字段。namespace 身份应联合验证 `readlink`、`stat` 和 `fstat`。跨接口限制和启动属性/SELinux 验证见 [应用环境可观察面](environment-observability.md)。

设备测试结果应包含预期、实际值和失败日志。仅主机回归通过时，应明确标注“主机回归通过，完整内核/设备结果未验证”，不要用固定测试数量、旧产物大小或历史测试记录代替当前提交的验证。
