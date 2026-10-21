#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
struct ExpectedCall {
    unsigned long command;
    std::function<int(void*)> reply;
};
std::deque<ExpectedCall> calls;

void expect(unsigned long command, std::function<int(void*)> reply) {
    calls.push_back({command, std::move(reply)});
}

void expect_features(int bitmask) {
    expect(SUMH_IOC_GET_FEATURES, [bitmask](void* arg) {
        *static_cast<int*>(arg) = bitmask;
        return 0;
    });
}

void expect_error(unsigned long command, int error) {
    expect(command, [error](void*) {
        errno = error;
        return -1;
    });
}

void expect_hide_rule(std::uint64_t cursor, std::uint64_t id, const char* path) {
    expect(SUMH_IOC_USER_HIDE_QUERY, [=](void* value) {
        auto& arg = *static_cast<sumh_user_hide_arg*>(value);
        assert(arg.rule_id == cursor);
        arg.rule_id = id;
        arg.path_len = std::strlen(path);
        std::memcpy(arg.path, path, arg.path_len + 1);
        return 0;
    });
}

void test_protocol_compatibility() {
    namespace sumh = sumhp::sumh;
    for (const int protocol : {0, 1, 2, 17}) {
        expect(SUMH_IOC_GET_VERSION, [protocol](void* arg) {
            *static_cast<int*>(arg) = protocol;
            return 0;
        });
        const auto info = sumh::version_info();
        assert(info.expected_protocol == 1);
        assert(info.kernel_protocol == protocol);
        const auto status = protocol == 1   ? sumh::Status::Available
                            : protocol == 0 ? sumh::Status::KernelTooOld
                                            : sumh::Status::ClientTooOld;
        assert(info.status == status);
    }
    expect_error(SUMH_IOC_GET_VERSION, ENOTTY);
    const auto missing = sumh::version_info();
    assert(missing.status == sumh::Status::NotPresent);
    assert(missing.last_errno == ENOTTY);
    assert(calls.empty());
}

void test_text_replies() {
    namespace sumh = sumhp::sumh;
    // The ABI reports its length separately and need not copy a terminator.
    expect(SUMH_IOC_LIST_RULES, [](void* value) {
        auto& arg = *static_cast<sumh_syscall_list_arg*>(value);
        std::memset(arg.buf, 'x', arg.size);
        return 0;
    });
    assert(sumh::active_rules() == std::string(64UL * 1024, 'x'));

    expect(SUMH_IOC_GET_HOOKS, [](void* value) {
        auto& arg = *static_cast<sumh_syscall_list_arg*>(value);
        std::memcpy(arg.buf, "hooks trailing bytes", 20);
        arg.size = 5;
        return 0;
    });
    assert(sumh::hooks() == "hooks");

    expect(SUMH_IOC_GET_HOOKS, [](void* value) {
        auto& arg = *static_cast<sumh_syscall_list_arg*>(value);
        std::memcpy(arg.buf, "a\0b", 3);
        arg.size = 3;
        return 0;
    });
    assert(sumh::hooks() == std::string("a\0b", 3));

    expect(SUMH_IOC_LIST_RULES, [](void* value) {
        auto& arg = *static_cast<sumh_syscall_list_arg*>(value);
        std::memset(arg.buf, 'x', arg.size);
        arg.size = 0;
        return 0;
    });
    assert(sumh::active_rules().empty());

    expect(SUMH_IOC_GET_HOOKS, [](void* value) {
        ++static_cast<sumh_syscall_list_arg*>(value)->size;
        return 0;
    });
    assert(sumh::hooks().empty());
    assert(errno == EPROTO);

    expect_error(SUMH_IOC_LIST_RULES, EACCES);
    assert(sumh::active_rules().empty());
    assert(errno == EACCES);
    assert(calls.empty());
}

void test_active_modules() {
    namespace sumh = sumhp::sumh;
    assert(sumh::active_modules_from_rules("").empty());
    assert(sumh::active_modules_from_rules("\nno module paths\n\n").empty());

    const auto modules =
        sumh::active_modules_from_rules("add /system/a /data/adb/modules/z/system/a 0\n"
                                        "merge /system/b /data/adb/modules/a/system/b\n"
                                        "add /system/c /data/adb/modules/z/system/c 0\n");
    assert((modules == std::vector<std::string>{"a", "z"}));

    // Multiple paths on a line, CRLF, and the last unterminated line retain
    // the same matching and lexical sorting behavior as the line parser.
    const auto mixed = sumh::active_modules_from_rules(
        "prefix/data/adb/modules/z/system/a /data/adb/modules/a/system/b\r\n"
        "\n/data/adb/modules/z/vendor/a\n"
        "/data/adb/modules/long module name/system/c\n"
        "/data/adb/modules/A/system/a");
    assert((mixed == std::vector<std::string>{"A", "a", "long module name", "z"}));

    // A slash on the following line must not finish an incomplete module
    // name; empty names and names with no following slash are ignored.
    assert(sumh::active_modules_from_rules("/data/adb/modules/incomplete\n"
                                           "/system/a\n"
                                           "/data/adb/modules//system/a\n"
                                           "/data/adb/modules/final")
               .empty());

    std::string repeated;
    for (int index = 0; index < 1024; ++index)
        repeated += "/data/adb/modules/a_module_name_longer_than_sso/system/a\n";
    auto unique = sumh::active_modules_from_rules(repeated);
    repeated.assign(repeated.size(), 'x');
    assert((unique == std::vector<std::string>{"a_module_name_longer_than_sso"}));
}

void test_feature_errors() {
    namespace sumh = sumhp::sumh;
    expect_error(SUMH_IOC_GET_FEATURES, EIO);
    assert(!sumh::clear_overlay_xattr_hiding());
    assert(errno == EIO);

    expect_features(0);
    assert(sumh::clear_overlay_xattr_hiding());
    expect_features(SUMH_FEATURE_OVERLAY_XATTR_HIDE);
    expect(SUMH_IOC_HIDE_OVERLAY_XATTRS, [](void* value) {
        const auto& arg = *static_cast<sumh_syscall_arg*>(value);
        assert(arg.src != nullptr && arg.src[0] == '\0');
        return 0;
    });
    assert(sumh::clear_overlay_xattr_hiding());

    expect_error(SUMH_IOC_GET_FEATURES, EACCES);
    assert(!sumh::set_mount_hide(true));
    assert(errno == EACCES);
    expect_features(SUMH_FEATURE_MOUNT_HIDE);
    assert(!sumh::set_mount_hide(true, sumh::MountHideMode::Aggressive));
    assert(errno == EOPNOTSUPP);

    expect_features(SUMH_FEATURE_MOUNT_HIDE);
    expect(SUMH_IOC_SET_MOUNT_HIDE, [](void* value) {
        const auto& arg = *static_cast<sumh_mount_hide_arg*>(value);
        assert(arg.enable == 1);
        return 0;
    });
    assert(sumh::set_mount_hide(true));

    expect_features(SUMH_FEATURE_MOUNT_HIDE | SUMH_FEATURE_MOUNT_HIDE_AGGRESSIVE);
    expect(SUMH_IOC_SET_MOUNT_HIDE_MODE, [](void* value) {
        assert(*static_cast<int*>(value) == SUMH_MOUNT_HIDE_MODE_AGGRESSIVE);
        return 0;
    });
    expect(SUMH_IOC_SET_MOUNT_HIDE, [](void* value) {
        auto& arg = *static_cast<sumh_mount_hide_arg*>(value);
        assert(arg.enable == 1);
        arg.err = -EBUSY;
        return 0;
    });
    assert(!sumh::set_mount_hide(true, sumh::MountHideMode::Aggressive));
    assert(errno == EBUSY);
    assert(calls.empty());
}

void test_maps_paths() {
    namespace sumh = sumhp::sumh;
    assert(!sumh::add_maps_rule(1, 2, 3, 4, std::string(SUMH_MAX_LEN_PATHNAME, 'x')));
    assert(errno == ENAMETOOLONG);
    assert(!sumh::add_maps_rule(1, 2, 3, 4, std::string("/path\0suffix", 12)));
    assert(errno == EINVAL);

    const std::string maximum_path(SUMH_MAX_LEN_PATHNAME - 1, 'x');
    expect(SUMH_IOC_ADD_MAPS_RULE, [&](void* value) {
        const auto& arg = *static_cast<sumh_maps_rule*>(value);
        assert(arg.target_ino == 1 && arg.target_dev == 2);
        assert(arg.spoofed_ino == 3 && arg.spoofed_dev == 4);
        assert(std::string(arg.spoofed_pathname) == maximum_path);
        return 0;
    });
    assert(sumh::add_maps_rule(1, 2, 3, 4, maximum_path));
    expect(SUMH_IOC_ADD_MAPS_RULE, [](void* value) {
        const auto& arg = *static_cast<sumh_maps_rule*>(value);
        assert(arg.spoofed_pathname[0] == '\0');
        return 0;
    });
    // An empty pathname preserves the original path while spoofing ino/dev.
    assert(sumh::add_maps_rule(1, 2, 3, 4, ""));
    assert(calls.empty());
}

void test_managed_maps() {
    namespace sumh = sumhp::sumh;
    const sumh::MapsRule valid{1, 2, 3, 4, "/replacement"};
    for (const auto& path : {std::string(), std::string("relative"), std::string("/bad\n"),
                             std::string("/bad\r"), std::string("/bad\0suffix", 11)}) {
        auto rule = valid;
        rule.spoofed_path = path;
        assert(!sumh::replace_managed_maps_rules({rule}));
        assert(errno == EINVAL);
    }
    auto rule = valid;
    rule.spoofed_path = "/" + std::string(SUMH_MAX_LEN_PATHNAME - 1, 'x');
    assert(!sumh::replace_managed_maps_rules({rule}) && errno == ENAMETOOLONG);
    for (int field = 0; field < 4; ++field) {
        rule = valid;
        if (field == 0)
            rule.target_ino = 0;
        if (field == 1)
            rule.target_dev = 0;
        if (field == 2)
            rule.spoofed_ino = 0;
        if (field == 3)
            rule.spoofed_dev = 0;
        assert(!sumh::replace_managed_maps_rules({rule}) && errno == EINVAL);
    }
    assert(!sumh::replace_managed_maps_rules({valid, valid}) && errno == EINVAL);
    assert(!sumh::replace_managed_maps_rules(
        std::vector<sumh::MapsRule>(SUMH_MANAGED_MAPS_MAX_RULES + 1, valid)));
    assert(errno == E2BIG && calls.empty());
    expect_features(0);
    bool submitted = true;
    assert(sumh::replace_managed_maps_rules_checked({}, submitted) && !submitted);
    expect_features(0);
    assert(!sumh::replace_managed_maps_rules({valid}) && errno == EOPNOTSUPP);
    expect_error(SUMH_IOC_GET_FEATURES, EACCES);
    assert(!sumh::replace_managed_maps_rules({}) && errno == EACCES);
    expect_features(SUMH_FEATURE_MANAGED_MAPS);
    expect(SUMH_IOC_SET_MANAGED_MAPS, [](void* value) {
        const auto& arg = *static_cast<sumh_managed_maps_arg*>(value);
        assert(arg.size == sizeof(arg) && arg.count == 1 && arg.rules != 0);
        assert(arg.reserved[0] == 0 && arg.reserved[1] == 0);
        const auto* rules =
            reinterpret_cast<const sumh_maps_rule*>(static_cast<std::uintptr_t>(arg.rules));
        assert(rules[0].target_ino == 1 && rules[0].target_dev == 2 && rules[0].err == 0);
        assert(rules[0].spoofed_ino == 3 && rules[0].spoofed_dev == 4);
        assert(std::string(rules[0].spoofed_pathname) == "/replacement");
        return 0;
    });
    assert(sumh::replace_managed_maps_rules_checked({valid}, submitted) && submitted);
    expect_features(SUMH_FEATURE_MANAGED_MAPS);
    expect_error(SUMH_IOC_SET_MANAGED_MAPS, ENOMEM);
    assert(!sumh::replace_managed_maps_rules_checked({valid}, submitted) && errno == ENOMEM &&
           !submitted);
    expect_features(SUMH_FEATURE_MANAGED_MAPS);
    expect(SUMH_IOC_SET_MANAGED_MAPS, [](void* value) {
        const auto& arg = *static_cast<sumh_managed_maps_arg*>(value);
        assert(arg.count == 0 && arg.rules == 0);
        return 0;
    });
    assert(sumh::replace_managed_maps_rules_checked({}, submitted) && submitted);
    assert((sumh::feature_names(SUMH_FEATURE_MANAGED_MAPS) ==
            std::vector<std::string>{"managed_maps"}));
    assert(calls.empty());
}

void test_kernel_build() {
    namespace sumh = sumhp::sumh;
    const std::string release = "6.1.75-android14-11-g16c5f6cd5e9b-ab12268515";
    const std::string version = "#1 SMP PREEMPT Fri Aug 23 03:08:10 UTC 2024";
    assert(!sumh::set_kernel_build(true, "", ""));
    assert(errno == EINVAL);
    for (const auto& invalid : {std::string("release\n"), std::string("bad release"),
                                std::string("bad\0release", 11), std::string("bad\x7f", 4)}) {
        assert(!sumh::set_kernel_build(true, invalid, version));
        assert(errno == EINVAL);
    }
    assert(!sumh::set_kernel_build(true, std::string(SUMH_KERNEL_BUILD_MAX, 'x'), version));
    assert(errno == ENAMETOOLONG);

    expect_error(SUMH_IOC_GET_FEATURES, EACCES);
    assert(!sumh::set_kernel_build(true, release, version));
    assert(errno == EACCES);
    expect_features(0);
    assert(!sumh::set_kernel_build(true, release, version));
    assert(errno == EOPNOTSUPP);
    expect_features(0);
    assert(sumh::set_kernel_build(false));

    const std::string maximum(SUMH_KERNEL_BUILD_MAX - 1, 'x');
    expect_features(SUMH_FEATURE_KERNEL_BUILD_SPOOF);
    expect(SUMH_IOC_SET_KERNEL_BUILD, [&](void* value) {
        const auto& arg = *static_cast<sumh_kernel_build_arg*>(value);
        assert(arg.size == sizeof(arg) && arg.enable == 1);
        assert(arg.reserved[0] == 0 && arg.reserved[1] == 0);
        assert(arg.reserved_tail[0] == 0 && arg.reserved_tail[1] == 0);
        assert(std::string(arg.release) == maximum && std::string(arg.version) == version);
        return 0;
    });
    assert(sumh::set_kernel_build(true, maximum, version));
    expect_features(SUMH_FEATURE_KERNEL_BUILD_SPOOF);
    expect(SUMH_IOC_SET_KERNEL_BUILD, [](void* value) {
        const auto& arg = *static_cast<sumh_kernel_build_arg*>(value);
        assert(arg.enable == 0 && arg.release[0] == '\0' && arg.version[0] == '\0');
        return 0;
    });
    assert(sumh::set_kernel_build(false, "ignored\ninvalid", std::string(100, 'x')));
    expect_features(SUMH_FEATURE_KERNEL_BUILD_SPOOF);
    expect_error(SUMH_IOC_SET_KERNEL_BUILD, EBUSY);
    assert(!sumh::set_kernel_build(true, release, version));
    assert(errno == EBUSY);

    const std::string original_release = "6.6.1-android15-original";
    const std::string original_version = "#2 original";
    for (const bool keep_release : {true, false}) {
        // Even after another spoof, an empty field always resolves through
        // GET_ORIGINAL; GET_KERNEL_BUILD must never supply the retained field.
        expect_features(SUMH_FEATURE_KERNEL_BUILD_SPOOF);
        expect(SUMH_IOC_GET_ORIGINAL_KERNEL_BUILD, [&](void* value) {
            auto& arg = *static_cast<sumh_kernel_build_arg*>(value);
            arg.enable = 1;
            std::strcpy(arg.release, original_release.c_str());
            std::strcpy(arg.version, original_version.c_str());
            return 0;
        });
        expect(SUMH_IOC_SET_KERNEL_BUILD, [&](void* value) {
            const auto& arg = *static_cast<sumh_kernel_build_arg*>(value);
            assert(std::string(arg.release) == (keep_release ? original_release : release));
            assert(std::string(arg.version) == (keep_release ? version : original_version));
            return 0;
        });
        assert(
            sumh::set_kernel_build(true, keep_release ? "" : release, keep_release ? version : ""));
    }
    expect_features(SUMH_FEATURE_KERNEL_BUILD_SPOOF);
    expect_error(SUMH_IOC_GET_ORIGINAL_KERNEL_BUILD, EIO);
    assert(!sumh::set_kernel_build(true, "", version));
    assert(errno == EIO);

    sumh::KernelBuild state;
    expect(SUMH_IOC_GET_KERNEL_BUILD, [&](void* value) {
        auto& arg = *static_cast<sumh_kernel_build_arg*>(value);
        assert(arg.size == sizeof(arg));
        arg.enable = 1;
        std::strcpy(arg.release, release.c_str());
        std::strcpy(arg.version, version.c_str());
        return 0;
    });
    assert(sumh::kernel_build(state));
    assert(state.enabled && state.release == release && state.version == version);

    expect(SUMH_IOC_GET_KERNEL_BUILD, [](void* value) {
        auto& arg = *static_cast<sumh_kernel_build_arg*>(value);
        std::memset(arg.release, 'x', sizeof(arg.release));
        return 0;
    });
    assert(!sumh::kernel_build(state));
    assert(errno == EPROTO && state.release == release);
    expect_error(SUMH_IOC_GET_ORIGINAL_KERNEL_BUILD, EIO);
    assert(!sumh::original_kernel_build(state));
    assert(errno == EIO && state.release == release);
    expect(SUMH_IOC_GET_ORIGINAL_KERNEL_BUILD, [](void* value) {
        auto& arg = *static_cast<sumh_kernel_build_arg*>(value);
        arg.enable = 1;
        std::strcpy(arg.release, "6.6.1-android15-original");
        std::strcpy(arg.version, "#2 original");
        return 0;
    });
    assert(sumh::original_kernel_build(state));
    assert(state.enabled && state.release == "6.6.1-android15-original");
    const auto names = sumh::feature_names(SUMH_FEATURE_KERNEL_BUILD_SPOOF);
    assert((names == std::vector<std::string>{"kernel_build_spoof"}));
    assert(calls.empty());
}

void test_hide_snapshot() {
    namespace sumh = sumhp::sumh;
    std::vector<sumh::UserHideRule> rules;
    expect_hide_rule(0, 7, "/first");
    expect_hide_rule(7, 15, "/second");
    expect_hide_rule(15, 0, "");
    assert(sumh::user_hide_rules(rules));
    assert(rules.size() == 2 && rules[0].path == "/first" && rules[1].id == 15);

    expect_hide_rule(0, 8, "/replacement");
    expect_error(SUMH_IOC_USER_HIDE_QUERY, EIO);
    assert(!sumh::user_hide_rules(rules));
    assert(errno == EIO);
    assert(rules.size() == 2 && rules[0].path == "/first" && rules[1].id == 15);

    expect(SUMH_IOC_USER_HIDE_QUERY, [](void* value) {
        auto& arg = *static_cast<sumh_user_hide_arg*>(value);
        arg.path_len = sizeof(arg.path);
        return 0;
    });
    assert(!sumh::user_hide_rules(rules));
    assert(errno == EPROTO);
    expect_hide_rule(0, 1, "/first");
    expect_hide_rule(1, 1, "/first");
    assert(!sumh::user_hide_rules(rules));
    assert(errno == EOVERFLOW);
    assert(calls.empty());
}
}  // namespace

extern "C" int ksu_sumh_ioctl(unsigned long command, void* arg) {
    assert(!calls.empty());
    auto call = std::move(calls.front());
    calls.pop_front();
    assert(call.command == command);
    return call.reply(arg);
}

int main() {
    test_protocol_compatibility();
    test_text_replies();
    test_feature_errors();
    test_maps_paths();
    test_managed_maps();
    test_kernel_build();
    test_hide_snapshot();
    test_active_modules();
    std::cout << "sumh_client_test: all checks passed\n";
}
