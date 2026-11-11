#!/usr/bin/env python3
"""Exercise SUMHP's SUMH mount-registration contract and production registry.

The kernel path lookup, allocation and RCU boundaries are doubled. No real
mount operation runs. Use --zig on Windows or a native --cc compiler on Linux.
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
    match = re.search(r"^[\w\s*]+\b" + name + r"\s*\([^;{}]*\)\s*\{", text, re.M)
    if not match:
        raise ValueError(f"missing {name} in {path}")
    end = text.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end] + "\n"


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uapi/sumh.h"
_Static_assert(sizeof(struct sumh_managed_mount_arg) == 4112, "ABI size");
_Static_assert(offsetof(struct sumh_managed_mount_arg, path) == 16, "ABI path");
_Static_assert(SUMH_PROTOCOL_VERSION == 3, "protocol must reject old clients");
#define __user
#define GFP_KERNEL 0
#undef PATH_MAX
#define PATH_MAX 4096
#define MNT_DETACH 2
#define LOOKUP_FOLLOW 1
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define MAJOR(d) ((unsigned)((d) >> 16))
#define MINOR(d) ((unsigned)((d) & 0xffff))
typedef uint64_t u64;
#define dev_t uint32_t
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define list_entry(p, type, member) container_of(p, type, member)
#define list_for_each_entry(p, head, member) \
    for (p = list_entry((head)->next, __typeof__(*p), member); \
         &p->member != (head); p = list_entry(p->member.next, __typeof__(*p), member))
#define list_for_each_entry_rcu list_for_each_entry
#define list_for_each_entry_safe(p, n, head, member) \
    for (p = list_entry((head)->next, __typeof__(*p), member), \
         n = list_entry(p->member.next, __typeof__(*p), member); \
         &p->member != (head); p = n, n = list_entry(n->member.next, __typeof__(*n), member))
static unsigned lock_depth, read_depth, notifications;
static u64 mount_policy_generation = 2;
static int mount_list_lock, mount_policy_wait;
#define smp_rmb() ((void)0)
#define smp_wmb() ((void)0)
#define atomic64_read(p) (*(p))
#define atomic64_inc(p) (++*(p))
static void down_write(int *lock) { (void)lock; assert(!lock_depth); ++lock_depth; }
static void up_write(int *lock) { (void)lock; assert(lock_depth == 1); --lock_depth; }
static void down_read(int *lock) { (void)lock; assert(!lock_depth); ++read_depth; }
static void up_read(int *lock) { (void)lock; assert(read_depth == 1); --read_depth; }
static void wake_up_all(int *queue) { (void)queue; assert(lock_depth == 1); ++notifications; }
static void rcu_read_lock(void) { ++read_depth; }
static void rcu_read_unlock(void) { assert(read_depth); --read_depth; }
static void list_add_rcu(struct list_head *entry, struct list_head *head) {
    assert(lock_depth == 1 && (mount_policy_generation & 1));
    entry->next = head->next; entry->prev = head;
    head->next->prev = entry; head->next = entry;
}
static void list_replace_rcu(struct list_head *old, struct list_head *entry) {
    assert(lock_depth == 1 && (mount_policy_generation & 1));
    entry->next = old->next; entry->prev = old->prev;
    old->next->prev = entry; old->prev->next = entry;
}
static void list_del_rcu(struct list_head *entry) {
    assert(lock_depth == 1 && (mount_policy_generation & 1));
    entry->next->prev = entry->prev; entry->prev->next = entry->next;
}
struct rcu_head { void (*release)(struct rcu_head *); };
static struct rcu_head *retired[64];
static unsigned retired_count;
static void call_rcu(struct rcu_head *head, void (*release)(struct rcu_head *)) {
    assert(lock_depth == 1 && retired_count < 64);
    head->release = release; retired[retired_count++] = head;
}
static void flush_rcu(void) {
    assert(!lock_depth && !read_depth);
    while (retired_count) { struct rcu_head *p = retired[--retired_count]; p->release(p); }
}
struct mount_entry {
    char *umountable; unsigned int flags; dev_t mount_dev;
    char *mount_root, *mount_fstype;
    struct list_head list; struct rcu_head rcu;
};
static struct list_head mount_list = LIST_HEAD_INIT(mount_list);
struct ksu_mount_field { const char *data; size_t len; };
struct ksu_mount_fields {
    struct ksu_mount_field dev, root, target, fstype, source, super;
    bool escaped;
};
#define scnprintf snprintf
static unsigned allocations, fail_allocation, live_allocations;
static bool input_fault;
static void *kmalloc(size_t size, int flags) {
    (void)flags; assert(!lock_depth);
    if (++allocations == fail_allocation) return NULL;
    void *p = malloc(size); assert(p); ++live_allocations; return p;
}
static void *kzalloc(size_t size, int flags) {
    void *p = kmalloc(size, flags); if (p) memset(p, 0, size); return p;
}
static void *kmalloc_array(size_t count, size_t size, int flags) { return kmalloc(count * size, flags); }
static char *kstrdup(const char *s, int flags) {
    size_t size = strlen(s) + 1; char *p = kmalloc(size, flags);
    if (p) memcpy(p, s, size); return p;
}
static void kfree(void *p) {
    if (p) { assert(live_allocations); --live_allocations; free(p); }
}
static void *memdup_user(const void *input, size_t size) {
    if (input_fault) return ERR_PTR(-EFAULT);
    void *p = kmalloc(size, 0); if (!p) return ERR_PTR(-ENOMEM);
    memcpy(p, input, size); return p;
}
struct file_system_type { const char *name; } fs_type = {"ext4"};
struct super_block { dev_t s_dev; struct file_system_type *s_type; } sb = {0x70013, &fs_type};
struct dentry { int unused; } mount_root, ordinary_file;
struct vfsmount { struct dentry *mnt_root; struct super_block *mnt_sb; } mount = {&mount_root, &sb};
struct path { struct dentry *dentry; struct vfsmount *mnt; };
static int lookup_error, identity_error;
static bool not_mountpoint;
static unsigned lookups, path_refs;
static const char *identity_root = "/module";
static const char *lookup_path;
static int kern_path(const char *name, int flags, struct path *path) {
    assert(!lock_depth && name[0] == '/' && (flags == LOOKUP_FOLLOW || flags == 0)); ++lookups;
    lookup_path = name;
    if (lookup_error) return lookup_error;
    path->mnt = &mount; path->dentry = not_mountpoint ? &ordinary_file : &mount_root;
    ++path_refs; return 0;
}
static char *dentry_path_raw(struct dentry *dentry, char *buffer, size_t size) {
    assert(dentry == &mount_root && path_refs);
    if (identity_error) return ERR_PTR(identity_error);
    assert(strlen(identity_root) < size); strcpy(buffer, identity_root); return buffer;
}
static void path_put(struct path *path) { (void)path; assert(path_refs); --path_refs; }
static unsigned detach_calls;
static int detach_error;
static const char *detached[4];
static int path_umount(struct path *path, int flags) {
    assert(read_depth == 1 && flags == MNT_DETACH && detach_calls < 4);
    detached[detach_calls++] = lookup_path;
    path_put(path); return detach_error;
}
#define pr_info(...) ((void)0)
#define KSU_UMOUNT_MAX_TARGETS 512
static void sort(void *items, size_t count, size_t size, int (*cmp)(const void *, const void *), void *swap) {
    (void)swap; qsort(items, count, size, cmp);
}
struct ksu_umount_target { char *path, *root, *fstype; dev_t dev; unsigned int order; };
'''

DETACH_BODY = r'''
static void test_managed_detach(void) {
    lookup_error = identity_error = 0; allocations = fail_allocation = 0;
    identity_root = "/module"; fs_type.name = "ext4"; sb.s_dev = 0x70013;
    assert(!ksu_register_managed_mount("/system/lib/deep"));
    assert(!ksu_register_managed_mount("/system/lib"));
    unsigned before_allocations = live_allocations;
    ksu_umount_mount_list();
    assert(detach_calls == 2 && !strcmp(detached[0], "/system/lib/deep") && !strcmp(detached[1], "/system/lib"));
    assert(!read_depth && !path_refs && live_allocations == before_allocations);
    // A replacement mount at the same path does not belong to SUMHP.
    detach_calls = 0; sb.s_dev++;
    ksu_umount_mount_list(); assert(!detach_calls); sb.s_dev--;
    identity_root = "/replacement";
    ksu_umount_mount_list(); assert(!detach_calls); identity_root = "/module";
    fs_type.name = "tmpfs";
    ksu_umount_mount_list(); assert(!detach_calls); fs_type.name = "ext4";
    not_mountpoint = true;
    ksu_umount_mount_list(); assert(!detach_calls); not_mountpoint = false;
    lookup_error = -ENOENT;
    ksu_umount_mount_list(); assert(!detach_calls); lookup_error = 0;
    identity_error = -ENOMEM;
    ksu_umount_mount_list(); assert(!detach_calls); identity_error = 0;
    // Allocation failures leave both the registry and mounts intact.
    for (unsigned fail = 1; fail <= 2; fail++) {
        allocations = 0; fail_allocation = fail;
        ksu_umount_mount_list();
        assert(!detach_calls && live_allocations == before_allocations && !read_depth && !path_refs);
    }
    fail_allocation = 0; detach_error = -EBUSY;
    ksu_umount_mount_list();
    assert(detach_calls == 2 && !path_refs && !read_depth);
    ksu_mount_policy_reset(); flush_rcu();
    assert(!live_allocations);
}
'''

BODY = r'''
static struct sumh_managed_mount_arg request;
static void request_path(const char *path, unsigned operation) {
    memset(&request, 0, sizeof(request)); request.size = sizeof(request);
    request.operation = operation; assert(strlen(path) < sizeof(request.path));
    strcpy(request.path, path); allocations = fail_allocation = 0;
}
static size_t entries(void) {
    size_t count = 0; struct mount_entry *entry;
    list_for_each_entry(entry, &mount_list, list) ++count;
    return count;
}
static struct mount_entry *first(void) {
    assert(entries()); return list_entry(mount_list.next, struct mount_entry, list);
}
static void reject(void) {
    u64 before = ksu_mount_policy_generation();
    unsigned before_live = live_allocations, before_lookups = lookups, before_notifies = notifications;
    assert(sumh_update_managed_mount(&request) == -EINVAL);
    assert(live_allocations == before_live && lookups == before_lookups);
    assert(ksu_mount_policy_generation() == before && notifications == before_notifies);
}
int main(void) {
    const char *invalid[] = {"", "/", "relative", "/trailing/", "//double", "/a//b",
                            "/.", "/..", "/a/./b", "/a/../b", "/a\rb", "/a\nb"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        request_path(invalid[i], SUMH_MANAGED_MOUNT_REGISTER); reject();
    }
    request_path("/system/lib", SUMH_MANAGED_MOUNT_REGISTER);
    memset(request.path, 'a', sizeof(request.path)); request.path[0] = '/'; reject();
    request_path("/system/lib", SUMH_MANAGED_MOUNT_REGISTER);
    request.path[100] = 'x'; reject();
    request_path("/system/lib", SUMH_MANAGED_MOUNT_REGISTER);
    request.size--; reject(); request.size++;
    request.reserved[0] = 1; reject(); request.reserved[0] = 0;
    request.reserved[1] = 1; reject(); request.reserved[1] = 0;
    request.operation = 0; reject(); request.operation = 3; reject();
    request.operation = SUMH_MANAGED_MOUNT_REGISTER;
    input_fault = true; assert(sumh_update_managed_mount(&request) == -EFAULT); input_fault = false;
    assert(!entries() && !live_allocations && !path_refs);

    // Every allocation boundary fails before publish and releases partial state.
    for (unsigned fault = 1; fault <= 6; ++fault) {
        request_path("/system/lib", SUMH_MANAGED_MOUNT_REGISTER); fail_allocation = fault;
        assert(sumh_update_managed_mount(&request) == -ENOMEM);
        assert(!entries() && !live_allocations && !path_refs && !lock_depth);
        assert(ksu_mount_policy_generation() == 2 && !notifications);
    }
    request_path("/system/lib", SUMH_MANAGED_MOUNT_REGISTER);
    lookup_error = -ENOENT; assert(sumh_update_managed_mount(&request) == -ENOENT); lookup_error = 0;
    not_mountpoint = true; assert(sumh_update_managed_mount(&request) == -EINVAL); not_mountpoint = false;
    identity_error = -ENAMETOOLONG; assert(sumh_update_managed_mount(&request) == -ENAMETOOLONG); identity_error = 0;
    mount.mnt_sb = NULL; assert(sumh_update_managed_mount(&request) == -EINVAL); mount.mnt_sb = &sb;
    sb.s_type = NULL; assert(sumh_update_managed_mount(&request) == -EINVAL); sb.s_type = &fs_type;
    fs_type.name = NULL; assert(sumh_update_managed_mount(&request) == -EINVAL); fs_type.name = "ext4";
    assert(!entries() && !live_allocations && !path_refs && ksu_mount_policy_generation() == 2);

    assert(!sumh_update_managed_mount(&request));
    assert(entries() == 1 && live_allocations == 4 && !path_refs && !lock_depth);
    assert(first()->flags == MNT_DETACH && first()->mount_dev == sb.s_dev);
    assert(!strcmp(first()->mount_root, "/module") && !strcmp(first()->mount_fstype, "ext4"));
    assert(ksu_mount_policy_generation() == 4 && notifications == 1);
    struct mount_entry *old = first();

    // Failed re-registration must leave the existing complete identity published.
    for (unsigned fault = 1; fault <= 6; ++fault) {
        allocations = 0; fail_allocation = fault;
        assert(sumh_update_managed_mount(&request) == -ENOMEM);
        assert(first() == old && live_allocations == 4 && !retired_count);
        assert(ksu_mount_policy_generation() == 4 && notifications == 1);
    }
    fail_allocation = 0; identity_root = "/replacement"; sb.s_dev = 0x80014;
    assert(!sumh_update_managed_mount(&request));
    assert(entries() == 1 && first() != old && first()->flags == MNT_DETACH);
    assert(!strcmp(first()->mount_root, "/replacement") && first()->mount_dev == 0x80014);
    assert(retired_count == 1 && live_allocations == 8 && !strcmp(old->mount_root, "/module"));
    assert(ksu_mount_policy_generation() == 6 && notifications == 2);
    flush_rcu(); assert(live_allocations == 4);

    // Complete names longer than the old 256-byte interface remain distinct.
    char long_path[SUMH_MANAGED_MOUNT_PATH_MAX];
    memset(long_path, 'a', sizeof(long_path)); long_path[0] = '/'; long_path[sizeof(long_path)-1] = 0;
    request_path(long_path, SUMH_MANAGED_MOUNT_REGISTER); assert(!sumh_update_managed_mount(&request));
    assert(entries() == 2 && strlen(first()->umountable) == sizeof(long_path)-1);
    assert(!strcmp(first()->umountable, long_path));
    request_path("/system/with spaces", SUMH_MANAGED_MOUNT_REGISTER); assert(!sumh_update_managed_mount(&request));
    assert(entries() == 3);

    // Unregistration is idempotent and never looks up an already detached path.
    unsigned before_lookups = lookups; u64 before = ksu_mount_policy_generation();
    lookup_error = -ENOENT;
    request_path("/system/lib", SUMH_MANAGED_MOUNT_UNREGISTER); assert(!sumh_update_managed_mount(&request));
    assert(entries() == 2 && lookups == before_lookups && ksu_mount_policy_generation() == before + 2);
    assert(!sumh_update_managed_mount(&request));
    assert(entries() == 2 && lookups == before_lookups && ksu_mount_policy_generation() == before + 2);
    flush_rcu(); assert(live_allocations == 8);
    ksu_mount_policy_reset(); assert(!entries() && retired_count == 2);
    flush_rcu(); assert(!live_allocations && !path_refs && !lock_depth && !read_depth);
    assert(!(ksu_mount_policy_generation() & 1));
    test_managed_detach();
    puts("PASS managed-mount payload, failure atomicity, identity refresh, RCU and detach lifecycle");
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
    # No legacy command may remain reachable through a handler or exported UAPI.
    for path in ("kernel/supercall/dispatch.c", "uapi/supercall.h"):
        text = (ROOT / path).read_text(encoding="utf-8")
        for removed in ("ADD_TRY_UMOUNT", "LIST_TRY_UMOUNT", "ksu_add_try_umount_cmd", "ksu_list_try_umount_cmd"):
            assert removed not in text, (path, removed)
    policy_functions = (
        "ksu_capture_mount_identity", "ksu_mount_policy_generation", "ksu_mount_policy_begin_update",
        "ksu_mount_policy_changed", "ksu_free_mount_entry", "ksu_mount_entry_release",
        "ksu_retire_mount_entry", "ksu_mount_policy_reset", "ksu_register_managed_mount",
        "ksu_unregister_managed_mount",
    )
    code = PRELUDE + "".join(function("kernel/infra/mount_policy.c", name) for name in policy_functions)
    code += function("kernel/sumh/control/sumh_ioctl.c", "sumh_managed_mount_path_valid")
    code += function("kernel/sumh/control/sumh_ioctl.c", "sumh_update_managed_mount")
    code += "".join(function("kernel/feature/kernel_umount.c", name) for name in (
        "ksu_umount_mnt", "ksu_fstype_matches", "ksu_try_umount_verified",
        "ksu_umount_target_cmp", "ksu_umount_mount_list"))
    code += DETACH_BODY + BODY
    with tempfile.TemporaryDirectory(prefix="sumh-managed-mounts-") as tmp:
        build = Path(tmp)
        if os.name == "nt":
            (build / "linux").mkdir()
            (build / "sys").mkdir()
            (build / "linux/types.h").write_text(
                "#pragma once\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef int32_t __s32;\n"
                "typedef int64_t __s64;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n")
            (build / "sys/ioctl.h").write_text(
                "#pragma once\n#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|"
                "((unsigned long)(t)<<8)|(n))\n#define _IO(t,n) _IOC(0,t,n,0)\n"
                "#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n"
                "#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n")
        src = build / "test.c"
        src.write_text(code, encoding="utf-8")
        binary = build / ("test.exe" if os.name == "nt" else "test")
        sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run([*compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        *sanitize, "-I" + str(build), "-I" + str(ROOT), str(src), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
