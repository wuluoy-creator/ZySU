# SUMH 第三十四批实施与验证记录

日期：2026-10-18（Asia/Shanghai）。对应[全面计划](SUMH-comprehensive-improvement-plan-2026-10-16.md)、[54 项账本](SUMH-implementation-progress-2026-10-16.md)和[本批静态设计](SUMH-dirhijack-ownership-design-2026-10-17.md)。十个产品文件已完成实现与独立静态审查，专用归属测试、相邻回归、ARM 模型对象和独立测试审查均已完成。**本批主机与静态验收完成，C06 仍部分实施，F06/F11/F12 继续保留门禁。**

## 已实现行为

目录挂接按持有的原生父 inode 与复制的子名称建立稳定槽位。普通映射、物化 SHADOW、旧 HIDE、受管隐藏、SU 和虚拟祖先均持有类型化归属凭据。非 SU 凭据按顺序选择最近仍存活的一项，撤销后恢复最近的剩余项；SU 独占。同源映射更新永久替换旧行，不保留撤销历史。

直接 ADD 与物化映射让注入标记、目录挂接共用同一次可见父目录观察。准备和联合预检完成正常错误与容量检查后才发布，失败保留旧行；MERGE 后续子项失败仍可能留下前面成功的子项。DEL 依次查原始存储键、词法规范化键、使用原始输入解析的当前原生别名，每一步保持映射优先于隐藏、派生键返回 EBUSY 的顺序。

规则退出时同步撤销归属，延迟回调只释放捕获的资源。CLEAR 保留活动 SU，排空已撤销的这一批资源；永久停止先关闭并等待生产入口，再排空全局释放链。尚未发布但已失效的准备对象仍由调用者终止。批量撤销不受普通单次变更的两个失效记录槽限制。

读取端在 RCU 与短锁内复制不可变视图，离开 RCU 前独立保留源路径和虚拟路径；必要路径复制失败返回 ENOMEM。dentry 名称在锁内复制，睡眠分配后重新检查父目录与名称。最后一个目录引用先释放原生父目录，再注销并同步回收 SOP；普通删除或准备终止无需依赖后续 CLEAR 请求回收。

产品范围为 [目录挂接实现](../../kernel/sumh/core/sumh_dirhijack.c)、[内部接口](../../kernel/sumh/core/sumh_dirhijack.h)、[规则结构](../../kernel/sumh/internal/sumh_types.h)、[运行存储](../../kernel/sumh/core/sumh_runtime.c)、[存储合同](../../kernel/sumh/core/sumh_store.h)、[ioctl 入口](../../kernel/sumh/control/sumh_ioctl.c)、[物化入口](../../kernel/sumh/overlay/sumh_overlay.c)、[SU 入口](../../kernel/feature/sucompat_vfs.c)、[启动与退出](../../kernel/sumh/core/sumh_bootstrap.c)和 [SOP 回收](../../kernel/sumh/core/sumh_sop_shadow.c)。本批没有新增 UAPI、CLI、Manager 或持久格式。

## 静态检查证据

合同、API 和测试设计分别保存在 `out/batch34-root-contract.md`、`out/batch34-ownership-api.md`、`out/batch34-root-api-notes.md` 与 `out/batch34-ownership-tests-design.md`。修改前 303 个文件保存在 `out/batch34-before/`，其指纹为 `out/batch34-before-manifest.json`；历史第三十二、三十三批证据保持不变。

`out/batch34-review-source.md` 已接受冻结范围，十文件审查指纹与 `out/batch34-kernel-hashes.json` 一致。S1 名称复制/重查、S2 精确键与原始路径的删除顺序、O1 普通删除的 SOP 回收已在源码关闭；对应可执行场景和定向故障已由下列专用测试验证。静态审查不能代替运行验证。

## 已执行检查

2026-10-18 执行 `python -B out/batch34-run-adapters.py --label resumed`，四个运行器均返回 0，递归导入的测试与产品输入前后指纹一致：

| 运行器 | 实际结果与边界 |
| --- | --- |
| `test_sumh_directory_stream.py` | 目录流场景通过；夹具使用当前生产 payload/view/read-view，保留原有游标与复制错误检查 |
| `test_sumh_runtime_state.py` | 运行控制状态通过；ARM32、ARM64 的运行 UAPI 对象编译通过 |
| `test_sumh_directory_names.py` | 名称索引、MERGE、虚拟名称/迭代/查找及 vnode 查找六组通过 |
| `test_sumh_lookup_compat.py` | 实际查找函数配合模型声明/调用者/快照检查通过；不兼容声明按预期拒绝；启动连接为源码检查 |

命令、退出码、日志及其 SHA-256 见 `out/batch34-adapter-resumed-manifest.json`；范围解释见 `out/batch34-root-adapter-report.md`。此前两个依赖测试编译失败来自 `merge_snapshot.specialized` 的旧夹具，更新后本次通过。早期 `out/batch34-adapter-manifest.json` 及含 `final` 字样的日志仍是失败的开发证据，未改写为通过。

同日执行 `python -B out/batch34-run-adjacent.py --label resumed --checks source_capture vfs_helpers unicode_guard fop_lifetime rule_inventory`，五个运行器均通过，输入指纹一致。包括源捕获、VFS 辅助、Unicode 防护、结构化规则清单及其 ARM32/ARM64 UAPI 编译，另复用 FOP 的 25 个正向场景和 1,000 次模型循环；没有重复第三十二批未变化的故障变异。证据见 `out/batch34-adjacent-resumed-manifest.json`。这些运行器与前四个为不同入口，不将相同共享底层行为重复计为新的归属场景。

同日执行 `python -B out/batch34-run-legacy-adapters.py --label stable`，source_identity、merge_install、core、rules、inject_claims、merge_snapshot 六个适配运行器均通过，完整输入指纹一致。core 覆盖两种模型 idmap 声明各 30 项检查；注入归属回归保留 49 组场景、1,000 次模型循环及 25 个成功编译后命中指定断言的行为变异。原始注入测试随当前接口适配，不将这些已有场景计为本批新 DH 场景。证据见 `out/batch34-legacy-stable-manifest.json` 及 `out/batch34-legacy-stable-injection-manifest.json`；未重跑第三十三批封存脚本或覆盖其历史产物。

专用 DH 归属运行器已完成最终主机执行：8 个实际源码测试视图、1,000 次准备/提交/撤销/排空模型循环、28 个成功编译并命中唯一指定断言的行为变异，以及 8 个保留旧实现的反例均符合预期。共 44 个执行项；另有 5 个提取器防护检查，覆盖缺失/重复函数定义、缺失/重复插入位置和声明与定义区分。源文件、传递导入的测试、CI 与生成产物均记录 SHA-256，执行前后输入一致。见 `out/batch34-tests-report.md` 及 A2 更正记录 `out/batch34-tests-a2-report.md`；最终执行证据为 `out/batch34-tests-a2-manifest.json` 与 `out/batch34-tests-a2-final.log`。

新场景包括 CLEAR 排空已撤销资源期间通过真实 SU 入口发布独立绑定；通过实际递归 materializer 重应用时，在父目录重绑定及嵌套失败后保留旧子项和先前成功的兄弟项；真实 FOP 已快照的 DH 回调在逻辑撤销后仍受 SRCU 保护；以及普通删除无需 CLEAR 即回收末次 SOP 引用。完整 R01–R24、L01–L16 对照和模型服务边界见测试报告，不声称穷尽所有调度或组合。独立审阅发现早期 A2 场景在外层目录观察就返回错误，未覆盖其声称的嵌套子项替换；已将错误延后到真实递归捕获替换叶节点之后，并用精确捕获和所需父目录失败次数证明到达该路径。更正后 44 项全部重新执行通过，原证据保留。

专用运行器已注册到 CI。注册后仅重新执行输入发生变化的注入归属回归，全部通过，当前证据为 `out/batch34-injection-ci-final-manifest.json`；原六组记录保留为当时的有效执行。CI 辅助工具的 7 项检查通过，见 `out/batch34-adjacent-ci-final-manifest.json`。

最终主机生成源码的 8 个视图全部通过 ARM64 模型对象编译，其中 6 个不含生产 SU 的视图也通过 ARM32 补充模型编译，共 14 个对象。SU 与 bootstrap 的 ARM32 项明确排除：`kernel/Kbuild` 限定产品为 ARM64，修改前已有的 SU bit-63 `unsigned long` 初始化不适用于 ARM32。首次尝试的失败日志保留在 `out/batch34-arm-evidence/su-arm32.log`，不计为通过。最终矩阵、排除原因及指纹见 `out/batch34-arm-evidence-a2-final/manifest.json`。ARM32 补充对象和 UAPI 编译不表示支持 ARM32 内核。

独立测试审查已接受，见 `out/batch34-review-tests.md` 和 `out/batch34-review-tests-hashes.json`。审阅独立重建了 44 份生成源码并核对全部输入、产物和指定失败断言；最终批次核对记录为 `out/batch34-final-manifest.json`。上述检查不证明真实内核锁、RCU、调度、缓存或观察者行为。

## 保留门禁

- 首次发布的原生缓存边界、atomic_open/FUSE READDIRPLUS 入口、虚拟后代缓存刷新仍需实现；本批未新增缓存收敛承诺。
- 旧 inode 标志、spoof/xattr、完整 MERGE 层级与公开 owner/generation 事务仍待完成；控制器全局清理与部分发布限制继续存在。
- 源引用固定不等于内容快照，原子链接配对、ABA 和检查后的路径变化仍未解决。
- 未执行完整内核编译/链接、四个声明 KMI、CFI、真实并发、设备观察者或性能验证。已有构建环境调查见第三十三批记录，宿主或 ARM 模型对象不能替代这些项目。
- 全部 54 项任务目标保持进行；本批主机验收完成后继续剩余实施与设备/发布验收。
