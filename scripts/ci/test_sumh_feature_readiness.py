#!/usr/bin/env python3
"""Run SUMH capability and ioctl guards against production C fragments.

Inject missing symbols, probe registration failures and lifecycle transitions.
These are kernel-API doubles, not a kernel build or per-observer device proof.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test


def readiness_source():
    preamble = r"""
typedef unsigned int __u32;
#define __user
#define EINVAL 22
#define EFAULT 14
#define ENOSYS 38
#define EOPNOTSUPP 95
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v, n) ((v) = (n))
#define atomic_read(p) (*(p))
#define atomic_set(p, n) (*(p) = (n))
#define atomic64_set(p, n) (*(p) = (n))
#define pr_info(...) ((void)0)
#define pr_warn(...) ((void)0)
#define sumh_log(...) ((void)0)
#define WARN_ON_ONCE(condition) ((condition) ? (warnings++, 1) : 0)
#define _IOC(dir, type, nr, size) (nr)
#define _IO(type, nr) (nr)
#define _IOR(type, nr, size) (nr)
#define _IOW(type, nr, size) (nr)
#define _IOWR(type, nr, size) (nr)
typedef char kprobe_opcode_t;
struct kprobe { kprobe_opcode_t *addr; int (*pre_handler)(void); };
static struct kprobe sumh_kp_fd_install;
static int sumh_proxy_hooks_lock, lock_depth, bad_context, warnings;
static unsigned int sumh_proxy_hook_users;
static bool sumh_fd_install_registered;
static int sumh_proc_proxy_registered, sumh_proxy_shutdown, sumh_proxy_live;
static int sumh_feature_enabled_mask, invalidations;
static int missing_ingress, registration_error, registrations, unregistrations, drains;
static int missing_mountinfo, missing_mounts;
static bool fake_mi_initialized;
static unsigned long long fake_mi_view_gen;
static void *sumh_mi_mountinfo_raw, *sumh_mi_mounts_raw;
static int available_hide, available_build, available_xattr, available_statfs;
static int from_fault, to_fault;
static void mutex_lock(int *lock) {
    if (lock != &sumh_proxy_hooks_lock || lock_depth) bad_context++;
    lock_depth++;
}
static void mutex_unlock(int *lock) {
    if (lock != &sumh_proxy_hooks_lock || lock_depth != 1) bad_context++;
    lock_depth--;
}
static unsigned long sumh_lookup_name_quiet(const char *name) {
    if (strcmp(name, "fd_install")) bad_context++;
    return missing_ingress ? 0 : 1;
}
static unsigned long sumh_lookup_name(const char *name) {
    if (!strcmp(name, "show_mountinfo")) return missing_mountinfo ? 0 : 2;
    if (!strcmp(name, "show_vfsmnt")) return missing_mounts ? 0 : 3;
    bad_context++; return 0;
}
static int sumh_fd_install_pre(void) { return 0; }
static int register_kprobe(struct kprobe *probe) {
    if (lock_depth != 1 || probe != &sumh_kp_fd_install || !probe->addr ||
        probe->pre_handler != sumh_fd_install_pre) bad_context++;
    registrations++; return registration_error;
}
static void unregister_kprobe(struct kprobe *probe) {
    if (lock_depth != 1 || probe != &sumh_kp_fd_install) bad_context++;
    unregistrations++;
}
static void sumh_mount_proxy_drain(void) {
    if (lock_depth != 1) bad_context++;
    drains++;
}
static int copy_from_user(void *out, const void *in, size_t size) {
    if (from_fault) return 1;
    memcpy(out, in, size); return 0;
}
static int copy_to_user(void *out, const void *in, size_t size) {
    if (to_fault) return 1;
    memcpy(out, in, size); return 0;
}
static bool sumh_hide_rules_available(void) { return available_hide; }
static bool sumh_kernel_build_available(void) { return available_build; }
static bool sumh_overlay_xattr_available(void) { return available_xattr; }
static bool sumh_statfs_view_available(void) { return available_statfs; }
static void sumh_fake_mi_invalidate_all(void) { invalidations++; }
"""
    uapi = (ROOT / "uapi/sumh.h").read_text(encoding="utf-8")
    # Keep real feature bits, command numbers and ABI structures; only kernel
    # services are doubled. A bypass of an ioctl guard must fail these tests.
    defines = "\n".join(re.findall(
        r"^#define SUMH_(?:FEATURE_\w+|IOC_\w+|MAX_LEN_PATHNAME)\b[^\n]*(?:\n[^\n]*)*?$(?<!\\)",
        uapi, re.M))
    structs = "\n".join(function("uapi/sumh.h", marker) + ";" for marker in (
        "struct sumh_maps_rule {", "struct sumh_mount_hide_arg {", "struct sumh_maps_spoof_arg {",
        "struct sumh_statfs_spoof_arg {"))
    lifecycle = "\n".join(function("kernel/sumh/hooks/sumh_proc_read_hooks.c", marker)
                          for marker in ("bool sumh_proc_proxy_available(",
                                         "int sumh_proc_proxy_get(",
                                         "void sumh_proc_proxy_put("))
    lifecycle += "\n" + function("kernel/sumh/features/sumh_fake_mountinfo.c",
                                  "int sumh_fake_mi_init(")
    lifecycle += "\n" + function("kernel/sumh/features/sumh_fake_mountinfo.c",
                                  "bool sumh_fake_mi_active(")
    lifecycle += "\n" + function("kernel/sumh/control/sumh_ioctl.c",
                                  "static bool sumh_mount_hide_available(")
    dispatch = "\nstatic int dispatch(unsigned int cmd, void *arg) {\n"
    for name in ("SET_MOUNT_HIDE", "SET_MAPS_SPOOF", "SET_STATFS_SPOOF", "GET_FEATURES"):
        dispatch += function("kernel/sumh/control/sumh_ioctl.c", f"if (cmd == SUMH_IOC_{name})") + "\n"
    dispatch += "return -EINVAL;\n}\n"
    body = r"""
static int features(void) {
    int mask = -1;
    return dispatch(SUMH_IOC_GET_FEATURES, &mask) ? -1 : mask;
}
int main(void) {
    struct sumh_maps_spoof_arg maps = {0};
    struct sumh_mount_hide_arg mount = {0};
    struct sumh_statfs_spoof_arg statfs = {0};
    int supported, mask, before;
    const int preserved = 1 << 29;
    const int proc_bits = SUMH_FEATURE_MAPS_SPOOF | SUMH_FEATURE_MANAGED_MAPS;
    const int mount_bits = SUMH_FEATURE_MOUNT_HIDE | SUMH_FEATURE_FAKE_MOUNTINFO;

    sumh_feature_enabled_mask = preserved;
    missing_ingress = 1;
    CHECK(sumh_proc_proxy_get() == -ENOSYS);
    CHECK(!registrations && !sumh_proxy_hook_users && !sumh_proc_proxy_available());
    CHECK(!(features() & (proc_bits | mount_bits)));
    CHECK(features() & SUMH_FEATURE_TEMPORARY_MAPS_DELETE);
    maps.enable = mount.enable = statfs.enable = 1;
    CHECK(dispatch(SUMH_IOC_SET_MAPS_SPOOF, &maps) == -EOPNOTSUPP);
    CHECK(dispatch(SUMH_IOC_SET_MOUNT_HIDE, &mount) == -EOPNOTSUPP);
    CHECK(dispatch(SUMH_IOC_SET_STATFS_SPOOF, &statfs) == -EOPNOTSUPP);
    CHECK(sumh_feature_enabled_mask == preserved && !invalidations);

    missing_ingress = 0; registration_error = -16;
    CHECK(sumh_proc_proxy_get() == -ENOSYS);
    CHECK(registrations == 1 && !sumh_proxy_hook_users && !sumh_fd_install_registered);
    CHECK(!sumh_proc_proxy_available() && !(features() & (proc_bits | mount_bits)));
    CHECK(dispatch(SUMH_IOC_SET_MAPS_SPOOF, &maps) == -EOPNOTSUPP);
    registration_error = 0;
    CHECK(sumh_proc_proxy_get() == 0 && sumh_proc_proxy_available());
    CHECK(sumh_proxy_hook_users == 1 && sumh_fd_install_registered);
    CHECK((features() & proc_bits) == proc_bits && !(features() & mount_bits));
    CHECK(dispatch(SUMH_IOC_SET_MOUNT_HIDE, &mount) == -EOPNOTSUPP);

    /* Symbols required by the mount renderer are independent of ingress. */
    missing_mountinfo = 1;
    CHECK(sumh_fake_mi_init() == -ENOSYS && !sumh_mount_hide_available());
    missing_mountinfo = 0; missing_mounts = 1;
    CHECK(sumh_fake_mi_init() == -ENOSYS && !sumh_mount_hide_available());
    CHECK(dispatch(SUMH_IOC_SET_MOUNT_HIDE, &mount) == -EOPNOTSUPP);
    missing_mounts = 0;
    CHECK(sumh_fake_mi_init() == 0 && sumh_mount_hide_available());
    CHECK((features() & (proc_bits | mount_bits)) == (proc_bits | mount_bits));

    /* Capabilities are not configured bits. Repeated updates retain other features. */
    available_statfs = available_xattr = available_build = available_hide = 1;
    supported = features();
    CHECK(supported & SUMH_FEATURE_STATFS_SPOOF);
    CHECK(sumh_feature_enabled_mask == preserved);
    for (int cycle = 0; cycle < 3; cycle++) {
        maps.enable = mount.enable = statfs.enable = 1;
        CHECK(!dispatch(SUMH_IOC_SET_MAPS_SPOOF, &maps));
        CHECK(!dispatch(SUMH_IOC_SET_MAPS_SPOOF, &maps));
        CHECK(!dispatch(SUMH_IOC_SET_MOUNT_HIDE, &mount));
        CHECK(!dispatch(SUMH_IOC_SET_STATFS_SPOOF, &statfs));
        CHECK(sumh_feature_enabled_mask == (preserved | SUMH_FEATURE_MAPS_SPOOF |
              SUMH_FEATURE_MOUNT_HIDE | SUMH_FEATURE_STATFS_SPOOF));
        CHECK(features() == supported);
        maps.enable = mount.enable = statfs.enable = 0;
        CHECK(!dispatch(SUMH_IOC_SET_MAPS_SPOOF, &maps));
        CHECK(!dispatch(SUMH_IOC_SET_MAPS_SPOOF, &maps));
        CHECK(!dispatch(SUMH_IOC_SET_MOUNT_HIDE, &mount));
        CHECK(!dispatch(SUMH_IOC_SET_STATFS_SPOOF, &statfs));
        CHECK(sumh_feature_enabled_mask == preserved && features() == supported);
    }

    from_fault = 1; maps.enable = mount.enable = statfs.enable = 1;
    before = invalidations;
    CHECK(dispatch(SUMH_IOC_SET_MAPS_SPOOF, &maps) == -EFAULT);
    CHECK(dispatch(SUMH_IOC_SET_MOUNT_HIDE, &mount) == -EFAULT);
    CHECK(dispatch(SUMH_IOC_SET_STATFS_SPOOF, &statfs) == -EFAULT);
    CHECK(sumh_feature_enabled_mask == preserved && before == invalidations);
    from_fault = 0; to_fault = 1; mask = -1;
    CHECK(dispatch(SUMH_IOC_GET_FEATURES, &mask) == -EFAULT && mask == -1);
    to_fault = 0;

    CHECK(!dispatch(SUMH_IOC_SET_MAPS_SPOOF, &maps));
    CHECK(!dispatch(SUMH_IOC_SET_MOUNT_HIDE, &mount));
    CHECK(!dispatch(SUMH_IOC_SET_STATFS_SPOOF, &statfs));
    CHECK(sumh_proc_proxy_get() == 0 && sumh_proxy_hook_users == 2 && registrations == 2);
    sumh_proc_proxy_put();
    CHECK(sumh_proc_proxy_available() && sumh_proxy_hook_users == 1 && !unregistrations);
    sumh_proc_proxy_put();
    CHECK(!sumh_proc_proxy_available() && !sumh_proxy_hook_users && unregistrations == 1 && drains == 1);
    CHECK(!sumh_fd_install_registered && !(features() & (proc_bits | mount_bits)));
    CHECK(features() & SUMH_FEATURE_TEMPORARY_MAPS_DELETE);
    available_statfs = 0;
    before = sumh_feature_enabled_mask;
    CHECK(dispatch(SUMH_IOC_SET_MAPS_SPOOF, &maps) == -EOPNOTSUPP);
    CHECK(dispatch(SUMH_IOC_SET_MOUNT_HIDE, &mount) == -EOPNOTSUPP);
    CHECK(dispatch(SUMH_IOC_SET_STATFS_SPOOF, &statfs) == -EOPNOTSUPP);
    CHECK(sumh_feature_enabled_mask == before);
    maps.enable = mount.enable = statfs.enable = 0;
    CHECK(!dispatch(SUMH_IOC_SET_MAPS_SPOOF, &maps));
    CHECK(!dispatch(SUMH_IOC_SET_MOUNT_HIDE, &mount));
    CHECK(!dispatch(SUMH_IOC_SET_STATFS_SPOOF, &statfs));
    CHECK(sumh_feature_enabled_mask == preserved);
    CHECK(!dispatch(SUMH_IOC_SET_MAPS_SPOOF, &maps));

    /* Shutdown wins even if a reader still observes the prior registered bit. */
    sumh_proc_proxy_registered = 1;
    CHECK(!sumh_proc_proxy_available() && !(features() & (proc_bits | mount_bits)));
    sumh_proc_proxy_registered = 0;
    CHECK(sumh_proc_proxy_get() == 0 && sumh_proc_proxy_available());
    CHECK(sumh_feature_enabled_mask == preserved);
    sumh_proc_proxy_put();
    CHECK(!bad_context && !lock_depth && !warnings && unregistrations == 2 && drains == 2);
    return 0;
}
"""
    return COMMON + preamble + defines + "\n" + structs + lifecycle + dispatch + body


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    source = readiness_source()
    if os.name == "nt":
        # Kernel long is pointer-width; Windows host long is only 32 bits.
        source = re.sub(r"\bunsigned long\b(?! long)", "unsigned long long", source)
    run_test(compiler, "feature-readiness", source)
