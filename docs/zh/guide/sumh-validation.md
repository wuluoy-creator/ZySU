# SUMH 回归验证指南

SUMH 是内核中的 VFS 视图引擎；`ksud` 内置的 SUMHP 控制器负责配置、模块挂载规划、规则恢复及诊断接口。管理器通过 JNI 和控制器呈现状态及执行配置操作。本文给出当前仓库已有的回归入口和设备验证范围，不作为某个设备或发行版的通过证明。

## 验证对象

| 层次 | 实现入口 | 重点 |
| --- | --- | --- |
| 内核协议 | [`uapi/sumh.h`](../../../uapi/sumh.h)、[`sumh_ioctl.c`](../../../kernel/sumh/control/sumh_ioctl.c) | 协议、能力查询、参数边界及错误返回 |
| 内核规则和 VFS | [`kernel/sumh`](../../../kernel/sumh) | 规则生命周期、引用回收、目录视图、属性及进程可见性 |
| 用户空间 | [`userspace/ksud/sumhp`](../../../userspace/ksud/sumhp) | ioctl 客户端、配置校验/持久化、恢复、挂载后端、daemon 传输 |
| 管理器 | [`ui/sumh`](../../../manager/app/src/main/java/com/zying/zysu/ui/sumh) | 能力判断、保存/应用结果、状态刷新与错误显示 |

当前 SUMH 协议版本为 3。隐身模式、挂载隐藏、OverlayFS 扩展属性隐藏和 Statfs 视图是固定内建行为，没有配置开关或可修改的启停状态。内核引擎初始化、控制器协议匹配、各项 hook 能力及目标视图范围仍须分别验证；协议匹配不表示所有可选 hook 都存在。

### 授权页的模块隔离

授权页“卸载模块挂载”保留按应用 UID 的设置。应用 profile 是唯一配置来源；SUMH 的有效 UID 策略同时供管理器查询、模块内容视图和实际卸载使用。SUMHP 负责登记真实模块挂载及其设备、根目录和文件系统身份，不维护另一份按应用开关，也不会因单个应用的设置而全局卸载模块。

开启后，该应用不使用 SUMH 提供的模块文件，并在安全的独立挂载命名空间内卸载继承的模块挂载。关闭后，应用可继续使用模块内容；挂载表过滤是独立行为。修改后必须重启应用，已有进程的命名空间和文件句柄不会自动恢复。Root 授权和管理器保持原有豁免，系统服务 UID 不提供按应用修改入口。

两段隔离 UID 范围（90000–98999、99000–99999，含多用户）使用相同的隔离决策。WebView zygote 的内容视图与卸载均遵循其专用设置。实际卸载仍要求可信 zygote 来源和与父进程不同的挂载命名空间；共享命名空间不会被修改。延迟任务执行前重新检查 UID 与有效策略，回退卸载须匹配 SUMHP 登记的挂载身份。

设备回归应分别检查 SUMH、Magic Mount、OverlayFS 下开关前后重启应用的实际文件内容，包含默认值、显式覆盖、Root 撤销、共享 UID、多用户、app-zygote 隔离子进程和 WebView。确认保留模块的父应用不受隔离子进程影响，并验证与父进程共享命名空间时不会卸载父进程挂载。

## 主机回归

以下命令从仓库根目录运行。推荐使用与 CI 一致的 Linux 环境，安装 Python 3、Clang 和支持 C++17 的 G++。

### 内核函数与接口

```sh
python3 scripts/ci/test_sumh_core.py --cc clang
python3 scripts/ci/test_sumh_vfs_helpers.py --cc clang
python3 scripts/ci/test_sumh_directory_names.py --cc clang
python3 scripts/ci/test_sumh_directory_stream.py --cc clang
python3 scripts/ci/test_sumh_mount_views.py --cc clang
python3 scripts/ci/test_sumh_managed_mounts.py --cc clang
python3 scripts/ci/test_sumh_app_umount.py --cc clang
python3 scripts/ci/test_sumh_zygote_umount.py --cc clang
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
| `test_sumh_vfs_helpers.py` | xattr 边界、目录遍历、无处理需求时的快速路径 |
| `test_sumh_directory_names.py` | 目录名称索引、合并和虚拟目录去重、首条优先与输出顺序、非 NUL 结尾名称及分配失败恢复 |
| `test_sumh_directory_stream.py` | 小缓冲目录续读、注入与原生目录 cookie 边界、合并规则与单独文件规则组合、负 dentry 及过滤后空批次 |
| `test_sumh_mount_views.py` | 保留模块时的 proc 视图策略与过滤函数 |
| `test_sumh_managed_mounts.py` | SUMHP 登记协议、挂载身份、失败原子性及回退卸载验证 |
| `test_sumh_app_umount.py` | 应用配置、有效 UID 策略、授权查询及延迟卸载复查 |
| `test_sumh_zygote_umount.py` | 可信 zygote 类型、非 root 隔离进程路径及父进程命名空间保护 |
| `test_sumh_native_namespace.py` | 保留原生命名空间身份，以及旧启停/模式修改请求被拒绝 |
| `test_selinux_hide_status.py` | 配置意图与实际 SELinux 运行状态、初始化失败 |
| `test_sumh_guards.py` | 递归保护、哈希冲突、嵌套任务标记和空桶锁获取 |
| `test_sumh_kernel_build.py` | 内核构建字段校验、状态更新和恢复语义 |
| `test_sumh_rules.py` | 规则导出的分配与清理失败分支 |
| `test_sumh_maps.py` | 受管规则替换、失败原子性、临时规则与精确设备匹配优先级 |
| `test_sumh_kstat.py` | 临时与受管属性规则的设备身份、字段掩码、整批替换、原始来源查询、失败保留与 RCU 回收 |

这些脚本读取生产 C 函数或源文件，以 VFS、RCU、工作队列、锁等测试替身执行关键分支。它们不加载内核模块，也不进行真实并发压力测试。分支内分配或锁计数的改善不能换算为设备延迟提升比例。

目录名称去重使用单次枚举内的红黑树索引，将该阶段的名称比较从平方级降为 O(N log N)，保留原列表顺序与首条规则优先级；这不是完整目录读取的耗时保证。文件视图的续读测试模拟 `getdents64` 将 offset 写入上一条记录的行为，并验证注入与原生目录之间的边界。设备上仍需在 ext4、f2fs 和 tmpfs 验证小缓冲 `getdents64`、`telldir`/`seekdir`、已有文件句柄下清空或重装规则，以及并发目录遍历。主机替身不能验证各文件系统的 `llseek` 和真实 SRCU 时序。

kstat 设备回归需覆盖：两个文件系统中的同号 inode、临时规则与受管规则共存、文件替换后重新应用路径、硬链接别名冲突、更新失败保留旧规则，以及持有旧文件句柄时替换规则。并发读取 stat 时应只观察到完整的新或旧属性集；旧 inode 若没有剩余规则或有效虚拟映射，其残留 hook 应保留原始属性。受管规则的字段掩码须严格限制覆盖范围，并允许显式零值；来源文件本身受投影时，原始来源查询仍须返回未投影属性。`is_static` 不提供自动重绑定，旧 ABI 的零值仍表示保留原字段。详见 [Kstat 路径规则](sumh-kstat-rules.md)。

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

Windows 下，内核脚本可通过 `--cc` 指定 NDK 的 `clang.exe`，使用同目录 `ld.lld.exe` 构建本机测试。SUMH 用户空间测试可使用 `--zig` 指定 Zig。daemon 测试的 `--portable-only` 仅执行可移植故障注入部分；`--target x86_64-linux-musl --compile-only` 仅交叉编译，不运行 Linux socket 测试。Windows 模拟传输头不能验证 Android ioctl ABI。

## 管理器与完整构建

从 `manager` 目录执行：

```sh
./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

这验证管理器编译、JNI 构建、现有单元测试及静态检查。完整内核还应按 [构建指南](workflow-build.md) 对发布所需的目标 KMI 编译，不能用提取函数的主机测试替代。是否完成 APK/ksud/LKM 打包也需检查实际产物。

### 功能管理

挂载与隔离页的“标准功能”包含进程隔离和内核伪装。四项固定保护不显示开关，也不进入配置、备份导出或配置应用流程。内核伪装显示当前值，点击后直接编辑 Release 和 Version；保存即应用，两项都清空后保存恢复原值。规则页仍保留路径搜索和状态筛选，用于在规则较多时快速定位。

旧配置文件和旧备份中遗留的四项启停字段直接忽略或剔除，不参与布尔校验，不转写为 `true`。直接配置更新及旧启停命令必须拒绝修改。切换到不匹配的旧内核时应报告协议不匹配，不能误报固定保护已生效。

保存功能设置会尝试立即应用；挂载后端、挂载源、文件系统和路径等设置在重启后生效；控制器日志选项只保存配置。挂载或日志设置不会清除先前的内核应用失败提示。保存成功后若读取状态失败，应分别显示保存结果和读取错误，并在成功刷新前禁用依赖旧配置的操作。

重点回归：固定保护不依赖任何保存值且不能通过旧命令关闭、旧备份不重新引入字段、内核伪装保存和清空恢复、保存失败保留草稿、大字体换行、保存成功后刷新失败、切回前台后刷新；规则页搜索与状态筛选仍需覆盖。相关管理器测试可单独执行：

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
| 文件属性 | 只读/可写源挂载、chmod、chown、truncate、已有文件句柄以及目标内核使用的 idmap 接口 |
| xattr | size-only 查询、短缓冲区、大列表、OverlayFS 属性过滤和原生错误返回 |
| 持久化/恢复 | 规则文件缺失、损坏、不可读、恢复正常及控制请求短暂失败后的重试；读取失败不能被当作清空规则 |
| 管理器操作 | 刷新与保存互斥、能力不可用、保存失败、保存成功但应用失败、重试及离开页面后的反馈 |
| 普通应用视图 | 主进程和 isolatedProcess 的模块保留/卸载策略、mountinfo、maps、statfs、namespace 身份及已打开文件句柄 |
| 重启 | post-fs-data 与 boot-completed 恢复、应用冷启动、不同加载方式下的能力变化 |

规则清空及后端变更应在可恢复的测试环境中执行，保留原配置和测试模块。OverlayFS、Magic Mount 的专用场景分别见 [OverlayFS 验证](overlayfs-validation.md) 和 [Magic Mount 验证](magic-mount-validation.md)。

挂载隐藏目前使用 `normal`。内核拒绝旧 `aggressive` 模式；用户空间恢复旧配置时仅对“不支持”错误回退到 normal，管理器保存相关功能时会规范该字段。namespace 身份应联合验证 `readlink`、`stat` 和 `fstat`。跨接口限制和启动属性/SELinux 验证见 [应用环境可观察面](environment-observability.md)。

设备测试结果应包含预期、实际值和失败日志。仅主机回归通过时，应明确标注“主机回归通过，完整内核/设备结果未验证”，不要用固定测试数量、旧产物大小或历史测试记录代替当前提交的验证。
