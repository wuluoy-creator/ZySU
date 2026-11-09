#!/usr/bin/env python3
"""Exercise production SELinux context parsing and SID synchronization.

Policy, SID table, allocation and RCU doubles expose ordering and cleanup without
a kernel tree. These tests do not replace kernel builds or on-device validation.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test


def context_source():
    preamble = r"""
typedef unsigned int u32;
typedef unsigned int gfp_t;
#define EINVAL 22
#define ENOMEM 12
#define ESTALE 116
#define EIO 5
#define EAGAIN 11
#define SECSID_NULL 0
#define GFP_KERNEL 1
struct symtab { int policy, component; };
struct policydb { struct symtab p_users, p_roles, p_types; int id; };
struct sidtab { int id; };
struct selinux_policy { struct policydb policydb; struct sidtab *sidtab; u32 latest_granting; };
struct user_datum { u32 value; };
struct role_datum { u32 value; };
struct type_datum { u32 value; bool attribute; };
struct context { u32 user, role, type; int initialized, mls_allocated; };
static struct selinux_policy policies[3];
static struct selinux_policy *backup_sepolicy = &policies[0];
static int type_mark, single_parse;
static int ksu_type_is_original(struct policydb *db, u32 generation, u32 type) {
    (void)db; (void)generation; (void)type; return type_mark;
}
static struct sidtab sidtabs[3];
static struct user_datum users[3];
static struct role_datum roles[3];
static struct type_datum types[3];
static struct { struct selinux_policy *policy; } selinux_state;
static int missing[3][3], mls_error[3], invalid[3], sid_error[3];
static int mls_calls[3], validity_calls[3], sid_calls[3];
static int initialized, destroyed, ranges, range_frees, errors;
static int rcu_depth, locks, unlocks, reload_after_unlock, stale_once;
static int alloc_calls, free_calls, fail_alloc, in_use[2];
static char allocations[2][64], events[128];
static int event_count;
static const char input[] = "u:r:t:s0:c1";

static void event(char c) {
    if (event_count + 1 >= sizeof(events)) { errors++; return; }
    events[event_count++] = c; events[event_count] = 0;
}
static void assert_rcu(void) { if (rcu_depth != 1) errors++; }
static void rcu_read_lock(void) { if (rcu_depth++) errors++; locks++; }
static void rcu_read_unlock(void) {
    if (rcu_depth != 1) errors++;
    rcu_depth--; unlocks++;
    if (reload_after_unlock) {
        selinux_state.policy = &policies[2]; reload_after_unlock = 0;
    }
}
static struct selinux_policy *read_policy(struct selinux_policy *p) {
    assert_rcu(); return p;
}
#define rcu_dereference(p) read_policy(p)

static void context_init(struct context *ctx) {
    memset(ctx, 0, sizeof(*ctx)); ctx->initialized = 1; initialized++;
}
static void context_destroy(struct context *ctx) {
    assert_rcu();
    if (ctx->initialized != 1) errors++;
    if (ctx->mls_allocated) range_frees++;
    ctx->initialized = 0; destroyed++;
}
static void *symtab_search(struct symtab *table, const char *name) {
    static const char names[] = "urt";
    static const char order[] = "uUrRtT";
    int id = table->policy, component = table->component;
    assert_rcu(); event(order[component * 2 + (id != 0)]);
    if (name[0] != names[component] || name[1]) errors++;
    if (missing[id][component]) return NULL;
    if (component == 0) return &users[id];
    if (component == 1) return &roles[id];
    return &types[id];
}
static int mls_context_to_sid(struct policydb *pol, char delimiter,
                              char *text, struct context *ctx,
                              struct sidtab *table, u32 def_sid) {
    int id = pol->id;
    assert_rcu(); event(id ? 'M' : 'm'); mls_calls[id]++;
    if (delimiter != ':' || strcmp(text, "s0:c1") ||
        table != &sidtabs[id] || def_sid != (single_parse ? 0 : 7) || !ctx->initialized)
        errors++;
    /* The real parser mutates its buffer and may allocate before failing. */
    text[2] = 0;
    ctx->mls_allocated = 1; ranges++;
    return mls_error[id];
}
static bool policydb_context_isvalid(struct policydb *pol, struct context *ctx) {
    int id = pol->id;
    assert_rcu(); event(id ? 'V' : 'v'); validity_calls[id]++;
    return !invalid[id] && ctx->user == users[id].value &&
           ctx->role == roles[id].value && ctx->type == types[id].value &&
           !types[id].attribute;
}
static int sidtab_context_to_sid(struct sidtab *table, struct context *ctx,
                                u32 *sid) {
    int id = table->id;
    assert_rcu(); event(id ? 'S' : 's'); sid_calls[id]++;
    if (ctx->user != users[id].value || ctx->role != roles[id].value ||
        ctx->type != types[id].value || !ctx->mls_allocated)
        errors++;
    if (id == 1 && stale_once) {
        stale_once = 0; reload_after_unlock = 1; return -ESTALE;
    }
    if (sid_error[id]) return sid_error[id];
    *sid = 100 + id; return 0;
}
static char *kmemdup_nul(const char *src, size_t len, gfp_t flags) {
    if (rcu_depth || flags != GFP_KERNEL) errors++;
    alloc_calls++;
    if (alloc_calls == fail_alloc) return NULL;
    for (int i = 0; i < 2; i++) {
        if (in_use[i]) continue;
        if (len >= sizeof(allocations[i])) { errors++; return NULL; }
        in_use[i] = 1;
        memcpy(allocations[i], src, len); allocations[i][len] = 0;
        return allocations[i];
    }
    errors++; return NULL;
}
static void kfree(void *ptr) {
    if (!ptr) return;
    if (rcu_depth) errors++;
    for (int i = 0; i < 2; i++) {
        if (ptr != allocations[i]) continue;
        if (!in_use[i]) errors++;
        in_use[i] = 0; free_calls++; return;
    }
    errors++;
}
static void reset(void) {
    memset(missing, 0, sizeof(missing)); memset(mls_error, 0, sizeof(mls_error));
    memset(invalid, 0, sizeof(invalid)); memset(sid_error, 0, sizeof(sid_error));
    memset(mls_calls, 0, sizeof(mls_calls));
    memset(validity_calls, 0, sizeof(validity_calls));
    memset(sid_calls, 0, sizeof(sid_calls));
    memset(in_use, 0, sizeof(in_use)); memset(events, 0, sizeof(events));
    event_count = initialized = destroyed = ranges = range_frees = errors = 0;
    rcu_depth = locks = unlocks = reload_after_unlock = stale_once = 0;
    alloc_calls = free_calls = fail_alloc = 0;
    type_mark = -1; single_parse = 0;
    for (int id = 0; id < 3; id++) {
        policies[id].policydb = (struct policydb){
            {id, 0}, {id, 1}, {id, 2}, id
        };
        sidtabs[id].id = id; policies[id].sidtab = &sidtabs[id];
        users[id].value = 10 + id;
        roles[id].value = 20 + id;
        types[id] = (struct type_datum){30 + id, false};
    }
    selinux_state.policy = &policies[1];
}
static bool clean(void) {
    return !errors && !rcu_depth && locks == unlocks &&
           initialized == destroyed && ranges == range_frees &&
           !in_use[0] && !in_use[1];
}
"""
    body = r"""
static int convert(u32 *sid, u32 *live_sid, int *live_rc) {
    return security_context_to_sid_with_policy(
        &policies[0], input, sizeof(input) - 1, sid, 7, GFP_KERNEL,
        live_sid, live_rc);
}
int main(void) {
    u32 sid, live_sid;
    int live_rc;

    /* Check each policy's values directly; the numeric IDs are independent. */
    reset();
    char backup_text[sizeof(input)], live_text[sizeof(input)];
    struct context backup_ctx, live_ctx;
    memcpy(backup_text, input, sizeof(input));
    memcpy(live_text, input, sizeof(input));
    rcu_read_lock();
    CHECK(!string_to_context_struct(&policies[0].policydb,
          &policies[1].policydb, &sidtabs[0], &sidtabs[1], backup_text,
          input, sizeof(input) - 1, &backup_ctx, &live_ctx, 1, 7, &live_rc));
    CHECK(!live_rc && backup_ctx.type == 30 && live_ctx.type == 31);
    CHECK(backup_ctx.user == 10 && live_ctx.user == 11);
    CHECK(backup_ctx.role == 20 && live_ctx.role == 21);
    CHECK(!strcmp(events, "uUrRtTmMVv"));
    context_destroy(&backup_ctx); context_destroy(&live_ctx);
    rcu_read_unlock();
    CHECK(clean());

    reset();
    CHECK(!convert(&sid, &live_sid, &live_rc));
    CHECK(sid == 100 && live_sid == 101 && !live_rc);
    CHECK(!strcmp(events, "uUrRtTmMVvsS"));
    CHECK(alloc_calls == 1 && free_calls == 1 && clean());
    CHECK(!strcmp(input, "u:r:t:s0:c1"));

    /* Unknown backup symbols fail before MLS; no SID may be published. */
    static const char *const missing_events[] = {"uUrRtT", "uUrRtT", "uUrRt"};
    for (int component = 0; component < 3; component++) {
        reset(); missing[0][component] = 1;
        sid = live_sid = 999; live_rc = 999;
        CHECK(convert(&sid, &live_sid, &live_rc) == -EINVAL);
        CHECK(!sid && !live_sid && live_rc == -EINVAL);
        CHECK(!strcmp(events, missing_events[component]));
        CHECK(!mls_calls[0] && !mls_calls[1]);
        CHECK(!sid_calls[0] && !sid_calls[1] && clean());
    }
    reset(); types[0].attribute = true;
    CHECK(convert(&sid, &live_sid, &live_rc) == -EINVAL);
    CHECK(!strcmp(events, "uUrRt") && clean());

    /* A live-only type cannot enter either MLS parser or either SID table. */
    reset(); missing[0][2] = 1; mls_error[1] = -EIO;
    CHECK(convert(&sid, &live_sid, &live_rc) == -EINVAL);
    CHECK(!mls_calls[0] && !mls_calls[1] && live_rc == -EINVAL);
    CHECK(!sid_calls[0] && !sid_calls[1] && clean());

    /* Invalid live symbols cannot be confused with backup numeric IDs. */
    for (int component = 0; component < 3; component++) {
        reset(); missing[1][component] = 1;
        CHECK(!convert(&sid, &live_sid, &live_rc));
        CHECK(sid == 100 && !live_sid && live_rc == -EINVAL);
        CHECK(sid_calls[0] == 1 && !sid_calls[1] && clean());
    }
    reset(); types[1].attribute = true;
    CHECK(!convert(&sid, &live_sid, &live_rc));
    CHECK(sid == 100 && !live_sid && live_rc == -EINVAL && clean());

    /* Keep the live MLS error; validity must not overwrite it with success. */
    reset(); mls_error[1] = -EIO;
    CHECK(!convert(&sid, &live_sid, &live_rc));
    CHECK(sid == 100 && !live_sid && live_rc == -EIO);
    CHECK(validity_calls[0] == 1 && !validity_calls[1]);
    CHECK(ranges == 2 && !sid_calls[1] && clean());

    /* Even a backup MLS failure performs the paired live MLS operation. */
    reset(); mls_error[0] = -EINVAL;
    CHECK(convert(&sid, &live_sid, &live_rc) == -EINVAL);
    CHECK(!strcmp(events, "uUrRtTmMV"));
    CHECK(!sid && !live_sid && ranges == 2 && clean());

    /* An invalid backup cannot publish a live SID or leak either MLS range. */
    reset(); invalid[0] = 1;
    CHECK(convert(&sid, &live_sid, &live_rc) == -EINVAL);
    CHECK(!strcmp(events, "uUrRtTmMVv"));
    CHECK(!sid && !live_sid && !sid_calls[0] && !sid_calls[1]);
    CHECK(ranges == 2 && clean());
    reset(); invalid[1] = 1;
    CHECK(!convert(&sid, &live_sid, &live_rc));
    CHECK(sid == 100 && !live_sid && live_rc == -EINVAL && clean());

    /* No parsing, RCU entry or retained allocation on empty/allocation errors. */
    reset(); sid = live_sid = 999; live_rc = 999;
    CHECK(security_context_to_sid_with_policy(&policies[0], input, 0,
          &sid, 7, GFP_KERNEL, &live_sid, &live_rc) == -EINVAL);
    CHECK(!sid && !live_sid && live_rc == -EINVAL);
    CHECK(!alloc_calls && !locks && clean());
    for (int failure = 1; failure <= 1; failure++) {
        reset(); fail_alloc = failure; sid = live_sid = 999; live_rc = 999;
        CHECK(convert(&sid, &live_sid, &live_rc) == -ENOMEM);
        CHECK(!sid && !live_sid && live_rc == -EINVAL);
        CHECK(alloc_calls == failure && free_calls == failure - 1);
        CHECK(!locks && !initialized && clean());
    }

    reset(); sid_error[0] = -ENOMEM;
    CHECK(convert(&sid, &live_sid, &live_rc) == -ENOMEM);
    CHECK(!sid && !live_sid && !sid_calls[1] && live_rc == -ENOMEM);
    CHECK(ranges == 2 && clean());
    reset(); sid_error[1] = -ENOMEM;
    CHECK(!convert(&sid, &live_sid, &live_rc));
    CHECK(sid == 100 && !live_sid && live_rc == -ENOMEM);
    CHECK(ranges == 2 && clean());

    /* The modified string must be restored before retry under a new RCU read.
     * The replacement live policy uses new IDs and a different SID table. */
    reset(); stale_once = 1;
    CHECK(!convert(&sid, &live_sid, &live_rc));
    CHECK(sid == 100 && live_sid == 102 && !live_rc);
    CHECK(!strcmp(events, "uUrRtTmMVvsSuUrRtTmMVvsS"));
    CHECK(sid_calls[0] == 2 && sid_calls[1] == 1 && sid_calls[2] == 1);
    CHECK(locks == 2 && unlocks == 2 && initialized == 4 && destroyed == 4);
    CHECK(alloc_calls == 1 && free_calls == 1 && ranges == 4 && clean());

    /* Optional live outputs do not suppress synchronization or retry. */
    reset(); stale_once = 1;
    CHECK(!convert(&sid, NULL, NULL));
    CHECK(sid == 100 && sid_calls[2] == 1 && locks == 2 && clean());

    /* Stock values are stable across our policy copies: parse once, publish
     * distinct backup/live SIDs, and release the sole MLS allocation. */
    reset(); single_parse = 1; type_mark = 1;
    users[1] = users[2] = users[0]; roles[1] = roles[2] = roles[0];
    types[1] = types[2] = types[0];
    CHECK(!security_context_to_sid_live(input, sizeof(input) - 1, &live_sid));
    CHECK(live_sid == 101 && !strcmp(events, "urtmvVsS"));
    CHECK(mls_calls[0] == 1 && !mls_calls[1] && ranges == 1);
    CHECK(alloc_calls == 1 && free_calls == 1 && clean());

    /* Stale metadata requests the policy-local fallback before MLS/SID work. */
    for (int mark = -1; mark <= 0; mark++) {
        reset(); single_parse = 1; type_mark = mark;
        CHECK(security_context_to_sid_live(input, sizeof(input) - 1,
              &live_sid) == -EAGAIN);
        CHECK(!mls_calls[0] && !sid_calls[0] && !sid_calls[1] && clean());
    }
    reset(); single_parse = 1; type_mark = 1; missing[0][2] = 1;
    CHECK(security_context_to_sid_live(input, sizeof(input) - 1,
          &live_sid) == -EINVAL);
    CHECK(!mls_calls[0] && !sid_calls[0] && !sid_calls[1] && clean());

    /* Partial MLS allocations and SID errors keep ownership balanced. */
    reset(); single_parse = 1; type_mark = 1; mls_error[0] = -ENOMEM;
    CHECK(security_context_to_sid_live(input, sizeof(input) - 1,
          &live_sid) == -ENOMEM);
    CHECK(ranges == 1 && !sid_calls[0] && clean());
    reset(); single_parse = 1; type_mark = 1;
    users[1] = users[2] = users[0]; roles[1] = roles[2] = roles[0];
    types[1] = types[2] = types[0]; sid_error[0] = -EIO;
    CHECK(security_context_to_sid_live(input, sizeof(input) - 1,
          &live_sid) == -EIO);
    CHECK(sid_calls[0] == 1 && !sid_calls[1] && clean());

    /* Single parsing also restores input after live SID-table rollover. */
    reset(); single_parse = 1; type_mark = 1; stale_once = 1;
    users[1] = users[2] = users[0]; roles[1] = roles[2] = roles[0];
    types[1] = types[2] = types[0];
    CHECK(!security_context_to_sid_live(input, sizeof(input) - 1, &live_sid));
    CHECK(live_sid == 102 && sid_calls[0] == 2 && sid_calls[2] == 1);
    CHECK(ranges == 2 && locks == 2 && alloc_calls == 1 && clean());
    return 0;
}
"""
    path = "kernel/feature/selinux_hide.c"
    source = (ROOT / path).read_text(encoding="utf-8")
    handlers = []
    for name in ("string_to_context_struct", "security_context_to_sid_with_policy",
                 "backup_string_to_context_struct", "security_context_to_sid_live"):
        # Include the opening brace in the marker to skip forward declarations.
        definition = re.search(r"static int\s+" + name + r"\([^;{}]*\)\s*\{", source)
        if definition is None:
            raise ValueError(f"Definition not found: {name}")
        handlers.append(function(path, definition.group(0)))
    return COMMON + preamble + "\n".join(handlers) + body


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler, "selinux-hide-context", context_source())
