#!/usr/bin/env python3
"""Run production per-app unmount decisions and deferred work on the host.

Only kernel boundaries (credentials, profile storage, task work and mount I/O)
are doubled. No real mounts are touched. Both policy build configurations run.
Use --zig on Windows, or a native --cc compiler on Linux.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def function(path, name):
    text = (ROOT / path).read_text(encoding="utf-8")
    match = re.search(r"^[\w\s*]+\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", text, re.M)
    if not match:
        raise ValueError(f"missing {name} in {path}")
    end = text.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end] + "\n"


def declaration(path, kind, name):
    text = (ROOT / path).read_text(encoding="utf-8")
    match = re.search(r"^" + kind + r"\s+" + re.escape(name) + r"\s*\{[^}]*\};", text, re.M)
    if not match:
        raise ValueError(f"missing {kind} {name} in {path}")
    return match.group() + "\n"


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int uid_t;
typedef uint32_t __u32;
typedef uint8_t __u8;
#define likely(x) (x)
#define unlikely(x) (x)
#define READ_ONCE(x) (x)
#define smp_load_acquire(p) (*(p))
#define __user
#define __kuid_val(x) (x)
#define task_uid(t) ((t)->real_uid)
#define GFP_KERNEL 0
#define PF_EXITING 4
#define TWA_RESUME 1
#define CLONE_NEWNS 0x20000
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define pr_info(...) ((void)0)
#define pr_warn(...) ((void)0)
#define pr_err(...) ((void)0)
#define pr_warn_ratelimited(...) ((void)0)

struct non_root_profile { bool umount_modules; };
struct app_profile {
    bool allow_su;
    struct { bool use_default; struct non_root_profile profile; } nrp_config;
};
static struct non_root_profile default_non_root_profile;
static struct app_profile app_profile;
static bool has_profile, allow_uid, manager_valid, webview_enabled;
static const uid_t manager_uids[] = {10002, 110003};
static unsigned profile_gets, profile_puts, profile_refs, allow_queries, webview_queries;
static uid_t queried_uid;
static bool credentials_overridden;
static bool ksu_is_manager_uid_valid(void) { return manager_valid; }
static bool ksu_is_uid_manager(uid_t uid) {
    for (size_t i = 0; i < sizeof(manager_uids) / sizeof(*manager_uids); ++i)
        if (uid == manager_uids[i]) return true;
    return false;
}
static bool __ksu_is_allow_uid(uid_t uid) {
    assert(!credentials_overridden); ++allow_queries; queried_uid = uid; return allow_uid;
}
static struct app_profile *ksu_get_app_profile(uid_t uid) {
    assert(!credentials_overridden); ++profile_gets; queried_uid = uid;
    if (!has_profile) return NULL;
    ++profile_refs; return &app_profile;
}
static void ksu_put_app_profile(struct app_profile *profile) {
    assert(profile == &app_profile && profile_refs); --profile_refs; ++profile_puts;
}
static bool ksu_is_webview_zygote_umount_enabled(void) {
    assert(!credentials_overridden); ++webview_queries; return webview_enabled;
}
static bool sumh_enabled, resolving_rules;
static bool sumh_hide_rules_resolving(void) { return resolving_rules; }
struct task_struct { uid_t real_uid; unsigned flags; int pid; void *fs, *nsproxy; };
static struct task_struct task;
static struct task_struct *current = &task;
struct cred { uid_t uid; } normal_cred, root_cred;
static const struct cred *ksu_cred;
static const struct cred *current_cred(void) { return &normal_cred; }
static bool zygote_child;
static bool ksu_is_trusted_zygote(const struct cred *cred) { assert(cred == &normal_cred); return zygote_child; }
static unsigned overrides, reverts;
static const struct cred *override_creds(const struct cred *cred) {
    assert(cred == ksu_cred && !credentials_overridden && !profile_refs);
    credentials_overridden = true; ++overrides; return &normal_cred;
}
static void revert_creds(const struct cred *cred) {
    assert(cred == &normal_cred && credentials_overridden);
    credentials_overridden = false; ++reverts;
}
static bool from_fault, to_fault;
static unsigned from_calls, to_calls;
static int copy_from_user(void *to, const void *from, size_t size) {
    ++from_calls; if (from_fault) return 1; memcpy(to, from, size); return 0;
}
static int copy_to_user(void *to, const void *from, size_t size) {
    ++to_calls; if (to_fault) return 1; memcpy(to, from, size); return 0;
}

struct callback_head { void (*func)(struct callback_head *); };
struct file { int unused; } mountinfo_file;
static struct callback_head *pending;
static bool ksu_module_mounted, ksu_unshare_mnt_enabled, allocation_fault;
static bool parent_fallback_safe, scan_signature_mismatch, exit_during_scan;
static int parent_error, queue_error, scan_result, unshare_error;
static unsigned parent_opens, file_refs, closes, allocations, live_allocations, frees;
static unsigned queue_calls, scans, fallbacks, unshares;
static struct file *ksu_open_parent_mountinfo(bool *fallback_safe) {
    ++parent_opens; *fallback_safe = parent_fallback_safe;
    if (parent_error) return ERR_PTR(parent_error);
    assert(!file_refs); ++file_refs; return &mountinfo_file;
}
static int filp_close(struct file *file, void *owner) {
    assert(file == &mountinfo_file && !owner && file_refs == 1);
    --file_refs; ++closes; return 0;
}
static void *kzalloc(size_t size, int flags) {
    assert(flags == GFP_KERNEL); ++allocations;
    if (allocation_fault) return NULL;
    void *p = calloc(1, size); assert(p); ++live_allocations; return p;
}
static void kfree(void *p) {
    assert(p && live_allocations); --live_allocations; ++frees; free(p);
}
static int task_work_add(struct task_struct *target, struct callback_head *cb, int mode) {
    assert(target == current && mode == TWA_RESUME && !pending); ++queue_calls;
    if (queue_error) return queue_error;
    pending = cb; return 0;
}
static int ksu_umount_scan_mountinfo(struct file *file, bool *mismatch) {
    assert(credentials_overridden && file_refs == 1); ++scans;
    *mismatch = scan_signature_mismatch;
    if (exit_during_scan) current->flags |= PF_EXITING;
    filp_close(file, NULL); return scan_result;
}
static void ksu_umount_mount_list(void) { assert(credentials_overridden); ++fallbacks; }
static int mock_unshare(unsigned flags) {
    assert(credentials_overridden && flags == CLONE_NEWNS && (scans || fallbacks));
    ++unshares; return unshare_error;
}
#define ksyscall(name, arg) mock_##name(arg)
'''


TESTS = r'''
static void reset_policy(void) {
    assert(!profile_refs && !credentials_overridden);
    memset(&app_profile, 0, sizeof(app_profile));
    has_profile = allow_uid = manager_valid = webview_enabled = false;
    default_non_root_profile.umount_modules = true;
    sumh_enabled = true; resolving_rules = false;
    profile_gets = profile_puts = allow_queries = webview_queries = 0;
    queried_uid = UINT32_MAX;
}
static void reset_runtime(void) {
    assert(!pending && !file_refs && !live_allocations && !credentials_overridden);
    task = (struct task_struct){.real_uid = 10001, .pid = 123, .fs = &task, .nsproxy = &task};
    normal_cred.uid = task.real_uid; root_cred.uid = 0; ksu_cred = &root_cred;
    ksu_module_mounted = zygote_child = parent_fallback_safe = true;
    ksu_unshare_mnt_enabled = allocation_fault = scan_signature_mismatch = exit_during_scan = false;
    parent_error = queue_error = unshare_error = 0; scan_result = 1;
    parent_opens = closes = allocations = frees = queue_calls = 0;
    scans = fallbacks = unshares = overrides = reverts = 0;
    from_fault = to_fault = false; from_calls = to_calls = 0;
}
static void finish_work(void) {
    assert(pending); struct callback_head *cb = pending; pending = NULL;
    cb->func(cb);
    assert(!live_allocations && !file_refs && !credentials_overridden && !profile_refs);
}
static void raw_result(uid_t uid, bool expected, bool used_profile) {
    unsigned gets = profile_gets, puts = profile_puts;
    assert(ksu_app_profile_should_umount(uid) == expected && !profile_refs);
    assert(profile_gets == gets + (used_profile ? 1u : 0u));
    assert(profile_puts == puts + (used_profile && has_profile ? 1u : 0u));
}
static void test_raw_profiles(void) {
    reset_policy();
#ifdef CONFIG_KSU_DISABLE_POLICY
    // Profile/default settings have no effect in the allowlist-only build.
    for (unsigned mask = 0; mask < 64; ++mask) {
        has_profile = mask & 1; default_non_root_profile.umount_modules = mask & 2;
        app_profile.allow_su = mask & 4; app_profile.nrp_config.use_default = mask & 8;
        app_profile.nrp_config.profile.umount_modules = mask & 16; allow_uid = mask & 32;
        raw_result(110001, !allow_uid, false); assert(queried_uid == 110001);
    }
    assert(allow_queries == 64);
#else
    struct profile_case { bool present, root, use_default, own, fallback, expected; } cases[] = {
        {false, false, false, false, false, false},
        {false, false, false, false, true, true},
        {true, false, true, false, true, true},
        {true, false, true, true, false, false},
        {true, false, false, true, false, true},
        {true, false, false, false, true, false},
        {true, true, false, true, true, false},
        {true, true, true, true, true, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        struct profile_case c = cases[i];
        has_profile = c.present; app_profile.allow_su = c.root;
        app_profile.nrp_config.use_default = c.use_default;
        app_profile.nrp_config.profile.umount_modules = c.own;
        default_non_root_profile.umount_modules = c.fallback;
        raw_result(110001, c.expected, true); assert(queried_uid == 110001);
    }
    assert(!allow_queries);
#endif
    // Manager signatures are exempt before profile or allowlist access.
    reset_policy(); manager_valid = true;
    for (size_t i = 0; i < sizeof(manager_uids) / sizeof(*manager_uids); ++i)
        raw_result(manager_uids[i], false, false);
    assert(!profile_gets && !allow_queries);
    manager_valid = false;
#ifdef CONFIG_KSU_DISABLE_POLICY
    raw_result(manager_uids[0], true, false);
#else
    raw_result(manager_uids[0], true, true);
#endif
}
static void expect_decision(uid_t uid, bool expected) {
    reset_runtime(); current->real_uid = uid;
    assert(sumh_policy_uid_should_umount(uid) == expected);
    assert(sumh_policy_uid_is_spoof_target(uid) == expected);
    assert(sumh_policy_current_is_spoof_target() == expected);
    assert(sumh_policy_current_is_view_target() == !expected);
    struct ksu_uid_should_umount_cmd cmd = {.uid = uid, .should_umount = 0xaa};
    assert(!do_uid_should_umount(&cmd) && cmd.uid == uid && cmd.should_umount == expected);
    assert(from_calls == 1 && to_calls == 1);
    // The setuid transition must decide on new_uid, not the old zygote UID.
    assert(!ksu_handle_umount(0, uid));
    assert(queue_calls == (expected ? 1u : 0u));
    assert(parent_opens == (expected ? 1u : 0u));
    if (expected) {
        struct umount_tw *tw = container_of(pending, struct umount_tw, cb);
        assert(tw->uid == uid); finish_work();
        assert(overrides == 1 && reverts == 1 && scans == 1 && closes == 1 && frees == 1);
    }
}
static void test_uid_decisions(void) {
    reset_policy();
    struct uid_case { uid_t appid; bool expected; } cases[] = {
        {0, false}, {1, false}, {1000, false}, {1053, false}, {2000, false},
        {9999, false}, {10000, true}, {19999, true}, {20000, false},
        {89999, false}, {90000, true}, {98999, true}, {99000, true}, {99999, true},
    };
    const uid_t users[] = {0, 1, 42};
    for (size_t u = 0; u < sizeof(users) / sizeof(*users); ++u) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
            uid_t uid = users[u] * 100000 + cases[i].appid;
            expect_decision(uid, cases[i].expected);
            assert(sumh_policy_current_is_isolated() == (cases[i].appid >= 90000));
        }
    }
    expect_decision(UINT32_MAX, false);
    webview_enabled = true;
    expect_decision(1053, true); expect_decision(101053, false);
    manager_valid = true;
    expect_decision(manager_uids[0], false); expect_decision(manager_uids[1], false);
    expect_decision(110002, true); // Same app ID in another user is not this manager UID.
    has_profile = true; app_profile.allow_su = true; allow_uid = true;
    expect_decision(10001, false); expect_decision(110001, false);
    // Isolated processes always isolate even when a mock root profile exists.
    expect_decision(90000, true); expect_decision(199999, true);
    reset_policy(); has_profile = true;
    app_profile.nrp_config.profile.umount_modules = false; allow_uid = true;
    expect_decision(10001, false);
    app_profile.nrp_config.profile.umount_modules = true; allow_uid = false;
    expect_decision(10001, true);

    // Temporary SUMH routing suppression must not change persistent app decisions.
    for (unsigned flags = 0; flags < 4; ++flags) {
        reset_runtime(); sumh_enabled = flags & 1; resolving_rules = flags & 2;
        assert(sumh_policy_uid_should_umount(10001));
        assert(sumh_policy_uid_is_spoof_target(10001));
        assert(sumh_policy_current_scope() ==
               ((sumh_enabled && !resolving_rules) ? SUMH_POLICY_SCOPE_SPOOF : SUMH_POLICY_SCOPE_NONE));
        struct ksu_uid_should_umount_cmd cmd = {.uid = 10001};
        assert(!do_uid_should_umount(&cmd) && cmd.should_umount);
        assert(!ksu_handle_umount(0, 10001) && pending); finish_work();
        assert(scans == 1 && overrides == 1);
    }
}
static void test_query_faults(void) {
    reset_policy(); reset_runtime();
    struct ksu_uid_should_umount_cmd cmd = {.uid = 10001, .should_umount = 0xaa};
    from_fault = true;
    assert(do_uid_should_umount(&cmd) == -EFAULT && cmd.should_umount == 0xaa);
    assert(from_calls == 1 && !to_calls && !profile_gets && !allow_queries);
    from_fault = false; to_fault = true;
    assert(do_uid_should_umount(&cmd) == -EFAULT && cmd.should_umount == 0xaa);
    assert(from_calls == 2 && to_calls == 1 && !profile_refs);
}
static void test_enqueue_cleanup(void) {
    reset_policy();
    for (unsigned gate = 0; gate < 3; ++gate) {
        reset_runtime();
        if (gate == 0) ksu_module_mounted = false;
        if (gate == 1) ksu_cred = NULL;
        if (gate == 2) zygote_child = false;
        assert(!ksu_handle_umount(0, 10001));
        assert(!parent_opens && !allocations && !pending);
    }
    reset_runtime(); parent_error = -EPERM; parent_fallback_safe = false;
    assert(!ksu_handle_umount(0, 10001) && parent_opens == 1 && !allocations && !pending);
    for (unsigned failed_open = 0; failed_open < 2; ++failed_open) {
        reset_runtime(); parent_error = failed_open ? -ENOENT : 0; allocation_fault = true;
        assert(!ksu_handle_umount(0, 10001));
        assert(allocations == 1 && !queue_calls && !live_allocations && !file_refs);
        assert(closes == (failed_open ? 0u : 1u));
        reset_runtime(); parent_error = failed_open ? -ENOENT : 0; queue_error = -ESRCH;
        assert(!ksu_handle_umount(0, 10001));
        assert(queue_calls == 1 && frees == 1 && !pending && !live_allocations && !file_refs);
        assert(closes == (failed_open ? 0u : 1u));
    }
}
static void test_worker_rechecks(void) {
    for (unsigned change = 0; change < 6; ++change) {
        reset_policy(); reset_runtime();
        if (change == 5) { webview_enabled = true; current->real_uid = 1053; }
        assert(!ksu_handle_umount(0, current->real_uid) && pending);
        switch (change) {
        case 0: current->real_uid = 10004; break;
        case 1: current->flags |= PF_EXITING; break;
        case 2: has_profile = true; allow_uid = true; break; // Custom toggle disabled.
        case 3: has_profile = true; app_profile.allow_su = true; allow_uid = true; break;
        case 4: current->real_uid = 0; break;
        case 5: webview_enabled = false; break;
        }
        finish_work();
        assert(!overrides && !reverts && !scans && !fallbacks && !unshares);
        assert(closes == 1 && frees == 1);
    }
    // Manager recognition may also change while deferred work is pending.
    reset_policy(); reset_runtime(); current->real_uid = manager_uids[0];
    assert(!ksu_handle_umount(0, current->real_uid) && pending);
    manager_valid = true; finish_work(); assert(!overrides && closes == 1 && frees == 1);
    // Default profile revocation in the policy-enabled build follows the same path.
#ifndef CONFIG_KSU_DISABLE_POLICY
    reset_policy(); reset_runtime(); assert(!ksu_handle_umount(0, 10001) && pending);
    default_non_root_profile.umount_modules = false;
    finish_work(); assert(!overrides && closes == 1 && frees == 1);
#endif
}
static void test_worker_paths(void) {
    // A successful scan, empty scan, mismatch, read error and unavailable parent.
    struct scan_case { int count, open_error; bool mismatch, fallback; } cases[] = {
        {1, 0, false, false}, {0, 0, false, true}, {0, 0, true, false},
        {-EIO, 0, false, true}, {0, -ENOENT, false, true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        reset_policy(); reset_runtime(); scan_result = cases[i].count;
        parent_error = cases[i].open_error; scan_signature_mismatch = cases[i].mismatch;
        ksu_unshare_mnt_enabled = true;
        assert(!ksu_handle_umount(0, 10001) && pending); finish_work();
        assert(overrides == 1 && reverts == 1 && frees == 1 && unshares == 1);
        assert(scans == (parent_error ? 0u : 1u) && closes == scans);
        assert(fallbacks == (cases[i].fallback ? 1u : 0u));
    }
    for (unsigned condition = 0; condition < 5; ++condition) {
        reset_policy(); reset_runtime(); ksu_unshare_mnt_enabled = condition != 0;
        if (condition == 1) current->fs = NULL;
        if (condition == 2) current->nsproxy = NULL;
        if (condition == 3) exit_during_scan = true;
        if (condition == 4) unshare_error = -ENOMEM;
        assert(!ksu_handle_umount(0, 10001) && pending); finish_work();
        assert(scans == 1 && overrides == 1 && reverts == 1 && frees == 1);
        assert(unshares == (condition == 4 ? 1u : 0u));
    }
}
int main(void) {
    test_raw_profiles(); test_uid_decisions(); test_query_faults();
    test_enqueue_cleanup(); test_worker_rechecks(); test_worker_paths();
#ifdef CONFIG_KSU_DISABLE_POLICY
    puts("PASS app unmount: allowlist-only policy, UID/query/routing parity and deferred lifecycle");
#else
    puts("PASS app unmount: profile policy, UID/query/routing parity and deferred lifecycle");
#endif
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--zig", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    compiler = [str(args.zig), "cc"] if args.zig else [args.cc]
    code = PRELUDE
    header = (ROOT / "kernel/policy/allowlist.h").read_text(encoding="utf-8")
    for name in ("PER_USER_RANGE", "WEBVIEW_ZYGOTE_UID", "FIRST_APPLICATION_UID", "LAST_APPLICATION_UID",
                 "FIRST_APP_ZYGOTE_ISOLATED_UID", "FIRST_ISOLATED_UID", "LAST_ISOLATED_UID"):
        match = re.search(r"^#define\s+" + name + r"\s+\d+\s*$", header, re.M)
        if not match:
            raise ValueError(f"missing UID constant {name}")
        code += match.group() + "\n"
    code += declaration("kernel/sumh/policy/sumh_path_policy.h", "enum", "sumh_policy_scope")
    code += declaration("uapi/supercall.h", "struct", "ksu_uid_should_umount_cmd")
    code += declaration("kernel/feature/kernel_umount.c", "enum", "ksu_umount_scan_source")
    code += declaration("kernel/feature/kernel_umount.c", "struct", "umount_tw")
    for name in ("is_appuid", "is_isolated_process"):
        code += function("kernel/policy/allowlist.h", name)
    code += function("kernel/policy/allowlist.c", "ksu_app_profile_should_umount")
    for name in ("sumh_policy_uid_should_umount", "sumh_policy_scope_for_uid", "sumh_policy_current_scope",
                 "sumh_policy_current_is_view_target", "sumh_policy_current_is_spoof_target",
                 "sumh_policy_uid_is_spoof_target", "sumh_policy_current_is_isolated"):
        code += function("kernel/sumh/policy/sumh_path_policy.c", name)
    code += function("kernel/supercall/dispatch.c", "do_uid_should_umount")
    for name in ("umount_tw_func", "ksu_handle_umount"):
        code += function("kernel/feature/kernel_umount.c", name)
    code += TESTS
    sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix="sumh-app-umount-") as tmp:
        build = Path(tmp)
        src = build / "test.c"
        src.write_text(code, encoding="utf-8")
        for disabled in (False, True):
            binary = build / ("test-" + str(int(disabled)) + (".exe" if os.name == "nt" else ""))
            defines = ["-DCONFIG_KSU_DISABLE_POLICY"] if disabled else []
            subprocess.run([*compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                            "-Wno-unused-parameter", *sanitize, *defines, str(src), "-o", str(binary)],
                           check=True, timeout=120)
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
