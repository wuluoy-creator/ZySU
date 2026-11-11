"""Exercise trusted Android zygote specialization with actual kernel C helpers.

The host fixtures cover SELinux type boundaries, marker lifecycle, UID routing
and parent namespace admission. They cannot establish Android device ordering.
"""

import argparse
import os
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, function, run_test


SELINUX = r"""
typedef unsigned int u32;
typedef unsigned int uid_t;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define KERNEL_VERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#define LINUX_VERSION_CODE KERNEL_VERSION(6, 1, 0)
struct task_security_struct { u32 sid; };
#define cred_security_struct task_security_struct
struct kuid { uid_t val; };
struct cred { struct kuid uid; struct task_security_struct *security; };
struct lsm_context { char *context; u32 len; };
static u32 cached_zygote_sid = 1;
static const char *contexts[] = {
    NULL, "u:r:zygote:s0", "u:r:app_zygote:s0",
    "u:r:app_zygote:s0:c512,c768", "u:r:webview_zygote:s0",
    "u:r:webview_zygote:s0:c1.c3", "u:r:untrusted_app:s0:c512,c768",
    "u:r:isolated_app:s0:c512,c768", "u:r:su:s0", "u:r:init:s0",
    "u:r:app_zygote_evil:s0", "u:r:zygote_suffix:s0",
};
static int secctx_reads, secctx_releases, secctx_error;
static struct task_security_struct *selinux_cred(const struct cred *cred) {
    return cred->security;
}
static int __security_secid_to_secctx(u32 sid, struct lsm_context *ctx) {
    secctx_reads++;
    if (secctx_error || sid >= ARRAY_SIZE(contexts) || !contexts[sid]) return -1;
    ctx->context = (char *)contexts[sid];
    ctx->len = (u32)strlen(ctx->context) + 1;
    return 0;
}
static void __security_release_secctx(struct lsm_context *ctx) {
    (void)ctx; secctx_releases++;
}
"""


def selinux_functions():
    return "\n".join(
        function("kernel/selinux/selinux.c", marker)
        for marker in (
            "static bool ksu_is_zygote_context",
            "bool ksu_is_trusted_zygote",
            "bool ksu_zygote_cred_changed",
        )
    )


def selinux_source():
    return COMMON + SELINUX + selinux_functions() + r"""
int main(void) {
    const char *valid[] = {
        "u:r:zygote:s0", "u:r:zygote:s0:c1,c2", "u:r:app_zygote:s0",
        "u:r:app_zygote:s0:c512,c768", "u:r:app_zygote:s0-s0:c0.c1023",
        "u:r:webview_zygote:s0", "u:r:webview_zygote:s0:c1.c3",
    };
    const char *invalid[] = {
        "", "u:r:zygote", "u:r:zygote:", "u:r:zygote_suffix:s0",
        "u:r:app_zygote_evil:s0", "u:r:app_zygote_child:s0",
        "u:r:webview_zygote2:s0", "u:object_r:app_zygote:s0",
        "x:r:app_zygote:s0", "u:x:zygote:s0", "u:r:untrusted_app:s0",
    };
    for (size_t i = 0; i < ARRAY_SIZE(valid); i++) {
        CHECK(ksu_is_zygote_context(valid[i], (u32)strlen(valid[i])));
        CHECK(ksu_is_zygote_context(valid[i], (u32)strlen(valid[i]) + 1));
    }
    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++)
        CHECK(!ksu_is_zygote_context(invalid[i], (u32)strlen(invalid[i]) + 1));
    CHECK(!ksu_is_zygote_context(NULL, 10));
    CHECK(!ksu_is_zygote_context("u:r:app_zygote:s0", 14));

    struct task_security_struct first = { 1 }, second = { 3 };
    struct cred a = { { 0 }, &first }, b = { { 10001 }, &second };
    CHECK(ksu_is_trusted_zygote(&a) && !secctx_reads);
    CHECK(ksu_is_trusted_zygote(&b) && secctx_reads == 1 && secctx_releases == 1);
    CHECK(ksu_zygote_cred_changed(&a, &b));
    CHECK(!ksu_zygote_cred_changed(&a, &a));
    int before = secctx_reads;
    CHECK(!ksu_zygote_cred_changed(&b, &b) && before == secctx_reads);
    first.sid = 6; second.sid = 7;
    CHECK(!ksu_zygote_cred_changed(&a, &b));
    second.sid = 4;
    CHECK(ksu_zygote_cred_changed(&a, &b));
    CHECK(ksu_zygote_cred_changed(&b, &a));
    first.sid = 10;
    CHECK(!ksu_is_trusted_zygote(&a));
    cached_zygote_sid = 0; first.sid = 1;
    CHECK(ksu_is_trusted_zygote(&a));
    secctx_error = 1;
    before = secctx_releases;
    CHECK(!ksu_is_trusted_zygote(&a) && secctx_releases == before);
    a.security = NULL;
    CHECK(!ksu_is_trusted_zygote(&a) && !ksu_is_trusted_zygote(NULL));
    CHECK(!ksu_zygote_cred_changed(&a, &b));
    return 0;
}
"""


TASKS = r"""
#define PER_USER_RANGE 100000
#define FIRST_APPLICATION_UID 10000
#define LAST_APPLICATION_UID 19999
#define FIRST_APP_ZYGOTE_ISOLATED_UID 90000
#define LAST_ISOLATED_UID 99999
#define WEBVIEW_ZYGOTE_UID 1053
#define unlikely(v) (v)
#define likely(v) (v)
#define READ_ONCE(v) (v)
#define pr_info(...) ((void)0)
#define pr_warn(...) ((void)0)
#define SIGKILL 9
#define __NR_reboot 142
#define SECCOMP_MODE_FILTER 2
struct sighand { int siglock; } sighand;
struct nsproxy { void *mnt_ns; } child_ns = { &child_ns }, parent_ns = { &parent_ns };
struct task_struct {
    int pid;
    const char *comm;
    struct cred cred;
    bool alive, marked;
    void *mm;
    struct task_struct *real_parent;
    struct nsproxy *nsproxy;
    struct sighand *sighand;
    struct { int mode; void *filter; } seccomp;
};
static struct task_security_struct child_sec = { 3 }, parent_sec = { 3 };
static struct task_struct parent = {
    .pid = 100, .comm = "app_zygote", .cred = { { 10001 }, &parent_sec },
    .alive = true, .nsproxy = &parent_ns, .mm = &parent,
};
static struct task_struct task = {
    .pid = 101, .comm = "child", .cred = { { 90000 }, &child_sec },
    .alive = true, .real_parent = &parent, .nsproxy = &child_ns,
    .sighand = &sighand, .seccomp = { 0, NULL }, .mm = &task,
};
static struct task_struct *current = &task;
#define current_cred() (&current->cred)
static struct kuid task_uid(struct task_struct *t) { return t->cred.uid; }
static bool allowed, manager, candidate;
static int kills, descriptors, reboot_grants, irq_depth, marker_depth;
static int tracepoint_reg_lock, tracepoint_reg_count = 1;
static int tasklist_lock, tasklist_depth, rcu_depth, parent_lock, cred_refs, task_refs;
static bool is_ksu_domain(void) { return current->cred.security->sid == 8; }
static bool is_task_ksu_domain(const struct cred *cred) { return cred->security->sid == 8; }
static bool ksu_is_allow_uid_for_current(uid_t uid) { (void)uid; return allowed; }
static bool ksu_is_allow_uid(uid_t uid) { (void)uid; return allowed || manager; }
static bool ksu_is_uid_manager(uid_t uid) { (void)uid; return manager; }
static bool ksu_is_manager_auth_candidate(uid_t uid) { (void)uid; return candidate; }
static void force_sig(int sig) { (void)sig; kills++; }
static void spin_lock_irq(int *lock) { (void)lock; irq_depth++; }
static void spin_unlock_irq(int *lock) { (void)lock; irq_depth--; }
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; marker_depth++; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); marker_depth--; } while (0)
static void ksu_set_task_tracepoint_flag(struct task_struct *t) { t->marked = true; }
static void ksu_clear_task_tracepoint_flag(struct task_struct *t) { t->marked = false; }
static void ksu_seccomp_allow_cache(void *filter, int nr) { (void)filter; (void)nr; reboot_grants++; }
static void ksu_install_fd(void) { descriptors++; }
static void rcu_read_lock(void) { rcu_depth++; }
static void rcu_read_unlock(void) { rcu_depth--; }
#define rcu_dereference(v) (v)
static bool pid_alive(struct task_struct *t) { return t->alive; }
static const struct cred *get_task_cred(struct task_struct *t) { cred_refs++; return &t->cred; }
static void put_cred(const struct cred *cred) { (void)cred; cred_refs--; }
static void task_lock(struct task_struct *t) { (void)t; parent_lock++; }
static void task_unlock(struct task_struct *t) { (void)t; parent_lock--; }
static void get_task_struct(struct task_struct *t) { (void)t; task_refs++; }
static void put_task_struct(struct task_struct *t) { (void)t; task_refs--; }
static int task_tgid_vnr(struct task_struct *t) { return t->pid; }
static void read_lock(int *lock) { (void)lock; tasklist_depth++; }
static void read_unlock(int *lock) { (void)lock; tasklist_depth--; }
#define for_each_process_thread(p, t) for ((p) = &parent, (t) = &task; (t); (t) = ((t) == &task ? &parent : NULL))
"""


def task_functions():
    return "\n".join(
        function(path, marker)
        for path, marker in (
            ("kernel/policy/allowlist.h", "static inline bool is_appuid"),
            ("kernel/policy/allowlist.h", "static inline bool is_isolated_process"),
            ("kernel/hook/tp_marker.c", "void ksu_clear_task_tracepoint_flag_if_needed"),
        )
    )


def routing_source():
    preamble = r"""
typedef int pid_t;
#define ESRCH 3
#define EPERM 1
#define O_RDONLY 0
#define GFP_KERNEL 0
#define TWA_RESUME 0
#define ERR_PTR(v) ((void *)(ssize_t)(v))
#define PTR_ERR(v) ((ssize_t)(v))
#define IS_ERR(v) ((size_t)(v) >= (size_t)-4095)
struct callback_head { void (*func)(struct callback_head *); };
struct file { int unused; } mountinfo;
static bool ksu_module_mounted = true, policy_umount = true;
static struct task_security_struct root_sec = { 8 };
static struct cred root_cred = { { 0 }, &root_sec };
static const struct cred *ksu_cred = &root_cred;
static int opens, closes, queued, allocs, frees, override_depth;
static bool fail_open, fail_alloc, fail_queue, reparent_on_open, share_on_open;
static struct callback_head *queued_work;
static unsigned char allocation[256];
static bool sumh_policy_uid_should_umount(uid_t uid) { (void)uid; return policy_umount; }
static void umount_tw_func(struct callback_head *cb) { (void)cb; }
static int scnprintf(char *dst, size_t size, const char *fmt, int pid) {
    (void)size; (void)fmt; (void)pid; dst[0] = '/'; dst[1] = 0; return 1;
}
static const struct cred *override_creds(const struct cred *cred) {
    (void)cred; override_depth++; return current_cred();
}
static void revert_creds(const struct cred *cred) { (void)cred; override_depth--; }
static struct file *filp_open(const char *path, int flags, int mode) {
    (void)path; (void)flags; (void)mode; opens++;
    if (reparent_on_open) current->real_parent = NULL;
    if (share_on_open) child_ns.mnt_ns = parent_ns.mnt_ns;
    return fail_open ? ERR_PTR(-EPERM) : &mountinfo;
}
static void filp_close(struct file *file, void *unused) { (void)file; (void)unused; closes++; }
static void *kzalloc(size_t size, int flags) {
    (void)flags; if (fail_alloc || size > sizeof(allocation)) return NULL;
    allocs++; memset(allocation, 0, size); return allocation;
}
static void kfree(void *ptr) { if (ptr) frees++; }
static int task_work_add(struct task_struct *t, struct callback_head *cb, int mode) {
    (void)t; (void)mode; if (fail_queue) return -1;
    queued++; queued_work = cb; return 0;
}
"""
    bodies = "\n".join((
        function("kernel/feature/kernel_umount.c", "enum ksu_umount_scan_source") + ";",
        function("kernel/feature/kernel_umount.c", "struct umount_tw {") + ";",
        function("kernel/feature/kernel_umount.c", "static bool ksu_parent_scan_context"),
        function("kernel/feature/kernel_umount.c", "static struct file *ksu_open_parent_mountinfo"),
        function("kernel/feature/kernel_umount.c", "int ksu_handle_umount"),
        function("kernel/hook/setuid_hook.c", "int ksu_handle_setresuid"),
    ))
    body = r"""
static void reset(uid_t uid, u32 sid, u32 parent_sid) {
    task.cred.uid.val = uid; child_sec.sid = sid; parent_sec.sid = parent_sid;
    parent.alive = true; task.real_parent = &parent;
    task.nsproxy = &child_ns; parent.nsproxy = &parent_ns;
    child_ns.mnt_ns = &child_ns; parent_ns.mnt_ns = &parent_ns;
    task.seccomp.mode = 0; task.seccomp.filter = NULL;
    allowed = manager = candidate = false;
    ksu_module_mounted = policy_umount = true;
    fail_open = fail_alloc = fail_queue = reparent_on_open = share_on_open = false;
    opens = closes = queued = allocs = frees = kills = descriptors = reboot_grants = 0;
    queued_work = NULL;
}
int main(void) {
    const uid_t isolated[] = { 90000, 98999, 99000, 99999, 190000, 299999 };
    for (size_t i = 0; i < ARRAY_SIZE(isolated); i++) {
        uid_t old_uid = isolated[i] / PER_USER_RANGE * PER_USER_RANGE + 10001;
        reset(isolated[i], 3, 3);
        CHECK(!ksu_handle_setresuid(old_uid, isolated[i]) && queued == 1);
        struct umount_tw *tw = (void *)queued_work;
        CHECK(tw->uid == isolated[i] && tw->source == KSU_UMOUNT_SCAN_PARENT);
        CHECK(tw->mountinfo == &mountinfo && !kills && !descriptors && !reboot_grants);
        CHECK(!cred_refs && !task_refs && !override_depth);
    }
    reset(199999, 5, 5);
    CHECK(!ksu_handle_setresuid(WEBVIEW_ZYGOTE_UID, 199999) && queued == 1);
    reset(10001, 1, 1);
    CHECK(!ksu_handle_setresuid(0, 10001) && queued == 1);

    /* These are not authorized factory-to-isolated transitions. */
    const uid_t excluded_old[] = { 2000, 89999, 90001, 99001, 110001 };
    for (size_t i = 0; i < ARRAY_SIZE(excluded_old); i++) {
        reset(90000, 3, 3);
        CHECK(!ksu_handle_setresuid(excluded_old[i], 90000) && !queued);
    }
    reset(190000, 3, 3);
    CHECK(!ksu_handle_setresuid(10001, 190000) && !queued && !kills);
    reset(10002, 3, 3);
    CHECK(!ksu_handle_setresuid(10001, 10002) && !queued);
    reset(90000, 6, 3);
    CHECK(!ksu_handle_setresuid(10001, 90000) && !queued && !opens);
    reset(90000, 10, 3);
    CHECK(!ksu_handle_setresuid(10001, 90000) && !queued);

    /* EoP checks stay ahead of the new route; a root grant is never added. */
    reset(0, 3, 3);
    CHECK(!ksu_handle_setresuid(10001, 0) && kills == 1 && !queued && !descriptors);
    reset(9999, 3, 3);
    CHECK(!ksu_handle_setresuid(10001, 9999) && kills == 1 && !queued);
    reset(0, 8, 3); allowed = true;
    CHECK(!ksu_handle_setresuid(10001, 0) && !kills && !queued && !descriptors);
    reset(10001, 1, 1); manager = true;
    task.seccomp.mode = SECCOMP_MODE_FILTER; task.seccomp.filter = &task;
    CHECK(!ksu_handle_setresuid(0, 10001) && descriptors == 1 && reboot_grants == 1 && !queued);

    /* A trusted parent alone cannot authorize detaching its shared mounts. */
    reset(90000, 3, 3); child_ns.mnt_ns = parent_ns.mnt_ns;
    CHECK(!ksu_handle_setresuid(10001, 90000) && !queued && !opens);
    reset(90000, 3, 6);
    CHECK(!ksu_handle_setresuid(10001, 90000) && !queued && !opens);
    reset(90000, 3, 3); parent.alive = false;
    CHECK(!ksu_handle_setresuid(10001, 90000) && !queued && !opens);
    reset(90000, 3, 3); task.real_parent = NULL;
    CHECK(!ksu_handle_setresuid(10001, 90000) && !queued && !opens);
    reset(90000, 3, 3); parent.nsproxy = NULL;
    CHECK(!ksu_handle_setresuid(10001, 90000) && !queued && !opens);
    reset(90000, 3, 3); reparent_on_open = true;
    CHECK(!ksu_handle_setresuid(10001, 90000) && !queued && closes == 1);
    reset(90000, 3, 3); share_on_open = true;
    CHECK(!ksu_handle_setresuid(10001, 90000) && !queued && closes == 1);
    reset(90000, 3, 3); fail_open = true;
    CHECK(!ksu_handle_setresuid(10001, 90000) && queued == 1);
    CHECK(((struct umount_tw *)(void *)queued_work)->source == KSU_UMOUNT_SCAN_NONE);
    reset(90000, 3, 3); fail_alloc = true;
    CHECK(!ksu_handle_setresuid(10001, 90000) && !queued && closes == 1);
    reset(90000, 3, 3); fail_queue = true;
    CHECK(!ksu_handle_setresuid(10001, 90000) && !queued && closes == 1 && frees == 1);
    reset(90000, 3, 3); policy_umount = false;
    CHECK(!ksu_handle_setresuid(10001, 90000) && !queued && !opens);
    CHECK(!cred_refs && !task_refs && !override_depth && !parent_lock && !rcu_depth);
    CHECK(!irq_depth && !marker_depth);
    return 0;
}
"""
    return COMMON + SELINUX + selinux_functions() + TASKS + task_functions() + preamble + bodies + body


def marker_source():
    preamble = r"""
#define CONFIG_ARM64 1
struct kprobe { int unused; };
struct pt_regs { size_t regs[31]; };
"""
    bodies = "\n".join(
        function("kernel/hook/tp_marker.c", marker)
        for marker in (
            "static void ksu_refresh_task_tracepoint_flag",
            "static int ksu_zygote_cred_pre",
            "static void ksu_mark_running_process_locked",
        )
    )
    body = r"""
int main(void) {
    struct task_security_struct next_sec = { 3 };
    struct cred next = { { 10001 }, &next_sec };
    struct pt_regs regs = { .regs = { (size_t)&next } };
    child_sec.sid = 1; task.cred.uid.val = 10001;
    task.marked = true;
    /* Root -> app UID may have cleared the marker before setcon. */
    ksu_clear_task_tracepoint_flag_if_needed(&task);
    CHECK(!task.marked);
    CHECK(!ksu_zygote_cred_pre(NULL, &regs) && task.marked);
    child_sec.sid = next_sec.sid;
    task.cred.uid = next.uid;
    /* A later policy/sweep refresh must continue to track app zygotes. */
    task.marked = parent.marked = false; parent_sec.sid = 5;
    ksu_mark_running_process_locked();
    CHECK(task.marked && parent.marked && !cred_refs && !tasklist_depth);

    /* Ordinary app and isolated child specialization drops the marker. */
    for (u32 sid = 6; sid <= 7; sid++) {
        child_sec.sid = 3; next_sec.sid = sid; task.marked = true;
        CHECK(!ksu_zygote_cred_pre(NULL, &regs) && !task.marked);
        child_sec.sid = sid; task.marked = true;
        ksu_mark_running_process_locked();
        CHECK(!task.marked);
    }
    child_sec.sid = 3; next_sec.sid = 6; task.marked = true; allowed = true;
    CHECK(!ksu_zygote_cred_pre(NULL, &regs) && task.marked);
    allowed = false; candidate = true;
    CHECK(!ksu_zygote_cred_pre(NULL, &regs) && task.marked);
    candidate = false; manager = true;
    CHECK(!ksu_zygote_cred_pre(NULL, &regs) && task.marked);
    manager = false; tracepoint_reg_count = 2;
    CHECK(!ksu_zygote_cred_pre(NULL, &regs) && task.marked);
    tracepoint_reg_count = 1;
    CHECK(!ksu_zygote_cred_pre(NULL, &regs) && !task.marked);

    /* Unrelated credential commits are not a marker policy entry point. */
    child_sec.sid = 6; next_sec.sid = 7; task.marked = true;
    CHECK(!ksu_zygote_cred_pre(NULL, &regs) && task.marked);
    child_sec.sid = next_sec.sid = 3; task.marked = false;
    CHECK(!ksu_zygote_cred_pre(NULL, &regs) && !task.marked);
    task.mm = NULL; child_sec.sid = 6; task.marked = true;
    ksu_mark_running_process_locked();
    CHECK(task.marked);
    CHECK(!descriptors && !reboot_grants && !kills && !marker_depth && !cred_refs);
    return 0;
}
"""
    return COMMON + SELINUX + selinux_functions() + TASKS + task_functions() + preamble + bodies + body


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler, "zygote SELinux type boundaries and MLS categories", selinux_source())
    run_test(compiler, "zygote SELinux credentials on Linux 6.18", selinux_source().replace(
        "#define LINUX_VERSION_CODE KERNEL_VERSION(6, 1, 0)",
        "#define LINUX_VERSION_CODE KERNEL_VERSION(6, 18, 0)"))
    run_test(compiler, "zygote UID routing and private parent namespace admission", routing_source())
    run_test(compiler, "zygote marker restoration and ordinary child cleanup", marker_source())
