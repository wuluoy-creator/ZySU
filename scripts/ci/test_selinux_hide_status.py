#!/usr/bin/env python3
"""Exercise requested/runtime SELinux state using production feature handlers.

The hook and mutex doubles inject runtime failures without a kernel tree. This
does not replace a kernel build or on-device validation of the actual hooks.
"""

import argparse
import os
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test


def status_source():
    preamble = r"""
typedef unsigned long long u64;
#define EAGAIN 11
#define ENOSYS 38
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, value) ((x) = (value))
#define pr_info(...) ((void)0)
#define pr_warn(...) ((void)0)
static int selinux_hide_mutex;
static struct { int status_lock; } selinux_state;
static int lock_depth, lock_errors;
static void mutex_lock(int *lock) {
    if (*lock) lock_errors++;
    *lock = 1; lock_depth++;
}
static void mutex_unlock(int *lock) {
    if (!*lock) lock_errors++;
    *lock = 0; lock_depth--;
}
static bool ksu_selinux_hide_enabled, ksu_selinux_hide_running;
static int ksu_selinux_hide_last_error;
static void *fake_status, *orig_sel_open_handle_status;
static int enable_result, enable_calls, disable_calls;
static int ksu_selinux_hide_enable(void) { enable_calls++; return enable_result; }
static void ksu_selinux_hide_disable(void) { disable_calls++; }
"""
    body = r"""
int main(void) {
    u64 requested, status;
    CHECK(!selinux_hide_status_get(&status));
    CHECK(status == KSU_SELINUX_HIDE_DISABLED);

    /* No backup is expected after boot: preserve the setting for next boot. */
    enable_result = -EAGAIN;
    CHECK(!selinux_hide_feature_set(1));
    CHECK(!selinux_hide_feature_get(&requested) && requested == 1);
    CHECK(!selinux_hide_status_get(&status));
    CHECK(status == KSU_SELINUX_HIDE_PENDING_REBOOT && enable_calls == 1);

    /* A permanent hook failure must not appear active or merely pending. */
    enable_result = -ENOSYS;
    CHECK(!selinux_hide_feature_set(1));
    CHECK(!selinux_hide_feature_get(&requested) && requested == 1);
    CHECK(!selinux_hide_status_get(&status));
    CHECK(status == KSU_SELINUX_HIDE_FAILED && enable_calls == 2);

    /* Policy hooks are insufficient when the status hook or page is absent. */
    enable_result = 0;
    CHECK(!selinux_hide_feature_set(1));
    CHECK(!selinux_hide_status_get(&status));
    CHECK(status == KSU_SELINUX_HIDE_FAILED);
    fake_status = &status;
    CHECK(!selinux_hide_status_get(&status));
    CHECK(status == KSU_SELINUX_HIDE_FAILED);
    fake_status = NULL;
    orig_sel_open_handle_status = &status;
    CHECK(!selinux_hide_status_get(&status));
    CHECK(status == KSU_SELINUX_HIDE_FAILED);
    fake_status = &status;
    CHECK(!selinux_hide_status_get(&status));
    CHECK(status == KSU_SELINUX_HIDE_ACTIVE);
    CHECK(!selinux_hide_feature_set(1) && enable_calls == 3);

    CHECK(!selinux_hide_feature_set(0) && disable_calls == 1);
    CHECK(!selinux_hide_feature_get(&requested) && requested == 0);
    CHECK(!selinux_hide_status_get(&status));
    CHECK(status == KSU_SELINUX_HIDE_DISABLED);
    CHECK(!ksu_selinux_hide_last_error && !ksu_selinux_hide_running);
    CHECK(!selinux_hide_feature_set(0) && disable_calls == 1);
    CHECK(!lock_depth && !lock_errors);
    return 0;
}
"""
    handlers = "\n".join(
        function("kernel/feature/selinux_hide.c", marker)
        for marker in (
            "static int selinux_hide_feature_get",
            "static int selinux_hide_status_get",
            "static int selinux_hide_feature_set",
        )
    )
    uapi = (ROOT / "uapi/feature.h").read_text(encoding="utf-8")
    return COMMON + uapi + preamble + handlers + body


def status_open_source(minor):
    preamble = r"""
#define READ_ONCE(x) (x)
#define likely(x) (x)
#define GFP_KERNEL 1
#define __GFP_ZERO 2
#define pr_info(...) ((void)0)
#define pr_warn(...) ((void)0)
#define pr_err(...) ((void)0)
struct selinux_kernel_status {
    unsigned int version, sequence, enforcing, policyload, deny_unknown;
};
struct page { struct selinux_kernel_status status; };
struct inode { int unused; };
struct file { void *private_data; };
struct uid { unsigned int val; };
static struct { int status_lock; struct page *status_page; } selinux_state;
static struct page source_page, snapshot_page, *fake_status;
static bool ksu_selinux_hide_enabled, ksu_late_loaded;
static int fake_status_initialize_key;
static unsigned int uid;
static int errors, locks, original_calls, original_result, allocations;
static int fail_allocation, publications, acquire_reads;
static struct uid current_uid(void) { return (struct uid){uid}; }
static bool static_branch_unlikely(int *key) { return *key; }
static void mutex_lock(int *lock) {
    if (*lock) errors++;
    *lock = 1; locks++;
}
static void mutex_unlock(int *lock) {
    if (!*lock) errors++;
    *lock = 0;
}
static struct page *alloc_page(int flags) {
    if (!selinux_state.status_lock || flags != (GFP_KERNEL | __GFP_ZERO)) errors++;
    allocations++;
    return fail_allocation ? NULL : &snapshot_page;
}
static void *page_address(struct page *page) { return &page->status; }
static void publish(struct page **slot, struct page *page) {
    if (!selinux_state.status_lock || page != &snapshot_page ||
        page->status.version != source_page.status.version ||
        page->status.deny_unknown != source_page.status.deny_unknown) errors++;
    publications++; *slot = page;
}
static struct page *acquire(struct page **slot) {
    acquire_reads++; return *slot;
}
#define smp_store_release(slot, page) publish(slot, page)
#define smp_load_acquire(slot) acquire(slot)
static int orig_sel_open_handle_status(struct inode *inode, struct file *file) {
    (void)inode; original_calls++;
    if (original_result) return original_result;
    selinux_state.status_page = &source_page;
    file->private_data = &source_page;
    return 0;
}
static void reset(void) {
    errors = locks = original_calls = original_result = allocations = 0;
    fail_allocation = publications = acquire_reads = 0;
    selinux_state.status_page = NULL; selinux_state.status_lock = 0;
    fake_status = NULL; uid = 10000;
    ksu_selinux_hide_enabled = true; ksu_late_loaded = false;
    fake_status_initialize_key = 1;
    source_page.status = (struct selinux_kernel_status){1, 8, 1, 3, 1};
    memset(&snapshot_page, 0, sizeof(snapshot_page));
}
"""
    body = r"""
int main(void) {
    struct inode inode;
    struct file file;

    /* The open that first creates the real page must receive the snapshot. */
    reset(); file.private_data = NULL;
    CHECK(!my_sel_open_handle_status(&inode, &file));
    CHECK(original_calls == 1 && allocations == 1 && publications == 1);
    CHECK(file.private_data == &snapshot_page && fake_status == &snapshot_page);
    CHECK(snapshot_page.status.sequence == 8 && snapshot_page.status.policyload == 3);
    CHECK(snapshot_page.status.enforcing == 1 && !errors && !selinux_state.status_lock);

    /* Ready-page reads neither allocate nor take the status mutex. */
    locks = allocations = original_calls = acquire_reads = 0;
    CHECK(!my_sel_open_handle_status(&inode, &file));
    CHECK(file.private_data == &snapshot_page);
    CHECK(!locks && !allocations && !original_calls && acquire_reads == 1);
    source_page.status.policyload++;
    CHECK(snapshot_page.status.policyload == 3);

    /* System callers and a disabled feature always get the original page. */
    uid = 9999;
    CHECK(!my_sel_open_handle_status(&inode, &file) && file.private_data == &source_page);
    uid = 10000; ksu_selinux_hide_enabled = false;
    CHECK(!my_sel_open_handle_status(&inode, &file) && file.private_data == &source_page);
    CHECK(original_calls == 2 && !allocations && !locks);

    /* A failed open must not publish, initialize or replace private_data. */
    reset(); file.private_data = &inode; original_result = -13;
    CHECK(my_sel_open_handle_status(&inode, &file) == -13);
    CHECK(file.private_data == &inode && !locks && !allocations && !fake_status);

    /* Allocation failure falls back and can recover on the next open. */
    reset(); fail_allocation = 1;
    CHECK(!my_sel_open_handle_status(&inode, &file));
    CHECK(file.private_data == &source_page && !fake_status && !publications);
    fail_allocation = 0;
    CHECK(!my_sel_open_handle_status(&inode, &file));
    CHECK(file.private_data == &snapshot_page && publications == 1 && !errors);

    reset(); fake_status_initialize_key = 0;
    CHECK(!my_sel_open_handle_status(&inode, &file));
    CHECK(file.private_data == &source_page && !locks && !allocations);

    /* Preserve the existing early-boot enforcing requirement. */
    reset(); source_page.status.enforcing = 0;
    CHECK(!my_sel_open_handle_status(&inode, &file));
    CHECK(file.private_data == &source_page && !fake_status && !allocations);

    reset(); ksu_late_loaded = true; source_page.status.enforcing = 0;
    CHECK(!my_sel_open_handle_status(&inode, &file));
    CHECK(file.private_data == &snapshot_page && snapshot_page.status.enforcing == 1);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
    CHECK(snapshot_page.status.sequence == 4 && snapshot_page.status.policyload == 1);
#else
    CHECK(snapshot_page.status.sequence == 0 && snapshot_page.status.policyload == 0);
#endif
    CHECK(!errors && !selinux_state.status_lock);
    return 0;
}
"""
    source = "\n".join(
        function("kernel/feature/selinux_hide.c", marker)
        for marker in ("static void initialize_fake_status", "static int my_sel_open_handle_status")
    )
    version = "#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))\n"
    version += f"#define LINUX_VERSION_CODE KERNEL_VERSION(6,{minor},0)\n"
    return COMMON + version + preamble + source + body


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler, "selinux-hide-status", status_source())
    for minor in (1, 6, 12, 18):
        run_test(compiler, f"selinux-hide-status-open-6.{minor}", status_open_source(minor))
