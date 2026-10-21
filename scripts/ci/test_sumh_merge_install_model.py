"""Kernel-service model for the production MERGE installation regression.

This deliberately models multiple pinned directories and open iterator files.
Source metadata capture is covered separately by test_sumh_source_capture.py.
"""

from test_sumh_source_identity import MODEL as IDENTITY_MODEL

MODEL = IDENTITY_MODEL.replace(
    "struct inode {umode_t i_mode;int id,marks;};",
    "struct inode {umode_t i_mode;int id,marks;unsigned long i_rdev;};",
).replace("heap[64]", "heap[256]").replace("used[64]", "used[256]").replace(
    "i<64", "i<256"
).replace("retired[16]", "retired[256]").replace(
    "n->next=h->first;n->prev=", "if(iter_lock)bad++;n->next=h->first;n->prev="
).replace(
    "i->marks++;return mark_error;", "if(iter_lock)bad++;i->marks++;return mark_error;"
).replace(
    "injects++;kfree(s);", "if(iter_lock)bad++;injects++;kfree(s);"
)

EXTRA = r"""
typedef long long loff_t;
typedef unsigned long long u64;
#define SUMH_IOC_ADD_MERGE_RULE 2
#define LOOKUP_DIRECTORY 2
#define ENOTDIR 20
#define EAGAIN 11
#define EEXIST 17
#define ELOOP 40
#define ESTALE 116
#define E2BIG 7
#define ENAMETOOLONG 36
#define NAME_MAX 255
#define SUMH_MAX_LEN_PATHNAME 4096
#define PTR_ERR(p) ((int)(ssize_t)(p))
#define O_RDONLY 0
#define O_DIRECTORY 1
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10
#define DT_CHR 2
#define DT_BLK 6
#define DT_FIFO 1
#define DT_SOCK 12
#define S_DT(mode) (((mode)&S_IFMT)>>12)
struct list_head {struct list_head *next,*prev;};
#define LIST_HEAD_INIT(name) {&(name),&(name)}
#define LIST_HEAD(name) struct list_head name=LIST_HEAD_INIT(name)
#define INIT_LIST_HEAD(p) do{(p)->next=(p);(p)->prev=(p);}while(0)
static bool list_empty(const struct list_head *p){return p->next==p;}
static void list_add_tail(struct list_head *n,struct list_head *head){n->next=head;n->prev=head->prev;head->prev->next=n;head->prev=n;}
static void list_del(struct list_head *n){n->prev->next=n->next;n->next->prev=n->prev;}
#define list_first_entry(h,t,m) container_of((h)->next,t,m)
#define list_for_each_entry(p,h,m) for(p=container_of((h)->next,__typeof__(*p),m);&p->m!=(h);p=container_of(p->m.next,__typeof__(*p),m))
#define list_for_each_entry_safe(p,n,h,m) for(p=container_of((h)->next,__typeof__(*p),m),n=container_of(p->m.next,__typeof__(*p),m);&p->m!=(h);p=n,n=container_of(n->m.next,__typeof__(*n),m))
static void cond_resched(void){}
static void *memchr(const void *p,int c,size_t n){const unsigned char *s=p;for(size_t i=0;i<n;i++)if(s[i]==(unsigned char)c)return (void *)(s+i);return NULL;}
static int memcmp(const void *a,const void *b,size_t n){const unsigned char *x=a,*y=b;for(size_t i=0;i<n;i++)if(x[i]!=y[i])return x[i]-y[i];return 0;}
static char *kasprintf(int f,const char *fmt,const char *prefix,int len,const char *name){
    if(strcmp(fmt,"%s/%.*s")||len<0){bad++;return NULL;}
    size_t pn=strlen(prefix);char *out=kmalloc(pn+(size_t)len+2,f);if(!out)return NULL;
    memcpy(out,prefix,pn);out[pn]='/';memcpy(out+pn+1,name,(size_t)len);out[pn+len+1]=0;return out;
}
struct sumh_merge_entry {char *src,*target,*resolved_src;struct path target_path;struct hlist_node node;};
static struct hlist_head sumh_merge_dirs[1];
struct model_item {const char *name;int len;unsigned type;struct path child;};
struct model_dir {struct path path;struct model_item items[16];int count;bool marker;};
static struct model_dir dirs[16];
static int dir_count;
static struct inode extra_i[16];
static struct dentry extra_d[16];
static struct path extra_p[16];
static int extra_count;
static bool vnode_source,trylock_fail;
static int dir_lock,lookup_child_error,lookup_children,marker_refs;
static int open_error,iterate_error,opens,iterations,files_live,files_peak;
static int batch_size,iteration_return,open_fail_at,child_error,checked_error,visible_error;
static bool no_progress,visible_children,replace_before_children,replace_before_checked;
static int relative_lookups,resolved_captures,checked_captures,source_text_lookups;
static struct dentry marker;
static struct path opened_path,first_child,checked_leaf,followed_link;
static struct model_dir *find_dir(const struct path *p){for(int i=0;i<dir_count;i++)if(path_equal(&dirs[i].path,p))return &dirs[i];return NULL;}
static int merge_kern_path(const char *s,int flags,struct path *p){
    if(iter_lock)bad++;
    if(!strncmp(s,"/visible/file/",14)){if(visible_error)return visible_error;if(!visible_children)return -ENOENT;return kern_path("/visible/file",flags,p);}
    if(!strncmp(s,"/source/",8)){source_text_lookups++;if(lookup_error)return lookup_error;*p=first_child;if(replace_before_checked)*p=(struct path){&new_m,&new_d};path_get(p);return 0;}
    return kern_path(s,flags,p);
}
#define kern_path merge_kern_path
static int vfs_path_lookup(struct dentry *d,struct vfsmount *m,const char *name,unsigned flags,struct path *p){
    relative_lookups++;if(iter_lock||flags)bad++;
    if(child_error)return child_error;
    struct path parent={m,d};struct model_dir *dir=find_dir(&parent);if(!dir)return -ENOENT;
    if(replace_before_children)current_target=(struct path){&new_m,&new_d};
    for(int i=0;i<dir->count;i++)if((int)strlen(name)==dir->items[i].len&&!memcmp(name,dir->items[i].name,(size_t)dir->items[i].len)){
        *p=dir->items[i].child;if(!p->dentry)return -ENOENT;path_get(p);return 0;
    }
    return -ENOENT;
}
static int sumh_entry_capture_resolved(struct sumh_entry *e,const struct path *p){
    resolved_captures++;if(iter_lock)bad++;if(capture_error)return capture_error;
    if(!d_inode(p->dentry))return -ENOENT;if(S_ISLNK(d_inode(p->dentry)->i_mode))return -ELOOP;
    e->source_path=*p;path_get(p);e->source_path_valid=true;e->source_inode=d_inode(p->dentry);
    e->source_mode=e->source_inode->i_mode;e->visible_ino=e->source_inode->id+100;return 0;
}
static int sumh_entry_capture_source_checked(struct sumh_entry *e,const char *text,const struct path *expected){
    checked_captures++;checked_leaf=*expected;if(iter_lock)bad++;if(checked_error)return checked_error;
    struct path leaf;int ret=kern_path(text,0,&leaf);if(ret)return ret;
    if(!path_equal(&leaf,expected)){path_put(&leaf);return -ESTALE;}
    ret=sumh_entry_capture_resolved(e,&followed_link);
    if(!ret){e->source_nofollow_path=leaf;path_get(&leaf);e->source_nofollow_path_valid=true;e->nofollow_visible_ino=303;}
    path_put(&leaf);return ret;
}
static int sumh_dirhijack_add_shadow(const char *s,const struct path *p,unsigned long ino,u8 flags){if(iter_lock)bad++;return sumh_dirhijack_add(s,p,ino,flags);}
static bool sumh_vnode_is_ours(struct inode *i){(void)i;return vnode_source;}
static bool inode_trylock(struct inode *i){(void)i;if(iter_lock)bad++;if(trylock_fail)return false;if(dir_lock)bad++;dir_lock++;return true;}
static void inode_unlock(struct inode *i){(void)i;if(dir_lock!=1)bad++;dir_lock--;}
static struct dentry *lookup_one(const char *name,struct dentry *parent,int len){
    lookup_children++;if(iter_lock||dir_lock!=1||len!=8||strcmp(name,".replace"))bad++;
    if(lookup_child_error)return ERR_PTR(lookup_child_error);
    bool found=false;for(int i=0;i<dir_count;i++)if(dirs[i].path.dentry==parent)found=dirs[i].marker;
    marker.inode=found?&link_i:NULL;marker_refs++;return &marker;
}
static struct dentry *(*sumh_lookup_one_len)(const char *,struct dentry *,int)=lookup_one;
static void dput(struct dentry *d){if(d!=&marker||marker_refs!=1)bad++;marker_refs--;}
struct file {struct path f_path;loff_t f_pos;struct model_dir *dir;bool live;};
struct dir_context {bool (*actor)(struct dir_context *,const char *,int,loff_t,u64,unsigned int);loff_t pos;};
static struct file files[16];
static int cred;
static const void *current_cred(void){return &cred;}
static struct file *dentry_open(const struct path *p,int flags,const void *c){
    opens++;if(iter_lock||flags!=O_DIRECTORY||c!=&cred||p->mnt->refs<=0||p->dentry->refs<=0)bad++;
    if(open_error||opens==open_fail_at)return ERR_PTR(open_error?open_error:-EIO);
    for(int i=0;i<16;i++)if(!files[i].live){struct file *f=&files[i];f->f_path=*p;path_get(p);opened_path=*p;
        f->f_pos=0;f->dir=find_dir(p);f->live=true;files_live++;if(files_live>files_peak)files_peak=files_live;return f;}
    bad++;return ERR_PTR(-ENOMEM);
}
static void fput(struct file *f){if(!f->live||iter_lock)bad++;f->live=false;files_live--;path_put(&f->f_path);}
static int iterate_dir(struct file *f,struct dir_context *ctx){
    iterations++;if(!f->live||iter_lock)bad++;if(iterate_error)return iterate_error;
    ctx->pos=f->f_pos;iter_lock++;
    if(f->dir){int limit=batch_size?batch_size:16,count=0;
        while(f->f_pos<f->dir->count&&count++<limit){struct model_item *it=&f->dir->items[(int)f->f_pos];
            if(!ctx->actor(ctx,it->name,it->len,f->f_pos,1,it->type))break;
            if(no_progress)break;f->f_pos++;ctx->pos=f->f_pos;
        }}
    iter_lock--;return iteration_return;
}
SUMH_NOCFI int sumh_materialize_merge(const char *,const char *,const struct path *,int);
"""
