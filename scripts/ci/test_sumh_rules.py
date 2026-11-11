#!/usr/bin/env python3
"""Exercise SUMH rule-dump allocation and cleanup with host kernel API doubles.

The actual LIST_RULES branch is compiled with a fragmented-allocation model.
These checks cover its error paths, not real kernel memory pressure or KMI.
"""

import argparse
import os
from pathlib import Path
import shutil
import sys

# Reuse the freestanding runner without leaving bytecode in the source tree.
sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, function, run_test


def rules_source():
    preamble = r"""
#define ENOMEM 12
#define EFAULT 14
#define GFP_KERNEL 0
#define SUMH_IOC_LIST_RULES 1
#define SUMH_PROTOCOL_VERSION 3
#define SUMH_FEATURE_MOUNT_HIDE 1
#define SUMH_FEATURE_MAPS_SPOOF 2
#define SUMH_FEATURE_STATFS_SPOOF 4
#define SUMH_MOUNT_HIDE_MODE_AGGRESSIVE 1
#define READ_ONCE(value) (value)
#define CAPACITY (64 * 1024)
struct sumh_syscall_list_arg { char *buf; size_t size; };
struct sumh_entry { const char *src, *target; int type; };
struct sumh_hide_entry { const char *path; };
struct sumh_inject_entry { const char *dir; };
struct sumh_merge_entry { const char *src, *target; };
struct sumh_xattr_sb_entry { void *sb; };
static struct sumh_entry rule = { "/system/test", "/data/adb/modules/demo/system/test", 0 };
static struct sumh_entry *sumh_paths = &rule;
static struct sumh_hide_entry *sumh_hide_paths;
static struct sumh_inject_entry *sumh_inject_dirs;
static struct sumh_merge_entry *sumh_merge_dirs;
static struct sumh_xattr_sb_entry *sumh_xattr_sbs;
static int sumh_enabled = 1, sumh_feature_enabled_mask;
static int sumh_proc_proxy_registered, sumh_mount_hide_vfsmnt_registered;
static int sumh_mount_hide_mountinfo_registered, statfs_ready;
static bool sumh_statfs_view_available(void) { return statfs_ready; }
static bool sumh_kernel_build_enabled(void) { return false; }
#define hash_for_each_rcu(table, bkt, pos, member) \
    for ((bkt) = 0, (pos) = (table); (pos); (pos) = NULL)

static char allocation[CAPACITY + 1], output[CAPACITY + 1];
static struct sumh_syscall_list_arg request;
static size_t allocated_size, copied_size;
static int fragmented, allocation_fails, virtual_backing, live;
static int contiguous_attempts, fallback_attempts, frees, bad_context;
static int rcu_depth, rcu_entries, copy_calls, copy_fault, input_fault;

static void rcu_read_lock(void) {
    if (rcu_depth || !live) bad_context++;
    rcu_depth++; rcu_entries++;
}
static void rcu_read_unlock(void) {
    if (rcu_depth != 1) bad_context++;
    rcu_depth--;
}
static void *allocate(size_t n, int virtual_memory) {
    if (n > CAPACITY || live) { bad_context++; return NULL; }
    allocated_size = n; virtual_backing = virtual_memory; live = 1;
    memset(allocation, 0, n);
    allocation[n] = 'G';
    return allocation;
}
static void *kzalloc(size_t n, int flags) {
    if (rcu_depth || flags != GFP_KERNEL) bad_context++;
    contiguous_attempts++;
    if (allocation_fails || fragmented) return NULL;
    return allocate(n, 0);
}
static void *kvzalloc(size_t n, int flags) {
    void *p = kzalloc(n, flags);
    if (p) return p;
    fallback_attempts++;
    return allocation_fails ? NULL : allocate(n, 1);
}
static void kvfree(void *p) {
    if (rcu_depth || p != allocation || !live || allocation[allocated_size] != 'G')
        bad_context++;
    live = 0; frees++;
}
static void kfree(void *p) {
    if (virtual_backing) bad_context++;
    kvfree(p);
}
static int copy_from_user(void *out, const void *in, size_t n) {
    if (rcu_depth || in != &request || n != sizeof(request)) bad_context++;
    if (input_fault) return 1;
    memcpy(out, in, n); return 0;
}
static int copy_to_user(void *out, const void *in, size_t n) {
    if (rcu_depth || !live) bad_context++;
    copy_calls++;
    if (copy_calls == 1) {
        if (out != output || in != allocation || n > allocated_size) bad_context++;
        copied_size = n;
    } else if (copy_calls != 2 || out != &request || n != sizeof(request)) {
        bad_context++;
    }
    if (copy_calls == copy_fault) return 1;
    memcpy(out, in, n); return 0;
}
/* The exercised records use only strings and single-digit integers. */
static int scnprintf(char *out, size_t n, const char *format, ...) {
    __builtin_va_list args;
    size_t written = 0;
    if (rcu_depth != 1) bad_context++;
    __builtin_va_start(args, format);
    while (*format) {
        char digit[2];
        const char *text;
        size_t count;
        if (*format != '%') {
            text = format++; count = 1;
        } else {
            format++;
            if (*format == 's') {
                text = __builtin_va_arg(args, const char *); count = strlen(text);
            } else if (*format == 'd') {
                int value = __builtin_va_arg(args, int);
                if (value < 0 || value > 9) bad_context++;
                digit[0] = '0' + value; text = digit; count = 1;
            } else {
                bad_context++; break;
            }
            format++;
        }
        for (size_t i = 0; i < count; i++)
            if (written + 1 < n) out[written++] = text[i];
    }
    __builtin_va_end(args);
    if (n) out[written] = 0;
    return written;
}
static void reset(size_t size) {
    request.buf = output; request.size = size;
    memset(output, 'Y', sizeof(output));
    allocated_size = copied_size = 0;
    fragmented = 1;
    allocation_fails = virtual_backing = live = 0;
    contiguous_attempts = fallback_attempts = frees = bad_context = 0;
    rcu_depth = rcu_entries = copy_calls = copy_fault = input_fault = 0;
}
static bool clean(void) { return !live && !rcu_depth && !bad_context; }
static long list_rules(unsigned int cmd, void *arg) {
    struct sumh_entry *entry;
    struct sumh_hide_entry *hide_entry;
    struct sumh_inject_entry *inject_entry;
"""
    branch = function("kernel/sumh/control/sumh_ioctl.c", "if (cmd == SUMH_IOC_LIST_RULES)")
    body = r"""
    return -1;
}
int main(void) {
    static const char expected[] = "SUMH Protocol: 3\nSUMH Enabled: 1\n"
        "add /system/test /data/adb/modules/demo/system/test 0\n"
        "stealth enabled\n";
    const size_t length = sizeof(expected) - 1;

    /* Fragmentation alone must not turn a small rule dump into ENOMEM. */
    reset(CAPACITY);
    CHECK(list_rules(SUMH_IOC_LIST_RULES, &request) == 0);
    CHECK(contiguous_attempts == 1 && fallback_attempts == 1 && virtual_backing);
    CHECK(allocated_size == CAPACITY && request.size == length && copied_size == length);
    CHECK(!strncmp(output, expected, length) && output[length] == 'Y');
    CHECK(frees == 1 && copy_calls == 2 && rcu_entries == 1 && clean());

    reset(CAPACITY); fragmented = 0;
    CHECK(list_rules(SUMH_IOC_LIST_RULES, &request) == 0);
    CHECK(contiguous_attempts == 1 && !fallback_attempts && !virtual_backing);
    CHECK(request.size == length && frees == 1 && clean());

    reset(CAPACITY); allocation_fails = 1;
    CHECK(list_rules(SUMH_IOC_LIST_RULES, &request) == -ENOMEM);
    CHECK(!frees && !copy_calls && !rcu_entries && clean());

    /* Both user-copy exits must release even vmalloc-backed buffers. */
    for (int fault = 1; fault <= 2; fault++) {
        reset(CAPACITY); copy_fault = fault;
        CHECK(list_rules(SUMH_IOC_LIST_RULES, &request) == -EFAULT);
        CHECK(copy_calls == fault && request.size == CAPACITY);
        CHECK(frees == 1 && virtual_backing && rcu_entries == 1 && clean());
    }
    reset(CAPACITY); input_fault = 1;
    CHECK(list_rules(SUMH_IOC_LIST_RULES, &request) == -EFAULT);
    CHECK(!contiguous_attempts && !copy_calls && !frees && clean());

    reset(CAPACITY + 1);
    CHECK(list_rules(SUMH_IOC_LIST_RULES, &request) == 0);
    CHECK(allocated_size == CAPACITY && request.size == length && frees == 1 && clean());
    for (size_t size = 0; size <= 8; size++) {
        reset(size);
        CHECK(list_rules(SUMH_IOC_LIST_RULES, &request) == 0);
        CHECK(request.size == (size ? size - 1 : 0));
        CHECK(copied_size == request.size && output[request.size] == 'Y');
        CHECK(!strncmp(output, expected, request.size) && frees == 1 && clean());
    }
    /* Built-in protections still report active with an empty rule set and
     * no optional features enabled, including after a rule cleanup. */
    reset(CAPACITY);
    sumh_paths = NULL;
    sumh_feature_enabled_mask = 0;
    sumh_proc_proxy_registered = statfs_ready = 1;
    static const char fixed[] = "SUMH Protocol: 3\nSUMH Enabled: 1\n"
        "mount_hide enabled mode=normal\nstatfs_spoof enabled\nstealth enabled\n";
    CHECK(list_rules(SUMH_IOC_LIST_RULES, &request) == 0);
    CHECK(request.size == sizeof(fixed) - 1 && !strncmp(output, fixed, request.size));
    CHECK(frees == 1 && clean());
    return 0;
}
"""
    return COMMON + preamble + branch + body


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler, "rules", rules_source())
