#!/usr/bin/env python3
"""Exercise BTF fields, runtime inode offsets and reopened wrapper ownership."""

import argparse
import os
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, function, run_test


PREAMBLE = r"""
typedef unsigned int u32;
typedef int s32;
#define EINVAL 22
#define ENOENT 2
#define EIO 5
#define ENOMEM 12
#define ERR_PTR(e) ((void *)(ssize_t)(e))
#define PTR_ERR(p) ((ssize_t)(p))
#define IS_ERR(p) ((size_t)(p) >= (size_t)-4095)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define READ_ONCE(p) (p)
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
#define __init
#define BTF_KIND_INT 1
#define BTF_KIND_PTR 2
#define BTF_KIND_STRUCT 4
struct ksu_btf_field { u32 offset, struct_size; };
"""


def btf_source():
    preamble = r"""
#define BTF_INFO_KIND(info) (((info) >> 24) & 0x1f)
#define BTF_INFO_KFLAG(info) ((info) >> 31)
#define BTF_MEMBER_BITFIELD_SIZE(offset) ((offset) >> 24)
#define BTF_MEMBER_BIT_OFFSET(offset) ((offset) & 0xffffff)
#define BTF_INT_OFFSET(info) (((info) >> 16) & 0xff)
#define BTF_INT_BITS(info) ((info) & 0xff)
#define BTF_INT_ENCODING(info) (((info) >> 24) & 0xf)
#define BTF_INT_CHAR 2
#define BTF_INT_BOOL 4
struct btf { int unused; } metadata;
struct btf_type { u32 name_off, info, size; };
struct btf_member { u32 name_off, type, offset; };
static struct btf_type structure;
static struct btf_member members[2];
static struct { struct btf_type type; u32 encoding; } member_type;
static int unavailable, lookup_error, bad_type, bad_member, bad_name, errors;
#define for_each_member(i, type, m) \
    for ((i) = 0, (m) = members; (i) < 2; (i)++, (m)++)
static struct btf *bpf_get_btf_vmlinux(void) {
    if (unavailable == 1) return NULL;
    if (unavailable == 2) return ERR_PTR(-EIO);
    return &metadata;
}
static s32 btf_find_by_name_kind(const struct btf *btf, const char *name, u32 kind) {
    if (btf != &metadata || strcmp(name, "lsm_blob_sizes") || kind != BTF_KIND_STRUCT) errors++;
    return lookup_error ? -ENOENT : 1;
}
static struct btf_type *btf_type_by_id(const struct btf *btf, s32 id) {
    if (btf != &metadata || id != 1) errors++;
    return bad_type ? NULL : &structure;
}
static const char *btf_name_by_offset(const struct btf *btf, u32 offset) {
    if (btf != &metadata || offset > 1) errors++;
    if (bad_name) return NULL;
    return offset ? "lbs_inode" : "lbs_backing_file";
}
static struct btf_type *btf_type_skip_modifiers(const struct btf *btf, u32 id, void *unused) {
    if (btf != &metadata || id != 2 || unused) errors++;
    return bad_member ? NULL : &member_type.type;
}
static void reset(void) {
    unavailable = lookup_error = bad_type = bad_member = bad_name = errors = 0;
    structure = (struct btf_type){0, BTF_KIND_STRUCT << 24, 40};
    members[0] = (struct btf_member){0, 2, 0};
    members[1] = (struct btf_member){1, 2, 96};
    member_type.type = (struct btf_type){0, BTF_KIND_INT << 24, 4};
    member_type.encoding = (1U << 24) | 32;
}
"""
    body = r"""
int main(void) {
    struct ksu_btf_field field;
    reset();
    CHECK(!ksu_btf_field("lsm_blob_sizes", "lbs_inode", BTF_KIND_INT, 4, &field));
    CHECK(field.offset == 12 && field.struct_size == 40 && !errors);
    /* New ABI inserts a field: obtain the runtime location, not a C offset. */
    members[1].offset = 128;
    CHECK(!ksu_btf_field("lsm_blob_sizes", "lbs_inode", BTF_KIND_INT, 4, &field));
    CHECK(field.offset == 16);
    unavailable = 1;
    CHECK(ksu_btf_field("lsm_blob_sizes", "lbs_inode", 1, 4, &field) == -ENOENT);
    unavailable = 2;
    CHECK(ksu_btf_field("lsm_blob_sizes", "lbs_inode", 1, 4, &field) == -EIO);
    reset(); lookup_error = 1;
    CHECK(ksu_btf_field("lsm_blob_sizes", "lbs_inode", 1, 4, &field) == -ENOENT);
    reset(); bad_type = 1;
    CHECK(ksu_btf_field("lsm_blob_sizes", "lbs_inode", 1, 4, &field) == -EINVAL);
    reset(); bad_member = 1;
    CHECK(ksu_btf_field("lsm_blob_sizes", "lbs_inode", 1, 4, &field) == -EINVAL);
    reset(); bad_name = 1;
    CHECK(ksu_btf_field("lsm_blob_sizes", "lbs_inode", 1, 4, &field) == -EINVAL);
    reset();
    CHECK(ksu_btf_field("lsm_blob_sizes", "missing", 1, 4, &field) == -ENOENT);
    static const u32 bad_offsets[] = {97, 104, 312, 328};
    for (size_t i = 0; i < sizeof(bad_offsets) / sizeof(bad_offsets[0]); i++) {
        reset(); members[1].offset = bad_offsets[i];
        CHECK(ksu_btf_field("lsm_blob_sizes", "lbs_inode", 1, 4, &field) == -EINVAL);
    }
    reset(); structure.info |= 1U << 31; members[1].offset |= 4U << 24;
    CHECK(ksu_btf_field("lsm_blob_sizes", "lbs_inode", 1, 4, &field) == -EINVAL);
    static const u32 bad_encodings[] = {16, (1U << 16) | 32, (BTF_INT_BOOL << 24) | 32,
                                       (BTF_INT_CHAR << 24) | 32};
    for (size_t i = 0; i < sizeof(bad_encodings) / sizeof(bad_encodings[0]); i++) {
        reset(); member_type.encoding = bad_encodings[i];
        CHECK(ksu_btf_field("lsm_blob_sizes", "lbs_inode", 1, 4, &field) == -EINVAL);
    }
    reset(); member_type.type.size = 8;
    CHECK(ksu_btf_field("lsm_blob_sizes", "lbs_inode", 1, 4, &field) == -EINVAL);
    reset(); member_type.type.info = BTF_KIND_PTR << 24;
    CHECK(ksu_btf_field("lsm_blob_sizes", "lbs_inode", 1, 4, &field) == -EINVAL);
    members[1].offset = 128;
    CHECK(!ksu_btf_field("lsm_blob_sizes", "lbs_inode", 2, sizeof(void *), &field));
    CHECK(!errors);
    return 0;
}
"""
    return COMMON + PREAMBLE + preamble + function("kernel/infra/btf_parser.c", "int ksu_btf_field") + body


def inode_source():
    preamble = r"""
#define MODULE 1
#define CONFIG_DEBUG_INFO_BTF 1
struct inode { void *i_security; };
struct inode_security_struct { struct inode *inode; u32 sid; };
static u32 inode_blob_offset;
static struct { int pad[4]; int lbs_inode; } selinux_blob_sizes;
static int btf_error, read_error, reads, errors;
static int ksu_btf_field(const char *type, const char *name, u32 kind, u32 size,
                         struct ksu_btf_field *field) {
    if (strcmp(type, "lsm_blob_sizes") || strcmp(name, "lbs_inode") || kind != 1 || size != 4) errors++;
    field->offset = 16; field->struct_size = sizeof(selinux_blob_sizes); return btf_error;
}
static int copy_from_kernel_nofault(void *dst, const void *src, size_t len) {
    reads++;
    if (src != &selinux_blob_sizes.lbs_inode || len != 4) errors++;
    if (read_error) return read_error;
    memcpy(dst, src, len); return 0;
}
"""
    body = r"""
int main(void) {
    struct { char padding[16]; struct inode_security_struct security; } blob;
    struct inode inode = { &blob }, other;
    blob.security.inode = &inode; blob.security.sid = 99;
    selinux_blob_sizes.lbs_inode = 16;
    CHECK(!ksu_selinux_inode_init() && reads == 1 && !errors);
    CHECK(ksu_selinux_inode(&inode) == &blob.security);
    blob.security.inode = &other;
    CHECK(PTR_ERR(ksu_selinux_inode(&inode)) == -EINVAL);
    inode.i_security = NULL;
    CHECK(!ksu_selinux_inode(&inode));
    btf_error = -ENOENT;
    CHECK(ksu_selinux_inode_init() == -ENOENT && reads == 1);
    btf_error = 0; read_error = -EIO;
    CHECK(ksu_selinux_inode_init() == -EIO);
    read_error = 0; selinux_blob_sizes.lbs_inode = -8;
    CHECK(ksu_selinux_inode_init() == -EINVAL);
    selinux_blob_sizes.lbs_inode = 3;
    CHECK(ksu_selinux_inode_init() == -EINVAL && !errors);
    return 0;
}
"""
    handlers = function("kernel/selinux/inode.c", "int __init ksu_selinux_inode_init")
    handlers += function("kernel/selinux/inode.c", "struct inode_security_struct *ksu_selinux_inode")
    return COMMON + PREAMBLE + preamble + handlers + body


def wrapper_source():
    preamble = r"""
struct inode { int unused; };
struct file_operations { int unused; };
struct dentry { void *d_fsdata; };
struct path { struct dentry *dentry; };
struct file { struct path f_path; int f_flags; void *private_data; const struct file_operations *f_op; };
struct ksu_file_wrapper { struct file *orig; struct file_operations ops; };
static struct file original;
static struct path original_path;
static struct ksu_file_wrapper wrapper;
static struct { int files; } task, *current = &task;
static int credentials, refs, open_error, wrapper_error, closes, puts, errors;
static void *current_cred(void) { return &credentials; }
static struct file *dentry_open(struct path *path, int flags, const void *cred) {
    if (path != &original_path || flags != 3 || cred != &credentials) errors++;
    if (open_error) return ERR_PTR(open_error);
    refs++; return &original;
}
static struct ksu_file_wrapper *ksu_create_file_wrapper(struct file *file) {
    if (file != &original || refs != 1) errors++;
    if (wrapper_error) return ERR_PTR(wrapper_error);
    refs++; wrapper.orig = file; return &wrapper;
}
static void filp_close(struct file *file, int files) {
    if (file != &original || files != current->files || refs != 1) errors++;
    refs--; closes++;
}
static void fput(struct file *file) {
    if (file != &original || refs != 2) errors++;
    refs--; puts++;
}
static const struct file_operations *fops_get(struct file_operations *ops) { return ops; }
static void replace_fops(struct file *file, const struct file_operations *ops) { file->f_op = ops; }
"""
    body = r"""
int main(void) {
    struct dentry dentry = { &original_path };
    struct file file = { .f_path = { &dentry }, .f_flags = 3 };
    open_error = -EIO;
    CHECK(ksu_wrapper_open(NULL, &file) == -EIO);
    CHECK(!refs && !puts && !closes && !file.private_data && !errors);
    open_error = 0; wrapper_error = -ENOMEM;
    CHECK(ksu_wrapper_open(NULL, &file) == -ENOMEM);
    CHECK(!refs && !puts && closes == 1 && !file.private_data && !errors);
    wrapper_error = 0;
    CHECK(!ksu_wrapper_open(NULL, &file));
    CHECK(refs == 1 && puts == 1 && closes == 1 && !errors);
    CHECK(file.private_data == &wrapper && file.f_op == &wrapper.ops);
    return 0;
}
"""
    return COMMON + PREAMBLE + preamble + function("kernel/infra/file_wrapper.c", "static int ksu_wrapper_open") + body


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler, "selinux-btf-field", btf_source())
    run_test(compiler, "selinux-inode-offset", inode_source())
    run_test(compiler, "file-wrapper-reopen", wrapper_source())
