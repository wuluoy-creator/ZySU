#include <algorithm>
#include "core/partition_config.hpp"
#include "core/rule_inventory.hpp"
#include "mount/backend.hpp"
#include "mount/development.hpp"

namespace sumhp::mount {
bool is_development_prefix(const std::vector<std::string>& args) {
    if (args.size() < 2)
        return false;
    return (args[0] == "config" && (args[1] == "discover-partitions" ||
                                    args[1] == "sync-partitions" || args[1] == "set-partitions")) ||
           (args[0] == "module" &&
            (args[1] == "inspect" ||
             (args[1] == "normalize" &&
              std::find(args.begin() + 2, args.end(), "--dry-run") != args.end())));
}
bool parse_development_arguments(const std::vector<std::string>& args, DevelopmentRequest& request,
                                 std::string& error) {
    error =
        "use exact local module inspect <absolute-path>, module normalize <absolute-path> "
        "--dry-run, config discover-partitions, or config set-partitions <revision> <JSON-array>";
    if (!is_development_prefix(args))
        return false;
    DevelopmentRequest parsed;
    if (args[0] == "module") {
        if (((args[1] != "inspect" || args.size() != 3) &&
             (args[1] != "normalize" || args.size() != 4 || args[3] != "--dry-run")) ||
            !valid_module_rule_path(args[2]) || !inventory::valid_utf8(args[2]))
            return false;
        parsed.kind =
            args[1] == "inspect" ? DevelopmentKind::Inspect : DevelopmentKind::NormalizePreview;
        parsed.module_path = args[2];
    } else if (args[1] == "set-partitions") {
        if ((args.size() != 4 && (args.size() != 5 || args[4] != "--channel")) ||
            !valid_config_revision(args[2]) || args[3].size() > PARTITIONS_INPUT_BYTES ||
            args[3].find('\0') != std::string::npos || !inventory::valid_utf8(args[3]))
            return false;
        parsed.kind = DevelopmentKind::PartitionSave;
        parsed.revision = args[2];
        parsed.partitions = args[3];
        parsed.channel = args.size() == 5;
    } else {
        if (args.size() != 2 &&
            (args[1] != "discover-partitions" || args.size() != 3 || args[2] != "--channel"))
            return false;
        parsed.kind = DevelopmentKind::PartitionDiscovery;
        parsed.channel = args.size() == 3;
    }
    request = std::move(parsed);
    error.clear();
    return true;
}
}  // namespace sumhp::mount
