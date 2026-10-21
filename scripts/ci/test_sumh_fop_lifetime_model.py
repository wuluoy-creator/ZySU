"""Instrumented kernel-service doubles for the actual FOP translation unit.

These schedules check source-level ownership/order, not Linux RCU, memory
barriers, real unmount, CFI or a kernel build. No production lifetime algorithm
is copied into this model.
"""

MODEL = r"""
/* Windows is LLP64 whereas the production Linux target is LP64. The actual
   pointer-to-unsigned-long hash-key casts truncate only in this host model;
   collisions are intentional and full pointer equality still selects entries.
   Do not change production casts/types or disable this warning for Linux. */
#if defined(_WIN64) && defined(__clang__)
#pragma clang diagnostic ignored "-Wpointer-to-int-cast"
#endif
#define EINVAL 22
#define ENOTDIR 20
#define ENOMEM 12
#define ENODEV 19
#define EOPNOTSUPP 95
#define EAGAIN 11
#define EBUSY 16
#define ENOENT 2
#define ESHUTDOWN 108
#define GFP_KERNEL 0
#define S_IFDIR 0040000
#define S_ISDIR(m) (((m)&0170000)==S_IFDIR)
#define SB_BORN 1
#define AS_FLAGS_SUMH_FOP_INSTALLED 2
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define smp_load_acquire(p) (*(p))
#define smp_store_release(p,v) (*(p)=(v))
#define smp_wmb() ((void)0)
#define pr_info(...) ((void)0)
#define sumh_log(...) ((void)0)
#define ERR_PTR(v) ((void *)(ssize_t)(v))
#define PTR_ERR(v) ((ssize_t)(v))
#define IS_ERR(p) ((size_t)(p)>=(size_t)-4095)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define container_of(p,t,m) ((t *)((char *)(p)-__builtin_offsetof(t,m)))
#define WARN_ON_ONCE(x) ((x)?(bad++,1):0)
typedef long long loff_t;
typedef int atomic_t;
struct module {int refs,available;};
static struct module self_module,foreign_module,second_module;
#define THIS_MODULE (&self_module)
struct rw_semaphore {int readers;};
struct super_block {unsigned long s_flags;atomic_t s_active;struct rw_semaphore s_umount;int id,deactivated,shutdowns,unpaired,releasable;};
struct address_space {unsigned long flags;};
struct inode;
struct file;
struct dir_context {loff_t pos;};
struct file_operations {struct module *owner;int (*iterate_shared)(struct file *,struct dir_context *);};
struct inode {struct address_space *i_mapping;unsigned i_mode;struct super_block *i_sb;const struct file_operations *i_fop;int refs,held,puts,id;int pending_retire,detach_seq;const struct file_operations *expected_after_detach;};
struct file {const struct file_operations *f_op;struct inode *inode;};
static struct inode *file_inode(struct file *f){return f->inode;}
struct hlist_node {struct hlist_node *next,**pprev;};
struct hlist_head {struct hlist_node *first;};
#define MODEL_BUCKETS 8
#define DEFINE_HASHTABLE(name,bits) struct hlist_head name[MODEL_BUCKETS]
#define DEFINE_SPINLOCK(name) int name
#define DEFINE_STATIC_SRCU(name) int name
static unsigned model_hash(unsigned long value){return (unsigned)((value>>3)^(value>>7))&(MODEL_BUCKETS-1);}
#define hash_init(t) memset(t,0,sizeof(t))
#define model_entry(p,t,m) ((p)?container_of(p,t,m):NULL)
#define hash_for_each_possible(t,p,m,k) for((p)=model_entry((t)[model_hash(k)].first,__typeof__(*(p)),m);p;(p)=model_entry((p)->m.next,__typeof__(*(p)),m))
#define hash_for_each_possible_rcu(t,p,m,k) hash_for_each_possible(t,p,m,k)
#define hash_for_each_safe(t,b,n,p,m) for((b)=0;(b)<(int)ARRAY_SIZE(t);(b)++) for((p)=model_entry((t)[b].first,__typeof__(*(p)),m);(p)&&((n)=(p)->m.next,1);(p)=model_entry(n,__typeof__(*(p)),m))
static bool model_hash_empty(const struct hlist_head *h,size_t n){for(size_t i=0;i<n;i++)if(h[i].first)return false;return true;}
#define hash_empty(t) model_hash_empty(t,ARRAY_SIZE(t))
static void model_hash_add(struct hlist_head *t,struct hlist_node *n,unsigned long key);
static void model_hash_del(struct hlist_node *n);
#define hash_add_rcu(t,n,k) model_hash_add(t,n,k)
#define hash_del_rcu(n) model_hash_del(n)
struct list_head {struct list_head *next,*prev;};
#define LIST_HEAD(name) struct list_head name={&name,&name}
static void INIT_LIST_HEAD(struct list_head *h){h->next=h->prev=h;}
static bool list_empty(const struct list_head *h){return h->next==h;}
static void list_add_tail(struct list_head *n,struct list_head *h){n->next=h;n->prev=h->prev;h->prev->next=n;h->prev=n;}
static void list_del_init(struct list_head *n){n->next->prev=n->prev;n->prev->next=n->next;INIT_LIST_HEAD(n);}
#define list_for_each_entry_safe(p,n,h,m) for((p)=container_of((h)->next,__typeof__(*(p)),m),(n)=container_of((p)->m.next,__typeof__(*(p)),m);&(p)->m!=(h);(p)=(n),(n)=container_of((n)->m.next,__typeof__(*(n)),m))
struct sumh_fop_bridge_entry {bool registered;};
static int bad,spin_depth,read_depth,srcu_depth,rcu_depth,allocations,fail_at,live_allocations,peak_allocations;
static int sequence,tasks_seq,rcu_seq,srcu_seq,task_drains,rcu_drains,srcu_drains;
static int sb_gets,sb_puts,inode_gets,inode_puts,bridge_registers,bridge_unregisters,module_gets,module_puts;
static int old_iterates,client_iterates,wrapper_prepares,ready_hook_calls;
static bool use_bridge;
static void (*before_spin)(void),(*during_tasks)(void),(*during_srcu)(void);
static struct inode inodes[4];
static struct address_space mappings[4];
static struct super_block supers[3];
static union {unsigned long long align;unsigned char bytes[512];} heap[64];
static bool allocated[64];
static int alloc_kind[64];
static int slot_for(const void *p){for(int i=0;i<64;i++)if(p==heap[i].bytes)return i;return -1;}
static void *kzalloc(size_t n,int flags){
    if(spin_depth||read_depth||rcu_depth||flags!=GFP_KERNEL)bad++;
    allocations++;if(allocations==fail_at)return NULL;
    if(n>sizeof(heap[0].bytes)){bad++;return NULL;}
    for(int i=0;i<64;i++)if(!allocated[i]){allocated[i]=true;alloc_kind[i]=0;live_allocations++;if(live_allocations>peak_allocations)peak_allocations=live_allocations;memset(heap[i].bytes,0,n);return heap[i].bytes;}
    bad++;return NULL;
}
static void kfree(void *p){
    if(!p)return;if(spin_depth||read_depth||rcu_depth)bad++;
    int slot=slot_for(p);if(slot<0||!allocated[slot]){bad++;return;}
    allocated[slot]=false;live_allocations--;memset(heap[slot].bytes,0xdd,sizeof(heap[slot].bytes));
}
static bool try_module_get(struct module *m){
    if(spin_depth||read_depth)bad++;if(!m->available)return false;
    m->refs++;module_gets++;return true;
}
static void module_put(struct module *m){if(spin_depth||read_depth||m->refs<=0)bad++;else m->refs--;module_puts++;}
static void spin_lock(int *m){
    (void)m;if(before_spin){void (*hook)(void)=before_spin;before_spin=NULL;hook();}
    if(spin_depth||read_depth)bad++;spin_depth++;
}
static void spin_unlock(int *m){(void)m;if(spin_depth!=1)bad++;spin_depth--;}
static void down_read(struct rw_semaphore *r){if(spin_depth||read_depth||r->readers)bad++;r->readers++;read_depth++;}
static void up_read(struct rw_semaphore *r){if(spin_depth||read_depth!=1||r->readers!=1)bad++;r->readers--;read_depth--;}
static bool atomic_inc_not_zero(atomic_t *v){if(read_depth!=1||spin_depth)bad++;if(!*v)return false;(*v)++;sb_gets++;container_of(v,struct super_block,s_active)->unpaired++;return true;}
static void atomic_inc(atomic_t *v){if(read_depth!=1||spin_depth)bad++;(*v)++;sb_gets++;container_of(v,struct super_block,s_active)->unpaired++;}
static int atomic_read(const atomic_t *v){return *v;}
static void ihold(struct inode *i){if(spin_depth||read_depth||i->refs<=0||i->i_sb->s_active<=0||i->i_sb->unpaired<=0)bad++;else i->i_sb->unpaired--;i->refs++;i->held++;inode_gets++;}
static void iput(struct inode *i){
    if(spin_depth||read_depth||rcu_depth||i->refs<=0||i->held<=0||i->i_sb->s_active<=0)bad++;
    if(i->pending_retire){
        if(!(i->detach_seq<tasks_seq&&tasks_seq<rcu_seq&&rcu_seq<srcu_seq))bad++;
        if(i->i_fop!=i->expected_after_detach)bad++;
        i->pending_retire=0;
    }
    i->refs--;i->held--;i->puts++;inode_puts++;i->i_sb->releasable++;
}
static void deactivate_super(struct super_block *sb){
    if(spin_depth||read_depth||rcu_depth||sb->s_umount.readers||sb->s_active<=0)bad++;
    /* A meta release must have dropped the matching inode first. The tests
       also use multiple metas sharing this sb, so compare aggregate holds. */
    if(sb->releasable>0)sb->releasable--;else if(sb->unpaired>0)sb->unpaired--;else bad++;
    int holds=0;for(int i=0;i<4;i++)if(inodes[i].i_sb==sb)holds+=inodes[i].held;
    if(sb->s_active-1<holds)bad++;
    sb->s_active--;sb->deactivated++;sb_puts++;if(!sb->s_active)sb->shutdowns++;
}
static bool test_bit(unsigned bit,const unsigned long *flags){return (*flags&(1UL<<bit))!=0;}
static void set_bit(unsigned bit,unsigned long *flags){*flags|=1UL<<bit;}
static void clear_bit(unsigned bit,unsigned long *flags){*flags&=~(1UL<<bit);}
static void synchronize_rcu_tasks(void){
    if(spin_depth||read_depth||rcu_depth||srcu_depth)bad++;tasks_seq=++sequence;task_drains++;
    if(during_tasks){void (*hook)(void)=during_tasks;during_tasks=NULL;hook();}
}
static void synchronize_rcu(void){if(spin_depth||read_depth||rcu_depth)bad++;rcu_seq=++sequence;rcu_drains++;}
static void synchronize_srcu(int *s){
    (void)s;if(spin_depth||read_depth||srcu_depth)bad++;srcu_seq=++sequence;srcu_drains++;
    if(during_srcu){void (*hook)(void)=during_srcu;during_srcu=NULL;hook();}
}
static void rcu_read_lock(void){rcu_depth++;}
static void rcu_read_unlock(void){if(rcu_depth<=0)bad++;rcu_depth--;}
static int srcu_read_lock(int *s){(void)s;srcu_depth++;return 5;}
static void srcu_read_unlock(int *s,int token){(void)s;if(token!=5||srcu_depth<=0)bad++;srcu_depth--;}
static void cond_resched(void){if(spin_depth||read_depth||rcu_depth)bad++;}
static void atomic64_inc(long long *v){(*v)++;}
static struct {long long iterate_fop_entries,iterate_fop_wrapped;} sumh_hook_stats;
struct sumh_filldir_wrapper {struct dir_context wrap_ctx;bool has_iterate_client,stopped;unsigned emitted;};
static bool sumh_dirhijack_injected_context(struct dir_context *ctx){return ctx&&ctx->pos>=1000;}
static struct sumh_filldir_wrapper *sumh_iterate_prepare_wrapper(struct file *f,struct dir_context *ctx){(void)f;(void)ctx;wrapper_prepares++;return NULL;}
static void sumh_iterate_rewind_injected(struct dir_context *ctx){if(ctx->pos>=1000)ctx->pos=0;}
static void sumh_iterate_emit_injected(struct sumh_filldir_wrapper *w){w->stopped=true;}
static void sumh_iterate_finish_wrapper(struct sumh_filldir_wrapper *w){(void)w;}
static int sumh_fop_bridge_register(struct sumh_fop_bridge_entry *b,struct file_operations *ingress,struct file_operations *shadow,const struct file_operations *orig){
    (void)ingress;(void)shadow;(void)orig;if(spin_depth!=1)bad++;b->registered=use_bridge;bridge_registers++;return 0;
}
static void sumh_fop_bridge_unregister(struct sumh_fop_bridge_entry *b){if(spin_depth!=1)bad++;b->registered=false;bridge_unregisters++;}
"""

POSTLUDE = r"""
static void model_hash_add(struct hlist_head *t,struct hlist_node *n,unsigned long key){
    if(spin_depth!=1||read_depth)bad++;
    struct hlist_head *h=&t[model_hash(key)];n->next=h->first;n->pprev=&h->first;if(n->next)n->next->pprev=&n->next;h->first=n;
    if(t==sumh_fop_table){
        struct sumh_fop_meta *m=container_of(n,struct sumh_fop_meta,node);int slot=slot_for(m);
        if(slot<0||!allocated[slot]||m->inode->held<=0)bad++;else alloc_kind[slot]=1;
    }else if(t==sumh_fop_templates){
        struct sumh_fop_template *p=container_of(n,struct sumh_fop_template,node);int slot=slot_for(p);
        if(slot<0||!allocated[slot])bad++;else alloc_kind[slot]=2;
    }else bad++;
}
static void model_hash_del(struct hlist_node *n){
    if(spin_depth!=1||!n->pprev)bad++;
    for(int b=0;b<MODEL_BUCKETS;b++)for(struct hlist_node *p=sumh_fop_table[b].first;p;p=p->next)if(p==n){
        struct sumh_fop_meta *m=container_of(n,struct sumh_fop_meta,node);
        if(m->iterate_client)bad++;
        m->inode->pending_retire=1;m->inode->detach_seq=++sequence;
        if(m->inode->i_fop==sumh_fop_installed_table(m))bad++;
        m->inode->expected_after_detach=m->inode->i_fop;
    }
    *n->pprev=n->next;if(n->next)n->next->pprev=n->pprev;n->pprev=NULL;
}
static int native_iterate(struct file *file,struct dir_context *ctx){if(!file||!ctx||srcu_depth!=1)bad++;old_iterates++;ctx->pos++;return 73;}
static int alternate_iterate(struct file *file,struct dir_context *ctx){(void)file;(void)ctx;return 29;}
static const struct file_operations native_ops={&foreign_module,native_iterate};
static const struct file_operations second_ops={&second_module,native_iterate};
static const struct file_operations self_ops={THIS_MODULE,native_iterate};
static const struct file_operations ownerless_ops={NULL,native_iterate};
static const struct file_operations external_ops={NULL,alternate_iterate};
static const struct file_operations no_iterate_ops={&foreign_module,NULL};
static int client_data,other_data;
static int client(struct file *f,struct dir_context *c,const struct file_operations *orig,void *data){
    if(!f||!c||orig!=&native_ops||data!=&client_data||srcu_depth!=1)bad++;
    client_iterates++;c->pos++;return 91;
}
static int table_count(const struct hlist_head *t){int count=0;for(int b=0;b<MODEL_BUCKETS;b++)for(struct hlist_node *n=t[b].first;n;n=n->next)count++;return count;}
static void setup(void){
    bad=spin_depth=read_depth=srcu_depth=rcu_depth=allocations=fail_at=live_allocations=peak_allocations=0;
    sequence=tasks_seq=rcu_seq=srcu_seq=task_drains=rcu_drains=srcu_drains=0;
    sb_gets=sb_puts=inode_gets=inode_puts=bridge_registers=bridge_unregisters=module_gets=module_puts=0;
    old_iterates=client_iterates=wrapper_prepares=ready_hook_calls=0;
    before_spin=during_tasks=during_srcu=NULL;use_bridge=false;
    memset(allocated,0,sizeof(allocated));memset(alloc_kind,0,sizeof(alloc_kind));
    memset(inodes,0,sizeof(inodes));memset(supers,0,sizeof(supers));memset(mappings,0,sizeof(mappings));
    memset(&sumh_hook_stats,0,sizeof(sumh_hook_stats));
    self_module=(struct module){0,1};foreign_module=(struct module){0,1};second_module=(struct module){0,1};
    for(int i=0;i<3;i++){supers[i].s_flags=SB_BORN;supers[i].s_active=1;supers[i].id=i;}
    for(int i=0;i<4;i++){inodes[i].i_mapping=&mappings[i];inodes[i].i_mode=S_IFDIR;inodes[i].i_sb=&supers[i%3];inodes[i].i_fop=&native_ops;inodes[i].refs=1;inodes[i].id=i;}
    sumh_fop_override_init();
}
static bool balanced(void){
    if(bad||live_allocations||spin_depth||read_depth||srcu_depth||rcu_depth||!hash_empty(sumh_fop_table)||!hash_empty(sumh_fop_templates))return false;
    if(foreign_module.refs||second_module.refs||self_module.refs||module_gets!=module_puts||inode_gets!=inode_puts||sb_gets!=sb_puts)return false;
    for(int i=0;i<4;i++)if(inodes[i].held||inodes[i].pending_retire||inodes[i].refs<0)return false;
    for(int i=0;i<3;i++)if(supers[i].s_umount.readers||supers[i].unpaired||supers[i].releasable)return false;
    return true;
}
static void close_ready(void){sumh_fop_override_ready=false;ready_hook_calls++;}
static void change_fop(void){inodes[0].i_fop=&external_ops;}
static int nested_install_result;
static void duplicate_install(void){nested_install_result=sumh_fop_install(&inodes[0]);}
static void stop_during_clear(void){sumh_fop_override_stop_new();}
static void caller_drops_native(struct inode *i){
    if(i->refs<=0||i->i_sb->s_active<=0)bad++;
    i->refs--;i->i_sb->s_active--;
    if(!i->i_sb->s_active){i->i_sb->shutdowns++;if(i->held)bad++;}
}
"""
