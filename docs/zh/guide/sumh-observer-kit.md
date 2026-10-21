# SUMH 按观察者采样与比较

当前已实现独立测试 APK、共享原生采集器、root helper 和离线比较器。它们用于 F13/T03 的合成数据取证，普通进程、isolated service 和 root 各自读取自己的视图。构建和宿主测试记录另见[第二十三批验证](../../plans/SUMH-implementation-validation-batch23-2026-10-17.md)；真实设备、授权应用、共享 UID、工作资料和完整 KMI 矩阵仍待验收。

这套工具不会生成或应用 SUMH 规则。现有受管集合尚不能提供完整的独立测试 owner，不能以全局 clear、替换生产 Maps 集合或修改授权来补这个缺口。F13 的自动布置和仅清理测试所有者仍是待办。

## 1. 目的与适用条件

用同一份精确请求，把三个进程的文件内容、目录项、stat/fstat、符号链接、Maps、挂载信息和身份记录在独立样本中，再与明确声明的期望比较。root 与普通应用可能适用不同策略；角色标签不是授权证明。

使用专用、可恢复的 ARM64 Android 12/API 31 或更高测试设备。测试 APK 包名为 `com.zying.zysu.sumhobservertest`，仅 debug/testOnly 构建，不集成到正式 Manager。无网络、存储、包查询权限或共享 UID；isolated service 不导出。安装、运行和 root shell 均由测试人员明确执行，本批本地构建没有执行设备安装。

构建入口和固定工具版本见[工具 README](../../../tests/sumh-observer/README.md#build)。内核作用范围背景见[能力中心](sumh-capabilities.md)、[行为验证](sumh-validation.md)、[模块规则](sumh-module-rules.md)和[Maps 指南](sumh-maps-rules.md)。

## 2. 创建固定合成数据并采样

1. 打开 **SUMH Observer Test**，选择“创建合成数据”。程序在自身 `files/sumh-observer/<随机 run ID>` 中独占创建标记、普通文件、Unicode 文件、一个目录/子文件、相对符号链接和 4096 字节 `map.bin`；`missing.txt` 保持不存在。失败的半成品保留，不覆盖原路径。
2. 导出请求 JSON。请求上限 32 KiB，所有观察者必须使用这份文件的**原始 UTF-8 字节**。格式化 JSON、添加换行或修改 context 都会改变摘要，必须作为另一份请求重新采集。
3. 分别采集普通进程和 isolated service，导出各自样本。每次采集最多 256 KiB；界面显示“已采集”不代表规则测试通过。
4. 将同版本 helper 和原样请求复制到设备，在已经授权的 root shell 运行下列命令，然后逐字节取回 `root-sample.json`。

```sh
/data/local/tmp/sumh-observer --request /data/local/tmp/observer-request.json > /data/local/tmp/root-sample.json
```

helper 会检查真实/有效 UID 均为 0，只在 stdout 输出完整 JSON，stderr 输出错误；非零退出不能作有效样本。路径仅允许固定包的用户私有目录或专用 `/data/local/tmp/sumh-observer/<run ID>`。helper 不创建目录、不运行 shell、不启动 daemon。普通应用不能仅靠导入请求获得其他用户目录的访问权限。

### 预期观察

| 记录 | 解释 |
| --- | --- |
| `identity`、`identity.start_ticks`、`identity.selinux` | 实际采样进程的 UID/GID/PID/TID、组、启动 tick 与 SELinux 标签，读取失败单独保留；不是 Binder 调用方身份 |
| `namespace.start/end.link/stat/fd` | 自身 mount namespace 的不同观测方法；前后不一致或不可读时不能声明稳定 |
| `fixture.marker` | 实际读取的标记；`fixture_manifest_sha256` 只是预期标记摘要，不能替代此结果 |
| `file.*`、`directory.entries`、`link.target` | 固定路径上的 fresh lookup/open、内容、目录原始顺序与相对链接；名称/内容使用十六进制字节，不作有损解码 |
| `map.path.*` | 通过当前路径打开专用文件，检查后只读私有 mmap，再在自身 Maps 中查对应地址区间 |
| `map.fd.*` | 普通进程传入的已打开只读 FD；isolated service 再次检查文件后采样，不能解释为它成功打开了路径 |
| `mount.info`、`mount.mounts` | 只保留固定合成目录祖先相关行，读取上限/截断单独报告 |
| `context` | 测试人员提供的 boot/config 关联字段，绝不是观测值或已应用配置证明 |

isolated service 的路径访问可能返回 EACCES，同时可以读取传入的 FD。这正是需要保留的两种不同能力。FD 委托失败时四个 `map.fd.*` 仍出现并标为未运行，界面记录委托失败说明。

Android `/data/user/0` 的系统别名可正常解析；最终 `filesDir` 及应用自有路径尾部使用固定锚点和 no-follow。`/data/local/tmp` 路线逐层 no-follow。未知条目、异常标记、文件类型或文件链接检查失败会停止依赖的内容采样，不能借测试入口采集任意文件。这些检查并不承诺跨调用原子性。

## 3. 为真实请求声明期望

下列命令只生成一个**可编辑的起始期望**，不读取样本来自动把实测设成期望。先将导出的请求命名为工作目录中的 `observer-request.json`，在仓库根目录执行。它检查普通/root 的标记、固定内容、Unicode 内容、链接目标和已知缺失文件，以及 isolated 委托 FD 的固定内容；这些检查不代表 SUMH 隐藏或重定向已生效。输出路径会被写入，需保留旧期望时使用新的文件名。

```sh
python tests/sumh-observer/host/make_expectations.py --request observer-request.json --role ordinary --role isolated --role root --binding none --out observer-expectations.json
```

角色须逐项明确选择；可只比较已采集的一个角色。生成器默认要求 `direct_boot`，上例明确选 `none`。将导出的样本保存为相应文件名，创建 `out` 输出目录后运行：

```sh
python tests/sumh-observer/host/compare.py --request observer-request.json --sample ordinary-sample.json --sample isolated-sample.json --sample root-sample.json --expectations observer-expectations.json --json-out out/observer-result.json --markdown-out out/observer-result.md
```

`required_binding: none` 不要求每个进程都能读取 boot ID，但仍核对 namespace 稳定性、角色/请求/版本等证据完整性，并拒绝相互矛盾的实际 boot ID。需要同次启动证明时改为 `direct_boot`，所有必需观察者都必须实际读到相同 UUID；不能用 root 值填补 isolated 的缺失字段。配置 revision 始终标为 `supplied_unverified`。

期望的 `field` 是 value 内的点分对象字段，不支持索引、表达式或脚本。所有输入拒绝重复键、未来版本、未知字段、错误类型和超出容量。完整字段与语义见[协议](../../../tests/sumh-observer/protocol/schema-v1.md)。

## 4. 比较结果、失败处理和撤销

| 结果 | 处理 |
| --- | --- |
| `match`，退出 0 | 仅所声明检查匹配；归档原请求、样本、期望、输出报告及 APK/helper 哈希，不扩大为整个设备或全部功能通过 |
| `mismatch`，退出 1 | 有可用的已完成观测且不符合期望；检查采样身份、实际规则和时间顺序，不改写样本 |
| `inconclusive`，退出 1 | 缺观察者、权限拒绝/截断/未运行、namespace 不稳定或绑定不足；保留原因，按失败入口显式重新采集 |
| `not_applicable`，退出 1 | 对应操作报告不支持；有任何必需检查无法执行时不能总体通过 |
| 输入/I/O 错误，退出 2 | 修正请求/样本配对、版本、路径或输出位置；不把解析失败转成测量成功 |

“路径变为不可见”必须用 `absent`，`expected: 2`（ENOENT）、`baseline_required: true`，并传 `--baseline-request` 和对应 `--baseline-sample`。基线需是兼容 fixture anchor、采集器、实际身份、同观察者/探针的成功结果。基线和当前样本不能复用同一个 invocation ID；`direct_boot` 比较还要求对应基线在当前采样开始前结束。这只证明采样区间次序，不证明中间配置事务。EACCES、EPERM、原来就不存在的文件和不完整扫描均不能证明隐藏。当前版不自动布置规则，规则实验需另行完成专用所有权和清理合同。

请求/结果超过上限会拒绝。proc 单次扫描最多 64 KiB，目录最多 256 项，固定文件读取最多 4 KiB。原生五秒预算只在系统调用之间检查，不可打断阻塞中的内核调用。APK 十秒超时将结果记为未知，不自动重试；尚未结束的本地 worker 会继续占用执行位置。旋转/重建 Activity 不重复提交；进程重建恢复已有证据，未完成操作标为中断；过期回调和过期导出不会覆盖新请求。

采集本身无需撤销内核状态。导出需要的证据后，在 **系统设置 → 应用 → SUMH Observer Test → 存储 → 清除存储** 清理这个独立测试应用。外部 helper、请求和报告按明确文件名分别移除。不运行全局 clear，不递归删除任意传入目录，也不声称清理测试人员在其他工具中另外创建的规则。

## 5. 后续验收

当前只交付观察工具链和本地验证。后续将补独立测试 owner 的布置/回收、普通/授权/root/isolated/共享 UID/多用户/工作资料矩阵，以及配置变更、源文件原子替换、旧 FD、新 open、mmap、写入入口和重启恢复。已有 FD 不承诺追溯撤权。设备结果要记录设备/固件、boot ID、实际身份、源码/工具链/产物指纹、配置证据和未测项，接入[54 项账本](../../plans/SUMH-implementation-progress-2026-10-16.md)。
