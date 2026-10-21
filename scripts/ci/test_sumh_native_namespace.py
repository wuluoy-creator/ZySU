#!/usr/bin/env python3
"""Check capability withdrawal for the retired namespace text projection.

Compile the production ioctl branches with host doubles. Runtime namespace
identity still requires readlink/stat/fstat checks on an Android test device.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test


def namespace_source():
    preamble = r"""
#define EFAULT 14
#define EINVAL 22
#define EOPNOTSUPP 95
#define ENOTTY 25
#define SUMH_IOC_SET_MOUNT_HIDE_MODE 1
#define SUMH_IOC_GET_FEATURES 2
#define SUMH_MOUNT_HIDE_MODE_NORMAL 0
#define SUMH_MOUNT_HIDE_MODE_AGGRESSIVE 1
#define WRITE_ONCE(value, new_value) ((value) = (new_value))
#define READ_ONCE(value) (value)
#define atomic_read(value) (*(value))
#define sumh_log(...) ((void)0)
static int sumh_mount_hide_mode, invalidations, copy_fault, ready;
static int sumh_proc_proxy_registered, sumh_proxy_shutdown;
static bool sumh_hide_rules_available(void) { return ready; }
static bool sumh_kernel_build_available(void) { return ready; }
static bool sumh_overlay_xattr_available(void) { return ready; }
static bool sumh_fake_mi_active(void) { return ready; }
static bool sumh_statfs_view_available(void) { return ready; }
static void sumh_fake_mi_invalidate_all(void) { invalidations++; }
static int copy_from_user(void *out, const void *in, size_t n) {
    if (copy_fault) return 1;
    memcpy(out, in, n); return 0;
}
static int copy_to_user(void *out, const void *in, size_t n) {
    return copy_from_user(out, in, n);
}
"""
    branches = "\n".join(
        function("kernel/sumh/control/sumh_ioctl.c", marker)
        for marker in (
            "if (cmd == SUMH_IOC_SET_MOUNT_HIDE_MODE)",
            "if (cmd == SUMH_IOC_GET_FEATURES)",
        )
    )
    control = function("kernel/sumh/hooks/sumh_proc_read_hooks.c", "bool sumh_proc_proxy_available(")
    control += function("kernel/sumh/control/sumh_ioctl.c", "static bool sumh_mount_hide_available(")
    control += "static int control(int cmd, void *arg) {\n" + branches + "\nreturn -ENOTTY;\n}\n"
    body = r"""
int main(void) {
    int mode = SUMH_MOUNT_HIDE_MODE_AGGRESSIVE, features;
    CHECK(control(SUMH_IOC_SET_MOUNT_HIDE_MODE, &mode) == -EOPNOTSUPP);
    CHECK(sumh_mount_hide_mode == SUMH_MOUNT_HIDE_MODE_NORMAL && !invalidations);
    mode = 42;
    CHECK(control(SUMH_IOC_SET_MOUNT_HIDE_MODE, &mode) == -EINVAL);
    CHECK(!invalidations);
    mode = SUMH_MOUNT_HIDE_MODE_NORMAL;
    copy_fault = 1;
    CHECK(control(SUMH_IOC_SET_MOUNT_HIDE_MODE, &mode) == -EFAULT);
    CHECK(!invalidations);
    copy_fault = 0;
    CHECK(!control(SUMH_IOC_SET_MOUNT_HIDE_MODE, &mode) && invalidations == 1);
    for (ready = 0; ready <= 1; ready++) {
        sumh_proc_proxy_registered = ready;
        CHECK(!control(SUMH_IOC_GET_FEATURES, &features));
        CHECK(!(features & SUMH_FEATURE_MOUNT_HIDE_AGGRESSIVE));
        CHECK(!!(features & SUMH_FEATURE_MOUNT_HIDE) == !!ready);
        CHECK(!!(features & SUMH_FEATURE_FAKE_MOUNTINFO) == !!ready);
        CHECK(!!(features & SUMH_FEATURE_MAPS_SPOOF) == !!ready);
        CHECK(!!(features & SUMH_FEATURE_MANAGED_MAPS) == !!ready);
    }
    copy_fault = 1;
    CHECK(control(SUMH_IOC_GET_FEATURES, &features) == -EFAULT);
    return 0;
}
"""
    uapi = (ROOT / "uapi/sumh.h").read_text(encoding="utf-8")
    feature_defines = "\n".join(re.findall(
        r"^#define SUMH_FEATURE_\w+\b[^\n]*(?:\n[^\n]*)*?$(?<!\\)", uapi, re.M))
    return COMMON + feature_defines + "\n" + preamble + control + body


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler, "native-namespace-capabilities", namespace_source())
