"""Run isolated SUMH VFS helper regressions against the actual C functions.

The small kernel API doubles make these runnable without a Linux kernel tree.
Use --cc with a native C compiler, or an NDK clang on Windows (no CRT needed).
These tests do not replace a kernel build or concurrent on-device validation.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def function(path, marker):
    text = (ROOT / path).read_text(encoding="utf-8")
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


COMMON = r"""
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ssize_t;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void *)0)
#define SUMH_NOCFI
/* Process exit status is only 8 bits on POSIX. Keep every failure in 1..255. */
#define CHECK(test) do { if (!(test)) return ((__LINE__ - 1) % 255) + 1; } while (0)
void *memset(void *dst, int value, size_t n) {
    char *d = dst; for (size_t i = 0; i < n; i++) d[i] = value; return dst;
}
void *memcpy(void *dst, const void *src, size_t n) {
    char *d = dst; const char *s = src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
static void *memmove(void *dst, const void *src, size_t n) {
    char *d = dst; const char *s = src;
    if (d < s) for (size_t i = 0; i < n; i++) d[i] = s[i];
    else for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    return dst;
}
static size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static size_t strnlen(const char *s, size_t max) {
    size_t n = 0; while (n < max && s[n]) n++; return n;
}
static int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) break;
    }
    return 0;
}
static int strcmp(const char *a, const char *b) {
    return strncmp(a, b, strlen(a) + 1);
}
"""


def xattr_policy_source():
    preamble = r"""
typedef unsigned int uid_t;
#define PER_USER_RANGE 100000
#define FIRST_APPLICATION_UID 10000
#define LAST_APPLICATION_UID 19999
#define FIRST_ISOLATED_UID 99000
#define LAST_ISOLATED_UID 99999
#define READ_ONCE(v) (v)
#define smp_load_acquire(p) (*(p))
#define __kuid_val(v) (v)
#define task_uid(t) ((t)->uid)
#define ARRAY_SIZE(v) (sizeof(v) / sizeof((v)[0]))
enum sumh_policy_scope { SUMH_POLICY_SCOPE_NONE, SUMH_POLICY_SCOPE_VIEW, SUMH_POLICY_SCOPE_SPOOF };
struct task_struct { uid_t uid; } task;
static struct task_struct *current = &task;
struct super_block { int unused; };
struct dentry { struct super_block *d_sb; };
struct sumh_xattr_sb_entry { struct super_block *sb; };
static struct sumh_xattr_sb_entry registered;
static int sumh_enabled = 1, sumh_xattr_ready = 1;
static int allowed, umount_app, resolving, rcu_depth;
#define hash_for_each_possible_rcu(table, entry, node, key) \
    for ((entry) = &registered; (entry); (entry) = NULL)
static void rcu_read_lock(void) { rcu_depth++; }
static void rcu_read_unlock(void) { rcu_depth--; }
static bool sumh_hide_rules_resolving(void) { return resolving; }
static bool ksu_uid_should_umount(uid_t uid) { (void)uid; return umount_app; }
static bool ksu_is_allow_uid(uid_t uid) { (void)uid; return allowed; }
"""
    policy = "\n".join(
        function("kernel/policy/allowlist.h", marker)
        for marker in ("static inline bool is_appuid", "static inline bool is_isolated_process")
    )
    policy += "\n".join(
        function("kernel/sumh/policy/sumh_path_policy.c", marker)
        for marker in ("static enum sumh_policy_scope sumh_policy_scope_for_uid",
                       "enum sumh_policy_scope sumh_policy_current_scope",
                       "bool sumh_policy_current_is_spoof_target",
                       "bool sumh_policy_current_is_isolated",
                       "bool sumh_policy_current_is_mount_view_target")
    )
    target = function("kernel/sumh/features/sumh_vfs_view.c", "static bool sumh_xattr_target")
    body = r"""
int main(void) {
    struct super_block overlay, ordinary;
    struct dentry mounted = { &overlay }, unregistered = { &ordinary }, invalid = { NULL };
    const uid_t retained[] = { 10000, 12345, 19999, 110000, 212345 };
    const uid_t isolated[] = { 90000, 98999, 99000, 99999, 190000, 299999 };
    const uid_t excluded[] = { 0, 1000, 2000, 9999, 20000, 89999, 100000, 102000 };

    registered.sb = &overlay;
    for (size_t i = 0; i < ARRAY_SIZE(retained); i++) {
        task.uid = retained[i];
        umount_app = 0;
        CHECK(sumh_policy_current_scope() == SUMH_POLICY_SCOPE_VIEW);
        CHECK(sumh_xattr_target(&mounted));
        CHECK(!sumh_xattr_target(&unregistered));
        umount_app = 1;
        CHECK(sumh_policy_current_scope() == SUMH_POLICY_SCOPE_SPOOF);
        CHECK(sumh_xattr_target(&mounted));
        allowed = 1;
        CHECK(!sumh_xattr_target(&mounted));
        umount_app = 0;
        CHECK(!sumh_xattr_target(&mounted));
        allowed = 0;
    }
    for (size_t i = 0; i < ARRAY_SIZE(isolated); i++) {
        task.uid = isolated[i];
        CHECK(sumh_xattr_target(&mounted));
    }
    for (size_t i = 0; i < ARRAY_SIZE(excluded); i++) {
        task.uid = excluded[i];
        CHECK(!sumh_xattr_target(&mounted));
    }
    task.uid = retained[0]; umount_app = 0;
    CHECK(!sumh_xattr_target(NULL) && !sumh_xattr_target(&invalid));
    sumh_enabled = 0;
    CHECK(!sumh_xattr_target(&mounted));
    sumh_enabled = 1; resolving = 1;
    CHECK(!sumh_xattr_target(&mounted));
    resolving = 0; sumh_xattr_ready = 0;
    CHECK(!sumh_xattr_target(&mounted));
    CHECK(!rcu_depth);
    return 0;
}
"""
    return COMMON + preamble + policy + target + body


def xattr_source():
    preamble = r"""
#define EIO 5
#define ENOMEM 12
#define EFAULT 14
#define ERANGE 34
#define EOVERFLOW 75
#define GFP_KERNEL 0
#define XATTR_LIST_MAX 65536
struct dentry { int unused; };
struct sumh_view_guard { int unused; };
static char input[XATTR_LIST_MAX], allocated[XATTR_LIST_MAX];
static size_t input_len;
static int allocs, frees, reads, active, atomic_context, irq_context;
static int alloc_fail, native_error, invalid_count, wrong_free;
static int in_atomic(void) { return atomic_context; }
static int irqs_disabled(void) { return irq_context; }
static void sumh_view_enter(struct sumh_view_guard *g) { (void)g; active++; }
static void sumh_view_leave(struct sumh_view_guard *g) { (void)g; active--; }
static void *kvmalloc(size_t n, int flags) {
    (void)n; (void)flags; allocs++; return alloc_fail ? NULL : allocated;
}
static void kvfree(void *p) { frees++; if (p != allocated) wrong_free++; }
static ssize_t sumh_view_listxattr_orig(struct dentry *d, char *out, size_t n) {
    (void)d; reads++;
    if (native_error) return native_error;
    if (invalid_count) return n + 1;
    if (!n) return input_len;
    if (input_len > n) return -ERANGE;
    if (out) memcpy(out, input, input_len);
    return input_len;
}
static void reset(const char *data, size_t length) {
    if (data) memcpy(input, data, length);
    input_len = length;
    allocs = frees = reads = active = atomic_context = irq_context = 0;
    alloc_fail = native_error = invalid_count = wrong_free = 0;
}
"""
    body = r"""
int main(void) {
    static const char mixed[] = "security.selinux\0trusted.overlay.origin\0user.keep\0user.overlay.test\0";
    static const char kept[] = "security.selinux\0user.keep\0";
    char output[256];
    struct dentry d;
    reset(mixed, sizeof(mixed) - 1);
    CHECK(sumh_view_listxattr(&d, output, sizeof(output)) == sizeof(kept) - 1);
    CHECK(!strncmp(output, "security.selinux", 17));
    CHECK(!strncmp(output + 17, "user.keep", 10));
    CHECK(allocs == 0 && reads == 1 && active == 0 && frees == 0);
    CHECK(sumh_view_listxattr(&d, NULL, 0) == sizeof(kept) - 1);
    CHECK(allocs == 0 && active == 0);
    CHECK(sumh_view_listxattr(&d, output, 1) == -ERANGE);
    CHECK(sumh_view_listxattr(&d, NULL, 100) == -EFAULT);
    reset("trusted.overlay.origin\0", 23);
    CHECK(sumh_view_listxattr(&d, NULL, 1) == 0);
    reset(NULL, 0);
    CHECK(sumh_view_listxattr(&d, output, sizeof(output)) == 0 && allocs == 0);
    reset(NULL, 400);
    for (size_t i = 0; i < 400; i += 10) memcpy(input + i, "user.keep\0", 10);
    CHECK(sumh_view_listxattr(&d, NULL, 0) == 400);
    CHECK(allocs == 1 && frees == 1 && reads == 2 && active == 0 && !wrong_free);
    reset(NULL, 400); alloc_fail = 1;
    CHECK(sumh_view_listxattr(&d, NULL, 0) == -ENOMEM && active == 0 && !frees);
    reset(NULL, 0); native_error = -EIO;
    CHECK(sumh_view_listxattr(&d, NULL, 0) == -EIO && active == 0 && !allocs);
    reset("bad", 3);
    CHECK(sumh_view_listxattr(&d, output, sizeof(output)) == -EIO);
    reset(NULL, 0); invalid_count = 1;
    CHECK(sumh_view_listxattr(&d, NULL, 0) == -EOVERFLOW && active == 0);
    reset(mixed, sizeof(mixed) - 1); atomic_context = 1;
    CHECK(sumh_view_listxattr(&d, NULL, 0) == sizeof(mixed) - 1 && !allocs);
    CHECK(active == 0 && !wrong_free);
    return 0;
}
"""
    helpers = "\n".join(
        function("kernel/sumh/features/sumh_xattr_filter.h", marker)
        for marker in ("static inline bool sumh_overlay_name", "static inline ssize_t sumh_xattr_filter_list")
    )
    wrapper = function("kernel/sumh/features/sumh_vfs_view.c", "static SUMH_NOCFI ssize_t sumh_view_listxattr")
    return COMMON + preamble + helpers + wrapper + body


def statfs_source():
    preamble = r"""
#define EIO 5
#define ENOMEM 12
#define EAGAIN 11
#define ELOOP 40
#define SEQ_SKIP 1
#define GFP_KERNEL 0
#define SUMH_VIEW_LINE_MAX 128
#define SUMH_VIEW_MAX_DEPTH 8
#define CONFIG_ARM64
#define PSR_I_BIT 128
#define SUMH_FEATURE_STATFS_SPOOF 1
#define SUMH_FEATURE_MOUNT_HIDE 2
#define READ_ONCE(v) (v)
struct sumh_view_guard { int unused; };
struct mount { void *mnt_ns; struct mount *parent; bool hidden; int refs; };
struct path { struct mount *mnt; void *dentry; };
struct fs_struct { struct path root; };
struct nsproxy { void *mnt_ns; };
struct task_struct { struct fs_struct *fs; struct nsproxy *nsproxy; void *mm; };
struct kstatfs {
    long f_type, f_bsize;
    unsigned long long f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    struct { int val[2]; } f_fsid;
    long f_namelen, f_frsize, f_flags, f_spare[4];
};
struct file { void *f_cred; };
struct seq_file { struct file *file; size_t size; void *buf, *private; };
struct proc_mounts { void *ns; struct path root; };
static struct { int statfs_spoofs, statfs_entries; } sumh_hook_stats;
static struct task_struct task, *current = &task;
static struct fs_struct fs;
static struct nsproxy ns;
static struct mount visible, hidden, nested;
static struct kstatfs backing_data, mounted_data;
static int *sumh_namespace_sem, lock_depth, active, reads, allocs, frees;
static int alloc_fail, lock_fail, native_error, backing_error, classify_error;
static int atomic_context, irq_context, bad_lifetime;
struct kprobe { int unused; };
struct pt_regs { __UINTPTR_TYPE__ pstate, regs[3], pc; };
struct dentry { int unused; };
static struct kprobe sumh_view_statfs_probe, sumh_view_list_probe;
static int sumh_view_stopped, sumh_statfs_ready = 1, sumh_view_active;
static int sumh_feature_enabled_mask, mount_view_target = 1;
static bool sumh_view_guarded(void) { return false; }
static bool sumh_policy_current_is_mount_view_target(void) { return mount_view_target; }
/* A retained app keeps VIEW content scope, so the old gate must reject it. */
static bool sumh_policy_current_is_spoof_target(void) { return false; }
static bool sumh_xattr_target(struct dentry *d) { (void)d; return false; }
static bool sumh_overlay_name(const char *s) { (void)s; return false; }
static void sumh_view_listxattr(void) {}
static void sumh_view_getxattr(void) {}
static void atomic_inc(int *value) { (*value)++; }
static void instruction_pointer_set(struct pt_regs *r, __UINTPTR_TYPE__ pc) { r->pc = pc; }
static char allocation[SUMH_VIEW_LINE_MAX];
static void *current_cred(void) { return NULL; }
static int in_atomic(void) { return atomic_context; }
static int irqs_disabled(void) { return irq_context; }
static void sumh_view_enter(struct sumh_view_guard *g) { (void)g; active++; }
static void sumh_view_leave(struct sumh_view_guard *g) { (void)g; active--; }
static void atomic64_inc(int *v) { (*v)++; }
static struct mount *real_mount(struct mount *m) { return m; }
static void path_get(struct path *p) { p->mnt->refs++; }
static void path_put(struct path *p) {
    if (--p->mnt->refs < 1) bad_lifetime++;
}
static void get_fs_root(struct fs_struct *f, struct path *p) {
    *p = f->root; path_get(p);
}
static void *kvmalloc(size_t n, int flags) {
    (void)n; (void)flags; allocs++; return alloc_fail ? NULL : allocation;
}
static void kvfree(void *p) { if (p != allocation) bad_lifetime++; frees++; }
static bool down_read_trylock(int *sem) {
    (void)sem; if (lock_fail) return false; lock_depth++; return true;
}
static void up_read(int *sem) { (void)sem; lock_depth--; }
static int sumh_view_mount_hidden(struct seq_file *s, const struct path *p,
                                  const struct path *r, bool *is_hidden) {
    (void)s; (void)r;
    if (lock_depth != 1 || p->mnt->refs < 2) bad_lifetime++;
    *is_hidden = p->mnt->hidden;
    return classify_error;
}
static bool sumh_view_follow_up(struct path *p) {
    if (lock_depth != 1) bad_lifetime++;
    if (!p->mnt->parent) return false;
    struct mount *parent = p->mnt->parent;
    path_put(p); p->mnt = parent; p->dentry = parent; path_get(p);
    return true;
}
static int sumh_view_statfs_orig(const struct path *p, struct kstatfs *out) {
    reads++;
    if (active != 1 || lock_depth) bad_lifetime++;
    if (reads == 1 && native_error) return native_error;
    if (reads > 1 && backing_error) return backing_error;
    *out = p->mnt == &visible ? backing_data : mounted_data;
    return 0;
}
static void reset(void) {
    ns.mnt_ns = &ns;
    visible = (struct mount){ &ns, NULL, false, 1 };
    hidden = (struct mount){ &ns, &visible, true, 1 };
    nested = (struct mount){ &ns, &hidden, true, 1 };
    fs.root = (struct path){ &visible, &visible };
    task = (struct task_struct){ &fs, &ns, &task };
    backing_data = (struct kstatfs){ 0xe0f5e1e2, 4096, 100000, 1000, 999,
        5000, 4000, {{17, 42}}, 255, 4096, 1, {0, 0, 0, 0} };
    mounted_data = (struct kstatfs){ 0x01021994, 1024, 20000, 19999, 19999,
        1000, 999, {{63, 71}}, 127, 1024, 0, {1, 2, 3, 4} };
    lock_depth = active = reads = allocs = frees = bad_lifetime = 0;
    alloc_fail = lock_fail = native_error = backing_error = classify_error = 0;
    atomic_context = irq_context = sumh_hook_stats.statfs_spoofs = 0;
}
static bool same_statfs(const struct kstatfs *a, const struct kstatfs *b) {
    if (a->f_type != b->f_type || a->f_bsize != b->f_bsize ||
        a->f_blocks != b->f_blocks || a->f_bfree != b->f_bfree ||
        a->f_bavail != b->f_bavail || a->f_files != b->f_files ||
        a->f_ffree != b->f_ffree || a->f_fsid.val[0] != b->f_fsid.val[0] ||
        a->f_fsid.val[1] != b->f_fsid.val[1] || a->f_namelen != b->f_namelen ||
        a->f_frsize != b->f_frsize || a->f_flags != b->f_flags) return false;
    for (int i = 0; i < 4; i++) if (a->f_spare[i] != b->f_spare[i]) return false;
    return true;
}
static bool clean(void) {
    return !active && !lock_depth && !bad_lifetime &&
        visible.refs == 1 && hidden.refs == 1 && nested.refs == 1;
}
"""
    body = r"""
int main(void) {
    struct path path;
    struct kstatfs output;
    struct pt_regs regs = {0};

    /* Retaining modules must still dispatch the statfs view. Both feature
     * switches are required; the module-content SPOOF scope is not. */
    reset();
    sumh_feature_enabled_mask = SUMH_FEATURE_STATFS_SPOOF | SUMH_FEATURE_MOUNT_HIDE;
    CHECK(sumh_view_pre(&sumh_view_statfs_probe, &regs) == 1);
    CHECK(regs.pc == (__UINTPTR_TYPE__)sumh_view_statfs && sumh_view_active == 1);
    CHECK(sumh_hook_stats.statfs_entries == 1 && reads == 0);
    sumh_view_active = 0; regs.pc = 0;
    mount_view_target = 0;
    CHECK(sumh_view_pre(&sumh_view_statfs_probe, &regs) == 0 && !regs.pc);
    mount_view_target = 1;
    for (int flags = 0; flags < 3; flags++) {
        sumh_feature_enabled_mask = flags;
        CHECK(sumh_view_pre(&sumh_view_statfs_probe, &regs) == 0);
    }
    sumh_feature_enabled_mask = SUMH_FEATURE_STATFS_SPOOF | SUMH_FEATURE_MOUNT_HIDE;
    regs.pstate = PSR_I_BIT;
    CHECK(sumh_view_pre(&sumh_view_statfs_probe, &regs) == 0);
    regs.pstate = 0; sumh_statfs_ready = 0;
    CHECK(sumh_view_pre(&sumh_view_statfs_probe, &regs) == 0);
    sumh_statfs_ready = 1;
    CHECK(!sumh_view_active && !regs.pc);

    /* Mounted files remain in place, but statfs exposes the whole backing
     * partition identity, including read-only flags and capacity. */
    reset(); path = (struct path){ &hidden, &hidden };
    CHECK(sumh_view_statfs(&path, &output) == 0);
    CHECK(same_statfs(&output, &backing_data));
    CHECK(path.mnt == &hidden && path.dentry == &hidden);
    CHECK(reads == 2 && sumh_hook_stats.statfs_spoofs == 1 && clean());
    CHECK(allocs == 1 && frees == 1);

    /* A bind inside a tmpfs skeleton must cross both hidden mount layers. */
    reset(); path = (struct path){ &nested, &nested };
    CHECK(sumh_view_statfs(&path, &output) == 0);
    CHECK(same_statfs(&output, &backing_data) && reads == 2 && clean());

    reset(); path = (struct path){ &visible, &visible };
    CHECK(sumh_view_statfs(&path, &output) == 0);
    CHECK(same_statfs(&output, &backing_data));
    CHECK(reads == 1 && !sumh_hook_stats.statfs_spoofs && clean());

    /* Permission / filesystem failures must not become synthetic successes. */
    reset(); path = (struct path){ &hidden, &hidden }; native_error = -EIO;
    CHECK(sumh_view_statfs(&path, &output) == -EIO);
    CHECK(!allocs && reads == 1 && clean());
    reset(); backing_error = -EIO;
    CHECK(sumh_view_statfs(&path, &output) == -EIO);
    CHECK(!sumh_hook_stats.statfs_spoofs && same_statfs(&output, &mounted_data));
    CHECK(reads == 2 && allocs == frees && clean());

    reset(); alloc_fail = 1;
    CHECK(sumh_view_statfs(&path, &output) == -ENOMEM && !frees && clean());
    reset(); lock_fail = 1;
    CHECK(sumh_view_statfs(&path, &output) == -EAGAIN && allocs == frees && clean());
    reset(); classify_error = -EAGAIN;
    CHECK(sumh_view_statfs(&path, &output) == -EAGAIN && allocs == frees && clean());

    /* An fd outside the caller's mount namespace has no safe projection. */
    reset(); hidden.mnt_ns = NULL;
    CHECK(sumh_view_statfs(&path, &output) == 0);
    CHECK(same_statfs(&output, &mounted_data) && reads == 1 && clean());
    reset(); atomic_context = 1;
    CHECK(sumh_view_statfs(&path, &output) == 0 && !allocs && clean());
    CHECK(same_statfs(&output, &mounted_data));
    return 0;
}
"""
    wrapper = function("kernel/sumh/features/sumh_vfs_view.c", "static SUMH_NOCFI int sumh_view_statfs(")
    ingress = function("kernel/sumh/features/sumh_vfs_view.c", "static int sumh_view_pre(")
    # Windows uses LLP64; model ARM64's pointer-width unsigned long for registers.
    ingress = ingress.replace("unsigned long", "__UINTPTR_TYPE__")
    return COMMON + preamble + wrapper + ingress + body


def directory_limits():
    """Use production limits; PATH_MAX is Linux's byte ABI, not host MAX_PATH."""
    text = (ROOT / "kernel/sumh/internal/sumh_base.h").read_text(encoding="utf-8")
    return "#define PATH_MAX 4096\n" + "\n".join(
        re.search(rf"^#define {name} .+$", text, re.MULTILINE).group(0)
        for name in ("SUMH_ITERATE_PATH_BUF",)
    ) + "\n"


def directory_source():
    preamble = r"""
typedef long long loff_t;
typedef unsigned int u32;
#define ENOENT 2
#define ENOMEM 12
#define EIO 5
#define ENAMETOOLONG 36
#define GFP_ATOMIC 0
#define GFP_KERNEL 1
#define SUMH_HASH_BITS 4
#define SUMH_MAGIC_POS 0x1000000000000000ULL
#define AS_FLAGS_SUMH_DIR_HAS_HIDDEN 0
#define AS_FLAGS_SUMH_DIR_HAS_INJECT 1
#define READ_ONCE(v) (v)
#define ERR_PTR(v) ((void *)(ssize_t)(v))
#define IS_ERR(v) ((ssize_t)(v) < 0)
#define IS_ERR_OR_NULL(v) (!(v) || IS_ERR(v))
#define PTR_ERR(v) ((ssize_t)(v))
enum sumh_policy_scope { SUMH_POLICY_SCOPE_NONE, SUMH_POLICY_SCOPE_VIEW, SUMH_POLICY_SCOPE_SPOOF };
struct mapping { unsigned long flags; };
struct inode { struct mapping *i_mapping; };
struct qstr { char *name; };
struct dentry { struct inode *inode; struct qstr d_name; int refs; };
struct path { struct dentry *dentry; const char *name; };
struct file { struct path f_path; };
struct dir_context { bool (*actor)(void); loff_t pos; };
PRODUCTION_WRAPPER
struct sumh_inject_entry { const char *dir; };
struct sumh_merge_entry { const char *src, *resolved_src; struct dentry *target_dentry; };
static struct sumh_merge_entry merges[1];
#define merge merges[0]
static int merge_count = 1;
static int sumh_ioctl_tgid, sumh_rule_count = 1, sumh_enabled = 1;
static int sumh_stealth_enabled, sumh_filldir_cache, current;
static int rcu_depth, bad_dget, freed, allocated_count, allocation_attempts;
static int policy_scope = SUMH_POLICY_SCOPE_VIEW, allow_hide, hide_checks;
static int allocation_fails, path_calls, path_fails, fallback_calls, malformed;
static int path_allocation_fails, path_allocations, path_frees, path_attempts, bad_context;
static int wrapper_live[4], path_live[4];
static unsigned char path_storage[4][PATH_MAX + 2];
static struct sumh_filldir_wrapper allocated[4], *nested_wrapper;
static struct file *nested_file;
static struct dir_context *nested_ctx;
static int nest;
#define atomic_long_read(v) (*(v))
#define atomic_read(v) (*(v))
#define task_tgid_vnr(v) (17)
#define test_bit(bit, flags) ((*(flags) >> (bit)) & 1)
#define hash_min(hash, bits) (0)
#define full_name_hash(p, name, len) ((u32)(len))
#define hlist_for_each_entry_rcu(ie, table, node) for ((ie) = NULL; (ie); (ie) = NULL)
#define hash_for_each_rcu(table, bucket, me, node) \
    for ((bucket) = 0; (bucket) < merge_count && ((me) = &merges[bucket], 1); (bucket)++)
static void rcu_read_lock(void) { rcu_depth++; }
static void rcu_read_unlock(void) { rcu_depth--; }
static struct dentry *dget(struct dentry *d) { if (!rcu_depth) bad_dget++; d->refs++; return d; }
static void dput(struct dentry *d) { d->refs--; }
struct sumh_merge_snapshot { struct dentry *target; };
static struct sumh_merge_snapshot snapshots[4];
static int snapshot_live[4], snapshot_calls, snapshot_error;
static int sumh_merge_snapshot_create(struct file *file, const char *primary, struct sumh_merge_snapshot **out) {
    (void)file; snapshot_calls++; *out = NULL;
    if (rcu_depth) bad_context++;
    if (snapshot_error) return -snapshot_error;
    for (int i = 0; i < 4; i++) if (!snapshot_live[i]) {
        snapshot_live[i] = 1; snapshots[i].target = NULL;
        if (!strcmp(primary, merge.src)) { rcu_depth++; snapshots[i].target = dget(merge.target_dentry); rcu_depth--; }
        *out = &snapshots[i]; return 0;
    }
    bad_context++; return -ENOMEM;
}
static void sumh_merge_snapshot_destroy(struct sumh_merge_snapshot *s) {
    if (!s) return;
    int index = (int)(s - snapshots);
    if (index < 0 || index >= 4 || !snapshot_live[index] || rcu_depth) { bad_context++; return; }
    if (s->target) dput(s->target);
    snapshot_live[index] = 0;
}
static struct inode *d_inode(struct dentry *d) { return d->inode; }
static int sumh_policy_current_scope(void) { return policy_scope; }
static bool sumh_policy_current_is_hide_target(struct inode *i) { (void)i; hide_checks++; return allow_hide; }
static bool sumh_filldir_filter(void) { return true; }
static bool sumh_merge_filldir(void) { return true; }
static bool ordinary_actor(void) { return true; }
static void *kmem_cache_zalloc(int cache, int flags) {
    (void)cache;
    if (rcu_depth || flags != GFP_KERNEL) bad_context++;
    allocation_attempts++;
    if (allocation_fails) return NULL;
    for (int i = 0; i < 4; i++) if (!wrapper_live[i]) {
        wrapper_live[i] = 1; allocated_count++;
        memset(&allocated[i], 0, sizeof(allocated[i])); return &allocated[i];
    }
    bad_context++; return NULL;
}
static void kmem_cache_free(int cache, void *ptr) {
    (void)cache;
    for (int i = 0; i < 4; i++) if (ptr == &allocated[i]) {
        if (!wrapper_live[i] || rcu_depth) bad_context++;
        wrapper_live[i] = 0; freed++; return;
    }
    bad_context++;
}
static void *kmalloc(size_t size, int flags) {
    path_attempts++;
    if (rcu_depth || flags != GFP_KERNEL || size != PATH_MAX) bad_context++;
    if (path_allocation_fails) return NULL;
    for (int i = 0; i < 4; i++) if (!path_live[i]) {
        path_live[i] = 1; path_allocations++;
        memset(path_storage[i], 0x5a, sizeof(path_storage[i])); return path_storage[i] + 1;
    }
    bad_context++; return NULL;
}
static void kfree(void *ptr) {
    if (!ptr) return;
    for (int i = 0; i < 4; i++) if (ptr == path_storage[i] + 1) {
        if (!path_live[i] || rcu_depth || path_storage[i][0] != 0x5a ||
            path_storage[i][PATH_MAX + 1] != 0x5a) bad_context++;
        path_live[i] = 0; path_frees++; return;
    }
    bad_context++;
}
struct sumh_filldir_wrapper *sumh_iterate_prepare_wrapper(struct file *, struct dir_context *);
void sumh_iterate_finish_wrapper(struct sumh_filldir_wrapper *);
static char *absolute_path(const struct path *path, char *buffer, size_t size) {
    path_calls++;
    if (size != PATH_MAX || rcu_depth) bad_context++;
    if (path_fails) return ERR_PTR(-path_fails);
    if (malformed == 1) return NULL;
    if (malformed == 2) return buffer - 1;
    if (malformed == 3) return buffer + size;
    if (malformed == 4) { memcpy(buffer, "relative", 9); return buffer; }
    if (malformed == 5) { memset(buffer, '/', size); return buffer; }
    if (malformed == 6) { buffer[size - 1] = '/'; return buffer + size - 1; }
    size_t len = strlen(path->name);
    if (len >= size) return ERR_PTR(-ENAMETOOLONG);
    char *out = buffer + size - len - 1;
    memcpy(out, path->name, len + 1);
    if (nest) {
        nest = 0;
        nested_wrapper = sumh_iterate_prepare_wrapper(nested_file, nested_ctx);
    }
    return out;
}
static char *(*sumh_d_absolute_path)(const struct path *, char *, size_t) = absolute_path;
static char *d_path(const struct path *path, char *buffer, int size) {
    fallback_calls++; return absolute_path(path, buffer, (size_t)size);
}
static void make_path(char *out, int bytes, int encoding) {
    out[0] = '/';
    for (int i = 1; i < bytes; i++) out[i] = i % 32 == 0 && i + 1 < bytes ? '/' : 'a';
    if (encoding == 1) { out[1] = (char)0xe7; out[2] = (char)0x95; out[3] = (char)0x8c; }
    if (encoding == 2) { out[1] = (char)0x80; out[2] = (char)0xff; }
    out[bytes] = 0;
}
"""
    body = r"""
int main(void) {
    struct mapping mapping = { 0 };
    struct inode inode = { &mapping };
    struct dentry parent = { &inode, { "system" }, 1 };
    struct dentry target = { &inode, { "module" }, 1 };
    struct file first = { { &parent, "/system" } }, second = { { &parent, "/vendor" } };
    struct dir_context ctx = { ordinary_actor, 0 }, second_ctx = { ordinary_actor, 0 };
    struct dir_context internal = { sumh_merge_filldir, 0 };
    struct dir_context filtered = { sumh_filldir_filter, 0 };
    struct dir_context missing_actor = { NULL, 0 };
    struct sumh_filldir_wrapper *w;
    merge = (struct sumh_merge_entry){ "/system", NULL, &target };
    CHECK(sumh_is_merge_context(&internal));
    CHECK(!sumh_is_merge_context(&ctx) && !sumh_is_merge_context(NULL));
    CHECK(!sumh_iterate_prepare_wrapper(&first, &internal) && allocated_count == 0);
    CHECK(!sumh_iterate_prepare_wrapper(&first, &filtered));
    CHECK(!sumh_iterate_prepare_wrapper(&first, &missing_actor));
    CHECK(!sumh_iterate_prepare_wrapper(&first, NULL));
    CHECK(!sumh_iterate_prepare_wrapper(NULL, &ctx));
    CHECK(!sumh_iterate_prepare_wrapper(&first, &ctx));
    CHECK(!allocation_attempts && !freed && !path_calls && !hide_checks);
    CHECK(ctx.pos == 0 && target.refs == 1 && !rcu_depth);

    /* A hidden flag still needs the per-process hide policy. */
    mapping.flags = 1;
    CHECK(!sumh_iterate_prepare_wrapper(&first, &ctx));
    CHECK(!allocation_attempts && hide_checks == 1);
    allow_hide = 1;
    w = sumh_iterate_prepare_wrapper(&first, &ctx);
    CHECK(w && w->dir_has_hidden && !w->dir_has_inject);
    CHECK(w->view_allowed && !w->spoof_allowed && !path_calls && !path_attempts);
    sumh_iterate_finish_wrapper(w);

    /* Injection is restricted to the view scope, even with the flag set. */
    mapping.flags = 2;
    policy_scope = SUMH_POLICY_SCOPE_SPOOF;
    CHECK(!sumh_iterate_prepare_wrapper(&first, &ctx));
    CHECK(allocation_attempts == 1 && freed == 1);
    mapping.flags = 1;
    w = sumh_iterate_prepare_wrapper(&first, &ctx);
    CHECK(w && w->dir_has_hidden && w->spoof_allowed && !w->view_allowed);
    sumh_iterate_finish_wrapper(w);

    /* Preserve the stealth /dev exception and its scope restriction. */
    mapping.flags = 0;
    parent.d_name.name = "dev";
    CHECK(!sumh_iterate_prepare_wrapper(&first, &ctx));
    sumh_stealth_enabled = 1;
    w = sumh_iterate_prepare_wrapper(&first, &ctx);
    CHECK(w && w->spoof_allowed && w->dir_path_len == 4);
    CHECK(!w->dir_has_hidden && !w->dir_has_inject && !path_calls);
    sumh_iterate_finish_wrapper(w);
    parent.d_name.name = "device";
    CHECK(!sumh_iterate_prepare_wrapper(&first, &ctx));
    parent.d_name.name = "dev";
    policy_scope = SUMH_POLICY_SCOPE_VIEW;
    CHECK(!sumh_iterate_prepare_wrapper(&first, &ctx));
    CHECK(allocation_attempts == 3 && allocated_count == 3 && freed == 3);

    parent.d_name.name = "system";
    sumh_stealth_enabled = 0;
    mapping.flags = 3;
    policy_scope = SUMH_POLICY_SCOPE_NONE;
    CHECK(!sumh_iterate_prepare_wrapper(&first, &ctx));
    policy_scope = SUMH_POLICY_SCOPE_VIEW;
    sumh_enabled = 0;
    CHECK(!sumh_iterate_prepare_wrapper(&first, &ctx));
    sumh_enabled = 1;
    sumh_ioctl_tgid = 17;
    CHECK(!sumh_iterate_prepare_wrapper(&first, &ctx));
    sumh_ioctl_tgid = 0;
    CHECK(allocation_attempts == 3 && !path_calls);
    allocation_fails = 1;
    CHECK(sumh_iterate_prepare_wrapper(&first, &ctx) == ERR_PTR(-ENOMEM));
    CHECK(allocation_attempts == 4 && allocated_count == 3 && !path_calls);
    allocation_fails = 0;
    mapping.flags = 2;
    allocated_count = allocation_attempts = freed = 0;

    nested_file = &second; nested_ctx = &second_ctx; nest = 1;
    w = sumh_iterate_prepare_wrapper(&first, &ctx);
    CHECK(w && nested_wrapper);
    CHECK(!strcmp(w->dir_path, "/system"));
    CHECK(!strcmp(nested_wrapper->dir_path, "/vendor"));
    CHECK(w->merge_snapshot && target.refs == 2 && !bad_dget);
    CHECK(rcu_depth == 0);
    dput(&target); /* Simulate CLEAR releasing the rule after an RCU grace period. */
    CHECK(target.refs == 1);
    w->wrap_ctx.pos = 42;
    sumh_iterate_finish_wrapper(w);
    CHECK(target.refs == 0 && ctx.pos == 42 && freed == 1);
    sumh_iterate_finish_wrapper(nested_wrapper);
    CHECK(freed == 2);
    sumh_iterate_finish_wrapper(NULL);
    CHECK(freed == 2);

    /* A required view must not silently omit its path on resolve failure. */
    path_fails = ENOENT;
    CHECK(sumh_iterate_prepare_wrapper(&first, &ctx) == ERR_PTR(-ENOENT));
    CHECK(ctx.pos == 42 && !fallback_calls);
    path_fails = 0;
    sumh_rule_count = 0;
    int previous_path_calls = path_calls;
    w = sumh_iterate_prepare_wrapper(&first, &ctx);
    CHECK(w && w->dir_has_inject && path_calls == previous_path_calls);
    sumh_iterate_finish_wrapper(w);
    CHECK(allocated_count == freed && path_allocations == path_frees && !rcu_depth);

    /* Production PATH_MAX handles both sides of the old512-byte boundary.
     * Components stay below NAME_MAX; opaque kernel bytes remain untouched. */
    static char long_path[PATH_MAX + 1];
    const int lengths[] = { 255, 256, 510, 511, 512, 513, 4094, 4095 };
    sumh_rule_count = 1; target.refs = 1; ctx.pos = 0;
    first.f_path.name = long_path;
    for (int encoding = 0; encoding < 3; encoding++) {
        for (unsigned int index = 0; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
            make_path(long_path, lengths[index], encoding); merge.src = long_path;
            w = sumh_iterate_prepare_wrapper(&first, &ctx);
            CHECK(w && !IS_ERR(w) && w->dir_path && !strcmp(w->dir_path, long_path));
            CHECK(strlen(w->dir_path) == (size_t)lengths[index] && w->merge_snapshot);
            CHECK(target.refs == 2 && !rcu_depth);
            sumh_iterate_finish_wrapper(w);
            CHECK(target.refs == 1 && allocated_count == freed && path_allocations == path_frees);
        }
    }
    make_path(long_path, 4096, 0); ctx.pos = SUMH_MAGIC_POS + 7;
    CHECK(sumh_iterate_prepare_wrapper(&first, &ctx) == ERR_PTR(-ENAMETOOLONG));
    CHECK(ctx.pos == (loff_t)SUMH_MAGIC_POS + 7 && target.refs == 1);

    /* A missing primary symbol uses d_path; existing primary errors do not. */
    first.f_path.name = "/system"; merge.src = "/system";
    sumh_d_absolute_path = NULL;
    w = sumh_iterate_prepare_wrapper(&first, &ctx);
    CHECK(w && !IS_ERR(w) && !strcmp(w->dir_path, "/system") && fallback_calls == 1);
    sumh_iterate_finish_wrapper(w);
    sumh_d_absolute_path = absolute_path;
    const int errors[] = { ENOENT, ENOMEM, EIO, ENAMETOOLONG };
    for (unsigned int i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        path_fails = errors[i]; ctx.pos = SUMH_MAGIC_POS + 7;
        CHECK(sumh_iterate_prepare_wrapper(&first, &ctx) == ERR_PTR(-errors[i]));
        CHECK(fallback_calls == 1 && ctx.pos == (loff_t)SUMH_MAGIC_POS + 7);
        CHECK(allocated_count == freed && path_allocations == path_frees && target.refs == 1);
    }
    sumh_d_absolute_path = NULL; path_fails = EIO;
    CHECK(sumh_iterate_prepare_wrapper(&first, &ctx) == ERR_PTR(-EIO) && fallback_calls == 2);
    sumh_d_absolute_path = absolute_path; path_fails = 0;
    for (malformed = 1; malformed <= 6; malformed++) {
        CHECK(sumh_iterate_prepare_wrapper(&first, &ctx) == ERR_PTR(malformed >= 5 ? -ENAMETOOLONG : -EIO));
        CHECK(ctx.pos == (loff_t)SUMH_MAGIC_POS + 7 && fallback_calls == 2);
        CHECK(allocated_count == freed && path_allocations == path_frees);
    }
    malformed = 0; path_allocation_fails = 1;
    previous_path_calls = path_calls;
    CHECK(sumh_iterate_prepare_wrapper(&first, &ctx) == ERR_PTR(-ENOMEM));
    CHECK(path_calls == previous_path_calls && ctx.pos == (loff_t)SUMH_MAGIC_POS + 7);
    CHECK(allocated_count == freed && path_allocations == path_frees);
    path_allocation_fails = 0;
    /* Failure of a nested prepare cannot release/change an outer path. */
    w = sumh_iterate_prepare_wrapper(&first, &ctx);
    CHECK(w && !IS_ERR(w) && target.refs == 2);
    path_allocation_fails = 1;
    CHECK(sumh_iterate_prepare_wrapper(&second, &second_ctx) == ERR_PTR(-ENOMEM));
    CHECK(!strcmp(w->dir_path, "/system") && target.refs == 2);
    path_allocation_fails = 0; sumh_iterate_finish_wrapper(w);
    CHECK(target.refs == 1 && allocated_count == freed && path_allocations == path_frees);

    /* Snapshot producer failure is distinct from wrapper/path failure. The
     * real producer and complete file/mount ownership are tested separately. */
    snapshot_error = EIO; ctx.pos = SUMH_MAGIC_POS + 3;
    CHECK(sumh_iterate_prepare_wrapper(&first, &ctx) == ERR_PTR(-EIO));
    CHECK(ctx.pos == (loff_t)SUMH_MAGIC_POS + 3);
    snapshot_error = 0;
    CHECK(allocated_count == freed && path_allocations == path_frees);
    CHECK(!bad_context && !bad_dget);
    return 0;
}
"""
    guard = function("kernel/sumh/overlay/sumh_overlay.c", "bool sumh_is_merge_context")
    position = function("kernel/sumh/hooks/sumh_vfs_hooks.c", "static bool sumh_iterate_injected_pos(")
    cleanup = function("kernel/sumh/hooks/sumh_vfs_hooks.c", "static void sumh_iterate_free_wrapper(")
    prepare = function("kernel/sumh/hooks/sumh_vfs_hooks.c", "SUMH_NOCFI struct sumh_filldir_wrapper *")
    finish = function("kernel/sumh/hooks/sumh_vfs_hooks.c", "void sumh_iterate_finish_wrapper")
    wrapper = function("kernel/sumh/internal/sumh_types.h", "struct sumh_filldir_wrapper {") + ";\n"
    # LLP64 Windows differs from the supported ARM64 kernel pointer model.
    prepare = prepare.replace("unsigned long", "__UINTPTR_TYPE__")
    preamble = preamble.replace("PRODUCTION_WRAPPER", wrapper)
    return COMMON + directory_limits() + preamble + guard + position + cleanup + prepare + finish + body


def run_test(compiler, name, source):
    with tempfile.TemporaryDirectory(prefix=f"sumh-{name}-") as temporary:
        directory = Path(temporary)
        src = directory / "test.c"
        exe = directory / ("test.exe" if os.name == "nt" else "test")
        src.write_text(source, encoding="utf-8")
        flags = ["-std=gnu11", "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-Werror", "-Wno-unused-function", "-Wno-unused-variable"]
        if os.name == "nt":
            obj = directory / "test.obj"
            subprocess.run([compiler, "--target=x86_64-pc-windows-msvc", *flags, "-c", str(src), "-o", str(obj)], check=True)
            linker = Path(compiler).with_name("ld.lld.exe")
            subprocess.run([str(linker), "-flavor", "link", "/entry:main", "/subsystem:console", "/nodefaultlib", f"/out:{exe}", str(obj)], check=True)
        else:
            subprocess.run([compiler, *flags, str(src), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], check=False)
        if result.returncode:
            lines = source.splitlines()
            candidates = [f"{i}: {line.strip()}" for i, line in enumerate(lines, 1)
                          if "CHECK(" in line and (i - 1) % 255 + 1 == result.returncode]
            raise AssertionError(f"{name}: exit {result.returncode}; " + "; ".join(candidates))
        print(f"PASS {name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler, "xattr-policy", xattr_policy_source())
    run_test(compiler, "xattr", xattr_source())
    run_test(compiler, "statfs", statfs_source())
    run_test(compiler, "directory", directory_source())
