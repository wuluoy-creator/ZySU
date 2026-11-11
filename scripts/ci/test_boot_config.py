#!/usr/bin/env python3
"""Run production boot-feature and shell-ancestry C code with host kernel API doubles."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return re.sub(r'^#include[^\n]*\n', '', (ROOT / path).read_text(encoding='utf-8'), flags=re.M)


def function(path, signature):
    text = (ROOT / path).read_text(encoding='utf-8')
    start = text.index(signature)
    end = text.index('\n}', start) + 2
    return text[start:end] + '\n'


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "uapi/boot_config.h"
typedef uint64_t u64;
typedef uint32_t u32;
typedef unsigned int uid_t;
#define __init
#define __exit
#define pr_info(...) ((void)0)
#define pr_warn(...) ((void)0)
#define pr_err(...) ((void)0)
#define pr_debug(...) ((void)0)
#define unlikely(x) (x)
#define __user
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v, x) ((v) = (x))
static unsigned proc_proxy_refs;
static int sumh_proc_proxy_get(void) { ++proc_proxy_refs; return 0; }
static void sumh_proc_proxy_put(void) { assert(proc_proxy_refs); --proc_proxy_refs; }
#define KSU_FEATURE_FLAG_SUPPORTED 1
#define KSU_FEATURE_FLAG_FORCED 2
struct ksu_get_feature_cmd { u32 feature_id; u64 value; unsigned char supported; };
static int copy_from_user(void *dest, const void *src, size_t size) { memcpy(dest, src, size); return 0; }
static int copy_to_user(void *dest, const void *src, size_t size) { memcpy(dest, src, size); return 0; }
#define DEFINE_MUTEX(name) int name
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define DECLARE_BITMAP(name, n) bool name[n]
#define test_bit(i, p) ((p)[i])
#define set_bit(i, p) ((p)[i] = true)
#define clear_bit(i, p) ((p)[i] = false)
#define for_each_set_bit(i, p, n) for ((i)=0; (i)<(n); ++(i)) if ((p)[i])
#define KERNEL_PARAM_OPS_FL_NOARG 1
struct kernel_param {};
struct kernel_param_ops { int flags; int (*set)(const char*, const struct kernel_param*); };
#define module_param_cb(...)
static const char *zysu_test_strchrnul(const char *s, int c) { const char *p = strchr(s, c); return p ? p : s + strlen(s); }
#define strchrnul zysu_test_strchrnul
static int kstrtou64(const char *s, int base, u64 *out) {
    char *end;
    if (!*s || *s == '-' || *s == ' ') return -EINVAL;
    errno = 0;
    *out = strtoull(s, &end, base);
    return errno || *end ? -EINVAL : 0;
}
static int kstrtou32(const char *s, int base, u32 *out) {
    u64 value;
    if (kstrtou64(s, base, &value) || value > UINT32_MAX) return -EINVAL;
    *out = value; return 0;
}
static int strscpy(char *out, const char *in, size_t n) {
    size_t len = strlen(in);
    if (len >= n) return -1;
    memcpy(out, in, len + 1); return (int)len;
}
#define GFP_KERNEL 0
static char *native_blacklist;
static unsigned barriers;
static unsigned long long find_kernel_symbol_exact(const char *name) {
    assert(!strcmp(name, "module_blacklist"));
    return (uintptr_t)&native_blacklist;
}
static char *kasprintf(int unused, const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    char *result = malloc(2048);
    vsnprintf(result, 2048, fmt, args);
    va_end(args); return result;
}
#define smp_store_release(p, v) (*(p) = (v))
#define kick_all_cpus_sync() (++barriers)
struct static_key { bool enabled; };
#define DEFINE_STATIC_KEY_FALSE(name) struct { struct static_key key; } name = {{false}}
#define static_branch_unlikely(p) ((p)->key.enabled)
#define static_key_enabled(p) ((p)->key.enabled)
#define static_key_enable(p) ((p)->enabled = true)
#define static_key_disable(p) ((p)->enabled = false)
struct cred { int domain; };
struct task_struct { int pid; struct cred cred; struct task_struct *real_parent; };
static struct task_struct *current;
static int rcu_depth;
#define rcu_read_lock() (++rcu_depth)
#define rcu_read_unlock() (--rcu_depth)
#define rcu_dereference(p) (p)
#define __task_cred(t) (&(t)->cred)
static bool is_adbd(const struct cred *c) { assert(rcu_depth); return c->domain == 1; }
static bool is_task_ksu_domain(const struct cred *c) { return c->domain == 2; }
static bool is_ksu_domain(void) { return is_task_ksu_domain(&current->cred); }
#define SHELL_UID 2000
static bool shell_granted;
static bool __ksu_is_allow_uid(uid_t uid) { return uid == SHELL_UID ? shell_granted : uid == 10001; }
'''

BODY = r'''
static u64 value;
static unsigned attempts;
static bool ready;
static int get_value(u64 *out) { *out = value; return 0; }
static int set_value(u64 val) { ++attempts; if (!ready) return -EAGAIN; value = val; return 0; }
static int deferred_set(u64 val) { ++attempts; value = val; return 0; }
static int get_status(u64 *out) { *out = ready ? KSU_SELINUX_HIDE_ACTIVE : KSU_SELINUX_HIDE_PENDING_REBOOT; return 0; }
int main(void) {
    ksu_feature_init();
    ksu_feature_parse_forced(NULL);
    ksu_feature_parse_forced(",,sulog:0x10,6:1,unknown:1,111:1,99:1,sulog:-1,sulog:18446744073709551616,sumh:0,100:0,108:1,0:1");
    assert(ksu_feature_is_forced(2) && forced_values[2] == 16);
    assert(ksu_feature_is_forced(6));
    assert(!ksu_feature_is_forced(99) && !ksu_feature_is_forced(100));
    assert(!ksu_feature_is_forced(106) && !ksu_feature_is_forced(108));
    assert(!ksu_feature_is_forced(UINT32_MAX) && !ksu_feature_is_forced(0));
    // The removed global switch has no handler, including after real umount init.
    ksu_kernel_umount_init();
    ksu_feature_parse_forced("kernel_umount:0,1:0,kernel_umount:1,1:1");
    assert(!ksu_feature_is_forced(1) && !test_bit(1, forced_pending));
    struct ksu_get_feature_cmd umount_query = {1, UINT64_MAX, 0xff};
    assert(!do_get_feature(&umount_query));
    assert(umount_query.value == 0 && umount_query.supported == 0);
    assert(ksu_set_feature(1, 0) == -EOPNOTSUPP);
    assert(ksu_set_feature(1, 1) == -EOPNOTSUPP);
    ksu_feature_apply_forced();
    assert(!do_get_feature(&umount_query) && umount_query.supported == 0);
    // The independent WebView and namespace cleanup controls still work.
    assert(!ksu_set_feature(KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT, 1));
    assert(ksu_is_webview_zygote_umount_enabled());
    assert(!ksu_set_feature(KSU_FEATURE_UNSHARE_MNT, 1));
    assert(ksu_is_unshare_mnt_enabled() && proc_proxy_refs == 1);
    ksu_kernel_umount_exit();
    assert(!ksu_is_unshare_mnt_enabled() && proc_proxy_refs == 0);
    ksu_feature_parse_forced("sulog:3");
    assert(forced_values[2] == 3);
    ksu_feature_apply_forced();
    assert(test_bit(2, forced_pending));
    struct ksu_feature_handler handler = {2, "sulog", get_value, set_value};
    assert(!ksu_register_feature_handler(&handler));
    assert(ksu_set_feature(2, 0) == -EPERM && attempts == 0);
    ksu_feature_apply_forced();
    assert(attempts == 1 && test_bit(2, forced_pending));
    ready = true; ksu_feature_apply_forced();
    assert(value == 3 && !test_bit(2, forced_pending));
    unsigned applied = attempts; ksu_feature_apply_forced(); assert(attempts == applied);
    bool supported = false; u64 observed = 0;
    assert(!ksu_get_feature(2, &observed, &supported) && supported && observed == 3);
    struct ksu_get_feature_cmd query = {2, 0, 0};
    assert(!do_get_feature(&query) && query.value == 3 && query.supported == 3);
    query.feature_id = 99; assert(!do_get_feature(&query) && query.supported == 0);
    query.feature_id = UINT32_MAX; assert(!do_get_feature(&query) && query.supported == 0);
    handler.feature_id = 3; assert(!ksu_register_feature_handler(&handler));
    assert(!ksu_set_feature(3, 0));

    // Local SELinux reports successful intent before runtime activation: retry it.
    struct ksu_feature_handler selinux = {4, "selinux_hide", get_value, deferred_set};
    struct ksu_feature_handler status = {108, "selinux_hide_status", get_status, NULL};
    assert(!ksu_register_feature_handler(&selinux));
    assert(!ksu_register_feature_handler(&status));
    ready = false; ksu_feature_parse_forced("4:1"); ksu_feature_apply_forced();
    assert(test_bit(4, forced_pending));
    ready = true; ksu_feature_apply_forced(); assert(!test_bit(4, forced_pending));

    struct task_struct init = {1, {2}, NULL}; init.real_parent = &init;
    struct task_struct adb = {10, {1}, &init};
    struct task_struct shell = {11, {0}, &adb}; current = &shell;
    ksu_shell_su_adb_only_init();
    shell_granted = true;
    assert(__ksu_is_allow_uid_for_current(SHELL_UID));
    shell_su_adb_only_feature_set(1);
    assert(__ksu_is_allow_uid_for_current(SHELL_UID));
    shell_granted = false; assert(!__ksu_is_allow_uid_for_current(SHELL_UID));
    shell_granted = true;
    adb.cred.domain = 2; assert(__ksu_is_allow_uid_for_current(SHELL_UID));
    adb.cred.domain = 0; assert(!__ksu_is_allow_uid_for_current(SHELL_UID));
    assert(ksu_is_uid_granted_root(SHELL_UID)); // Manager query is ancestry-independent.
    assert(__ksu_is_allow_uid_for_current(10001));
    shell.real_parent = &init; assert(!__ksu_is_allow_uid_for_current(SHELL_UID));
    struct task_struct chain[129];
    for (unsigned i=0; i<129; ++i) { chain[i].pid = 20+i; chain[i].cred.domain = 0; chain[i].real_parent = i+1 < 129 ? &chain[i+1] : &init; }
    current = chain; chain[128].cred.domain = 1;
    assert(!__ksu_is_allow_uid_for_current(SHELL_UID));
    chain[127].cred.domain = 1; assert(__ksu_is_allow_uid_for_current(SHELL_UID));
    current = &shell; shell_su_adb_only_feature_set(0);
    assert(__ksu_is_allow_uid_for_current(SHELL_UID) && rcu_depth == 0);

    ksu_module_blacklist_init(); assert(native_blacklist == NULL);
    ksu_module_blacklist_parse("mkp,vr");
    ksu_module_blacklist_parse("bad;name"); assert(!strcmp(block_modules, "mkp,vr"));
    ksu_module_blacklist_parse("mkp,"); assert(!strcmp(block_modules, "mkp,vr"));
    char long_list[257]; memset(long_list, 'a', sizeof(long_list)-1); long_list[256] = 0;
    ksu_module_blacklist_parse(long_list); assert(!strcmp(block_modules, "mkp,vr"));
    native_blacklist = "existing"; ksu_module_blacklist_init();
    assert(!strcmp(native_blacklist, "existing,mkp,vr") && barriers == 1);
    char *installed = native_blacklist; ksu_module_blacklist_exit();
    assert(native_blacklist == installed); // Remains valid after unload.
    free(installed); native_blacklist = NULL;
    puts("PASS kernel forced features, deferred activation, shell ancestry and module blacklist");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'clang'))
    parser.add_argument('--zig', type=Path)
    args = parser.parse_args()
    compiler = [str(args.zig), 'cc'] if args.zig else [args.cc]
    code = PRELUDE + source('kernel/policy/feature.h') + source('kernel/policy/feature.c')
    umount = source('kernel/feature/kernel_umount.c')
    code += umount[:umount.index('extern int path_umount(')]
    code += function('kernel/feature/kernel_umount.c', 'void ksu_kernel_umount_init(')
    code += function('kernel/feature/kernel_umount.c', 'void ksu_kernel_umount_exit(')
    code += function('kernel/supercall/dispatch.c', 'static int do_get_feature(')
    code += source('kernel/feature/shell_su_adb_only.c')
    code += function('kernel/policy/allowlist.c', 'bool ksu_is_uid_granted_root(')
    code += function('kernel/policy/allowlist.c', 'bool __ksu_is_allow_uid_for_current(')
    code += source('kernel/feature/module_blacklist.c') + BODY
    with tempfile.TemporaryDirectory(prefix='ksu-boot-config-') as tmp:
        directory = Path(tmp)
        path = directory / 'test.c'
        binary = directory / ('test.exe' if os.name == 'nt' else 'test')
        path.write_text(code, encoding='utf-8')
        subprocess.run([*compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter',
                        '-I' + str(ROOT), str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == '__main__':
    main()
