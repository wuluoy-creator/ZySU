"""Exercise SuperKey configuration, authorization and first-login reachability.

Runs production C functions with deterministic kernel API doubles. These tests
check policy and state transitions; they do not replace a kernel/device test.
"""

import argparse
import os
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True
from test_manager_scan import harness
from test_sumh_vfs_helpers import COMMON, function, run_test


def configuration_source():
    preamble = r"""
typedef unsigned long long u64;
typedef unsigned char u8;
#define SUPERKEY_MAGIC 0x5355504552ULL
#define SUPERKEY_UNPATCHED (~0ULL)
#define SUPERKEY_SALT_LEN 16
#define SUPERKEY_MAX_LEN 64
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
static u64 ksu_superkey_hash, ksu_superkey_verification_mode;
static u8 ksu_superkey_salt[SUPERKEY_SALT_LEN];
static bool ksu_signature_bypass;
static const char *test_compile_key;
#define COMPILE_TIME_SUPERKEY test_compile_key
static int random_calls, hash_calls, hash_errors;
static void get_random_bytes(void *out, size_t size) {
    random_calls++;
    memset(out, 0x7b, size);
}
static u64 hash_superkey(const char *key) {
    hash_calls++;
    if (strcmp(key, test_compile_key)) hash_errors++;
    for (size_t i = 0; i < SUPERKEY_SALT_LEN; i++)
        if (ksu_superkey_salt[i] != 0x7b) hash_errors++;
    return 0x12345678;
}
"""
    structure = function("kernel/manager/superkey.c", "struct superkey_data {")
    fixtures = r"""
static volatile struct superkey_data superkey_store;
static void reset(void) {
    ksu_superkey_hash = ksu_superkey_verification_mode = 0;
    memset(ksu_superkey_salt, 0, sizeof(ksu_superkey_salt));
    ksu_signature_bypass = false;
    random_calls = hash_calls = hash_errors = 0;
    memset((void *)&superkey_store, 0, sizeof(superkey_store));
    superkey_store.magic = SUPERKEY_MAGIC;
    superkey_store.flags = SUPERKEY_UNPATCHED;
    test_compile_key = "compiled-key";
}
"""
    production = function("kernel/manager/superkey.c", "void superkey_init")
    body = r"""
int main(void) {
    reset(); superkey_init();
    CHECK(ksu_superkey_hash == 0x12345678 && ksu_superkey_verification_mode == 1);
    CHECK(!ksu_signature_bypass && random_calls == 1 && hash_calls == 1 && !hash_errors);

    /* An explicit injected signature-only mode overrides the build-time key. */
    reset(); superkey_store.flags = 0; superkey_init();
    CHECK(!ksu_superkey_hash && !ksu_superkey_verification_mode && !ksu_signature_bypass);
    CHECK(!random_calls && !hash_calls);

    for (u64 mode = 1; mode <= 2; mode++) {
        reset();
        superkey_store.flags = mode;
        superkey_store.hash = 0xa1b2c3d4;
        memset((void *)superkey_store.salt, 0x42, sizeof(superkey_store.salt));
        superkey_init();
        CHECK(ksu_superkey_hash == 0xa1b2c3d4 && ksu_superkey_verification_mode == mode);
        CHECK(ksu_signature_bypass == (mode == 2) && !random_calls && !hash_calls);
        for (size_t i = 0; i < SUPERKEY_SALT_LEN; i++) CHECK(ksu_superkey_salt[i] == 0x42);
    }
    /* A patch without a hash must never silently activate the build-time key. */
    reset(); superkey_store.flags = 2; superkey_init();
    CHECK(!ksu_superkey_hash && !ksu_signature_bypass && !hash_calls);
    reset(); test_compile_key = NULL; superkey_init();
    CHECK(!ksu_superkey_hash && !random_calls && !hash_calls);
    reset(); test_compile_key = ""; superkey_init();
    CHECK(!ksu_superkey_hash && !random_calls && !hash_calls);

    char long_key[SUPERKEY_MAX_LEN + 2];
    memset(long_key, 'x', sizeof(long_key)); long_key[sizeof(long_key) - 1] = 0;
    reset(); test_compile_key = long_key; superkey_init();
    CHECK(!ksu_superkey_hash && !random_calls && !hash_calls);
    long_key[SUPERKEY_MAX_LEN] = 0;
    reset(); test_compile_key = long_key; superkey_init();
    CHECK(ksu_superkey_hash == 0x12345678 && hash_calls == 1 && !hash_errors);
    return 0;
}
"""
    return COMMON + preamble + structure + ";\n" + fixtures + production + body


def signature_policy_source():
    return harness(superkey=True) + r"""
int main(void) {
    init_fixture();
    superkey_set = true;
    apps[1].installed = false;
    track_throne(true);
    test_current_uid = 110001;
    /* Correct package and password with an untrusted signature must fail when
     * signature bypass is off. A signature-only crown is not fabricated. */
    CHECK(superkey_authenticate("correct-key") == -EACCES);
    CHECK(superkey_get_manager_uid() == KSU_INVALID_UID && !superkey_is_manager());
    CHECK(!granted && signatures == 1);

    apps[0].trusted = true;
    apps[0].stat.ctime.tv_nsec++;
    CHECK(superkey_authenticate("correct-key") == 0);
    CHECK(superkey_get_manager_uid() == 110001 && superkey_is_manager());

    /* Explicit password-only mode accepts the same package and password with
     * a different signing certificate, without making it signature-trusted. */
    init_fixture();
    superkey_set = superkey_bypassed = true;
    track_throne(true);
    test_current_uid = 110001;
    CHECK(superkey_authenticate("correct-key") == 0);
    CHECK(superkey_is_manager() && !granted && !signatures);
    test_current_uid = 10002;
    CHECK(superkey_authenticate("correct-key") == -EACCES);
    CHECK(!superkey_is_manager());
    clear_apk_scan_cache();
    CHECK(!live_allocations && !allocation_errors && !creds_depth && !lock_depth && !bad_lock);
    CHECK(!superkey_spin_depth && !superkey_spin_errors);
    return 0;
}
"""


def rate_limit_source():
    return harness(superkey=True) + r"""
int main(void) {
    init_fixture();
    superkey_set = superkey_bypassed = true;
    track_throne(true);
    test_current_uid = 10002;
    for (int i = 0; i < 100; i++) CHECK(superkey_authenticate("wrong-key") == -EACCES);
    CHECK(!password_failures);

    /* Wrong packages cannot consume the manager's quota; Android profiles
     * have separate quotas. Blocked attempts do not hash the password. */
    test_current_uid = 10001;
    for (int i = 0; i < SUPERKEY_AUTH_ATTEMPTS; i++)
        CHECK(superkey_authenticate("wrong-key") == -EACCES);
    CHECK(password_failures == SUPERKEY_AUTH_ATTEMPTS);
    CHECK(superkey_authenticate("correct-key") == -EAGAIN);
    CHECK(password_failures == SUPERKEY_AUTH_ATTEMPTS);
    test_current_uid = 110001;
    CHECK(superkey_authenticate("correct-key") == 0);
    test_current_uid = 10001;
    CHECK(superkey_authenticate("correct-key") == -EAGAIN);
    jiffies += SUPERKEY_AUTH_WINDOW;
    CHECK(superkey_authenticate("correct-key") == 0);

    /* More than ten failures across multiple windows never reboot the host,
     * and each successful authentication clears only that UID's quota. */
    for (int round = 0; round < 20; round++) {
        for (int i = 1; i < SUPERKEY_AUTH_ATTEMPTS; i++)
            CHECK(superkey_authenticate("wrong-key") == -EACCES);
        CHECK(superkey_authenticate("correct-key") == 0);
    }
    CHECK(superkey_is_manager());

    /* Filling the bounded table must not let a new identity evict and reset
     * an active limit. Expiry releases the slots, including after wraparound. */
    init_fixture();
    superkey_set = superkey_bypassed = true;
    track_throne(true);
    for (int i = 0; i < SUPERKEY_AUTH_SLOTS; i++) {
        test_current_uid = (uid_t)i * PER_USER_RANGE + 10001;
        for (int attempt = 0; attempt < SUPERKEY_AUTH_ATTEMPTS; attempt++)
            CHECK(superkey_authenticate("wrong-key") == -EACCES);
    }
    test_current_uid = SUPERKEY_AUTH_SLOTS * PER_USER_RANGE + 10001;
    CHECK(superkey_authenticate("correct-key") == -EAGAIN);
    test_current_uid = 10001;
    CHECK(superkey_authenticate("correct-key") == -EAGAIN);
    jiffies += SUPERKEY_AUTH_WINDOW;
    CHECK(superkey_authenticate("correct-key") == 0);

    memset(superkey_auth_limits, 0, sizeof(superkey_auth_limits));
    jiffies = ~0UL - SUPERKEY_AUTH_WINDOW / 2;
    for (int attempt = 0; attempt < SUPERKEY_AUTH_ATTEMPTS; attempt++)
        CHECK(superkey_authenticate("wrong-key") == -EACCES);
    CHECK(superkey_authenticate("correct-key") == -EAGAIN);
    jiffies += SUPERKEY_AUTH_WINDOW;
    CHECK(superkey_authenticate("correct-key") == 0);
    CHECK(!superkey_spin_depth && !superkey_spin_errors);
    CHECK(!live_allocations && !allocation_errors && !creds_depth && !lock_depth && !bad_lock);
    return 0;
}
"""


def tracepoint_source():
    preamble = r"""
typedef unsigned int uid_t;
#define CONFIG_KSU_SUPERKEY 1
#define PER_USER_RANGE 100000
#define WEBVIEW_ZYGOTE_UID 1053
#define KSU_INVALID_UID ((uid_t)-1)
#define READ_ONCE(value) (value)
#define unlikely(value) (value)
#define pr_info(...) ((void)0)
#define pr_warn(...) ((void)0)
#define SIGKILL 9
#define __NR_reboot 142
#define SECCOMP_MODE_FILTER 2
static uid_t ksu_manager_package_appid = 10001;
static bool key_set = true, authenticated, allowed;
static int descriptors, reboot_grants, umounts, kills, irq_depth, marker_depth;
static int tracepoint_reg_lock, tracepoint_reg_count = 1;
struct sighand { int siglock; } sighand;
struct task_struct {
    struct sighand *sighand;
    struct { int mode; void *filter; } seccomp;
    uid_t uid;
    bool marked;
} task = { .sighand = &sighand, .seccomp = { 2, &sighand }, .uid = 10001 };
static struct task_struct *current = &task;
struct kuid { uid_t val; };
static struct kuid task_uid(struct task_struct *task) { return (struct kuid){task->uid}; }
static bool superkey_is_set(void) { return key_set; }
static bool ksu_is_uid_manager(uid_t uid) {
    return authenticated && uid % PER_USER_RANGE == ksu_manager_package_appid;
}
static bool is_appuid(uid_t uid) { return uid % PER_USER_RANGE >= 10000; }
static bool is_isolated_process(uid_t uid) { return uid % PER_USER_RANGE >= 90000; }
static const void *current_cred(void) { return NULL; }
static bool ksu_is_trusted_zygote(const void *cred) { (void)cred; return false; }
static bool is_ksu_domain(void) { return false; }
static bool ksu_is_allow_uid_for_current(uid_t uid) { (void)uid; return allowed; }
static void force_sig(int signal) { (void)signal; kills++; }
static void spin_lock_irq(int *lock) { (void)lock; irq_depth++; }
static void spin_unlock_irq(int *lock) { (void)lock; irq_depth--; }
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; marker_depth++; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); marker_depth--; } while (0)
static void ksu_seccomp_allow_cache(void *filter, int nr) { (void)filter; (void)nr; reboot_grants++; }
static void ksu_set_task_tracepoint_flag(struct task_struct *task) { task->marked = true; }
static void ksu_clear_task_tracepoint_flag(struct task_struct *task) { task->marked = false; }
static void ksu_install_fd(void) { descriptors++; }
static void ksu_handle_umount(uid_t old_uid, uid_t new_uid) { (void)old_uid; (void)new_uid; umounts++; }
"""
    production = "\n".join((
        function("kernel/manager/throne_tracker.c", "bool ksu_is_manager_package_uid"),
        function("kernel/manager/throne_tracker.c", "bool ksu_is_manager_auth_candidate"),
        function("kernel/hook/tp_marker.c", "void ksu_clear_task_tracepoint_flag_if_needed"),
        function("kernel/hook/setuid_hook.c", "int ksu_handle_setresuid"),
    ))
    body = r"""
int main(void) {
    /* A fresh manager process keeps syscall interception but gets neither a
     * privileged FD nor a seccomp exception before proving its credentials. */
    CHECK(ksu_handle_setresuid(0, task.uid) == 0 && task.marked);
    CHECK(!descriptors && !reboot_grants && umounts == 1);
    ksu_clear_task_tracepoint_flag_if_needed(&task);
    CHECK(task.marked);
    task.uid = 210001; task.marked = false;
    CHECK(ksu_handle_setresuid(0, task.uid) == 0 && task.marked);
    CHECK(!descriptors && !reboot_grants);

    task.uid = 10002;
    CHECK(ksu_handle_setresuid(0, task.uid) == 0 && !task.marked);
    task.uid = 10001; key_set = false; task.marked = true;
    CHECK(ksu_handle_setresuid(0, task.uid) == 0 && !task.marked);
    key_set = true; ksu_manager_package_appid = KSU_INVALID_UID;
    CHECK(ksu_handle_setresuid(0, task.uid) == 0 && !task.marked);

    ksu_manager_package_appid = 10001; authenticated = true;
    CHECK(ksu_handle_setresuid(0, task.uid) == 0 && task.marked);
    CHECK(descriptors == 1 && reboot_grants == 1);
    key_set = false; ksu_clear_task_tracepoint_flag_if_needed(&task);
    CHECK(task.marked);
    /* Another tracepoint consumer keeps its marker on unrelated processes. */
    authenticated = false; tracepoint_reg_count = 2; task.uid = 10002;
    ksu_clear_task_tracepoint_flag_if_needed(&task);
    CHECK(task.marked);
    CHECK(!kills && !irq_depth && !marker_depth);
    return 0;
}
"""
    return COMMON + preamble + production + body


def native_transport_source():
    preamble = r"""
typedef unsigned long long uint64_t;
typedef unsigned long long __u64;
typedef int __s32;
#define nullptr NULL
#define LogDebug(...) ((void)0)
#define ENOTTY 25
#define EACCES 13
#define EAGAIN 11
#define EFAULT 14
#define EBADF 9
#define KSU_IOCTL_SUPERKEY_AUTH 107
#define KSU_IOCTL_GET_INFO 2
#define KSU_PRCTL_SUPERKEY_AUTH 0x59554B49
#define KSU_INSTALL_MAGIC1 0xDEADBEEF
#define KSU_SUPERKEY_MAGIC2 0xCAFE5555
#define __NR_reboot 142
static int fd, errno, auth_calls, prctl_calls, reboot_calls, cache_resets;
static int auth_error, auth_result, info_flags;
static bool prctl_success, reboot_success;
static char submitted[65];
static char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}
static long time(void *unused) { (void)unused; return 100; }
static void usleep(unsigned int unused) { (void)unused; }
static void reset_cached_info(void) { cache_resets++; }
struct ksu_get_info_cmd { unsigned int version, flags; };
"""
    structures = "\n".join(
        function("uapi/supercall.h", "struct " + name + " {") + ";"
        for name in ("ksu_superkey_auth_cmd", "ksu_superkey_prctl_cmd",
                     "ksu_superkey_reboot_cmd")
    )
    doubles = r"""
static int ksuctl(unsigned long op, void *arg) {
    (void)op;
    if (fd < 0) { errno = EBADF; return -1; }
    auth_calls++;
    struct ksu_superkey_auth_cmd *cmd = arg;
    strncpy(submitted, cmd->superkey, sizeof(submitted));
    if (auth_error == ENOTTY) { errno = ENOTTY; return -1; }
    cmd->result = auth_result;
    errno = auth_error;
    return auth_error ? -1 : 0;
}
static int ioctl(int descriptor, unsigned long op, void *arg) {
    (void)descriptor; (void)op;
    struct ksu_get_info_cmd *cmd = arg;
    cmd->version = 1; cmd->flags = info_flags;
    return 0;
}
static long prctl(int option, struct ksu_superkey_prctl_cmd *cmd, int a, int b, int c) {
    (void)option; (void)a; (void)b; (void)c;
    prctl_calls++;
    if (prctl_success) { cmd->result = 0; cmd->fd = 7; }
    return -1; /* Original prctl still fails even when task_work succeeds. */
}
static long syscall(int nr, unsigned int a, unsigned int b, int c,
                    struct ksu_superkey_reboot_cmd *cmd) {
    (void)nr; (void)a; (void)b; (void)c;
    reboot_calls++;
    if (reboot_success) { cmd->result = 0; cmd->fd = 8; }
    return -1;
}
static void reset(void) {
    fd = 4; errno = auth_error = auth_result = 0; info_flags = 2;
    auth_calls = prctl_calls = reboot_calls = cache_resets = 0;
    prctl_success = reboot_success = false;
    memset(submitted, 0, sizeof(submitted));
}
"""
    production = function("manager/app/src/main/cpp/ksu.c", "bool authenticate_superkey(")
    body = r"""
int main(void) {
    /* Definitive rejection and throttling never retry another transport. */
    const int errors[] = { EACCES, EAGAIN, EFAULT };
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        reset(); auth_error = errors[i]; auth_result = -errors[i];
        CHECK(!authenticate_superkey("wrong-key"));
        CHECK(auth_calls == 1 && !prctl_calls && !reboot_calls);
    }
    reset(); auth_result = -EACCES;
    CHECK(!authenticate_superkey("wrong-key"));
    CHECK(auth_calls == 1 && !prctl_calls && !reboot_calls);
    reset(); CHECK(authenticate_superkey(" \t "));
    CHECK(!strcmp(submitted, " \t ") && auth_calls == 1 && cache_resets == 1);
    CHECK(!prctl_calls && !reboot_calls);
    reset(); info_flags = 0;
    CHECK(!authenticate_superkey("correct-key"));
    CHECK(auth_calls == 1 && !prctl_calls && !reboot_calls);

    /* First login has no FD and performs only the seccomp-safe prctl check. */
    reset(); fd = -1;
    CHECK(!authenticate_superkey("wrong-key"));
    CHECK(!auth_calls && prctl_calls == 1 && !reboot_calls);
    reset(); fd = -1; prctl_success = true;
    CHECK(authenticate_superkey("correct-key") && fd == 7);
    CHECK(!auth_calls && prctl_calls == 1 && !reboot_calls);

    /* Older kernels that explicitly lack the ioctl retain legacy fallbacks. */
    reset(); auth_error = ENOTTY; prctl_success = true;
    CHECK(authenticate_superkey("correct-key"));
    CHECK(auth_calls == 1 && prctl_calls == 1 && !reboot_calls);
    reset(); auth_error = ENOTTY; reboot_success = true;
    CHECK(authenticate_superkey("correct-key") && fd == 8);
    CHECK(auth_calls == 1 && prctl_calls == 1 && reboot_calls == 1);
    reset(); auth_error = ENOTTY; prctl_success = true; info_flags = 0;
    CHECK(!authenticate_superkey("correct-key"));
    CHECK(auth_calls == 1 && prctl_calls == 1 && !reboot_calls);
    reset(); CHECK(!authenticate_superkey(NULL));
    CHECK(!auth_calls && !prctl_calls && !reboot_calls);
    return 0;
}
"""
    return COMMON + preamble + structures + doubles + production + body


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler, "superkey-configuration", configuration_source())
    run_test(compiler, "superkey-signature-policy", signature_policy_source())
    run_test(compiler, "superkey-rate-limit", rate_limit_source())
    run_test(compiler, "superkey-first-login-tracepoint", tracepoint_source())
    run_test(compiler, "superkey-native-single-attempt", native_transport_source())
