"""Actual indexed ADD/HIDE/DEL/CLEAR and store/injection transactions."""

from test_sumh_dh_ownership_source import ROOT, core, FIXTURES, read, function, no_includes
from test_sumh_dh_ownership_cases import HELPERS

TYPES = "kernel/sumh/internal/sumh_types.h"
STORE = "kernel/sumh/core/sumh_store.h"
RUNTIME = "kernel/sumh/core/sumh_runtime.c"
IOCTL = "kernel/sumh/control/sumh_ioctl.c"
OVERLAY = "kernel/sumh/overlay/sumh_overlay.c"
INPUTS = (TYPES, STORE, RUNTIME, IOCTL, OVERLAY, "uapi/sumh.h")

SERVICES = r"""
typedef unsigned kuid_t,kgid_t;
typedef long long atomic64_t;
#define EALREADY 114
#define ELOOP 40
#define ESTALE 116
#define SUMH_HASH_BITS 3
#define SUMH_BLOOM_SIZE 1024
#define SUMH_MAX_LEN_PATHNAME 4096
#define SUMH_MOUNT_HIDE_MODE_NORMAL 0
#define SUMH_VNODE_INO_BASE 100
#define AS_FLAGS_SUMH_SPOOF_KSTAT 5
#define S_ISCHR(m) (((m)&S_IFMT)==0020000)
#define S_ISBLK(m) (((m)&S_IFMT)==0060000)
#define S_ISFIFO(m) (((m)&S_IFMT)==0010000)
#define S_ISSOCK(m) (((m)&S_IFMT)==0140000)
#define STATX_BASIC_STATS 1
#define STATX_BTIME 2
#define STATX_TYPE 4
#define STATX_MODE 8
#define STATX_NLINK 16
#define STATX_UID 32
#define STATX_GID 64
#define STATX_INO 128
#define STATX_SIZE 256
#define STATX_BLOCKS 512
#define AT_STATX_SYNC_AS_STAT 0
#define BITS_TO_LONGS(n) (((n)+8*sizeof(unsigned long)-1)/(8*sizeof(unsigned long)))
struct kstat {unsigned long ino,dev,rdev,blocks;unsigned result_mask,mode,nlink,uid,gid,blksize;loff_t size;};
static loff_t i_size_read(const struct inode *i){return i->i_size;}
static long long sumh_vnode_allocated_count,sumh_ino_next;
static int sumh_vnode_live_count,sumh_rule_count,sumh_hide_count,sumh_spoof_kstat_count,sumh_ino_map_count,sumh_feature_enabled_mask,sumh_mount_hide_mode;
static bool sumh_device_sources_enabled=true,sumh_stealth_enabled;
static unsigned long sumh_path_bloom[BITS_TO_LONGS(SUMH_BLOOM_SIZE)],sumh_hide_bloom[BITS_TO_LONGS(SUMH_BLOOM_SIZE)];
struct mutex sumh_config_mutex;
struct hlist_head sumh_paths[MODEL_BUCKETS],sumh_hide_paths[MODEL_BUCKETS],sumh_inject_dirs[MODEL_BUCKETS],sumh_xattr_sbs[MODEL_BUCKETS],sumh_merge_dirs[MODEL_BUCKETS],sumh_spoof_kstat_path[MODEL_BUCKETS],sumh_spoof_kstat_ino[MODEL_BUCKETS];
static struct hlist_head sumh_ino_map[MODEL_BUCKETS];
struct sumh_ino_map_entry {struct hlist_node node;struct rcu_head rcu;};
#define kfree_rcu(p,m) kfree(p)
static void bitmap_zero(unsigned long *p,int n){memset(p,0,BITS_TO_LONGS(n)*sizeof(*p));}
static void atomic64_set(long long *p,long long n){*p=n;}
static unsigned jhash(const char *s,unsigned n,unsigned seed){return full_name_hash(NULL,s,n)^seed;}
static char *d_path(const struct path *p,char *buf,int n){
    const char *s=p->dentry->canonical;
    if(!s)for(int i=0;i<native_path_count;i++)if(path_equal(&native_paths[i].path,p)&&!native_paths[i].error){s=native_paths[i].name;break;}
    if(!s)return ERR_PTR(-ENOENT);size_t len=strlen(s);if(len>=(size_t)n)return ERR_PTR(-ENAMETOOLONG);memcpy(buf,s,len+1);return buf;
}
static char *(*sumh_d_absolute_path)(const struct path *,char *,int)=d_path;
static int sumh_vfs_getattr_unprojected(const struct path *p,struct kstat *s,unsigned mask,unsigned flags){(void)mask;(void)flags;struct inode *i=d_inode(p->dentry);memset(s,0,sizeof(*s));s->ino=i->i_ino;s->mode=i->i_mode;s->nlink=i->i_nlink;s->dev=i->i_sb->s_dev;return 0;}
static unsigned long sumh_vnode_ino_alloc(unsigned long dev,unsigned long ino){return dev*1000+ino+1000;}
static unsigned long sumh_vnode_visible_dev(const char *s){(void)s;return 200;}
static int sumh_iop_mark_spoof(struct inode *i){set_bit(AS_FLAGS_SUMH_SPOOF_KSTAT,&i->i_mapping->flags);return 0;}
static bool sumh_hide_storage_parent(struct inode *i){(void)i;return false;}
struct sumh_xattr_sb_entry;
static void sumh_overlay_xattr_retire(struct sumh_xattr_sb_entry *e){(void)e;BAD();}
static void sumh_hide_rules_clear(void){}
static void sumh_kernel_build_clear(void){}
static void sumh_fake_mi_invalidate_all(void){}
"""


def dispatch_case(root, command, name):
    text = read(root, IOCTL)
    actual = function(text, "static SUMH_NOCFI int sumh_dispatch_cmd(")
    declarations = actual[actual.index("{") + 1:actual.index("\n\tif (cmd == SUMH_IOC_CLEAR_ALL)")]
    start = actual.index(f"\tcase {command}: {{")
    end = actual.find("\n\tcase ", start + 1)
    default = actual.find("\n\tdefault:", start + 1)
    if end < 0 or 0 <= default < end:
        end = default
    branch = actual[start:end]
    return f"\n#undef {command}\n#define {command} 1\nstatic int {name}(const char *input,const char *output){{" + declarations + r"""
    memset(&req,0,sizeof(req));
    src=kstrdup(input,GFP_KERNEL);target=output?kstrdup(output,GFP_KERNEL):NULL;
    if(!src||(output&&!target)){kfree(src);kfree(target);return -ENOMEM;}
""" + f"\n switch({command}){{\n" + branch + "\n }kfree(src);kfree(target);return ret;}\n"


def indexed_bodies(root=ROOT):
    runtime = read(root, RUNTIME)
    types = read(root, TYPES)
    layouts = types[types.index("struct sumh_inject_claim;"):types.index("struct sumh_name_list {")]
    header = no_includes(read(root, STORE))
    capture = runtime[runtime.index("static int SUMH_NOCFI sumh_entry_capture_paths("):runtime.index("static LLIST_HEAD(sumh_retired_entries)")]
    start = runtime.index("static LLIST_HEAD(sumh_retired_entries)")
    end = runtime.index("struct sumh_spoof_kstat_entry *\nsumh_spoof_kstat_lookup_by_path", start)
    store = runtime[start:end]
    for marker in ("void sumh_mark_inode_hidden(", "SUMH_NOCFI void sumh_clear_inode_flags_for_path(", "int sumh_store_upsert(", "void sumh_cleanup_locked("):
        store += function(runtime, marker) + "\n"
    ioctl = read(root, IOCTL)
    helpers = function(ioctl, "static int SUMH_NOCFI sumh_resolve_rule_path(")
    if "static int sumh_normalize_rule_key(" in ioctl:
        helpers += function(ioctl, "static int sumh_normalize_rule_key(")
    request = function(read(root, "uapi/sumh.h"), "struct sumh_syscall_arg {") + ";\n"
    wrappers = "\n".join(dispatch_case(root, cmd, name) for cmd, name in (
        ("SUMH_IOC_ADD_RULE", "add_rule"), ("SUMH_IOC_HIDE_RULE", "hide_rule"), ("SUMH_IOC_DEL_RULE", "del_rule")))
    actual = function(ioctl, "static SUMH_NOCFI int sumh_dispatch_cmd(")
    clear = function(actual, "\tif (cmd == SUMH_IOC_CLEAR_ALL)")
    wrappers += "\n#define SUMH_IOC_CLEAR_ALL 1\nstatic int clear_rules(void){unsigned cmd=SUMH_IOC_CLEAR_ALL;" + clear + "\nreturn -EINVAL;}\n"
    return SERVICES + layouts + header + capture + store + request + helpers + wrappers


CASES = r"""
static struct sumh_entry *stored(const char *name){struct sumh_entry *e;unsigned b;hash_for_each(sumh_paths,b,e,node)if(!strcmp(e->src,name))return e;return NULL;}
static struct sumh_hide_entry *hidden(const char *name){struct sumh_hide_entry *e;unsigned b;hash_for_each(sumh_hide_paths,b,e,node)if(!strcmp(e->path,name))return e;return NULL;}
static void producer_drain(void){sumh_store_drain();sumh_dh_drain_releases();}
static int producer_smoke(void){
    fixture_init();CHECK(add_rule("/one/file","/source/a")==0);
    struct sumh_entry *e=stored("/one/file");CHECK(e&&e->dh_claim&&e->inject_claim);
    CHECK(e->dh_claim->slot->dir->dir_inode==e->inject_claim->binding->inode);
    CHECK(del_rule("/one/file",NULL)==0&&!stored("/one/file"));
    sumh_store_drain();CHECK(callback_count>0&&sumh_sop_lookup_locked(&fixture_sb)->dh_clients==1);
    sumh_dh_drain_releases();CHECK(atomic_read(&sumh_sop_live)==0);
    CHECK(hide_rule("/one/hidden",NULL)==0&&sumh_hide_count==1);
    CHECK(clear_rules()==0&&sumh_hide_count==0&&sumh_rule_count==0&&!callback_count);
    return finish_case();
}
static int parent_probes;
static void rebind_second_parent(const char *name,unsigned flags,int result,const struct path *path){
    (void)flags;(void)path;
    if(!strcmp(name,"/one")&&!result&&++parent_probes==2){native_path("/one",fixture_path(2),0);after_named_path=NULL;}
}
static int shared_parent_case(void){
    fixture_init();parent_probes=0;after_named_path=rebind_second_parent;
    CHECK(add_rule("/one/file","/source/a")==0);after_named_path=NULL;
    struct sumh_entry *e=stored("/one/file");
    CHECK(parent_probes==2&&e&&e->dh_claim->slot->dir->dir_inode==&fixture_inodes[1]);
    CHECK(e->inject_claim->binding->inode==&fixture_inodes[1]);
    CHECK(del_rule("/one/file",NULL)==0);producer_drain();return finish_case();
}
static int replace_and_failure_case(void){
    fixture_init();CHECK(add_rule("/one/file","/source/a")==0);
    struct sumh_entry *old=stored("/one/file");struct path held=old->source_path;path_get(&held);
    struct sumh_dh_claim *old_claim=old->dh_claim;int count=sumh_rule_count;
    CHECK(add_rule("/one/file","/source/a")==0);
    struct sumh_entry *same=stored("/one/file");CHECK(same!=old&&same->dh_claim!=old_claim&&old_claim->state==DH_DETACHED&&sumh_rule_count==count);
    CHECK(add_rule("/one/file","/source/b")==0);
    struct sumh_entry *newer=stored("/one/file");CHECK(newer!=same&&newer->source_path.dentry==&fixture_dentries[17]&&held.dentry==&fixture_dentries[16]);
    native_path("/source/b",fixture_path(18),0);CHECK(newer->source_path.dentry==&fixture_dentries[17]);
    native_path("/one",fixture_path(1),-EIO);
    CHECK(add_rule("/one/file","/source/a")==-EIO&&stored("/one/file")==newer&&sumh_rule_count==count);
    native_path("/one",fixture_path(1),0);native_path("/source/a",fixture_path(16),-EACCES);
    CHECK(add_rule("/one/file","/source/a")==-EACCES&&stored("/one/file")==newer&&sumh_rule_count==count);
    native_path("/source/a",fixture_path(16),0);
    struct sumh_inject_inode *binding=newer->inject_claim->binding;unsigned users=binding->users;binding->users=UINT_MAX;
    CHECK(add_rule("/one/file","/source/a")==-EOVERFLOW&&stored("/one/file")==newer&&newer->dh_claim->state==DH_ATTACHED&&sumh_rule_count==count);binding->users=users;
    /* Required DH admission failure happens after source capture, preserving
       both old receipt sets; the SU physical slot owns this exact name. */
    struct sumh_dh_claim *su;CHECK(prepared("/one/su",SUMH_DH_OWNER_SU,0,909,&su)==0&&committed(su,NULL)==0);
    CHECK(add_rule("/one/su","/source/a")==-EPERM&&!stored("/one/su")&&winner(1,"su")==909&&sumh_rule_count==count);
    withdraw(su);CHECK(del_rule("/one/file",NULL)==0&&!stored("/one/file"));producer_drain();
    CHECK(!sumh_dirhijack_emits(&fixture_inodes[1],"file",4));path_put(&held);return finish_case();
}
static int hide_precedence_case(void){
    fixture_init();CHECK(add_rule("/one/file","/source/a")==0&&hide_rule("/one/file",NULL)==0);
    struct sumh_hide_entry *first=hidden("/one/file");CHECK(first&&hide_rule("/one/file",NULL)==0);
    struct sumh_hide_entry *last=hidden("/one/file");CHECK(last&&last!=first&&first->dh_claim->state==DH_DETACHED&&sumh_hide_count==1);
    CHECK(del_rule("/one",NULL)==-EBUSY&&stored("/one/file")&&hidden("/one/file"));
    CHECK(del_rule("/one/file",NULL)==0&&!stored("/one/file")&&hidden("/one/file")==last);
    CHECK(sumh_dirhijack_hidden(&fixture_inodes[1],"file",4));
    CHECK(del_rule("/one/file",NULL)==0&&!hidden("/one/file")&&!sumh_hide_count);producer_drain();return finish_case();
}
static int raw_delete_case(void){
    fixture_init();CHECK(add_rule("/missing//leaf","/source/a")==0&&stored("/missing//leaf"));
    CHECK(add_rule("/missing/leaf","/source/b")==0&&stored("/missing/leaf"));
    native_path("/missing//leaf",fixture_path(18),-EACCES);
    CHECK(del_rule("/missing//leaf",NULL)==0&&!stored("/missing//leaf")&&stored("/missing/leaf"));
    native_path("/missing/./leaf",fixture_path(18),-EIO);
    CHECK(del_rule("/missing/./leaf",NULL)==0&&!stored("/missing/leaf"));producer_drain();
    CHECK(add_rule("/one/leaf","/source/a")==0);
    /* Present parent canonicalizes ADD. Normalized lexical fallback removes
       the stored key despite today's failing native alias. */
    native_path("/one//leaf",fixture_path(18),-EACCES);
    CHECK(del_rule("/one//leaf",NULL)==0&&!stored("/one/leaf"));producer_drain();
    CHECK(add_rule("/real/item","/source/a")==0);
    fixture_dentries[18].canonical="/real/item";fixture_dentries[18].d_parent=&fixture_dentries[0];
    native_path("/link/../item",fixture_path(18),0);native_path("/item",fixture_path(19),-EACCES);
    CHECK(del_rule("/link/../item",NULL)==0&&!stored("/real/item"));producer_drain();return finish_case();
}
int main(void){int r;if((r=producer_smoke()))return r;if((r=shared_parent_case()))return r;if((r=replace_and_failure_case()))return r;if((r=hide_precedence_case()))return r;if((r=raw_delete_case()))return r;return 0;}
"""


def source(root=ROOT):
    return core(root) + indexed_bodies(root) + FIXTURES + HELPERS + CASES
