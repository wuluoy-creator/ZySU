#!/usr/bin/env python3
"""Run production policy publication/type-generation code against host doubles."""

import argparse
import os
from pathlib import Path
import re
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test


def policy_source(minor):
    preamble = r"""
typedef unsigned int u32;
#define NULL ((void *)0)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, value) ((x) = (value))
#define pr_info(...) ((void)0)
#define pr_warn(...) ((void)0)
#define pr_err(...) ((void)0)
#define ALL NULL
#define KERNEL_SU_DOMAIN "su"
#define KERNEL_SU_FILE "ksu_file"
#define POLICYDB_VERSION_XPERMS_IOCTL 30
#define IS_ERR(p) ((p) == (void *)-12)
#define PTR_ERR(p) ((long)(p))
struct policydb { struct { u32 nprim; } p_types; int policyvers; };
struct selinux_policy { struct policydb policydb; u32 latest_granting; };
struct selinux_avc { int unused; };
static struct selinux_avc avc;
static struct { struct selinux_policy *policy; int policy_mutex; struct selinux_avc *avc; } selinux_state;
static int ksu_rules;
static const struct policydb *ksu_original_types_policy;
static u32 ksu_original_types_limit, ksu_original_types_generation;
static struct selinux_policy policies[8], backup, *backup_sepolicy;
static int next_policy, clone_fail, errors, rules, syncs, destroys, notifications;
static u32 reset_generation, netlink_generation, status_generation;
#define lockdep_is_held(p) (*(p))
#define rcu_dereference_protected(p, held) ((held) ? (p) : (errors++, (p)))
#define rcu_assign_pointer(p, value) ((p) = (value))
static void mutex_lock(int *lock) { if (*lock) errors++; *lock = 1; }
static void mutex_unlock(int *lock) { if (!*lock) errors++; *lock = 0; }
static bool getenforce(void) { return true; }
static void synchronize_rcu(void) { syncs++; }
static struct selinux_policy *ksu_dup_sepolicy(struct selinux_policy *old) {
    if (!selinux_state.policy_mutex || !ksu_rules) errors++;
    if (clone_fail) return (void *)-12;
    policies[next_policy] = *old;
    return &policies[next_policy++];
}
static void ksu_destroy_sepolicy(struct selinux_policy *pol) {
    if (pol == selinux_state.policy || syncs != destroys + 1) errors++;
    destroys++;
}
static void backup_original_sepolicy_once(void) {
    if (!backup_sepolicy) { backup = *selinux_state.policy; backup_sepolicy = &backup; }
}
static void rule(struct policydb *db) {
    if (db == &selinux_state.policy->policydb || !selinux_state.policy_mutex) errors++;
    rules++;
}
#define ksu_permissive(db, ...) rule(db)
#define ksu_typeattribute(db, ...) rule(db)
#define ksu_allow(db, ...) rule(db)
#define ksu_allowxperm(db, ...) rule(db)
#define ksu_dontaudit(db, ...) rule(db)
static void ksu_type(struct policydb *db, const char *name, const char *attr) {
    rule(db);
    if (strcmp(name, "ksu_file") || strcmp(attr, "file_type")) errors++;
    if (db->p_types.nprim == 4) db->p_types.nprim++;
}
static int avc_ss_reset(
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    struct selinux_avc *cache,
#endif
    u32 seqno) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    if (cache != &avc) errors++;
#endif
    reset_generation = seqno; return 0;
}
static void selnl_notify_policyload(u32 seqno) { netlink_generation = seqno; }
static void selinux_status_update_policyload(
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    typeof(selinux_state) *state,
#endif
    u32 seqno) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    if (state != &selinux_state) errors++;
#endif
    status_generation = seqno;
}
static void selinux_xfrm_notify_policyload(void) { notifications++; }
"""
    metadata = "\n".join(
        function("kernel/selinux/sepolicy.c", name)
        for name in ("bool ksu_set_original_types", "bool ksu_advance_original_types",
                     "int ksu_type_is_original")
    )
    path = "kernel/selinux/rules.c"
    source = (ROOT / path).read_text(encoding="utf-8")
    reset = re.search(r"static void reset_avc_cache\([^;{}]*\)\s*\{", source)
    handlers = function(path, reset.group(0)) + "\n" + function(path, "void apply_kernelsu_rules")
    body = r"""
int main(void) {
    policies[0] = (struct selinux_policy){ .policydb = { {4}, 33 }, .latest_granting = 7 };
    selinux_state.policy = &policies[0]; selinux_state.avc = &avc; next_policy = 1;

    /* Failed clone never mutates the live policy, backup, marks or generation. */
    clone_fail = 1; apply_kernelsu_rules();
    CHECK(selinux_state.policy == &policies[0] && !backup_sepolicy && !rules);
    CHECK(!notifications && !destroys && !ksu_rules && !selinux_state.policy_mutex);
    clone_fail = 0; apply_kernelsu_rules();
    CHECK(selinux_state.policy == &policies[1] && policies[0].policydb.p_types.nprim == 4);
    CHECK(policies[1].latest_granting == 8 && backup.policydb.p_types.nprim == 4);
    CHECK(reset_generation == 8 && netlink_generation == 8 && status_generation == 8);
    CHECK(ksu_type_is_original(&policies[1].policydb, 8, 4) == 1);
    CHECK(ksu_type_is_original(&policies[1].policydb, 8, 5) == 0);

    /* Reapplying cannot classify ksu_file as an original policy type. */
    apply_kernelsu_rules();
    CHECK(selinux_state.policy == &policies[2] && policies[2].latest_granting == 9);
    CHECK(reset_generation == 9 && netlink_generation == 9 && status_generation == 9);
    CHECK(ksu_type_is_original(&policies[2].policydb, 9, 5) == 0);
    CHECK(ksu_type_is_original(&policies[1].policydb, 8, 4) == -1);
    CHECK(ksu_type_is_original(&policies[2].policydb, 8, 4) == -1);
    CHECK(ksu_type_is_original(&policies[2].policydb, 9, 0) == 0);
    CHECK(ksu_type_is_original(&policies[2].policydb, 9, 6) == -1);
    CHECK(notifications == 2 && syncs == 2 && destroys == 2 && !errors);

    /* Unrelated policy identity, generation wrap and shrinking tables fall back. */
    policies[3] = policies[2];
    CHECK(!ksu_advance_original_types(&policies[3].policydb, &policies[1].policydb, 8, 10));
    CHECK(ksu_type_is_original(&policies[2].policydb, 9, 4) == -1);
    CHECK(ksu_set_original_types(&policies[2].policydb, &policies[0].policydb, 9));
    CHECK(!ksu_advance_original_types(&policies[3].policydb, &policies[2].policydb, 9, 0));
    CHECK(ksu_type_is_original(&policies[2].policydb, 9, 4) == -1);
    CHECK(ksu_set_original_types(&policies[2].policydb, &policies[0].policydb, 9));
    policies[3].policydb.p_types.nprim = 3;
    CHECK(!ksu_advance_original_types(&policies[3].policydb, &policies[2].policydb, 9, 10));
    CHECK(!ksu_set_original_types(&policies[3].policydb, &policies[0].policydb, 10));
    CHECK(ksu_type_is_original(&policies[3].policydb, 10, 1) == -1);
    CHECK(!ksu_rules && !selinux_state.policy_mutex && !errors);
    return 0;
}
"""
    version = "#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))\n"
    version += f"#define LINUX_VERSION_CODE KERNEL_VERSION(6,{minor},0)\n"
    return COMMON + version + preamble + metadata + handlers + body


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    for minor in (1, 6):
        run_test(compiler, f"selinux-policy-updates-6.{minor}", policy_source(minor))
