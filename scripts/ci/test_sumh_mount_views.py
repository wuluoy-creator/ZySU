#!/usr/bin/env python3
"""Exercise retained-module proc views using production policy/filter bodies.

Kernel allocation, task credentials and file-operation registration are doubled.
No mount/unmount operation is performed. These checks do not replace a kernel
build or device validation of retained module files and procfs observations.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, run_test
from test_sumh_proc_memory import memory_source


def extract(path, name):
    source = (ROOT / path).read_text(encoding="utf-8")
    match = re.search(r"^[\w\s*]+\b" + re.escape(name) +
                      r"\s*\([^;{}]*\)\s*\{", source, re.MULTILINE)
    if not match:
        raise ValueError(f"Definition of {name} missing from {path}")
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


PRIMITIVES = r"""
typedef unsigned int u32;
typedef unsigned char u8;
typedef unsigned int uid_t;
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v, x) ((v) = (x))
#define ARRAY_SIZE(v) (sizeof(v) / sizeof((v)[0]))
#define EINVAL 22
#define ENOMEM 12
#define EOVERFLOW 75
#define check_mul_overflow(a, b, out) __builtin_mul_overflow((a), (b), (out))
#define GFP_KERNEL 0
static int memcmp(const void *left, const void *right, size_t size) {
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < size; i++) if (a[i] != b[i]) return a[i] - b[i];
    return 0;
}
static void *memchr(const void *input, int value, size_t size) {
    const unsigned char *p = input;
    for (size_t i = 0; i < size; i++) if (p[i] == (unsigned char)value) return (void *)(p + i);
    return NULL;
}
static char *strnstr(const char *input, const char *text, size_t size) {
    size_t length = strlen(text);
    if (length > size) return NULL;
    for (size_t i = 0; i <= size - length; i++)
        if (!memcmp(input + i, text, length)) return (char *)(input + i);
    return NULL;
}
"""


def policy_source():
    preamble = r"""
#define PER_USER_RANGE 100000
#define FIRST_APPLICATION_UID 10000
#define LAST_APPLICATION_UID 19999
#define FIRST_ISOLATED_UID 99000
#define LAST_ISOLATED_UID 99999
#define PROC_SUPER_MAGIC 0x9fa0
#define SUMH_FEATURE_MOUNT_HIDE 1
#define SUMH_FEATURE_MAPS_SPOOF 2
#define ESHUTDOWN 108
#define EALREADY 114
#define EAGAIN 11
#define ENOENT 2
#define GFP_ATOMIC 0
#define FMODE_LSEEK 4
#define FMODE_PREAD 8
#define THIS_MODULE ((void *)1)
#define SUMH_PROXY_STATE_OPEN 0
#define smp_load_acquire(p) (*(p))
#define __kuid_val(v) (v)
#define task_uid(t) ((t)->uid)
#define current_uid() (current->uid)
#define atomic_read(p) (*(p))
#define atomic_set(p, v) (*(p) = (v))
#define atomic_inc(p) (++*(p))
#define sumh_log(...) ((void)0)
enum sumh_policy_scope { SUMH_POLICY_SCOPE_NONE, SUMH_POLICY_SCOPE_VIEW, SUMH_POLICY_SCOPE_SPOOF };
enum sumh_proc_proxy_kind { SUMH_PROC_PROXY_NONE, SUMH_PROC_PROXY_MOUNTINFO, SUMH_PROC_PROXY_MOUNTS,
                           SUMH_PROC_PROXY_MAPS, SUMH_PROC_PROXY_MOUNTINFO_GROUPS };
struct task_struct { uid_t uid; } task;
static struct task_struct *current = &task;
struct super_block { unsigned long s_magic; };
struct inode { struct super_block *i_sb; };
struct qstr { const char *name; unsigned int len; };
struct dentry { struct qstr d_name; };
struct path { struct dentry *dentry; };
struct file_operations {
    void *owner;
    void (*read)(void), (*read_iter)(void), (*splice_read)(void), (*llseek)(void);
    void (*poll)(void), (*unlocked_ioctl)(void), (*release)(void);
};
struct file { struct path f_path; struct inode *f_inode; const struct file_operations *f_op; int f_mode; };
struct sumh_mount_file_proxy {
    const struct file_operations *orig_fops;
    struct file_operations proxy_fops;
    enum sumh_proc_proxy_kind kind;
    enum sumh_policy_scope scope;
    bool stream_failed;
    struct sumh_maps_snapshot *maps_snapshot;
    bool maps_snapshot_acquired;
    int orig_f_mode, node, state, filter_invalidated, stream_lock;
    long stream_orig_pos, stream_user_pos, stream_generation;
};
static int sumh_enabled = 1, sumh_feature_enabled_mask, resolving, allowed, umount_app;
static int unshare_mnt;
static int fake_mi_active = 1;
static int sumh_proxy_shutdown, sumh_proxy_live, sumh_proxy_list_lock, sumh_proxy_list;
static int allocation_fails, allocations, frees, fops_get_fails, lock_depth;
static struct sumh_mount_file_proxy allocated;
static bool sumh_hide_rules_resolving(void) { return resolving; }
static bool ksu_uid_should_umount(uid_t uid) { (void)uid; return umount_app; }
static bool ksu_is_allow_uid(uid_t uid) { (void)uid; return allowed; }
static bool ksu_is_unshare_mnt_enabled(void) { return unshare_mnt; }
static bool sumh_fake_mi_active(void) { return fake_mi_active; }
static int sumh_fake_mi_generation(void) { return 7; }
static void sumh_mount_proxy_read(void) {}
static void sumh_mount_proxy_read_iter(void) {}
static void sumh_mount_proxy_splice_read(void) {}
static void sumh_mount_proxy_llseek(void) {}
static void sumh_mount_proxy_poll(void) {}
static void sumh_mount_proxy_ioctl(void) {}
static void sumh_mount_proxy_release(void) {}
static void original_callback(void) {}
static void *kzalloc(size_t size, int flags) {
    (void)size; (void)flags; allocations++;
    memset(&allocated, 0, sizeof(allocated));
    return allocation_fails ? NULL : &allocated;
}
static void kfree(void *ptr) { if (ptr) frees++; }
static void INIT_LIST_HEAD(int *node) { *node = 0; }
static void mutex_init(int *lock) { *lock = 0; }
static void list_add(int *node, int *list) { (void)node; (*list)++; }
static void spin_lock(int *lock) { (void)lock; lock_depth++; }
static void spin_unlock(int *lock) { (void)lock; lock_depth--; }
static const struct file_operations *fops_get(const struct file_operations *ops) {
    return fops_get_fails ? NULL : ops;
}
static void select_name(struct file *file, const char *name) {
    file->f_path.dentry->d_name = (struct qstr){name, strlen(name)};
}
"""
    bodies = "".join(extract("kernel/policy/allowlist.h", name)
                     for name in ("is_appuid", "is_isolated_process"))
    bodies += "".join(extract("kernel/sumh/policy/sumh_path_policy.c", name)
                      for name in ("sumh_policy_scope_for_uid", "sumh_policy_current_scope",
                                   "sumh_policy_current_is_isolated",
                                   "sumh_policy_current_is_mount_view_target"))
    bodies += "".join(extract("kernel/sumh/hooks/sumh_proc_read_hooks.c", name)
                      for name in ("sumh_proc_proxy_kind_for_file",
                                   "sumh_mount_proxy_install_file"))
    body = r"""
int main(void) {
    struct super_block sb = { PROC_SUPER_MAGIC };
    struct inode inode = { &sb };
    struct dentry dentry;
    struct file_operations original = {
        .read = original_callback, .read_iter = original_callback,
        .splice_read = original_callback, .llseek = original_callback,
        .poll = original_callback, .release = original_callback,
    };
    struct file file = { { &dentry }, &inode, &original, FMODE_LSEEK | FMODE_PREAD };
    enum sumh_policy_scope scope = SUMH_POLICY_SCOPE_NONE;
    enum sumh_proc_proxy_kind kind;
    const uid_t excluded[] = { 0, 1000, 2000, 9999, 20000, 89999, 100000, 102000 };
    const uid_t retained[] = { 10000, 12345, 19999, 110000, 212345 };
    const uid_t isolated[] = { 90000, 98999, 99000, 99999, 190000, 299999 };

    sumh_feature_enabled_mask = SUMH_FEATURE_MOUNT_HIDE | SUMH_FEATURE_MAPS_SPOOF;
    select_name(&file, "mountinfo");
    for (size_t i = 0; i < ARRAY_SIZE(retained); i++) {
        task.uid = retained[i];
        umount_app = 0;
        CHECK(sumh_policy_current_scope() == SUMH_POLICY_SCOPE_VIEW);
        CHECK(sumh_policy_current_is_mount_view_target());
        CHECK(sumh_proc_proxy_kind_for_file(&file, &scope) == SUMH_PROC_PROXY_MOUNTINFO);
        CHECK(scope == SUMH_POLICY_SCOPE_VIEW);
        umount_app = 1;
        CHECK(sumh_policy_current_scope() == SUMH_POLICY_SCOPE_SPOOF);
        CHECK(sumh_policy_current_is_mount_view_target());
    }
    for (size_t i = 0; i < ARRAY_SIZE(excluded); i++) {
        task.uid = excluded[i];
        CHECK(!sumh_policy_current_is_mount_view_target());
        CHECK(sumh_proc_proxy_kind_for_file(&file, &scope) == SUMH_PROC_PROXY_NONE);
        CHECK(sumh_mount_proxy_install_file(&file, SUMH_PROC_PROXY_MOUNTINFO, scope) == -EINVAL);
    }
    CHECK(!allocations);
    for (size_t i = 0; i < ARRAY_SIZE(isolated); i++) {
        task.uid = isolated[i]; umount_app = 0;
        CHECK(sumh_policy_current_scope() == SUMH_POLICY_SCOPE_SPOOF);
        CHECK(sumh_policy_current_is_mount_view_target());
        CHECK(sumh_proc_proxy_kind_for_file(&file, &scope) == SUMH_PROC_PROXY_MOUNTINFO);
    }
    task.uid = 12345;
    allowed = 1;
    CHECK(!sumh_policy_current_is_mount_view_target());
    CHECK(sumh_proc_proxy_kind_for_file(&file, &scope) == SUMH_PROC_PROXY_NONE);
    allowed = 0;
    for (int state = 0; state < 2; state++) {
        sumh_enabled = state; resolving = state;
        CHECK(sumh_policy_current_scope() == SUMH_POLICY_SCOPE_NONE);
        CHECK(!sumh_policy_current_is_mount_view_target());
        CHECK(sumh_proc_proxy_kind_for_file(&file, &scope) == SUMH_PROC_PROXY_NONE);
    }
    resolving = 0;
    fake_mi_active = 0;
    CHECK(sumh_proc_proxy_kind_for_file(&file, &scope) == SUMH_PROC_PROXY_NONE);
    fake_mi_active = 1;
    sumh_feature_enabled_mask = SUMH_FEATURE_MAPS_SPOOF;
    CHECK(sumh_proc_proxy_kind_for_file(&file, &scope) == SUMH_PROC_PROXY_NONE);
    sumh_feature_enabled_mask |= SUMH_FEATURE_MOUNT_HIDE;
    select_name(&file, "mounts");
    CHECK(sumh_proc_proxy_kind_for_file(&file, &scope) == SUMH_PROC_PROXY_MOUNTS);
    sb.s_magic = 0;
    CHECK(sumh_proc_proxy_kind_for_file(&file, &scope) == SUMH_PROC_PROXY_NONE);
    sb.s_magic = PROC_SUPER_MAGIC;
    CHECK(sumh_proc_proxy_kind_for_file(NULL, &scope) == SUMH_PROC_PROXY_NONE);
    const char *other[] = {"mountstats", "mountinfo.bak", "maps_backup", "status", "mount"};
    for (size_t i = 0; i < ARRAY_SIZE(other); i++) {
        select_name(&file, other[i]);
        CHECK(sumh_proc_proxy_kind_for_file(&file, &scope) == SUMH_PROC_PROXY_NONE);
    }
    const char *maps[] = {"maps", "smaps", "smaps_rollup"};
    for (size_t i = 0; i < ARRAY_SIZE(maps); i++) {
        select_name(&file, maps[i]);
        umount_app = 0;
        CHECK(sumh_proc_proxy_kind_for_file(&file, &scope) == SUMH_PROC_PROXY_NONE);
        umount_app = 1;
        CHECK(sumh_proc_proxy_kind_for_file(&file, &scope) == SUMH_PROC_PROXY_MAPS);
        sumh_feature_enabled_mask = SUMH_FEATURE_MOUNT_HIDE;
        CHECK(sumh_proc_proxy_kind_for_file(&file, &scope) == SUMH_PROC_PROXY_NONE);
        sumh_feature_enabled_mask |= SUMH_FEATURE_MAPS_SPOOF;
    }

    /* Exercise the real installer too: routing a VIEW app is insufficient
     * if installation still rejects its content scope. */
    umount_app = 0;
    select_name(&file, "mountinfo");
    kind = sumh_proc_proxy_kind_for_file(&file, &scope);
    CHECK(sumh_mount_proxy_install_file(&file, kind, scope) == 0);
    CHECK(allocated.scope == SUMH_POLICY_SCOPE_VIEW);
    CHECK(file.f_op == &allocated.proxy_fops && allocated.orig_fops == &original);
    CHECK(!allocated.maps_snapshot && !allocated.maps_snapshot_acquired);
    CHECK(file.f_mode == (FMODE_LSEEK | FMODE_PREAD));
    CHECK(file.f_op->read == sumh_mount_proxy_read && file.f_op->read_iter == sumh_mount_proxy_read_iter);
    CHECK(file.f_op->splice_read == sumh_mount_proxy_splice_read && file.f_op->poll == sumh_mount_proxy_poll);
    CHECK(sumh_mount_proxy_install_file(&file, kind, scope) == -EALREADY);
    CHECK(!lock_depth && sumh_proxy_live == 1);
    file.f_op = &original;
    task.uid = 99001;
    kind = sumh_proc_proxy_kind_for_file(&file, &scope);
    CHECK(sumh_mount_proxy_install_file(&file, kind, scope) == 0);
    CHECK(allocated.scope == SUMH_POLICY_SCOPE_SPOOF);
    CHECK(file.f_op == &allocated.proxy_fops && allocated.orig_fops == &original);
    CHECK(!allocated.maps_snapshot && !allocated.maps_snapshot_acquired);
    CHECK(file.f_op->read == sumh_mount_proxy_read && file.f_op->read_iter == sumh_mount_proxy_read_iter);
    file.f_op = &original; task.uid = 12345;
    allocation_fails = 1;
    CHECK(sumh_mount_proxy_install_file(&file, kind, scope) == -ENOMEM && file.f_op == &original);
    allocation_fails = 0; fops_get_fails = 1;
    CHECK(sumh_mount_proxy_install_file(&file, kind, scope) == -ENOENT && file.f_op == &original);
    CHECK(frees == 1 && !lock_depth);
    fops_get_fails = 0; sumh_proxy_shutdown = 1;
    CHECK(sumh_mount_proxy_install_file(&file, kind, scope) == -ESHUTDOWN);
    return 0;
}
"""
    return COMMON + PRIMITIVES + preamble + bodies + body


def snapshot_source():
    preamble = r"""
#define SUMH_EMBEDDED
#define MAJOR(dev) ((unsigned int)(dev) >> 16)
#define MINOR(dev) ((unsigned int)(dev) & 65535)
struct ksu_mount_field { const char *data; size_t len; };
struct ksu_mount_fields { struct ksu_mount_field dev, root, target, fstype, source, super; bool escaped; };
struct mount_entry { const char *mount_root, *mount_fstype, *umountable; unsigned int mount_dev; };
static struct mount_entry registered[4];
static int registered_count, rcu_depth, allocations, frees, allocation_fails;
#define list_for_each_entry_rcu(entry, head, member) \
    for (int entry_i = 0; entry_i < registered_count && ((entry) = &registered[entry_i], 1); entry_i++)
static void rcu_read_lock(void) { rcu_depth++; }
static void rcu_read_unlock(void) { rcu_depth--; }
static int scnprintf(char *out, size_t capacity, const char *format, unsigned int major, unsigned int minor) {
    unsigned int values[2] = {major, minor};
    size_t used = 0;
    (void)format;
    for (int i = 0; i < 2; i++) {
        char reversed[16]; size_t digits = 0;
        do { reversed[digits++] = '0' + values[i] % 10; values[i] /= 10; } while (values[i]);
        if (i && used + 1 < capacity) out[used++] = ':';
        while (digits && used + 1 < capacity) out[used++] = reversed[--digits];
    }
    if (capacity) out[used] = 0;
    return used;
}
static int kstrtou32(const char *input, unsigned int base, u32 *out) {
    unsigned long long value = 0;
    if (base != 10 || !*input) return -EINVAL;
    for (; *input; input++) {
        if (*input < '0' || *input > '9') return -EINVAL;
        value = value * 10 + (*input - '0');
        if (value > 4294967295ULL) return -EINVAL;
    }
    *out = value; return 0;
}
static unsigned long long allocation[2048];
static void *kvcalloc(size_t count, size_t size, int flags) {
    (void)flags; allocations++;
    if (allocation_fails || count > sizeof(allocation) / size) return NULL;
    memset(allocation, 0, count * size); return allocation;
}
static void kvfree(void *ptr) { if (ptr) frees++; }
static void sort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *), void *swap) {
    unsigned char *bytes = base; (void)swap;
    for (size_t i = 1; i < count; i++)
        for (size_t j = i; j && compare(bytes + (j - 1) * size, bytes + j * size) > 0; j--)
            for (size_t k = 0; k < size; k++) {
                unsigned char temp = bytes[(j - 1) * size + k];
                bytes[(j - 1) * size + k] = bytes[j * size + k]; bytes[j * size + k] = temp;
            }
}
struct sumh_mi_snapshot { char *data, *mounts; size_t len, mounts_len; size_t data_capacity, mounts_capacity; int refs; };
struct sumh_mi_row { u32 id, parent; u8 state; };
enum { SUMH_MI_UNVISITED, SUMH_MI_VISITING, SUMH_MI_VISIBLE, SUMH_MI_HIDDEN };
static char mi_buffer[4096], mo_buffer[4096];
static struct sumh_mi_snapshot snapshot;
static void input(const char *mi, const char *mo) {
    memset(mi_buffer, 0, sizeof(mi_buffer)); memset(mo_buffer, 0, sizeof(mo_buffer));
    memcpy(mi_buffer, mi, strlen(mi)); memcpy(mo_buffer, mo, strlen(mo));
    snapshot = (struct sumh_mi_snapshot){mi_buffer, mo_buffer, strlen(mi), strlen(mo)};
}
static bool equal(const char *actual, size_t length, const char *expected) {
    return length == strlen(expected) && !memcmp(actual, expected, length);
}
"""
    bodies = "".join(extract("kernel/infra/mount_policy.c", name)
                     for name in ("field_char", "field_matches", "private_path", "private_options",
                                  "ksu_mount_is_module"))
    bodies += "".join(extract("kernel/sumh/features/sumh_fake_mountinfo.c", name)
                      for name in ("sumh_mi_line_target", "sumh_mi_pair_matches", "sumh_mi_line_size",
                                   "sumh_mi_line_id", "sumh_mi_module_line", "sumh_mi_compare_rows",
                                   "sumh_mi_find_row", "sumh_mi_filter_native"))
    body = r"""
#define ROOT_MI "20 20 8:1 / / ro shared:400 - ext4 /dev/root ro\n"
#define ROOT_MO "/dev/root / ext4 ro 0 0\n"
#define SYSTEM_MI "42 20 253:0 / /system ro shared:402 - ext4 /dev/block/dm-0 ro\n"
#define SYSTEM_MO "/dev/block/dm-0 /system ext4 ro 0 0\n"
#define STORAGE_MI "65 20 0:55 / /storage/emulated rw master:900 - fuse /dev/fuse rw\n"
#define STORAGE_MO "/dev/fuse /storage/emulated fuse rw 0 0\n"
#define MIRROR_MI "80 70 253:0 /bin/sh /system/bin/sh ro - ext4 /dev/block/dm-0 ro\n"
#define MIRROR_MO "/dev/block/dm-0 /system/bin/sh ext4 ro 0 0\n"
#define SKELETON_MI "70 42 0:99 /system/bin /system/bin ro - tmpfs sumhp ro\n"
#define SKELETON_MO "sumhp /system/bin tmpfs ro 0 0\n"
#define GRANDCHILD_MI "90 80 253:0 /bin/sub /system/bin/sh/sub ro - ext4 /dev/block/dm-0 ro\n"
#define GRANDCHILD_MO "/dev/block/dm-0 /system/bin/sh/sub ext4 ro 0 0\n"
#define CONTENT_MI "71 42 8:2 /adb/modules/example/system/etc/hosts /system/etc/hosts ro - ext4 /dev/block/data rw\n"
#define CONTENT_MO "/dev/block/data /system/etc/hosts ext4 ro 0 0\n"
int main(void) {
    const char live_mi[] = ROOT_MI SYSTEM_MI MIRROR_MI GRANDCHILD_MI SKELETON_MI CONTENT_MI STORAGE_MI;
    const char live_mo[] = ROOT_MO SYSTEM_MO MIRROR_MO GRANDCHILD_MO SKELETON_MO CONTENT_MO STORAGE_MO;
    registered[0] = (struct mount_entry){ "/system/bin", "tmpfs", "/system/bin", 99 };
    registered_count = 1;
    input(live_mi, live_mo);
    CHECK(sumh_mi_filter_native(&snapshot) == 0);
    CHECK(equal(snapshot.data, snapshot.len, ROOT_MI SYSTEM_MI STORAGE_MI));
    CHECK(equal(snapshot.mounts, snapshot.mounts_len, ROOT_MO SYSTEM_MO STORAGE_MO));
    CHECK(!rcu_depth && allocations == frees);
    /* The source namespace snapshot is unchanged: only the private proc
     * output buffers lose module rows. Native IDs, parents and propagation
     * groups of the remaining storage/system mounts stay byte-identical. */
    CHECK(!strcmp(live_mi, ROOT_MI SYSTEM_MI MIRROR_MI GRANDCHILD_MI SKELETON_MI CONTENT_MI STORAGE_MI));
    CHECK(!strcmp(live_mo, ROOT_MO SYSTEM_MO MIRROR_MO GRANDCHILD_MO SKELETON_MO CONTENT_MO STORAGE_MO));

    /* OverlayFS staging and Magic Mount file binds use the same generic
     * policy, independent of the module directory and changed filename. */
    const char *module_mi[] = {
        ROOT_MI SYSTEM_MI
        "110 20 0:101 / /mnt/random-stage rw - tmpfs KSU rw,seclabel\n"
        "111 42 0:102 / /system/fonts rw - overlay KSU ro,lowerdir=/mnt/random-stage/example/system/fonts:.,redirect_dir=on\n",
        ROOT_MI SYSTEM_MI
        "120 42 0:103 /system/fonts /system/fonts ro - tmpfs KSU rw,seclabel\n"
        "121 120 253:55 /adb/modules/example/system/fonts/Example.ttf /system/fonts/Example.ttf ro - f2fs /dev/block/dm-55 rw,seclabel\n",
    };
    const char *module_mo[] = {
        ROOT_MO SYSTEM_MO
        "KSU /mnt/random-stage tmpfs rw,seclabel 0 0\n"
        "KSU /system/fonts overlay ro,lowerdir=/mnt/random-stage/example/system/fonts:.,redirect_dir=on 0 0\n",
        ROOT_MO SYSTEM_MO
        "KSU /system/fonts tmpfs ro,seclabel 0 0\n"
        "/dev/block/dm-55 /system/fonts/Example.ttf f2fs ro,seclabel 0 0\n",
    };
    for (size_t i = 0; i < ARRAY_SIZE(module_mi); i++) {
        input(module_mi[i], module_mo[i]);
        CHECK(sumh_mi_filter_native(&snapshot) == 0);
        CHECK(equal(snapshot.data, snapshot.len, ROOT_MI SYSTEM_MI));
        CHECK(equal(snapshot.mounts, snapshot.mounts_len, ROOT_MO SYSTEM_MO) && !rcu_depth);
    }

    /* A reused target with a different device/root must remain visible. */
    registered[0].mount_dev = 100;
    input(ROOT_MI SYSTEM_MI MIRROR_MI SKELETON_MI, ROOT_MO SYSTEM_MO MIRROR_MO SKELETON_MO);
    CHECK(sumh_mi_filter_native(&snapshot) == 0);
    CHECK(equal(snapshot.data, snapshot.len, ROOT_MI SYSTEM_MI MIRROR_MI SKELETON_MI));
    registered[0].mount_dev = 99;
    registered[0].mount_root = "/other";
    input(ROOT_MI SYSTEM_MI SKELETON_MI, ROOT_MO SYSTEM_MO SKELETON_MO);
    CHECK(sumh_mi_filter_native(&snapshot) == 0);
    CHECK(equal(snapshot.data, snapshot.len, ROOT_MI SYSTEM_MI SKELETON_MI));
    registered[0].mount_root = "/system/bin";

    /* Escaped identity is classified by the real embedded mount policy. */
    registered[1] = (struct mount_entry){ "/system/app space", "tmpfs", "/system/app space", 100 };
    registered_count = 2;
    input(ROOT_MI "101 20 0:100 /system/app\\040space /system/app\\040space ro - tmpfs sumhp ro\n",
          ROOT_MO "sumhp /system/app\\040space tmpfs ro 0 0\n");
    CHECK(sumh_mi_filter_native(&snapshot) == 0);
    CHECK(equal(snapshot.data, snapshot.len, ROOT_MI) && equal(snapshot.mounts, snapshot.mounts_len, ROOT_MO));

    /* Output pairs must refer to the same topology and have unique IDs. */
    int previous_allocs = allocations;
    input(ROOT_MI SYSTEM_MI, ROOT_MO STORAGE_MO);
    CHECK(sumh_mi_filter_native(&snapshot) == -EINVAL && allocations == previous_allocs);
    CHECK(equal(snapshot.data, snapshot.len, ROOT_MI SYSTEM_MI));
    input(ROOT_MI ROOT_MI, ROOT_MO ROOT_MO);
    CHECK(sumh_mi_filter_native(&snapshot) == -EINVAL);
    input(ROOT_MI "30 31 8:1 / /first ro - ext4 /dev/root ro\n"
          "31 30 8:1 / /second ro - ext4 /dev/root ro\n",
          ROOT_MO "/dev/root /first ext4 ro 0 0\n/dev/root /second ext4 ro 0 0\n");
    CHECK(sumh_mi_filter_native(&snapshot) == -EINVAL);
    input("4294967296 20 8:1 / / ro - ext4 /dev/root ro\n", ROOT_MO);
    CHECK(sumh_mi_filter_native(&snapshot) == -EINVAL);
    input(ROOT_MI, ROOT_MO);
    allocation_fails = 1;
    CHECK(sumh_mi_filter_native(&snapshot) == -ENOMEM);
    CHECK(equal(snapshot.data, snapshot.len, ROOT_MI) && equal(snapshot.mounts, snapshot.mounts_len, ROOT_MO));
    CHECK(allocations == frees + 1 && !rcu_depth);
    allocation_fails = 0;
    /* A namespace root may name a parent outside the observed root. */
    input("22 1 8:1 / / ro - ext4 /dev/root ro", "/dev/root / ext4 ro 0 0");
    CHECK(sumh_mi_filter_native(&snapshot) == 0);
    CHECK(equal(snapshot.data, snapshot.len, "22 1 8:1 / / ro - ext4 /dev/root ro"));
    CHECK(equal(snapshot.mounts, snapshot.mounts_len, "/dev/root / ext4 ro 0 0"));
    return 0;
}
"""
    return COMMON + PRIMITIVES + memory_source() + preamble + bodies + body


def local_snapshot_source():
    # Reuse the production classifier, paired filter and their kernel doubles.
    source = snapshot_source().split("int main(void) {", 1)[0]
    preamble = r"""
typedef unsigned long long u64;
#define EAGAIN 11
#define EOPNOTSUPP 95
#define SUMH_MI_INITIAL_SIZE 4096
#define SUMH_MI_MAX_SIZE 4096
#define refcount_set(p, value) (*(p) = (value))
#define refcount_dec_and_test(p) (--*(p) == 0)
struct mnt_namespace { const char *mi, *mo; } main_ns, isolated_ns;
struct vfsmount { int unused; } mount;
struct seq_file;
typedef int (*sumh_mi_show_fn)(struct seq_file *, struct vfsmount *);
struct sumh_proc_mounts_prefix {
    struct mnt_namespace *ns;
    struct { void *mnt, *dentry; } root;
    sumh_mi_show_fn show;
};
struct seq_file { struct sumh_proc_mounts_prefix *private; unsigned long poll_event; } sequence;
struct file { struct seq_file *private_data; };
struct file_operations {
    long (*llseek)(struct file *, long, int);
    unsigned int (*poll)(struct file *, void *);
};
static struct sumh_mi_snapshot allocated[2];
static char buffers[4][SUMH_MI_INITIAL_SIZE];
static int sumh_mi_snapshot_lock, lock_depth, alloc_index, buffer_index, failed_allocations;
static int fail_allocation, render_calls, render_error_at, policy_changes, policy_reads, odd_policy_reads;
static int polls, event_changes;
static u64 generation;
static void mutex_lock(int *lock) { (void)lock; lock_depth++; }
static void mutex_unlock(int *lock) { (void)lock; lock_depth--; }
static void *kzalloc(size_t size, int flags) {
    (void)flags;
    if (++allocations == fail_allocation || size != sizeof(allocated[0]) || alloc_index == 2) {
        failed_allocations++; return NULL;
    }
    memset(&allocated[alloc_index], 0, size); return &allocated[alloc_index++];
}
static void *kvmalloc(size_t size, int flags) {
    (void)flags;
    if (++allocations == fail_allocation || size > sizeof(buffers[0]) || buffer_index == 4) {
        failed_allocations++; return NULL;
    }
    return buffers[buffer_index++];
}
static void kfree(void *ptr) { if (ptr) frees++; }
static int original_show(struct seq_file *seq, struct vfsmount *mnt) { (void)seq; (void)mnt; return 0; }
static int sumh_mi_show_mountinfo(struct seq_file *seq, struct vfsmount *mnt) { (void)seq; (void)mnt; return 0; }
static int sumh_mi_show_mounts(struct seq_file *seq, struct vfsmount *mnt) { (void)seq; (void)mnt; return 0; }
static long seek_file(struct file *file, long offset, int whence) { (void)file; (void)whence; return offset; }
static unsigned int poll_file(struct file *file, void *wait) {
    (void)wait;
    if (++polls % 3 == 0 && event_changes) { event_changes--; file->private_data->poll_event++; }
    return 0;
}
static u64 ksu_mount_policy_generation(void) {
    if (odd_policy_reads) { odd_policy_reads--; return generation | 1; }
    if (++policy_reads % 2 == 0 && policy_changes) { policy_changes--; generation += 2; }
    return generation;
}
static int sumh_mi_render(struct file *file, const struct file_operations *ops,
        sumh_mi_show_fn show, char **data, size_t *len, size_t *capacity) {
    (void)ops;
    struct sumh_proc_mounts_prefix *pm = file->private_data->private;
    pm->show = show;
    if (++render_calls == render_error_at) return -EINVAL;
    const char *raw = show == sumh_mi_show_mountinfo ? pm->ns->mi : pm->ns->mo;
    *len = strlen(raw);
    if (*len > *capacity) return -EINVAL;
    memcpy(*data, raw, *len); return 0;
}
static int sumh_mi_normalize_groups(char *data, size_t *len) { (void)data; (void)len; return 0; }
static struct sumh_proc_mounts_prefix proc_mounts;
static struct file proc_file;
static struct file_operations operations;
#define MAIN_MI ROOT_MI "42871 20 254:13 / /vendor ro - erofs /dev/block/dm-13 ro\n" \
    "42881 20 0:24 / /apex rw - tmpfs tmpfs rw\n" STORAGE_MI
#define ISOLATED_MI ROOT_MI "52871 20 254:13 / /vendor ro - erofs /dev/block/dm-13 ro\n" \
    "52881 20 0:24 / /apex rw - tmpfs tmpfs rw\n" STORAGE_MI
#define ANCHOR_MO ROOT_MO "/dev/block/dm-13 /vendor erofs ro 0 0\n" \
    "tmpfs /apex tmpfs rw 0 0\n" STORAGE_MO
#define MODULE_MI "60001 20 0:101 /system/fonts /system/fonts ro - tmpfs KSU rw\n" \
    "60002 60001 253:55 /adb/modules/example/system/fonts/Example.ttf /system/fonts/Example.ttf ro - f2fs /dev/block/dm-55 rw\n"
#define MODULE_MO "KSU /system/fonts tmpfs ro 0 0\n" \
    "/dev/block/dm-55 /system/fonts/Example.ttf f2fs ro 0 0\n"
static void reset(void) {
    allocations = frees = allocation_fails = rcu_depth = registered_count = 0;
    failed_allocations = alloc_index = buffer_index = fail_allocation = lock_depth = 0;
    render_calls = render_error_at = policy_changes = policy_reads = odd_policy_reads = polls = event_changes = 0;
    generation = 2;
    main_ns = (struct mnt_namespace){MAIN_MI MODULE_MI, ANCHOR_MO MODULE_MO};
    isolated_ns = (struct mnt_namespace){ISOLATED_MI MODULE_MI, ANCHOR_MO MODULE_MO};
    proc_mounts = (struct sumh_proc_mounts_prefix){&main_ns, {&mount, &mount}, original_show};
    sequence = (struct seq_file){&proc_mounts, 0};
    proc_file.private_data = &sequence;
    operations = (struct file_operations){seek_file, poll_file};
}
"""
    bodies = "".join(extract("kernel/sumh/features/sumh_fake_mountinfo.c", name)
                     for name in ("sumh_mi_alloc_buffer", "sumh_mi_free_buffer",
                                  "sumh_fake_mi_put_snapshot", "sumh_fake_mi_get_snapshot"))
    body = r"""
int main(void) {
    struct sumh_mi_snapshot *result, *other;
    reset();
    CHECK(!sumh_fake_mi_get_snapshot(&proc_file, &operations, &result));
    CHECK(equal(result->data, result->len, MAIN_MI));
    CHECK(equal(result->mounts, result->mounts_len, ANCHOR_MO));
    CHECK(result->refs == 1 && proc_mounts.ns == &main_ns && proc_mounts.show == original_show);
    /* The helper's raw statx/fdinfo IDs must resolve against its own table.
     * Same paths/devices in a different namespace do not share mount IDs.
     * Keep both snapshots alive to catch shared backing buffers as well. */
    struct sumh_proc_mounts_prefix isolated_pm = {&isolated_ns, {&mount, &mount}, original_show};
    struct seq_file isolated_seq = {&isolated_pm, 0};
    struct file isolated_file = {&isolated_seq};
    CHECK(!sumh_fake_mi_get_snapshot(&isolated_file, &operations, &other));
    CHECK(other != result && other->data != result->data && other->mounts != result->mounts && other->refs == 1);
    CHECK(equal(other->data, other->len, ISOLATED_MI));
    CHECK(equal(other->mounts, other->mounts_len, ANCHOR_MO));
    CHECK(strnstr(other->data, "52871 20 254:13 / /vendor", other->len));
    CHECK(strnstr(other->data, "52881 20 0:24 / /apex", other->len));
    CHECK(!strnstr(other->data, "42871", other->len) && !strnstr(other->data, "42881", other->len));
    CHECK(equal(result->data, result->len, MAIN_MI));
    CHECK(proc_mounts.ns == &main_ns && isolated_pm.ns == &isolated_ns);
    CHECK(isolated_pm.root.mnt == &mount && isolated_pm.root.dentry == &mount && isolated_pm.show == original_show);
    sumh_fake_mi_put_snapshot(result); sumh_fake_mi_put_snapshot(other);
    CHECK(allocations == frees && !lock_depth && !rcu_depth);

    /* A later open observes fresh state, with no frozen process/global cache. */
    reset();
    CHECK(!sumh_fake_mi_get_snapshot(&proc_file, &operations, &result));
    main_ns.mi = MAIN_MI "70000 20 0:100 / /mnt/new rw - tmpfs tmpfs rw\n";
    main_ns.mo = ANCHOR_MO "tmpfs /mnt/new tmpfs rw 0 0\n";
    CHECK(!sumh_fake_mi_get_snapshot(&proc_file, &operations, &other));
    CHECK(strnstr(other->data, "/mnt/new", other->len));
    CHECK(!strnstr(result->data, "/mnt/new", result->len));
    CHECK(render_calls == 4);
    sumh_fake_mi_put_snapshot(result); sumh_fake_mi_put_snapshot(other);
    CHECK(allocations == frees && !lock_depth);

    for (int change = 0; change < 2; change++) {
        reset();
        if (change) event_changes = 1; else policy_changes = 1;
        CHECK(!sumh_fake_mi_get_snapshot(&proc_file, &operations, &result));
        CHECK(render_calls == 4 && result->refs == 1 && equal(result->data, result->len, MAIN_MI));
        sumh_fake_mi_put_snapshot(result);
        CHECK(allocations == frees && !lock_depth && proc_mounts.show == original_show);
    }
    for (int change = 0; change < 3; change++) {
        reset();
        if (change == 0) policy_changes = 3;
        if (change == 1) event_changes = 3;
        if (change == 2) odd_policy_reads = 3;
        CHECK(sumh_fake_mi_get_snapshot(&proc_file, &operations, &result) == -EAGAIN);
        CHECK(!result && render_calls == (change == 2 ? 0 : 6));
        CHECK(allocations == frees && !lock_depth && proc_mounts.show == original_show);
    }
    for (int failure = 0; failure < 4; failure++) {
        reset();
        if (failure < 2) render_error_at = failure + 1;
        if (failure == 2) main_ns.mi = "bad line\n";
        if (failure == 3) main_ns.mo = ROOT_MO;
        CHECK(sumh_fake_mi_get_snapshot(&proc_file, &operations, &result) == (failure < 2 ? -EINVAL : -EAGAIN));
        CHECK(!result && allocations == frees && !lock_depth && proc_mounts.show == original_show);
    }
    reset(); main_ns.mi = "bad parent 8:1 / / ro - ext4 /dev/root ro\n"; main_ns.mo = ROOT_MO;
    CHECK(sumh_fake_mi_get_snapshot(&proc_file, &operations, &result) == -EINVAL);
    CHECK(!result && allocations == frees && !lock_depth);
    for (int fail = 1; fail <= 4; fail++) {
        reset();
        if (fail == 4) allocation_fails = 1; else fail_allocation = fail;
        CHECK(sumh_fake_mi_get_snapshot(&proc_file, &operations, &result) == -ENOMEM);
        CHECK(!result && allocations == frees + failed_allocations + (fail == 4));
        CHECK(!lock_depth && !rcu_depth && proc_mounts.show == original_show);
    }
    reset(); operations.llseek = NULL;
    CHECK(sumh_fake_mi_get_snapshot(&proc_file, &operations, &result) == -EOPNOTSUPP);
    CHECK(!result && !allocations && !render_calls && !lock_depth);
    return 0;
}
"""
    return source + preamble + bodies + body


def proxy_read_source():
    preamble = r"""
typedef long long loff_t;
typedef unsigned int __poll_t;
typedef unsigned long long u64;
#define __user
#define EIO 5
#define EFAULT 14
#define ESPIPE 29
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define MAX_LFS_FILESIZE 0x7fffffffffffffffLL
#define EPOLLIN 1
#define EPOLLRDNORM 2
#define EPOLLERR 4
#define EPOLLPRI 8
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define container_of(p, t, m) ((t *)((char *)(p) - __builtin_offsetof(t, m)))
enum sumh_proc_proxy_kind { SUMH_PROC_PROXY_NONE, SUMH_PROC_PROXY_MOUNTINFO,
    SUMH_PROC_PROXY_MOUNTS, SUMH_PROC_PROXY_MAPS, SUMH_PROC_PROXY_MOUNTINFO_GROUPS };
struct file;
struct poll_table_struct { int unused; };
struct pipe_inode_info { int unused; };
struct file_operations {
    ssize_t (*read)(struct file *, char *, size_t, loff_t *);
    loff_t (*llseek)(struct file *, loff_t, int);
    __poll_t (*poll)(struct file *, struct poll_table_struct *);
    ssize_t (*splice_read)(struct file *, loff_t *, struct pipe_inode_info *, size_t, unsigned int);
};
struct file { const struct file_operations *f_op; loff_t f_pos; };
struct kiocb { struct file *ki_filp; loff_t ki_pos; };
struct iov_iter { char *buffer; size_t count; };
struct mnt_namespace { int unused; };
struct sumh_mi_snapshot { char *data, *mounts; size_t len, mounts_len; };
struct sumh_mount_file_proxy {
    struct file_operations proxy_fops;
    const struct file_operations *orig_fops;
    enum sumh_proc_proxy_kind kind;
    int stream_lock, mountinfo_error;
    bool mountinfo_checked;
    struct sumh_mi_snapshot *mountinfo_snapshot;
    size_t stream_raw_len;
    u64 stream_generation;
};
static int sumh_proxy_srcu, lock_depth, srcu_depth, raw_reads, preparations, result_error, missing_snapshot;
static int puts, seek_error, native_polls, view_polls;
static int native_splices;
static __poll_t native_events;
static u64 view_generation = 1;
static struct sumh_mi_snapshot prepared = {"clean info\n", "clean mounts\n", 11, 13};
static void mutex_lock(int *lock) { (void)lock; lock_depth++; }
static void mutex_unlock(int *lock) { (void)lock; lock_depth--; }
static int srcu_read_lock(int *lock) { (void)lock; srcu_depth++; return 0; }
static void srcu_read_unlock(int *lock, int index) { (void)lock; (void)index; srcu_depth--; }
static size_t copy_to_user(char *dst, const char *src, size_t count) { memcpy(dst, src, count); return 0; }
static size_t copy_to_iter(const char *src, size_t count, struct iov_iter *to) {
    count = min_t(size_t, count, to->count);
    memcpy(to->buffer, src, count); to->buffer += count; to->count -= count; return count;
}
static size_t iov_iter_count(struct iov_iter *to) { return to->count; }
static void sumh_fake_mi_put_snapshot(struct sumh_mi_snapshot *snapshot) { if (snapshot) puts++; }
static u64 sumh_fake_mi_generation(void) { return view_generation; }
static void sumh_fake_mi_poll_wait(struct file *file, struct poll_table_struct *wait) {
    (void)file; (void)wait; view_polls++;
}
static loff_t seek_file(struct file *file, loff_t offset, int whence) {
    (void)whence;
    if (seek_error) return seek_error;
    file->f_pos = offset; return offset;
}
static __poll_t poll_file(struct file *file, struct poll_table_struct *wait) {
    (void)file; (void)wait; native_polls++; return native_events;
}
static loff_t generic_file_llseek_size(struct file *file, loff_t offset, int whence, loff_t max, loff_t len) {
    (void)max;
    if (whence == SEEK_END) offset += len;
    if (whence == SEEK_CUR) offset += file->f_pos;
    if (offset < 0) return -EINVAL;
    return file->f_pos = offset;
}
static int sumh_fake_mi_get_snapshot(struct file *file, const struct file_operations *ops,
        struct sumh_mi_snapshot **out) {
    (void)file; (void)ops; preparations++;
    *out = result_error || missing_snapshot ? NULL : &prepared;
    return result_error;
}
static ssize_t raw_read(struct file *file, char *buffer, size_t count, loff_t *pos) {
    (void)file; (void)buffer; (void)count; (void)pos; raw_reads++; return -EINVAL;
}
static ssize_t raw_splice(struct file *file, loff_t *pos, struct pipe_inode_info *pipe,
        size_t count, unsigned int flags) {
    (void)file; (void)pos; (void)pipe; (void)count; (void)flags; native_splices++; return 0;
}
static int sumh_mount_proxy_prepare_groups(struct sumh_mount_file_proxy *proxy, struct file *file) {
    (void)proxy; (void)file; return -EINVAL;
}
static ssize_t sumh_mount_proxy_orig_read_iter(struct sumh_mount_file_proxy *proxy,
        struct kiocb *iocb, struct iov_iter *to) {
    (void)proxy; (void)iocb; (void)to; raw_reads++; return -EINVAL;
}
static bool sumh_mount_proxy_filter_active(struct sumh_mount_file_proxy *proxy) { (void)proxy; return false; }
static ssize_t sumh_mount_proxy_read_groups(struct sumh_mount_file_proxy *proxy,
        struct file *file, char *buffer, struct iov_iter *to, size_t count, loff_t *pos) {
    (void)proxy; (void)file; (void)buffer; (void)to; (void)count; (void)pos; return -EINVAL;
}
static ssize_t sumh_mount_proxy_filtered_read(struct sumh_mount_file_proxy *proxy,
        struct file *file, char *buffer, struct iov_iter *to, size_t count, loff_t *pos) {
    (void)proxy; (void)file; (void)buffer; (void)to; (void)count; (void)pos; return -EINVAL;
}
"""
    bodies = "".join(extract("kernel/sumh/hooks/sumh_proc_read_hooks.c", name)
                     for name in ("sumh_mount_proxy_prepare_mountinfo", "sumh_mount_proxy_read_buffer",
                                  "sumh_mount_proxy_read_snapshot", "sumh_mount_proxy_read",
                                  "sumh_mount_proxy_read_iter", "sumh_mount_proxy_llseek",
                                  "sumh_mount_proxy_poll", "sumh_mount_proxy_splice_read"))
    body = r"""
int main(void) {
    struct file_operations original = {raw_read, seek_file, poll_file, raw_splice};
    for (int mounts = 0; mounts < 2; mounts++) for (int iter = 0; iter < 2; iter++) {
        for (int failure = 0; failure < 4; failure++) {
            struct sumh_mount_file_proxy proxy = {0};
            proxy.orig_fops = &original;
            proxy.kind = mounts ? SUMH_PROC_PROXY_MOUNTS : SUMH_PROC_PROXY_MOUNTINFO;
            struct file file = {&proxy.proxy_fops, 0};
            struct kiocb iocb = {&file, 0};
            char output[32] = {0};
            struct iov_iter to = {output, 0};
            loff_t pos = 0;
            preparations = raw_reads = 0;
            result_error = failure == 1 ? -ENOMEM : failure == 3 ? -EIO : 0;
            missing_snapshot = failure == 2;
            CHECK((iter ? sumh_mount_proxy_read_iter(&iocb, &to)
                        : sumh_mount_proxy_read(&file, output, 0, &pos)) == 0);
            CHECK(!preparations && !proxy.mountinfo_checked);
            to.count = 3;
            ssize_t ret = iter ? sumh_mount_proxy_read_iter(&iocb, &to)
                               : sumh_mount_proxy_read(&file, output, 3, &pos);
            CHECK(preparations == 1 && proxy.mountinfo_checked);
            CHECK(!raw_reads && !lock_depth && !srcu_depth);
            if (failure) {
                CHECK(ret == (failure == 1 ? -ENOMEM : -EIO));
                CHECK(!pos && !iocb.ki_pos && !output[0]);
                native_splices = 0;
                CHECK(sumh_mount_proxy_splice_read(&file, &pos, NULL, 1, 0) ==
                      (failure == 1 ? -ENOMEM : -EIO));
                CHECK(!native_splices && !raw_reads && !pos && !lock_depth);
            } else {
                CHECK(ret == 3 && !memcmp(output, "cle", 3));
                size_t length = mounts ? prepared.mounts_len : prepared.len;
                const char *expected = mounts ? prepared.mounts : prepared.data;
                to.count = sizeof(output) - 3;
                ret = iter ? sumh_mount_proxy_read_iter(&iocb, &to)
                           : sumh_mount_proxy_read(&file, output + 3, sizeof(output) - 3, &pos);
                CHECK(ret == (ssize_t)(length - 3) && !memcmp(output, expected, length));
                CHECK((iter ? iocb.ki_pos : pos) == (loff_t)length);
                CHECK((iter ? sumh_mount_proxy_read_iter(&iocb, &to)
                            : sumh_mount_proxy_read(&file, output, 1, &pos)) == 0);
                CHECK(preparations == 1 && !raw_reads && !lock_depth && !srcu_depth);
            }
            /* Rewind must reset both a valid snapshot and a failed read;
             * subsequent reads render again without borrowing any cache. */
            puts = 0;
            CHECK(!sumh_mount_proxy_llseek(&file, 0, SEEK_SET));
            CHECK(puts == !failure && !proxy.mountinfo_checked && !proxy.mountinfo_error);
            CHECK(!proxy.mountinfo_snapshot && !file.f_pos);
            result_error = missing_snapshot = 0;
            CHECK(sumh_mount_proxy_read(&file, output, 3, &file.f_pos) == 3);
            CHECK(preparations == 2 && !memcmp(output, "cle", 3) && !raw_reads);

            native_polls = view_polls = 0;
            native_events = EPOLLIN | EPOLLRDNORM;
            proxy.stream_generation = view_generation;
            CHECK(sumh_mount_proxy_poll(&file, NULL) == native_events);
            CHECK(native_polls == 1 && view_polls == 1);
            native_events |= EPOLLERR | EPOLLPRI;
            CHECK(sumh_mount_proxy_poll(&file, NULL) == native_events);
            native_events = EPOLLIN | EPOLLRDNORM;
            view_generation++;
            CHECK(sumh_mount_proxy_poll(&file, NULL) == (native_events | EPOLLERR | EPOLLPRI));
            CHECK(sumh_mount_proxy_poll(&file, NULL) == native_events);
            proxy.mountinfo_error = -ENOMEM;
            CHECK(sumh_mount_proxy_poll(&file, NULL) == (native_events | EPOLLERR));
            seek_error = -EIO;
            CHECK(sumh_mount_proxy_llseek(&file, 0, SEEK_SET) == -EIO);
            CHECK(proxy.mountinfo_snapshot == &prepared && proxy.mountinfo_checked && proxy.mountinfo_error == -ENOMEM);
            seek_error = 0;
            original.llseek = NULL;
            CHECK(sumh_mount_proxy_llseek(&file, 0, SEEK_SET) == -ESPIPE);
            original.llseek = seek_file;
            CHECK(!lock_depth && !srcu_depth);
        }
    }
    return 0;
}
"""
    return COMMON + PRIMITIVES + preamble + bodies + body


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler, "retained mount-view policy and proc installation", policy_source())
    run_test(compiler, "native paired mount snapshots and mirrored descendants", snapshot_source())
    run_test(compiler, "namespace-local anchors, private snapshots and retries", local_snapshot_source())
    run_test(compiler, "mount proxy partial reads, rewind, poll and snapshot errors", proxy_read_source())
