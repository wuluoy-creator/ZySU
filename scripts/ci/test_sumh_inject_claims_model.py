"""Kernel services shared by actual injection ownership and producer tests.

The pre-existing identity model supplies only allocation, path, hash and source
metadata services. Claim/index/store algorithms are extracted by _source.py.
Strict adapters keep old regression fixtures usable without a second algorithm.
"""
import re


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise AssertionError(f"model marker is not unique: {old[:90]!r}")
    return text.replace(old, new, 1)


def remove_function(text, marker):
    start = text.index(marker)
    brace = text.index("{", start)
    level, end = 1, brace + 1
    while level:
        level += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[:start] + text[end:]


PREAMBLE = r"""
#if defined(_WIN64) && defined(__clang__)
#pragma clang diagnostic ignored "-Wpointer-to-int-cast"
#endif
typedef long long loff_t;
typedef unsigned long long u64;
typedef unsigned int kuid_t,kgid_t;
typedef int atomic_t;
struct kstat {int unused;};
struct llist_node {struct llist_node *next;};
struct llist_head {struct llist_node *first;};
struct work_struct {void (*fn)(struct work_struct *);bool pending;};
#define LLIST_HEAD(n) struct llist_head n={NULL}
#define DECLARE_WORK(n,f) struct work_struct n={f,false}
#define LOOKUP_DIRECTORY 2
#define EOVERFLOW 75
#define ESHUTDOWN 108
#define EBUSY 16
#define EAGAIN 11
#define ENAMETOOLONG 36
#define UINT_MAX 4294967295U
#define INT_MAX 2147483647
#define SB_BORN 1
#define AS_FLAGS_SUMH_DIR_HAS_INJECT 1
#define AS_FLAGS_SUMH_HIDE 2
#define AS_FLAGS_SUMH_SPOOF_KSTAT 3
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define PTR_ERR(p) ((int)(ssize_t)(p))
#define WARN_ON_ONCE(v) ((v)?(bad++,warnings++,1):0)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#ifndef MODEL_BUCKETS
#define MODEL_BUCKETS 1
#endif
#define DEFINE_HASHTABLE(n,b) struct hlist_head n[MODEL_BUCKETS]
static int warnings,iter_lock,read_depth,callback_depth,worker_depth;
static int fop_calls,fop_error,sb_gets,sb_puts,inode_gets,inode_puts;
static int path_lookups,path_renders,path_error,render_error,dh_deletes,dh_delete_error;
static int clear_by_path[8],inject_sets,inject_clears;
static bool invalid_render,vnode_source;
static void (*release_hook)(void);
struct rw_semaphore {int readers;};
struct super_block {unsigned long s_flags;atomic_t s_active;struct rw_semaphore s_umount;};
struct address_space {unsigned long flags;};
static struct super_block native_supers[64];
static struct address_space native_mappings[64];
static struct super_block *last_put_sb;
"""

SERVICES = r"""
#define hash_for_each_possible(t,p,m,k) hlist_for_each_entry(p,&(t)[hash_min(k,SUMH_HASH_BITS)],m)
#define hash_for_each_possible_rcu(t,p,m,k) hash_for_each_possible(t,p,m,k)
#define hash_for_each_safe(t,b,n,p,m) for((b)=0;(b)<(int)ARRAY_SIZE(t);(b)++) for((p)=typed_entry((t)[b].first,__typeof__(*(p)),m);(p)&&((n)=(p)->m.next,1);(p)=typed_entry(n,__typeof__(*(p)),m))
#define hash_add(t,n,k) hlist_add_head_rcu(n,&(t)[hash_min(k,SUMH_HASH_BITS)])
#define hash_add_rcu(t,n,k) hash_add(t,n,k)
#define hash_del(n) hlist_del_rcu(n)
#define hash_del_rcu(n) hlist_del_rcu(n)
#define hash_empty(t) model_hash_empty(t,ARRAY_SIZE(t))
#define kfree_rcu(e,r) kfree(e)
static bool model_hash_empty(struct hlist_head *t,size_t n){for(size_t i=0;i<n;i++)if(t[i].first)return false;return true;}
static int table_count(const struct hlist_head *t){int n=0;for(int b=0;b<MODEL_BUCKETS;b++)for(struct hlist_node *p=t[b].first;p;p=p->next)n++;return n;}
static void atomic_dec(int *p){(*p)--;}
static void atomic_set(int *p,int v){*p=v;}
static int atomic_read(const int *p){return *p;}
static void atomic64_set(long long *p,long long v){*p=v;}
static bool atomic_inc_not_zero(int *p){if(read_depth!=1||lock_depth||callback_depth)bad++;if(!*p)return false;(*p)++;sb_gets++;return true;}
static void down_read(struct rw_semaphore *p){if(lock_depth||read_depth||callback_depth)bad++;p->readers++;read_depth++;}
static void up_read(struct rw_semaphore *p){if(p->readers!=1||read_depth!=1)bad++;p->readers--;read_depth--;}
static void deactivate_super(struct super_block *sb){if(lock_depth||read_depth||callback_depth||sb->s_active<=0||last_put_sb!=sb)bad++;last_put_sb=NULL;sb->s_active--;sb_puts++;}
static void clear_bit(unsigned long bit,unsigned long *p){if(bit==AS_FLAGS_SUMH_DIR_HAS_INJECT)inject_clears++;*p&=~(1UL<<(bit%32));}
static bool test_bit(unsigned long bit,const unsigned long *p){return (*p&(1UL<<(bit%32)))!=0;}
static void bitmap_zero(unsigned long *p,int size){(void)size;*p=0;}
static void cond_resched(void){if(lock_depth||read_depth||callback_depth)bad++;}
static void llist_add(struct llist_node *n,struct llist_head *h){n->next=h->first;h->first=n;}
static struct llist_node *llist_del_all(struct llist_head *h){struct llist_node *n=h->first;h->first=NULL;return n;}
#define llist_for_each_safe(n,next_,head) for(n=head;(n)&&((next_)=(n)->next,1);n=next_)
static bool schedule_work(struct work_struct *w){bool was=w->pending;w->pending=true;return !was;}
static struct {struct rcu_head *head;void (*fn)(struct rcu_head *);} callbacks[8192];
static int callback_count,callback_runs,worker_runs,barriers;
static void call_rcu(struct rcu_head *r,void (*fn)(struct rcu_head *)){
    if(callback_count>=8192){bad++;return;}retire_count++;callbacks[callback_count].head=r;callbacks[callback_count++].fn=fn;
    /* An immediately scheduled RCU callback runs in its own context even if
       the writer still owns config. Do not inherit writer-local lock depth. */
    if(retire_immediately){int writer_lock=lock_depth;lock_depth=0;callback_depth++;fn(r);callback_depth--;lock_depth=writer_lock;callback_count--;callback_runs++;}
}
static void rcu_barrier(void){if(lock_depth||read_depth||callback_depth)bad++;barriers++;while(callback_count){struct rcu_head *h=callbacks[0].head;void (*fn)(struct rcu_head *)=callbacks[0].fn;for(int i=1;i<callback_count;i++)callbacks[i-1]=callbacks[i];callback_count--;callback_depth++;fn(h);callback_depth--;callback_runs++;}retire_count=0;}
static void flush_work(struct work_struct *w){if(lock_depth||callback_depth||read_depth)bad++;int guard=0;while(w->pending&&guard++<8192){w->pending=false;worker_depth++;worker_runs++;w->fn(w);worker_depth--;}if(w->pending)bad++;}
static int sumh_fop_install(struct inode *i){if(lock_depth||read_depth||callback_depth||!i||i->refs<=1)bad++;fop_calls++;return fop_error;}
static bool sumh_vnode_is_ours(struct inode *i){return vnode_source||i->synthetic;}
static void native_inode(struct inode *i,int n){i->i_mapping=&native_mappings[n];i->i_sb=&native_supers[n];i->refs=1;native_supers[n]=(struct super_block){SB_BORN,1,{0}};}
struct path_override {const char *name;struct path path;int error;};
static struct path_override path_overrides[32];
static int override_count;
static void path_override(const char *name,struct path p,int error){for(int i=0;i<override_count;i++)if(!strcmp(path_overrides[i].name,name)){path_overrides[i]=(struct path_override){name,p,error};return;}path_overrides[override_count++]=(struct path_override){name,p,error};}
static struct hlist_head sumh_inject_dirs[MODEL_BUCKETS],sumh_merge_dirs[MODEL_BUCKETS],sumh_hide_paths[MODEL_BUCKETS],sumh_xattr_sbs[MODEL_BUCKETS];
static struct hlist_head sumh_spoof_kstat_path[MODEL_BUCKETS],sumh_spoof_kstat_ino[MODEL_BUCKETS],sumh_ino_map[MODEL_BUCKETS];
static bool sumh_stealth_enabled;
static int sumh_feature_enabled_mask,sumh_mount_hide_mode,sumh_hide_count,sumh_vnode_live_count,sumh_spoof_kstat_count,sumh_ino_map_count;
static long long sumh_ino_next;
static unsigned long sumh_hide_bloom[1];
#define SUMH_MOUNT_HIDE_MODE_NORMAL 0
#define SUMH_VNODE_INO_BASE 100
struct sumh_spoof_kstat_entry {char *target_pathname;unsigned long target_ino;struct hlist_node path_node,ino_node;struct rcu_head rcu;};
struct sumh_ino_map_entry {struct hlist_node node;struct rcu_head rcu;};
static void sumh_overlay_xattr_retire(struct sumh_xattr_sb_entry *e){(void)e;bad++;}
static int sumh_dirhijack_del(const char *s){(void)s;dh_deletes++;return dh_delete_error;}
static void sumh_clear_inode_flags_for_path(const char *s,unsigned bit){clear_by_path[bit]++;struct path p;if(!kern_path(s,LOOKUP_FOLLOW,&p)){if(d_inode(p.dentry)&&d_inode(p.dentry)->i_mapping)clear_bit(bit,&d_inode(p.dentry)->i_mapping->flags);path_put(&p);}}
"""


def model(original, layouts):
    """Adapt old service doubles. Never replace production claim algorithms."""
    text = original
    for marker in (
        "static void ihold(", "static void iput(", "static int kern_path(",
        "static char *d_path(", "static void sumh_entry_release_source(",
        "static void sumh_entry_free_rcu(", "static void call_rcu(",
        "static void sumh_mark_dir_has_inject(", "static void sumh_add_inject_rule(",
    ):
        text = remove_function(text, marker)
    text = re.sub(r"static struct rcu_head \*retired\[\d+\];", "", text)
    start = text.index("struct sumh_entry {")
    end = text.index("\n};", start) + 3
    text = text[:start] + layouts + text[end:]
    text = re.sub(r"heap\[\d+\]", "heap[4096]", text)
    text = re.sub(r"used\[\d+\]", "used[4096]", text)
    text = text.replace("i<64", "i<4096").replace("i<256", "i<4096")
    text = replace_once(text, "static int bad,allocs,fail_at,live,peak;", "static int bad,allocs,fail_at,live,peak,lock_depth;")
    text = replace_once(text, "(void)flags; allocs++;", "(void)flags;if(lock_depth||read_depth||callback_depth)bad++; allocs++;")
    text = replace_once(text, "if(!p)return;for(int i=0;i<4096;i++)", "if(!p)return;if(lock_depth||read_depth||iter_lock)bad++;for(int i=0;i<4096;i++)")
    text = replace_once(text, "struct inode {umode_t i_mode;int id,marks;};", "struct inode {umode_t i_mode;int id,marks;unsigned long i_rdev;struct address_space *i_mapping;struct super_block *i_sb;int refs;bool synthetic;};")
    text = replace_once(text, "struct dentry {struct inode *inode;int refs;};", "struct dentry {struct inode *inode;int refs;struct dentry *d_parent;const char *canonical;};")
    text = replace_once(text, "static struct inode old_i,new_i,link_i,visible_i;", "static struct inode old_i,new_i,link_i,visible_i,parent_i;")
    text = replace_once(text, "static struct dentry old_d,new_d,link_d,visible_d;", "static struct dentry old_d,new_d,link_d,visible_d,parent_d;")
    text = replace_once(text, "static struct hlist_head sumh_paths[1];", "static struct hlist_head sumh_paths[MODEL_BUCKETS];")
    text = replace_once(text, "sumh_config_mutex,sumh_rule_count,lock_depth;", "sumh_config_mutex,sumh_rule_count;")
    text = replace_once(text, "#define hash_min(h,b) 0", "#define hash_min(h,b) ((unsigned long)(h)&(MODEL_BUCKETS-1))")
    text = replace_once(text, "h->first=n;publishes++;", "h->first=n;for(int b=0;b<MODEL_BUCKETS;b++)if(h==&sumh_paths[b])publishes++;model_merge_publish(h);")
    text = replace_once(text, "*b|=1UL<<(n%32);", "if(n==AS_FLAGS_SUMH_DIR_HAS_INJECT)inject_sets++;*b|=1UL<<(n%32);")
    text = replace_once(text, "static void hlist_add_head_rcu(", "static void model_merge_publish(struct hlist_head *h);\nstatic void hlist_add_head_rcu(")
    text = replace_once(text, "static int sumh_entry_capture_source(", "static void ihold(struct inode *i);\nstatic void iput(struct inode *i);\nstatic int kern_path(const char *,int,struct path *);\nstatic char *d_path(const struct path *,char *,size_t);\nstatic int sumh_entry_capture_source(")
    text = replace_once(text, "e->source_path_valid=true;", "e->source_path_valid=true;sumh_vnode_live_count++;")
    # Source capture references this counter before the extended services.
    text = text.replace("static int bad,allocs,fail_at,live,peak,lock_depth;", "static int bad,allocs,fail_at,live,peak,lock_depth,sumh_vnode_live_count;")
    services = SERVICES.replace(",sumh_vnode_live_count,sumh_spoof_kstat_count", ",sumh_spoof_kstat_count")
    return PREAMBLE + text + services + NATIVE_SERVICES + DH_BOUNDARY


NATIVE_SERVICES = r"""
static void model_merge_publish(struct hlist_head *h){if(iter_lock)bad++;for(int b=0;b<MODEL_BUCKETS;b++)if(h==&sumh_merge_dirs[b])publishes++;}
static void ihold(struct inode *i){if(lock_depth||read_depth||callback_depth||!i||i->refs<=0||!i->i_sb||i->i_sb->s_active<=0){bad++;return;}i->refs++;inode_gets++;}
static void iput(struct inode *i){if(lock_depth||read_depth||callback_depth||!i||i->refs<=1||!i->i_sb||i->i_sb->s_active<=0){bad++;return;}i->refs--;inode_puts++;last_put_sb=i->i_sb;if(release_hook){void (*hook)(void)=release_hook;release_hook=NULL;hook();}}
static int kern_path(const char *s,int flags,struct path *p){
    path_lookups++;if(iter_lock||callback_depth||worker_depth)bad++;
    for(int i=0;i<override_count;i++)if(!strcmp(s,path_overrides[i].name)){if(path_overrides[i].error)return path_overrides[i].error;*p=path_overrides[i].path;goto found;}
    if(!strcmp(s,"/source")){target_lookups++;if(lookup_error)return lookup_error;*p=current_target;if(replace_on_lookup)current_target=(struct path){&new_m,&new_d};}
    else if(!strcmp(s,"/visible/file")){if(path_error)return path_error;*p=(struct path){&visible_m,&visible_d};}
    else if(!strcmp(s,"/visible")){if(path_error)return path_error;*p=(struct path){&visible_m,&parent_d};}
    else return -ENOENT;
found:
    if((flags&LOOKUP_DIRECTORY)&&(!d_inode(p->dentry)||!S_ISDIR(d_inode(p->dentry)->i_mode)))return -ENOTDIR;
    path_get(p);return 0;
}
static char *d_path(const struct path *p,char *b,size_t n){path_renders++;if(render_error)return ERR_PTR(render_error);const char *s=invalid_render?"relative":p->dentry->canonical;if(!s)s="/visible/file";if(strlen(s)+1>n)return ERR_PTR(-ENAMETOOLONG);strcpy(b,s);return b;}
"""

# Deliberately a service boundary for the injection/source regression suite.
# No slot, winner ordering, directory registry or release algorithm is modeled:
# test_sumh_dh_ownership executes those actual bodies independently.
DH_BOUNDARY = r"""
enum sumh_dh_owner_kind {SUMH_DH_OWNER_MAPPING,SUMH_DH_OWNER_SHADOW,SUMH_DH_OWNER_LEGACY_HIDE};
struct sumh_dh_spec {enum sumh_dh_owner_kind kind;const struct path *source;unsigned long v_ino;u8 flags;};
struct sumh_dh_claim {struct path source;unsigned long ino;u8 flags;bool attached;};
struct sumh_dh_change {struct sumh_dh_claim *add,*remove;};
struct sumh_dh_retire {bool initialized;};
static int dh_live;
static int (*dh_path_lookup)(const char *,int,struct path *)=kern_path;
static void sumh_dh_retire_init(struct sumh_dh_retire *r,bool bulk){(void)bulk;r->initialized=true;}
static void sumh_dh_retire_finish(struct sumh_dh_retire *r){if(lock_depth||!r->initialized)bad++;}
static void sumh_dh_claim_put(struct sumh_dh_claim *c){if(!c)return;if(lock_depth||c->attached)bad++;if(c->source.dentry)path_put(&c->source);dh_live--;kfree(c);}
static void sumh_dh_abort(struct sumh_dh_claim *c){sumh_dh_claim_put(c);}
static void sumh_dh_drain_releases(void){if(lock_depth)bad++;}
static int sumh_dh_prepare_path(const char *s,const struct sumh_dh_spec *spec,struct sumh_dh_claim **out,struct path *parent){
    *out=NULL;if(parent)*parent=(struct path){0};if(lock_depth)bad++;
    if(!dh_enabled)return -EOPNOTSUPP;if(shadow_error)return shadow_error;
    char *copy=kstrdup(s,GFP_KERNEL);if(!copy)return -ENOMEM;char *slash=strrchr(copy,'/');
    if(!slash||!slash[1]){kfree(copy);return -EINVAL;}if(slash==copy)slash[1]=0;else *slash=0;
    struct path observed={0};int ret=dh_path_lookup(copy,LOOKUP_FOLLOW|LOOKUP_DIRECTORY,&observed);kfree(copy);
    if(ret&&!(ret==-ENOENT&&spec->kind==SUMH_DH_OWNER_MAPPING))return ret;
    struct sumh_dh_claim *c=kzalloc(sizeof(*c),GFP_KERNEL);if(!c){if(observed.dentry)path_put(&observed);return -ENOMEM;}
    c->ino=spec->v_ino;c->flags=spec->flags;if(spec->source){c->source=*spec->source;path_get(&c->source);}
    if(parent)*parent=observed;else if(observed.dentry)path_put(&observed);dh_live++;*out=c;return 0;
}
static int sumh_dh_change_begin(struct sumh_dh_claim *add,struct sumh_dh_claim *remove,struct sumh_dh_change *c){
    if(!add)return -EINVAL;if(add->attached)bad++;c->add=add;c->remove=remove;return 0;
}
static void sumh_dh_change_commit(struct sumh_dh_change *c,struct sumh_dh_retire *r){
    if(!lock_depth||!r->initialized)bad++;if(c->remove)c->remove->attached=false;c->add->attached=true;
    if(c->add->source.dentry)sumh_dirhijack_add("",&c->add->source,c->add->ino,c->add->flags);
}
static void sumh_dh_change_end(void){}
static void sumh_dh_detach(struct sumh_dh_claim *c,struct sumh_dh_retire *r){if(!r->initialized)bad++;if(c){if(!c->attached)bad++;c->attached=false;}}
"""


SETUP_EXTRA = r"""
    warnings=read_depth=callback_depth=worker_depth=iter_lock=dh_live=0;dh_path_lookup=kern_path;
    fop_calls=fop_error=sb_gets=sb_puts=inode_gets=inode_puts=0;
    path_lookups=path_renders=path_error=render_error=dh_deletes=dh_delete_error=0;
    invalid_render=vnode_source=false;last_put_sb=NULL;release_hook=NULL;override_count=0;
    memset(clear_by_path,0,sizeof(clear_by_path));inject_sets=inject_clears=0;
    callback_count=callback_runs=worker_runs=barriers=sumh_vnode_live_count=0;
    memset(native_mappings,0,sizeof(native_mappings));memset(native_supers,0,sizeof(native_supers));
    memset(sumh_paths,0,sizeof(sumh_paths));memset(sumh_merge_dirs,0,sizeof(sumh_merge_dirs));
    memset(sumh_inject_dirs,0,sizeof(sumh_inject_dirs));memset(sumh_inject_inodes,0,sizeof(sumh_inject_inodes));
    memset(sumh_hide_paths,0,sizeof(sumh_hide_paths));sumh_hide_count=0;
    sumh_retired_entries.first=sumh_retired_merges.first=sumh_retired_inject_inodes.first=NULL;
    sumh_store_free_work.pending=false;
    parent_i=(struct inode){S_IFDIR,5,0};parent_d=(struct dentry){&parent_i,0,&parent_d,"/visible"};
    old_d.canonical=new_d.canonical=link_d.canonical="/source";
    visible_d.canonical="/visible/file";visible_d.d_parent=&parent_d;
    old_d.d_parent=new_d.d_parent=link_d.d_parent=&parent_d;
    native_inode(&old_i,0);native_inode(&new_i,1);native_inode(&link_i,2);native_inode(&visible_i,3);native_inode(&parent_i,4);
"""


def fixtures(old_cases):
    setup = old_cases[:old_cases.index("static void cleanup(void)")]
    pos = setup.rindex("}")
    setup = setup[:pos] + SETUP_EXTRA + setup[pos:]
    return setup + r"""
static void cleanup(void){
    if(shadow.dentry){path_put(&shadow);memset(&shadow,0,sizeof(shadow));}
    struct sumh_dh_retire retired;sumh_dh_retire_init(&retired,true);
    mutex_lock(&sumh_config_mutex);sumh_cleanup_locked(&retired);mutex_unlock(&sumh_config_mutex);sumh_dh_retire_finish(&retired);sumh_store_drain();sumh_dh_drain_releases();
}
static int clean(void){
    return bad==0&&live==0&&!dh_live&&!warnings&&!lock_depth&&!read_depth&&!callback_depth&&!worker_depth&&path_gets==path_puts
        &&!old_m.refs&&!new_m.refs&&!visible_m.refs&&!old_d.refs&&!new_d.refs&&!link_d.refs&&!visible_d.refs&&!parent_d.refs
        &&sb_gets==sb_puts&&inode_gets==inode_puts&&!sumh_vnode_live_count&&!callback_count
        &&!sumh_store_free_work.pending&&!sumh_retired_entries.first&&!sumh_retired_merges.first&&!sumh_retired_inject_inodes.first
        &&hash_empty(sumh_paths)&&hash_empty(sumh_merge_dirs)&&hash_empty(sumh_inject_dirs)&&hash_empty(sumh_inject_inodes);
}
"""
