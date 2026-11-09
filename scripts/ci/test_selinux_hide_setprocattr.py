#!/usr/bin/env python3
"""Check the SELinux current-attribute hook against production C code.

Kernel API doubles exercise permission order and credential ownership across
the supported API versions. This is not an on-device timing measurement.
"""

import argparse
import os
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test


def setprocattr_source(minor):
    version = f"#define LINUX_VERSION_CODE KERNEL_VERSION(6, {minor}, 0)\n"
    version += "#define KERNEL_VERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))\n"
    source = (ROOT / "kernel/feature/selinux_hide.c").read_text(encoding="utf-8")
    start = source.index("#if LINUX_VERSION_CODE", source.index("static write_op_fn orig_context_write"))
    compat = source[start:source.index("static ssize_t my_write_context", start)]
    preamble = r"""
typedef unsigned int u32;
typedef int gfp_t;
#define __nocfi
#define likely(x) (x)
#define SECSID_NULL 0
#define GFP_KERNEL 1
#define EACCES 13
#define EINVAL 22
#define EAGAIN 11
#define ENOMEM 12
#define SECCLASS_PROCESS 2
#define PROCESS__SETCURRENT 1
#define PROCESS__DYNTRANSITION 2
#define PROCESS__PTRACE 3
struct ksu_task_security_struct { u32 sid; };
struct cred { struct ksu_task_security_struct security; };
struct task_struct { const struct cred *cred; struct task_struct *tracer; };
static struct cred old_cred, new_cred, tracer_cred;
static struct task_struct task, tracer;
static struct task_struct *current = &task;
static struct { u32 val; } uid;
#define current_uid() uid
#define selinux_cred(c) (&(c)->security)
#define __task_cred(t) ((t)->cred)
#define ptrace_parent(t) ((t)->tracer)
static u32 current_sid(void) { return task.cred->security.sid; }
struct selinux_policy { int unused; };
static struct selinux_policy backup, *backup_sepolicy = &backup;
struct selinux_state { int unused; };
static struct selinux_state selinux_state;
static int set_error, dyn_error, ptrace_error, bounds_error;
static int parse_error, live_error, prepare_fails, single_threaded;
static int fast_result, fast_calls;
static int parse_calls, prepare_calls, abort_calls, commit_calls, orig_calls;
static int perm_calls, bounds_calls, rcu_depth, bad_call;
static u32 committed_sid;
static size_t parsed_size;
static u32 permission_log[3];

static void rcu_read_lock(void) { rcu_depth++; }
static void rcu_read_unlock(void) { rcu_depth--; }
static bool current_is_single_threaded(void) { return single_threaded; }
static int avc_has_perm(
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
    struct selinux_state *state,
#endif
    u32 ssid, u32 tsid, int tclass, u32 permission, void *audit)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
    if (state != &selinux_state) bad_call++;
#endif
    if (tclass != SECCLASS_PROCESS || audit || perm_calls >= 3) {
        bad_call++; return -EINVAL;
    }
    permission_log[perm_calls++] = permission;
    switch (permission) {
    case PROCESS__SETCURRENT:
        if (ssid != 7 || tsid != 7 || parse_calls) bad_call++;
        return set_error;
    case PROCESS__DYNTRANSITION:
        if (ssid != 7 || tsid != 103 || prepare_calls != 1) bad_call++;
        return dyn_error;
    case PROCESS__PTRACE:
        if (ssid != 29 || tsid != 103 || rcu_depth) bad_call++;
        return ptrace_error;
    default: bad_call++; return -EINVAL;
    }
}
static int security_bounded_transition(
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
    struct selinux_state *state,
#endif
    u32 old_sid, u32 new_sid)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
    if (state != &selinux_state) bad_call++;
#endif
    if (old_sid != 7 || new_sid != 103 || perm_calls != 1) bad_call++;
    bounds_calls++;
    return bounds_error;
}
static int security_context_to_sid_with_policy(struct selinux_policy *policy,
    const char *value, u32 size, u32 *sid, u32 def_sid, gfp_t flags,
    u32 *live_sid, int *live_rc)
{
    if (policy != &backup || def_sid || flags != GFP_KERNEL ||
        perm_calls != 1 || !live_sid || !live_rc) bad_call++;
    if (size != 8 || strncmp(value, "u:r:t:s0", size)) bad_call++;
    parse_calls++; parsed_size = size;
    *sid = 31; /* Backup and live SIDs deliberately differ. */
    *live_sid = 103;
    *live_rc = live_error;
    return parse_error;
}
static int security_context_to_sid_live(const char *value, u32 size, u32 *sid) {
    if (perm_calls != 1 || size != 8 || strncmp(value, "u:r:t:s0", size)) bad_call++;
    fast_calls++; parsed_size = size; *sid = 103; return fast_result;
}
static struct cred *prepare_creds(void) {
    prepare_calls++;
    new_cred = old_cred;
    return prepare_fails ? NULL : &new_cred;
}
static void abort_creds(struct cred *cred) {
    if (cred != &new_cred || commit_calls) bad_call++;
    abort_calls++;
}
static int commit_creds(struct cred *cred) {
    if (cred != &new_cred || abort_calls) bad_call++;
    commit_calls++; committed_sid = cred->security.sid;
    return 0;
}
static int original(const char *name, void *value, size_t size) {
    (void)name; (void)value; (void)size; orig_calls++; return 1337;
}
typedef int (*setprocattr_fn)(const char *, void *, size_t);
static struct { setprocattr_fn original; } selinux_setprocattr_hook = { original };
static void reset(void) {
    set_error = dyn_error = ptrace_error = bounds_error = 0;
    fast_result = -EAGAIN; fast_calls = 0;
    parse_error = live_error = prepare_fails = 0;
    parse_calls = prepare_calls = abort_calls = commit_calls = orig_calls = 0;
    perm_calls = bounds_calls = rcu_depth = bad_call = 0;
    parsed_size = committed_sid = 0;
    memset(permission_log, 0, sizeof(permission_log));
    uid.val = 10000; single_threaded = 1;
    old_cred.security.sid = 7; tracer_cred.security.sid = 29;
    task.cred = &old_cred; task.tracer = NULL; tracer.cred = &tracer_cred;
}
"""
    handlers = "\n".join(
        function("kernel/feature/selinux_hide.c", marker)
        for marker in (
            "static u32 ksu_ptrace_parent_sid",
            "static int __nocfi my_setprocattr",
        )
    )
    body = r"""
int main(void) {
    char context[] = "u:r:t:s0";
    char newline[] = "u:r:t:s0\n";
    char empty[] = "";
    char only_newline[] = "\n";

    reset(); uid.val = 9999;
    CHECK(my_setprocattr("current", context, 8) == 1337);
    CHECK(orig_calls == 1 && !perm_calls && !parse_calls);
    reset();
    CHECK(my_setprocattr("exec", context, 8) == 1337);
    CHECK(orig_calls == 1 && !perm_calls && !parse_calls);

    reset(); set_error = -EACCES; parse_error = -EINVAL;
    CHECK(my_setprocattr("current", newline, 9) == -EACCES);
    CHECK(newline[8] == '\n' && !parse_calls && !prepare_calls && !bad_call);

    reset(); parse_error = -EINVAL; live_error = -ENOMEM;
    CHECK(my_setprocattr("current", context, 8) == -EINVAL);
    CHECK(parse_calls == 1 && !prepare_calls && !orig_calls && !bad_call);
    reset(); live_error = -ENOMEM;
    CHECK(my_setprocattr("current", context, 8) == -ENOMEM);
    CHECK(parse_calls == 1 && !prepare_calls && !orig_calls && !bad_call);

    reset(); prepare_fails = 1;
    CHECK(my_setprocattr("current", context, 8) == -ENOMEM);
    CHECK(prepare_calls == 1 && !abort_calls && !commit_calls && !bad_call);
    reset();
    CHECK(my_setprocattr("current", NULL, 0) == -EINVAL);
    CHECK(!parse_calls && prepare_calls == 1 && abort_calls == 1 && !bad_call);
    reset();
    CHECK(my_setprocattr("current", empty, 1) == -EINVAL);
    CHECK(!parse_calls && abort_calls == 1 && !bad_call);
    reset();
    CHECK(my_setprocattr("current", only_newline, 1) == -EINVAL);
    CHECK(!parse_calls && abort_calls == 1 && !bad_call);

    reset(); single_threaded = 0; bounds_error = -EACCES;
    CHECK(my_setprocattr("current", context, 8) == -EACCES);
    CHECK(bounds_calls == 1 && perm_calls == 1 && abort_calls == 1 && !bad_call);
    reset(); dyn_error = -EACCES;
    CHECK(my_setprocattr("current", context, 8) == -EACCES);
    CHECK(perm_calls == 2 && abort_calls == 1 && !commit_calls && !bad_call);
    reset(); task.tracer = &tracer; ptrace_error = -EACCES;
    CHECK(my_setprocattr("current", context, 8) == -EACCES);
    CHECK(perm_calls == 3 && abort_calls == 1 && !rcu_depth && !bad_call);

    reset();
    CHECK(my_setprocattr("current", newline, 9) == 8);
    CHECK(newline[8] == 0 && parsed_size == 8 && parse_calls == 1);
    CHECK(commit_calls == 1 && committed_sid == 103 && !abort_calls);
    CHECK(!orig_calls && !bounds_calls && perm_calls == 2 && !rcu_depth && !bad_call);

    reset(); single_threaded = 0; task.tracer = &tracer;
    CHECK(my_setprocattr("current", context, 8) == 8);
    CHECK(bounds_calls == 1 && perm_calls == 3 && commit_calls == 1);
    CHECK(permission_log[0] == PROCESS__SETCURRENT);
    CHECK(permission_log[1] == PROCESS__DYNTRANSITION);
    CHECK(permission_log[2] == PROCESS__PTRACE);
    CHECK(committed_sid == 103 && !orig_calls && !abort_calls && !rcu_depth && !bad_call);
    /* Current metadata bypasses the dual parser; failures never prepare creds. */
    reset(); fast_result = 0;
    CHECK(my_setprocattr("current", context, 8) == 8);
    CHECK(fast_calls == 1 && !parse_calls && commit_calls == 1 && !bad_call);
    reset(); fast_result = -EINVAL;
    CHECK(my_setprocattr("current", context, 8) == -EINVAL);
    CHECK(fast_calls == 1 && !parse_calls && !prepare_calls && !bad_call);
    reset(); fast_result = -ENOMEM;
    CHECK(my_setprocattr("current", context, 8) == -ENOMEM);
    CHECK(fast_calls == 1 && !parse_calls && !prepare_calls && !bad_call);
    reset(); fast_result = 0;
    CHECK(my_setprocattr("current", only_newline, 1) == -EINVAL);
    CHECK(!fast_calls && !parse_calls && abort_calls == 1 && !bad_call);
    return 0;
}
"""
    return COMMON + version + compat + preamble + handlers + body


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    for minor in (1, 6, 12, 18):
        run_test(compiler, f"selinux-hide-setprocattr-6.{minor}", setprocattr_source(minor))
