#!/usr/bin/env python3
"""Exercise the real SUMH runtime ABI, control wrapper, CAS and lifecycle.

Kernel services are doubles; this is not a kernel build or observer proof.
The ABI is additionally compiled for ARM32 and ARM64 without linking.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test

IOCTL = "kernel/sumh/control/sumh_ioctl.c"
TYPES = r"""
typedef __SIZE_TYPE__ size_t;
typedef unsigned int __u32, u32;
typedef int __s32, s32;
typedef unsigned long long u64;
#define __aligned_u64 unsigned long long __attribute__((aligned(8)))
#define _IOC(dir, type, nr, size) (((dir) << 30U) | ((size) << 16U) | ((type) << 8U) | (nr))
#define _IO(type, nr) _IOC(0U, type, nr, 0U)
#define _IOR(type, nr, t) _IOC(2U, type, nr, sizeof(t))
#define _IOW(type, nr, t) _IOC(1U, type, nr, sizeof(t))
#define _IOWR(type, nr, t) _IOC(3U, type, nr, sizeof(t))
"""


def uapi():
    return re.sub(r"^#include[^\n]*", "", (ROOT / "uapi/sumh.h").read_text(encoding="utf-8"), flags=re.M)


def abi_source():
    return TYPES + uapi() + r"""
_Static_assert(sizeof(struct sumh_runtime_feature_state) == 32, "entry width");
_Static_assert(sizeof(struct sumh_runtime_state_arg) == 240, "state width");
_Static_assert(sizeof(struct sumh_runtime_set_arg) == 48, "setter width");
_Static_assert(__alignof__(struct sumh_runtime_state_arg) == 8, "state alignment");
_Static_assert(__alignof__(struct sumh_runtime_set_arg) == 8, "setter alignment");
_Static_assert(__builtin_offsetof(struct sumh_runtime_state_arg, generation) == 16, "generation offset");
_Static_assert(__builtin_offsetof(struct sumh_runtime_state_arg, instance_id) == 24, "instance offset");
_Static_assert(__builtin_offsetof(struct sumh_runtime_state_arg, entries) == 48, "entries offset");
_Static_assert(__builtin_offsetof(struct sumh_runtime_set_arg, expected_generation) == 16, "CAS generation");
_Static_assert(__builtin_offsetof(struct sumh_runtime_set_arg, expected_instance_id) == 24, "CAS instance");
_Static_assert(__builtin_offsetof(struct sumh_runtime_set_arg, mount_hide_mode) == 32, "CAS mode");
_Static_assert(SUMH_IOC_GET_RUNTIME_STATE == 0xc0f05331U, "query encoding");
_Static_assert(SUMH_IOC_SET_RUNTIME_FEATURE == 0x40305332U, "setter encoding");
_Static_assert(SUMH_PROTOCOL_VERSION == 1, "legacy protocol retained");
_Static_assert(SUMH_FEATURE_RUNTIME_STATE == (1U << 17), "query capability");
_Static_assert(SUMH_FEATURE_RUNTIME_SET == (1U << 18), "setter capability");
_Static_assert(sizeof(struct sumh_kernel_build_cas_arg) == 168, "build CAS width");
_Static_assert(__alignof__(struct sumh_kernel_build_cas_arg) == 8, "build CAS alignment");
_Static_assert(__builtin_offsetof(struct sumh_kernel_build_cas_arg, expected_generation) == 16, "build CAS generation");
_Static_assert(__builtin_offsetof(struct sumh_kernel_build_cas_arg, release) == 32, "build release offset");
_Static_assert(__builtin_offsetof(struct sumh_kernel_build_cas_arg, version_string) == 97, "build version offset");
_Static_assert(SUMH_IOC_SET_KERNEL_BUILD_CAS == 0x40a85333U, "build setter encoding");
_Static_assert(SUMH_FEATURE_KERNEL_BUILD_CAS == (1U << 19), "build CAS capability");
"""


def runtime_source():
    preamble = r"""
#define __user
#define EINVAL 22
#define EFAULT 14
#define EOPNOTSUPP 95
#define ESHUTDOWN 108
#define ESTALE 116
#define EPERM 1
#define ENAMETOOLONG 36
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v,n) ((v)=(n))
#define smp_load_acquire(p) (*(p))
#define smp_store_release(p,n) (*(p)=(n))
#define BUILD_BUG_ON(v) _Static_assert(!(v), "build constraint")
#define sumh_log(...) ((void)0)
#define pr_warn(...) ((void)0)
static int sumh_mutation_mutex, sumh_config_mutex, sumh_lifecycle_lock;
static int lock_depth, config_depth, read_depth, write_depth, bad_context;
static int trace_writes, from_fault, to_fault, random_calls, hide_calls, rebinds, invalidations;
static int start_error, sumh_init_error = -EOPNOTSUPP;
static bool sumh_started, sumh_enabled, sumh_debug_enabled, sumh_stealth_enabled;
static int sumh_feature_enabled_mask, sumh_mount_hide_mode;
static int sumh_ioctl_tgid, current;
static bool proc_ready = true, mount_ready = true, statfs_ready = true, build_ready = true;
static bool build_enabled, sumh_fop_override_ready = true, sumh_fop_override_stopped;
static int sumh_control_begin(void) {
    if (sumh_init_error) return sumh_init_error;
    if (read_depth || write_depth) bad_context++;
    read_depth++; return 0;
}
static void sumh_control_end(void) { if (read_depth != 1) bad_context++; read_depth--; }
static void mutex_lock(int *m) {
    if (m == &sumh_mutation_mutex) { if (lock_depth || read_depth != 1) bad_context++; lock_depth++; }
    else if (m == &sumh_config_mutex) { if (config_depth || lock_depth != 1) bad_context++; config_depth++; }
    else bad_context++;
}
static void mutex_unlock(int *m) {
    if (m == &sumh_mutation_mutex) { if (lock_depth != 1) bad_context++; lock_depth--; }
    else if (m == &sumh_config_mutex) { if (config_depth != 1) bad_context++; config_depth--; }
    else bad_context++;
}
static void down_write(int *p) { if (p != &sumh_lifecycle_lock || write_depth || read_depth) bad_context++; write_depth++; }
static void up_write(int *p) { if (p != &sumh_lifecycle_lock || write_depth != 1) bad_context++; write_depth--; }
static void get_random_bytes(void *p, size_t n) { if (write_depth != 1) bad_context++; memset(p, 0, n); random_calls++; }
static void *memchr_inv(const void *p, int value, size_t n) {
    const unsigned char *s = p;
    for (size_t i=0; i<n; i++) if (s[i] != (unsigned char)value) return (void *)(s+i);
    return NULL;
}
static int copy_from_user(void *out, const void *in, size_t n) {
    if (lock_depth != 1) bad_context++;
    if (from_fault) return 1;
    memcpy(out, in, n); return 0;
}
static int copy_to_user(void *out, const void *in, size_t n) {
    if (lock_depth != 1) bad_context++;
    if (to_fault) return 1;
    memcpy(out, in, n); return 0;
}
static void atomic_long_set(int *p, long n) { if (p != &sumh_ioctl_tgid) bad_context++; *p=n; trace_writes++; }
static int task_tgid_vnr(int task) { (void)task; return 42; }
static int ksu_sucompat_vfs_enabled(void) { return 1; }
static int ksu_sucompat_vfs_refresh(void) { rebinds++; return 0; }
static bool sumh_proc_proxy_available(void) { return proc_ready; }
static bool sumh_fake_mi_active(void) { return mount_ready; }
static bool sumh_statfs_view_available(void) { return statfs_ready; }
static bool sumh_kernel_build_available(void) { return build_ready; }
static bool sumh_kernel_build_enabled(void) { return build_enabled; }
static bool sumh_hide_rules_available(void) { return true; }
static bool sumh_overlay_xattr_available(void) { return true; }
static bool sumh_is_ready(void) { return !sumh_init_error; }
static void sumh_fake_mi_invalidate_all(void) { invalidations++; }
static int sumh_kernel_build_set(struct sumh_kernel_build_arg *b) {
    if (b->size != sizeof(*b) || b->enable>1) return -EINVAL;
    if (b->enable && !build_ready) return -EOPNOTSUPP;
    build_enabled = b->enable; return 0;
}
static void sumh_kernel_build_clear(void) { build_enabled = false; }
static void sumh_hide_rules_clear(void) { hide_calls++; }
static int sumh_hide_rules_ioctl(unsigned int cmd, void *p) { (void)cmd; (void)p; hide_calls++; return 0; }
static void sumh_dirhijack_clear(void) {}
static void sumh_fop_override_clear(void) {}
static void sumh_sop_shadow_reap(void) {}
static void sumh_store_drain(void) {}
/* Rule-store ownership is covered by its dedicated executable suite. These
 * service doubles check the control wrapper's retirement lock boundary. */
struct sumh_dh_retire { bool initialized, finished; };
static int retire_starts, retire_finishes, release_drains;
static void sumh_dh_retire_init(struct sumh_dh_retire *retired, bool bulk) {
    if (config_depth || lock_depth != 1 || !bulk) bad_context++;
    *retired = (struct sumh_dh_retire){ .initialized = true }; retire_starts++;
}
static void sumh_dh_retire_finish(struct sumh_dh_retire *retired) {
    if (config_depth || lock_depth != 1 || !retired->initialized || retired->finished) bad_context++;
    retired->finished = true; retire_finishes++;
}
static void sumh_dh_drain_releases(void) {
    if (config_depth || lock_depth != 1 || retire_starts != retire_finishes) bad_context++;
    release_drains++;
}
static int sumh_check_builtin_conflicts(void) { return 0; }
static int ksu_sucompat_module_guard_init(void) { return 0; }
static int ksu_sucompat_module_guard_acquire(void) { return 0; }
static int ksu_sucompat_sumh_init(void) { return 0; }
static void ksu_sucompat_sumh_exit(void) {}
static void ksu_sucompat_module_guard_release(void) {}
static void ksu_sucompat_module_guard_exit(void) {}
static int sumh_start_views(void) { return start_error; }
/* Inventory executes its production implementation in the dedicated runner. */
static int sumh_rule_inventory_get(void *arg) { (void)arg; return -EOPNOTSUPP; }
/* Maps deletion executes the real store/control path in its dedicated runner. */
static long sumh_maps_delete_temporary(void *arg) { (void)arg; return -EOPNOTSUPP; }
"""
    text = (ROOT / IOCTL).read_text(encoding="utf-8")
    globals_ = text[text.index("static u64 sumh_runtime_generation;"):text.index("static void sumh_runtime_advance(")]
    functions = function("kernel/sumh/features/sumh_fop_override.c", "bool sumh_fop_override_available(")
    functions += "\n" + function("kernel/sumh/features/sumh_kernel_build.c", "static int sumh_kernel_build_validate_string(")
    functions += "\n" + function("kernel/sumh/features/sumh_kernel_build.c", "int sumh_kernel_build_validate(")
    for marker in ("static bool sumh_mount_hide_available(", "static void sumh_runtime_advance(",
                   "void sumh_runtime_state_init(", "void sumh_runtime_state_shutdown(",
                   "static void sumh_runtime_entry(", "static int sumh_runtime_state_get(",
                   "static void sumh_runtime_record_result(", "static int sumh_runtime_feature_set(",
                   "static int sumh_kernel_build_conditional_set("):
        functions += "\n" + function(IOCTL, marker)
    cleanup = function("kernel/sumh/core/sumh_runtime.c", "void sumh_cleanup_locked(")
    # Use the actual toggle-reset prefix, omitting unrelated rule-table doubles.
    signature = cleanup[:cleanup.index("{")].replace("void sumh_cleanup_locked", "static void sumh_cleanup_locked")
    functions += "\n" + signature + "{\n(void)retired;\n" + cleanup[
        cleanup.index("\t/* Rule cleanup"):cleanup.index("\thash_for_each_safe")] + "\n}\n"
    dispatch = "\nstatic int sumh_dispatch_cmd(unsigned int cmd, void *arg) {\n"
    for name in ("CLEAR_ALL", "GET_VERSION", "GET_ENABLED", "SET_DEBUG", "SET_STEALTH",
                 "SET_ENABLED", "SET_KERNEL_BUILD", "SET_MOUNT_HIDE", "SET_MOUNT_HIDE_MODE",
                 "SET_MAPS_SPOOF", "SET_STATFS_SPOOF", "GET_FEATURES"):
        dispatch += function(IOCTL, f"if (cmd == SUMH_IOC_{name})") + "\n"
    dispatch += "return -EINVAL;\n}\n"
    functions += dispatch + function(IOCTL, "long sumh_handle_ioctl(")
    functions += "\n" + function("kernel/sumh/core/sumh_bootstrap.c", "static int sumh_bootstrap_init(")
    return COMMON + abi_source() + preamble + globals_ + functions + TESTS


TESTS = r"""
static int query(struct sumh_runtime_state_arg *state) {
    memset(state, 0, sizeof(*state)); state->size=sizeof(*state); state->version=1;
    return sumh_handle_ioctl(SUMH_IOC_GET_RUNTIME_STATE, state);
}
static struct sumh_runtime_set_arg setter(u32 id, u32 enable) {
    struct sumh_runtime_set_arg arg = {0};
    arg.size=sizeof(arg); arg.version=1; arg.id=id; arg.enable=enable;
    arg.expected_generation=sumh_runtime_generation; arg.expected_instance_id=sumh_runtime_instance;
    return arg;
}
static int test_query(void) {
    struct sumh_runtime_state_arg state;
    CHECK(!sumh_bootstrap_init() && sumh_runtime_generation==1 && sumh_runtime_instance==1);
    CHECK(random_calls==1 && !sumh_bootstrap_init() && sumh_runtime_generation==1 && random_calls==1);
    int trace=trace_writes, hides=hide_calls, binds=rebinds;
    CHECK(!query(&state) && state.count==6 && state.flags==3 && state.generation==1 && state.instance_id==1);
    CHECK(state.mount_hide_mode==0 && !memchr_inv(state.reserved,0,sizeof(state.reserved)));
    for (int i=0; i<6; i++) {
        CHECK(state.entries[i].id==(u32)i+1 && state.entries[i].supported==1);
        CHECK(!state.entries[i].configured && !state.entries[i].active && !state.entries[i].degraded);
        CHECK(!state.entries[i].last_error && !state.entries[i].flags && !state.entries[i].reserved);
    }
    CHECK(!query(&state) && trace_writes==trace && hide_calls==hides && rebinds==binds);
    /* Every byte after size/version is required zero, including entry output fields. */
    for (size_t i=8; i<sizeof(state); i++) {
        memset(&state,0,sizeof(state)); state.size=sizeof(state); state.version=1;
        ((unsigned char *)&state)[i]=1;
        CHECK(sumh_handle_ioctl(SUMH_IOC_GET_RUNTIME_STATE,&state)==-EINVAL);
    }
    memset(&state,0,sizeof(state)); state.size=sizeof(state)-1; state.version=1;
    CHECK(sumh_handle_ioctl(SUMH_IOC_GET_RUNTIME_STATE,&state)==-EINVAL);
    state.size=sizeof(state); state.version=2;
    CHECK(sumh_handle_ioctl(SUMH_IOC_GET_RUNTIME_STATE,&state)==-EINVAL);
    from_fault=1; CHECK(query(&state)==-EFAULT); from_fault=0;
    to_fault=1; CHECK(query(&state)==-EFAULT); to_fault=0;
    CHECK(sumh_runtime_generation==1 && trace_writes==trace && hide_calls==hides && rebinds==binds);
    int features=0; CHECK(!sumh_handle_ioctl(SUMH_IOC_GET_FEATURES,&features));
    CHECK((features&(SUMH_FEATURE_RUNTIME_STATE|SUMH_FEATURE_RUNTIME_SET))==
          (SUMH_FEATURE_RUNTIME_STATE|SUMH_FEATURE_RUNTIME_SET));
    CHECK(sumh_runtime_generation==1);
    CHECK(!sumh_handle_ioctl(SUMH_IOC_USER_HIDE_QUERY,NULL));
    CHECK(sumh_runtime_generation==1);
    CHECK(!sumh_handle_ioctl(SUMH_IOC_USER_HIDE_UPSERT,NULL));
    CHECK(sumh_runtime_generation==2); /* rule changes share the CAS token */
    return 0;
}
static int test_legacy(void) {
    struct sumh_runtime_state_arg state;
    unsigned int commands[]={SUMH_IOC_SET_DEBUG,SUMH_IOC_SET_STEALTH,SUMH_IOC_SET_MOUNT_HIDE,
                             SUMH_IOC_SET_MAPS_SPOOF,SUMH_IOC_SET_STATFS_SPOOF,SUMH_IOC_SET_KERNEL_BUILD};
    union { int flag; struct sumh_mount_hide_arg mount; struct sumh_maps_spoof_arg maps;
            struct sumh_statfs_spoof_arg statfs; struct sumh_kernel_build_arg build; } payload;
    for (int i=0; i<6; i++) {
        memset(&payload,0,sizeof(payload)); payload.flag=1;
        if (i==5) { payload.build.size=sizeof(payload.build); payload.build.enable=1; }
        u64 before=sumh_runtime_generation;
        from_fault=1; CHECK(sumh_handle_ioctl(commands[i],&payload)==-EFAULT); from_fault=0;
        CHECK(!query(&state) && state.generation==before+1 && state.entries[i].last_error==-EFAULT);
        CHECK(!state.entries[i].configured);
        CHECK(!sumh_handle_ioctl(commands[i],&payload));
        CHECK(!query(&state) && state.generation==before+2 && !state.entries[i].last_error);
        CHECK(state.entries[i].configured && state.entries[i].active && !state.entries[i].degraded);
    }
    /* Logging checks its own toggle; the modified initial UTS remains
     * modified independently of the view-policy gate. */
    sumh_enabled=false;
    CHECK(!query(&state) && state.entries[0].active && state.entries[5].active);
    for (int i=1; i<5; i++) CHECK(state.entries[i].configured && !state.entries[i].active && state.entries[i].degraded);
    sumh_enabled=true;
    /* Readiness loss does not alter configured state or impersonate a setter error. */
    u64 before=sumh_runtime_generation;
    proc_ready=mount_ready=statfs_ready=build_ready=false; sumh_fop_override_stopped=true;
    CHECK(!query(&state) && state.generation==before);
    for (int i=1; i<6; i++) CHECK(!state.entries[i].supported && state.entries[i].configured &&
        !state.entries[i].active && state.entries[i].degraded && !state.entries[i].last_error);
    proc_ready=mount_ready=statfs_ready=build_ready=true; sumh_fop_override_stopped=false;
    memset(&payload,0,sizeof(payload)); CHECK(!sumh_handle_ioctl(SUMH_IOC_SET_MOUNT_HIDE,&payload));
    CHECK(!query(&state) && state.entries[4].supported && state.entries[4].configured &&
          !state.entries[4].active && state.entries[4].degraded);
    int mode=1; before=sumh_runtime_generation;
    CHECK(sumh_handle_ioctl(SUMH_IOC_SET_MOUNT_HIDE_MODE,&mode)==-EOPNOTSUPP);
    CHECK(!query(&state) && state.generation==before+1 && state.mount_hide_mode==0 && state.entries[2].last_error==-EOPNOTSUPP);
    mode=0; CHECK(!sumh_handle_ioctl(SUMH_IOC_SET_MOUNT_HIDE_MODE,&mode));
    from_fault=1; CHECK(sumh_handle_ioctl(SUMH_IOC_SET_DEBUG,&mode)==-EFAULT); from_fault=0;
    before=sumh_runtime_generation;
    CHECK(!sumh_handle_ioctl(SUMH_IOC_CLEAR_ALL,NULL));
    CHECK(retire_starts==1 && retire_finishes==1 && release_drains==1);
    CHECK(!query(&state) && state.generation==before+1 && state.entries[0].configured && state.entries[0].last_error==-EFAULT);
    for (int i=1; i<6; i++) CHECK(!state.entries[i].configured && !state.entries[i].active &&
                                  !state.entries[i].degraded && !state.entries[i].last_error);
    return 0;
}
static int test_cas(void) {
    struct sumh_runtime_state_arg state;
    struct sumh_runtime_set_arg first=setter(SUMH_RUNTIME_DEBUG,0), stale=first;
    u64 before=sumh_runtime_generation;
    CHECK(!sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&first));
    CHECK(sumh_runtime_generation==before+1 && !sumh_debug_enabled);
    CHECK(sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&stale)==-ESTALE);
    CHECK(sumh_runtime_generation==before+1 && !sumh_debug_enabled);
    first=setter(SUMH_RUNTIME_DEBUG,1); first.expected_instance_id++;
    CHECK(sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&first)==-ESTALE && sumh_runtime_generation==before+1);
    /* A failed stale request cannot erase the actual writer's error. */
    int mode=1; CHECK(sumh_handle_ioctl(SUMH_IOC_SET_MOUNT_HIDE_MODE,&mode)==-EOPNOTSUPP);
    before=sumh_runtime_generation; first=setter(SUMH_RUNTIME_MOUNT_HIDE,0); first.expected_generation--;
    CHECK(sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&first)==-ESTALE && sumh_runtime_generation==before);
    CHECK(!query(&state) && state.entries[2].last_error==-EOPNOTSUPP);
    /* Another writer after confirmation prevents a rollback from overwriting it. */
    stale=setter(SUMH_RUNTIME_DEBUG,1); int enabled=1;
    CHECK(!sumh_handle_ioctl(SUMH_IOC_SET_DEBUG,&enabled)); before=sumh_runtime_generation;
    CHECK(sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&stale)==-ESTALE && sumh_runtime_generation==before && sumh_debug_enabled);
    stale=setter(SUMH_RUNTIME_DEBUG,0);
    CHECK(!sumh_handle_ioctl(SUMH_IOC_CLEAR_ALL,NULL)); before=sumh_runtime_generation;
    CHECK(sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&stale)==-ESTALE && sumh_runtime_generation==before);
    for (int i=0; i<8; i++) {
        first=setter(SUMH_RUNTIME_DEBUG,0);
        switch (i) { case 0:first.size--;break; case 1:first.version=2;break; case 2:first.enable=2;break;
            case 3:first.mount_hide_mode=1;break; case 4:first.reserved[0]=1;break;
            case 5:first.reserved[1]=1;break; case 6:first.reserved[2]=1;break; case 7:first.version=0;break; }
        CHECK(sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&first)==-EINVAL && sumh_runtime_generation==before);
    }
    first=setter(SUMH_RUNTIME_KERNEL_BUILD,0);
    CHECK(sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&first)==-EOPNOTSUPP && sumh_runtime_generation==before);
    first.id=0; CHECK(sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&first)==-EOPNOTSUPP && sumh_runtime_generation==before);
    first.id=999; CHECK(sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&first)==-EOPNOTSUPP && sumh_runtime_generation==before);
    from_fault=1; CHECK(sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&first)==-EFAULT); from_fault=0;
    CHECK(sumh_runtime_generation==before);
    int invalidated=invalidations;
    first=setter(SUMH_RUNTIME_MOUNT_HIDE,1); first.expected_generation--;
    CHECK(sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&first)==-ESTALE && invalidations==invalidated);
    for (u32 id=1; id<=5; id++) {
        first=setter(id,1); before=sumh_runtime_generation;
        CHECK(!sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&first));
        CHECK(!query(&state) && state.generation==before+1 && state.entries[id-1].configured && !state.entries[id-1].last_error);
        first=setter(id,1); CHECK(!sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&first));
        CHECK(sumh_runtime_generation==before+2); /* valid no-op still has a token */
        proc_ready=mount_ready=statfs_ready=false; sumh_fop_override_stopped=true;
        first=setter(id,1); before=sumh_runtime_generation;
        int result=sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&first);
        CHECK(result==(id==1 ? 0 : -EOPNOTSUPP));
        CHECK(!query(&state) && state.generation==before+1 && state.entries[id-1].configured && state.entries[id-1].last_error==result);
        first=setter(id,0); CHECK(!sumh_handle_ioctl(SUMH_IOC_SET_RUNTIME_FEATURE,&first));
        CHECK(!query(&state) && !state.entries[id-1].configured && !state.entries[id-1].last_error);
        proc_ready=mount_ready=statfs_ready=true; sumh_fop_override_stopped=false;
    }
    /* Conditional string SET also traverses the real outer wrapper exactly
     * once: no duplicate generation from generic legacy result accounting. */
    struct sumh_kernel_build_cas_arg build={0};
    build.size=sizeof(build); build.version=1; build.enable=1;
    build.release[0]='x'; build.version_string[0]='y';
    build.expected_generation=sumh_runtime_generation; build.expected_instance_id=sumh_runtime_instance;
    before=sumh_runtime_generation;
    CHECK(!sumh_handle_ioctl(SUMH_IOC_SET_KERNEL_BUILD_CAS,&build) && sumh_runtime_generation==before+1);
    CHECK(!query(&state) && state.entries[5].configured && !state.entries[5].last_error);
    CHECK(sumh_handle_ioctl(SUMH_IOC_SET_KERNEL_BUILD_CAS,&build)==-ESTALE && sumh_runtime_generation==before+1);
    build.expected_generation=sumh_runtime_generation; build.enable=0;
    memset(build.release,0,sizeof(build.release)); memset(build.version_string,0,sizeof(build.version_string));
    CHECK(!sumh_handle_ioctl(SUMH_IOC_SET_KERNEL_BUILD_CAS,&build) && sumh_runtime_generation==before+2);
    CHECK(!build_enabled);
    return 0;
}
static int test_lifecycle(void) {
    struct sumh_runtime_state_arg state;
    u64 old_instance=sumh_runtime_instance, old_generation=sumh_runtime_generation;
    CHECK(!sumh_control_begin()); mutex_lock(&sumh_mutation_mutex);
    sumh_runtime_state_shutdown();
    mutex_unlock(&sumh_mutation_mutex); sumh_control_end();
    CHECK(sumh_runtime_generation==old_generation+1 && !sumh_runtime_live);
    sumh_init_error=-ESHUTDOWN; sumh_started=false; sumh_enabled=false;
    int trace=trace_writes;
    CHECK(query(&state)==-ESHUTDOWN && sumh_runtime_instance==old_instance && trace==trace_writes);
    start_error=-EOPNOTSUPP;
    CHECK(sumh_bootstrap_init()==-EOPNOTSUPP && sumh_runtime_instance==old_instance && !sumh_runtime_live);
    start_error=0; CHECK(!sumh_bootstrap_init());
    CHECK(!query(&state) && state.instance_id!=old_instance && state.instance_id && state.generation==1 && random_calls==1);
    for (int i=0; i<6; i++) CHECK(!state.entries[i].configured && !state.entries[i].last_error);
    CHECK(!bad_context && !lock_depth && !config_depth && !read_depth && !write_depth);
    return 0;
}
int main(void) {
    int ret=test_query(); if (ret) return ret;
    ret=test_legacy(); if (ret) return ret;
    ret=test_cas(); if (ret) return ret;
    return test_lifecycle();
}
"""


def check_lifecycle_wiring():
    start = function("kernel/sumh/core/sumh_bootstrap.c", "static int sumh_bootstrap_init(")
    stop = function("kernel/sumh/core/sumh_bootstrap.c", "static void sumh_stop_views(")
    assert start.index("sumh_start_views();") < start.index("sumh_runtime_state_init();") < start.index("smp_store_release(&sumh_enabled, true)")
    assert stop.index("mutex_lock(&sumh_mutation_mutex)") < stop.index("sumh_runtime_state_shutdown();") < stop.index("mutex_unlock(&sumh_mutation_mutex)")


def check_abi(compiler):
    # ARM ABI structures compile without target headers: only Linux ioctl
    # encoding and fixed-width primitive aliases are supplied by this harness.
    with tempfile.TemporaryDirectory(prefix="sumh-runtime-abi-") as tmp:
        source = Path(tmp) / "abi.c"
        source.write_text(abi_source(), encoding="utf-8")
        for target in ("armv7a-linux-gnueabi", "aarch64-linux-gnu"):
            subprocess.run([compiler, "--target=" + target, "-std=gnu11", "-ffreestanding", "-Werror",
                            "-c", str(source), "-o", str(Path(tmp) / (target + ".o"))], check=True)
            print("PASS runtime ABI compile " + target)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("clang is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    check_lifecycle_wiring()
    check_abi(compiler)
    run_test(compiler, "runtime-control-state", runtime_source())
