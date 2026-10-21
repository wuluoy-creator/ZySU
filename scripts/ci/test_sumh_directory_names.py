#!/usr/bin/env python3
"""Exercise SUMH directory deduplication against the production C helpers.

The host doubles cover name matching, first-entry precedence, list ordering and
allocation failures. Kernel RB-tree balancing and concurrent VFS behavior still
require a kernel build and device validation.
"""

import argparse
import os
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test


PREAMBLE = r"""
typedef unsigned long long u64;
typedef long long loff_t;
typedef unsigned int dev_t;
typedef unsigned short umode_t;
#define NAME_MAX 255
#define ENOMEM 12
#define ENOTDIR 20
#define GFP_KERNEL 1
#define GFP_ATOMIC 2
#define DT_DIR 4
#define DT_REG 8
#define DT_CHR 2
#define S_IFMT 0170000
#define LOOKUP_FOLLOW 1
#define STATX_TYPE 1
#define AT_STATX_SYNC_AS_STAT 0
#define S_ISCHR(m) (((m) & S_IFMT) == 0020000)
#define container_of(p, t, m) ((t *)((char *)(p) - __builtin_offsetof(t, m)))
struct list_head { struct list_head *next, *prev; };
static void INIT_LIST_HEAD(struct list_head *head) { head->next = head->prev = head; }
static void list_add(struct list_head *item, struct list_head *head) {
    item->next = head->next; item->prev = head;
    head->next->prev = item; head->next = item;
}
static void list_add_tail(struct list_head *item, struct list_head *head) {
    item->next = head; item->prev = head->prev;
    head->prev->next = item; head->prev = item;
}
static void list_del(struct list_head *item) {
    item->prev->next = item->next; item->next->prev = item->prev;
}
#define list_for_each_entry_safe(item, tmp, head, member) \
    for (struct list_head *cursor = (head)->next, *next = cursor->next; \
         cursor != (head) && ((item) = container_of(cursor, __typeof__(*(item)), member), \
                              (tmp) = container_of(next, __typeof__(*(tmp)), member), 1); \
         cursor = next, next = cursor->next)
#define list_for_each_entry(item, head, member) \
    for (struct list_head *cursor = (head)->next; \
         cursor != (head) && ((item) = container_of(cursor, __typeof__(*(item)), member), 1); \
         cursor = cursor->next)
struct rb_node { struct rb_node *rb_left, *rb_right; };
struct rb_root { struct rb_node *rb_node; };
#define RB_ROOT ((struct rb_root) { NULL })
#define rb_entry(p, t, m) container_of(p, t, m)
static void rb_link_node(struct rb_node *node, struct rb_node *parent,
                         struct rb_node **link) {
    (void)parent; node->rb_left = node->rb_right = NULL; *link = node;
}
/* Preserve the binary-search topology; kernel-owned balancing is not under test. */
static void rb_insert_color(struct rb_node *node, struct rb_root *root) {
    (void)node; (void)root;
}
static int memcmp(const void *left, const void *right, size_t length) {
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < length; i++) if (a[i] != b[i]) return a[i] - b[i];
    return 0;
}
static char *strchr(const char *text, int needle) {
    do { if (*text == needle) return (char *)text; } while (*text++);
    return NULL;
}
struct allocation { unsigned char bytes[1024]; };
static struct allocation allocations[64];
static int alloc_count, alloc_attempts, fail_at, frees, rcu_depth, bad_alloc;
static void *kmalloc(size_t bytes, int flags) {
    alloc_attempts++;
    if (rcu_depth && flags != GFP_ATOMIC) bad_alloc++;
    if (alloc_attempts == fail_at || bytes > sizeof(allocations[0]) || alloc_count == 64)
        return NULL;
    return allocations[alloc_count++].bytes;
}
static void kfree(void *memory) { if (memory) frees++; }
static char *kstrndup(const char *text, size_t len, int flags) {
    char *copy = kmalloc(len + 1, flags);
    if (copy) { memcpy(copy, text, len); copy[len] = 0; }
    return copy;
}
static void rcu_read_lock(void) { rcu_depth++; }
static void rcu_read_unlock(void) { rcu_depth--; }
"""


def helpers():
    types = (ROOT / "kernel/sumh/internal/sumh_types.h").read_text(encoding="utf-8")
    start = types.index("struct sumh_name_list {")
    structure = types[start:types.index("};", start) + 2]
    source = "\n".join(
        function("kernel/sumh/internal/sumh_name_index.h", marker)
        for marker in (
            "static inline int sumh_name_compare",
            "static inline bool sumh_name_index_contains",
            "static inline void sumh_name_index_add",
            "static inline void sumh_name_index_init",
        )
    )
    count = r"""
static int list_count(struct list_head *head) {
    struct sumh_name_list *item; int count = 0;
    list_for_each_entry(item, head, list) count++;
    return count;
}
static struct sumh_name_list *at(struct list_head *head, int index) {
    struct sumh_name_list *item;
    list_for_each_entry(item, head, list) if (!index--) return item;
    return NULL;
}
"""
    return COMMON + PREAMBLE + structure + source + count


def index_source():
    body = r"""
int main(void) {
    struct list_head head;
    struct rb_root names;
    struct sumh_name_list first = { .name = "alpha", .ino = 17 };
    struct sumh_name_list duplicate = { .name = "alpha", .ino = 99 };
    struct sumh_name_list prefix = { .name = "alphabet" };
    struct sumh_name_list high_byte = { .name = "\200leaf" };
    static struct sumh_name_list many[128];
    static char labels[128][4];
    const char bounded[] = { 'a', 'l', 'p', 'h', 'a', 'X' };
    INIT_LIST_HEAD(&head);
    list_add_tail(&first.list, &head);
    list_add_tail(&duplicate.list, &head);
    list_add_tail(&prefix.list, &head);
    list_add_tail(&high_byte.list, &head);
    sumh_name_index_init(&names, &head);
    CHECK(list_count(&head) == 4 && at(&head, 0) == &first && at(&head, 1) == &duplicate);
    CHECK(at(&head, 2) == &prefix && at(&head, 3) == &high_byte);
    CHECK(first.name_len == 5 && prefix.name_len == 8 && duplicate.name_len == 5);
    CHECK(sumh_name_index_contains(&names, bounded, 5));
    CHECK(!sumh_name_index_contains(&names, bounded, 6));
    CHECK(!sumh_name_index_contains(&names, "alph", 4));
    CHECK(!sumh_name_index_contains(&names, "alphabets", 9));
    CHECK(sumh_name_index_contains(&names, "alphabet", 8));
    CHECK(sumh_name_index_contains(&names, "\200leaf", 5));
    CHECK(!sumh_name_index_contains(&names, "\177leaf", 5));
    CHECK(rb_entry(names.rb_node, struct sumh_name_list, name_node)->ino == 17);
    /* Rebuild an index from a previously indexed list, without retaining its root. */
    sumh_name_index_init(&names, &head);
    CHECK(sumh_name_index_contains(&names, bounded, 5) && list_count(&head) == 4);
    for (int reverse = 0; reverse <= 1; reverse++) {
        INIT_LIST_HEAD(&head);
        for (int i = 0; i < 128; i++) {
            int index = reverse ? 127 - i : i;
            labels[index][0] = 'n';
            labels[index][1] = 'A' + index / 16;
            labels[index][2] = 'A' + index % 16;
            labels[index][3] = 0;
            many[index].name = labels[index];
            list_add_tail(&many[index].list, &head);
        }
        sumh_name_index_init(&names, &head);
        CHECK(list_count(&head) == 128 && at(&head, 0) == &many[reverse ? 127 : 0]);
        for (int i = 0; i < 128; i++)
            CHECK(sumh_name_index_contains(&names, labels[i], 3));
        CHECK(!sumh_name_index_contains(&names, "nZZ", 3));
    }
    CHECK(!alloc_attempts);
    return 0;
}
"""
    return helpers() + body


def merge_source():
    # The old merge actor allowed allocation failures to omit names silently.
    # Exercise its production snapshot replacement; keep virtual K02 groups
    # below independent and unchanged.
    from test_sumh_merge_snapshot import fixture, TESTS
    return (fixture() + TESTS[:TESTS.index("int main(void)")] +
            "int main(void) { return whiteouts(); }\n").replace("unsigned long)", "__UINTPTR_TYPE__)")


def virtual_source():
    preamble = r"""
struct sumh_entry {
    const char *src; bool source_nofollow_path_valid;
    umode_t source_nofollow_mode, source_mode;
    u64 nofollow_visible_ino, visible_ino;
};
static struct sumh_entry rules[] = {
    { .src = "/vdir/first", .source_mode = 0100000, .visible_ino = 11 },
    { .src = "/vdir/sub/one" },
    { .src = "/vdir/sub/two" },
    { .src = "/vdir/first", .source_mode = 0040000, .visible_ino = 99 },
    { .src = "/vdir/prefix", .source_nofollow_path_valid = true,
      .source_nofollow_mode = 0120000, .nofollow_visible_ino = 31 },
    { .src = "/vdir/prefix-long", .source_mode = 0100000, .visible_ino = 41 },
    { .src = "/other/path" }, { .src = NULL }, { .src = "/vdir/" },
};
#define hash_for_each_rcu(table, bucket, item, member) \
    for ((bucket) = 0; (bucket) < (int)(sizeof(rules) / sizeof(rules[0])) && \
         ((item) = &rules[bucket], 1); (bucket)++)
static u64 sumh_vnode_vpath_ino(const char *path) {
    return strcmp(path, "/vdir/sub") ? 0 : 71;
}
"""
    body = r"""
int main(void) {
    struct list_head head;
    struct sumh_name_list seed = { .name = "first", .ino = 17, .type = DT_DIR };
    INIT_LIST_HEAD(&head); list_add_tail(&seed.list, &head);
    CHECK(sumh_rule_vpath_emit("/vdir", &head) == 3);
    CHECK(list_count(&head) == 4 && at(&head, 0) == &seed);
    CHECK(seed.ino == 17 && seed.type == DT_DIR);
    CHECK(!strcmp(at(&head, 1)->name, "sub") && at(&head, 1)->ino == 71);
    CHECK(at(&head, 1)->type == DT_DIR && at(&head, 1)->name_len == 3);
    CHECK(!strcmp(at(&head, 2)->name, "prefix") && at(&head, 2)->ino == 31);
    CHECK(at(&head, 2)->type == 10 && at(&head, 3)->ino == 41);
    CHECK(!rcu_depth && !bad_alloc);
    int previous = alloc_attempts;
    CHECK(sumh_rule_vpath_emit("/vdir", &head) == 0);
    CHECK(list_count(&head) == 4 && alloc_attempts == previous);
    CHECK(!sumh_rule_vpath_emit(NULL, &head) && !sumh_rule_vpath_emit("/vdir", NULL));
    INIT_LIST_HEAD(&head); alloc_count = alloc_attempts = frees = 0;
    for (int failure = 1; failure <= 9; failure++) {
        INIT_LIST_HEAD(&head); alloc_count = alloc_attempts = frees = 0;
        fail_at = failure;
        CHECK(sumh_rule_vpath_emit("/vdir", &head) == -ENOMEM);
        CHECK(!rcu_depth && !bad_alloc);
        /* An incomplete list is owned by the caller, never a successful view. */
        while (head.next != &head) {
            struct sumh_name_list *item = at(&head, 0);
            list_del(&item->list); kfree(item->name); kfree(item);
        }
        CHECK(frees == alloc_count);
    }
    INIT_LIST_HEAD(&head); alloc_count = alloc_attempts = frees = fail_at = 0;
    CHECK(sumh_rule_vpath_emit("/vdir", &head) == 4);
    CHECK(at(&head, 0)->ino == 11 && at(&head, 1)->ino == 71);
    return 0;
}
"""
    emit = function("kernel/sumh/policy/sumh_path_policy.c", "int SUMH_NOCFI sumh_rule_vpath_emit")
    return helpers() + preamble + emit + body


def virtual_iteration_source():
    # Keep the same production rule enumeration and replace only main() with
    # an actual vnode iterator exercising error cleanup and small buffers.
    source = virtual_source().split("int main(void) {")[0]
    preamble = r"""
struct file { int unused; };
struct dir_context { loff_t pos; };
struct sumh_vnode_info { const char *visible_path; };
static int capacity, emitted, dots_called;
static bool accept_dots = true;
static char observed[8][32];
static bool dir_emit_dots(struct file *file, struct dir_context *ctx) {
    (void)file; dots_called++;
    if (!accept_dots) return false;
    if (ctx->pos < 2) ctx->pos = 2;
    return true;
}
static bool dir_emit(struct dir_context *ctx, const char *name, size_t len,
                     u64 ino, unsigned int type) {
    (void)ctx; (void)type;
    if (!capacity) return false;
    if (len >= 32 || emitted == 8 || !ino) return false;
    memcpy(observed[emitted], name, len); observed[emitted++][len] = 0;
    capacity--; return true;
}
static void allocator_reset(int failure) {
    alloc_count = alloc_attempts = frees = 0; fail_at = failure;
}
"""
    iterator = function("kernel/sumh/core/sumh_vnode.c",
                        "static int SUMH_NOCFI sumh_vnode_dir_iterate_virtual(")
    body = r"""
int main(void) {
    struct file file = {};
    struct sumh_vnode_info info = { .visible_path = "/vdir" };
    for (int failure = 1; failure <= 9; failure++) {
        struct dir_context ctx = {};
        allocator_reset(failure); emitted = dots_called = 0; capacity = 8;
        CHECK(sumh_vnode_dir_iterate_virtual(&file, &ctx, &info) == -ENOMEM);
        CHECK(!ctx.pos && !emitted && !dots_called);
        CHECK(frees == alloc_count && !rcu_depth && !bad_alloc);
    }
    struct dir_context ctx = {};
    emitted = 0;
    allocator_reset(0); capacity = 8; accept_dots = false;
    CHECK(!sumh_vnode_dir_iterate_virtual(&file, &ctx, &info));
    CHECK(!ctx.pos && !emitted && alloc_count == frees);
    accept_dots = true;
    for (int child = 0; child < 4; child++) {
        allocator_reset(0); capacity = 1;
        CHECK(!sumh_vnode_dir_iterate_virtual(&file, &ctx, &info));
        CHECK(emitted == child + 1 && ctx.pos == child + 3);
        CHECK(frees == alloc_count);
        const loff_t cursor = ctx.pos;
        allocator_reset(4); capacity = 1;
        CHECK(sumh_vnode_dir_iterate_virtual(&file, &ctx, &info) == -ENOMEM);
        CHECK(ctx.pos == cursor && emitted == child + 1 && frees == alloc_count);
    }
    CHECK(!strcmp(observed[0], "first") && !strcmp(observed[1], "sub"));
    CHECK(!strcmp(observed[2], "prefix") && !strcmp(observed[3], "prefix-long"));
    allocator_reset(0); capacity = 8;
    CHECK(!sumh_vnode_dir_iterate_virtual(&file, &ctx, &info));
    CHECK(ctx.pos == 6 && emitted == 4 && frees == alloc_count);
    return 0;
}
"""
    return source + preamble + iterator + body


def virtual_lookup_source():
    preamble = r"""
typedef unsigned int u32;
struct dentry;
struct path { int id; struct dentry *dentry; void *mnt; };
struct sumh_entry {
    const char *src; u32 src_hash; bool source_path_valid, source_nofollow_path_valid;
    struct path source_path, source_nofollow_path;
    umode_t source_mode, source_nofollow_mode;
    unsigned long visible_ino, nofollow_visible_ino;
};
static struct sumh_entry rules[] = {
    { .src = "/vdir/first", .src_hash = 1, .source_path_valid = true,
      .source_path.id = 7, .source_mode = 0100000, .visible_ino = 11 },
    { .src = "/vdir/sub/one" },
};
#define SUMH_VPATH_NONE 0
#define SUMH_VPATH_LEAF 1
#define SUMH_VPATH_VDIR 2
static int pinned;
static u32 full_name_hash(void *salt, const char *name, size_t len) {
    (void)salt; (void)name; (void)len; return 1;
}
static void path_get(struct path *path) { (void)path; pinned++; }
#define hlist_for_each_entry_rcu(item, head, member) \
    for (int i = 0; i < 2 && ((item) = &rules[i], 1); i++)
#define hash_for_each_rcu(table, bucket, item, member) \
    for ((bucket) = 0; (bucket) < 2 && ((item) = &rules[bucket], 1); (bucket)++)
"""
    lookup = function("kernel/sumh/policy/sumh_path_policy.c",
                      "int SUMH_NOCFI sumh_rule_vpath_child(")
    body = r"""
int main(void) {
    struct path leaf = {};
    umode_t mode = 0;
    unsigned long ino = 0;
    fail_at = 1;
    CHECK(sumh_rule_vpath_child("/vdir", "first", &leaf, &mode, &ino) == -ENOMEM);
    CHECK(!pinned && !rcu_depth && !leaf.id && !ino);
    fail_at = 0;
    CHECK(sumh_rule_vpath_child("/vdir", "first", &leaf, &mode, &ino) == SUMH_VPATH_LEAF);
    CHECK(leaf.id == 7 && ino == 11 && pinned == 1);
    CHECK(sumh_rule_vpath_child("/vdir", "sub", NULL, NULL, NULL) == SUMH_VPATH_VDIR);
    CHECK(sumh_rule_vpath_child("/vdir", "absent", NULL, NULL, NULL) == SUMH_VPATH_NONE);
    CHECK(alloc_count == frees && !rcu_depth && !bad_alloc);
    return 0;
}
"""
    return helpers() + preamble + lookup + body


def vnode_lookup_source():
    source = virtual_lookup_source().split("int main(void) {")[0]
    preamble = r"""
typedef unsigned char u8;
#define ENOENT 2
#define ENAMETOOLONG 36
#define SUMH_VNODE_F_DIR 1
#define SUMH_VNODE_F_LNK 2
#define S_ISDIR(m) (((m) & S_IFMT) == 0040000)
#define S_ISLNK(m) (((m) & S_IFMT) == 0120000)
#define ERR_PTR(error) ((struct dentry *)(__INTPTR_TYPE__)(error))
struct super_block { dev_t s_dev; };
struct inode { void *i_private; struct super_block *i_sb; umode_t i_mode; unsigned long i_ino; };
struct dentry { struct { const char *name; size_t len; } d_name; struct inode *inode; };
struct sumh_vnode_info { const char *visible_path; struct path source; };
#define THIS_MODULE NULL
static int module_refs, negative_entries;
static struct inode created;
static bool try_module_get(void *module) { (void)module; module_refs++; return true; }
static void module_put(void *module) { (void)module; module_refs--; }
static void path_put(struct path *path) { (void)path; pinned--; }
static struct inode *d_inode(struct dentry *entry) { return entry->inode; }
static void d_add(struct dentry *entry, struct inode *inode) {
    (void)entry; if (!inode) negative_entries++;
}
static struct dentry *d_splice_alias(struct inode *inode, struct dentry *entry) {
    (void)inode; return entry;
}
static int vfs_path_lookup(struct dentry *d, void *mount, const char *name,
                            int flags, struct path *path) {
    (void)d; (void)mount; (void)name; (void)flags; (void)path; return -ENOENT;
}
static unsigned long sumh_vnode_source_ino(dev_t dev, u64 ino) { (void)dev; return ino; }
static unsigned long sumh_vnode_vpath_ino(const char *path) { (void)path; return 71; }
static struct inode *sumh_vnode_new(struct super_block *sb, struct path *path,
                                    unsigned long ino, umode_t mode, u8 flags) {
    (void)sb; (void)path; (void)ino; (void)mode; (void)flags; return &created;
}
static struct inode *sumh_vnode_new_virtual(struct super_block *sb, const char *path,
                                            unsigned long ino, struct inode *parent) {
    (void)sb; (void)path; (void)ino; (void)parent; return &created;
}
static char *kasprintf(int flags, const char *format, const char *dir, const char *name) {
    (void)format; (void)dir; return kstrndup(name, strlen(name), flags);
}
"""
    lookup = function("kernel/sumh/core/sumh_vnode.c",
                      "static struct dentry *SUMH_NOCFI sumh_vnode_dir_lookup(")
    body = r"""
int main(void) {
    struct sumh_vnode_info info = { .visible_path = "/vdir" };
    struct inode parent = { .i_private = &info };
    struct dentry entry = { .d_name = { "first", 5 } };
    for (int failure = 1; failure <= 2; failure++) {
        alloc_count = alloc_attempts = frees = 0; fail_at = failure;
        CHECK(sumh_vnode_dir_lookup(&parent, &entry, 0) == ERR_PTR(-ENOMEM));
        CHECK(!negative_entries && !module_refs && !pinned && alloc_count == frees);
    }
    alloc_count = alloc_attempts = frees = fail_at = 0;
    CHECK(sumh_vnode_dir_lookup(&parent, &entry, 0) == &entry);
    CHECK(!negative_entries && !module_refs && !pinned && alloc_count == frees);
    entry.d_name.name = "sub"; entry.d_name.len = 3;
    alloc_count = alloc_attempts = frees = 0; fail_at = 3;
    CHECK(sumh_vnode_dir_lookup(&parent, &entry, 0) == ERR_PTR(-ENOMEM));
    CHECK(!negative_entries && !module_refs && !pinned && alloc_count == frees);
    alloc_count = alloc_attempts = frees = fail_at = 0;
    CHECK(sumh_vnode_dir_lookup(&parent, &entry, 0) == &entry);
    CHECK(!negative_entries && !module_refs && !pinned && alloc_count == frees);
    entry.d_name.name = "absent"; entry.d_name.len = 6;
    CHECK(!sumh_vnode_dir_lookup(&parent, &entry, 0));
    CHECK(negative_entries == 1 && !module_refs && !pinned && alloc_count == frees);
    return 0;
}
"""
    return source + preamble + lookup + body


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler, "directory-name-index", index_source())
    run_test(compiler, "directory-name-merge", merge_source())
    run_test(compiler, "directory-name-virtual", virtual_source())
    run_test(compiler, "directory-virtual-iteration", virtual_iteration_source())
    run_test(compiler, "directory-virtual-lookup", virtual_lookup_source())
    run_test(compiler, "directory-vnode-lookup", vnode_lookup_source())
