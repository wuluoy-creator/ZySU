#!/usr/bin/env python3
"""Execute SUMH's reclaim/setattr code with small VFS and workqueue doubles.

These host checks exercise ownership and error paths; they do not replace a
kernel build or on-device filesystem tests. The function bodies are read from
the production sources, so a regression in those bodies changes the test.
"""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def function(source, name, declaration):
    match = re.search(r"\b" + name + r"\s*\([^;{}]*\)\s*\{", source)
    if not match:
        raise ValueError(f"Function {name} not found")
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return declaration + "\n" + source[start:end] + "\n"


PRELUDE = r"""
#define NULL ((void *)0)
#define true 1
#define false 0
#define bool int
#define SUMH_NOCFI
#define EROFS 30
#define EOPNOTSUPP 95
#define ENOENT 2
#define ATTR_FILE 8192
#define THIS_MODULE ((void *)1)
#define container_of(p, t, m) ((t *)((char *)(p) - __builtin_offsetof(t, m)))
#define CHECK(id, expr) do { if (!(expr)) return id; } while (0)

typedef __SIZE_TYPE__ size_t;
void *memset(void *dest, int value, size_t count)
{
    unsigned char *p = dest;
    while (count--) *p++ = (unsigned char)value;
    return dest;
}

struct llist_node { struct llist_node *next; };
struct llist_head { struct llist_node *first; };
struct rcu_head { void (*callback)(struct rcu_head *); };
struct work_struct { void (*fn)(struct work_struct *); int pending; };
#define llist_for_each_safe(node, next_node, head) \
    for ((node) = (head); (node) && ((next_node) = (node)->next, 1); \
         (node) = (next_node))
static void llist_add(struct llist_node *node, struct llist_head *head)
{
    node->next = head->first;
    head->first = node;
}
static struct llist_node *llist_del_all(struct llist_head *head)
{
    struct llist_node *first = head->first;
    head->first = NULL;
    return first;
}
static void schedule_work(struct work_struct *work) { work->pending = 1; }
static struct rcu_head *callbacks[16];
static int callback_count, atomic_context, bad_context, free_count;
static int path_puts, dputs, mount_puts, sumh_vnode_live_count;
static void (*release_hook)(void);
static void call_rcu(struct rcu_head *head, void (*fn)(struct rcu_head *))
{
    head->callback = fn;
    callbacks[callback_count++] = head;
}
static void rcu_barrier(void)
{
    int i;
    atomic_context = 1;
    for (i = 0; i < callback_count; i++) callbacks[i]->callback(callbacks[i]);
    callback_count = 0;
    atomic_context = 0;
}
static void flush_work(struct work_struct *work)
{
    while (work->pending) {
        work->pending = 0;
        work->fn(work);
    }
}
static void cond_resched(void) { bad_context |= atomic_context; }
static void kfree(void *p) { if (p) free_count++; }
#define atomic_dec(p) (--*(p))

struct mnt_idmap { int value; };
struct user_namespace { int value; };
#ifdef OLD_IDMAP
#define MAP_TYPE struct user_namespace
#define KVN_IDMAP_ARG struct user_namespace *userns,
#define KVN_IDMAP_CALL userns,
#else
#define MAP_TYPE struct mnt_idmap
#define KVN_IDMAP_ARG struct mnt_idmap *idmap,
#define KVN_IDMAP_CALL idmap,
#endif
struct vfsmount { MAP_TYPE *idmap; int refs; };
#define KVN_SRC_IDMAP(mnt) (mnt)->idmap,
struct inode { void *i_private; int locked; };
struct dentry { struct inode *inode; int refs; };
struct path { struct dentry *dentry; struct vfsmount *mnt; };
struct sumh_vnode_info { struct path source; };
struct file { void *private_data; };
struct iattr { unsigned int ia_valid; struct file *ia_file; };
/* Record-only fixtures deliberately have NULL owners. Actual claim and third
 * queue lifetimes run in test_sumh_inject_claims.py, with production helpers. */
struct sumh_inject_claim;
struct sumh_dh_claim;
static void sumh_dh_claim_put(struct sumh_dh_claim *c) { if (c) bad_context = 1; }
struct sumh_inject_inode { struct llist_node free_node; };
static void sumh_inject_destroy(struct sumh_inject_claim *c) { if (c) bad_context = 1; }
static void sumh_inject_inode_free(struct sumh_inject_inode *c) { if (c) bad_context = 1; }
struct sumh_entry {
    struct path source_path, source_nofollow_path;
    struct inode *source_inode;
    bool source_path_valid, source_nofollow_path_valid;
    char *src, *target, *source_canonical;
    struct sumh_inject_claim *inject_claim;
    struct sumh_dh_claim *dh_claim;
    struct rcu_head rcu;
    struct llist_node free_node;
};
struct sumh_hide_entry {
    char *path;
    struct sumh_dh_claim *dh_claim;
    struct llist_node free_node;
};
struct sumh_merge_entry {
    struct path target_path;
    char *src, *target, *resolved_src;
    struct sumh_inject_claim *inject_claims[2];
    struct rcu_head rcu;
    struct llist_node free_node;
};
static void path_put(struct path *path)
{
    bad_context |= atomic_context;
    if (!path || !path->dentry || !path->mnt ||
        path->dentry->refs <= 1 || path->mnt->refs <= 1) bad_context = 1;
    else {
        path->dentry->refs--;
        path->mnt->refs--;
    }
    path_puts++;
    dputs++;
    mount_puts++;
    if (release_hook) {
        void (*hook)(void) = release_hook;
        release_hook = NULL;
        hook();
    }
}
static struct llist_head sumh_retired_entries, sumh_retired_merges, sumh_retired_inject_inodes, sumh_retired_hides;
static void sumh_store_free_workfn(struct work_struct *work);
static struct work_struct sumh_store_free_work = { sumh_store_free_workfn, 0 };

static int module_available = 1, module_refs, want_calls, drop_calls;
static int want_error, notify_error, notify_calls, write_refs, lock_errors;
static MAP_TYPE *seen_idmap;
static struct dentry *seen_dentry;
static struct iattr seen_attr;
static struct vfsmount *seen_mount;
static struct inode *d_inode(struct dentry *dentry) { return dentry->inode; }
static int try_module_get(void *module)
{
    (void)module;
    if (!module_available) return 0;
    module_refs++;
    return 1;
}
static void module_put(void *module) { (void)module; module_refs--; }
static void inode_lock(struct inode *inode)
{
    lock_errors |= inode->locked || write_refs != 1;
    inode->locked = 1;
}
static void inode_unlock(struct inode *inode)
{
    lock_errors |= !inode->locked;
    inode->locked = 0;
}
static int test_want_write(struct vfsmount *mnt)
{
    seen_mount = mnt;
    want_calls++;
    if (!want_error) write_refs++;
    return want_error;
}
static void test_drop_write(struct vfsmount *mnt)
{
    lock_errors |= mnt != seen_mount || write_refs != 1;
    drop_calls++;
    write_refs--;
}
static int test_notify(MAP_TYPE *idmap, struct dentry *dentry,
                       struct iattr *attr, void *delegated)
{
    (void)delegated;
    notify_calls++;
    seen_idmap = idmap;
    seen_dentry = dentry;
    seen_attr = *attr;
    lock_errors |= !dentry->inode->locked;
    return notify_error;
}
static void *sumh_mnt_want_write_addr = test_want_write;
static void *sumh_mnt_drop_write_addr = test_drop_write;
static int (*sumh_notify_change)(MAP_TYPE *, struct dentry *,
                                  struct iattr *, void *) = test_notify;
"""


TESTS = r"""
static struct sumh_entry extra_entry;
static void retire_during_release(void)
{
    atomic_context = 1;
    sumh_entry_free_rcu(&extra_entry.rcu);
    atomic_context = 0;
}

int run(void)
{
    struct inode src_inode = {0}, visible_inode = {0};
    struct dentry src_dentry = {&src_inode, 4}, visible_dentry = {&visible_inode, 1};
    MAP_TYPE visible_map = {1}, source_map = {2};
    struct vfsmount source_mount = {&source_map, 4};
    struct path source = {&src_dentry, &source_mount};
    struct sumh_vnode_info info = {source};
    struct file real_file = {0}, visible_file = {&real_file};
    struct iattr attr = {ATTR_FILE, &visible_file};
    struct sumh_entry entry = {0};
    struct sumh_merge_entry merge = {0};
    int ret;

    /* Reclaim must wait for both RCU readers and the process-context worker. */
    entry.source_path = source;
    entry.source_nofollow_path = source;
    entry.source_inode = &src_inode;
    entry.source_path_valid = entry.source_nofollow_path_valid = true;
    merge.target_path = source;
    sumh_vnode_live_count = 1;
    call_rcu(&entry.rcu, sumh_entry_free_rcu);
    call_rcu(&merge.rcu, sumh_merge_entry_free_rcu);
    CHECK(1, !path_puts && !dputs && !free_count);
    rcu_barrier();
    CHECK(2, !path_puts && !dputs && !free_count);
    CHECK(3, sumh_store_free_work.pending);
    sumh_store_drain();
    CHECK(4, path_puts == 3 && dputs == 3 && mount_puts == 3 && free_count == 2);
    CHECK(5, !bad_context && !sumh_vnode_live_count);
    CHECK(6, !entry.source_path.dentry && !entry.source_nofollow_path.dentry);
    CHECK(7, !entry.source_path_valid && !entry.source_nofollow_path_valid);
    CHECK(8, !entry.source_inode && !sumh_store_free_work.pending);
    CHECK(29, src_dentry.refs == 1 && source_mount.refs == 1);

    /* A callback arriving during destruction must also be drained. */
    memset(&entry, 0, sizeof(entry));
    entry.source_path = source;
    entry.source_path_valid = true;
    extra_entry.source_path = source;
    extra_entry.source_path_valid = true;
    src_dentry.refs += 2;
    source_mount.refs += 2;
    sumh_vnode_live_count = 2;
    release_hook = retire_during_release;
    call_rcu(&entry.rcu, sumh_entry_free_rcu);
    sumh_store_drain();
    CHECK(9, path_puts == 5 && free_count == 4 && !bad_context);
    CHECK(10, !sumh_vnode_live_count && !sumh_store_free_work.pending);
    sumh_store_drain();
    CHECK(11, path_puts == 5 && free_count == 4);
    CHECK(30, src_dentry.refs == 1 && source_mount.refs == 1 && mount_puts == 5);

    /* Attributes must reach the source mount's idmap and opened file. */
    visible_inode.i_private = &info;
    ret = sumh_vnode_setattr(&visible_map, &visible_dentry, &attr);
    CHECK(12, !ret && notify_calls == 1 && seen_idmap == &source_map);
    CHECK(13, seen_dentry == &src_dentry && seen_attr.ia_file == &real_file);
    CHECK(14, seen_attr.ia_valid & ATTR_FILE);
    CHECK(15, attr.ia_file == &visible_file && attr.ia_valid == ATTR_FILE);
    CHECK(16, want_calls == 1 && drop_calls == 1 && !write_refs && !module_refs);
    CHECK(17, !lock_errors && !src_inode.locked && seen_mount == &source_mount);

    /* Read-only sources reject mutation and never drop an unacquired ref. */
    want_error = -EROFS;
    ret = sumh_vnode_setattr(&visible_map, &visible_dentry, &attr);
    CHECK(18, ret == -EROFS && notify_calls == 1);
    CHECK(19, want_calls == 2 && drop_calls == 1 && !write_refs && !module_refs);

    /* Notify failures still release the writer, lock and module reference. */
    want_error = 0;
    notify_error = -5;
    ret = sumh_vnode_setattr(&visible_map, &visible_dentry, &attr);
    CHECK(20, ret == -5 && notify_calls == 2 && drop_calls == 2);
    CHECK(21, !write_refs && !module_refs && !src_inode.locked && !lock_errors);
    notify_error = 0;

    /* A missing opened source removes ATTR_FILE only from the copied attr. */
    visible_file.private_data = NULL;
    ret = sumh_vnode_setattr(&visible_map, &visible_dentry, &attr);
    CHECK(22, !ret && !(seen_attr.ia_valid & ATTR_FILE));
    CHECK(23, attr.ia_valid == ATTR_FILE && !write_refs && !module_refs);
    info.source.dentry = NULL;
    CHECK(24, sumh_vnode_setattr(&visible_map, &visible_dentry, &attr) == -EROFS);
    info.source = source;
    sumh_notify_change = NULL;
    CHECK(25, sumh_vnode_setattr(&visible_map, &visible_dentry, &attr) == -EOPNOTSUPP);
    sumh_notify_change = test_notify;
    module_available = 0;
    CHECK(26, sumh_vnode_setattr(&visible_map, &visible_dentry, &attr) == -ENOENT);
    module_available = 1;
    src_dentry.inode = NULL;
    CHECK(27, sumh_vnode_setattr(&visible_map, &visible_dentry, &attr) == -ENOENT);
    CHECK(28, !write_refs && !module_refs && !lock_errors);
    return 0;
}

#ifndef _WIN32
int main(void) { return run(); }
#endif
"""


def test_source():
    runtime = (ROOT / "kernel/sumh/core/sumh_runtime.c").read_text(encoding="utf-8")
    vnode = (ROOT / "kernel/sumh/core/sumh_vnode.c").read_text(encoding="utf-8")
    functions = [
        (runtime, "sumh_entry_release_source", "void sumh_entry_release_source(struct sumh_entry *entry)"),
        (runtime, "sumh_entry_free", "static void sumh_entry_free(struct sumh_entry *e)"),
        (runtime, "sumh_entry_free_rcu", "void sumh_entry_free_rcu(struct rcu_head *head)"),
        (runtime, "sumh_merge_entry_free", "static void sumh_merge_entry_free(struct sumh_merge_entry *e)"),
        (runtime, "sumh_merge_entry_free_rcu", "void sumh_merge_entry_free_rcu(struct rcu_head *head)"),
        (runtime, "sumh_store_free_workfn", "static void sumh_store_free_workfn(struct work_struct *work)"),
        (runtime, "sumh_store_drain", "void sumh_store_drain(void)"),
        (vnode, "sumh_vnode_src_want_write", "static int sumh_vnode_src_want_write(const struct path *src)"),
        (vnode, "sumh_vnode_src_drop_write", "static void sumh_vnode_src_drop_write(const struct path *src)"),
        (vnode, "sumh_vnode_setattr", "static int sumh_vnode_setattr(KVN_IDMAP_ARG struct dentry *dentry, struct iattr *attr)"),
    ]
    return PRELUDE + "\n".join(function(*args) for args in functions) + TESTS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=shutil.which("clang") or shutil.which("cc"))
    parser.add_argument("--ld", help="Windows: path to LLVM ld.lld")
    args = parser.parse_args()
    if not args.cc:
        parser.error("No C compiler found; pass --cc")
    with tempfile.TemporaryDirectory(prefix="sumh-core-") as temp:
        directory = Path(temp)
        source = directory / "test.c"
        source.write_text(test_source(), encoding="utf-8")
        for old_idmap in (False, True):
            output = directory / ("test.exe" if sys.platform == "win32" else "test")
            command = [args.cc, "-std=gnu11", "-Wall", "-Werror", "-ffreestanding",
                       "-fno-stack-protector", "-fno-builtin"]
            if old_idmap:
                command.append("-DOLD_IDMAP")
            if sys.platform == "win32":
                obj = directory / "test.obj"
                subprocess.run(command + ["--target=x86_64-pc-windows-msvc", "-c", str(source), "-o", str(obj)], check=True)
                linker = args.ld or str(Path(args.cc).with_name("ld.lld.exe"))
                subprocess.run([linker, "-flavor", "link", "/entry:run", "/subsystem:console", "/nodefaultlib", f"/out:{output}", str(obj)], check=True)
            else:
                subprocess.run(command + [str(source), "-o", str(output)], check=True)
            result = subprocess.run([str(output)])
            if result.returncode:
                raise AssertionError(f"SUMH core check {result.returncode} failed (old_idmap={old_idmap})")
            print(f"SUMH core: 30 ownership/error checks passed (old_idmap={old_idmap})")


if __name__ == "__main__":
    main()
