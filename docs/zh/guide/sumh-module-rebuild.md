# SUMH 模块重建记录与失败处理

第二十四批为已有启动挂载、hot-mount/hot-unmount 和符合条件的 config apply 增加重建记录及失败保护。它仍使用旧内核的全局重置与逐条规则接口；**没有实现模块 owner 事务，也没有开放 F06 的 `module apply` 草案命令**。

## 查询一次重建发生了什么

**目的：** 重建失败或响应丢失后，先取得当次计划与已确认的调用范围，避免反复重建。

**适用条件：** 控制器 `api meta` 返回 `module_rebuild_status:true`、`module_rebuild_status_schema:1`。该能力属于控制器，不能由内核协议版本推断。需要已有 root 授权。

**入口与步骤：** 使用精确本地命令，不增加 daemon 包装或 `--channel`。它不启动 daemon、不恢复配置、不调用内核、不创建目录或锁文件。

```sh
/data/adb/ksud sumhp module rebuild-status
```

保存完整 JSON，查看 `read_status`、`intent_read_status`、`boot_id`、`latest`、`acknowledged` 和 `rebuild_admission`。`latest.id` 是内部重建 ID；外层 `operation show` 的 ID 是命令 ID，两者没有自动绑定，不得互换。

**期望观察：** `latest.plan` 保存本次编译后的有序 ADD/MERGE 与 HIDE 定义，`module_ids` 是选中的模块。`acknowledged` 是最近一次全部规则调用返回成功的计划；新尝试尚未完全确认时保留旧记录。首次无记录为 null，不能理解为旧运行集合为空。新尝试完整确认后旧记录被替换，这不是永久历史或配置恢复点。

| 记录或状态 | 能确认的事实 | 不能推断的结论 |
| --- | --- | --- |
| `pending` | 写前意图已留存，最终结果没有确认 | 一定未调用内核、已成功或失败 |
| `partial_or_unknown` | 记录了调用停止的位置、返回错误和成功前缀 | 返回错误的调用没有副作用、旧集合完整保留 |
| `acknowledged` | reset、全部 ADD/MERGE/HIDE 调用均返回成功 | 当前 VFS、owner、Maps、特性或 boot marker 已核实 |
| 同启动 intent 仍存在 | 最终回执或意图清除没有完成确认，同启动拒绝再重建 | 可删除记录后安全重试 |
| `allowed_by_retained_history_only` | 读取到的历史没有要求阻止新尝试 | 已取得应用令牌或其他 owner 不受影响 |
| `unknown_blocked` | 格式、读取或 boot 上下文不可确认 | 没有发生重建 |

`reset_acknowledged`、`mappings_acknowledged`、`hides_acknowledged` 是返回成功的调用数量；`failed_stage` 与从 0 开始的 `failed_index` 定位失败调用。中断可能只留下 pending，此时不补造精确前缀。路径按 UTF-8 或 hex 保留原始字节，不修剪末尾空格和非 UTF-8 名称。

结果始终 `runtime_verified:false`、`owner_verified:false`、`atomic_runtime_publication:false`。历史和 intent 是分次读取的观察，不能用作后续写入授权；执行前会在控制器锁内重新检查。退出码 0 仅表示记录和 boot 上下文可读取，**不表示重建成功或允许重建**；读取错误返回 1，参数错误返回 2。

## 失败处理与撤销

**失败处理：** 首次失败立即停止后续规则写入，不再执行第二次全局 clear，也不自动回放旧计划。同启动有未解决的 pending、partial 或 intent 时，已有重建入口拒绝再执行；没有 force、自动续跑或删除记录解锁的恢复流程。保留记录，核对保存配置、[运行规则](sumh-runtime-state.md)、日志与普通应用观察。按明确配置安排下一次正常启动；重启只建立新的运行上下文，不能把旧结果变成成功或修复错误配置。

内核调用前持久保存候选计划及独立 intent；最终回执耐久写入并读回后才移除 intent。即使最终文件 rename 已完成但 fsync/读回失败，intent 仍阻止重复重建。历史/intent 损坏、未来格式、读取失败、容量不足在 reset 前拒绝执行，保留文件排查。

两种“丢失回复”分别处理：内层执行或最终回执未确认会留下阻止标记；若内层已确认回执并移除 intent，之后外层 namespace 回复丢失，调用者仍报告部分/未知并保留所请求的热启停选择，但查询可取得独立内层证据。后一种情况允许以后显式新请求，当前调用不会重试。内层回执不证明 active marker、功能恢复或外层命令收尾成功。

**撤销方式：** 查询无需撤销。只有确认内核尚未修改的失败才恢复旧热启停选择；一旦可能发生修改，保留请求标记并报告失败，不声称运行状态回滚。恢复旧配置前审查原定义和现在的源文件，在对应入口保存并按支持的阶段生效；不直接执行留存计划，不用全局 clear 充当单模块撤销。

## 实现边界

- 第三十三批为映射/MERGE 的注入路径索引与精确 inode 标记增加内部规则归属，替换、DEL、CLEAR 同步撤销，旧回收任务只释放资源。仅有派生注入键的 DEL 返回 EBUSY，且在目录挂接和隐藏位修改前拒绝。新的注入准备错误会传播；部分物化、旧全局 reset 与其他派生状态的限制仍在。使用边界见[注入目录归属](sumh-module-rules.md#注入目录归属与撤销边界)，集成验证进度见[第三十三批记录](../../plans/SUMH-implementation-validation-batch33-2026-10-17.md)。

- 第三十二批让目录遍历的 FOP inode 缓存独立持有 superblock 引用，安装失败和退休时先释放 inode，再释放 superblock。该引用不增加 mount 引用，但缓存可能把文件系统最终关闭推迟到已有清理/停止边界；运行清理保留仍有专用客户端的条目，共享操作模板继续保留至退出。详见[验证记录](../../plans/SUMH-implementation-validation-batch32-2026-10-17.md)。FOP 缓存与第三十三批注入引用相互独立，仍未实现按模块精确撤销或全局事务，也没有新增用户清理命令。
- 第二十七、二十八批内核为 ADD/MERGE 固定源引用，非链接子项相对持有父目录解析，并在迭代锁外执行物化；第三十一批增加链接跟随后 nofollow 检查，发现仍可观察的对象替换返回 ESTALE，其他查询错误原样返回。[详细边界](sumh-module-rules.md#源对象身份与更新边界)包括尚未解决的 ABA/原子配对、可变目录内容、marker 检查和物化预算错误。这没有固定控制器计划中的源树，也没有改变下面的全局 reset 和部分发布限制；E2BIG/EIO/ESTALE 等错误仍可能对应部分发布。
- 第一次旧全局 reset 仍影响整个 SUMH 集合，包括用户隐藏绑定、构建覆盖、开关、xattr/kstat 和目录挂接。Maps 列表与启用状态须分开理解。本批消除失败后的重复破坏，不保证保留其他 owner。
- 文件锁仅序列化配合此记录协议的控制器重建，不能阻止其他进程直接调用旧 ioctl。boot 模块名单不是内核 owner 证据。
- 计划保存路径、顺序和模式，不固定 inode、源内容、配置 revision 或运行 generation。不能凭旧路径恢复旧内容，既有 FD 与新 open 需分别验证。
- schema 1 的 `module_rebuild_history.json`、`module_rebuild_intent.json` 位于控制器私有数据目录；完整历史最多 512 KiB、单计划最多 4096 个编译动作、intent 最多 1 KiB。执行前预留终态容量；已有模块/Maps 的 256 条及 56 KiB IPC 限额不变。
- 查询直接输出有界本地 JSON，不能放入普通 56 KiB daemon 回包。Manager 尚无专用重建记录页，原热操作入口保留错误提示。

源码见[记录接口](../../../userspace/ksud/sumhp/src/mount/module_rebuild.hpp)、[格式](../../../userspace/ksud/sumhp/src/mount/module_rebuild_model.cpp)、[存储与执行](../../../userspace/ksud/sumhp/src/mount/module_rebuild.cpp)、[namespace 桥接](../../../userspace/ksud/sumhp/src/mount/sumh.cpp)和[后端编排](../../../userspace/ksud/sumhp/src/mount/backend.cpp)。测试与待设备验收见[第二十四批验证](../../plans/SUMH-implementation-validation-batch24-2026-10-17.md)。
