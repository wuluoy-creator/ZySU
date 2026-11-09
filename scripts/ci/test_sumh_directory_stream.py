"""Exercise directory stream pagination and composed views with real C bodies."""

import argparse
import os
from pathlib import Path
import re
import shutil

from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test


def source():
    preamble = r"""
typedef long long loff_t;
typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
#define EINVAL 22
#define ENOTDIR 20
#define ENOENT 2
#define EIO 5
#define ENOMEM 12
#define GFP_ATOMIC 0
#define GFP_KERNEL 0
#define NAME_MAX 255
#define DT_DIR 4
#define DT_LNK 10
#define DT_REG 8
#define SUMH_VNODE_F_DIR 1
#define SUMH_VNODE_F_LNK 2
#define SUMH_VNODE_F_SU 4
#define SUMH_DH_POS_SIG 0x6B6DULL
#define SUMH_DH_EMIT_BATCH 8
#define SUMH_MAGIC_POS 0x1000000000000000ULL
#define SUMH_ITERATE_PATH_BUF 128
#define SUMH_MAX_MERGE_TARGETS 8
#define SUMH_HASH_BITS 4
#define AS_FLAGS_SUMH_DIR_HAS_HIDDEN 0
#define AS_FLAGS_SUMH_DIR_HAS_INJECT 1
#define AS_FLAGS_SUMH_HIDE 2
#define READ_ONCE(v) (v)
#define smp_load_acquire(p) (*(p))
#define unlikely(v) (v)
#define container_of(p, t, m) ((t *)((char *)(p) - __builtin_offsetof(t, m)))
#define ERR_PTR(v) ((void *)(ssize_t)(v))
#define IS_ERR(v) ((ssize_t)(v) < 0)
#define IS_ERR_OR_NULL(v) (!(v) || IS_ERR(v))
enum sumh_policy_scope { SUMH_POLICY_SCOPE_NONE, SUMH_POLICY_SCOPE_VIEW, SUMH_POLICY_SCOPE_SPOOF };
struct mapping { unsigned long flags; };
struct inode { struct mapping *i_mapping; };
struct qstr { const char *name; int len; };
#define QSTR_INIT(n, l) { n, l }
struct dentry { struct inode *inode; struct qstr d_name; int refs; };
struct path { struct dentry *dentry; };
struct dir_context {
    bool (*actor)(struct dir_context *, const char *, int, loff_t, u64, unsigned int);
    loff_t pos;
};
struct file;
struct file_operations { int (*iterate_shared)(struct file *, struct dir_context *); };
struct file { struct path f_path; const struct file_operations *f_op; };
struct list_head { struct list_head *next, *prev; };
#define INIT_LIST_HEAD(h) ((h)->next = (h)->prev = (h))
static void list_add_tail(struct list_head *n, struct list_head *h) {
    n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n;
}
static void list_del(struct list_head *n) { n->prev->next = n->next; n->next->prev = n->prev; }
#define list_for_each_entry_safe(p, nextp, h, m) \
    for (p = container_of((h)->next, __typeof__(*p), m), \
         nextp = container_of(p->m.next, __typeof__(*p), m); &p->m != (h); \
         p = nextp, nextp = container_of(p->m.next, __typeof__(*p), m))
struct sumh_name_list { char *name; u64 ino; unsigned int type; struct list_head list; };
struct hlist_node { void *next; };
struct hlist_head { void *first; };
struct sumh_inject_entry { const char *dir; struct hlist_node node; };
static struct hlist_head sumh_inject_dirs[1];
struct sumh_merge_entry { const char *src, *resolved_src; struct dentry *target_dentry; };
typedef int (*sumh_fop_iterate_client_fn)(struct file *, struct dir_context *, const struct file_operations *, void *);
struct sumh_fop_template { const struct file_operations *orig_fop; struct file_operations shadow_fop; };
struct sumh_fop_meta { struct sumh_fop_template *template; sumh_fop_iterate_client_fn iterate_client; void *iterate_client_data; };
struct sumh_dh_child { struct hlist_node node; unsigned long v_ino; u16 name_len; u8 flags; unsigned int hide_users; bool legacy; u8 hide, lookup_only; char name[NAME_MAX + 1]; };
struct sumh_dh_dir { struct inode *dir_inode; struct hlist_head children; bool retiring; };
struct sumh_dh_iop { struct sumh_dh_dir *dir; };
static struct sumh_dh_child exact_child = { .v_ino = 999, .name_len = 5, .legacy = true, .name = "exact" };
static struct sumh_dh_dir dh_dir;
static struct sumh_dh_iop dh_iop = { &dh_dir };
static struct sumh_fop_meta meta;
static struct mapping parent_mapping, child_mapping;
static struct inode parent_inode = { &parent_mapping }, target_inode = { &child_mapping };
static struct dentry parent = { &parent_inode, { "dir", 3 }, 1 };
static struct dentry target = { &target_inode, { "target", 6 }, 1 };
static struct dentry positive = { &target_inode, { "dup", 3 }, 1 };
static struct dentry negative = { NULL, { "negative", 8 }, 1 };
static struct sumh_merge_entry merge = { "/dir", NULL, &target };
static int sumh_ioctl_tgid, sumh_rule_count = 1, sumh_enabled = 1, sumh_stealth_enabled;
static int sumh_filldir_cache, current, sumh_fop_client_srcu, sumh_dh_srcu;
static int rcu_depth, srcu_depth, allocations, releases, freed_names, path_calls;
static int native_calls, client_calls, reschedules, bad_position, native_error;
static int native_count, injection_count, native_batch, have_client, allocation_fails;
static bool su_enabled, su_visible;
static const char *native_names[16], *injected_names[16];
static struct sumh_name_list injected[16];
static struct { int filldir_injected, filldir_hidden, iterate_fop_entries, iterate_fop_wrapped; } sumh_hook_stats;
#define atomic_long_read(p) (*(p))
#define atomic_read(p) (*(p))
#define atomic64_inc(p) (++*(p))
#define task_tgid_vnr(p) (17)
#define test_bit(bit, flags) ((*(flags) >> (bit)) & 1)
#define hash_min(hash, bits) (0)
#define full_name_hash(p, name, len) ((u32)(len))
#define hlist_for_each_entry_rcu(ie, table, member) for ((ie) = (table)->first; (ie); (ie) = (ie)->member.next)
#define hash_for_each_rcu(table, bucket, me, node) for ((bucket) = 0, (me) = &merge; (bucket) < 1; (bucket)++)
static void rcu_read_lock(void) { rcu_depth++; }
static void rcu_read_unlock(void) { rcu_depth--; }
static int srcu_read_lock(int *s) { (void)s; srcu_depth++; return 0; }
static void srcu_read_unlock(int *s, int i) { (void)s; (void)i; srcu_depth--; }
static struct dentry *dget(struct dentry *d) { d->refs++; return d; }
static void dput(struct dentry *d) { d->refs--; }
static struct inode *d_inode(struct dentry *d) { return d->inode; }
static struct inode *file_inode(struct file *f) { return d_inode(f->f_path.dentry); }
static int sumh_policy_current_scope(void) { return SUMH_POLICY_SCOPE_VIEW; }
static bool sumh_policy_current_is_hide_target(const struct inode *i) { (void)i; return true; }
static bool sumh_is_merge_context(struct dir_context *ctx) { (void)ctx; return false; }
static bool sumh_dirhijack_hidden(struct inode *i, const char *name, int len) {
    (void)i; (void)len; return !strcmp(name, "hidden");
}
static bool sumh_dirhijack_enabled(void) { return true; }
static bool sumh_policy_current_is_view_target(void) { return true; }
static bool ksu_sucompat_vfs_enabled(void) { return false; }
static bool ksu_sucompat_vfs_visible(void) { return su_visible; }
static bool ksu_sucompat_vfs_current_ino(unsigned long ino) { (void)ino; return su_enabled; }
static struct sumh_dh_iop *sumh_dh_iop_of(struct inode *i) { (void)i; return have_client ? &dh_iop : NULL; }
static struct sumh_dh_child *sumh_dh_find_child(struct sumh_dh_dir *dir, const char *name, u16 len, u32 hash) {
    (void)hash;
    struct sumh_dh_child *c;
    hlist_for_each_entry_rcu(c, &dir->children, node) {
        if (c->name_len == len && !strncmp(c->name, name, len)) return c;
    }
    return NULL;
}
static struct dentry *lookup(struct dentry *d, const struct qstr *q) {
    if (d != &target) return NULL;
    if (!strcmp(q->name, "dup") || !strcmp(q->name, "exact")) return dget(&positive);
    if (!strcmp(q->name, "negative")) return dget(&negative);
    return NULL;
}
static struct dentry *(*sumh_d_hash_and_lookup)(struct dentry *, const struct qstr *) = lookup;
static char *absolute_path(const struct path *p, char *buf, size_t size) {
    (void)p; (void)size; path_calls++; memcpy(buf, "/dir", 5); return buf;
}
static char *(*sumh_d_absolute_path)(const struct path *, char *, size_t) = absolute_path;
static char *dentry_path_raw(struct dentry *d, char *buf, size_t size) {
    (void)d; (void)buf; (void)size; return ERR_PTR(-ENOENT);
}
static void kfree(void *p) { (void)p; freed_names++; }
static void cond_resched(void) { reschedules++; }
static struct sumh_fop_meta *sumh_fop_lookup_rcu(struct inode *i) { (void)i; return &meta; }
static void sumh_populate_injected_list(const char *path, struct dentry *d, struct list_head *head) {
    (void)path; (void)d;
    for (int i = 0; i < injection_count; i++) {
        injected[i].name = (char *)injected_names[i];
        injected[i].ino = i + 10; injected[i].type = 8;
        list_add_tail(&injected[i].list, head);
    }
}
"""
    wrapper = function("kernel/sumh/internal/sumh_types.h", "struct sumh_filldir_wrapper {") + ";\n"
    allocation = r"""
static struct sumh_filldir_wrapper allocation;
static void *kmem_cache_zalloc(int cache, int flags) {
    (void)cache; (void)flags;
    if (allocation_fails) return NULL;
    allocations++; memset(&allocation, 0, sizeof(allocation)); return &allocation;
}
static void kmem_cache_free(int cache, void *p) { (void)cache; (void)p; releases++; }
"""
    dh_path = "kernel/sumh/core/sumh_dirhijack.c"
    dh = "\n".join(function(dh_path, marker) for marker in [
        "static inline bool sumh_dh_virtual_pos(",
        "static inline loff_t sumh_dh_pack_pos(",
        "static inline u32 sumh_dh_unpack_pos(",
        "bool sumh_dirhijack_injected_context(",
        "static bool sumh_dh_control_current(",
        "static bool sumh_dh_current_sees(",
        "static bool sumh_dh_current_hides(",
        "static bool sumh_dh_current_targets(",
        "static bool sumh_dh_child_emitted(",
        "bool sumh_dirhijack_emits(",
    ])
    dh += "\n" + function(dh_path, "struct sumh_dh_proxy {") + ";\n"
    dh += function(dh_path, "struct sumh_dh_emit_entry {") + ";\n"
    dh += r"""
static struct sumh_dh_emit_entry emit_allocation[SUMH_DH_EMIT_BATCH];
static void *kmalloc_array(size_t count, size_t size, int flags) {
    (void)count; (void)size; (void)flags; return emit_allocation;
}
static bool dir_emit(struct dir_context *ctx, const char *name, int len, u64 ino, unsigned int type) {
    return ctx->actor(ctx, name, len, ctx->pos, ino, type);
}
"""
    dh += "\n".join(function(dh_path, marker) for marker in [
        "static bool SUMH_NOCFI sumh_dh_proxy_actor(",
        "static int sumh_dh_emit_children(",
        "static int SUMH_NOCFI sumh_dh_iterate_inner(",
    ])
    hooks = "kernel/sumh/hooks/sumh_vfs_hooks.c"
    bodies = "\n".join(function(hooks, marker) for marker in [
        "static bool sumh_iterate_injected_pos(",
        "void sumh_iterate_rewind_injected(",
        "void sumh_iterate_emit_injected(",
        "SUMH_NOCFI bool sumh_filldir_filter(",
        "SUMH_NOCFI struct sumh_filldir_wrapper *",
        "void sumh_iterate_finish_wrapper(",
    ])
    fop_path = "kernel/sumh/features/sumh_fop_override.c"
    marker = re.search(r"SUMH_NOCFI static int sumh_shadow_iterate_shared\([^;{}]*\)\s*\{", (ROOT / fop_path).read_text()).group(0)
    bodies += "\n" + function(fop_path, marker)
    tests = r"""
#define CLIENT_POS 0x6B6D000000000000LL
static int native_iterate(struct file *f, struct dir_context *ctx) {
    (void)f; native_calls++;
    if (native_error) return native_error;
    if (ctx->pos < 0 || ctx->pos > native_count) { bad_position++; return -EINVAL; }
    int count = 0;
    while (ctx->pos < native_count && count++ < native_batch) {
        const char *name = native_names[ctx->pos];
        if (!ctx->actor(ctx, name, (int)strlen(name), ctx->pos, ctx->pos + 100, 8)) return 0;
        ctx->pos++;
    }
    return 0;
}
static int client_iterate(struct file *f, struct dir_context *ctx, const struct file_operations *orig, void *data) {
    client_calls++;
    return sumh_dh_iterate_inner(f, ctx, orig, data);
}
static const struct file_operations original = { native_iterate };
static struct sumh_fop_template template = { &original, { sumh_shadow_iterate_shared } };
static struct file file = { { &parent }, &template.shadow_fop };
struct record { char name[32]; loff_t off; };
struct reader { struct dir_context ctx; int capacity, count; struct record records[64]; };
static bool collect(struct dir_context *ctx, const char *name, int len, loff_t off, u64 ino, unsigned int type) {
    struct reader *r = container_of(ctx, struct reader, ctx);
    (void)ino; (void)type;
    if (r->count == r->capacity) return false;
    /* Linux getdents64 writes the current cookie into the previous record. */
    if (r->count) r->records[r->count - 1].off = off;
    memcpy(r->records[r->count].name, name, len);
    r->records[r->count].name[len] = 0;
    r->count++;
    return true;
}
static int read_batch(struct reader *r, int capacity) {
    r->capacity = capacity; r->count = 0;
    int ret = sumh_shadow_iterate_shared(&file, &r->ctx);
    if (r->count) r->records[r->count - 1].off = r->ctx.pos;
    return ret;
}
static void reset(void) {
    sumh_enabled = 1; sumh_rule_count = 1; allocation_fails = 0;
    sumh_ioctl_tgid = 0; su_enabled = su_visible = false;
    parent_mapping.flags = 2; child_mapping.flags = 0;
    native_calls = client_calls = reschedules = native_error = bad_position = 0;
    native_count = injection_count = have_client = 0; native_batch = 100;
    meta = (struct sumh_fop_meta){ &template, NULL, NULL };
    dh_dir = (struct sumh_dh_dir){ &parent_inode, { &exact_child }, false };
}
static void bind_client(void) { have_client = 1; meta.iterate_client = client_iterate; meta.iterate_client_data = &dh_dir; }
int main(void) {
    struct reader r = { .ctx.actor = collect };
    struct sumh_dh_child su_child = exact_child;
    reset();
    su_child.flags = SUMH_VNODE_F_SU;
    su_enabled = su_visible = true;
    CHECK(sumh_dh_current_sees() && sumh_dh_current_targets(&exact_child, &parent_inode));
    CHECK(sumh_dh_current_targets(&su_child, &parent_inode));
    sumh_ioctl_tgid = 17;
    CHECK(sumh_dh_control_current() && !sumh_dh_current_sees());
    CHECK(!sumh_dh_current_targets(&exact_child, &parent_inode));
    CHECK(!sumh_dh_current_targets(&su_child, &parent_inode));
    CHECK(!sumh_dh_child_emitted(&su_child, &parent_inode));
    reset();
    /* Empty native directories must still expose injected entries. One-entry
     * buffers must resume after the accepted item instead of replaying it. */
    reset(); injection_count = 3;
    injected_names[0] = "a"; injected_names[1] = "b"; injected_names[2] = "c";
    for (int i = 0; i < 3; i++) {
        CHECK(read_batch(&r, 1) == 0 && r.count == 1);
        CHECK(!strcmp(r.records[0].name, injected_names[i]));
        CHECK(r.ctx.pos == (loff_t)(SUMH_MAGIC_POS + i + 1));
    }
    CHECK(read_batch(&r, 1) == 0 && !r.count && !bad_position);
    r.ctx.pos = 0;
    CHECK(read_batch(&r, 0) == 0 && !r.count && r.ctx.pos == (loff_t)SUMH_MAGIC_POS);
    CHECK(read_batch(&r, 1) == 0 && !strcmp(r.records[0].name, "a"));

    /* The final injected d_off must retain the boundary when the native
     * iterator starts at zero. Replaying every saved cookie yields its suffix. */
    reset(); injection_count = 2; injected_names[0] = "a"; injected_names[1] = "b";
    native_count = 3; native_names[0] = "."; native_names[1] = ".."; native_names[2] = "real";
    r.ctx.pos = 0;
    CHECK(read_batch(&r, 64) == 0 && r.count == 5);
    struct record full[5]; memcpy(full, r.records, sizeof(full));
    CHECK(full[1].off == (loff_t)SUMH_MAGIC_POS + 2);
    for (int i = 0; i < 5; i++) {
        r.ctx.pos = full[i].off;
        CHECK(read_batch(&r, 64) == 0 && r.count == 4 - i);
        for (int j = 0; j < r.count; j++) CHECK(!strcmp(r.records[j].name, full[i + j + 1].name));
    }
    r.ctx.pos = 0;
    for (int i = 0; i < 5; i++) {
        CHECK(read_batch(&r, 1) == 0 && r.count == 1);
        CHECK(!strcmp(r.records[0].name, full[i].name));
    }
    CHECK(read_batch(&r, 1) == 0 && !r.count && !bad_position);

    /* A hide client and merge view compose. Exact rules are emitted only by
     * the client even when the same name exists in the merge target. */
    reset(); bind_client(); r.ctx.pos = 0;
    injection_count = 3; injected_names[0] = "merge"; injected_names[1] = "exact"; injected_names[2] = "hidden";
    native_count = 3; native_names[0] = "real"; native_names[1] = "exact"; native_names[2] = "hidden";
    CHECK(read_batch(&r, 64) == 0 && r.count == 2);
    CHECK(!strcmp(r.records[0].name, "merge") && !strcmp(r.records[1].name, "real"));
    CHECK(read_batch(&r, 64) == 0 && r.count == 1 && !strcmp(r.records[0].name, "exact"));
    int before_paths = path_calls, before_allocations = allocations;
    CHECK(read_batch(&r, 64) == 0 && !r.count && client_calls && !bad_position);
    CHECK(path_calls == before_paths && allocations == before_allocations);
    /* A cleared client must not send its saved private cookie to native fs. */
    have_client = 0; meta.iterate_client = NULL; parent_mapping.flags = 0;
    CHECK(read_batch(&r, 64) == 0 && r.count == 3 && !bad_position);

    /* Native batches accepted by the lower layer can contain no visible
     * entries. Continue until actual output or stable EOF, with/without client. */
    for (int client = 0; client < 2; client++) {
        reset(); if (client) bind_client(); r.ctx.pos = 0; native_batch = 1;
        native_count = 3; native_names[0] = "hidden"; native_names[1] = "dup"; native_names[2] = "negative";
        CHECK(read_batch(&r, 1) == 0 && r.count == 1);
        CHECK(!strcmp(r.records[0].name, "negative"));
        CHECK(native_calls == 3 && reschedules == 2 && !bad_position);
    }

    /* Private continuation cookies never reach the native fs after a rule
     * removal, disable, or wrapper allocation failure. Native errors survive. */
    for (int mode = 0; mode < 3; mode++) {
        reset(); r.ctx.pos = SUMH_MAGIC_POS + 2;
        if (mode == 0) parent_mapping.flags = 0;
        if (mode == 1) sumh_enabled = 0;
        if (mode == 2) allocation_fails = 1;
        CHECK(read_batch(&r, 1) == 0 && !bad_position && r.ctx.pos == 0);
    }
    reset(); r.ctx.pos = 0; native_error = -EIO;
    CHECK(read_batch(&r, 1) == -EIO);

    /* Resume every mixed-view cookie with both one-record and large user
     * buffers. Native batches can also be smaller than the caller's buffer. */
    reset(); bind_client(); r.ctx.pos = 0; native_batch = 1;
    injection_count = 3; injected_names[0] = "merge"; injected_names[1] = "exact"; injected_names[2] = "hidden";
    native_count = 6; native_names[0] = "."; native_names[1] = ".."; native_names[2] = "exact";
    native_names[3] = "hidden"; native_names[4] = "dup"; native_names[5] = "negative";
    const char *want[] = { "merge", ".", "..", "negative", "exact" };
    struct record combined[5];
    for (int i = 0; i < 5; i++) {
        CHECK(read_batch(&r, 1) == 0 && r.count == 1);
        CHECK(!strcmp(r.records[0].name, want[i]));
        combined[i] = r.records[0];
    }
    CHECK(read_batch(&r, 1) == 0 && !r.count && !bad_position);
    for (int i = 0; i < 5; i++) {
        r.ctx.pos = combined[i].off;
        for (int j = i + 1; j < 5; j++) {
            CHECK(read_batch(&r, 1) == 0 && r.count == 1);
            CHECK(!strcmp(r.records[0].name, want[j]));
        }
        CHECK(read_batch(&r, 1) == 0 && !r.count && !bad_position);
    }
    for (int batch = 0; batch < 2; batch++) {
        native_batch = batch ? 100 : 1;
        r.ctx.pos = 0;
        int used = 0;
        while (used < 5) {
            CHECK(read_batch(&r, 64) == 0 && r.count > 0);
            for (int j = 0; j < r.count; j++) {
                CHECK(used < 5 && !strcmp(r.records[j].name, want[used]));
                combined[used++] = r.records[j];
            }
        }
        CHECK(read_batch(&r, 64) == 0 && !r.count);
        for (int i = 0; i < 5; i++) {
            r.ctx.pos = combined[i].off;
            int observed = i + 1;
            while (observed < 5) {
                CHECK(read_batch(&r, 64) == 0 && r.count > 0);
                for (int j = 0; j < r.count; j++) {
                    CHECK(observed < 5 && !strcmp(r.records[j].name, want[observed++]));
                }
            }
            CHECK(read_batch(&r, 64) == 0 && !r.count && !bad_position);
        }
    }
    CHECK(allocations == releases && !rcu_depth && !srcu_depth);
    CHECK(parent.refs == 1 && target.refs == 1 && positive.refs == 1 && negative.refs == 1);
    return 0;
}
"""
    return COMMON + preamble + wrapper + allocation + dh + bodies + tests


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler, "directory-stream", source())
