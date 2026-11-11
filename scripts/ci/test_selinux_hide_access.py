#!/usr/bin/env python3
"""Exercise SELinux access transactions using the production parser and handler.

Kernel API doubles check bounded tokenization, syscall error ordering, and
in-place buffer ownership. The class scanner is a double: these tests verify
that its input and %hu format stay intact, not the kernel's integer parser.
"""

import argparse
import os
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, function, run_test


def access_source(minor):
    version = f"#define LINUX_VERSION_CODE KERNEL_VERSION(6, {minor}, 0)\n"
    version += "#define KERNEL_VERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))\n"
    preamble = r"""
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned int gfp_t;
#define likely(x) (x)
#define EINVAL 22
#define EACCES 13
#define ENOMEM 12
#define GFP_KERNEL 1
#define SECSID_NULL 0
#define SECINITSID_SECURITY 2
#define SECCLASS_SECURITY 3
#define SECURITY__COMPUTE_AV 4
#define SIMPLE_TRANSACTION_LIMIT 256
struct file { int unused; };
struct selinux_policy { int unused; };
struct av_decision { u32 allowed, auditallow, auditdeny, seqno, flags; };
static struct file request_file;
static struct selinux_policy backup, *backup_sepolicy = &backup;
static int fake_state;
static struct { u32 val; } uid;
#define current_uid() uid
static char request[SIMPLE_TRANSACTION_LIMIT + 2];
static char original_request[SIMPLE_TRANSACTION_LIMIT + 2];
static size_t request_size;
static int errors, orig_calls, perm_calls, scan_calls, parse_calls, compute_calls, format_calls;
static int perm_error, scan_result, context_errors[2];
static const char *expected_contexts[2], *expected_class_text;
static u16 expected_class;

static int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
static u32 current_sid(void) { return 17; }
static int ksu_avc_has_perm(u32 source, u32 target, u32 tclass, u32 perm, void *audit) {
    if (source != 17 || target != SECINITSID_SECURITY || tclass != SECCLASS_SECURITY ||
        perm != SECURITY__COMPUTE_AV || audit || scan_calls || parse_calls)
        errors++;
    perm_calls++;
    return perm_error;
}
static ssize_t orig_access_write(struct file *file, char *buf, size_t size) {
    if (file != &request_file || buf != request || size != request_size ||
        perm_calls || scan_calls || parse_calls)
        errors++;
    orig_calls++;
    return 123;
}
static int sscanf(const char *text, const char *format, u16 *tclass) {
    if (perm_calls != 1 || parse_calls || strcmp(format, "%hu") ||
        text < request || text >= request + request_size ||
        strcmp(text, expected_class_text))
        errors++;
    scan_calls++;
    *tclass = expected_class;
    return scan_result;
}
static int security_context_to_sid_with_policy(struct selinux_policy *policy,
    const char *text, u32 len, u32 *sid, u32 def_sid, gfp_t flags,
    u32 *live_sid, int *live_rc)
{
    int index = parse_calls++;
    if (index > 1) { errors++; return -EINVAL; }
    if (policy != &backup || def_sid || flags != GFP_KERNEL || live_sid || live_rc ||
        perm_calls != 1 || scan_calls != 1 || text < request ||
        text + len >= request + request_size || text[len] ||
        len != strlen(expected_contexts[index]) || strcmp(text, expected_contexts[index]))
        errors++;
    if (context_errors[index]) return context_errors[index];
    *sid = 31 + index;
    return 0;
}
static void compute(u32 source, u32 target, u16 tclass, struct av_decision *avd) {
    if (source != 31 || target != 32 || tclass != expected_class || parse_calls != 2)
        errors++;
    compute_calls++;
    *avd = (struct av_decision){1, 2, 3, 99, 4};
}
static void security_compute_av_user_with_policy(struct selinux_policy *policy,
    u32 source, u32 target, u16 tclass, struct av_decision *avd)
{
    if (policy != &backup) errors++;
    compute(source, target, tclass, avd);
}
static void security_compute_av_user(int *state,
    u32 source, u32 target, u16 tclass, struct av_decision *avd)
{
    if (state != &fake_state) errors++;
    compute(source, target, tclass, avd);
}
static int scnprintf(char *buf, size_t size, const char *format,
    u32 allowed, u32 decided, u32 auditallow, u32 auditdeny, u32 seqno, u32 flags)
{
    if (buf != request || size != SIMPLE_TRANSACTION_LIMIT || compute_calls != 1 ||
        strcmp(format, "%x %x %x %x %u %x") || allowed != 1 || decided != 0xffffffff ||
        auditallow != 2 || auditdeny != 3 || seqno != 1 || flags != 4)
        errors++;
    format_calls++;
    memcpy(buf, "response", 9);
    return 8;
}
static void reset(const char *text, size_t size) {
    memset(request, 0x5a, sizeof(request));
    memcpy(request, text, size); request[size] = 0;
    memcpy(original_request, request, sizeof(request));
    request_size = size;
    errors = orig_calls = perm_calls = scan_calls = parse_calls = compute_calls = format_calls = 0;
    perm_error = context_errors[0] = context_errors[1] = 0;
    scan_result = 1; expected_class = 7;
    expected_contexts[0] = "u:r:s:s0"; expected_contexts[1] = "u:r:t:s0";
    expected_class_text = "7";
    uid.val = 10000;
}
static bool intact(void) {
    for (size_t i = 0; i < sizeof(request); i++)
        if (request[i] != original_request[i]) return false;
    return true;
}
static bool guarded(void) {
    return request[request_size] == 0 && request[request_size + 1] == 0x5a;
}
"""
    handlers = "\n".join(
        function("kernel/feature/selinux_hide.c", marker)
        for marker in ("static char *selinux_access_context", "static ssize_t my_write_access")
    )
    body = r"""
int main(void) {
    const char valid[] = "u:r:s:s0 u:r:t:s0 7";
    const char *malformed[] = {"", " \t\n\r\v\f", "u:r:s:s0", "u:r:s:s0 ",
                              "u:r:s:s0 u:r:t:s0", "u:r:s:s0 u:r:t:s0 "};

    reset(valid, sizeof(valid) - 1); uid.val = 9999;
    CHECK(my_write_access(&request_file, request, request_size) == 123);
    CHECK(orig_calls == 1 && !perm_calls && !scan_calls && !errors && intact());

    /* Permission failure wins over malformed input and never mutates it. */
    reset("bad", 3); perm_error = -EACCES;
    CHECK(my_write_access(&request_file, request, request_size) == -EACCES);
    CHECK(perm_calls == 1 && !scan_calls && !parse_calls && !errors && intact());

    reset(valid, sizeof(valid) - 1);
    CHECK(my_write_access(&request_file, request, request_size) == 8);
    CHECK(parse_calls == 2 && compute_calls == 1 && format_calls == 1);
    CHECK(!strcmp(request, "response") && guarded() && !errors);

    /* All six kernel whitespace characters separate tokens; repeated space
     * before the numeric field must still reach sscanf unchanged. */
    const char whitespace[] = " \t\n\r\f\v";
    for (size_t i = 0; i < sizeof(whitespace) - 1; i++) {
        char text[] = " u:r:s:s0 u:r:t:s0 7";
        for (size_t j = 0; j < sizeof(text) - 1; j++)
            if (text[j] == ' ') text[j] = whitespace[i];
        reset(text, sizeof(text) - 1);
        CHECK(my_write_access(&request_file, request, request_size) == 8);
        CHECK(parse_calls == 2 && !errors && guarded());
    }
    const char repeated[] = " \tu:r:s:s0\n\r u:r:t:s0\t \v7 trailing";
    reset(repeated, sizeof(repeated) - 1); expected_class_text = " \v7 trailing";
    CHECK(my_write_access(&request_file, request, request_size) == 8);
    CHECK(!errors && guarded());

    /* Preserve the numeric suffix, signs, overflow and trailing-data handling
     * by passing the original field to the existing kernel %hu scanner. */
    const char numeric[] = "u:r:s:s0 u:r:t:s0 +65536tail";
    reset(numeric, sizeof(numeric) - 1); expected_class_text = "+65536tail";
    expected_class = 0;
    CHECK(my_write_access(&request_file, request, request_size) == 8);
    CHECK(!errors && guarded());
    const char invalid_class[] = "u:r:s:s0 u:r:t:s0 bad";
    reset(invalid_class, sizeof(invalid_class) - 1);
    expected_class_text = "bad"; scan_result = 0;
    CHECK(my_write_access(&request_file, request, request_size) == -EINVAL);
    CHECK(scan_calls == 1 && !parse_calls && !compute_calls && !errors && guarded());

    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        reset(malformed[i], strlen(malformed[i]));
        CHECK(my_write_access(&request_file, request, request_size) == -EINVAL);
        CHECK(!scan_calls && !parse_calls && !compute_calls && !errors && guarded());
    }

    /* Embedded NUL ends the request even if further tokens follow it. */
    const char nul_source[] = "u:r:s:s0\0 u:r:t:s0 7";
    reset(nul_source, sizeof(nul_source) - 1);
    CHECK(my_write_access(&request_file, request, request_size) == -EINVAL);
    CHECK(!scan_calls && !parse_calls && !errors && guarded());
    const char nul_target[] = "u:r:s:s0 u:r:t:s0\0 7";
    reset(nul_target, sizeof(nul_target) - 1);
    CHECK(my_write_access(&request_file, request, request_size) == -EINVAL);
    CHECK(!scan_calls && !parse_calls && !errors && guarded());
    const char nul_class[] = "u:r:s:s0 u:r:t:s0 \0 7";
    reset(nul_class, sizeof(nul_class) - 1);
    expected_class_text = ""; scan_result = 0;
    CHECK(my_write_access(&request_file, request, request_size) == -EINVAL);
    CHECK(scan_calls == 1 && !parse_calls && !errors && guarded());
    const char nul_trailing[] = "u:r:s:s0 u:r:t:s0 7\0 extra";
    reset(nul_trailing, sizeof(nul_trailing) - 1);
    CHECK(my_write_access(&request_file, request, request_size) == 8);
    CHECK(parse_calls == 2 && !errors && guarded());

    /* Keep conversion errors and source-before-target precedence unchanged. */
    reset(valid, sizeof(valid) - 1); context_errors[0] = -EINVAL; context_errors[1] = -ENOMEM;
    CHECK(my_write_access(&request_file, request, request_size) == -EINVAL);
    CHECK(parse_calls == 1 && !compute_calls && !format_calls && !errors && guarded());
    reset(valid, sizeof(valid) - 1); context_errors[1] = -ENOMEM;
    CHECK(my_write_access(&request_file, request, request_size) == -ENOMEM);
    CHECK(parse_calls == 2 && !compute_calls && !format_calls && !errors && guarded());

    /* No delimiter in a maximum-sized request, or at a truncated boundary,
     * may make the token helper inspect/mutate bytes outside its input. */
    static char long_token[SIMPLE_TRANSACTION_LIMIT];
    memset(long_token, 'x', sizeof(long_token));
    reset(long_token, sizeof(long_token) - 1);
    CHECK(my_write_access(&request_file, request, request_size) == -EINVAL);
    CHECK(!scan_calls && !parse_calls && !errors && intact());
    char bounded[] = {'u', ':', 'r', ' ', 'Z'};
    char *cursor = bounded;
    u32 len = 999;
    CHECK(!selinux_access_context(&cursor, bounded + 3, &len));
    CHECK(cursor == bounded && len == 999 && bounded[3] == ' ' && bounded[4] == 'Z');
    cursor = bounded + 3;
    CHECK(!selinux_access_context(&cursor, bounded + 3, &len));
    CHECK(len == 999 && bounded[3] == ' ' && bounded[4] == 'Z');
    return 0;
}
"""
    return COMMON + version + preamble + handlers + body


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    for minor in (1, 6):
        run_test(compiler, f"selinux-hide-access-6.{minor}", access_source(minor))
