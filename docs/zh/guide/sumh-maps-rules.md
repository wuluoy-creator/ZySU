# SUMH 按路径管理 maps 规则

SUMH 的 maps 路径规则保存两个文件路径：需要匹配的文件，以及提供替换身份的文件。应用时，SUMHP 在 init 挂载命名空间中读取两者当前的 inode/device，将替换文件的路径和身份交给已有的 maps/smaps 过滤器。重启后重新解析路径，不保存容易失效的 inode。

本功能仅调整 SUMH 既有策略范围内的 maps/smaps 文本，不重定向文件读取，也不改变 stat、文件内容或访问权限。SUMH 虚拟映射本身已有的身份投影仍由原来的 vnode 机制处理。

## 管理器与 CLI

管理器在 SUMH 的 Maps 设置中显示已保存的路径规则，支持添加、编辑、删除和重新应用。编辑目标路径或替换文件只提交一次配置，再整批替换受管规则，不需要先删除旧规则；编辑时不把原规则算作重复项，达到 256 条上限后仍可修改已有规则。各条规则的草稿分别保留，保存失败可继续修改。路径规则需要内核报告 `managed_maps` 能力；已有数字规则入口仍可用于当前运行时。

以下示例中的文件需按设备情况替换，且两者都必须是可解析的普通文件：

```sh
# 检查内核能力
/data/adb/ksud sumhp api features

# 保存规则并尝试应用；相同 target-path 会更新其 replacement-path
/data/adb/ksud sumhp sumh maps add-path \
  /data/local/tmp/maps-target.so /system/lib64/liblog.so

# 查看保存的定义（不表示已经生效）
/data/adb/ksud sumhp sumh maps list-paths

# 开启 maps 过滤并保存开关
/data/adb/ksud sumhp config merge-json '{"enable_maps_spoof":true}'
/data/adb/ksud sumhp config apply

# 文件就绪、替换或重新挂载后，重新解析并应用
/data/adb/ksud sumhp sumh maps apply

# 删除一条保存的定义并重新应用剩余规则
/data/adb/ksud sumhp sumh maps remove-path /data/local/tmp/maps-target.so
```

路径必须是绝对路径，不含 NUL、回车或换行；空格和 Unicode 原样保存。目标路径的 UTF-8 长度小于 4096 字节，替换路径小于 256 字节。最多保存 256 条规则，同时规则数组经双层 JSON 编码后的总大小不得超过 56 KiB，为现有控制器请求的包装及其他设置保留空间；长路径、引号和反斜杠会更早达到总大小限制。同一目标路径只能保存一次；不同目标路径若解析到相同 inode/device（例如硬链接或路径别名），整批应用会失败，避免隐含的覆盖顺序。

配置沿用 SUMHP 的配置文件和原子写入流程：

```json
{
  "enable_maps_spoof": true,
  "maps_rules": [
    {
      "target_path": "/data/local/tmp/maps-target.so",
      "replacement_path": "/system/lib64/liblog.so"
    }
  ]
}
```

## 保存、应用与恢复

保存定义与应用内核规则是两步。CLI 添加/删除成功保存后返回 `saved: true`，`applied` 表示本次内核规则替换是否成功；应用失败时退出码非零，并保留定义供重试。管理器继续使用已有的保存/应用结果反馈，列表中的“已保存”不等同于“已生效”。

SUMHP 先解析、校验全部路径，再通过一次 ioctl 原子替换受管 maps 规则。任一路径不存在、不是普通文件、身份重复或内核拒绝请求时，已安装的受管规则保持原样，不会先清空再逐条添加。配置损坏或读取失败也不会被当成空列表。删除后若剩余路径无法解析，旧规则同样保留，应修复路径并重新应用；可保存空 `maps_rules` 后应用以移除全部受管规则。

开启过滤后的配置应用、boot-completed，以及启动完成后的控制器重启会重新解析路径。启动前不抢先读取尚未完成挂载的文件；安全模式跳过自动恢复。使用外部 MetaModule 时仍可以恢复 maps 定义，因为它们不负责模块挂载。运行期间文件再次变化后需手动重新应用；本功能不持续监控文件替换。

关闭 Maps 开关会停止过滤并保留定义，配置应用时清除内核中的受管规则，不会因已停用规则的文件缺失而阻止关闭。`maps apply` 遵循保存的开关：开启时重新解析、安装并启用过滤，关闭时停用过滤并清除受管规则。添加/删除路径命令仍会立即尝试更新受管部分，不更改过滤开关。

配置应用、CLI 的 `maps apply` 和控制器启动恢复共用同一应用流程，先解析并安装规则，再启用过滤。普通配置应用中，路径解析或规则替换失败时保留原 Maps 开关与规则，不再关闭其他已应用的功能。模块重放仍需先重建模块视图；若重建后 Maps 应用失败，保留已重建的模块规则并返回失败供重试，不再二次清空模块。规则替换和开关设置是两个内核请求：若规则替换成功但开关设置失败，新规则已安装，仍会报告失败供重试。管理器的“应用已保存配置”复用现有配置应用操作；已有待应用提示时只显示提示中的重试入口。

原有 `maps add <target-ino> <target-dev> <spoof-ino> <spoof-dev> <path>` 仍是临时数字规则：同一 inode/device 再次添加会更新规则。匹配时精确 device 优先于 `target-dev=0` 通配规则，同一匹配键下临时规则优先于受管规则。数字 device 使用内核的 `major << 20 | minor` 编码；路径入口由控制器完成转换。

`maps clear` 清空当前内核中的所有 maps 规则，不删除保存的路径定义；它们会在下次应用或启动恢复时重新安装。受管规则的添加、删除及重试只替换受管部分，保留临时数字规则。

## 与参考项目的能力对应

设计参考 [susfs4ksu-module，提交 c6a1c065](https://github.com/sidex15/susfs4ksu-module/tree/c6a1c065efff9eaead049acf529a10bc93d34e75) 的能力与使用场景，未引入其脚本或可执行文件。主要参考其 [boot-completed.sh](https://github.com/sidex15/susfs4ksu-module/blob/c6a1c065efff9eaead049acf529a10bc93d34e75/boot-completed.sh) 中按路径配置和启动恢复的用法。

| 参考能力 | SUMH 中的对应与本轮处理 |
| --- | --- |
| 路径隐藏、等待路径出现 | 复用现有受管隐藏、绑定状态和内核事件重试 |
| 按模块选择路径行为 | 补齐已有模块规则的隐藏入口；模式与完整规则一次原子保存，详见[模块路径规则](sumh-module-rules.md) |
| maps 自定义规则与启动恢复 | 本轮补齐路径定义、持久化、重新解析、原子替换及管理器入口 |
| 重定向、目录合并 | 继续使用 SUMH 的虚拟文件与 vnode 身份投影；通用用户重定向的归属管理不在本轮开放 |
| uname release/version | 复用已有 kernel-build 设置、原值查询与应用阶段 |
| 挂载、statfs、OverlayFS xattr | 继续使用既有视图与挂载控制器 |
| 静态 kstat | 旧接口保留零值语义；受管路径规则现提供独立配置、整批替换与启动恢复，详见 [Kstat 路径规则](sumh-kstat-rules.md) |
| cmdline、avc、legacy sus_su | 未为对齐开关数量加入专用实现 |

功能归属保持明确：Maps 路径规则负责 proc 文本，虚拟文件映射负责实际文件视图，显式 kstat 负责独立的属性规则。不会给每条虚拟映射额外生成 kstat 规则，也不会另建用户重定向引擎。现有模块映射表按路径管理，开放通用用户重定向前仍需补齐规则归属、冲突拒绝和模块重放保护。

模块路径规则使用现有模块配置和编译流程，用户隐藏路径继续使用受管隐藏池。规则页区分保存定义与内核记录，已绑定隐藏路径去重展示；不以重复入口或重复计数增加功能数量。参考项目的全局 loop 扫描未直接移植：原生 SUMH 不依赖镜像 loop 挂载，Overlay/Magic 的存储应按控制器已有的挂载归属处理。

既有 kstat 的 `ADD/UPDATE` 共用一套临时规则存储。提供路径时，以该路径当前解析到的设备和 inode 为准，返回更新后的 `target_ino`；路径缺失或另一条路径规则已占用相同身份时拒绝更新，保留旧规则。未提供路径的数字规则保留 inode 通配语义；数字规则自身不安装 inode hook，只对已经进入现有属性处理流程的文件生效。每次更新发布完整新对象，旧对象在 RCU 读者退出后回收。旧文件仅残留 hook 标记时不会继续套用通用属性投影。原 ABI 的零值仍表示保留原字段（时间为零值对），`is_static` 仅保留兼容字段。受管路径规则使用独立快照，文件替换后需重新应用；两套规则均不自动跟踪路径变化。

## 验证

```sh
python3 scripts/ci/test_sumh_maps.py --cc clang
python3 scripts/ci/test_sumh_kstat.py --cc clang
python3 userspace/ksud/tests/run_sumh_host_tests.py --cxx g++
cd manager
./gradlew :app:testDebugUnitTest --tests '*SUMH*'
```

主机测试覆盖规则替换失败的原子性、匹配优先级、配置校验、设备编码、路径重新解析、客户端协议和管理器配置流。设备验证还需覆盖 maps/smaps、应用策略范围、冷启动、文件替换后的重新应用、重启恢复、安全模式，以及使用旧内核时的能力提示。主机模拟测试不等于真实内核或设备兼容性验证。
