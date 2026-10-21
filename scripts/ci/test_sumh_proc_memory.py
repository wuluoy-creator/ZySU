#!/usr/bin/env python3
"""Run production SUMH proc accounting with deterministic kernel API doubles.

The lock/scheduling and allocation models are not real kernel concurrency,
physical RAM measurements, KMI builds or proof of safe proxy admission.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test


MEMORY = "kernel/sumh/core/sumh_proc_memory.c"
MOUNT = "kernel/sumh/features/sumh_fake_mountinfo.c"
PROC = "kernel/sumh/hooks/sumh_proc_read_hooks.c"


def bodies(path, names):
    # Import lazily: mount_views imports memory_source for its existing fixtures.
    from test_sumh_mount_views import extract
    return "\n".join(extract(path, name) for name in names)


def declaration(path, marker):
    return function(path, marker) + ";\n"


def defines(path, prefix):
    return "\n".join(line for line in (ROOT / path).read_text(encoding="utf-8").splitlines()
                     if line.startswith("#define " + prefix)) + "\n"


def memory_source():
    """Actual helper, shared by this runner and existing policy regressions."""
    header = (ROOT / "kernel/sumh/core/sumh_proc_memory.h").read_text(encoding="utf-8")
    header = re.sub(r"^#include[^\n]*\n", "", header, flags=re.MULTILINE)
    source = (ROOT / MEMORY).read_text(encoding="utf-8")
    source = re.sub(r"^#include[^\n]*\n", "", source, flags=re.MULTILINE)
    model = r"""
typedef unsigned long long u64;
#define DEFINE_SPINLOCK(name) int name
static int memory_lock_depth, memory_bad_context, memory_warnings, memory_lock_entries;
static void (*before_memory_lock)(void);
static void memory_lock(int *lock) {
    (void)lock;
    if (before_memory_lock) {
        void (*work)(void) = before_memory_lock;
        before_memory_lock = NULL;
        work();
    }
    if (memory_lock_depth) memory_bad_context++;
    memory_lock_depth++; memory_lock_entries++;
}
static void memory_unlock(int *lock) {
    (void)lock;
    if (memory_lock_depth != 1) memory_bad_context++;
    memory_lock_depth--;
}
static bool memory_warn(bool value) {
    if (value) { memory_warnings++; if (memory_lock_depth) memory_bad_context++; }
    return value;
}
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; memory_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); memory_unlock(lock); } while (0)
#define WARN_ON_ONCE(value) memory_warn(!!(value))
"""
    return model + header + source


TRACKER = r"""
#define GFP_KERNEL 0
#define GFP_ATOMIC 1
#define ENOENT 2
#define EIO 5
#define ENOMEM 12
#define EFAULT 14
#define EINVAL 22
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EOVERFLOW 75
#define EOPNOTSUPP 95
#define EAGAIN 11
#define READ 0
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v, x) ((v) = (x))
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define check_mul_overflow(a, b, out) __builtin_mul_overflow((a), (b), (out))
#define ARRAY_SIZE(v) (sizeof(v) / sizeof((v)[0]))
#define __user
static unsigned long long memory_blocks[32][(1024 * 1024) / sizeof(unsigned long long)];
static size_t memory_block_sizes[32], physical_requested, physical_peak;
static int allocation_attempts, allocation_fail_at, allocation_failures, allocation_frees, allocator_bad;
static struct sumh_proc_memory_stats stats(void) {
    struct sumh_proc_memory_stats value;
    sumh_proc_memory_snapshot(&value);
    size_t kinds = 0;
    for (unsigned int i = 0; i < SUMH_PROC_MEMORY_KINDS; i++) kinds += value.by_kind[i];
    if (value.used != kinds ||
        value.used > SUMH_PROC_MEMORY_BUDGET || value.peak > SUMH_PROC_MEMORY_BUDGET)
        allocator_bad++;
    return value;
}
static void *tracked_alloc(size_t size, bool zero) {
    if (memory_lock_depth) memory_bad_context++;
    struct sumh_proc_memory_stats value = stats();
    /* Admission must cover simultaneous old + new bytes before allocation. */
    if (size > value.used || physical_requested > value.used - size) allocator_bad++;
    allocation_attempts++;
    if (allocation_attempts == allocation_fail_at) { allocation_failures++; return NULL; }
    for (size_t i = 0; i < ARRAY_SIZE(memory_blocks); i++) {
        if (!memory_block_sizes[i] && size <= sizeof(memory_blocks[i]) && size) {
            memory_block_sizes[i] = size;
            physical_requested += size;
            if (physical_peak < physical_requested) physical_peak = physical_requested;
            if (zero) memset(memory_blocks[i], 0, size);
            return memory_blocks[i];
        }
    }
    allocation_failures++;
    return NULL;
}
static void tracked_free(void *ptr) {
    if (!ptr) return;
    if (memory_lock_depth) memory_bad_context++;
    struct sumh_proc_memory_stats value = stats();
    /* Memory must still be charged while its free callback executes. */
    if (physical_requested > value.used) allocator_bad++;
    for (size_t i = 0; i < ARRAY_SIZE(memory_blocks); i++) {
        if (ptr == memory_blocks[i] && memory_block_sizes[i]) {
            physical_requested -= memory_block_sizes[i];
            memory_block_sizes[i] = 0; allocation_frees++; return;
        }
    }
    allocator_bad++;
}
static void *kvmalloc(size_t size, int flags) { (void)flags; return tracked_alloc(size, false); }
static void *kvzalloc(size_t size, int flags) { (void)flags; return tracked_alloc(size, true); }
static void *kzalloc(size_t size, int flags) { (void)flags; return tracked_alloc(size, true); }
static void *kvcalloc(size_t count, size_t size, int flags) {
    size_t bytes; (void)flags;
    if (check_mul_overflow(count, size, &bytes)) { allocator_bad++; return NULL; }
    return tracked_alloc(bytes, true);
}
static void *kvmalloc_array(size_t count, size_t size, int flags) {
    size_t bytes; (void)flags;
    if (check_mul_overflow(count, size, &bytes)) { allocator_bad++; return NULL; }
    return tracked_alloc(bytes, false);
}
static void kvfree(void *ptr) { tracked_free(ptr); }
static void kfree(void *ptr) { tracked_free(ptr); }
static bool balanced(void) {
    return !stats().used && !physical_requested && !allocator_bad &&
           !memory_bad_context && !memory_lock_depth;
}
"""


def ledger_source():
    tests = r"""
static bool competing_success;
static void competing_reserve(void) {
    competing_success = sumh_proc_memory_reserve(SUMH_PROC_MEMORY_WORKSPACE, 1);
}
static void competing_release(void) {
    sumh_proc_memory_release(SUMH_PROC_MEMORY_WORKSPACE, 1);
}
int main(void) {
    struct sumh_proc_memory_stats value;
    CHECK(balanced());
    CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_SNAPSHOT, 0));
    CHECK(!sumh_proc_memory_reserve((enum sumh_proc_memory_kind)-1, 1));
    CHECK(!sumh_proc_memory_reserve(SUMH_PROC_MEMORY_KINDS, 1));
    CHECK(!stats().used && !stats().denied);
    CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_SNAPSHOT, 65536));
    CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_STREAM, SUMH_PROC_MEMORY_BUDGET - 65537));
    CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_WORKSPACE, 1));
    CHECK(stats().used == SUMH_PROC_MEMORY_BUDGET && stats().peak == SUMH_PROC_MEMORY_BUDGET);
    CHECK(!sumh_proc_memory_reserve(SUMH_PROC_MEMORY_WORKSPACE, 1));
    CHECK(!sumh_proc_memory_reserve(SUMH_PROC_MEMORY_SNAPSHOT, (size_t)-1));
    CHECK(!sumh_proc_memory_reserve(SUMH_PROC_MEMORY_STREAM, SUMH_PROC_MEMORY_BUDGET + 1));
    CHECK(stats().used == SUMH_PROC_MEMORY_BUDGET && stats().denied == 3);
    sumh_proc_memory_release(SUMH_PROC_MEMORY_WORKSPACE, 2);
    sumh_proc_memory_release((enum sumh_proc_memory_kind)-1, 1);
    CHECK(stats().used == SUMH_PROC_MEMORY_BUDGET && memory_warnings == 2);
    sumh_proc_memory_release(SUMH_PROC_MEMORY_WORKSPACE, 1);
    /* Deterministic acquisition ordering model; this is not threaded proof. */
    before_memory_lock = competing_reserve;
    CHECK(!sumh_proc_memory_reserve(SUMH_PROC_MEMORY_SNAPSHOT, 1));
    CHECK(competing_success && stats().used == SUMH_PROC_MEMORY_BUDGET);
    before_memory_lock = competing_release;
    CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_SNAPSHOT, 1));
    CHECK(stats().used == SUMH_PROC_MEMORY_BUDGET);
    sumh_proc_memory_release(SUMH_PROC_MEMORY_SNAPSHOT, 65537);
    sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM, SUMH_PROC_MEMORY_BUDGET - 65537);
    CHECK(balanced());
    value = stats();
    sumh_proc_memory_alloc_failed(SUMH_PROC_MEMORY_STREAM);
    CHECK(stats().allocation_failed == value.allocation_failed + 1 && !stats().used);
    CHECK(stats().peak == SUMH_PROC_MEMORY_BUDGET && memory_lock_entries > 10);
    CHECK(balanced());
    return 0;
}
"""
    return COMMON + memory_source() + TRACKER + tests


IO = r"""
typedef long long loff_t;
typedef unsigned int u32;
typedef unsigned char u8;
typedef unsigned int fmode_t;
typedef int atomic_t;
typedef int refcount_t;
struct list_head { int unused; };
struct mutex { int depth; };
#define DEFINE_MUTEX(name) struct mutex name
#define refcount_set(p, value) (*(p) = (value))
#define refcount_inc(p) (++*(p))
#define refcount_dec_and_test(p) (--*(p) == 0)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
static int stream_locks, io_bad, copy_fails, native_reads, native_fallbacks, read_failure, read_no_progress, read_overflow;
struct file;
struct inode { int unused; };
struct kiocb { struct file *ki_filp; loff_t ki_pos; };
struct iov_iter { char *buffer; size_t count; };
struct kvec { void *iov_base; size_t iov_len; };
struct file_operations {
    ssize_t (*read)(struct file *, char *, size_t, loff_t *);
    ssize_t (*read_iter)(struct kiocb *, struct iov_iter *);
    loff_t (*llseek)(struct file *, loff_t, int);
    unsigned int (*poll)(struct file *, void *);
    int (*release)(struct inode *, struct file *);
};
struct file { void *private_data; loff_t f_pos; const struct file_operations *f_op; fmode_t f_mode; };
struct mnt_namespace { int unused; };
struct vfsmount { int unused; };
struct path { void *mnt, *dentry; };
struct seq_file;
typedef int (*sumh_mi_show_fn)(struct seq_file *, struct vfsmount *);
struct sumh_proc_mounts_prefix { struct mnt_namespace *ns; struct path root; sumh_mi_show_fn show; };
struct seq_file { struct sumh_proc_mounts_prefix *private; unsigned long poll_event; };
static const char *read_data;
static size_t read_size, read_chunk;
static void mutex_lock(struct mutex *lock) {
    if (lock->depth || memory_lock_depth) io_bad++;
    lock->depth++; stream_locks++;
}
static void mutex_unlock(struct mutex *lock) {
    if (lock->depth != 1) io_bad++;
    lock->depth--; stream_locks--;
}
static void iov_iter_kvec(struct iov_iter *iter, int direction, struct kvec *vec, int count, size_t size) {
    (void)direction; (void)count; iter->buffer = vec->iov_base; iter->count = size;
}
static void init_sync_kiocb(struct kiocb *iocb, struct file *file) { iocb->ki_filp = file; iocb->ki_pos = 0; }
static size_t copy_to_iter(const char *src, size_t count, struct iov_iter *iter) {
    if (copy_fails) return 0;
    count = min_t(size_t, count, iter->count);
    memcpy(iter->buffer, src, count); iter->buffer += count; iter->count -= count; return count;
}
static size_t copy_to_user(char *dst, const char *src, size_t count) {
    if (copy_fails) return count;
    memcpy(dst, src, count); return 0;
}
static size_t iov_iter_count(struct iov_iter *iter) { return iter->count; }
static ssize_t seq_read_iter(struct kiocb *iocb, struct iov_iter *iter) {
    native_reads++;
    if (read_failure) return read_failure;
    if (read_overflow) return iter->count + 1;
    if (iocb->ki_pos < 0 || (size_t)iocb->ki_pos >= read_size) return 0;
    size_t count = min_t(size_t, iter->count, read_size - (size_t)iocb->ki_pos);
    if (read_chunk && read_chunk < count) count = read_chunk;
    for (size_t i = 0; i < count; i++)
        iter->buffer[i] = read_data ? read_data[(size_t)iocb->ki_pos + i] : 'n';
    if (!read_no_progress) iocb->ki_pos += count;
    return count;
}
static loff_t native_seek(struct file *file, loff_t pos, int whence) {
    (void)whence; return file->f_pos = pos;
}
static ssize_t native_user_read(struct file *file, char *buf, size_t count, loff_t *pos) {
    (void)file; (void)buf; (void)count; (void)pos; native_fallbacks++; return -EINVAL;
}
"""


def maps_store_source():
    """Actual immutable store, sharing the same production budget/allocator model."""
    header = (ROOT / "kernel/sumh/core/sumh_maps_store.h").read_text(encoding="utf-8")
    source = (ROOT / "kernel/sumh/core/sumh_maps_store.c").read_text(encoding="utf-8")
    header = re.sub(r"^#include[^\n]*\n", "", header, flags=re.MULTILINE)
    source = re.sub(r"^#include[^\n]*\n", "", source, flags=re.MULTILINE)
    preamble = defines("uapi/sumh.h", "SUMH_MAX_LEN_PATHNAME")
    preamble += defines("uapi/sumh.h", "SUMH_MANAGED_MAPS_MAX_RULES")
    preamble += declaration("uapi/sumh.h", "struct sumh_maps_rule {")
    preamble += r"""
#define check_add_overflow(a, b, out) __builtin_add_overflow((a), (b), (out))
static void *memchr(const void *data, int needle, size_t size) {
    const unsigned char *s = data;
    for (size_t i = 0; i < size; i++) if (s[i] == (unsigned char)needle) return (void *)(s + i);
    return NULL;
}
static ssize_t strscpy(char *dst, const char *src, size_t size) {
    size_t len = strnlen(src, size);
    if (len == size) { if (size) { memcpy(dst, src, size - 1); dst[size - 1] = 0; } return -7; }
    memcpy(dst, src, len + 1); return len;
}
/* Test double for the kernel generic sort, using the production comparator. */
static void sort(void *base, size_t count, size_t width,
        int (*compare)(const void *, const void *), void (*swap)(void *, void *, int)) {
    unsigned char *bytes = base;
    (void)swap;
    for (size_t i = 1; i < count; i++) for (size_t j = i; j &&
            compare(bytes + (j - 1) * width, bytes + j * width) > 0; j--)
        for (size_t k = 0; k < width; k++) {
            unsigned char v = bytes[(j - 1) * width + k];
            bytes[(j - 1) * width + k] = bytes[j * width + k];
            bytes[j * width + k] = v;
        }
}
"""
    return preamble + header + source


def stream_common():
    structs = declaration(PROC, "enum sumh_proc_proxy_kind {")
    structs += "enum sumh_policy_scope { SUMH_POLICY_SCOPE_NONE, SUMH_POLICY_SCOPE_VIEW, SUMH_POLICY_SCOPE_SPOOF };\n"
    structs += declaration(PROC, "struct sumh_mount_file_proxy {")
    return (COMMON + memory_source() + TRACKER + IO + maps_store_source() + structs + defines(PROC, "SUMH_PROC_") +
            bodies(PROC, ["sumh_mount_proxy_stream_reserve", "sumh_mount_proxy_stream_free", "sumh_mount_proxy_stream_alloc",
                          "sumh_mount_proxy_stream_grow_raw", "sumh_mount_proxy_stream_alloc_filtered",
                          "sumh_mount_proxy_stream_grow_filtered"]))


def stream_allocation_source():
    return stream_common() + r"""
int main(void) {
    struct sumh_mount_file_proxy proxy = {0};
    CHECK(balanced());
    CHECK(!sumh_mount_proxy_stream_alloc(&proxy));
    CHECK(!sumh_mount_proxy_stream_alloc_filtered(&proxy));
    CHECK(stats().used == 2 * SUMH_PROC_STREAM_ALLOCATION);
    CHECK(stats().by_kind[SUMH_PROC_MEMORY_STREAM] == stats().used);
    int calls = allocation_attempts;
    CHECK(!sumh_mount_proxy_stream_alloc(&proxy) && allocation_attempts == calls);
    proxy.stream_raw_len = 8; memcpy(proxy.stream_raw, "retained", 8);
    char *original = proxy.stream_raw;
    size_t old = proxy.stream_raw_capacity, baseline = stats().used;
    size_t filler = SUMH_PROC_MEMORY_BUDGET - baseline - (2 * old - 1);
    CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_SNAPSHOT, filler));
    u64 failed = stats().allocation_failed, denied = stats().denied;
    CHECK(sumh_mount_proxy_stream_grow_raw(&proxy) == -ENOMEM);
    CHECK(proxy.stream_raw == original && proxy.stream_raw_capacity == old);
    CHECK(!strncmp(proxy.stream_raw, "retained", 8));
    CHECK(allocation_attempts == calls && stats().denied == denied + 1);
    CHECK(stats().allocation_failed == failed && stats().used == baseline + filler);
    sumh_proc_memory_release(SUMH_PROC_MEMORY_SNAPSHOT, filler);
    allocation_fail_at = allocation_attempts + 1;
    CHECK(sumh_mount_proxy_stream_grow_raw(&proxy) == -ENOMEM);
    CHECK(proxy.stream_raw == original && stats().used == baseline);
    CHECK(stats().allocation_failed == failed + 1);
    allocation_fail_at = 0;
    CHECK(!sumh_mount_proxy_stream_grow_raw(&proxy));
    CHECK(proxy.stream_raw != original && proxy.stream_raw_capacity == old * 2);
    CHECK(!strncmp(proxy.stream_raw, "retained", 8));
    CHECK(physical_peak == baseline + 2 * old);
    CHECK(!allocator_bad && stats().used == baseline + old);
    old = proxy.stream_filtered_capacity; original = proxy.stream_filtered;
    baseline = stats().used;
    allocation_fail_at = allocation_attempts + 1;
    CHECK(sumh_mount_proxy_stream_grow_filtered(&proxy) == -ENOMEM);
    CHECK(proxy.stream_filtered == original && stats().used == baseline);
    allocation_fail_at = 0;
    CHECK(!sumh_mount_proxy_stream_grow_filtered(&proxy));
    CHECK(proxy.stream_filtered_capacity == old * 2 && stats().used == baseline + old);
    while (proxy.stream_raw_capacity < SUMH_PROC_STREAM_MAX)
        CHECK(!sumh_mount_proxy_stream_grow_raw(&proxy));
    while (proxy.stream_filtered_capacity < SUMH_PROC_STREAM_MAX)
        CHECK(!sumh_mount_proxy_stream_grow_filtered(&proxy));
    baseline = stats().used;
    CHECK(sumh_mount_proxy_stream_grow_raw(&proxy) == -EFBIG);
    CHECK(sumh_mount_proxy_stream_grow_filtered(&proxy) == -ENOSPC);
    CHECK(stats().used == baseline);
    sumh_mount_proxy_stream_free(&proxy);
    CHECK(!proxy.stream_raw && !proxy.stream_filtered && !proxy.stream_raw_capacity && !proxy.stream_filtered_capacity);
    sumh_mount_proxy_stream_free(&proxy);
    CHECK(balanced());
    for (int filtered = 0; filtered < 2; filtered++) {
        failed = stats().allocation_failed;
        allocation_fail_at = allocation_attempts + 1;
        CHECK((filtered ? sumh_mount_proxy_stream_alloc_filtered(&proxy) :
                          sumh_mount_proxy_stream_alloc(&proxy)) == -ENOMEM);
        CHECK(stats().allocation_failed == failed + 1 && balanced());
    }
    return 0;
}
"""


def snapshot_source():
    structs = declaration("kernel/sumh/features/sumh_fake_mountinfo.h", "struct sumh_mi_snapshot {")
    support = r"""
static DEFINE_MUTEX(sumh_mi_snapshot_lock);
static int original_show(struct seq_file *seq, struct vfsmount *mnt) { (void)seq; (void)mnt; return 0; }
static int sumh_mi_show_mountinfo(struct seq_file *seq, struct vfsmount *mnt) { (void)seq; (void)mnt; return 0; }
static int sumh_mi_show_mounts(struct seq_file *seq, struct vfsmount *mnt) { (void)seq; (void)mnt; return 0; }
static int pair_failure, filter_failure, normalize_failure;
static bool sumh_mi_pair_matches(struct sumh_mi_snapshot *snapshot) { (void)snapshot; return !pair_failure; }
static int sumh_mi_filter_native(struct sumh_mi_snapshot *snapshot) { (void)snapshot; return filter_failure; }
static int sumh_mi_normalize_groups(char *data, size_t *len) { (void)data; (void)len; return normalize_failure; }
"""
    methods = bodies(MOUNT, ["sumh_mi_alloc_buffer", "sumh_mi_free_buffer", "sumh_fake_mi_put_snapshot",
                             "sumh_mi_read_snapshot", "sumh_mi_render", "sumh_fake_mi_get_snapshot"])
    tests = r"""
int main(void) {
    struct mnt_namespace ns;
    struct sumh_proc_mounts_prefix pm = {&ns, {&ns, &ns}, original_show};
    struct seq_file seq = {&pm, 0};
    struct file_operations ops = {native_user_read, seq_read_iter, native_seek, NULL};
    struct file file = {&seq, 0, &ops};
    struct sumh_mi_snapshot *first = NULL, *second = NULL;
    CHECK(balanced());
    read_size = SUMH_MI_INITIAL_SIZE + 1;
    CHECK(!sumh_fake_mi_get_snapshot(&file, &ops, &first));
    CHECK(first && first->data_capacity == 2 * SUMH_MI_INITIAL_SIZE);
    CHECK(first->mounts_capacity == 2 * SUMH_MI_INITIAL_SIZE && first->refs == 1);
    size_t baseline = sizeof(*first) + first->data_capacity + first->mounts_capacity;
    CHECK(stats().used == baseline && physical_requested == baseline);
    CHECK(stats().by_kind[SUMH_PROC_MEMORY_SNAPSHOT] == baseline);
    CHECK(pm.show == original_show && !stream_locks && !io_bad && !allocator_bad);
    CHECK(!sumh_fake_mi_get_snapshot(&file, &ops, &second));
    CHECK(first != second && first->data != second->data && stats().used == 2 * baseline);
    first->refs++;
    sumh_fake_mi_put_snapshot(first);
    CHECK(first->refs == 1 && stats().used == 2 * baseline);
    first->len = first->mounts_len = 1;
    sumh_fake_mi_put_snapshot(first);
    CHECK(stats().used == baseline && second->data[0] == 'n');
    sumh_fake_mi_put_snapshot(second);
    sumh_fake_mi_put_snapshot(NULL);
    CHECK(balanced());
    /* First and second initial buffer and each growth allocator fault. */
    for (int fault = 1; fault <= 5; fault++) {
        u64 failures = stats().allocation_failed;
        allocation_fail_at = allocation_attempts + fault;
        CHECK(sumh_fake_mi_get_snapshot(&file, &ops, &first) == -ENOMEM);
        CHECK(!first && balanced() && pm.show == original_show && !stream_locks);
        CHECK(stats().allocation_failed == failures + 1);
    }
    allocation_fail_at = 0;
    for (int fault = 0; fault < 7; fault++) {
        pair_failure = fault == 0;
        filter_failure = fault == 1 ? -EINVAL : 0;
        normalize_failure = fault == 2 ? -ENOMEM : 0;
        read_failure = fault == 3 ? -EIO : 0;
        read_no_progress = fault == 4;
        read_overflow = fault == 5;
        read_size = fault == 6 ? 0 : SUMH_MI_INITIAL_SIZE + 1;
        int expected = fault == 0 ? -EAGAIN : fault == 1 ? -EINVAL : fault == 2 ? -ENOMEM : -EIO;
        CHECK(sumh_fake_mi_get_snapshot(&file, &ops, &first) == expected);
        CHECK(!first && balanced() && pm.show == original_show && !stream_locks);
    }
    pair_failure = filter_failure = normalize_failure = read_failure = read_no_progress = read_overflow = 0;
    for (int over = 0; over < 2; over++) {
        read_size = SUMH_MI_MAX_SIZE + over;
        CHECK(sumh_fake_mi_get_snapshot(&file, &ops, &first) == (over ? -EFBIG : 0));
        if (!over) {
            CHECK(first->len == SUMH_MI_MAX_SIZE && first->data_capacity == SUMH_MI_MAX_SIZE);
            sumh_fake_mi_put_snapshot(first);
        } else CHECK(!first);
        CHECK(balanced());
    }
    /* Cross-kind competition denies before allocation, preserves other owner. */
    size_t held = SUMH_PROC_MEMORY_BUDGET - sizeof(*first) - 2 * SUMH_MI_INITIAL_SIZE - 1;
    CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_STREAM, held));
    read_size = SUMH_MI_INITIAL_SIZE + 1;
    u64 denied = stats().denied, failures = stats().allocation_failed;
    CHECK(sumh_fake_mi_get_snapshot(&file, &ops, &first) == -ENOMEM);
    CHECK(!first && stats().used == held && stats().denied == denied + 1);
    CHECK(stats().allocation_failed == failures && !physical_requested && !allocator_bad);
    sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM, held);
    CHECK(!sumh_fake_mi_get_snapshot(&file, &ops, &first));
    sumh_fake_mi_put_snapshot(first);
    CHECK(balanced() && !native_fallbacks);
    return 0;
}
"""
    return COMMON + memory_source() + TRACKER + IO + structs + defines(MOUNT, "SUMH_MI_") + support + methods + tests


FORMAT = r"""
static int scnprintf(char *out, size_t capacity, const char *format, ...) {
    __builtin_va_list ap;
    size_t written = 0;
    __builtin_va_start(ap, format);
    for (size_t i = 0; format[i]; i++) {
        if (format[i] != '%') {
            if (written + 1 < capacity) out[written++] = format[i];
            continue;
        }
        i++;
        unsigned long long value = 0;
        if (format[i] == 'z') { value = __builtin_va_arg(ap, size_t); i++; }
        else if (format[i] == 'l') {
            i++;
            if (format[i] == 'l') { value = __builtin_va_arg(ap, unsigned long long); i++; }
            else value = __builtin_va_arg(ap, unsigned long);
        } else value = __builtin_va_arg(ap, unsigned int);
        char number[32]; size_t digits = 0;
        do { number[digits++] = '0' + value % 10; value /= 10; } while (value);
        while (digits) { char c = number[--digits]; if (written + 1 < capacity) out[written++] = c; }
    }
    __builtin_va_end(ap);
    if (capacity) out[written] = 0;
    return written;
}
"""


def workspace_source():
    from test_sumh_mount_views import snapshot_source as mount_source, extract
    source = mount_source().split("int main(void) {", 1)[0]
    for name in ("scnprintf", "kvcalloc", "kvfree"):
        source = source.replace(extract("scripts/ci/test_sumh_mount_views.py", name), "")
    source = source.replace("#define SUMH_EMBEDDED", TRACKER + FORMAT + "\n#define SUMH_EMBEDDED", 1)
    source += declaration(MOUNT, "struct sumh_mi_prop_ref {")
    source += bodies(MOUNT, ["sumh_mi_collect_props", "sumh_mi_compare_ids", "sumh_mi_normalize_groups"])
    # Inject count overflow at the parser boundary only; admission/overflow and
    # unwind logic still come from the actual normalizer body.
    source = source.replace("static int sumh_mi_collect_props(", "static int real_collect_props(", 1)
    normalizer_start = source.index("int sumh_mi_normalize_groups(")
    wrapper = r"""
static int impossible_count;
static int sumh_mi_collect_props(const char *data, size_t len, struct sumh_mi_prop_ref *refs, size_t *count) {
    if (impossible_count) { *count = (size_t)-1; return 0; }
    return real_collect_props(data, len, refs, count);
}
"""
    source = source[:normalizer_start] + wrapper + source[normalizer_start:]
    return source + r"""
int main(void) {
    CHECK(balanced());
    input(ROOT_MI SYSTEM_MI, ROOT_MO SYSTEM_MO);
    CHECK(!sumh_mi_filter_native(&snapshot));
    CHECK(balanced());
    CHECK(!sumh_mi_normalize_groups(snapshot.data, &snapshot.len));
    CHECK(equal(snapshot.data, snapshot.len,
        "20 20 8:1 / / ro shared:1 - ext4 /dev/root ro\n"
        "42 20 253:0 / /system ro shared:2 - ext4 /dev/block/dm-0 ro\n"));
    CHECK(balanced() && !rcu_depth);
    for (int fault = 1; fault <= 2; fault++) {
        input(ROOT_MI SYSTEM_MI, ROOT_MO SYSTEM_MO);
        u64 failures = stats().allocation_failed;
        allocation_fail_at = allocation_attempts + fault;
        CHECK(sumh_mi_normalize_groups(snapshot.data, &snapshot.len) == -ENOMEM);
        CHECK(stats().allocation_failed == failures + 1 && balanced());
        CHECK(equal(snapshot.data, snapshot.len, ROOT_MI SYSTEM_MI));
    }
    allocation_fail_at = 0;
    for (int space = 0; space < 2; space++) {
        input(ROOT_MI SYSTEM_MI, ROOT_MO SYSTEM_MO);
        size_t held = SUMH_PROC_MEMORY_BUDGET - (space ? 2 * sizeof(struct sumh_mi_prop_ref) : 0);
        CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_STREAM, held));
        u64 failures = stats().allocation_failed, denied = stats().denied;
        CHECK(sumh_mi_normalize_groups(snapshot.data, &snapshot.len) == -ENOMEM);
        CHECK(stats().used == held && stats().denied == denied + 1 && stats().allocation_failed == failures);
        CHECK(!physical_requested && equal(snapshot.data, snapshot.len, ROOT_MI SYSTEM_MI));
        sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM, held);
        CHECK(balanced());
    }
    input(ROOT_MI, ROOT_MO);
    allocation_fail_at = allocation_attempts + 1;
    CHECK(sumh_mi_filter_native(&snapshot) == -ENOMEM);
    CHECK(balanced()); allocation_fail_at = 0;
    CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_SNAPSHOT, SUMH_PROC_MEMORY_BUDGET));
    int attempts = allocation_attempts;
    CHECK(sumh_mi_filter_native(&snapshot) == -ENOMEM);
    CHECK(allocation_attempts == attempts && !physical_requested);
    sumh_proc_memory_release(SUMH_PROC_MEMORY_SNAPSHOT, SUMH_PROC_MEMORY_BUDGET);
    CHECK(balanced());
    input(ROOT_MI, ROOT_MO);
    impossible_count = 1;
    CHECK(sumh_mi_normalize_groups(snapshot.data, &snapshot.len) == -EOVERFLOW);
    CHECK(allocation_attempts == attempts && balanced()); impossible_count = 0;
    input("bad\n", ROOT_MO);
    CHECK(sumh_mi_normalize_groups(snapshot.data, &snapshot.len) == -EINVAL && balanced());
    CHECK(!allocator_bad && !memory_bad_context && !rcu_depth);
    return 0;
}
"""


def diagnostic_source():
    return COMMON + memory_source() + TRACKER + FORMAT + bodies(
        "kernel/sumh/control/sumh_ioctl.c", ["sumh_proc_memory_report"]) + r"""
static bool has_text(const char *buffer, const char *text) {
    size_t n = strlen(text);
    for (; *buffer; buffer++) if (!strncmp(buffer, text, n)) return true;
    return false;
}
int main(void) {
    char output[514];
    CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_SNAPSHOT, 100));
    CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_STREAM, 200));
    CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_WORKSPACE, 300));
    CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_MAPS, 400));
    CHECK(!sumh_proc_memory_reserve(SUMH_PROC_MEMORY_WORKSPACE, SUMH_PROC_MEMORY_BUDGET));
    sumh_proc_memory_alloc_failed(SUMH_PROC_MEMORY_STREAM);
    int n = sumh_proc_memory_report(output, sizeof(output));
    CHECK(n > 0 && n < 400 && output[n - 1] == '\n');
    CHECK(has_text(output, "scope=snapshot+stream+workspace+maps proxy=excluded"));
    CHECK(has_text(output, "used=1000 limit=16777216 peak=1000 snapshot=100 stream=200 workspace=300 maps=400 denied=1 allocation_failed=1"));
    for (size_t capacity = 0; capacity <= 514; capacity++) {
        char guard[516]; memset(guard, '!', sizeof(guard));
        int result = sumh_proc_memory_report(guard + 1, capacity);
        CHECK(guard[0] == '!' && guard[capacity + 1] == '!');
        CHECK(result == (capacity ? min_t(size_t, n, capacity - 1) : 0));
        if (capacity) CHECK(!guard[result + 1]);
    }
    sumh_proc_memory_state.denied = (u64)-1;
    sumh_proc_memory_state.allocation_failed = (u64)-1;
    CHECK(sumh_proc_memory_report(output, sizeof(output)) < 400);
    CHECK(has_text(output, "denied=18446744073709551615 allocation_failed=18446744073709551615"));
    sumh_proc_memory_release(SUMH_PROC_MEMORY_SNAPSHOT, 100);
    sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM, 200);
    sumh_proc_memory_release(SUMH_PROC_MEMORY_WORKSPACE, 300);
    sumh_proc_memory_release(SUMH_PROC_MEMORY_MAPS, 400);
    CHECK(balanced());
    return 0;
}
"""


def release_source():
    source = stream_common().replace("static void kfree(void *ptr) { tracked_free(ptr); }", r"""
static void *excluded_proxy;
static int excluded_proxy_frees;
static void kfree(void *ptr) {
    if (ptr == excluded_proxy) excluded_proxy_frees++;
    else tracked_free(ptr);
}
""")
    source += declaration("kernel/sumh/features/sumh_fake_mountinfo.h", "struct sumh_mi_snapshot {")
    source += bodies(MOUNT, ["sumh_mi_alloc_buffer", "sumh_mi_free_buffer", "sumh_fake_mi_put_snapshot"])
    source += r"""
#define SUMH_PROXY_STATE_RELEASED 1
#define atomic_set(p, value) (*(p) = (value))
#define atomic_dec(p) (--*(p))
#define container_of(p, t, m) ((t *)((char *)(p) - __builtin_offsetof(t, m)))
static int sumh_proxy_srcu, sumh_proxy_live, sumh_proxy_list_lock;
static int srcu_depth, list_lock_depth, listed, orig_releases, orig_puts, closed_puts, orig_error;
static const struct file_operations sumh_closed_proxy_fops = {0};
static struct file *closing_file;
static const struct file_operations *original_ops;
static int srcu_read_lock(int *lock) { (void)lock; srcu_depth++; return 0; }
static void srcu_read_unlock(int *lock, int value) { (void)lock; (void)value; srcu_depth--; }
static void spin_lock(int *lock) { (void)lock; list_lock_depth++; }
static void spin_unlock(int *lock) { (void)lock; list_lock_depth--; }
static void list_del_init(struct list_head *node) {
    (void)node; if (list_lock_depth != 1 || !listed) io_bad++; listed--;
}
static int original_release(struct inode *inode, struct file *file) {
    (void)inode; orig_releases++;
    if (file->f_mode != 42 || excluded_proxy_frees || !srcu_depth || !stats().used) io_bad++;
    return orig_error;
}
static void fops_put(const struct file_operations *ops) {
    if (ops == original_ops) {
        orig_puts++;
        if (closing_file->f_op != &sumh_closed_proxy_fops || excluded_proxy_frees) io_bad++;
    } else if (ops == &sumh_closed_proxy_fops) {
        closed_puts++;
        if (!excluded_proxy_frees) io_bad++;
    } else io_bad++;
}
"""
    source += bodies(PROC, ["sumh_mount_proxy_release"])
    return source + r"""
int main(void) {
    struct file_operations ops = {0}; ops.release = original_release; original_ops = &ops;
    for (int round = 0; round < 20; round++) {
        struct sumh_mount_file_proxy proxy = {0};
        struct file file = {NULL, 0, &proxy.proxy_fops, 0};
        proxy.orig_fops = &ops; proxy.orig_f_mode = 42;
        excluded_proxy = &proxy; closing_file = &file;
        excluded_proxy_frees = orig_releases = orig_puts = closed_puts = 0;
        sumh_proxy_live = listed = 1; orig_error = round & 1 ? -EIO : 0;
        CHECK(!sumh_mount_proxy_stream_alloc(&proxy));
        CHECK(!sumh_mount_proxy_stream_alloc_filtered(&proxy));
        CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_SNAPSHOT, sizeof(struct sumh_mi_snapshot)));
        struct sumh_mi_snapshot *snapshot = kzalloc(sizeof(*snapshot), GFP_KERNEL);
        CHECK(snapshot); snapshot->refs = 1;
        snapshot->data = sumh_mi_alloc_buffer(64); snapshot->data_capacity = 64;
        snapshot->mounts = sumh_mi_alloc_buffer(64); snapshot->mounts_capacity = 64;
        CHECK(snapshot->data && snapshot->mounts); proxy.mountinfo_snapshot = snapshot;
        /* Closing a proxy also drops the final reference to a retired Maps set. */
        struct sumh_maps_rule rule = {0}; rule.target_ino = 7;
        struct sumh_maps_snapshot *maps = NULL;
        CHECK(!sumh_maps_prepare_add(&rule, &maps)); sumh_maps_publish(maps);
        proxy.maps_snapshot = sumh_maps_snapshot_get(); proxy.maps_snapshot_acquired = true;
        sumh_maps_store_clear();
        CHECK(proxy.maps_snapshot && stats().by_kind[SUMH_PROC_MEMORY_MAPS]);
        size_t owned = stats().used;
        /* Existing FD retains bytes after invalidation until final release. */
        proxy.filter_invalidated = 1;
        CHECK(stats().used == owned && physical_requested == owned);
        CHECK(sumh_mount_proxy_release(NULL, &file) == orig_error);
        CHECK(orig_releases == 1 && orig_puts == 1 && excluded_proxy_frees == 1);
        CHECK(file.f_op == &sumh_closed_proxy_fops && file.f_mode == 42);
        CHECK(!sumh_proxy_live && !listed && !srcu_depth && !list_lock_depth);
        CHECK(balanced() && !io_bad);
        /* VFS consumes the retained owner on static fops after release returns. */
        fops_put(file.f_op); CHECK(closed_puts == 1 && !io_bad);
    }
    return 0;
}
"""


def stream_read_source():
    support = r"""
static int filter_error, invalid_maps, normalizer_error, filter_calls, native_calls;
static size_t filter_required;
static bool filter_uses_snapshot;
static struct sumh_maps_snapshot *filter_publish;
static int filter_lookup_bad;
static int sumh_filter_maps_lines(const char *src, size_t len, char *dst, size_t size,
        size_t *written, bool *changed, bool *valid, enum sumh_policy_scope scope,
        const struct sumh_maps_snapshot *snapshot) {
    (void)src; (void)len; (void)scope; filter_calls++;
    if (filter_publish) {
        struct sumh_maps_snapshot *prepared = filter_publish;
        filter_publish = NULL; sumh_maps_publish(prepared);
    }
    if (filter_error) return filter_error;
    if (filter_required > size) return -ENOSPC;
    *changed = true; *valid = !invalid_maps;
    if (filter_uses_snapshot) {
        const struct sumh_maps_rule_entry *rule = sumh_maps_snapshot_lookup(snapshot, 7, 9);
        const char *text = rule ? rule->spoofed_pathname : "empty\n";
        size_t n = strlen(text);
        if (n > size) return -ENOSPC;
        if (memory_lock_depth || !stream_locks) filter_lookup_bad++;
        memcpy(dst, text, n); *written = n; return 0;
    }
    memcpy(dst, "clean\n", 6); *written = 6; return 0;
}
static int sumh_mi_normalize_groups(char *data, size_t *size) {
    (void)data; (void)size; return normalizer_error;
}
#define container_of(p, t, m) ((t *)((char *)(p) - __builtin_offsetof(t, m)))
static int sumh_proxy_srcu, srcu_depth;
static int srcu_read_lock(int *lock) { (void)lock; srcu_depth++; return 0; }
static void srcu_read_unlock(int *lock, int index) { (void)lock; (void)index; srcu_depth--; }
static bool sumh_mount_proxy_filter_active(struct sumh_mount_file_proxy *proxy) { (void)proxy; return true; }
static int sumh_mount_proxy_prepare_mountinfo(struct sumh_mount_file_proxy *proxy, struct file *file) {
    (void)proxy; (void)file; return -EINVAL;
}
static ssize_t sumh_mount_proxy_read_snapshot(struct sumh_mi_snapshot *snapshot, bool mounts,
        char *buf, struct iov_iter *to, size_t count, loff_t *pos) {
    (void)snapshot; (void)mounts; (void)buf; (void)to; (void)count; (void)pos; return -EINVAL;
}
"""
    methods = bodies(PROC, ["sumh_mount_proxy_orig_read_iter", "sumh_mount_proxy_stream_compact",
                            "sumh_mount_proxy_stream_fill", "sumh_mount_proxy_prepare_groups",
                            "sumh_mount_proxy_read_buffer", "sumh_mount_proxy_read_groups",
                            "sumh_mount_proxy_filtered_read", "sumh_mount_proxy_read", "sumh_mount_proxy_read_iter"])
    tests = r"""
static int prepare_maps(const char *text, struct sumh_maps_snapshot **prepared) {
    struct sumh_maps_rule rule = {0};
    rule.target_ino=7; rule.target_dev=9;
    strscpy(rule.spoofed_pathname,text,sizeof(rule.spoofed_pathname));
    return sumh_maps_prepare_add(&rule, prepared);
}
static int publish_maps(const char *text) {
    struct sumh_maps_snapshot *prepared = NULL;
    int ret=prepare_maps(text,&prepared);
    if(!ret) sumh_maps_publish(prepared);
    return ret;
}
static int test_maps_stream_pins(void) {
    struct file_operations ops = {native_user_read, seq_read_iter, native_seek, NULL};
    char output[64];
    filter_uses_snapshot=true;
    read_failure=read_overflow=read_no_progress=allocation_fail_at=filter_error=normalizer_error=0;
    read_data="source\nsource\nsource\n"; read_size=strlen(read_data); read_chunk=7;
    for(int empty=0;empty<2;empty++) for(int iter=0;iter<2;iter++) {
        struct sumh_mount_file_proxy proxy={0};
        struct file file={NULL,0,&proxy.proxy_fops};
        struct kiocb iocb={&file,0}; loff_t pos=0;
        struct iov_iter to={output,0};
        proxy.orig_fops=&ops; proxy.kind=SUMH_PROC_PROXY_MAPS; proxy.scope=SUMH_POLICY_SCOPE_SPOOF;
        sumh_maps_store_clear(); CHECK(balanced());
        if(!empty) CHECK(!publish_maps("old"));
        size_t baseline=stats().used;
        /* A zero read does not pin even a currently nonempty collection. */
        CHECK((iter ? sumh_mount_proxy_read_iter(&iocb,&to) : sumh_mount_proxy_read(&file,output,0,&pos))==0);
        CHECK(!proxy.maps_snapshot_acquired&&!proxy.maps_snapshot&&stats().used==baseline);
        /* ENOSPC retries after intervening publication must retain the initial pin. */
        CHECK(!prepare_maps("new",&filter_publish));
        filter_required=SUMH_PROC_STREAM_ALLOCATION+1;
        int before_filter=filter_calls;
        to.count=1; copy_fails=1;
        CHECK((iter ? sumh_mount_proxy_read_iter(&iocb,&to) : sumh_mount_proxy_read(&file,output,1,&pos))==-EFAULT);
        CHECK(proxy.maps_snapshot_acquired&&(!!proxy.maps_snapshot==!empty));
        CHECK(!proxy.stream_failed&&!filter_publish&&filter_calls==before_filter+2);
        CHECK(!pos&&!iocb.ki_pos&&proxy.stream_filtered_capacity>SUMH_PROC_STREAM_ALLOCATION);
        copy_fails=0; filter_required=0;
        size_t received=0;
        for(;;) {
            to.buffer=output+received; to.count=1;
            ssize_t n=iter ? sumh_mount_proxy_read_iter(&iocb,&to) : sumh_mount_proxy_read(&file,output+received,1,&pos);
            CHECK(n>=0&&n<=1);
            if(!n) break;
            received+=n; CHECK(received<sizeof(output));
            /* Keep the new current generation alive across a second native chunk,
             * then clear before the third. Neither may enter this stream. */
            if(received==(size_t)(empty ? 12 : 6)) sumh_maps_store_clear();
        }
        const char *expected=empty ? "empty\nempty\nempty\n" : "oldoldold";
        CHECK(received==strlen(expected)&&!strncmp(output,expected,received));
        CHECK(proxy.maps_snapshot_acquired&&!proxy.stream_failed);
        CHECK(stats().by_kind[SUMH_PROC_MEMORY_MAPS]==baseline);
        sumh_mount_proxy_stream_free(&proxy);
        CHECK(!proxy.maps_snapshot&&balanced());
        /* New stream samples the newly published collection. */
        CHECK(!publish_maps("new"));
        memset(&proxy,0,sizeof(proxy)); pos=0; iocb.ki_pos=0;
        proxy.orig_fops=&ops; proxy.kind=SUMH_PROC_PROXY_MAPS; proxy.scope=SUMH_POLICY_SCOPE_SPOOF;
        to.buffer=output; to.count=3;
        CHECK((iter ? sumh_mount_proxy_read_iter(&iocb,&to) : sumh_mount_proxy_read(&file,output,3,&pos))==3);
        CHECK(!strncmp(output,"new",3)&&proxy.maps_snapshot_acquired);
        sumh_maps_store_clear();
        CHECK(stats().by_kind[SUMH_PROC_MEMORY_MAPS]>0);
        read_failure=-EIO; to.buffer=output; to.count=1;
        CHECK((iter ? sumh_mount_proxy_read_iter(&iocb,&to) : sumh_mount_proxy_read(&file,output,1,&pos))==-EIO);
        CHECK(proxy.stream_failed&&proxy.maps_snapshot_acquired&&!proxy.maps_snapshot&&balanced());
        int reads=native_reads;
        CHECK(sumh_mount_proxy_read(&file,output,1,&pos)==-EIO&&native_reads==reads);
        sumh_mount_proxy_stream_free(&proxy); CHECK(balanced());
        read_failure=0;
    }
    filter_uses_snapshot=false; read_chunk=0;
    CHECK(!filter_lookup_bad&&!memory_bad_context&&!io_bad);
    return 0;
}
int main(void) {
    struct file_operations ops = {native_user_read, seq_read_iter, native_seek, NULL};
    char output[32];
    for (int iter = 0; iter < 2; iter++) for (int fault = 0; fault < 9; fault++) {
        struct sumh_mount_file_proxy proxy = {0};
        struct file file = {NULL, 0, &proxy.proxy_fops};
        struct kiocb iocb = {&file, 0};
        struct iov_iter to = {output, 16};
        loff_t pos = 0;
        proxy.orig_fops = &ops; proxy.kind = SUMH_PROC_PROXY_MAPS; proxy.scope = SUMH_POLICY_SCOPE_SPOOF;
        read_data = "native secret\n"; read_size = strlen(read_data); read_chunk = 0;
        read_failure = fault == 2 ? -EIO : 0; read_overflow = fault == 8;
        filter_error = fault == 3 ? -EINVAL : 0; invalid_maps = fault == 4;
        filter_required = fault == 5 ? SUMH_PROC_STREAM_ALLOCATION + 1 : 0;
        allocation_fail_at = fault < 2 ? allocation_attempts + fault + 1 :
            fault == 5 ? allocation_attempts + 3 : 0;
        size_t held = 0;
        if (fault == 6) {
            read_data = NULL; read_size = SUMH_PROC_STREAM_ALLOCATION;
            allocation_fail_at = allocation_attempts + 2;
        }
        if (fault == 7) {
            held = SUMH_PROC_MEMORY_BUDGET;
            CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_SNAPSHOT, held));
        }
        memset(output, '!', sizeof(output));
        int expected = fault == 2 || fault == 4 || fault == 8 ? -EIO : fault == 3 ? -EINVAL : -ENOMEM;
        CHECK((iter ? sumh_mount_proxy_read_iter(&iocb, &to) :
                      sumh_mount_proxy_read(&file, output, 16, &pos)) == expected);
        CHECK(proxy.stream_failed && !proxy.stream_raw && !proxy.stream_filtered);
        CHECK(!proxy.stream_raw_capacity && !proxy.stream_filtered_capacity);
        CHECK(!proxy.stream_raw_len && !proxy.stream_out_len && !proxy.stream_tail_off);
        CHECK(stats().used == held && !physical_requested && !pos && !iocb.ki_pos);
        CHECK(output[0] == '!' && !native_fallbacks && !stream_locks && !srcu_depth && !io_bad);
        int prior_reads = native_reads, prior_allocations = allocation_attempts;
        CHECK(sumh_mount_proxy_read(&file, output, 1, &pos) == -EIO);
        CHECK(native_reads == prior_reads && allocation_attempts == prior_allocations);
        if (held) sumh_proc_memory_release(SUMH_PROC_MEMORY_SNAPSHOT, held);
        CHECK(balanced());
    }
    read_failure = read_overflow = filter_error = invalid_maps = allocation_fail_at = 0;
    filter_required = 0;
    for (int iter = 0; iter < 2; iter++) {
        struct sumh_mount_file_proxy proxy = {0};
        struct file file = {NULL, 0, &proxy.proxy_fops};
        struct kiocb iocb = {&file, 0};
        struct iov_iter to = {output, 3}; loff_t pos = 0;
        proxy.orig_fops = &ops; proxy.kind = SUMH_PROC_PROXY_MAPS;
        read_data = "native secret\n"; read_size = strlen(read_data); copy_fails = 1;
        CHECK((iter ? sumh_mount_proxy_read_iter(&iocb, &to) :
                      sumh_mount_proxy_read(&file, output, 3, &pos)) == -EFAULT);
        CHECK(!proxy.stream_failed && proxy.stream_raw && proxy.stream_filtered && !pos && !iocb.ki_pos);
        size_t retained = stats().used;
        copy_fails = 0;
        CHECK((iter ? sumh_mount_proxy_read_iter(&iocb, &to) :
                      sumh_mount_proxy_read(&file, output, 3, &pos)) == 3);
        CHECK(!strncmp(output, "cle", 3) && stats().used == retained);
        to.buffer = output; to.count = 3;
        CHECK((iter ? sumh_mount_proxy_read_iter(&iocb, &to) :
                      sumh_mount_proxy_read(&file, output, 3, &pos)) == 3);
        CHECK(!strncmp(output, "an\n", 3));
        /* After clean partial output, next native read fails: no stale fallback. */
        read_failure = -EIO; to.buffer = output; to.count = 3;
        CHECK((iter ? sumh_mount_proxy_read_iter(&iocb, &to) :
                      sumh_mount_proxy_read(&file, output, 3, &pos)) == -EIO);
        CHECK((iter ? iocb.ki_pos : pos) == 6 && proxy.stream_failed && balanced());
        CHECK(!native_fallbacks && !stream_locks && !srcu_depth);
        read_failure = 0;
    }
    for (int fault = 0; fault < 5; fault++) {
        struct sumh_mount_file_proxy proxy = {0};
        struct file file = {NULL, 17, &proxy.proxy_fops};
        proxy.orig_fops = &ops; proxy.kind = SUMH_PROC_PROXY_MOUNTINFO_GROUPS;
        read_data = "20 20 8:1 / / ro - ext4 /dev/root ro\n"; read_size = strlen(read_data);
        allocation_fail_at = fault == 0 ? allocation_attempts + 1 : 0;
        read_failure = fault == 1 ? -EIO : 0;
        normalizer_error = fault == 2 ? -ENOMEM : 0;
        if (fault == 3) { read_data = NULL; read_size = SUMH_PROC_STREAM_ALLOCATION; allocation_fail_at = allocation_attempts + 2; }
        read_no_progress = fault == 4;
        int expected = fault == 1 || fault == 4 ? -EIO : -ENOMEM;
        CHECK(sumh_mount_proxy_prepare_groups(&proxy, &file) == expected);
        CHECK(proxy.mountinfo_checked && proxy.mountinfo_error == expected && file.f_pos == 17);
        CHECK(!proxy.stream_raw && balanced() && !stream_locks);
        int reads = native_reads, calls = allocation_attempts;
        CHECK(sumh_mount_proxy_prepare_groups(&proxy, &file) == expected);
        CHECK(native_reads == reads && allocation_attempts == calls);
    }
    CHECK(!io_bad && !memory_bad_context && !native_fallbacks);
    return test_maps_stream_pins();
}
"""
    return stream_common() + support + methods + tests


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler, "proc-memory-ledger-and-ordering-model", ledger_source())
    run_test(compiler, "proc-stream-allocation-and-transient-growth", stream_allocation_source())
    run_test(compiler, "proc-snapshot-growth-refcounts-and-unwind", snapshot_source())
    run_test(compiler, "proc-stream-public-errors-cleanup-and-copy-retry", stream_read_source())
    run_test(compiler, "proc-filter-normalizer-workspace-budget-and-unwind", workspace_source())
    run_test(compiler, "proc-memory-bounded-diagnostic", diagnostic_source())
    run_test(compiler, "proc-final-close-buffers-snapshot-and-fops-handoff", release_source())
