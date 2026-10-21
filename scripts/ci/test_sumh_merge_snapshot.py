"""Run the production SUMH MERGE collector with bounded kernel API doubles.

Models pinned mount+dentry ownership, errors and source-level ordering; not real VFS permission,
RCU, kernel RB balancing, lock ordering, KMI or device acceptance.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test


def limits():
    text = (ROOT / "kernel/sumh/internal/sumh_base.h").read_text(encoding="utf-8")
    names = ("SUMH_ITERATE_PATH_BUF", "SUMH_MERGE_MAX_TARGETS", "SUMH_MERGE_MAX_NAMES",
             "SUMH_MERGE_MAX_RECORDS", "SUMH_MERGE_MAX_PASSES", "SUMH_MERGE_MEMORY_BUDGET")
    return "#define PATH_MAX 4096\n" + "\n".join(
        re.search(rf"^#define {name} .+$", text, re.MULTILINE).group(0) for name in names) + "\n"


MODEL = r"""
typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef struct { unsigned int refs; } refcount_t;
typedef unsigned int dev_t;
typedef unsigned short umode_t;
typedef long long loff_t;
typedef unsigned int gfp_t;
#define EIO 5
#define ENOENT 2
#define E2BIG 7
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EINVAL 22
#define ENOTDIR 20
#define ENAMETOOLONG 36
#define EOPNOTSUPP 95
#define ESTALE 116
#define GFP_KERNEL 1
#define GFP_ATOMIC 2
#define NAME_MAX 255
#define DT_CHR 2
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10
#define S_IFMT 0170000
#define S_ISDIR(m) (((m) & S_IFMT) == 0040000)
#define S_ISCHR(m) (((m) & S_IFMT) == 0020000)
#define LOOKUP_FOLLOW 1
#define O_RDONLY 0
#define O_DIRECTORY 0x10000
#define SIZE_MAX ((size_t)-1)
#define READ_ONCE(x) (x)
#define unlikely(x) (x)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ERR_PTR(v) ((void *)(ssize_t)(v))
#define PTR_ERR(v) ((ssize_t)(v))
#define IS_ERR(v) ((ssize_t)(v) < 0)
#define IS_ERR_OR_NULL(v) (!(v) || IS_ERR(v))
#define container_of(p,t,m) ((t *)((char *)(p) - __builtin_offsetof(t,m)))
static void *memchr(const void *p, int value, size_t n) {
    const unsigned char *s = p; for (size_t i=0;i<n;i++) if(s[i]==(unsigned char)value) return (void *)(s+i); return NULL;
}
static int memcmp(const void *a,const void *b,size_t n) {
    const unsigned char *x=a,*y=b; for(size_t i=0;i<n;i++) if(x[i]!=y[i]) return x[i]-y[i]; return 0;
}
static char *strchr(const char *s,int n) { do { if(*s==n) return (char *)s; } while(*s++); return NULL; }
struct list_head { struct list_head *next,*prev; };
#define LIST_HEAD(name) struct list_head name={&name,&name}
static void INIT_LIST_HEAD(struct list_head *h) { h->next=h->prev=h; }
static void list_add(struct list_head *n,struct list_head *h) { n->next=h->next;n->prev=h;h->next->prev=n;h->next=n; }
static void list_add_tail(struct list_head *n,struct list_head *h) { n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n; }
static void list_del(struct list_head *n) { n->prev->next=n->next;n->next->prev=n->prev; }
static bool list_empty(const struct list_head *h) { return h->next==h; }
#define list_for_each_entry(p,h,m) for(p=container_of((h)->next,__typeof__(*p),m);&p->m!=(h);p=container_of(p->m.next,__typeof__(*p),m))
#define list_for_each_entry_safe(p,t,h,m) for(p=container_of((h)->next,__typeof__(*p),m),t=container_of(p->m.next,__typeof__(*p),m);&p->m!=(h);p=t,t=container_of(t->m.next,__typeof__(*t),m))
struct rb_node { struct rb_node *rb_left,*rb_right; };
struct rb_root { struct rb_node *rb_node; };
#define RB_ROOT ((struct rb_root){NULL})
#define rb_entry(p,t,m) container_of(p,t,m)
static void rb_link_node(struct rb_node *n,struct rb_node *p,struct rb_node **link) { (void)p;n->rb_left=n->rb_right=NULL;*link=n; }
static void rb_insert_color(struct rb_node *n,struct rb_root *r) { (void)n;(void)r; }
struct hlist_node { struct hlist_node *next; };
struct hlist_head { struct hlist_node *first; };
#define hlist_for_each_entry_rcu(p,h,m) for(struct hlist_node *cursor=(h)->first;cursor&&((p)=container_of(cursor,__typeof__(*p),m),1);cursor=cursor->next)
#define hash_for_each_rcu(table,b,p,m) for((b)=0;(b)<(int)ARRAY_SIZE(table);(b)++) hlist_for_each_entry_rcu(p,&table[b],m)
#define hash_min(x,b) (0)
#define SUMH_HASH_BITS 1
static u32 full_name_hash(const void *p,const char *name,size_t len) { (void)p;(void)name;return (u32)len; }
struct super_block { dev_t s_dev; };
struct mapping { unsigned long flags; };
struct inode { struct super_block *i_sb; umode_t i_mode; unsigned long i_ino; dev_t i_rdev; struct mapping *i_mapping; int ours,locked; };
struct qstr { const char *name; int len; };
#define QSTR_INIT(n,l) {n,l}
struct dentry { struct inode *inode; struct qstr d_name; int refs,object; };
struct vfsmount { int refs,identity; };
struct path { struct dentry *dentry; struct vfsmount *mnt; };
struct dir_context { bool (*actor)(struct dir_context *,const char *,int,loff_t,u64,unsigned int);loff_t pos; };
static bool dir_emit(struct dir_context *ctx,const char *name,int len,u64 ino,unsigned int type) { return ctx->actor(ctx,name,len,ctx->pos,ino,type); }
struct file;
struct file_operations { int (*iterate_shared)(struct file *,struct dir_context *); };
struct file { struct path f_path;const struct file_operations *f_op;loff_t f_pos;int live; };
struct cred { int refs; };
static struct cred credential;
static int init_task;
struct sumh_entry { char *src;u64 visible_ino;bool source_stat_valid;umode_t source_mode;unsigned int type;struct hlist_node node; };
struct rcu_head { int unused; };
struct llist_node { int unused; };
PRODUCTION_MERGE_ENTRY
struct sumh_inject_entry { char *dir;struct hlist_node node; };
static struct hlist_head sumh_paths[1],sumh_merge_dirs[1],sumh_inject_dirs[1];
static int rcu_depth,srcu_depth,bad_context,live_allocations,allocation_attempts,allocation_successes,allocation_frees;
static int fail_at,resolve_error,resolve_error_at,open_error,open_error_at,lock_busy,lookup_error,lookup_negative,child_change;
static int resolve_calls,open_calls,iterate_calls,lookup_calls,trylock_calls,render_calls,target_actor_calls,source_actor_calls;
static int target_lock_depth,clear_at_capture,cleared,replace_before,replace_after,alias_error,alias_bad,cond_calls;
static size_t arena_used,live_bytes,peak_bytes;
static unsigned char arena[64*1024*1024];
struct alloc_header { size_t bytes;unsigned int live,guard; };
static void *kmalloc(size_t bytes,gfp_t flags) {
    allocation_attempts++;
    if((rcu_depth&&flags!=GFP_ATOMIC)||(!rcu_depth&&flags!=GFP_KERNEL)) bad_context++;
    if(fail_at==allocation_attempts) return NULL;
    size_t total=(sizeof(struct alloc_header)+bytes+1+15)&~(size_t)15;
    if(total>sizeof(arena)-arena_used) { bad_context++;return NULL; }
    struct alloc_header *h=(void *)(arena+arena_used);arena_used+=total;
    h->bytes=bytes;h->live=1;h->guard=0x42face;
    unsigned char *p=(void *)(h+1);memset(p,0xa5,bytes);p[bytes]=0x5a;
    allocation_successes++;live_allocations++;live_bytes+=bytes;if(live_bytes>peak_bytes)peak_bytes=live_bytes;return p;
}
static void *kzalloc(size_t bytes,gfp_t flags) { void *p=kmalloc(bytes,flags);if(p)memset(p,0,bytes);return p; }
static void *kmalloc_array(size_t count,size_t size,gfp_t flags) { if(size&&count>SIZE_MAX/size)return NULL;return kmalloc(count*size,flags); }
static void kfree(void *p) {
    if(!p)return;struct alloc_header *h=((struct alloc_header *)p)-1;
    if(h->guard!=0x42face||!h->live||((unsigned char *)p)[h->bytes]!=0x5a) { bad_context++;return; }
    h->live=0;live_allocations--;live_bytes-=h->bytes;allocation_frees++;memset(p,0xdd,h->bytes);
}
static char *kstrndup(const char *p,size_t len,gfp_t flags) { char *s=kmalloc(len+1,flags);if(s){memcpy(s,p,len);s[len]=0;}return s; }
static char *kstrdup(const char *p,gfp_t flags) { return kstrndup(p,strlen(p),flags); }
static struct inode *d_inode(const struct dentry *d) { return d?d->inode:NULL; }
static struct inode *file_inode(const struct file *f) { return d_inode(f->f_path.dentry); }
static struct dentry *dget(struct dentry *d) { if(d)d->refs++;return d; }
static void dput(struct dentry *d) { if(!d||d->refs<=0)bad_context++;else d->refs--; }
static void path_get(const struct path *p) { if(!p||!p->dentry||!p->mnt){bad_context++;return;}dget(p->dentry);p->mnt->refs++; }
static void path_put(const struct path *p) { if(rcu_depth)bad_context++;if(!p||!p->dentry||!p->mnt){bad_context++;return;}dput(p->dentry);if(p->mnt->refs<=0)bad_context++;else p->mnt->refs--; }
static bool sumh_vnode_is_ours(const struct inode *inode) { return inode&&inode->ours; }
static u64 sumh_vnode_source_ino(dev_t dev,u64 ino) { return 1000000ULL+(u64)dev*10000+ino; }
static void cond_resched(void) { if(rcu_depth||target_lock_depth)bad_context++;cond_calls++; }
static const struct cred *get_task_cred(void *task) { (void)task;credential.refs++;return &credential; }
static void put_cred(const struct cred *c) { if(c!=&credential||credential.refs<=0)bad_context++;else credential.refs--; }
#define MODEL_TARGETS 1030
#define MODEL_RECORDS 12
struct record { char name[NAME_MAX+2];int len;u64 ino;unsigned int type;bool projected; };
struct object { struct super_block sb;struct mapping mapping;struct inode inode;struct dentry dentry;struct vfsmount mount;char path[PATH_MAX];struct record records[MODEL_RECORDS];int count,batch,iterate_error,error_after,no_progress,empty_progress,generated,generated_kind,specialized; };
static struct object objects[MODEL_TARGETS+2];
static struct file opened[MODEL_TARGETS+2],source_file;
static struct sumh_merge_entry rules[MODEL_TARGETS];
static bool rule_holds[MODEL_TARGETS];
static char rule_text[MODEL_TARGETS][PATH_MAX];
static struct sumh_entry exact[8];
static char exact_text[8][PATH_MAX];
static struct sumh_inject_entry marker;
static struct inode child_inode;
static struct dentry child_dentry;
static const char *alias_path="/visible";
static int rule_count,exact_count;
static void drop_rule_path(int index) {
    if(rule_holds[index]){path_put(&rules[index].target_path);rule_holds[index]=false;}
    rules[index].target_path=(struct path){0};
}
static void bind_rule_path(int index,struct dentry *dentry,struct vfsmount *mount) {
    drop_rule_path(index);rules[index].target_path=(struct path){dentry,mount};
    path_get(&rules[index].target_path);rule_holds[index]=true;
}
static void rcu_read_lock(void) { rcu_depth++; }
static void rcu_read_unlock(void) {
    if(rcu_depth!=1)bad_context++;rcu_depth--;
    if(clear_at_capture&&!cleared){cleared=1;sumh_paths[0].first=sumh_merge_dirs[0].first=sumh_inject_dirs[0].first=NULL;
        for(int i=0;i<rule_count;i++){drop_rule_path(i);memset(rule_text[i],'!',PATH_MAX-1);rule_text[i][PATH_MAX-1]=0;rules[i].target=NULL;rules[i].src=NULL;}
        for(int i=0;i<exact_count;i++){memset(exact_text[i],'!',PATH_MAX-1);exact_text[i][PATH_MAX-1]=0;exact[i].src=NULL;exact[i].visible_ino=0;}}
}
static char *d_path(const struct path *path,char *buffer,int size) {
    render_calls++;if(rcu_depth||path->dentry!=source_file.f_path.dentry||path->mnt!=source_file.f_path.mnt)bad_context++;
    if(alias_error)return ERR_PTR(-alias_error);
    if(alias_bad==1)return NULL;if(alias_bad==2)return buffer-1;if(alias_bad==3)return buffer+size;
    if(alias_bad==4){memcpy(buffer,"relative",9);return buffer;}if(alias_bad==5){memset(buffer,'/',(size_t)size);return buffer;}
    if(alias_bad==6){buffer[size-1]='/';return buffer+size-1;}
    size_t len=strlen(alias_path);if(len>=(size_t)size)return ERR_PTR(-ENAMETOOLONG);
    char *out=buffer+size-len-1;memcpy(out,alias_path,len+1);return out;
}
static int kern_path(const char *text,unsigned int flags,struct path *out) {
    (void)flags;resolve_calls++;if(rcu_depth||target_lock_depth||!text)bad_context++;
    if(resolve_error&&resolve_calls==resolve_error_at)return -resolve_error;
    for(int i=1;i<=rule_count;i++)if(!strcmp(text,objects[i].path)){
        int selected=(replace_before||(replace_after&&open_calls))&&i==1?rule_count+1:i;
        *out=(struct path){&objects[selected].dentry,&objects[selected].mount};path_get(out);return 0;
    }
    return -ENOENT;
}
static struct file *dentry_open(const struct path *path,int flags,const struct cred *cred) {
    (void)flags;open_calls++;if(rcu_depth||target_lock_depth||cred!=&credential)bad_context++;
    if(open_error&&open_calls==open_error_at)return ERR_PTR(-open_error);
    for(int i=0;i<MODEL_TARGETS+2;i++)if(!opened[i].live){opened[i]=(struct file){.f_path=*path,.live=1};path_get(path);return &opened[i];}
    return ERR_PTR(-ENOMEM);
}
static void fput(struct file *f) { if(!f||!f->live)bad_context++;else{path_put(&f->f_path);f->live=0;} }
static bool inode_trylock(struct inode *inode) { trylock_calls++;if(rcu_depth||target_lock_depth)bad_context++;if(lock_busy)return false;if(inode->locked)bad_context++;inode->locked=1;return true; }
static void inode_unlock(struct inode *inode) { if(!inode->locked)bad_context++;inode->locked=0; }
static struct dentry *lookup_child(const char *name,struct dentry *parent,int length) {
    lookup_calls++;if(rcu_depth||target_lock_depth||!parent->inode->locked)bad_context++;
    if(lookup_error)return ERR_PTR(-lookup_error);
    struct object *o=&objects[parent->object];
    for(int i=0;i<o->count;i++)if(o->records[i].len==length&&!memcmp(o->records[i].name,name,(size_t)length)){
        child_inode=(struct inode){.i_mode=0020000,.i_ino=(unsigned long)o->records[i].ino,.i_rdev=0};
        if(child_change==1)child_inode.i_ino++;if(child_change==2)child_inode.i_mode=0100000;if(child_change==3)child_inode.i_rdev=1;
        child_dentry=(struct dentry){lookup_negative?NULL:&child_inode,{name,length},1,-1};return &child_dentry;
    }
    return ERR_PTR(-ENOENT);
}
static struct dentry *(*sumh_lookup_one_len)(const char *,struct dentry *,int)=lookup_child;
bool sumh_emit_projected_dirent(struct dir_context *,const char *,int,u64,unsigned int);
static int emit_specialized(struct dir_context *,struct object *);
static int iterate_dir(struct file *file,struct dir_context *ctx) {
    if(rcu_depth||!file->live)bad_context++;iterate_calls++;
    struct object *o=&objects[file->f_path.dentry->object];
    if(o->iterate_error&&!o->error_after)return -o->iterate_error;
    ctx->pos=file->f_pos;target_lock_depth++;int batch=o->batch?o->batch:MODEL_RECORDS,emitted=0;
    int total=o->generated?o->generated:o->count;
    if(o->specialized&&ctx->pos>=total){int result=emit_specialized(ctx,o);target_lock_depth--;file->f_pos=ctx->pos;return result;}
    if(o->empty_progress){ctx->pos++;goto finish;}
    while(ctx->pos<total&&emitted++<batch){
        struct record generated,*r;
        if(o->generated){memset(&generated,0,sizeof(generated));generated.type=DT_REG;generated.ino=100+(u64)ctx->pos;
            if(o->generated_kind==0){generated.name[0]='.';generated.len=1;}
            else{generated.len=o->generated_kind==2?255:8;memset(generated.name,'x',(size_t)generated.len);
                u64 n=(u64)ctx->pos;for(int j=0;j<7;j++){generated.name[j]=(char)('A'+n%26);n/=26;}generated.name[generated.len]=0;}
            r=&generated;
        }else r=&o->records[ctx->pos];
        target_actor_calls++;
        bool accepted=r->projected?sumh_emit_projected_dirent(ctx,r->name,r->len,r->ino,r->type):ctx->actor(ctx,r->name,r->len,ctx->pos,r->ino,r->type);
        if(!accepted)break;
        if(!o->no_progress)ctx->pos++;else break;
    }
finish:
    target_lock_depth--;file->f_pos=ctx->pos;
    if(o->iterate_error&&o->error_after)return -o->iterate_error;
    return 0;
}
static void label(char *out,int n) { out[0]='/';out[1]='t';out[2]=(char)('A'+n/26);out[3]=(char)('A'+n%26);out[4]=0; }
static void setup(int count) {
    arena_used=live_bytes=peak_bytes=0;live_allocations=allocation_attempts=allocation_successes=allocation_frees=0;
    rcu_depth=srcu_depth=bad_context=fail_at=resolve_error=open_error=lock_busy=lookup_error=lookup_negative=child_change=0;
    resolve_error_at=open_error_at=1;resolve_calls=open_calls=iterate_calls=lookup_calls=trylock_calls=render_calls=target_actor_calls=source_actor_calls=0;
    clear_at_capture=cleared=replace_before=replace_after=alias_error=alias_bad=target_lock_depth=cond_calls=0;credential.refs=0;
    alias_path="/visible";sumh_lookup_one_len=lookup_child;rule_count=count;exact_count=0;
    child_dentry.refs=0;
    memset(opened,0,sizeof(opened));memset(rules,0,sizeof(rules));memset(rule_holds,0,sizeof(rule_holds));memset(exact,0,sizeof(exact));
    for(int i=0;i<=count+1;i++){memset(&objects[i],0,sizeof(objects[i]));struct object *o=&objects[i];
        o->sb.s_dev=(dev_t)(i+1);o->inode=(struct inode){.i_sb=&o->sb,.i_mode=0040000,.i_ino=(unsigned long)(i+1),.i_mapping=&o->mapping};
        o->dentry=(struct dentry){&o->inode,{"target",6},1,i};o->mount=(struct vfsmount){1,i};label(o->path,i);o->batch=1;}
    source_file=(struct file){.f_path={&objects[0].dentry,&objects[0].mount}};
    marker=(struct sumh_inject_entry){.dir="/visible"};sumh_inject_dirs[0].first=&marker.node;sumh_paths[0].first=NULL;
    sumh_merge_dirs[0].first=count?&rules[0].node:NULL;
    for(int i=0;i<count;i++){memcpy(rule_text[i],objects[i+1].path,strlen(objects[i+1].path)+1);rules[i]=(struct sumh_merge_entry){.src="/visible",.target=rule_text[i],.node={i+1<count?&rules[i+1].node:NULL}};bind_rule_path(i,&objects[i+1].dentry,&objects[i+1].mount);}
}
static void record(int target,const char *name,u64 ino,unsigned int type,bool projected) {
    struct object *o=&objects[target];struct record *r=&o->records[o->count++];memset(r,0,sizeof(*r));
    r->len=(int)strlen(name);memcpy(r->name,name,(size_t)r->len+1);r->ino=ino;r->type=type;r->projected=projected;
}
static void exact_rule(const char *path,u64 ino,unsigned int type) {
    int i=exact_count++;memcpy(exact_text[i],path,strlen(path)+1);exact[i]=(struct sumh_entry){.src=exact_text[i],.visible_ino=ino,.type=type};
    if(i)exact[i-1].node.next=&exact[i].node;else sumh_paths[0].first=&exact[i].node;
}
static bool clean(void) {
    if(live_allocations||live_bytes||rcu_depth||target_lock_depth||credential.refs||bad_context)return false;
    for(int i=0;i<=rule_count+1;i++){
        int dentries=1,mounts=1;
        for(int j=0;j<rule_count;j++)if(rule_holds[j]){
            dentries+=rules[j].target_path.dentry==&objects[i].dentry;
            mounts+=rules[j].target_path.mnt==&objects[i].mount;
        }
        if(objects[i].dentry.refs!=dentries||objects[i].mount.refs!=mounts||objects[i].inode.locked)return false;
    }
    for(int i=0;i<MODEL_TARGETS+2;i++)if(opened[i].live)return false;
    return true;
}
"""


def production():
    text = (ROOT / "kernel/sumh/overlay/sumh_overlay.c").read_text(encoding="utf-8")
    start = text.index("/* ======================================================================\n * Part 10b:")
    end = text.index("/* Materialize merge targets", start)
    return text[start:end]


def fixture():
    merge = function("kernel/sumh/internal/sumh_types.h", "struct sumh_merge_entry {") + ";\n"
    names = function("kernel/sumh/internal/sumh_types.h", "struct sumh_name_list {") + ";\n"
    index = "\n".join(function("kernel/sumh/internal/sumh_name_index.h", marker) for marker in (
        "static inline int sumh_name_compare", "static inline bool sumh_name_index_contains",
        "static inline void sumh_name_index_add", "static inline void sumh_name_index_init"))
    return COMMON + limits() + MODEL.replace("PRODUCTION_MERGE_ENTRY", merge) + names + index + production() + specialized()


def specialized():
    path = "kernel/sumh/core/sumh_dirhijack.c"
    preamble = r"""
#define SUMH_DH_POS_SIG 0x6B6DULL
#define SUMH_DH_EMIT_BATCH 8
#define SUMH_VNODE_F_DIR 1
#define SUMH_VNODE_F_LNK 2
PRODUCTION_DH_VIEW_TYPES
/* Fixed published slot for the stream collector; ownership transitions have
 * their own whole-body suite. Snapshot interpretation is production code. */
struct sumh_dh_child { struct hlist_node node;struct sumh_dh_payload *winner;unsigned int attached,hide_users;int lock;u16 name_len;char name[NAME_MAX+1]; };
struct sumh_dh_dir { struct inode *dir_inode;struct hlist_head children; };
static void spin_lock(int *lock) { if(*lock||!rcu_depth)bad_context++;(*lock)++; }
static void spin_unlock(int *lock) { if(*lock!=1||!rcu_depth)bad_context++;(*lock)--; }
#define rcu_dereference_protected(p,condition) (p)
static bool sumh_dh_child_emitted(struct sumh_dh_view *v,struct inode *i) { (void)v;(void)i;return true; }
"""
    preamble = preamble.replace("PRODUCTION_DH_VIEW_TYPES", "\n".join(
        function(path, marker) + ";" for marker in (
            "struct sumh_dh_payload {", "struct sumh_dh_view {")))
    parts = "\n".join(function(path, marker) for marker in (
        "static bool sumh_dh_read_view(",
        "static inline bool sumh_dh_virtual_pos(", "static inline loff_t sumh_dh_pack_pos(",
        "static inline u32 sumh_dh_unpack_pos("))
    parts += "\n" + function(path, "struct sumh_dh_emit_entry {") + ";\n"
    parts += function(path, "static int sumh_dh_emit_children(")
    parts += r"""
static int emit_specialized(struct dir_context *ctx,struct object *object) {
    struct sumh_dh_payload payload={.v_ino=777};
    struct sumh_dh_child child={.winner=&payload,.attached=1,.name_len=9,.name="projected"};
    struct sumh_dh_dir directory={&object->inode,{&child.node}};
    return sumh_dh_emit_children(ctx,&directory);
}
"""
    return preamble + parts


def source():
    return fixture() + TESTS


INTEGRATION = r"""
#define SUMH_MAGIC_POS 0x1000000000000000ULL
#define CLIENT_POS 0x6B6D000000000000LL
#define AS_FLAGS_SUMH_DIR_HAS_HIDDEN 0
#define AS_FLAGS_SUMH_DIR_HAS_INJECT 1
#define AS_FLAGS_SUMH_HIDE 2
#define atomic_long_read(p) (*(p))
#define atomic_read(p) (*(p))
#define atomic64_inc(p) (++*(p))
#define smp_load_acquire(p) (*(p))
#define task_tgid_vnr(p) (17)
#define test_bit(bit,p) ((*(p)>>(bit))&1)
enum sumh_policy_scope { SUMH_POLICY_SCOPE_NONE,SUMH_POLICY_SCOPE_VIEW,SUMH_POLICY_SCOPE_SPOOF };
typedef int (*sumh_fop_iterate_client_fn)(struct file *,struct dir_context *,const struct file_operations *,void *);
struct sumh_fop_template { const struct file_operations *orig_fop;struct file_operations shadow_fop; };
struct sumh_fop_meta { struct sumh_fop_template *template;sumh_fop_iterate_client_fn iterate_client;void *iterate_client_data; };
static struct sumh_fop_meta meta;
static int sumh_ioctl_tgid,sumh_rule_count=1,sumh_enabled=1,sumh_stealth_enabled,sumh_filldir_cache,current,sumh_fop_client_srcu;
static int native_calls,client_calls,cache_calls,have_client,user_count,user_capacity;
static const char *primary_path="/visible";
static struct { int filldir_hidden,filldir_injected,iterate_fop_entries,iterate_fop_wrapped; } sumh_hook_stats;
static struct { char name[NAME_MAX+1];u64 ino;loff_t off; } outputs[64];
static int srcu_read_lock(int *p) { (void)p;srcu_depth++;return 0; }
static void srcu_read_unlock(int *p,int idx) { (void)p;(void)idx;srcu_depth--; }
static int sumh_policy_current_scope(void) { return SUMH_POLICY_SCOPE_VIEW; }
static bool sumh_policy_current_is_hide_target(const struct inode *i) { (void)i;return true; }
static bool sumh_dirhijack_hidden(struct inode *i,const char *name,int len) { (void)i;return len==6&&!memcmp(name,"hidden",6); }
static bool sumh_dirhijack_emits(struct inode *i,const char *name,int len) { (void)i;return have_client&&len==6&&!memcmp(name,"client",6); }
static bool sumh_dirhijack_injected_context(const struct dir_context *ctx) { return (ctx->pos&0xFFFF000000000000ULL)==CLIENT_POS; }
static struct sumh_fop_meta *sumh_fop_lookup_rcu(struct inode *i) { (void)i;return &meta; }
static void *kmem_cache_zalloc(int cache,gfp_t flags) { (void)cache;return kzalloc(sizeof(struct sumh_filldir_wrapper),flags); }
static void kmem_cache_free(int cache,void *p) { (void)cache;kfree(p); }
static char *primary_render(const struct path *p,char *buf,int size) {
    if(p->dentry!=source_file.f_path.dentry||p->mnt!=source_file.f_path.mnt)bad_context++;
    size_t len=strlen(primary_path);if(len>=(size_t)size)return ERR_PTR(-ENAMETOOLONG);char *out=buf+size-len-1;memcpy(out,primary_path,len+1);return out;
}
static char *(*sumh_d_absolute_path)(const struct path *,char *,int)=primary_render;
static struct dentry *cold_lookup(struct dentry *p,const struct qstr *q) { (void)p;(void)q;cache_calls++;return NULL; }
static struct dentry *(*sumh_d_hash_and_lookup)(struct dentry *,const struct qstr *)=cold_lookup;
"""


INTEGRATION_TESTS = r"""
static bool user_actor(struct dir_context *ctx,const char *name,int len,loff_t off,u64 ino,unsigned int type) {
    (void)ctx;(void)type;source_actor_calls++;
    if(user_count==user_capacity)return false;
    /* Model getdents64 storing the next entry's cookie in the previous one. */
    if(user_count)outputs[user_count-1].off=off;
    memcpy(outputs[user_count].name,name,(size_t)len);outputs[user_count].name[len]=0;outputs[user_count++].ino=ino;return true;
}
static int native(struct file *file,struct dir_context *ctx) {
    (void)file;native_calls++;
    while(ctx->pos<objects[0].count){struct record *r=&objects[0].records[ctx->pos];if(!ctx->actor(ctx,r->name,r->len,ctx->pos,r->ino,r->type))return 0;ctx->pos++;}return 0;
}
static int client(struct file *file,struct dir_context *ctx,const struct file_operations *orig,void *data) {
    (void)data;client_calls++;
    if(sumh_dirhijack_injected_context(ctx)) { if(ctx->pos==CLIENT_POS&&ctx->actor(ctx,"client",6,ctx->pos,999,DT_REG))ctx->pos++;return 0; }
    int ret=orig->iterate_shared(file,ctx);if(ret)return ret;ctx->pos=CLIENT_POS;return 0;
}
static const struct file_operations original={native};
static struct sumh_fop_template template={&original,{sumh_shadow_iterate_shared}};
static void integration_setup(void) {
    six();native_calls=client_calls=cache_calls=have_client=user_count=source_actor_calls=0;user_capacity=64;primary_path=alias_path="/visible";
    sumh_enabled=sumh_rule_count=1;objects[0].mapping.flags=2;source_file.f_op=&template.shadow_fop;meta=(struct sumh_fop_meta){&template,NULL,NULL};
}
static int count_output(const char *name) { int n=0;for(int i=0;i<user_count;i++)if(!strcmp(outputs[i].name,name))n++;return n; }
static int read_outputs(struct dir_context *ctx,int capacity) {
    user_count=0;user_capacity=capacity;int result=sumh_shadow_iterate_shared(&source_file,ctx);
    if(user_count)outputs[user_count-1].off=ctx->pos;return result;
}
static int integrated(void) {
    struct dir_context ctx={user_actor,0};integration_setup();record(0,"late",900,DT_REG,false);record(0,"ordinary",901,DT_REG,false);
    CHECK(!sumh_shadow_iterate_shared(&source_file,&ctx));CHECK(count_output("late")==1&&count_output("ordinary")==1&&user_count==8&&!cache_calls);
    const char *wanted[]={"tAG","late","tAF","tAE","tAD","tAC","tAB","ordinary"};
    for(int i=0;i<8;i++)CHECK(!strcmp(outputs[i].name,wanted[i]));
    CHECK(outputs[0].ino==sumh_vnode_source_ino(objects[6].sb.s_dev,16));CHECK(clean()&&!srcu_depth);
    int previous=resolve_calls;user_count=0;CHECK(!sumh_shadow_iterate_shared(&source_file,&ctx));CHECK(!previous&&!resolve_calls&&!user_count&&clean());
    /* A rejected first record retains the private boundary; every saved
     * getdents-style cookie must reproduce the complete expected suffix. */
    integration_setup();ctx.pos=0;record(0,"late",900,DT_REG,false);record(0,"ordinary",901,DT_REG,false);
    CHECK(!read_outputs(&ctx,0)&&!user_count&&ctx.pos==(loff_t)SUMH_MAGIC_POS&&!native_calls&&clean());
    CHECK(!read_outputs(&ctx,64)&&user_count==8&&clean());loff_t saved[8];
    for(int i=0;i<8;i++){CHECK(!strcmp(outputs[i].name,wanted[i]));saved[i]=outputs[i].off;}
    for(int i=0;i<8;i++){ctx.pos=saved[i];CHECK(!read_outputs(&ctx,64)&&user_count==7-i&&clean());
        for(int j=0;j<user_count;j++)CHECK(!strcmp(outputs[j].name,wanted[i+j+1]));}
    integration_setup();ctx.pos=0;record(0,"late",900,DT_REG,false);record(0,"ordinary",901,DT_REG,false);user_capacity=1;
    for(int i=0;i<8;i++){user_count=0;CHECK(!sumh_shadow_iterate_shared(&source_file,&ctx)&&user_count==1);CHECK(!strcmp(outputs[0].name,wanted[i])&&clean());}
    user_count=0;CHECK(!sumh_shadow_iterate_shared(&source_file,&ctx)&&!user_count&&clean());
    integration_setup();ctx.pos=0;exact_rule("/visible/client",998,DT_REG);record(5,"client",57,DT_REG,false);have_client=1;meta.iterate_client=client;meta.iterate_client_data=&meta;
    CHECK(!sumh_shadow_iterate_shared(&source_file,&ctx));CHECK(!count_output("client"));user_count=0;previous=resolve_calls;
    CHECK(!sumh_shadow_iterate_shared(&source_file,&ctx));CHECK(user_count==1&&outputs[0].ino==999&&resolve_calls==previous&&clean());
    /* Full production preparation failures never enter the source iterator,
     * including after target5 succeeded and the retired-client rewind. */
    const loff_t positions[]={0,3,SUMH_MAGIC_POS+2,CLIENT_POS+2};
    for(unsigned int i=0;i<ARRAY_SIZE(positions);i++){integration_setup();ctx.pos=positions[i];objects[6].iterate_error=EIO;objects[6].error_after=1;
        CHECK(sumh_shadow_iterate_shared(&source_file,&ctx)==-EIO);CHECK(ctx.pos==positions[i]&&!user_count&&!native_calls&&!client_calls&&!source_actor_calls&&clean()&&!srcu_depth);}
    integration_setup();ctx.pos=0;CHECK(!sumh_shadow_iterate_shared(&source_file,&ctx));int allocations=allocation_attempts;
    for(int failure=1;failure<=allocations;failure++){integration_setup();ctx.pos=SUMH_MAGIC_POS+2;fail_at=failure;
        CHECK(sumh_shadow_iterate_shared(&source_file,&ctx)==-ENOMEM);CHECK(ctx.pos==(loff_t)SUMH_MAGIC_POS+2&&!user_count&&!native_calls&&!client_calls&&!source_actor_calls&&clean()&&!srcu_depth);}
    integration_setup();ctx.pos=CLIENT_POS+2;objects[6].empty_progress=1;
    CHECK(sumh_shadow_iterate_shared(&source_file,&ctx)==-E2BIG&&ctx.pos==CLIENT_POS+2);CHECK(!user_count&&!source_actor_calls&&!native_calls&&!client_calls&&clean()&&!srcu_depth);
    return 0;
}
int main(void) { return integrated(); }
"""


def integration_source():
    wrapper = function("kernel/sumh/internal/sumh_types.h", "struct sumh_filldir_wrapper {") + ";\n"
    hooks = "kernel/sumh/hooks/sumh_vfs_hooks.c"
    bodies = "\n".join(function(hooks, marker) for marker in (
        "static bool sumh_iterate_injected_pos(", "void sumh_iterate_rewind_injected(",
        "void sumh_iterate_emit_injected(", "SUMH_NOCFI bool sumh_filldir_filter(",
        "static void sumh_iterate_free_wrapper(", "SUMH_NOCFI struct sumh_filldir_wrapper *",
        "void sumh_iterate_finish_wrapper("))
    fop = "kernel/sumh/features/sumh_fop_override.c"
    marker = re.search(r"SUMH_NOCFI static int sumh_shadow_iterate_shared\([^;{}]*\)\s*\{", (ROOT / fop).read_text()).group(0)
    return (fixture() + wrapper + INTEGRATION + bodies + function(fop, marker) +
            TESTS[:TESTS.index("int main(void)")] + INTEGRATION_TESTS)


TESTS = r"""
static int names_count(struct sumh_merge_snapshot *s) { int count=0;struct sumh_name_list *n;list_for_each_entry(n,sumh_merge_snapshot_names(s),list)count++;return count; }
static struct sumh_name_list *find_name(struct sumh_merge_snapshot *s,const char *name) {
    struct sumh_name_list *n;list_for_each_entry(n,sumh_merge_snapshot_names(s),list)if(!strcmp(n->name,name))return n;return NULL;
}
static void six(void) { setup(6);for(int i=1;i<=6;i++){char name[8];label(name,i);record(i,name+1,(u64)(10+i),DT_REG,false);}record(5,"late",51,DT_REG,false);record(6,"late",61,DT_DIR,false); }
static int basic(void) {
    struct sumh_merge_snapshot *s=NULL;six();exact_rule("/visible/exact",777,DT_REG);record(1,"exact",11,DT_DIR,false);
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s)&&s);
    CHECK(names_count(s)==8&&!resolve_calls&&open_calls==6);
    for(int i=1;i<=6;i++)CHECK(objects[i].dentry.refs==4&&objects[i].mount.refs==4);
    CHECK(objects[0].dentry.refs==1&&objects[0].mount.refs==1);
    CHECK(find_name(s,"late")->ino==sumh_vnode_source_ino(objects[5].sb.s_dev,51));
    CHECK(find_name(s,"exact")->ino==777);
    CHECK(sumh_merge_snapshot_covers(s,"lateX",4)&&!sumh_merge_snapshot_covers(s,"lateX",5));
    CHECK(!sumh_merge_snapshot_covers(s,"absent",6));
    sumh_merge_snapshot_destroy(s);CHECK(clean());
    setup(8);for(int i=1;i<=8;i++){char name[8];label(name,i);record(i,name+1,(u64)i,DT_REG,false);}
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s)&&names_count(s)==8&&open_calls==8);sumh_merge_snapshot_destroy(s);CHECK(clean());
    setup(1);record(1,"prefix",11,DT_REG,false);record(1,"prefix-long",12,DT_REG,false);record(1,"\xe7\x95\x8c",13,DT_REG,false);record(1,"\x80\xff",14,DT_REG,false);
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s)&&names_count(s)==4);CHECK(find_name(s,"\x80\xff")&&find_name(s,"\xe7\x95\x8c"));
    CHECK(sumh_merge_snapshot_covers(s,"prefixX",6)&&!sumh_merge_snapshot_covers(s,"prefixX",7));sumh_merge_snapshot_destroy(s);CHECK(clean());
    six();record(6,"new-late-child",99,DT_REG,false);
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s));CHECK(find_name(s,"new-late-child"));sumh_merge_snapshot_destroy(s);CHECK(clean());
    six();rules[0].src="/alias";alias_path="/alias";
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s));CHECK(find_name(s,"tAB")&&render_calls==1);sumh_merge_snapshot_destroy(s);CHECK(clean());
    six();rules[0].src="/elsewhere";rules[0].resolved_src="/visible";
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s));CHECK(find_name(s,"tAB"));sumh_merge_snapshot_destroy(s);CHECK(clean());
    six();exact_rule("/visible/copied",777,DT_REG);clear_at_capture=1;CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s)&&cleared);CHECK(names_count(s)==8&&find_name(s,"copied")->ino==777);sumh_merge_snapshot_destroy(s);CHECK(clean());
    setup(1);record(1,"old",1,DT_REG,false);record(2,"new",2,DT_REG,false);replace_before=1;
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s));CHECK(find_name(s,"old")&&!find_name(s,"new")&&!resolve_calls);sumh_merge_snapshot_destroy(s);CHECK(clean());
    setup(1);record(1,"old",1,DT_REG,false);record(2,"new",2,DT_REG,false);replace_after=1;
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s));CHECK(find_name(s,"old")&&!find_name(s,"new")&&!resolve_calls);sumh_merge_snapshot_destroy(s);CHECK(clean());
    /* Replacing the rule explicitly chooses the new object; replacing only
     * its pathname above does not. */
    setup(1);record(1,"old",1,DT_REG,false);record(2,"new",2,DT_REG,false);
    bind_rule_path(0,&objects[2].dentry,&objects[2].mount);
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s));CHECK(find_name(s,"new")&&!find_name(s,"old")&&!resolve_calls);sumh_merge_snapshot_destroy(s);CHECK(clean());
    /* A held dentry with another mount is a distinct path. Capture must
     * preserve both, including when two records share the same dentry. */
    setup(2);record(1,"same-child",7,DT_REG,false);bind_rule_path(1,&objects[1].dentry,&objects[2].mount);
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s)&&open_calls==2&&names_count(s)==1);
    CHECK(opened[0].f_path.dentry==&objects[1].dentry&&opened[0].f_path.mnt==&objects[1].mount);
    CHECK(opened[1].f_path.dentry==&objects[1].dentry&&opened[1].f_path.mnt==&objects[2].mount);
    CHECK(objects[1].dentry.refs==7&&objects[1].mount.refs==4&&objects[2].mount.refs==4);
    sumh_merge_snapshot_destroy(s);CHECK(clean());
    setup(0);CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s)&&s&&!open_calls&&!names_count(s));sumh_merge_snapshot_destroy(s);CHECK(clean());
    setup(1);objects[1].dentry.inode=source_file.f_path.dentry->inode;
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s));CHECK(!open_calls&&!iterate_calls&&!names_count(s));sumh_merge_snapshot_destroy(s);CHECK(clean());
    setup(1);objects[1].inode.ours=1;CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-EOPNOTSUPP&&!s);CHECK(!iterate_calls&&clean());
    return 0;
}
static int errors(void) {
    struct sumh_merge_snapshot *s=NULL;six();record(2,"char",20,DT_CHR,false);exact_rule("/visible/copied",778,DT_REG);objects[3].specialized=1;
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s));int attempts=allocation_attempts;sumh_merge_snapshot_destroy(s);CHECK(clean());
    for(int failure=1;failure<=attempts;failure++){six();record(2,"char",20,DT_CHR,false);exact_rule("/visible/copied",778,DT_REG);objects[3].specialized=1;fail_at=failure;
        CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-ENOMEM&&!s);CHECK(clean()&&!source_actor_calls);}
    for(int failure=1;failure<=attempts;failure++){six();record(2,"char",20,DT_CHR,false);exact_rule("/visible/copied",778,DT_REG);objects[3].specialized=1;fail_at=failure;clear_at_capture=1;
        CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-ENOMEM&&!s);CHECK(clean()&&!source_actor_calls&&!resolve_calls);}
    /* Incomplete stored paths are a corrupt/unavailable definition, never a
     * reason to fall back to the descriptive pathname. Fail after five valid
     * acquisitions to check cleanup of the already captured references. */
    for(int missing=0;missing<3;missing++){six();drop_rule_path(5);
        if(missing==0)rules[5].target_path.dentry=&objects[6].dentry;
        if(missing==1)rules[5].target_path.mnt=&objects[6].mount;
        CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-ESTALE&&!s);
        CHECK(!open_calls&&!iterate_calls&&!resolve_calls&&clean());}
    six();objects[6].dentry.inode=NULL;
    CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-ENOTDIR&&!s);CHECK(clean()&&!resolve_calls);
    for(int phase=0;phase<6;phase++){six();
        if(phase==0){open_error=EACCES;open_error_at=6;}
        if(phase==1){open_error=ENOMEM;open_error_at=6;}
        if(phase==2){objects[6].iterate_error=EIO;objects[6].error_after=1;}
        if(phase==3){objects[6].no_progress=1;}
        if(phase==4){objects[6].inode.i_mode=0100000;}
        if(phase==5){alias_error=ENOENT;}
        int expected=phase==0?EACCES:phase==1?ENOMEM:phase==4?ENOTDIR:phase==5?ENOENT:EIO;
        CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-expected&&!s);CHECK(clean());}
    /* Retiring all definitions after capture must not revoke the snapshot's
     * paths; an eventual open failure still releases every captured ref. */
    six();clear_at_capture=1;open_error=EACCES;open_error_at=6;
    CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-EACCES&&!s&&cleared);CHECK(clean()&&!resolve_calls);
    for(int kind=0;kind<5;kind++){setup(1);record(1,"bad",7,DT_REG,false);struct record *r=&objects[1].records[0];
        if(kind==0)r->len=0;if(kind==1)r->len=-1;if(kind==2)r->len=256;if(kind==3)r->name[1]=0;if(kind==4)r->name[1]='/';
        CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-EIO&&!s);CHECK(clean());}
    for(int bad=1;bad<=6;bad++){setup(1);alias_bad=bad;CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==(bad>=5?-ENAMETOOLONG:-EIO)&&!s);CHECK(clean());}
    setup(0);static char bad_exact[300];memcpy(bad_exact,"/visible/",9);memset(bad_exact+9,'x',256);bad_exact[265]=0;exact_rule(bad_exact,5,DT_REG);
    CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-EIO&&!s);CHECK(clean());
    return 0;
}
static int whiteouts(void) {
    struct sumh_merge_snapshot *s=NULL;setup(2);record(1,"char",20,DT_CHR,false);record(2,"char",21,DT_REG,false);record(1,".replace",30,DT_REG,false);
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s));CHECK(find_name(s,"char")->ino==sumh_vnode_source_ino(objects[2].sb.s_dev,21));
    CHECK(!find_name(s,".replace")&&sumh_merge_snapshot_covers(s,".replace",8));sumh_merge_snapshot_destroy(s);CHECK(clean());
    for(int mode=0;mode<7;mode++){setup(1);record(1,"char",20,DT_CHR,false);
        if(mode==0)sumh_lookup_one_len=NULL;if(mode==1)lock_busy=1;if(mode==2)lookup_error=EACCES;if(mode==3)lookup_negative=1;if(mode==4)child_change=1;if(mode==5)child_change=2;if(mode==6)child_change=3;
        int expected=mode==0?EOPNOTSUPP:mode==1?EAGAIN:mode==2?EACCES:ESTALE;
        if(mode==6){CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s));CHECK(find_name(s,"char"));sumh_merge_snapshot_destroy(s);}
        else CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-expected&&!s);
        CHECK(clean()&&!child_dentry.refs);}
    setup(1);record(1,"visible",77,DT_REG,true);record(1,"raw",78,DT_REG,false);
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s));CHECK(find_name(s,"visible")->ino==77&&find_name(s,"raw")->ino==sumh_vnode_source_ino(objects[1].sb.s_dev,78));sumh_merge_snapshot_destroy(s);CHECK(clean());
    setup(1);record(1,"projected-char",20,DT_CHR,true);
    CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-EOPNOTSUPP&&!s&&!lookup_calls);CHECK(clean());
    for(int projected=0;projected<2;projected++)for(int missing=0;missing<2;missing++){setup(1);record(1,".replace",20,DT_CHR,projected);if(missing)sumh_lookup_one_len=NULL;lock_busy=1;
        CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s));CHECK(!find_name(s,".replace")&&sumh_merge_snapshot_covers(s,".replace",8)&&!lookup_calls&&!trylock_calls);sumh_merge_snapshot_destroy(s);CHECK(clean());}
    setup(1);record(1,"native",31,DT_REG,false);objects[1].specialized=1;
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s));CHECK(find_name(s,"native")->ino==sumh_vnode_source_ino(objects[1].sb.s_dev,31));
    CHECK(find_name(s,"projected")->ino==777&&names_count(s)==2);sumh_merge_snapshot_destroy(s);CHECK(clean());
    return 0;
}
static int capacity(void) {
    struct sumh_merge_snapshot *s=NULL;
    setup(SUMH_MERGE_MAX_TARGETS);CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s));CHECK(open_calls==SUMH_MERGE_MAX_TARGETS);sumh_merge_snapshot_destroy(s);CHECK(clean());
    setup(SUMH_MERGE_MAX_TARGETS+1);CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-E2BIG&&!s);CHECK(clean());
    for(int extra=0;extra<2;extra++){setup(1);objects[1].generated=SUMH_MERGE_MAX_RECORDS+extra;objects[1].generated_kind=0;objects[1].batch=SUMH_MERGE_MAX_RECORDS;
        int result=sumh_merge_snapshot_create(&source_file,"/visible",&s);if(extra)CHECK(result==-E2BIG&&!s);else{CHECK(!result&&s);sumh_merge_snapshot_destroy(s);}CHECK(clean());}
    setup(1);objects[1].empty_progress=1;CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-E2BIG&&!s);CHECK(iterate_calls==SUMH_MERGE_MAX_PASSES&&clean());
    for(int extra=0;extra<2;extra++){setup(1);objects[1].generated=SUMH_MERGE_MAX_NAMES/2+extra;objects[1].generated_kind=1;objects[1].batch=SUMH_MERGE_MAX_NAMES;
        int result=sumh_merge_snapshot_create(&source_file,"/visible",&s);if(extra)CHECK(result==-E2BIG&&!s);else{CHECK(!result&&names_count(s)==SUMH_MERGE_MAX_NAMES/2);sumh_merge_snapshot_destroy(s);}CHECK(clean());}
    setup(SUMH_MERGE_MAX_TARGETS);for(int i=1;i<=rule_count;i++){memset(objects[i].path+4,'a',PATH_MAX-5);objects[i].path[PATH_MAX-1]=0;memcpy(rule_text[i-1],objects[i].path,PATH_MAX);}
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s)&&s&&open_calls==SUMH_MERGE_MAX_TARGETS);
    CHECK(!resolve_calls&&peak_bytes<=SUMH_MERGE_MEMORY_BUDGET);sumh_merge_snapshot_destroy(s);CHECK(clean());
    /* Long emitted names exhaust the real workspace even though descriptive
     * target pathname strings no longer occupy snapshot allocations. */
    setup(1);objects[1].generated=SUMH_MERGE_MAX_NAMES/2;objects[1].generated_kind=2;objects[1].batch=SUMH_MERGE_MAX_NAMES;
    CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-E2BIG&&!s);CHECK(peak_bytes<=SUMH_MERGE_MEMORY_BUDGET&&clean());
    /* Exact boundary/overflow arithmetic in the actual budget helper. */
    setup(0);struct sumh_merge_snapshot budget={.requested_bytes=SUMH_MERGE_MEMORY_BUDGET-1};
    void *last=sumh_merge_alloc(&budget,1,GFP_KERNEL);CHECK(!IS_ERR(last)&&budget.requested_bytes==SUMH_MERGE_MEMORY_BUDGET);
    CHECK(sumh_merge_alloc(&budget,1,GFP_KERNEL)==ERR_PTR(-E2BIG));CHECK(sumh_merge_alloc(&budget,SIZE_MAX,GFP_KERNEL)==ERR_PTR(-E2BIG));
    sumh_merge_free(&budget,last,1);CHECK(clean());
    return 0;
}
int main(void) { int ret=basic();if(ret)return ret;ret=errors();if(ret)return ret;ret=whiteouts();if(ret)return ret;return capacity(); }
"""


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler, "merge-snapshot", source().replace("unsigned long)", "__UINTPTR_TYPE__)"))
    run_test(compiler, "merge-snapshot-active-caller", integration_source().replace("unsigned long)", "__UINTPTR_TYPE__)"))
