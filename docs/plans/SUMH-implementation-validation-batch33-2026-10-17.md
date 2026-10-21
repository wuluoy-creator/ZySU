# SUMH 第三十三批实施与验证记录

日期：2026-10-17。对应[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)与[54 项账本](SUMH-implementation-progress-2026-10-16.md)。本批已为现有映射与 MERGE 的注入目录路径索引和精确 inode 标记建立规则归属，完成源码审查、主机模型验证及独立测试复核。C06 保持部分实施，全部 54 项目标继续进行；真实内核与设备验收仍待完成。

## 实施合同

普通映射至多持有一个注入声明，MERGE 至多持有两个。路径键的使用计数与具体 inode 指针的使用计数分别维护；两个别名可共用一个 inode，同一个路径键也可关联不同时间观察到的 inode。最后一个使用者退出时才移除对应键或清除该 inode 的 `DIR_HAS_INJECT` 标记。

准备步骤在配置锁外执行，持有原生路径期间取得独立 inode 和 superblock 活跃引用；拒绝未出生或已经关闭的 superblock、非目录和 SUMH 合成对象。一个原生可见目录观察同时提供 MERGE 主键、别名和 inode。FOP 安装必须成功；后续准备失败仍可能留下第三十二批允许保留的 FOP 缓存。未使用的准备对象和退休对象均先释放 inode，再释放其保存的精确 superblock，不用路径重新查找清理目标。

每组最多两个声明先检查全部计数与容量，成功后才一起登记。普通 ADD 的首次映射行与新增路径键共用有符号规则总数的溢出检查；相同键或 inode 的重复贡献累计检查。替换先登记新声明再撤销旧声明，包括目标身份未变的重复 ADD。规则替换、DEL 与 CLEAR 在配置锁内同步撤销；延迟 RCU/worker 只释放资源，不改变活跃索引、标记、计数或 generation。复用已有 worker 并增加独立退休队列，保留 `rcu_barrier` 后 `flush_work` 的排空边界。

直接 ADD 只对真实 ENOENT 父目录保持无声明的兼容行为；权限、类型、渲染和分配错误必须返回，不能因父目录字符串为空而跳过登记。物化映射可为缺失前缀登记未绑定键；缺失的 MERGE 可见目录保留未绑定主键。顶层重复 MERGE 在新可见目录准备前返回 EEXIST，嵌套重复 MERGE 复用原捕获根。MERGE 父规则与已经成功的子项仍可能部分发布。

DEL 保留精确映射优先、原有隐藏规则其次的语义。只有派生注入键存在时返回 EBUSY，且在 dirhijack 删除或隐藏位修改前拒绝，不再提供原始路径键删除兜底。CLEAR 同步撤销所有映射/MERGE 声明后才归零；这没有把全局 CLEAR 变成单条或单模块撤销方式。

源码范围为 [规则结构](../../kernel/sumh/internal/sumh_types.h)、[存储合同](../../kernel/sumh/core/sumh_store.h)、[运行存储](../../kernel/sumh/core/sumh_runtime.c)、[物化接口](../../kernel/sumh/overlay/sumh_overlay.h)、[物化实现](../../kernel/sumh/overlay/sumh_overlay.c)和 [ioctl 生产入口](../../kernel/sumh/control/sumh_ioctl.c)。不新增 UAPI、能力位、CLI、Manager 按钮或持久格式。

## 静态检查记录

冻结合同和 API 为 `out/batch33-contract.md`、`out/batch33-kernel-api.md`。独立设计检查 `out/batch33-review-design.md` 指出直接 ADD 旧规范化分支会吞掉错误，以及顶层重复 MERGE 必须先于新增准备步骤识别；两项已修正。源码审查还发现删除旧 DEL 迭代器声明影响了文本 LIST 分支，现已改为 LIST 内部声明。最终六文件源码审查 `out/batch33-review-source.md` 未发现剩余阻断问题，指纹保存在 `out/batch33-review-source-hashes.json`；执行证据另行审查。

修改前 119 个文件保存在 `out/batch33-before/`，原始指纹为 `out/batch33-before-manifest.json`。第三十一、三十二批历史证据不重写。

## 执行验证记录

主测试由 [实际函数体提取](../../scripts/ci/test_sumh_inject_claims_source.py)、[可控内核服务](../../scripts/ci/test_sumh_inject_claims_model.py)、[声明与回收场景](../../scripts/ci/test_sumh_inject_claims_cases.py)、[生产入口场景](../../scripts/ci/test_sumh_inject_claims_producers.py)、[FOP 生命周期组合](../../scripts/ci/test_sumh_inject_claims_lifetime.py)和[运行器](../../scripts/ci/test_sumh_inject_claims.py)组成。主机执行结果如下：

| 检查 | 实际结果与范围 |
| --- | --- |
| 三个实际函数体视图 | 声明/存储 21 组、ADD/MERGE/DEL/CLEAR 20 组、实际 FOP/声明/CLEAR/stop 8 组，共 49 组场景通过；这是三个可执行程序中的场景分组，不是 49 个独立测试函数 |
| 重复生命周期 | 1,000 次实际声明 prepare/attach/detach/drain 模型循环通过，无累计引用或待释放对象；不代表设备启动/卸载循环 |
| 定向行为变异 | 25 个仅作用于生成代码的故障变体均编译、链接成功，并在各自指定断言返回保留退出码 253；其他断言返回 1，编译失败不能计作成功拦截 |
| 修改前反例 | 从保留源码提取旧 ADD/cleanup；父目录路径从 A 改指 B 后，旧实现错误清除 B 并遗留 A 标记，指定组合断言返回 253 |
| 相邻实现回归 | source identity、MERGE install、core 两种 idmap（各 30 项）、文本 LIST 和 CI helper（7 项）通过；原有覆盖保留，物化测试先建立真实前缀归属 |
| root 相邻集成 | directory stream/names、MERGE snapshot、source capture、VFS helpers、structured inventory、runtime state、FOP lifetime 八个运行器通过；最后一项执行已有 25 场景，不重复第三十二批未变化的变异 |
| LIST 声明回归 | 使用实际 dispatch 声明与 LIST 分支，非空 `inject /visible` 输出通过；正常生成源码语法检查成功，移除生产局部迭代器声明后按预期报未声明错误。该检查不计入 25 个行为变异 |
| ARM 模型对象 | 将最终三个正向视图分别编译为 ARM64/ARM32 freestanding 对象，六个对象均成功；保存命令、生成源码及对象指纹，无内核链接 |

主测试命令为：

```text
python scripts/ci/test_sumh_inject_claims.py --cc <NDK-r30-clang> --baseline-root out/batch33-before --out-dir out/batch33-tests-complete-inputs-generated --manifest out/batch33-tests-manifest.json
```

精确工具绝对路径、命令、退出码和生产/测试输入前后指纹保存在 `out/batch33-tests-manifest.json`，主日志为 `out/batch33-tests-final.log`；适配与 CI 日志索引为 `out/batch33-test-adapters-manifest.json`。root 八个相邻运行器证据为 `out/batch33-adjacent-manifest.json`。早期失败、未完整记录输入或断言归因不充分的运行均单独保留，不用作最终通过证据。

测试审查补齐了新建/替换 ADD 的规范化错误保留旧状态、活跃 FOP 客户端跨实际 CLEAR 的独立引用、溢出边界测试自身的宽整数运算、逐个断言唯一失败标识及全部提取依赖指纹。root 进一步发现旧文本 LIST 测试的手工声明会掩盖生产变量缺失，现已使用实际 dispatch 声明并加入上述语法反例，记录于 `out/batch33-list-declarations-manifest.json`。修正后重新执行完整主测试与相关适配测试；ARM 对象与最终主测试生成源码对应，记录于 `out/batch33-arm-evidence/manifest.json`。

独立测试复核 `out/batch33-review-tests.md` 已关闭 T1–T6，148 项已审查文件及证据指纹保存在 `out/batch33-review-tests-hashes.json`。root 汇总检查由 `out/batch33-final-check.py` 执行并保存 `out/batch33-final-manifest.json`，核对 54 项任务、文档本地链接、修改前快照、已审查源码/测试/日志/产物、ARM 对象及空白检查；不重新解释第三十一、三十二批历史结果。

上述视图执行实际声明/索引/计数/回收算法和相关生产入口；原生路径、元数据捕获、锁、RCU、worker、dirhijack 等仍含服务替身。没有执行完整 ioctl 准入与运行 generation 层；可控 FOP 客户端也不是实际 dh/su 内部实现。服务替身测试不能代替内核锁、RCU、卸载或设备观察者验收。

本批没有设备测试、完整内核构建或性能通过结论。ARM 模型对象编译独立记录，不能写成内核链接或 KMI 通过。

本机构建条件经过只读检查，记录于 `out/batch33-kernel-build-feasibility.md`：NDK r30 可用，但相关输出目录没有完整内核构建树，未找到可用的 Linux/DDK 执行环境、匹配的生成头文件、`Module.symvers` 和 `vmlinux`。因此当前测试模型不能替代四个受支持 KMI 的完整模块编译、链接和符号检查。

## 剩余边界

- 本批只覆盖注入路径索引与 inode 标记；dirhijack 子项/虚拟祖先、hidden/spoof/xattr 状态及被覆盖规则恢复仍需完整归属模型。
- 路径规范化、可见目录捕获与源捕获不是原子快照；ABA、检查后的替换和目录内容变化仍存在。
- FOP 与注入聚合对象各自持有引用，缓存可能延迟文件系统最终关闭；没有按规则即时回收全部 FOP 缓存的承诺。
- 不提供模块 owner、generation 条件提交、整体回滚或精确恢复；现有控制器首次全局 reset 与部分发布限制不变。F06/F11/F12 和 F07 激活不因本批开放。
- 真实设备仍须验证普通/延迟卸载、freeze、旧 FD、并发 CLEAR/重装、内存压力、lockdep/KASAN/KCSAN、KMI/CFI 及普通应用视图。
