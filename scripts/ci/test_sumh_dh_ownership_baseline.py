"""Retained-old-source counterexamples; never emulate unavailable receipts.

These call the old public anonymous operations exactly as distinct producers
did. The old implementation compiling is required; only the designated semantic
CHECK may reject a retained source view.
"""

from test_sumh_dh_ownership_source import core, FIXTURES, function, read, no_includes

CASES = {
    "alias-owner-erasure": (r"""
    struct path a=fixture_path(16),b=fixture_path(17);
    CHECK(sumh_dirhijack_add("/one/file",&a,501,0)==0);
    CHECK(sumh_dirhijack_add("/alias/file",&b,502,0)==0);
    CHECK(sumh_dirhijack_del("/one/file")==0);
    struct sumh_dh_iop *meta=sumh_dh_iop_of(&fixture_inodes[1]);
    struct sumh_dh_child *child=meta?sumh_dh_find_child(meta->dir,"file",4,full_name_hash(&fixture_inodes[1],"file",4)):NULL;
    CHECK(child&&child->source.dentry==b.dentry&&child->v_ino==502);
""", "child&&child->source.dentry==b.dentry&&child->v_ino==502"),
    "parent-rebind-detaches-wrong-owner": (r"""
    struct path a=fixture_path(16),b=fixture_path(17);
    CHECK(sumh_dirhijack_add("/one/file",&a,501,0)==0);
    CHECK(sumh_dirhijack_add("/two/file",&b,502,0)==0);
    native_path("/one",fixture_path(2),0);
    CHECK(sumh_dirhijack_del("/one/file")==0);
    struct sumh_dh_iop *old=sumh_dh_iop_of(&fixture_inodes[1]);
    struct sumh_dh_iop *other=sumh_dh_iop_of(&fixture_inodes[2]);
    CHECK(!old&&other);
""", "!old&&other"),
    "virtual-top-dependency-leak": (r"""
    struct path a=fixture_path(16);
    CHECK(sumh_dirhijack_add("/one/top/file",&a,501,0)==0);
    int deleted=sumh_dirhijack_del("/one/top/file");
    CHECK(deleted==0&&sumh_dh_iop_of(&fixture_inodes[1])==NULL);
""", "deleted==0&&sumh_dh_iop_of(&fixture_inodes[1])==NULL"),
}


def views(root):
    common = core(root) + FIXTURES
    results = [(name, common + "\nint main(void){fixture_init();\n" + code + "\nreturn 0;}\n", target)
               for name, (code, target) in CASES.items()]
    from test_sumh_dh_ownership_producers import indexed_bodies, CASES as PRODUCER_CASES
    from test_sumh_dh_ownership_materializer import bodies, CASES as MAT_CASES
    indexed = core(root) + indexed_bodies(root)
    fixtures = FIXTURES + function(PRODUCER_CASES, "static struct sumh_entry *stored(")
    hook = r"""
static int old_parent_probes,old_rebind_at;
static bool old_parent_fail;
static void old_change_parent(const char *name,unsigned flags,int error,const struct path *p){
    (void)flags;(void)p;if(!strcmp(name,"/one")&&!error&&++old_parent_probes==old_rebind_at){native_path("/one",fixture_path(2),old_parent_fail?-EIO:0);after_named_path=NULL;}
}
"""
    for failure in (False, True):
        case = r"""
int main(void){fixture_init();old_rebind_at=2;old_parent_fail=FAIL;after_named_path=old_change_parent;
    int result=add_rule("/one/file","/source/a");
    struct sumh_entry *row=stored("/one/file");
    TARGET
    return 0;}
"""
        target = "result==-EIO&&!row" if failure else "row&&sumh_dh_iop_of(row->inject_claim->binding->inode)"
        case = case.replace("FAIL", "true" if failure else "false").replace("TARGET", f"CHECK({target});")
        results.append(("direct-required-failure" if failure else "direct-split-parent", indexed + fixtures + hook + case, target))
    materialized = indexed + bodies(root) + fixtures + hook + function(MAT_CASES, "static void setup_tree(")
    for failure in (False, True):
        case = r"""
int main(void){fixture_init();setup_tree();old_rebind_at=1;old_parent_fail=FAIL;after_named_path=old_change_parent;
    struct path source=fixture_path(4);
    int result=sumh_materialize_merge("/one","/module",&source,0);
    struct sumh_entry *row=stored("/one/first");
    TARGET
    return 0;}
"""
        target = "result==-EIO&&!row" if failure else "row&&sumh_dh_iop_of(row->inject_claim->binding->inode)"
        case = case.replace("FAIL", "true" if failure else "false").replace("TARGET", f"CHECK({target});")
        results.append(("materialized-required-failure" if failure else "materialized-split-parent", materialized + case, target))
    old_su = core(root, su=True) + no_includes(read(root, "kernel/feature/sucompat_vfs.c")) + FIXTURES + r"""
int main(void){fixture_init();su_next_ino=10000;native_path("/system/bin",fixture_path(1),0);
    CHECK(ksu_sucompat_vfs_init()==0&&ksu_sucompat_vfs_refresh()==0&&su_ino);
    ksu_sucompat_vfs_exit();CHECK(!su_ready&&!su_enabled&&!su_ino);return 0;}
"""
    results.append(("su-exit-stale-inode", old_su, "!su_ready&&!su_enabled&&!su_ino"))
    return results
