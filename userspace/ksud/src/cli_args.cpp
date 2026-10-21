#include "cli_args.hpp"
#include "../sumhp/include/sumhp/config.hpp"
#include "../sumhp/src/core/user_hide_input.hpp"
#include "terminal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <limits>
#include <numeric>
#include <string_view>
#include <utility>
#include "core/json.hpp"

namespace ksud {
namespace {

thread_local bool quiet_validation = false;

struct Option {
    std::string_view name;
    std::string_view alias;
    std::string_view value;
    std::string_view default_value;
    std::string_view description;
};

struct Command {
    std::string_view path;
    std::string_view description;
    std::string_view usage;
    size_t minimum = 0;
    size_t maximum = 0;
    std::vector<Option> options;
    bool forward_options = false;
};

constexpr size_t kTrailingArgs = std::numeric_limits<size_t>::max();
const Option kTemp{"--temp", "-t", "", ""};
const Option kSlot{"--slot", "", "a|b|_a|_b", ""};
const Option kFlags{"--flags", "-f", "UINT32", ""};
const Option kJson{"--json", "", "", ""};
const Option kOta{"--ota", "-u", "", ""};
const Option kMagiskboot{"--magiskboot", "", "PATH", ""};

const std::vector<Command>& commands() {
    static const std::vector<Command> specs = {
        {"", "ZySU userspace daemon", "<COMMAND>"},
        {"module", "Manage modules", "<COMMAND>"},
        {"module install", "Install a module archive", "<ZIP>", 1, 1},
        {"module uninstall", "Mark a module for removal", "<ID>", 1, 1},
        {"module undo-uninstall", "Cancel module removal", "<ID>", 1, 1},
        {"module enable", "Enable a module", "<ID>", 1, 1},
        {"module disable", "Disable a module", "<ID>", 1, 1},
        {"module action", "Run a module action", "<ID>", 1, 1},
        {"module list", "List modules as JSON", ""},
        {"module config", "Manage configuration in a module's environment", "<COMMAND>"},
        {"module config get", "Read a configuration value", "<KEY>", 1, 1},
        {"module config set", "Write a configuration value", "<KEY> <VALUE>", 2, 2, {kTemp}},
        {"module config list", "List configuration values", ""},
        {"module config delete", "Delete a configuration value", "<KEY>", 1, 1, {kTemp}},
        {"module config clear", "Clear configuration values", "", 0, 0, {kTemp}},
        {"feature", "Manage kernel features", "<COMMAND>"},
        {"feature get", "Read a feature value", "<ID>", 1, 1},
        {"feature set", "Set a feature value", "<ID> <VALUE>", 2, 2},
        {"feature set-save", "Set and persist a feature atomically", "<ID> <VALUE>", 2, 2},
        {"feature list", "List features", ""},
        {"feature check", "Check feature support", "<ID>", 1, 1},
        {"feature load", "Apply saved feature configuration", ""},
        {"feature save", "Persist current feature configuration", ""},
        {"install",
         "Install userspace components",
         "",
         0,
         0,
         {kMagiskboot, {"--libadbroot", "", "PATH", ""}}},
        {"uninstall", "Uninstall ZySU", "", 0, 0, {kMagiskboot}},
        {"insmod", "Load a kernel module with kallsyms access", "<KO> [PARAMS...]", 1,
         kTrailingArgs},
        {"post-fs-data", "Trigger post-fs-data", ""},
        {"services", "Trigger service scripts", ""},
        {"boot-completed", "Trigger boot-completed", ""},
        {"sepolicy", "Manage SELinux policy", "<COMMAND>"},
        {"sepolicy patch", "Apply a policy statement", "<POLICY>", 1, 1},
        {"sepolicy apply", "Apply policy from a file", "<FILE>", 1, 1},
        {"sepolicy check", "Check a policy statement", "<POLICY>", 1, 1},
        {"profile", "Manage app profiles", "<COMMAND>"},
        {"profile get-sepolicy", "Read an app's policy", "<PACKAGE>", 1, 1},
        {"profile set-sepolicy", "Write an app's policy", "<PACKAGE> <POLICY>", 2, 2},
        {"profile get-template", "Read a profile template", "<ID>", 1, 1},
        {"profile set-template", "Write a profile template", "<ID> <TEMPLATE>", 2, 2},
        {"profile delete-template", "Delete a profile template", "<ID>", 1, 1},
        {"profile list-templates", "List profile templates", ""},
        {"boot-info", "Inspect boot information", "<COMMAND>"},
        {"boot-info current-kmi", "Read the running kernel's KMI", ""},
        {"boot-info target-kmi",
         "Read the patch target's KMI",
         "",
         0,
         0,
         {kOta, {"--boot", "", "PATH", ""}}},
        {"boot-info supported-kmis", "List embedded KMIs", ""},
        {"boot-info is-ab-device", "Check A/B support", ""},
        {"boot-info default-partition", "Read the default boot partition", ""},
        {"boot-info available-partitions", "List boot partitions", ""},
        {"boot-info slot-suffix", "Read the slot suffix", "", 0, 0, {kOta}},
        {"ramdisk-editor", "Run a ramdisk editor protocol session", "<RAMDISK.CPIO>", 1, 1},
        {"boot-ramdisk-editor", "Edit a boot image's ramdisk", "<SOURCE.IMG> <OUTPUT.IMG>", 2, 2},
        {"umount", "Manage unmount paths", "<COMMAND>"},
        {"umount add", "Register an unmount path", "<MOUNT>", 1, 1, {kFlags}},
        {"umount del", "Delete an unmount path", "<MOUNT>", 1, 1},
        {"umount remove", "Alias for umount del", "<MOUNT>", 1, 1},
        {"umount list", "List unmount paths", ""},
        {"umount save", "Save the kernel unmount list", ""},
        {"umount apply", "Apply the saved unmount list", ""},
        {"umount clear-custom", "Clear custom unmount paths", ""},
        {"kernel", "Access kernel interfaces", "<COMMAND>"},
        {"kernel nuke-ext4-sysfs", "Hide ext4 sysfs entries", "<MOUNT>", 1, 1},
        {"kernel notify-module-mounted", "Report completed module mounting", ""},
        {"kernel umount", "Manage the kernel unmount list", "<COMMAND>"},
        {"kernel umount add", "Register an unmount path", "<MOUNT>", 1, 1, {kFlags}},
        {"kernel umount del", "Delete an unmount path", "<MOUNT>", 1, 1},
        {"kernel umount wipe", "Clear the kernel unmount list", ""},
        {"debug", "Developer tools", "<COMMAND>"},
        {"debug set-manager", "Set the manager package", "[PACKAGE]", 0, 1},
        {"debug insmod", "Alias for insmod", "<KO> [PARAMS...]", 1, kTrailingArgs},
        {"debug su", "Open a root shell", "", 0, 0, {{"-g", "--global-mnt", "", ""}}},
        {"debug version", "Read the kernel version", ""},
        {"debug info", "Read kernel compatibility and load information", ""},
        {"debug sulogd", "Launch the su log daemon", ""},
        {"debug mark", "Manage process marks", "<COMMAND>"},
        {"debug mark get", "Read process marks (0 means all)", "[PID]", 0, 1},
        {"debug mark mark", "Mark a process (0 means all)", "[PID]", 0, 1},
        {"debug mark unmark", "Unmark a process (0 means all)", "[PID]", 0, 1},
        {"debug mark refresh", "Refresh process marks", ""},
        {"flash", "Flash and inspect partition images", "<COMMAND>"},
        {"flash ak3",
         "Flash an AnyKernel3 archive",
         "<ZIP>",
         1,
         1,
         {kSlot,
          {"--log", "", "FILE", ""},
          {"--use-mkbootfs", "", "", ""},
          {"--verbose", "-v", "", ""}}},
        {"flash ak3-info", "Inspect an AnyKernel3 archive", "<ZIP>", 1, 1},
        {"flash image", "Flash an image to a partition", "<IMAGE> <PARTITION>", 2, 2, {kSlot}},
        {"flash backup", "Back up a partition", "<PARTITION> <OUTPUT>", 2, 2, {kSlot}},
        {"flash list", "List partitions", "", 0, 0, {kSlot, {"--all", "", "", ""}}},
        {"flash info", "Inspect a partition", "<PARTITION>", 1, 1, {kSlot}},
        {"flash slots", "Show A/B slot information", ""},
        {"flash map", "Map logical partitions for a slot", "<SLOT>", 1, 1},
        {"flash avb", "Show AVB/dm-verity status", "[disable]", 0, 1},
        {"flash kernel", "Read the kernel version", "", 0, 0, {kSlot}},
        {"flash boot-info", "Read boot slot information", ""},
        {"initrc", "Manage init.rc injection", "<COMMAND>"},
        {"initrc refresh", "Regenerate preinit modules.rc", ""},
        {"sulogd", "Run the su log daemon", ""},
        {"su-path", "Manage the persistent SUMH su path", "<COMMAND>"},
        {"su-path get", "Read the active su path", ""},
        {"su-path set", "Set and persist the su path", "<ABSOLUTE-PATH>", 1, 1, {kJson}},
        {"su-path reset", "Restore the default su path", ""},
        {"version", "Show version information", ""},
        {"boot-patch",
         "Patch a boot image",
         "",
         0,
         0,
         {{"--boot", "-b", "IMAGE", "", "Read this image instead of the detected boot partition"},
          {"--flash", "-f", "", "", "Write the patched result to the selected partition"},
          {"--out", "-o", "DIRECTORY", "", "Write the patched image into this directory"},
          {"--out-name", "", "NAME", "", "Name of the patched output image"},
          {"--module", "-m", "KO", "", "Use this LKM instead of an embedded one"},
          {"--kernel", "-k", "IMAGE", "", "Replace the kernel image"},
          {"--init", "-i", "FILE", "", "Replace init"},
          kOta,
          {"--partition", "", "boot|init_boot", "", "Select the boot partition"},
          {"--kmi", "", "KMI", "", "Override the detected KMI"},
          {"--backup", "", "", "", "Back up the stock image"},
          {"--superkey", "-s", "KEY", "", "Set the SuperKey"},
          {"--signature-bypass", "", "", "", "Relax LKM signature checking"},
          {"--allow-shell", "", "", "", "Keep a root shell available"},
          {"--no-custom-rc", "", "", "", "Skip custom init.rc injection"},
          {"--force-feature", "", "FEATURE=VALUE", "", "Force a configurable feature (repeatable)"},
          {"--block-module", "", "NAME[,NAME...]", "",
           "Block kernel modules (repeatable; default none)"},
          {"--enable-adbd", "", "", "", "Run adbd as root"},
          {"--adb-debug-prop", "", "FILE", "", "Embed adb debug properties from this file"},
          kMagiskboot},
         true},
        {"boot-restore",
         "Restore a boot image",
         "",
         0,
         0,
         {{"--boot", "-b", "IMAGE", "", "Read this image instead of the detected boot partition"},
          {"--flash", "-f", "", "", "Write the restored result back to the partition"},
          {"--out-name", "", "NAME", "", "Name of the restored output image"},
          kMagiskboot},
         true},
        {"boot-patch-v2",
         "Patch a boot image with direct LKM injection",
         "",
         0,
         0,
         {{"--boot", "-b", "IMAGE", ""},
          {"--module", "-m", "KO", ""},
          {"--output", "-o", "IMAGE", ""},
          {"--out", "", "IMAGE", ""},
          kMagiskboot,
          {"--superkey", "", "KEY", ""},
          {"--force", "", "", ""},
          {"--flash", "", "", ""},
          {"--ota", "", "", ""},
          {"--allow-shell", "", "", ""},
          {"--no-custom-rc", "", "", ""},
          {"--force-feature", "", "FEATURE=VALUE", ""},
          {"--block-module", "", "NAME[,NAME...]", ""},
          {"--enable-adbd", "", "", ""},
          {"--signature-bypass", "", "", ""},
          {"--no-reuse", "", "", ""}}},
        {"sumhp", "Manage built-in SUMH and module mounts", "<COMMAND>"},
        {"sumhp version", "Show the built-in controller version", ""},
        {"sumhp operation", "Read bounded recorded command history without daemon startup",
         "<COMMAND>"},
        {"sumhp operation list", "List retained operation records", ""},
        {"sumhp operation show", "Read one retained operation record", "<ID>", 1, 1},
        {"sumhp operation result", "Read a retained typed resource result", "<ID>", 1, 1},
        {"sumhp operation wait", "Poll a recorded operation; never repeat or cancel it",
         "<ID> <TIMEOUT-MS>", 2, 2},
        {"sumhp operation prepare",
         "Reserve a bound operation ID without executing the command",
         "--input <ABSOLUTE-FILE|->",
         0,
         0,
         {{"--input", "", "ABSOLUTE-FILE|-", ""}}},
        {"sumhp operation execute",
         "Execute a retained prepared operation once",
         "<ID> --input <ABSOLUTE-FILE|->",
         1,
         1,
         {{"--input", "", "ABSOLUTE-FILE|-", ""}}},
        {"sumhp profile", "Manage inactive SUMH configuration profiles (exact local route)",
         "<COMMAND>"},
        {"sumhp profile list", "List inactive profiles and the bank revision", ""},
        {"sumhp profile show", "Read an inactive profile and its revision", "<ID>", 1, 1},
        {"sumhp profile export", "Export a portable profile as JSON", "<ID>", 1, 1},
        {"sumhp profile preview",
         "Compare selected saved resources without applying",
         "<ID> --resources <SCOPES>",
         1,
         1,
         {{"--resources", "", "SCOPES", ""}}},
        {"sumhp profile import-preview",
         "Validate and compare a portable profile without saving",
         "--input <ABSOLUTE-FILE|-> --resources <SCOPES>",
         0,
         0,
         {{"--input", "", "ABSOLUTE-FILE|-", ""}, {"--resources", "", "SCOPES", ""}}},
        {"sumhp profile capture",
         "Capture saved settings into an inactive profile",
         "--input <ABSOLUTE-FILE|->",
         0,
         0,
         {{"--input", "", "ABSOLUTE-FILE|-", ""}}},
        {"sumhp profile import",
         "Import into a new inactive profile ID",
         "--input <ABSOLUTE-FILE|->",
         0,
         0,
         {{"--input", "", "ABSOLUTE-FILE|-", ""}}},
        {"sumhp profile clone",
         "Copy an inactive profile with revision checks",
         "--input <ABSOLUTE-FILE|->",
         0,
         0,
         {{"--input", "", "ABSOLUTE-FILE|-", ""}}},
        {"sumhp profile rename",
         "Change profile name and note with revision checks",
         "--input <ABSOLUTE-FILE|->",
         0,
         0,
         {{"--input", "", "ABSOLUTE-FILE|-", ""}}},
        {"sumhp profile remove",
         "Remove an inactive profile with revision checks",
         "--input <ABSOLUTE-FILE|->",
         0,
         0,
         {{"--input", "", "ABSOLUTE-FILE|-", ""}}},
        {"sumhp config", "Manage mount configuration", "<COMMAND>"},
        {"sumhp config show", "Read configuration as JSON", ""},
        {"sumhp config gen", "Write the default configuration", ""},
        {"sumhp config merge-json", "Merge a JSON object into configuration", "<JSON>", 1, 1},
        {"sumhp config apply", "Apply the saved configuration", ""},
        {"sumhp config sync-partitions", "Discover partition candidates without saving", ""},
        {"sumhp config discover-partitions", "Discover extra partition candidates without saving",
         ""},
        {"sumhp config set-partitions",
         "Conditionally save selected extra partitions; reboot required", "<REVISION> <JSON-ARRAY>",
         2, 2},
        {"sumhp api", "Query controller state as JSON", "<COMMAND>"},
        {"sumhp api system", "Read system information", ""},
        {"sumhp api storage", "Read storage information", ""},
        {"sumhp api sumh", "Read SUMH information", ""},
        {"sumhp api runtime", "Read versioned SUMH runtime state (not observer visibility)", ""},
        {"sumhp api capabilities", "Read a bounded capability and scope summary without recovery",
         ""},
        {"sumhp api state", "Read saved revisions, live drift and the persistent last operation",
         ""},
        {"sumhp api inventory", "Read one bounded page of registered rule definitions",
         "[CATEGORY [INSTANCE_ID GENERATION CURSOR]]", 0, 4},
        {"sumhp api features", "Read SUMH features", ""},
        {"sumhp api hooks", "Read active kernel hooks", ""},
        {"sumhp api mounts", "Read mount state", ""},
        {"sumhp api backends", "Read available mount backends", ""},
        {"sumhp api meta",
         "Read module mount metadata",
         "",
         0,
         0,
         {{"--control-only", "", "", "", "Omit the full mount inventory"}},
         true},
        {"sumhp daemon", "Control the mount daemon", "<COMMAND>"},
        {"sumhp daemon status", "Read daemon status", ""},
        {"sumhp daemon start", "Start the daemon", ""},
        {"sumhp daemon serve", "Run the daemon in the foreground", ""},
        {"sumhp daemon ping", "Ping the daemon", ""},
        {"sumhp daemon stop", "Stop the daemon", ""},
        {"sumhp daemon call", "Send a validated SUMHP command to the daemon", "<COMMAND> [ARGS...]",
         1, kTrailingArgs},
        {"sumhp module", "Manage module mounting", "<COMMAND>"},
        {"sumhp module list",
         "List module mount state",
         "",
         0,
         0,
         {{"--all", "", "", "", "Include disabled modules"}},
         true},
        {"sumhp module set-mode", "Set a module's mount backend", "<ID> <MODE>", 2, 2},
        {"sumhp module configure", "Atomically save a module's backend and path rules",
         "<ID> <MODE> <RULES-JSON>", 3, 3},
        {"sumhp module add-rule", "Add a module path rule", "<ID> <ABSOLUTE-PATH> <MODE>", 3, 3},
        {"sumhp module remove-rule", "Remove a module path rule", "<ID> <ABSOLUTE-PATH>", 2, 2},
        {"sumhp module add", "Hot-mount a SUMH module", "<ID>", 1, 1},
        {"sumhp module delete", "Hot-unmount a SUMH module", "<ID>", 1, 1},
        {"sumhp module hot-mount", "Hot-mount a SUMH module", "<ID>", 1, 1},
        {"sumhp module hot-unmount", "Hot-unmount a SUMH module", "<ID>", 1, 1},
        {"sumhp module check-conflicts", "Find conflicting module paths", ""},
        {"sumhp module plan",
         "Preview production module planning without saving or applying",
         "[--input <ABSOLUTE-FILE|->]",
         0,
         0,
         {{"--input", "", "ABSOLUTE-FILE|-", ""}}},
        {"sumhp module explain",
         "Explain a path's production module plan contributions",
         "<ABSOLUTE-PATH> [--input <ABSOLUTE-FILE|->]",
         1,
         1,
         {{"--input", "", "ABSOLUTE-FILE|-", ""}}},
        {"sumhp module inspect", "Inspect an isolated developer module with production planning",
         "<ABSOLUTE-PATH>", 1, 1},
        {"sumhp module mount-all", "Mount enabled modules during boot", ""},
        {"sumhp module unmount", "Unmount the controller's mounts", ""},
        {"sumhp module normalize",
         "Normalize module layout or preview changes",
         "<PATH> [--dry-run]",
         1,
         1,
         {{"--dry-run", "", "", "Preview only"}}},
        {"sumhp sumh", "Control SUMH", "<COMMAND>"},
        {"sumhp sumh version", "Read the SUMH version", ""},
        {"sumhp sumh list", "Read SUMH rules", ""},
        {"sumhp sumh features", "Read supported SUMH features", ""},
        {"sumhp sumh clear", "Clear SUMH rules", ""},
        {"sumhp sumh hide-path", "Hide an absolute path", "<ABSOLUTE-PATH>", 1, 1},
        {"sumhp sumh delete-rule", "Delete a path rule", "<ABSOLUTE-PATH>", 1, 1},
        {"sumhp sumh fix-mounts", "Reorder mount IDs", ""},
        {"sumhp sumh hide-overlay-xattrs", "Hide OverlayFS attributes on a path", "<ABSOLUTE-PATH>",
         1, 1},
        {"sumhp sumh mount-hide", "Set mount hiding: off, on, normal, aggressive", "<MODE>", 1, 1},
        {"sumhp sumh maps-spoof", "Set maps spoofing: off or on", "<STATE>", 1, 1},
        {"sumhp sumh statfs-spoof", "Set statfs spoofing: off or on", "<STATE>", 1, 1},
        {"sumhp sumh kernel-build", "Manage persistent kernel build spoofing", "<COMMAND>"},
        {"sumhp sumh kernel-build show", "Read the effective kernel release and build", ""},
        {"sumhp sumh kernel-build set",
         "Set the kernel release and full build version (default keeps an original field)",
         "<RELEASE> <VERSION>", 2, 2},
        {"sumhp sumh kernel-build reset", "Restore the original kernel build", ""},
        {"sumhp sumh maps", "Manage maps spoofing rules", "<COMMAND>"},
        {"sumhp sumh maps clear", "Clear maps spoofing rules", ""},
        {"sumhp sumh maps add", "Add a maps spoofing rule",
         "<TARGET-INO> <TARGET-DEV> <SPOOF-INO> <SPOOF-DEV> <PATH>", 5, 5},
        {"sumhp sumh maps list-paths", "Read saved maps path definitions", ""},
        {"sumhp sumh maps preview",
         "Read saved Maps paths, current identities and submission history", ""},
        {"sumhp doctor",
         "Read one structured SUMH diagnostic scenario without recovery",
         "--input <ABSOLUTE-FILE|->",
         0,
         0,
         {{"--input", "", "ABSOLUTE-FILE|-", ""}}},
        {"sumhp doctor package", "Manage local diagnostic packages", "<COMMAND>"},
        {"sumhp doctor package list", "List retained diagnostic packages", ""},
        {"sumhp doctor package create",
         "Create a private diagnostic package; redact identities by default",
         "<ID> --input <ABSOLUTE-FILE|-> [--include-identifiers]",
         1,
         1,
         {{"--input", "", "ABSOLUTE-FILE|-", ""}, {"--include-identifiers", "", "", ""}}},
        {"sumhp doctor package preview", "Preview a complete retained diagnostic package", "<ID>",
         1, 1},
        {"sumhp doctor package export",
         "Export the exact reviewed package bytes to stdout",
         "<ID> --revision <REVISION>",
         1,
         1,
         {{"--revision", "", "REVISION", ""}}},
        {"sumhp doctor package remove",
         "Remove one retained diagnostic package with its reviewed revision",
         "<ID> --revision <REVISION>",
         1,
         1,
         {{"--revision", "", "REVISION", ""}}},
        {"sumhp sumh maps list-temporary",
         "Read one temporary Maps inventory page without recovery", "[INSTANCE GENERATION CURSOR]",
         0, 3},
        {"sumhp sumh maps remove",
         "Delete one temporary Maps key with an inventory token",
         "<TARGET-INO> <TARGET-DEV> --instance <ID> --generation <GENERATION>",
         2,
         2,
         {{"--instance", "", "ID", ""}, {"--generation", "", "GENERATION", ""}}},
        {"sumhp sumh maps add-path", "Save and apply a maps path definition",
         "<TARGET-PATH> <REPLACEMENT-PATH>", 2, 2},
        {"sumhp sumh maps remove-path", "Remove a saved maps path definition", "<TARGET-PATH>", 1,
         1},
        {"sumhp sumh maps apply", "Resolve saved paths and replace managed maps rules", ""},
        {"sumhp debug", "Control kernel diagnostics", "<COMMAND>"},
        {"sumhp debug enable", "Enable kernel diagnostics", ""},
        {"sumhp debug disable", "Disable kernel diagnostics", ""},
        {"sumhp debug stealth", "Set stealth mode: enable or disable", "<STATE>", 1, 1},
        {"sumhp hide", "Manage persistent path hiding", "<COMMAND>"},
        {"sumhp hide list", "Read persistent hidden paths", ""},
        {"sumhp hide apply", "Restore hidden paths after boot completion", ""},
        {"sumhp hide add", "Add an absolute path", "<ABSOLUTE-PATH>", 1, 1},
        {"sumhp hide remove", "Remove an absolute path", "<ABSOLUTE-PATH>", 1, 1},
        {"sumhp hide retry-path", "Retry a saved enabled absolute path", "<ABSOLUTE-PATH>", 1, 1},
        {"sumhp hide status", "Read paged saved definitions and sampled registration",
         "[REVISION CURSOR]", 0, 2},
        {"sumhp hide definitions", "Read paged persistent definitions without kernel queries",
         "[REVISION CURSOR]", 0, 2},
        {"sumhp hide export", "Export a complete portable definition document without mutation",
         ""},
        {"sumhp hide create", "Create a managed hide definition", "<REVISION> <JSON>", 2, 2},
        {"sumhp hide import-id", "Conditionally import one reviewed stable definition",
         "<ID> <REVISION> <JSON>", 3, 3},
        {"sumhp hide annotate", "Save definition name, note or group", "<ID> <REVISION> <JSON>", 3,
         3},
        {"sumhp hide enable", "Enable a saved definition", "<ID> <REVISION>", 2, 2},
        {"sumhp hide disable", "Disable a definition without deleting it", "<ID> <REVISION>", 2, 2},
        {"sumhp hide enable-group", "Enable all definitions in an exact group",
         "<GROUP> <REVISION>", 2, 2},
        {"sumhp hide disable-group", "Disable all definitions in an exact group",
         "<GROUP> <REVISION>", 2, 2},
        {"sumhp hide retry", "Retry one enabled definition", "<ID> <REVISION>", 2, 2},
        {"sumhp hide remove-id", "Remove a definition by stable ID", "<ID> <REVISION>", 2, 2},
        {"sumhp recovery", "Manage mount recovery state", "<COMMAND>"},
        {"sumhp recovery status", "Read recovery state", ""},
        {"sumhp recovery reset", "Reset recovery state", ""},
    };
    return specs;
}

const std::array<Option, 2> kGlobalOptions = {{
    {"--color", "", "auto|always|never", "", "Choose terminal colors (default: auto)"},
    {"--verbose", "", "", "", "Include detailed diagnostic logs"},
}};

std::vector<const Option*> options_for(const Command& command) {
    std::vector<const Option*> result;
    for (const auto& parent : commands()) {
        if (parent.path == command.path ||
            (!parent.path.empty() && command.path.size() > parent.path.size() &&
             command.path.substr(0, parent.path.size()) == parent.path &&
             command.path[parent.path.size()] == ' ')) {
            for (const auto& option : parent.options)
                result.push_back(&option);
        }
    }
    return result;
}

std::string usage(const Command& command) {
    std::string text = "ksud";
    if (!command.path.empty())
        text += " " + std::string(command.path);
    if (!command.usage.empty())
        text += " " + std::string(command.usage);
    if (!options_for(command).empty())
        text += " [OPTIONS]";
    return text;
}

std::vector<const Command*> children(std::string_view path) {
    const std::string prefix = path.empty() ? "" : std::string(path) + " ";
    std::vector<const Command*> result;
    for (const auto& candidate : commands()) {
        if (candidate.path.size() <= prefix.size() ||
            candidate.path.substr(0, prefix.size()) != prefix)
            continue;
        if (candidate.path.find(' ', prefix.size()) == std::string_view::npos)
            result.push_back(&candidate);
    }
    return result;
}

std::string_view option_description(const Option& option, std::string_view path) {
    if (!option.description.empty())
        return option.description;
    if (option.name == "--temp")
        return "Use temporary configuration, cleared on reboot";
    if (option.name == "--slot")
        return "Target an A/B slot (default: current slot)";
    if (option.name == "--flags")
        return "Unmount flags as an unsigned 32-bit integer (default: 0)";
    if (option.name == "--json")
        return "Write a machine-readable JSON result to stdout";
    if (option.name == "--ota")
        return "Target the inactive A/B slot";
    if (option.name == "--magiskboot")
        return path == "boot-patch" || path == "boot-restore" || path == "boot-patch-v2" ||
                       path == "uninstall"
                   ? "Accepted for compatibility; the embedded tool is used"
                   : "Use this magiskboot path";
    if (option.name == "--libadbroot")
        return "Install this adb root library";
    if (option.name == "--allow-shell")
        return "Keep a root shell available";
    if (option.name == "--enable-adbd")
        return "Run adbd as root";
    if (option.name == "--boot")
        return "Read this boot image instead of detecting a partition";
    if (option.name == "--module")
        return "Use this kernel module";
    if (option.name == "--output" || option.name == "--out")
        return "Write the patched boot image to this path";
    if (option.name == "--superkey")
        return "Set the SuperKey";
    if (option.name == "--force")
        return "Allow replacement of an existing direct LKM capsule";
    if (option.name == "--flash")
        return "Write the result to the selected partition";
    if (option.name == "--signature-bypass")
        return "Relax LKM signature checking";
    if (option.name == "--no-reuse")
        return "Build a fresh capsule instead of reusing an existing one";
    if (option.name == "--log")
        return "Write the flashing log to this file";
    if (option.name == "--use-mkbootfs")
        return "Use the embedded mkbootfs in AnyKernel3";
    if (option.name == "--all")
        return "Include all partitions";
    if (option.name == "--verbose")
        return "Include diagnostic logs";
    if (option.name == "--uid")
        return "Resolve an APK from this Android UID";
    if (option.name == "-g")
        return "Use the global mount namespace";
    return "";
}

std::string_view example(std::string_view path) {
    if (path.empty())
        return "ksud module --help\n  ksud feature list\n  ksud --verbose boot-info current-kmi";
    if (path == "module" || path == "module install")
        return "ksud module install /sdcard/module.zip";
    if (path == "feature" || path == "feature set" || path == "feature set-save")
        return "ksud feature get sulog\n  ksud feature set-save sulog 1";
    if (path == "boot-patch")
        return "ksud boot-patch --boot /sdcard/boot.img --out /sdcard/patched";
    if (path == "boot-patch-v2")
        return "ksud boot-patch-v2 --boot /sdcard/boot.img --output /sdcard/patched.img";
    if (path == "boot-restore")
        return "ksud boot-restore --boot /sdcard/patched.img";
    if (path == "flash" || path == "flash image")
        return "ksud flash image /sdcard/boot.img boot --slot a";
    if (path == "sumhp" || path == "sumhp module" || path == "sumhp module set-mode")
        return "ksud sumhp module list\n  ksud sumhp module set-mode example sumh";
    if (path == "sumhp module configure")
        return "ksud sumhp module configure example sumh "
               "'[{\"path\":\"/system/app/Example\",\"mode\":\"hide\"}]'";
    if (path == "sumhp config" || path == "sumhp config merge-json")
        return "ksud sumhp config merge-json '{\"debug\":true}'";
    if (path == "sumhp daemon call")
        return "ksud sumhp daemon call api system";
    if (path == "su-path" || path == "su-path set")
        return "ksud su-path set /data/local/su";
    return "";
}

void help(const Command& command) {
    if (quiet_validation)
        return;
    (void)printf("%.*s\n\n", static_cast<int>(command.description.size()),
                 command.description.data());
    terminal::heading(stdout, "Usage: ");
    (void)printf("%s\n", usage(command).c_str());
    const auto subcommands = children(command.path);
    if (!subcommands.empty()) {
        terminal::heading(stdout, "\nCommands:\n");
        for (const auto* child : subcommands) {
            const auto name =
                child->path.substr(command.path.empty() ? 0 : command.path.size() + 1);
            (void)printf("  %-22.*s %.*s\n", static_cast<int>(name.size()), name.data(),
                         static_cast<int>(child->description.size()), child->description.data());
        }
    }
    terminal::heading(stdout, "\nOptions:\n");
    (void)printf("  %-32s %s\n", "-h, --help", "Show this help");
    for (const auto* option : options_for(command)) {
        std::string label(option->name);
        if (!option->alias.empty())
            label += ", " + std::string(option->alias);
        if (!option->value.empty())
            label += (option->default_value.empty() ? " <" : " [") + std::string(option->value) +
                     (option->default_value.empty() ? ">" : "]");
        const auto description = option_description(*option, command.path);
        (void)printf("  %-32s %.*s", label.c_str(), static_cast<int>(description.size()),
                     description.data());
        if (!option->default_value.empty())
            (void)printf(" (default when value is omitted: %.*s)",
                         static_cast<int>(option->default_value.size()),
                         option->default_value.data());
        (void)fputc('\n', stdout);
    }
    terminal::heading(stdout, "\nGlobal options:\n");
    (void)printf("  %-32s %s\n", "--color <auto|always|never>", "Choose colors (default: auto)");
    (void)printf("  %-32s %s\n", "--verbose", "Include diagnostic logs");
    if (command.path.empty())
        (void)printf("  %-32s %s\n", "-V, --version", "Show version");
    const auto examples = example(command.path);
    if (!examples.empty()) {
        terminal::heading(stdout, "\nExamples:\n");
        (void)printf("  %.*s\n", static_cast<int>(examples.size()), examples.data());
    }
    if (command.path == "boot-patch" || command.path == "boot-patch-v2" ||
        command.path == "boot-restore")
        (void)printf(
            "\nWithout --boot, the partition is detected. Partition writes require --flash.\n");
    (void)printf("\nUse -- before positional values that begin with '-'.\n");
}

size_t distance(std::string_view left, std::string_view right) {
    std::vector<size_t> row(right.size() + 1);
    std::iota(row.begin(), row.end(), 0U);
    for (size_t i = 0; i < left.size(); ++i) {
        size_t previous = row[0];
        row[0] = i + 1;
        for (size_t j = 0; j < right.size(); ++j) {
            const size_t old = row[j + 1];
            row[j + 1] = std::min({row[j] + 1, old + 1, previous + (left[i] != right[j])});
            previous = old;
        }
    }
    return row.back();
}

std::string suggestion(std::string_view word, const std::vector<std::string_view>& candidates) {
    if (word.size() > 64)
        return "";
    size_t best = 3;
    std::string result;
    for (const auto candidate : candidates) {
        const size_t score = distance(word, candidate);
        if (score < best) {
            best = score;
            result = candidate;
        } else if (score == best && result != candidate)
            result.clear();
    }
    return result.empty() ? "" : "; did you mean '" + result + "'?";
}

int fail(const Command& command, const std::string& message) {
    if (quiet_validation)
        return 2;
    const int result = terminal::usage_error(message, usage(command));
    const std::string path = command.path.empty() ? "ksud" : "ksud " + std::string(command.path);
    (void)fprintf(stderr, "For more information, run '%s --help'.\n", path.c_str());
    return result;
}

const Option* find_option(const Command& command, std::string_view name) {
    for (const auto* option : options_for(command))
        if (option->name == name || (!option->alias.empty() && option->alias == name))
            return option;
    for (const auto& option : kGlobalOptions)
        if (option.name == name)
            return &option;
    return nullptr;
}

bool unsigned_value(std::string_view value, uint64_t maximum, bool auto_base = false) {
    if (value.empty() || value[0] == '-' || value[0] == '+')
        return false;
    int base = 10;
    if (auto_base && value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        value.remove_prefix(2);
    } else if (auto_base && value.size() > 1 && value[0] == '0')
        base = 8;
    uint64_t number = 0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), number, base);
    return error == std::errc{} && end == value.data() + value.size() && number <= maximum;
}

bool one_of(std::string_view value, std::string_view choices) {
    while (!choices.empty()) {
        const size_t separator = choices.find('|');
        if (value == choices.substr(0, separator))
            return true;
        if (separator == std::string_view::npos)
            break;
        choices.remove_prefix(separator + 1);
    }
    return false;
}

int read_option(const Command& command, const std::vector<std::string>& input, size_t& index,
                CliArguments& parsed) {
    const auto& argument = input[index];
    const size_t equals = argument.find('=');
    std::string name = argument.substr(0, equals);
    const Option* option = find_option(command, name);
    std::string attached;
    bool has_attached = equals != std::string::npos;
    if (has_attached)
        attached = argument.substr(equals + 1);
    if (!option && argument.size() > 2 && argument[0] == '-' && argument[1] != '-') {
        option = find_option(command, argument.substr(0, 2));
        if (option && !option->value.empty()) {
            name = argument.substr(0, 2);
            attached = argument.substr(2);
            has_attached = true;
        } else
            option = nullptr;
    }
    if (!option) {
        std::vector<std::string_view> names;
        for (const auto* candidate : options_for(command))
            names.push_back(candidate->name);
        for (const auto& candidate : kGlobalOptions)
            names.push_back(candidate.name);
        return fail(command, "unknown option '" + name + "'" + suggestion(name, names));
    }
    const bool global = option->name == "--color" || option->name == "--verbose";
    const bool repeatable = (command.path == "boot-patch" || command.path == "boot-patch-v2") &&
                            (option->name == "--force-feature" || option->name == "--block-module");
    if (!global && !repeatable && parsed.has(std::string(option->name)))
        return fail(command, "option '" + name + "' was specified more than once");
    std::string value;
    if (option->value.empty()) {
        if (has_attached)
            return fail(command, "option '" + name + "' does not take a value");
    } else if (has_attached)
        value = attached;
    else if (index + 1 < input.size() && (input[index + 1].empty() || input[index + 1][0] != '-'))
        value = input[++index];
    else if (!option->default_value.empty())
        value = option->default_value;
    else
        return fail(command, "option '" + name + "' requires <" + std::string(option->value) + ">");
    if (!option->value.empty() && value.empty())
        return fail(command, "option '" + name + "' requires a non-empty value");
    if (option->value.find('|') != std::string_view::npos && !one_of(value, option->value))
        return fail(command, "invalid value '" + value + "' for " + name + "; expected " +
                                 std::string(option->value));
    if (option->value == "UINT32" && !unsigned_value(value, UINT32_MAX))
        return fail(command, "invalid value '" + value + "' for " + name +
                                 "; expected an unsigned 32-bit integer");
    if (option->value == "FD" && !unsigned_value(value, INT32_MAX))
        return fail(command,
                    "invalid file descriptor '" + value + "'; expected 0 through 2147483647");
    if (option->value == "PORT" &&
        (!unsigned_value(value, 65535) || value.find_first_not_of('0') == std::string::npos))
        return fail(command, "invalid port '" + value + "'; expected 1 through 65535");
    if (option->name == "--color") {
        if (!quiet_validation)
            (void)terminal::set_color(value);
    } else if (option->name == "--verbose") {
        parsed.verbose = true;
    } else {
        parsed.options[std::string(option->name)] = value;
        parsed.option_args.emplace_back(option->name);
        if (!option->value.empty())
            parsed.option_args.push_back(value);
    }
    ++index;
    return -1;
}

bool identifier(std::string_view value) {
    return !value.empty() && value != "." && value != ".." &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-';
           });
}

int validate(const Command& command, const std::vector<std::string>& operands,
             const CliArguments& parsed) {
    const auto path = command.path;
    const auto bad = [&](const std::string& message) { return fail(command, message); };
    const auto numeric = [&](size_t index, uint64_t maximum, bool auto_base = false) {
        return unsigned_value(operands[index], maximum, auto_base);
    };
    if (path == "sumhp api inventory") {
        if (!operands.empty() && operands.size() != 1 && operands.size() != 4)
            return bad("expected CATEGORY alone or CATEGORY INSTANCE_ID GENERATION CURSOR");
        if (!operands.empty() &&
            !one_of(operands[0], "metadata|mapping|legacy_hide|merge|maps_managed|maps_temporary|"
                                 "inject|user_hide|overlay_xattr|kstat|vnode"))
            return bad("unknown inventory category");
        if (operands.size() == 4 &&
            (!numeric(1, UINT64_MAX) || !numeric(2, UINT64_MAX) || !numeric(3, UINT64_MAX) ||
             operands[1].find_first_not_of('0') == std::string::npos ||
             operands[2].find_first_not_of('0') == std::string::npos ||
             (operands[0] == "metadata" &&
              operands[3].find_first_not_of('0') != std::string::npos)))
            return bad("expected nonzero decimal instance/generation and a decimal cursor "
                       "(metadata requires zero)");
    }
    if (path == "module config get" || path == "module config set" ||
        path == "module config delete") {
        if (operands[0].find_first_of("=\r\n") != std::string::npos)
            return bad("module configuration keys cannot contain '=' or line breaks");
        if (path == "module config set" && operands[1].find_first_of("\r\n") != std::string::npos)
            return bad("module configuration values must fit on one line");
    }
    if ((path == "feature set" || path == "feature set-save") && !numeric(1, UINT64_MAX))
        return bad("invalid value '" + operands[1] + "'; expected an unsigned 64-bit integer");
    if (path.substr(0, 11) == "debug mark " && !operands.empty() && !numeric(0, INT32_MAX))
        return bad("invalid PID '" + operands[0] + "'; expected 0 through 2147483647");
    if (path == "flash map" && !one_of(operands[0], "a|b|_a|_b"))
        return bad("invalid slot '" + operands[0] + "'; expected a, b, _a or _b");
    if (path == "flash avb" && !operands.empty() && operands[0] != "disable")
        return bad("unknown AVB operation '" + operands[0] + "'; expected disable");
    if (path == "boot-info target-kmi" && parsed.has("--ota") && parsed.has("--boot"))
        return bad("--ota and --boot cannot be used together");
    if (path == "boot-patch-v2" && parsed.has("--out") && parsed.has("--output"))
        return bad("--out and --output are aliases; specify only one");
    if (path == "dynamic get-sign") {
        if ((!operands.empty()) == parsed.has("--uid"))
            return bad("specify exactly one APK path or --uid <UID>");
        if (parsed.has("--uid") && !unsigned_value(parsed.value("--uid"), UINT32_MAX, true))
            return bad("invalid UID; expected an unsigned 32-bit integer");
    }
    if (path == "dynamic set-uid" && !numeric(0, UINT32_MAX, true))
        return bad("invalid UID; expected an unsigned 32-bit integer");
    if (path == "dynamic set-hash" || path == "dynamic del") {
        if (!numeric(0, UINT32_MAX, true))
            return bad("invalid signature size; expected an unsigned 32-bit integer");
        if (operands[1].size() != 64 ||
            !std::all_of(operands[1].begin(), operands[1].end(),
                         [](unsigned char ch) { return std::isxdigit(ch); }))
            return bad("invalid signature hash; expected exactly 64 hexadecimal digits");
    }
    if (path.rfind("sumhp ", 0) == 0) {
        if (path.rfind("sumhp hide ", 0) == 0) {
            std::vector<std::string> args = {"hide", std::string(path.substr(11))};
            if (sumhp::hide::definition_command(args)) {
                args.insert(args.end(), operands.begin(), operands.end());
                sumhp::hide::Request request;
                std::string error;
                if (!sumhp::hide::parse_request(args, request, error))
                    return bad(error);
            }
        }
        if (path == "sumhp config merge-json") {
            json::Value object;
            std::string error;
            if (!json::parse_checked(operands[0], object, error) ||
                object.type != json::Type::Object)
                return bad("invalid configuration JSON: " +
                           (error.empty() ? std::string("expected an object") : error));
            if (!sumhp::validate_config_patch(object, error))
                return bad(error);
        }
        if ((path == "sumhp module set-mode" || path == "sumhp module configure") &&
            !one_of(operands[1], "auto|sumh|overlay|magic|none"))
            return bad("invalid mount mode '" + operands[1] +
                       "'; expected auto, sumh, overlay, magic or none");
        if (path == "sumhp module configure") {
            json::Value rules;
            std::string error;
            if (!json::parse_checked(operands[2], rules, error) || !rules.is_array())
                return bad("invalid module rules JSON: " +
                           (error.empty() ? std::string("expected an array") : error));
        }
        if (path == "sumhp module add-rule" &&
            !one_of(operands[2], "auto|sumh|overlay|magic|none|hide"))
            return bad("invalid rule mode '" + operands[2] +
                       "'; expected auto, sumh, overlay, magic, none or hide");
        if (path == "sumhp sumh mount-hide" && !one_of(operands[0], "off|on|normal|aggressive"))
            return bad("invalid mount hiding mode; expected off, on, normal or aggressive");
        if ((path == "sumhp sumh maps-spoof" || path == "sumhp sumh statfs-spoof") &&
            !one_of(operands[0], "off|on"))
            return bad("invalid state '" + operands[0] + "'; expected off or on");
        if (path == "sumhp sumh kernel-build set") {
            if (operands[0].empty() || operands[1].empty())
                return bad("kernel release and full build version are required; use default to "
                           "keep an original field");
            if (operands[0] == "default" && operands[1] == "default")
                return bad("at least one custom kernel build field is required; use reset to "
                           "restore both");
            json::Value patch = json::Value::object();
            patch.o["kernel_build_release"] = operands[0] == "default" ? "" : operands[0];
            patch.o["kernel_build_version"] = operands[1] == "default" ? "" : operands[1];
            std::string error;
            if (!sumhp::validate_config_patch(patch, error))
                return bad(error);
        }
        if (path == "sumhp debug stealth" && !one_of(operands[0], "enable|disable"))
            return bad("invalid stealth state; expected enable or disable");
        if (path == "sumhp sumh maps add") {
            for (size_t i = 0; i < 4; ++i)
                if (!numeric(i, UINT64_MAX, true))
                    return bad("invalid maps value '" + operands[i] +
                               "'; expected an unsigned integer");
        }
        if (path.rfind("sumhp module ", 0) == 0 && !operands.empty() &&
            path != "sumhp module normalize" && !identifier(operands[0]))
            return bad("invalid module ID '" + operands[0] + "'");
    }
    if (command.usage.find("<ABSOLUTE-PATH>") != std::string_view::npos && path != "su-path set") {
        const size_t index =
            path == "sumhp module add-rule" || path == "sumhp module remove-rule" ? 1 : 0;
        if (operands[index].empty() || operands[index][0] != '/')
            return bad("expected an absolute path, got '" + operands[index] + "'");
    }
    return -1;
}

int parse_input(const std::vector<std::string>& input, CliArguments& parsed, unsigned depth) {
    if (depth > 8)
        return quiet_validation ? 2
                                : terminal::usage_error("too many nested daemon calls",
                                                        "ksud sumhp daemon call <COMMAND>");
    size_t index = 0;
    const Command* current = &commands().front();
    bool help_requested = false;
    while (true) {
        if (index == input.size())
            break;
        const auto& argument = input[index];
        if (argument == "-h" || argument == "--help") {
            help(*current);
            return 0;
        }
        if (argument == "help") {
            help_requested = true;
            ++index;
            continue;
        }
        if (argument.size() > 1 && argument[0] == '-' &&
            !(current->path.empty() && one_of(argument, "-v|-V|--version")) &&
            !(current->path == "sumhp" && argument == "--version")) {
            const int result = read_option(*current, input, index, parsed);
            if (result >= 0)
                return result;
            continue;
        }
        std::string name = argument;
        if (current->path.empty()) {
            if (one_of(name, "-v|-V|--version"))
                name = "version";
        } else if (current->path == "sumhp" && name == "--version")
            name = "version";
        const auto candidates = children(current->path);
        const std::string path =
            current->path.empty() ? name : std::string(current->path) + " " + name;
        const auto found =
            std::find_if(candidates.begin(), candidates.end(),
                         [&path](const Command* item) { return item->path == path; });
        if (found == candidates.end()) {
            std::vector<std::string_view> names;
            names.reserve(candidates.size());
            for (const auto* child : candidates)
                names.push_back(
                    child->path.substr(current->path.empty() ? 0 : current->path.size() + 1));
            return fail(*current, "unknown command '" + name + "'" + suggestion(name, names));
        }
        current = *found;
        ++index;
        if (children(current->path).empty())
            break;
    }
    if (help_requested || current->path.empty()) {
        help(*current);
        return 0;
    }
    if (!children(current->path).empty())
        return fail(*current, "a subcommand is required");
    // Preview's read-only guarantee belongs to the dedicated outer argv route,
    // not a generic/nested daemon call that may perform startup restoration.
    // Keep the public command discoverable, but reject alternative dispatches.
    if (current->path == "sumhp sumh maps preview" &&
        (depth != 0 || input != std::vector<std::string>{"sumhp", "sumh", "maps", "preview"}))
        return fail(*current, "use the exact read-only command: ksud sumhp sumh maps preview");
    // Unlike ordinary commands, these must enter through cli_run's exact raw
    // argv route. Reject every generic route, including daemon call and global
    // option wrappers; parsing cannot silently turn them into restoring calls.
    if (current->path == "sumhp doctor" || current->path.compare(0, 13, "sumhp doctor ") == 0 ||
        current->path == "sumhp profile" || current->path.compare(0, 14, "sumhp profile ") == 0 ||
        current->path == "sumhp sumh maps remove" ||
        current->path == "sumhp sumh maps list-temporary" ||
        current->path.compare(0, 16, "sumhp operation ") == 0 ||
        current->path == "sumhp api capabilities" || current->path == "sumhp hide status" ||
        current->path == "sumhp hide definitions" || current->path == "sumhp hide export" ||
        current->path == "sumhp module plan" || current->path == "sumhp module explain" ||
        current->path == "sumhp module inspect" ||
        current->path == "sumhp config discover-partitions" ||
        current->path == "sumhp config sync-partitions" ||
        current->path == "sumhp config set-partitions" ||
        (current->path == "sumhp module normalize" &&
         std::find(input.begin(), input.end(), "--dry-run") != input.end()))
        return fail(*current,
                    "use the exact local command; generic or wrapped dispatch is not supported");
    parsed.path = current->path;
    const size_t separator = parsed.path.find(' ');
    parsed.command = parsed.path.substr(0, separator);
    parsed.forward_options = current->forward_options;
    size_t begin = separator;
    while (begin != std::string::npos) {
        const size_t end = parsed.path.find(' ', begin + 1);
        parsed.args.push_back(parsed.path.substr(begin + 1, end - begin - 1));
        begin = end;
    }
    std::vector<std::string> operands;
    bool options_enabled = true;
    while (index < input.size()) {
        const auto& argument = input[index];
        if (current->maximum == kTrailingArgs && !operands.empty()) {
            operands.push_back(argument);
            ++index;
            continue;
        }
        if (options_enabled && argument == "--") {
            options_enabled = false;
            ++index;
            continue;
        }
        if (options_enabled && (argument == "-h" || argument == "--help")) {
            help(*current);
            return 0;
        }
        if (options_enabled && argument.size() > 1 && argument[0] == '-') {
            const int result = read_option(*current, input, index, parsed);
            if (result >= 0)
                return result;
        } else {
            operands.push_back(argument);
            ++index;
        }
    }
    if (operands.size() < current->minimum)
        return fail(*current, "missing required argument: " + std::string(current->usage));
    if (operands.size() > current->maximum)
        return fail(*current, "unexpected argument '" + operands[current->maximum] + "'");
    const int validation = validate(*current, operands, parsed);
    if (validation >= 0)
        return validation;
    if (current->path == "sumhp daemon call") {
        std::vector<std::string> nested_input = {"sumhp"};
        nested_input.insert(nested_input.end(), operands.begin(), operands.end());
        CliArguments nested;
        nested.verbose = parsed.verbose;
        const int result = parse_input(nested_input, nested, depth + 1);
        if (result >= 0)
            return result;
        parsed.verbose = nested.verbose;
        operands = nested.args;
        operands.insert(operands.end(), nested.option_args.begin(), nested.option_args.end());
    }
    parsed.args.insert(parsed.args.end(), operands.begin(), operands.end());
    return -1;
}

}  // namespace

bool CliArguments::has(const std::string& name) const {
    return options.find(name) != options.end();
}
std::string CliArguments::value(const std::string& name) const {
    const auto found = options.find(name);
    return found == options.end() ? "" : found->second;
}

int parse_cli(const std::vector<std::string>& input, CliArguments& parsed) {
    parsed = {};
    (void)terminal::set_color("auto");
    return parse_input(input, parsed, 0);
}

int parse_cli_quiet(const std::vector<std::string>& input, CliArguments& parsed) {
    class RestoreQuiet {
    public:
        explicit RestoreQuiet(bool previous) : previous_(previous) {}
        RestoreQuiet(const RestoreQuiet&) = delete;
        RestoreQuiet& operator=(const RestoreQuiet&) = delete;
        RestoreQuiet(RestoreQuiet&&) = delete;
        RestoreQuiet& operator=(RestoreQuiet&&) = delete;
        ~RestoreQuiet() { quiet_validation = previous_; }

    private:
        bool previous_;
    };
    const RestoreQuiet restore{quiet_validation};
    quiet_validation = true;
    parsed = {};
    return parse_input(input, parsed, 0);
}

}  // namespace ksud
