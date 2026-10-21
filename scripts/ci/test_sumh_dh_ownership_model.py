"""Deterministic Linux API services for the actual DH/SOP/FOP source bodies.

No receipt, winner, producer, registry or retirement algorithm is modeled here.
Callbacks/work run only at explicit schedule points. This is not Linux scheduler,
RCU, memory-ordering, native filesystem, kernel-build or device validation.
"""

MODEL = r"""
#if defined(_WIN64) && defined(__clang__)
#pragma clang diagnostic ignored "-Wpointer-to-int-cast"
#endif
typedef unsigned char u8;
typedef unsigned short u16,umode_t;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long loff_t;
typedef int atomic_t,spinlock_t;
typedef struct {unsigned int refs;} refcount_t;
#define EINVAL 22
#define EPERM 1
#define EEXIST 17
#define ENOENT 2
#define EIO 5
#define ENOMEM 12
#define EBUSY 16
#define ENODEV 19
#define ENOTDIR 20
#define ENAMETOOLONG 36
#define ENOSPC 28
#define EOVERFLOW 75
#define EOPNOTSUPP 95
#define ESHUTDOWN 108
#define EAGAIN 11
#define EACCES 13
#define ECHILD 10
#define UINT_MAX 4294967295U
#define INT_MAX 2147483647
#define REFCOUNT_SATURATED (1U<<31)
#define NAME_MAX 255
#define PATH_MAX 4096
#define GFP_KERNEL 0
#define GFP_ATOMIC 1
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_IFMT 0170000
#define S_ISDIR(m) (((m)&S_IFMT)==S_IFDIR)
#define S_ISLNK(m) (((m)&S_IFMT)==S_IFLNK)
#define S_ISREG(m) (((m)&S_IFMT)==S_IFREG)
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10
#define LOOKUP_FOLLOW 1
#define LOOKUP_DIRECTORY 2
#define LOOKUP_RCU 4
#define SB_BORN 1
#define FUSE_SUPER_MAGIC 0x65735546
#define AS_FLAGS_SUMH_FOP_INSTALLED 1
#define AS_FLAGS_SUMH_DIR_HAS_INJECT 2
#define AS_FLAGS_SUMH_DIR_HAS_HIDDEN 3
#define AS_FLAGS_SUMH_HIDE 4
#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define LINUX_VERSION_CODE KERNEL_VERSION(6,18,0)
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v,n) ((v)=(n))
#define smp_load_acquire(p) (*(p))
#define smp_store_release(p,v) (*(p)=(v))
#define smp_wmb() ((void)0)
#define likely(v) (v)
#define unlikely(v) (v)
#define __rcu
#define __init
#define __maybe_unused
#define pr_info(...) ((void)0)
#define pr_warn(...) ((void)0)
#define pr_err(...) ((void)0)
#define sumh_log(...) ((void)0)
#define ARRAY_SIZE(v) (sizeof(v)/sizeof((v)[0]))
#define container_of(p,t,m) ((t *)((char *)(p)-__builtin_offsetof(t,m)))
#define ERR_PTR(v) ((void *)(ssize_t)(v))
#define PTR_ERR(p) ((int)(ssize_t)(p))
#define IS_ERR(p) ((size_t)(p)>=(size_t)-4095)
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
#define min(a,b) ((a)<(b)?(a):(b))
#define min_t(t,a,b) ((t)min(a,b))
static int bad,bad_line,spin_depth,rcu_depth,srcu_depth,callback_depth,worker_depth,config_depth;
static int allocations,fail_at,live_allocations,peak_allocations,path_calls,path_gets,path_puts;
static int inode_gets,inode_puts,dentry_gets,dentry_puts,sb_gets,sb_puts,module_gets,module_puts;
static int barrier_count,rcu_syncs,tasks_syncs,srcu_syncs,sequence,last_parent_put,last_deactivate;
#define BAD() do{bad++;bad_line=__LINE__;}while(0)
#define WARN_ON_ONCE(v) ((v)?(bad++,bad_line=__LINE__,1):0)
#define WARN_ON(v) WARN_ON_ONCE(v)
struct inode;struct dentry;
static void (*before_mutex)(void),(*after_path)(void),(*during_tasks)(void),(*during_srcu)(void),(*during_allocation)(void),(*before_iput)(struct inode *),(*before_dput)(struct dentry *);
struct list_head {struct list_head *next,*prev;};
#define LIST_HEAD_INIT(n) {&(n),&(n)}
#define LIST_HEAD(n) struct list_head n=LIST_HEAD_INIT(n)
static void INIT_LIST_HEAD(struct list_head *h){h->next=h->prev=h;}
static bool list_empty(const struct list_head *h){return h->next==h;}
static void list_add_tail(struct list_head *n,struct list_head *h){n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;}
static void list_add(struct list_head *n,struct list_head *h){n->next=h->next;n->prev=h;h->next->prev=n;h->next=n;}
static void list_del(struct list_head *n){n->next->prev=n->prev;n->prev->next=n->next;}
static void list_del_init(struct list_head *n){list_del(n);INIT_LIST_HEAD(n);}
static void list_move(struct list_head *n,struct list_head *h){list_del(n);list_add(n,h);}
static void list_move_tail(struct list_head *n,struct list_head *h){list_del(n);list_add_tail(n,h);}
static void list_splice_tail_init(struct list_head *f,struct list_head *t){while(!list_empty(f)){struct list_head *n=f->next;list_move_tail(n,t);}}
#define list_first_entry(h,t,m) container_of((h)->next,t,m)
#define list_last_entry(h,t,m) container_of((h)->prev,t,m)
#define list_for_each_entry(p,h,m) for((p)=container_of((h)->next,__typeof__(*(p)),m);&(p)->m!=(h);(p)=container_of((p)->m.next,__typeof__(*(p)),m))
#define list_for_each_entry_reverse(p,h,m) for((p)=container_of((h)->prev,__typeof__(*(p)),m);&(p)->m!=(h);(p)=container_of((p)->m.prev,__typeof__(*(p)),m))
#define list_for_each_entry_safe(p,n,h,m) for((p)=container_of((h)->next,__typeof__(*(p)),m),(n)=container_of((p)->m.next,__typeof__(*(p)),m);&(p)->m!=(h);(p)=(n),(n)=container_of((n)->m.next,__typeof__(*(n)),m))
struct hlist_node {struct hlist_node *next,**pprev;};
struct hlist_head {struct hlist_node *first;};
static void INIT_HLIST_HEAD(struct hlist_head *h){h->first=NULL;}
static void INIT_HLIST_NODE(struct hlist_node *n){n->next=NULL;n->pprev=NULL;}
static bool hlist_empty(const struct hlist_head *h){return !h->first;}
static bool hlist_unhashed(const struct hlist_node *n){return !n->pprev;}
static void hlist_add_head(struct hlist_node *n,struct hlist_head *h){n->next=h->first;n->pprev=&h->first;if(n->next)n->next->pprev=&n->next;h->first=n;}
static void hlist_del(struct hlist_node *n){if(!n->pprev){BAD();return;}*n->pprev=n->next;if(n->next)n->next->pprev=n->pprev;n->pprev=NULL;}
static void hlist_del_init(struct hlist_node *n){if(!hlist_unhashed(n))hlist_del(n);INIT_HLIST_NODE(n);}
#define hlist_add_head_rcu hlist_add_head
#define hlist_del_rcu hlist_del
#define model_entry(p,t,m) ((p)?container_of(p,t,m):NULL)
#define hlist_for_each_entry(p,h,m) for((p)=model_entry((h)->first,__typeof__(*(p)),m);p;(p)=model_entry((p)->m.next,__typeof__(*(p)),m))
#define hlist_for_each_entry_rcu hlist_for_each_entry
#define hlist_for_each_entry_safe(p,n,h,m) for((p)=model_entry((h)->first,__typeof__(*(p)),m);(p)&&((n)=(p)->m.next,1);(p)=model_entry(n,__typeof__(*(p)),m))
#define MODEL_BUCKETS 8
#define DEFINE_HASHTABLE(n,b) struct hlist_head n[MODEL_BUCKETS]
#define hash_init(t) memset(t,0,sizeof(t))
#define hash_min(k,b) ((unsigned)(k)&(MODEL_BUCKETS-1))
#define hash_add(t,n,k) hlist_add_head(n,&(t)[hash_min(k,0)])
#define hash_add_rcu hash_add
#define hash_del hlist_del
#define hash_del_rcu hlist_del
#define hash_for_each_possible(t,p,m,k) hlist_for_each_entry(p,&(t)[hash_min(k,0)],m)
#define hash_for_each_possible_rcu hash_for_each_possible
#define hash_for_each(t,b,p,m) for((b)=0;(b)<(int)ARRAY_SIZE(t);(b)++)hlist_for_each_entry(p,&(t)[b],m)
#define hash_for_each_rcu hash_for_each
#define hash_for_each_safe(t,b,n,p,m) for((b)=0;(b)<(int)ARRAY_SIZE(t);(b)++)hlist_for_each_entry_safe(p,n,&(t)[b],m)
static bool model_hash_empty(struct hlist_head *t,size_t n){for(size_t i=0;i<n;i++)if(t[i].first)return false;return true;}
#define hash_empty(t) model_hash_empty(t,ARRAY_SIZE(t))
struct mutex {int held;};
#define DEFINE_MUTEX(n) struct mutex n
#define DEFINE_SPINLOCK(n) spinlock_t n
#define DEFINE_STATIC_SRCU(n) int n
static void mutex_lock(struct mutex *m){if(before_mutex){void(*h)(void)=before_mutex;before_mutex=NULL;h();}if(m->held||spin_depth||callback_depth)BAD();m->held++;}
static void (*after_mutex_unlock)(struct mutex *);
static void mutex_unlock(struct mutex *m){if(m->held!=1)BAD();m->held--;if(after_mutex_unlock)after_mutex_unlock(m);}
#define lockdep_assert_held(m) do{if(!(m)->held)BAD();}while(0)
static void spin_lock_init(spinlock_t *l){*l=0;}
static void spin_lock(spinlock_t *l){if(*l)BAD();(*l)++;spin_depth++;}
static void spin_unlock(spinlock_t *l){if(*l!=1||spin_depth<=0)BAD();(*l)--;spin_depth--;}
#define spin_lock_irqsave(l,f) do{(f)=0;spin_lock(l);}while(0)
#define spin_unlock_irqrestore(l,f) do{(void)(f);spin_unlock(l);}while(0)
static void refcount_set(refcount_t *r,unsigned n){r->refs=n;}
static unsigned refcount_read(const refcount_t *r){return r->refs;}
static void refcount_inc(refcount_t *r){if(!r->refs||r->refs>=REFCOUNT_SATURATED-1)BAD();else r->refs++;}
static bool refcount_inc_not_zero(refcount_t *r){if(!r->refs)return false;refcount_inc(r);return true;}
static bool refcount_dec_and_test(refcount_t *r){if(!r->refs){BAD();return false;}return !--r->refs;}
static void refcount_dec(refcount_t *r){(void)refcount_dec_and_test(r);}
#define ATOMIC_INIT(v) (v)
static int atomic_read(const atomic_t *v){return *v;}
static void atomic_set(atomic_t *v,int n){*v=n;}
static void atomic_inc(atomic_t *v){(*v)++;}
static void atomic_dec(atomic_t *v){if(*v<=0)BAD();(*v)--;}
static bool atomic_dec_and_test(atomic_t *v){atomic_dec(v);return !*v;}
static bool atomic_inc_not_zero(atomic_t *v){if(!*v)return false;(*v)++;return true;}
static void atomic64_inc(long long *v){(*v)++;}
static void rcu_read_lock(void){rcu_depth++;}
static void (*after_rcu_unlock)(void);
static void rcu_read_unlock(void){if(rcu_depth<=0)BAD();rcu_depth--;if(!rcu_depth&&after_rcu_unlock){void(*h)(void)=after_rcu_unlock;after_rcu_unlock=NULL;h();}}
static int srcu_read_lock(int *s){(*s)++;srcu_depth++;return 0;}
static void srcu_read_unlock(int *s,int i){(void)i;if(*s<=0||srcu_depth<=0)BAD();(*s)--;srcu_depth--;}
#define rcu_dereference(p) (p)
#define rcu_dereference_protected(p,c) (p)
#define rcu_assign_pointer(p,v) ((p)=(v))
#define RCU_INIT_POINTER(p,v) ((p)=(v))
static void sleeping(void){if(spin_depth||rcu_depth||callback_depth||config_depth)BAD();}
static void synchronize_rcu(void){sleeping();rcu_syncs++;sequence++;}
static void synchronize_rcu_tasks(void){sleeping();tasks_syncs++;sequence++;if(during_tasks){void(*h)(void)=during_tasks;during_tasks=NULL;h();}}
static void synchronize_srcu(int *s){sleeping();if(*s)BAD();srcu_syncs++;sequence++;if(during_srcu){void(*h)(void)=during_srcu;during_srcu=NULL;h();}}
static void cond_resched(void){sleeping();}
struct rcu_head {void *unused;};
struct work_struct {void(*fn)(struct work_struct *);bool pending;};
#define DECLARE_WORK(n,f) struct work_struct n={f,false}
static struct {struct rcu_head *head;void(*fn)(struct rcu_head *);} callbacks[8192];
static int callback_count,callback_runs,work_runs;
static void call_rcu(struct rcu_head *h,void(*f)(struct rcu_head *)){if(callback_count>=8192){BAD();return;}callbacks[callback_count].head=h;callbacks[callback_count++].fn=f;}
static void rcu_barrier(void){sleeping();barrier_count++;int n=callback_count;while(n--){struct rcu_head *h=callbacks[0].head;void(*f)(struct rcu_head *)=callbacks[0].fn;for(int i=1;i<callback_count;i++)callbacks[i-1]=callbacks[i];callback_count--;callback_depth++;f(h);callback_depth--;callback_runs++;}}
static bool schedule_work(struct work_struct *w){bool old=w->pending;w->pending=true;return !old;}
static void (*before_work_flush)(struct work_struct *);
static void flush_work(struct work_struct *w){sleeping();if(before_work_flush)before_work_flush(w);int n=0;while(w->pending&&n++<8192){w->pending=false;worker_depth++;w->fn(w);worker_depth--;work_runs++;}if(w->pending)BAD();}
static bool cancel_work(struct work_struct *w){bool pending=w->pending;w->pending=false;return pending;}
static bool cancel_work_sync(struct work_struct *w){sleeping();return cancel_work(w);}
struct llist_node {struct llist_node *next;};
struct llist_head {struct llist_node *first;};
#define LLIST_HEAD(n) struct llist_head n
static void llist_add(struct llist_node *n,struct llist_head *h){n->next=h->first;h->first=n;}
static struct llist_node *llist_del_all(struct llist_head *h){struct llist_node *n=h->first;h->first=NULL;return n;}
#define llist_for_each_safe(n,next_,h) for(n=h;(n)&&((next_)=(n)->next,1);n=next_)
#define DECLARE_WAIT_QUEUE_HEAD(n) int n
#define wait_event(q,c) do{(void)(q);sleeping();if(!(c))BAD();}while(0)
#define wake_up_all(q) ((void)(q))
static union {unsigned long long align;unsigned char bytes[8192];} heap[4096];
static bool used[4096];
static int allocation_slot(const void *p){for(int i=0;i<4096;i++)if(p==heap[i].bytes)return i;return -1;}
static void *kmalloc(size_t n,int flags){if((flags!=GFP_ATOMIC&&(spin_depth||rcu_depth))||callback_depth||config_depth)BAD();allocations++;if(allocations==fail_at)return NULL;if(n>sizeof(heap[0].bytes)){BAD();return NULL;}for(int i=0;i<4096;i++)if(!used[i]){used[i]=true;live_allocations++;if(live_allocations>peak_allocations)peak_allocations=live_allocations;if(during_allocation){void(*h)(void)=during_allocation;during_allocation=NULL;h();}return heap[i].bytes;}BAD();return NULL;}
static void *kzalloc(size_t n,int f){void *p=kmalloc(n,f);if(p)memset(p,0,n);return p;}
static void *kmalloc_array(size_t c,size_t n,int f){return kmalloc(c*n,f);}
static void *kcalloc(size_t c,size_t n,int f){return kzalloc(c*n,f);}
static void kfree(void *p){if(!p)return;if(spin_depth||rcu_depth||config_depth)BAD();int i=allocation_slot(p);if(i<0||!used[i]){BAD();return;}used[i]=false;live_allocations--;memset(p,0xdd,sizeof(heap[i].bytes));}
static char *kstrdup(const char *s,int f){size_t n=strlen(s)+1;char *p=kmalloc(n,f);if(p)memcpy(p,s,n);return p;}
static char *kstrndup(const char *s,size_t n,int f){n=strnlen(s,n);char *p=kmalloc(n+1,f);if(p){memcpy(p,s,n);p[n]=0;}return p;}
static void *kmemdup(const void *s,size_t n,int f){void *p=kmalloc(n,f);if(p)memcpy(p,s,n);return p;}
static char *strrchr(const char *s,int c){char *r=NULL;do{if(*s==c)r=(char *)s;}while(*s++);return r;}
static char *strchr(const char *s,int c){do{if(*s==c)return (char *)s;}while(*s++);return NULL;}
static int memcmp(const void *a,const void *b,size_t n){const unsigned char *x=a,*y=b;for(size_t i=0;i<n;i++)if(x[i]!=y[i])return x[i]-y[i];return 0;}
static size_t strscpy(char *d,const char *s,size_t n){size_t l=strlen(s);if(!n)return (size_t)-1;size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;return l<n?l:(size_t)-1;}
struct module {int refs;bool available;};
static struct module self_module={1,true},foreign_module={1,true};
#define THIS_MODULE (&self_module)
static bool try_module_get(struct module *m){if(!m)return true;if(!m->available)return false;m->refs++;module_gets++;return true;}
static void module_put(struct module *m){if(!m)return;if(m->refs<=0)BAD();m->refs--;module_puts++;}
struct inode;struct dentry;struct file;struct dir_context;struct qstr;struct super_block;
struct super_operations {void(*destroy_inode)(struct inode *);void(*free_inode)(struct inode *);void(*evict_inode)(struct inode *);int(*drop_inode)(struct inode *);};
struct rw_semaphore {int readers;};
struct super_block {const struct super_operations *s_op;unsigned long s_flags,s_magic,s_dev;atomic_t s_active;struct rw_semaphore s_umount;int id;};
struct address_space {unsigned long flags;};
struct qstr {const char *name;unsigned len,hash;};
#define QSTR_INIT(n,l) {.name=(n),.len=(l)}
struct mnt_idmap;
struct inode_operations {struct dentry *(*lookup)(struct inode *,struct dentry *,unsigned);int(*atomic_open)(struct inode *,struct dentry *,struct file *,unsigned,umode_t);int(*permission)(struct mnt_idmap *,struct inode *,int);};
struct file_operations {struct module *owner;int(*iterate_shared)(struct file *,struct dir_context *);int(*open)(struct inode *,struct file *);int(*release)(struct inode *,struct file *);};
struct dentry_operations {int(*d_revalidate)(struct inode *,const struct qstr *,struct dentry *,unsigned);int unused;};
struct inode {const struct inode_operations *i_op;const struct file_operations *i_fop;struct super_block *i_sb;struct address_space *i_mapping,i_data;unsigned long i_ino,i_rdev,i_blocks;umode_t i_mode;unsigned i_nlink,i_uid,i_gid,i_blkbits;loff_t i_size;int refs,id;bool synthetic;void *i_private;};
struct dentry {struct inode *d_inode;struct dentry *d_parent;struct super_block *d_sb;const struct dentry_operations *d_op;struct qstr d_name;unsigned d_flags;spinlock_t d_lock;int refs,drops,id;const char *canonical;};
struct vfsmount {int refs;};
struct path {struct vfsmount *mnt;struct dentry *dentry;};
struct file {struct path f_path;const struct file_operations *f_op;loff_t f_pos;};
struct dir_context {bool(*actor)(struct dir_context *,const char *,int,loff_t,u64,unsigned);loff_t pos;};
static struct inode *d_inode(const struct dentry *d){return d?d->d_inode:NULL;}
static struct inode *file_inode(const struct file *f){return d_inode(f->f_path.dentry);}
static struct dentry *dget(struct dentry *d){if(d){if(d->refs<=0)BAD();d->refs++;dentry_gets++;}return d;}
static void dput(struct dentry *d){if(d){sleeping();if(before_dput)before_dput(d);if(d->refs<=0)BAD();d->refs--;dentry_puts++;}}
static void path_get(const struct path *p){dget(p->dentry);if(p->mnt){if(p->mnt->refs<=0)BAD();p->mnt->refs++;}path_gets++;}
static void path_put(const struct path *p){dput(p->dentry);if(p->mnt){if(p->mnt->refs<=0)BAD();p->mnt->refs--;}path_puts++;}
static void ihold(struct inode *i){if(i->refs<=0||i->i_sb->s_active<=0)BAD();i->refs++;inode_gets++;}
static struct inode *igrab(struct inode *i){if(!i||!i->refs)return NULL;ihold(i);return i;}
static void iput(struct inode *i){if(!i)return;sleeping();if(before_iput)before_iput(i);if(i->refs<=0||i->i_sb->s_active<=0)BAD();i->refs--;inode_puts++;last_parent_put=++sequence;}
static void down_read(struct rw_semaphore *r){sleeping();r->readers++;}
static void up_read(struct rw_semaphore *r){if(r->readers<=0)BAD();r->readers--;}
static void deactivate_super(struct super_block *sb){sleeping();if(sb->s_active<=0||sb->s_umount.readers)BAD();sb->s_active--;sb_puts++;last_deactivate=++sequence;}
static bool test_bit(unsigned b,const unsigned long *v){return !!(v[b/(8*sizeof(*v))]&(1UL<<(b%(8*sizeof(*v)))));}
static void set_bit(unsigned b,unsigned long *v){v[b/(8*sizeof(*v))]|=1UL<<(b%(8*sizeof(*v)));}
static void clear_bit(unsigned b,unsigned long *v){v[b/(8*sizeof(*v))]&=~(1UL<<(b%(8*sizeof(*v))));}
static bool test_and_set_bit(unsigned b,unsigned long *v){bool r=test_bit(b,v);set_bit(b,v);return r;}
static unsigned full_name_hash(const void *salt,const char *s,unsigned n){(void)salt;unsigned h=2166136261U;for(unsigned i=0;i<n;i++)h=(h^(unsigned char)s[i])*16777619U;return h;}
#define DCACHE_OP_HASH 1
#define DCACHE_OP_COMPARE 2
#define DCACHE_OP_REVALIDATE 4
#define DCACHE_OP_WEAK_REVALIDATE 8
#define DCACHE_OP_DELETE 16
#define DCACHE_OP_PRUNE 32
#define DCACHE_OP_REAL 64
static void d_drop(struct dentry *d){if(d->d_lock)BAD();d->drops++;}
static void d_add(struct dentry *d,struct inode *i){d->d_inode=i;}
static struct dentry *d_splice_alias(struct inode *i,struct dentry *d){d_add(d,i);return NULL;}
static void shrink_dcache_sb(struct super_block *s){(void)s;sleeping();}
static void truncate_inode_pages_final(struct address_space *a){(void)a;}
static void clear_inode(struct inode *i){(void)i;}
static bool inode_unhashed(struct inode *i){(void)i;return false;}
static bool d_unlinked(const struct dentry *d){return d->drops>0;}
struct kprobe {void *addr;int(*pre_handler)(struct kprobe *,void *);};
struct pt_regs {unsigned long regs[32];};
static void unregister_kprobe(struct kprobe *p){(void)p;}
static unsigned long sumh_lookup_name(const char *n){(void)n;return 0;}
static void native_free_inode(struct inode *i){(void)i;}
static void (*sumh_free_inode_nonrcu_ptr)(struct inode *)=native_free_inode;
struct native_binding {const char *name;struct path path;int error;};
static struct native_binding native_paths[128];static int native_path_count;
static void (*after_named_path)(const char *,unsigned,int,const struct path *);
static void native_path(const char *n,struct path p,int e){for(int i=0;i<native_path_count;i++)if(!strcmp(native_paths[i].name,n)){native_paths[i]=(struct native_binding){n,p,e};return;}native_paths[native_path_count++]=(struct native_binding){n,p,e};}
static int kern_path(const char *n,unsigned flags,struct path *out){sleeping();path_calls++;int ret=-ENOENT;for(int i=0;i<native_path_count;i++)if(!strcmp(native_paths[i].name,n)){ret=native_paths[i].error;if(!ret){*out=native_paths[i].path;if((flags&LOOKUP_DIRECTORY)&&!S_ISDIR(d_inode(out->dentry)->i_mode))ret=-ENOTDIR;else path_get(out);}break;}if(after_named_path)after_named_path(n,flags,ret,ret?NULL:out);if(after_path){void(*h)(void)=after_path;after_path=NULL;h();}return ret;}
struct cache_binding {struct dentry *parent,*child;const char *name;};
static struct cache_binding cache_bindings[64];static int cache_count,cache_lookups;
static struct dentry *native_child_lookup(struct dentry *p,const struct qstr *n){cache_lookups++;for(int i=0;i<cache_count;i++)if(cache_bindings[i].parent==p&&strlen(cache_bindings[i].name)==n->len&&!memcmp(cache_bindings[i].name,n->name,n->len))return dget(cache_bindings[i].child);return NULL;}
static struct dentry *(*sumh_d_hash_and_lookup)(struct dentry *,const struct qstr *)=native_child_lookup;
static bool model_sees=true,model_hides=true,model_su_enabled,model_su_visible;
static unsigned long model_su_ino;
static bool sumh_policy_current_is_view_target(void){return model_sees;}
static bool sumh_policy_current_is_hide_target(const struct inode *i){(void)i;return model_hides;}
static bool ksu_sucompat_vfs_enabled(void){return model_su_enabled;}
static bool ksu_sucompat_vfs_visible(void){return model_su_visible;}
static bool ksu_sucompat_vfs_current_ino(unsigned long ino){return model_su_enabled&&ino==model_su_ino;}
static bool ksu_sucompat_vfs_is_inode(struct inode *i){return i&&i->synthetic&&i->i_ino==model_su_ino;}
static bool ksu_sucompat_vfs_is_path(const struct path *p){return p&&ksu_sucompat_vfs_is_inode(d_inode(p->dentry));}
static bool sumh_vnode_is_ours(struct inode *i){return i&&i->synthetic;}
static void sumh_vnode_free_info(struct inode *i){(void)i;}
static int vnode_calls;static struct path last_vnode_source;
static void (*during_vnode)(const struct path *);
static struct inode *sumh_vnode_new(struct super_block *sb,const struct path *p,unsigned long ino,umode_t mode,u8 flags){(void)sb;(void)ino;(void)mode;(void)flags;vnode_calls++;last_vnode_source=p?*p:(struct path){0};if(during_vnode)during_vnode(p);return NULL;}
static struct inode *sumh_vnode_new_virtual(struct super_block *sb,const char *p,unsigned long ino,struct inode *parent){(void)p;(void)parent;return sumh_vnode_new(sb,NULL,ino,S_IFDIR,0);}
static bool sumh_is_merge_context(struct dir_context *c){(void)c;return false;}
static bool dir_emit(struct dir_context *c,const char *n,int len,u64 ino,unsigned type){return c->actor(c,n,len,c->pos,ino,type);}
static bool sumh_emit_projected_dirent(struct dir_context *c,const char *n,int len,u64 ino,unsigned type){return dir_emit(c,n,len,ino,type);}
struct sumh_fop_bridge_entry {bool registered;};
static bool model_use_bridge;
static int sumh_fop_bridge_register(struct sumh_fop_bridge_entry *b,struct file_operations *ingress,struct file_operations *shadow,const struct file_operations *orig){(void)ingress;(void)shadow;(void)orig;b->registered=model_use_bridge;return 0;}
static void sumh_fop_bridge_unregister(struct sumh_fop_bridge_entry *b){b->registered=false;}
static struct {long long iterate_fop_entries,iterate_fop_wrapped;} sumh_hook_stats;
struct sumh_filldir_wrapper {struct dir_context wrap_ctx;bool has_iterate_client,stopped;unsigned emitted;};
static struct sumh_filldir_wrapper *sumh_iterate_prepare_wrapper(struct file *f,struct dir_context *c){(void)f;(void)c;return NULL;}
static void sumh_iterate_rewind_injected(struct dir_context *c){(void)c;}
static void sumh_iterate_emit_injected(struct sumh_filldir_wrapper *w){(void)w;}
static void sumh_iterate_finish_wrapper(struct sumh_filldir_wrapper *w){(void)w;}
"""
