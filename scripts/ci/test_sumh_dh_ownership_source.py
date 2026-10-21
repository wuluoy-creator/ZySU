"""Source assembly for DH ownership tests; product bodies remain verbatim."""

from pathlib import Path
import re

from test_sumh_vfs_helpers import COMMON, ROOT
from test_sumh_dh_ownership_model import MODEL

DH = "kernel/sumh/core/sumh_dirhijack.c"
SOP = "kernel/sumh/core/sumh_sop_shadow.c"
FOP = "kernel/sumh/features/sumh_fop_override.c"
HEADERS = (
    "kernel/sumh/core/sumh_dirhijack.h",
    "kernel/sumh/core/sumh_sop_shadow.h",
    "kernel/sumh/features/sumh_fop_override.h",
)
INPUTS = (DH, SOP, FOP, *HEADERS, "kernel/sumh/core/sumh_vnode.h")


def read(root, path):
    return (Path(root) / path).read_text(encoding="utf-8")


def no_includes(text):
    return re.sub(r"^\s*#include[^\n]*\n", "", text, flags=re.M)


def function(text, marker):
    candidates = []
    for match in re.finditer(re.escape(marker), text):
        opening = text.find("{", match.start())
        if opening >= 0 and ";" not in text[match.start():opening]:
            candidates.append(match.start())
    if len(candidates) != 1:
        raise AssertionError(f"expected one definition, got {len(candidates)}: {marker!r}")
    start = candidates[0]
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise AssertionError(f"expected one source marker, got {text.count(old)}: {old[:100]!r}")
    return text.replace(old, new, 1)


EXTRA_SERVICES = r"""
static bool path_equal(const struct path *a,const struct path *b){return a->dentry==b->dentry&&a->mnt==b->mnt;}
static struct dentry *d_lookup(struct dentry *p,const struct qstr *n){return native_child_lookup(p,n);}
static unsigned long sumh_vnode_vpath_ino(const char *s){return full_name_hash(NULL,s,(unsigned)strlen(s));}
"""


API_SERVICES = r"""
/* Adjacent owner fixtures only. Cache publication tests use their stronger
 * parent-reader/native-publication scheduler instead of this API adapter. */
static int model_inode_locks;
static struct inode *model_locked_inode;
static void inode_lock(struct inode *i){
    sleeping();if(model_inode_locks||config_depth)BAD();
    model_inode_locks++;model_locked_inode=i;
}
static bool inode_trylock(struct inode *i){
    sleeping();if(model_inode_locks)return false;inode_lock(i);return true;
}
static void inode_unlock(struct inode *i){
    if(model_inode_locks!=1||model_locked_inode!=i)BAD();
    model_inode_locks--;model_locked_inode=NULL;
}
static int finish_no_open(struct file *f,struct dentry *d){
    if(IS_ERR(d))return PTR_ERR(d);f->f_path.dentry=d;return 0;
}
"""


def core(root=ROOT, su=False, api_services=True):
    headers = "\n".join(no_includes(read(root, h)) for h in HEADERS)
    model = MODEL
    if su:
        from test_sumh_dh_ownership_su import prefix
        for name in ("enabled", "visible", "current_ino", "is_inode", "is_path"):
            stub = function(model, f"static bool ksu_sucompat_vfs_{name}(")
            model = replace_once(model, stub, "")
        model += prefix(root)
    constants = "\n".join(re.findall(r"^#define SUMH_VNODE_F_[^\n]+", read(root, "kernel/sumh/core/sumh_vnode.h"), re.M))
    instrumented = replace_once(COMMON, "#define SUMH_NOCFI", "#define SUMH_NOCFI\nstatic const char *retired_name;static size_t retired_name_len;static int retired_name_reads;")
    instrumented = replace_once(instrumented, "void *memcpy(void *dst, const void *src, size_t n) {\n    char *d = dst; const char *s = src;", "void *memcpy(void *dst, const void *src, size_t n) {\n    if(retired_name&&(size_t)src>=(size_t)retired_name&&(size_t)src<(size_t)retired_name+retired_name_len)retired_name_reads++;\n    char *d = dst; const char *s = src;")
    return (instrumented + constants + "\n" + model + EXTRA_SERVICES
            + (API_SERVICES if api_services else "") + headers
            + "\n" + no_includes(read(root, FOP))
            + "\n" + no_includes(read(root, SOP))
            + "\n" + no_includes(read(root, DH)))


FIXTURES = r"""
static struct super_block fixture_sb;
static struct inode fixture_inodes[32];
static struct dentry fixture_dentries[32];
static struct address_space fixture_mappings[32];
static struct vfsmount fixture_mount;
static int native_lookups,native_opens,native_error,native_iterations;
static void (*native_iteration_hook)(void);
static struct dentry *fixture_native_lookup(struct inode *i,struct dentry *d,unsigned f){(void)i;(void)d;(void)f;native_lookups++;return native_error?ERR_PTR(native_error):NULL;}
static int fixture_native_open(struct inode *i,struct dentry *d,struct file *f,unsigned flags,umode_t m){(void)i;(void)d;(void)f;(void)flags;(void)m;native_opens++;return native_error;}
static int fixture_native_iterate(struct file *f,struct dir_context *c){(void)f;(void)c;native_iterations++;if(native_iteration_hook)native_iteration_hook();return native_error;}
static struct inode_operations fixture_iop={fixture_native_lookup,fixture_native_open};
static struct file_operations fixture_fop={&foreign_module,fixture_native_iterate};
static struct super_operations fixture_sop={.destroy_inode=native_free_inode};
static struct path fixture_path(int n){return (struct path){&fixture_mount,&fixture_dentries[n]};}
static void fixture_init(void){
    native_path_count=0;model_sees=true;model_hides=true;
    fixture_sb=(struct super_block){.s_op=&fixture_sop,.s_flags=SB_BORN,.s_active=1};
    fixture_mount.refs=1;
    for(int i=0;i<32;i++){
        fixture_inodes[i]=(struct inode){.i_op=&fixture_iop,.i_fop=&fixture_fop,.i_sb=&fixture_sb,.i_mapping=&fixture_mappings[i],.i_ino=(unsigned long)(100+i),.i_mode=i<16?S_IFDIR:S_IFREG,.i_nlink=1,.refs=1,.id=i};
        fixture_dentries[i]=(struct dentry){.d_inode=&fixture_inodes[i],.d_parent=&fixture_dentries[0],.d_sb=&fixture_sb,.d_name={"entry",5,0},.refs=1,.id=i};
    }
    native_path("/",fixture_path(0),0);native_path("/one",fixture_path(1),0);native_path("/alias",fixture_path(1),0);native_path("/two",fixture_path(2),0);
    native_path("/source/a",fixture_path(16),0);native_path("/source/b",fixture_path(17),0);
    sumh_fop_override_init();sumh_sop_shadow_init();sumh_dirhijack_init();
}
"""


def old_probe(root):
    """Smoke view of the retained old bodies; not an ownership pass claim."""
    return core(root) + FIXTURES + r"""
int main(void){
    fixture_init();
    struct path source=fixture_path(16);
    CHECK(sumh_dirhijack_add("/one/file",&source,501,0)==0);
    CHECK(sumh_dirhijack_del("/one/file")==0);
    sumh_dirhijack_exit();sumh_fop_override_exit();sumh_sop_shadow_exit();
    CHECK(!bad&&!live_allocations);
    CHECK(fixture_sb.s_active==1&&self_module.refs==1&&foreign_module.refs==1);
    return 0;
}
"""
