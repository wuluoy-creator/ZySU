# ksud command-line reference

`ksud` is ZySU's C++ userspace entry point for modules, boot images, kernel features and the built-in SUMHP controller. Installed devices use `/data/adb/ksud`; the manager also invokes its packaged `libruntime.so` executable, whose name is defined in `manager/ksud-library-name.txt`. Operations on modules, kernel state and partitions require the corresponding root/kernel access.

This reference follows [cli_args.cpp](../userspace/ksud/src/cli_args.cpp), [cli.cpp](../userspace/ksud/src/cli.cpp) and their implementations. Consult the help shipped with your binary when using another version:

```sh
ksud --help
ksud help boot-patch
ksud sumhp module set-mode --help
```

## Arguments and output

| Option | Meaning |
| --- | --- |
| `-h`, `--help` | Show help without executing the selected operation |
| `--color auto\|always\|never` | Diagnostic colors; default `auto` |
| `--verbose` | Include internal diagnostic logs |
| Top-level `-v`, `-V`, `--version` | Print the userspace and UAPI versions |

Global options work before or after commands and operands. The shared parser rejects unknown options, duplicate command options, missing values and extra operands. Use `--` before operands beginning with `-`, and `--option=-value` for option values beginning with `-`. Everything after the module filename in `insmod` is passed to the loader verbatim.

Shared-parser errors exit with status `2`; help exits with `0`. Runtime statuses depend on the command. Diagnostics go to stderr; results and JSON go to stdout, which is not uniformly JSON. Automatic color requires a terminal, nonempty `TERM` other than `dumb`, and no nonempty `NO_COLOR`. Explicit color selection takes precedence.

Embedded `busybox`, `magiskboot`, `bootctl`, `resetprop`, `mkbootfs` and `readelf` are dispatched before the shared parser when compiled in. For example, `ksud magiskboot --help` uses magiskboot's grammar. Invoking the binary as `su` selects the separate su parser; there is no `ksud su` subcommand.

## Modules and features

| Command | Purpose / options |
| --- | --- |
| `module install <ZIP>` | Run the module installer |
| `module list` | List modules as JSON, including prepared preinit state |
| `module preinit status` | Show module preinit preparation state and boot failure count |
| `module preinit reset` | Re-enable preinit after repeated failed boots |
| `module enable <ID>`, `module disable <ID>` | Change enabled state |
| `module uninstall <ID>`, `module undo-uninstall <ID>` | Mark removal or cancel the mark |
| `module action <ID>` | Run the module action script |
| `module config get <KEY>`, `module config list` | Read configuration for `KSU_MODULE` |
| `module config set <KEY> <VALUE>` | Save a value; `-t` / `--temp` selects temporary configuration |
| `module config delete <KEY>`, `module config clear` | Remove values; also accept `-t` / `--temp` |
| `feature list`, `feature get <ID>`, `feature check <ID>` | Inspect values and support |
| `feature set <ID> <VALUE>` | Set an unsigned 64-bit value at runtime |
| `feature set-save <ID> <VALUE>` | Set and persist a value |
| `feature load`, `feature save` | Apply saved values or save current values |

Module commands enter init's mount namespace. `module config` requires `KSU_MODULE` to contain the module ID, normally supplied by module scripts. Temporary values override persistent ones and are cleared during boot. Keys cannot contain `=` or line breaks; values cannot contain line breaks. Reboot to apply normal module enable/disable/removal changes to the running system.

## Userspace installation and boot images

`install [--magiskboot <PATH>] [--libadbroot <PATH>]` installs userspace components; it does not itself patch a boot image. `uninstall [--magiskboot <PATH>]` runs the uninstall flow. Boot patchers use embedded magiskboot; their `--magiskboot` argument and the uninstaller's equivalent are retained for compatibility.

`uninstall` removes userspace and modules, attempts boot restoration, uninstalls Manager and requests a reboot after about five seconds. It currently continues removal and reboot even if restoration fails. Use `boot-restore` for image restoration alone, and keep matching original images available before uninstalling.

```sh
ksud boot-info current-kmi
ksud boot-info supported-kmis
ksud boot-info available-partitions
ksud boot-info default-partition
ksud boot-info target-kmi --boot /path/to/boot.img
```

Other queries are `boot-info is-ab-device`, `boot-info slot-suffix [--ota]` and `boot-info target-kmi [--ota]`. `target-kmi` rejects combining `--boot` with `--ota`.

### Ramdisk patching

`boot-patch` uses the ksuinit/ramdisk path. Without `--boot`, it detects the device partition. Partition writes require `--flash`; otherwise it produces an image file.

| Option | Meaning |
| --- | --- |
| `-b`, `--boot <IMAGE>` | Input image instead of the detected partition |
| `-f`, `--flash` | Write the result to the selected partition |
| `-o`, `--out <DIRECTORY>` | Output directory, not an image filename |
| `--out-name <NAME>` | Output image filename |
| `-m`, `--module <KO>` | Use an explicit LKM instead of an embedded asset |
| `-k`, `--kernel <IMAGE>` | Replace the kernel |
| `-i`, `--init <FILE>` | Supply replacement init |
| `-u`, `--ota` | Target the inactive A/B slot |
| `--partition boot\|init_boot` | Select the partition |
| `--kmi <KMI>` | Override KMI for asset selection |
| `--backup` | Back up the stock image |
| `-s`, `--superkey <KEY>` | Set SuperKey |
| `--signature-bypass` | With SuperKey, skip the configured Manager's certificate check; its package restriction still applies |
| `--allow-shell` | Allow Android shell UID to request root with the default profile |
| `--no-preinit` | Skip module preinit actions |
| `--no-custom-rc` | Skip custom init.rc injection |
| `--force-feature <FEATURE=VALUE>` | Force a configurable feature at boot; repeatable |
| `--block-module <NAME[,NAME...]>` | Block kernel modules at boot; repeatable; default none |
| `--enable-adbd` | Enable the adbd root setup |
| `--adb-debug-prop <FILE>` | Embed adb debug properties |
| `--magiskboot <PATH>` | Compatibility argument; embedded tool is used |

For file patching, KMI is detected from the selected boot image, or from the
replacement kernel when `--kernel` is supplied. An `init_boot` image has no kernel;
pass `--kmi` with the matching firmware's KMI. Also pass `--kmi` if a replacement
kernel's version cannot be detected (for example, a compressed kernel).
`--kernel` requires a boot image that contains a kernel; it cannot add one to
`init_boot`. `--adb-debug-prop` reads the contents of the specified file.

Both patchers accept `--force-feature`, `--block-module`, `--no-custom-rc` and `--no-preinit`.
Ramdisk patching writes `/ksu_config`; direct LKM patching uses ZySU's embedded
512-byte ImgPatch block (version 4). Version 3 introduced forced features and module
blocking; version 4 adds preinit control. Older LKMs remain usable with their
original options, but unsupported new options are rejected before
writing if unsupported. Omitting these options clears previous overrides.

Forced features report `forced` from `feature check`, appear as `[FORCED]` in
`feature list`, and reject runtime writes. `feature load` and boot initialization
skip them; `feature save` preserves their previous saved values. Forced sulog
still starts its userspace logger. Fixed SUMH/security features and read-only
status IDs cannot be forced. See the [boot configuration guide](zh/guide/boot-config.md)
for available IDs and validation commands.

`shell_su_adb_only` (ID 6, off by default) adds an ancestry check when an already
authorized shell UID requests root. An ancestor must be in the adbd or ZySU root
domain. Detached sessions are denied; local wireless debugging through adbd is
still allowed. It does not grant Shell root. Local ID 5 remains
`webview_zygote_umount`.

Module names use letters, digits or underscores, up to 64 characters each and
255 characters for the complete list. Manager only applies its suggested list
after explicitly checking **Block kernel modules (risky)**. Blocking required
vendor modules (including `mkp` on some devices) can prevent boot; restore the
matching stock boot / init_boot image to recover. The native blacklist only
affects future loads and stays installed if ZySU is unloaded during that boot.

```sh
# Replace android15-6.6 with the KMI of the matching firmware's boot.img.
ksud boot-patch --boot /path/to/init_boot.img --kmi android15-6.6 --out /path/to/output --out-name patched.img
```

Select the image for the actual firmware and installation method. A manually supplied KMI does not make an arbitrary LKM compatible. See the [installation guide](guide/installation.md).

### Direct kernel-image injection

`boot-patch-v2` injects the LKM into a kernel image. Its two modes are:

```sh
ksud boot-patch-v2 --boot /path/to/boot.img --output /path/to/patched.img
ksud boot-patch-v2 --flash --ota
```

File mode requires `--boot` (`-b`) and `--output` (`-o`, or `--out`); here the output is a **file**. Device mode uses `--flash`, optionally `--ota`, and rejects explicit input/output paths. `--ota` requires `--flash`. This path needs a supported AOSP **boot image containing a kernel** and rejects `init_boot` and `vendor_boot` as patch targets.

Additional options are `--module` / `-m`, `--superkey`, `--signature-bypass`, `--allow-shell`, `--enable-adbd`, `--no-custom-rc`, `--no-preinit`, `--force-feature`, `--block-module`, `--magiskboot`, `--force` and `--no-reuse`. `--force` permits replacing an existing output, never an input. `--no-reuse` forces full kernel analysis instead of reusing compatible injection metadata. In both patch paths, `--signature-bypass` selects SuperKey-only authentication for the configured Manager package, normally `com.zying.zysu`; without `--superkey`, it emits a warning and is ignored. It does not bypass the package restriction or Linux module-signature verification.

When a nonempty SuperKey is requested, a module without a complete SuperKey configuration block causes patching to fail before output or flashing. Keys use 1–64 UTF-8 bytes without NUL characters; whitespace is preserved. Omitting the key writes signature-only configuration and overrides any compile-time key in a module that supports SuperKey.

`boot-restore` accepts `--boot` / `-b`, `--flash` / `-f`, `--out-name` and `--magiskboot`. It uses available restore data; retain the original firmware image for recovery.

`ramdisk-editor <CPIO>` and `boot-ramdisk-editor <SOURCE> <OUTPUT>` are binary protocol sessions, not shell editors. See [Ramdisk editor protocol](ramdisk-editor-protocol.md) for image and wire-format limits.

## SUMHP and mount backends

SUMHP is built into ksud. State lives under `/data/adb/ksu/sumhp`; logs are written to `/data/adb/ksu/log/sumhp.log`.

```sh
ksud sumhp config show
ksud sumhp api backends
ksud sumhp api meta --control-only
ksud sumhp module list --all
ksud sumhp module set-mode example.module magic
```

| Command group | Operations |
| --- | --- |
| `sumhp config` | `show`, `gen`, `merge-json <JSON>`, `apply`, `sync-partitions` |
| `sumhp api` | `system`, `storage`, `sumh`, `features`, `hooks`, `mounts`, `backends`, `meta [--control-only]` |
| `sumhp daemon` | `status`, `start`, `serve`, `ping`, `stop`, `call <COMMAND> [ARGS...]` |
| `sumhp module` | `list [--all]`, `configure <ID> <MODE> <RULES-JSON>`, `set-mode <ID> <MODE>`, `add-rule <ID> <ABSOLUTE-PATH> <MODE>`, `remove-rule <ID> <ABSOLUTE-PATH>`, `check-conflicts`, `normalize <PATH>` |
| `sumhp module` runtime operations | `add` / `hot-mount <ID>`, `delete` / `hot-unmount <ID>`, `mount-all`, `unmount` |
| `sumhp sumh` | `version`, `list`, `features`, `clear`, `hide-path <ABSOLUTE-PATH>`, `delete-rule <ABSOLUTE-PATH>`, `fix-mounts`, `hide-overlay-xattrs <ABSOLUTE-PATH>` |
| `sumhp sumh` feature controls | `mount-hide off\|on\|normal\|aggressive`, `maps-spoof off\|on`, `statfs-spoof off\|on` |
| `sumhp sumh maps` | `clear`, `add <TARGET-INO> <TARGET-DEV> <SPOOF-INO> <SPOOF-DEV> <PATH>` |
| `sumhp sumh maps` saved paths | `list-paths`, `add-path <TARGET-PATH> <REPLACEMENT-PATH>`, `remove-path <TARGET-PATH>`, `apply` |
| `sumhp sumh kernel-build` | `show`, `set <RELEASE> <VERSION>`, `reset` |
| `sumhp hide` | `list`, `apply`, `add <ABSOLUTE-PATH>`, `remove <ABSOLUTE-PATH>` |
| `sumhp debug` | `enable`, `disable`, `stealth enable\|disable` |
| `sumhp recovery` | `status`, `reset` |

Module modes are `auto`, `sumh`, `overlay`, `magic` and `none`; path rules also accept `hide`. A non-`auto` global `mount_backend` overrides individual module choices. `auto` follows **OverlayFS → Magic Mount → none**, selecting Magic Mount for files directly under a partition root, such as `system/build.prop`. SUMH is selected explicitly. Unavailable requested backends can resolve to fallback backends; inspect the actual plan and logs after boot.

Hot operations replay only modules assigned to SUMH in the current boot. Changing a mode does not migrate live OverlayFS/Magic Mount state; reboot for backend changes. `mount-all` is restricted to the boot path and runs at most once per boot. Recovery protection disables mounting after three unconfirmed boot attempts. Inspect `recovery status`, fix the affected modules, then `recovery reset` and reboot.

SUMH replay compiles the new module plan before clearing existing mappings; unsupported trees, metadata errors and conflicting path aliases fail before reset. Installation errors after reset still use partial-rule cleanup. Hot-operation state markers must be written successfully before replay. A failed mapping rebuild restores the previous marker or reports a separate restoration error; a Maps restoration failure after mappings have committed retains the requested marker and reports partial completion. Mount commands reject damaged or unreadable saved configuration; missing configuration alone permits defaults. Configured `partitions` extend the built-in partition set without duplicating it or replacing the defaults.

`module configure` atomically saves a module mode and its complete rule array, for example `[{"path":"/system/app/Example","mode":"hide"}]`; `[]` clears that module's rules. Single-rule commands use the same locked store. Legacy modes and rule arrays are imported into the existing `module_rules.json` as `{ "mode": "sumh", "rules": [...] }` entries. Invalid input or persistence failures preserve the old configuration. Path rules only affect modules using SUMH; path-level `overlay` and `magic` remain compatibility values that map through SUMH. Module paths must be canonical absolute paths, with literal whitespace preserved, no NUL/CR/LF, and fewer than 4096 UTF-8 bytes. Each module allows 256 rules and a 56 KiB double-encoded JSON budget. See [module path rules](zh/guide/sumh-module-rules.md).

`config merge-json` validates keys, types, enums, paths and numeric ranges. `config gen` writes defaults. `config apply` applies supported runtime settings; changing a mount layout still requires reboot. For kernel build overrides, either `set` operand may be `default`, but not both; `reset` restores both. See [kernel build overrides](zh/guide/sumh-kernel-build.md).

The parser still accepts `mount-hide aggressive`, but the current kernel rejects this retired mode and does not advertise its capability. Applying an older saved aggressive configuration falls back to normal mode. Direct feature controls affect runtime state; use saved configuration and `config apply` for persistent settings.

Daemon control requests use a 5-second client timeout; other requests use 60 seconds. Timeout does not cancel an executing operation. Lost responses do not trigger replay; inspect state before retrying changes. Automatic startup/retry is limited to missing/refused socket connections. The newline-terminated transport limits requests to 64 KiB and client responses to 1 MiB; the daemon allows 5 seconds to receive or send a frame.

Legacy state migration runs before daemon startup or kernel-build restoration. Missing destination files are imported; existing destination data wins and conflicts remain in the old directory. Read, JSON or write errors stop migration. Default paths and the old backend name are updated while custom paths remain. Reboot after replacing an already-running controller or one that mounted modules during this boot.

## Partition and kernel tools

| Command | Options / purpose |
| --- | --- |
| `flash ak3 <ZIP>` | `--slot <SLOT>`, `--log <FILE>`, `--use-mkbootfs`, `--verbose` / `-v`; executes the archive installer |
| `flash ak3-info <ZIP>` | Inspect an archive |
| `flash image <IMAGE> <PARTITION>` | `--slot <SLOT>`; writes immediately |
| `flash backup <PARTITION> <OUTPUT>` | `--slot <SLOT>` |
| `flash list` | `--slot <SLOT>`, `--all` |
| `flash info <PARTITION>`, `flash kernel` | `--slot <SLOT>` |
| `flash slots`, `flash boot-info` | Query slot information |
| `flash map <SLOT>` | Map logical partitions |
| `flash avb [disable]` | Query AVB; `disable` modifies verification configuration |
| `insmod <KO> [PARAMS...]` | Load an LKM with kallsyms support; also `debug insmod` |
| `umount add <MOUNT>` | `--flags` / `-f <UINT32>`; register an unmount path |
| `umount del <MOUNT>`, `umount remove <MOUNT>` | Delete an unmount entry |
| `umount list`, `save`, `apply`, `clear-custom` | Inspect/persist custom unmount state |
| `kernel umount add <MOUNT>` | `--flags` / `-f <UINT32>` |
| `kernel umount del <MOUNT>`, `kernel umount wipe` | Modify the kernel unmount list |
| `kernel nuke-ext4-sysfs <MOUNT>`, `kernel notify-module-mounted` | Low-level integration |

`SLOT` accepts `a`, `b`, `_a`, `_b`; omission selects the current slot. `flash image` and `flash ak3` do not require an additional `--flash` flag.

Other command families are `sepolicy patch|check <POLICY>`, `sepolicy apply <FILE>`, `profile get-sepolicy|set-sepolicy|get-template|set-template|delete-template|list-templates`, `su-path get|set|reset`, and `debug set-manager|su|version|info|mark|sulogd`. Consult subcommand help for operands. `su-path set <ABSOLUTE-PATH>` supports `--json`.

`post-fs-data`, `services`, `boot-completed`, `initrc refresh` and `sulogd` are boot/runtime integration entry points.

## Script waits and interactive shells

Common, metamodule and module `post-fs-data`/`post-mount` scripts share one 35-second waiting deadline. Timeout stops waiting, not the script. Later scripts still start. Service and boot-completed scripts are not waited for. Action, uninstall and metamodule mount scripts can wait until completion, so 35 seconds is not a bound on the entire boot handler.

Interactive system mksh and bundled BusyBox ash use `/data/adb/ksu/bin/shellrc.sh` when readable. Personal settings belong in `/data/adb/ksu/.shellrc`, sourced last. An explicit `ENV`, exported `PS1`, `su -p` or custom shell keeps its configuration; `su -c` does not receive interactive startup. `/data/adb/ksu/.ksurc` is feature configuration, not the personal shell startup file.
