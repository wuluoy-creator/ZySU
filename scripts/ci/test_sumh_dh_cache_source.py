"""Actual DH/FOP/SOP bodies with deterministic cache/native-service boundaries.

Only the native-service model is adapted. Production bodies are unchanged.
This is not a Linux scheduler, VFS, KMI or device acceptance test.
"""

from test_sumh_dh_ownership_source import (
    ROOT, INPUTS, MODEL, core, function, replace_once,
)


def service(text, marker, replacement):
    return replace_once(text, function(text, marker), replacement)


EXTRA = r"""
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define FMODE_OPENED 1
#define FMODE_CREATED 2
#define DENTRY_NOT_SET ERR_PTR(-1)
static int inode_lock_calls,inode_unlock_calls;
static int first_native_complete,first_invalidation;
static void (*before_inode_lock)(struct inode *);
static void (*after_inode_unlock)(struct inode *);
static void inode_lock(struct inode *i){
    sleeping();inode_lock_calls++;
    if(config_depth||spin_depth||rcu_depth||inode_write_depth)BAD();
    if(before_inode_lock){void(*h)(struct inode *)=before_inode_lock;before_inode_lock=NULL;h(i);}
    if(i->readers||i->writer)BAD();i->writer=1;inode_write_depth++;
}
static void inode_unlock(struct inode *i){
    if(i->writer!=1||inode_write_depth!=1)BAD();
    i->writer=0;inode_write_depth--;inode_unlock_calls++;
    if(after_inode_unlock){void(*h)(struct inode *)=after_inode_unlock;after_inode_unlock=NULL;h(i);}
}
static bool d_unhashed(const struct dentry *d){return !d->hashed;}
static bool d_is_negative(const struct dentry *d){return !d->d_inode;}
static bool d_in_lookup(const struct dentry *d){return d->in_lookup;}
static int finish_calls;
static int finish_no_open(struct file *f,struct dentry *d){
    finish_calls++;if(IS_ERR(d))return PTR_ERR(d);f->f_path.dentry=d;return 0;
}
static int modeled_vnode_count,vnode_fail;
static struct inode modeled_vnodes[64];
static struct dentry *splice_result;
static int splice_error,splice_calls,rehash_errors;
static void (*before_publication)(struct dentry *);
static void (*after_publication)(struct dentry *);
"""


def model(version):
    text = MODEL
    text = replace_once(text, "static int bad,bad_line,spin_depth", "static int inode_write_depth;\nstatic int bad,bad_line,spin_depth")
    text = replace_once(text, "#define LINUX_VERSION_CODE KERNEL_VERSION(6,18,0)",
                        f"#define LINUX_VERSION_CODE KERNEL_VERSION({version},0)")
    text = replace_once(text, "int refs,id;bool synthetic;", "int refs,id,readers,writer;bool synthetic;")
    text = replace_once(text, "int refs,drops,id;const char *canonical;", "int refs,drops,id;bool hashed,in_lookup;const char *canonical;")
    text = replace_once(text, "struct file {struct path f_path;const struct file_operations *f_op;loff_t f_pos;};",
                        "struct file {struct path f_path;const struct file_operations *f_op;loff_t f_pos;unsigned f_mode;};")
    if version == "6,12":
        text = replace_once(text, "int(*d_revalidate)(struct inode *,const struct qstr *,struct dentry *,unsigned);",
                            "int(*d_revalidate)(struct dentry *,unsigned);")
    text = replace_once(text, "static void d_drop(struct dentry *d)", EXTRA + "\nstatic void d_drop(struct dentry *d)")
    text = service(text, "static void d_drop(", r"""static void d_drop(struct dentry *d){
        if(d->d_lock)BAD();d->drops++;d->hashed=false;
        if(inode_write_depth&&!first_invalidation)first_invalidation=++sequence;
    }""")
    text = service(text, "static void d_add(", r"""static void d_add(struct dentry *d,struct inode *i){
        if(before_publication){void(*h)(struct dentry *)=before_publication;before_publication=NULL;h(d);}
        if(d->hashed){rehash_errors++;BAD();}d->d_inode=i;d->hashed=true;
        if(after_publication){void(*h)(struct dentry *)=after_publication;after_publication=NULL;h(d);}
    }""")
    text = service(text, "static struct dentry *d_splice_alias(", r"""static struct dentry *d_splice_alias(struct inode *i,struct dentry *d){
        splice_calls++;if(d->hashed){rehash_errors++;BAD();}
        if(splice_error){iput(i);return ERR_PTR(splice_error);}
        if(splice_result){struct dentry *r=splice_result;splice_result=NULL;
            if(before_publication){void(*h)(struct dentry *)=before_publication;before_publication=NULL;h(r);}r->d_inode=i;r->hashed=true;
            if(after_publication){void(*h)(struct dentry *)=after_publication;after_publication=NULL;h(r);}return dget(r);}
        d_add(d,i);return NULL;
    }""")
    text = service(text, "static struct dentry *native_child_lookup(", r"""static struct dentry *native_child_lookup(struct dentry *p,const struct qstr *n){
        cache_lookups++;for(int i=0;i<cache_count;i++)if(cache_bindings[i].child->hashed&&cache_bindings[i].parent==p&&strlen(cache_bindings[i].name)==n->len&&!memcmp(cache_bindings[i].name,n->name,n->len))return dget(cache_bindings[i].child);return NULL;
    }""")
    text = service(text, "static struct inode *sumh_vnode_new(", r"""static struct inode *sumh_vnode_new(struct super_block *sb,const struct path *p,unsigned long ino,umode_t mode,u8 flags){
        (void)flags;vnode_calls++;last_vnode_source=p?*p:(struct path){0};
        if(during_vnode){void(*h)(const struct path *)=during_vnode;during_vnode=NULL;h(p);}
        if(vnode_fail)return NULL;
        if(modeled_vnode_count>=64){BAD();return NULL;}
        struct inode *i=&modeled_vnodes[modeled_vnode_count++];
        *i=(struct inode){.i_sb=sb,.i_ino=ino,.i_mode=mode,.refs=1,.synthetic=true};return i;
    }""")
    # A parent rwsem cannot be acquired below the DH mutex. The model knows all
    # mutexes, so this also detects acquisition under a different held mutex.
    text = replace_once(text, "static void mutex_lock(struct mutex *m)", "static int cache_mutex_depth;\nstatic void mutex_lock(struct mutex *m)")
    text = replace_once(text, "m->held++;}", "m->held++;cache_mutex_depth++;}")
    text = replace_once(text, "m->held--;if(after_mutex_unlock)", "m->held--;cache_mutex_depth--;if(after_mutex_unlock)")
    text = replace_once(text, "if(config_depth||spin_depth||rcu_depth||inode_write_depth)BAD();", "if(config_depth||spin_depth||rcu_depth||inode_write_depth||cache_mutex_depth)BAD();")
    # No grace wait can be placed under the parent lock.
    for marker in ("static void synchronize_rcu(", "static void synchronize_rcu_tasks(", "static void synchronize_srcu("):
        body = function(text, marker)
        text = replace_once(text, body, body.replace("sleeping();", "sleeping();if(inode_write_depth)BAD();", 1))
    return text


FIXTURES = r"""
static struct super_block fixture_sb;
static struct inode fixture_inodes[32];
static struct dentry fixture_dentries[32];
static struct address_space fixture_mappings[32];
static struct vfsmount fixture_mount;
static int native_lookups,native_opens,native_error,native_iterations;
static int native_create_count,native_lookup_flags,native_open_flags,native_open_mode;
static int native_result_kind;
static bool native_negative;
static struct dentry *native_alias;
static void (*native_completion)(void);
static struct dentry *fixture_native_lookup(struct inode *i,struct dentry *d,unsigned f){
    (void)i;native_lookups++;native_lookup_flags=f;
    if(native_completion){void(*h)(void)=native_completion;native_completion=NULL;h();}
    if(native_error)return ERR_PTR(native_error);
    struct dentry *r=native_alias?native_alias:d;
    r->d_inode=native_negative?NULL:&fixture_inodes[18];r->hashed=true;
    return native_alias?dget(r):NULL;
}
static int fixture_native_open(struct inode *i,struct dentry *d,struct file *f,unsigned flags,umode_t m){
    (void)i;native_opens++;native_open_flags=flags;native_open_mode=m;
    if(flags&O_CREAT)native_create_count++;
    if(native_completion){void(*h)(void)=native_completion;native_completion=NULL;h();}
    if(native_error)return native_error;
    struct dentry *r=native_alias?native_alias:d;
    r->d_inode=native_negative?NULL:&fixture_inodes[18];r->hashed=true;
    if(native_result_kind==1){f->f_mode|=FMODE_OPENED;f->f_path.dentry=dget(r);}
    else if(native_result_kind==2){f->f_path.dentry=dget(r);}
    else if(native_result_kind==3){f->f_path.dentry=DENTRY_NOT_SET;}
    else f->f_path.dentry=NULL;
    return 0;
}
static int fixture_native_iterate(struct file *f,struct dir_context *c){(void)f;(void)c;native_iterations++;return native_error;}
static struct inode_operations fixture_iop={fixture_native_lookup,fixture_native_open};
static struct file_operations fixture_fop={&foreign_module,fixture_native_iterate};
static struct super_operations fixture_sop={.destroy_inode=native_free_inode};
static struct path fixture_path(int n){return (struct path){&fixture_mount,&fixture_dentries[n]};}
static void fixture_init(void){
    native_path_count=0;model_sees=true;model_hides=true;model_su_enabled=false;model_su_visible=false;
    native_lookups=native_opens=native_error=native_iterations=native_create_count=0;
    native_lookup_flags=native_open_flags=native_open_mode=native_result_kind=0;
    native_negative=false;native_alias=NULL;native_completion=NULL;
    inode_lock_calls=inode_unlock_calls=first_native_complete=first_invalidation=0;
    before_inode_lock=NULL;after_inode_unlock=NULL;finish_calls=0;vnode_fail=0;
    splice_result=NULL;splice_error=splice_calls=rehash_errors=0;before_publication=NULL;after_publication=NULL;
    modeled_vnode_count=0;vnode_calls=0;during_vnode=NULL;
    fixture_sb=(struct super_block){.s_op=&fixture_sop,.s_flags=SB_BORN,.s_active=1};fixture_mount.refs=1;
    for(int i=0;i<32;i++){
        fixture_inodes[i]=(struct inode){.i_op=&fixture_iop,.i_fop=&fixture_fop,.i_sb=&fixture_sb,.i_mapping=&fixture_mappings[i],.i_ino=(unsigned long)(100+i),.i_mode=i<16?S_IFDIR:S_IFREG,.i_nlink=1,.refs=1,.id=i};
        fixture_dentries[i]=(struct dentry){.d_inode=&fixture_inodes[i],.d_parent=&fixture_dentries[0],.d_sb=&fixture_sb,.d_name={"entry",5,0},.refs=1,.id=i};
    }
    native_path("/",fixture_path(0),0);native_path("/one",fixture_path(1),0);native_path("/alias",fixture_path(1),0);native_path("/two",fixture_path(2),0);
    native_path("/source/a",fixture_path(16),0);native_path("/source/b",fixture_path(17),0);
    sumh_fop_override_init();sumh_sop_shadow_init();sumh_dirhijack_init();
}
"""


def base(root=ROOT, version="6,18"):
    source = core(root, api_services=False)
    identity = "struct sumh_dh_projection {" in source
    return f"#define CACHE_HAS_PROJECTION {int(identity)}\n" + replace_once(source, MODEL, model(version)) + FIXTURES
