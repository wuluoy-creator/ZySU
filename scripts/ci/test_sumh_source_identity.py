#!/usr/bin/env python3
"""Execute production ADD_RULE publication against source-replacement faults.

The ioctl case and store upsert are extracted verbatim. Kernel path lookup,
capture, RCU, allocation and dirhijack services are doubles; this is not a
kernel build, device test or a proof of an atomic rule transaction.
"""
import argparse
import os
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test


MODEL = r"""
typedef unsigned int u32, umode_t;
typedef unsigned char u8;
#define EINVAL 22
#define ENOMEM 12
#define ENOENT 2
#define EIO 5
#define EACCES 13
#define ENOTDIR 20
#define ELOOP 40
#define ESTALE 116
#define EOPNOTSUPP 95
#define PATH_MAX 4096
#define GFP_KERNEL 0
#define LOOKUP_FOLLOW 1
#define SUMH_IOC_ADD_RULE 1
#define SUMH_HASH_BITS 0
#define SUMH_BLOOM_SIZE 64
#define SUMH_VNODE_F_LNK 1
#define SUMH_VNODE_F_DIR 2
#define SUMH_VNODE_F_SPECIAL 4
#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFLNK 0120000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFIFO 0010000
#define S_ISREG(m) (((m)&S_IFMT)==S_IFREG)
#define S_ISDIR(m) (((m)&S_IFMT)==S_IFDIR)
#define S_ISLNK(m) (((m)&S_IFMT)==S_IFLNK)
#define S_ISCHR(m) (((m)&S_IFMT)==S_IFCHR)
#define S_ISBLK(m) (((m)&S_IFMT)==S_IFBLK)
#define S_ISFIFO(m) (((m)&S_IFMT)==S_IFIFO)
#define ERR_PTR(n) ((void *)(ssize_t)(n))
#define IS_ERR(p) ((size_t)(p)>=(size_t)-4095)
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
#define pr_warn(...) ((void)0)
#define sumh_log(...) ((void)0)
static char *strrchr(const char *s,int c) { const char *r=NULL; do { if(*s==c)r=s; }while(*s++);return (char *)r; }
static char *strcpy(char *d,const char *s) { memcpy(d,s,strlen(s)+1);return d; }
static char *strcat(char *d,const char *s) { strcpy(d+strlen(d),s);return d; }
static int bad,allocs,fail_at,live,peak;
static union { long long align; char data[8192]; } heap[64];
static bool used[64];
static void *kmalloc(size_t n,int flags) {
    (void)flags; allocs++; if(allocs==fail_at)return NULL;
    if(n>sizeof(heap[0].data)){bad++;return NULL;}
    for(int i=0;i<64;i++)if(!used[i]){used[i]=true;live++;if(live>peak)peak=live;return heap[i].data;}
    bad++;return NULL;
}
static void *kzalloc(size_t n,int f) {void *p=kmalloc(n,f);if(p)memset(p,0,n);return p;}
static char *kstrdup(const char *s,int f) {size_t n=strlen(s)+1;char *p=kmalloc(n,f);if(p)memcpy(p,s,n);return p;}
static void kfree(void *p) {if(!p)return;for(int i=0;i<64;i++)if(p==heap[i].data){if(!used[i])bad++;else{used[i]=false;live--;memset(p,0xa5,sizeof(heap[i]));}return;}bad++;}
struct inode {umode_t i_mode;int id,marks;};
struct dentry {struct inode *inode;int refs;};
struct vfsmount {int refs,id;};
struct path {struct vfsmount *mnt;struct dentry *dentry;};
static struct inode old_i,new_i,link_i,visible_i;
static struct dentry old_d,new_d,link_d,visible_d;
static struct vfsmount old_m,new_m,visible_m;
static struct path captured,link_path,current_target,shadow;
static int path_gets,path_puts,target_lookups,capture_error,shadow_error,mark_error,lookup_error;
static bool replace_on_lookup;
static int shadow_calls,shadow_flags,shadow_ino,publishes,retire_count,injects;
static bool nofollow,dh_enabled=true,sumh_device_sources_enabled=true,retire_immediately;
static struct inode *d_inode(struct dentry *d){return d?d->inode:NULL;}
static void path_get(const struct path *p){if(!p->mnt||!p->dentry){bad++;return;}p->mnt->refs++;p->dentry->refs++;path_gets++;}
static void path_put(const struct path *p){if(!p->mnt||!p->dentry||p->mnt->refs<=0||p->dentry->refs<=0){bad++;return;}p->mnt->refs--;p->dentry->refs--;path_puts++;}
static bool path_equal(const struct path *a,const struct path *b){return a->mnt==b->mnt&&a->dentry==b->dentry;}
static void ihold(struct inode *i){(void)i;}
static void iput(struct inode *i){(void)i;}
static int kern_path(const char *s,int flags,struct path *p){
    (void)flags;
    if(!strcmp(s,"/source")){target_lookups++;if(lookup_error)return lookup_error;*p=current_target;if(replace_on_lookup)current_target=(struct path){&new_m,&new_d};}
    else if(!strcmp(s,"/visible/file")||!strcmp(s,"/visible")){p->mnt=&visible_m;p->dentry=&visible_d;}
    else return -ENOENT;
    path_get(p);return 0;
}
static char *d_path(const struct path *p,char *b,size_t n){(void)p;(void)n;strcpy(b,"/visible/file");return b;}
struct hlist_node {struct hlist_node *next,**prev;};
struct hlist_head {struct hlist_node *first;};
struct rcu_head {int unused;};
struct sumh_entry {
    char *src,*target,*source_canonical;u32 src_hash;u8 type;
    struct path source_path,source_nofollow_path;
    bool source_path_valid,source_nofollow_path_valid;
    struct inode *source_inode;umode_t source_mode;
    unsigned long visible_ino,nofollow_visible_ino;
    struct hlist_node node;struct rcu_head rcu;
};
static struct hlist_head sumh_paths[1];
static int sumh_config_mutex,sumh_rule_count,lock_depth;
static unsigned long sumh_path_bloom[1];
#define container_of(p,t,m) ((t *)((char *)(p)-__builtin_offsetof(t,m)))
#define hentry(p) ((p)?container_of(p,struct sumh_entry,node):NULL)
#define typed_entry(p,t,m) ((p)?container_of(p,t,m):NULL)
#define hlist_for_each_entry(pos,head,member) for(pos=typed_entry((head)->first,__typeof__(*pos),member);pos;pos=typed_entry(pos->member.next,__typeof__(*pos),member))
#define hash_min(h,b) 0
static void mutex_lock(int *m){(void)m;if(lock_depth)bad++;lock_depth++;}
static void mutex_unlock(int *m){(void)m;if(lock_depth!=1)bad++;lock_depth--;}
static void hlist_add_head_rcu(struct hlist_node *n,struct hlist_head *h){n->next=h->first;n->prev=&h->first;if(n->next)n->next->prev=&n->next;h->first=n;publishes++;}
static void hlist_del_rcu(struct hlist_node *n){*n->prev=n->next;if(n->next)n->next->prev=n->prev;}
static unsigned long full_name_hash(void *salt,const char *s,size_t n){(void)salt;unsigned long h=0;for(size_t i=0;i<n;i++)h=h*31+(unsigned char)s[i];return h;}
static unsigned long jhash(const char *s,size_t n,int seed){return full_name_hash(NULL,s,n)+(unsigned)seed;}
static void set_bit(unsigned long n,unsigned long *b){*b|=1UL<<(n%32);}
static void atomic_inc(int *p){(*p)++;}
static void sumh_entry_release_source(struct sumh_entry *e){
    if(e->source_path_valid){path_put(&e->source_path);e->source_path_valid=false;}
    if(e->source_nofollow_path_valid){path_put(&e->source_nofollow_path);e->source_nofollow_path_valid=false;}
}
static int sumh_entry_capture_source(struct sumh_entry *e,const char *s){
    (void)s;if(capture_error)return capture_error;
    e->source_path=captured;path_get(&e->source_path);e->source_path_valid=true;
    e->source_inode=d_inode(captured.dentry);e->source_mode=e->source_inode->i_mode;e->visible_ino=e->source_inode->id+100;
    if(nofollow){e->source_nofollow_path=link_path;path_get(&e->source_nofollow_path);e->source_nofollow_path_valid=true;e->nofollow_visible_ino=303;}
    /* An atomic rename changes the string target immediately after capture. */
    current_target=(struct path){&new_m,&new_d};return 0;
}
static void sumh_entry_free_rcu(struct rcu_head *r){
    struct sumh_entry *e=container_of(r,struct sumh_entry,rcu);
    sumh_entry_release_source(e);kfree(e->src);kfree(e->target);kfree(e->source_canonical);kfree(e);
}
static struct rcu_head *retired[16];
static void call_rcu(struct rcu_head *r,void (*fn)(struct rcu_head *)){
    if(retire_immediately)fn(r);else retired[retire_count++]=r;
}
static int sumh_iop_mark_spoof(struct inode *i){i->marks++;return mark_error;}
static bool sumh_dirhijack_enabled(void){return dh_enabled;}
static void sumh_mark_dir_has_inject(const char *s){(void)s;}
static void sumh_add_inject_rule(char *s){injects++;kfree(s);}
static int sumh_dirhijack_add(const char *s,const struct path *p,unsigned long ino,u8 flags){
    (void)s;shadow_calls++;shadow_flags=flags;shadow_ino=ino;
    if(!p->mnt||!p->dentry||p->mnt->refs<=0||p->dentry->refs<=0)bad++;
    if(shadow_error)return shadow_error;
    if(shadow.dentry)path_put(&shadow);shadow=*p;path_get(&shadow);return 0;
}
"""


CASES = r"""
static void setup(umode_t mode){
    bad=allocs=fail_at=live=peak=path_gets=path_puts=target_lookups=0;
    capture_error=shadow_error=mark_error=shadow_calls=publishes=retire_count=injects=0;
    lookup_error=0;replace_on_lookup=false;
    nofollow=false;dh_enabled=true;sumh_device_sources_enabled=true;retire_immediately=false;
    sumh_paths[0].first=NULL;sumh_rule_count=lock_depth=0;sumh_path_bloom[0]=0;
    memset(used,0,sizeof(used));memset(&shadow,0,sizeof(shadow));
    old_i=(struct inode){mode,1,0};new_i=(struct inode){mode,2,0};link_i=(struct inode){S_IFLNK,3,0};visible_i=(struct inode){S_IFDIR,4,0};
    old_d=(struct dentry){&old_i,0};new_d=(struct dentry){&new_i,0};link_d=(struct dentry){&link_i,0};visible_d=(struct dentry){&visible_i,0};
    old_m=(struct vfsmount){0,1};new_m=(struct vfsmount){0,2};visible_m=(struct vfsmount){0,3};
    captured=(struct path){&old_m,&old_d};link_path=(struct path){&old_m,&link_d};current_target=captured;
}
static void cleanup(void){
    if(shadow.dentry){path_put(&shadow);memset(&shadow,0,sizeof(shadow));}
    while(sumh_paths[0].first){struct sumh_entry *e=hentry(sumh_paths[0].first);hlist_del_rcu(&e->node);sumh_entry_free_rcu(&e->rcu);}
    for(int i=0;i<retire_count;i++)sumh_entry_free_rcu(retired[i]);retire_count=0;
}
static int clean(void){return bad==0&&live==0&&lock_depth==0&&path_gets==path_puts&&!old_m.refs&&!new_m.refs&&!visible_m.refs&&!old_d.refs&&!new_d.refs&&!link_d.refs&&!visible_d.refs;}
int main(void){
    const umode_t modes[]={S_IFREG,S_IFDIR,S_IFCHR,S_IFBLK,S_IFIFO};
    const int flags[]={0,SUMH_VNODE_F_DIR,SUMH_VNODE_F_SPECIAL,SUMH_VNODE_F_SPECIAL,SUMH_VNODE_F_SPECIAL};
    for(int i=0;i<5;i++){
        setup(modes[i]);CHECK(submit()==0);CHECK(publishes==1&&sumh_rule_count==2);
        CHECK(target_lookups==0);CHECK(path_equal(&shadow,&captured));CHECK(shadow_ino==101&&shadow_flags==flags[i]);
        CHECK(old_i.marks==1&&new_i.marks==0);cleanup();CHECK(clean());
    }
    setup(S_IFREG);nofollow=true;CHECK(submit()==0);CHECK(target_lookups==0);
    CHECK(path_equal(&shadow,&link_path)&&shadow_ino==303&&shadow_flags==SUMH_VNODE_F_LNK);
    CHECK(old_i.marks==1&&new_i.marks==0);cleanup();CHECK(clean());

    /* Same target text, different source inode/mount: new mapping must mark
       and use the new source; an existing FD can retain the old reference. */
    setup(S_IFREG);CHECK(submit()==0);struct path old_fd=captured;path_get(&old_fd);
    captured=(struct path){&new_m,&new_d};CHECK(submit()==0);
    CHECK(sumh_rule_count==2&&retire_count==1&&target_lookups==0&&table_count(sumh_inject_dirs)==1);
    CHECK(path_equal(&shadow,&captured)&&shadow_ino==102);
    CHECK(old_i.marks==1&&new_i.marks==1&&old_fd.dentry==&old_d);
    cleanup();CHECK(old_m.refs==1&&old_d.refs==1);path_put(&old_fd);CHECK(clean());

    setup(S_IFREG);CHECK(submit()==0);CHECK(submit()==0);
    CHECK(table_count(sumh_inject_dirs)==1&&sumh_rule_count==2&&shadow_calls==2);cleanup();CHECK(clean());
    setup(S_IFREG);CHECK(submit()==0);captured.mnt=&new_m;
    CHECK(submit()==0);CHECK(table_count(sumh_inject_dirs)==1&&shadow.mnt==&new_m&&shadow.dentry==&old_d);cleanup();CHECK(clean());
    setup(S_IFREG);nofollow=true;CHECK(submit()==0);link_path.mnt=&new_m;
    CHECK(submit()==0);CHECK(table_count(sumh_inject_dirs)==1&&shadow.mnt==&new_m&&shadow.dentry==&link_d);cleanup();CHECK(clean());
    setup(S_IFREG);CHECK(submit()==0);nofollow=true;CHECK(submit()==0);
    CHECK(table_count(sumh_inject_dirs)==1&&shadow.dentry==&link_d);nofollow=false;CHECK(submit()==0);
    CHECK(table_count(sumh_inject_dirs)==1&&shadow.dentry==&old_d);cleanup();CHECK(clean());

    setup(S_IFREG);CHECK(submit()==0);retire_immediately=true;
    captured=(struct path){&new_m,&new_d};CHECK(submit()==0);
    CHECK(path_equal(&shadow,&captured)&&new_i.marks==1);cleanup();CHECK(clean());
    setup(S_IFREG);dh_enabled=false;CHECK(submit()==-EOPNOTSUPP);CHECK(!shadow_calls&&!old_i.marks&&target_lookups==0);cleanup();CHECK(clean());
    const int capture_errors[]={-ENOENT,-ENOTDIR,-EACCES,-ELOOP,-EIO,-ENOMEM,-ESTALE};
    for(int existing=0;existing<2;existing++)for(int i=0;i<7;i++){
        setup(S_IFREG);if(existing)CHECK(submit()==0);
        struct hlist_node *prior=sumh_paths[0].first;struct path prior_shadow=shadow;
        int prior_live=live,prior_refs=path_gets-path_puts,prior_marks=old_i.marks;
        capture_error=capture_errors[i];CHECK(submit()==capture_errors[i]);
        CHECK(sumh_paths[0].first==prior&&publishes==existing&&shadow_calls==existing);
        CHECK(path_equal(&shadow,&prior_shadow)&&sumh_rule_count==2*existing&&!retire_count);
        CHECK(live==prior_live&&path_gets-path_puts==prior_refs&&old_i.marks==prior_marks&&table_count(sumh_inject_dirs)==existing);
        cleanup();CHECK(clean());
    }
    /* DH preparation is now required before publication; failure preserves
       the prior indexed/source state and does not publish an incomplete row. */
    setup(S_IFREG);shadow_error=-ENOMEM;CHECK(submit()==-ENOMEM);CHECK(!publishes&&!shadow_calls&&!shadow.dentry);cleanup();CHECK(clean());

    /* Every allocation in the real case: failed ownership preparation must
       not publish, mark the source or leak native references. */
    setup(S_IFREG);CHECK(submit()==0);int count=allocs;cleanup();CHECK(clean());
    for(int i=3;i<=count;i++){
        setup(S_IFREG);fail_at=i;int ret=submit();CHECK(ret==-ENOMEM);
        CHECK(target_lookups==0);if(!publishes)CHECK(!shadow_calls&&old_i.marks==0);
        cleanup();CHECK(clean());
    }
    return 0;
}
"""


def source():
    from test_sumh_inject_claims_source import add_source
    return add_source() + "int main(void){" + CASES.split("int main(void){", 1)[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    run_test(args.cc, "source-identity-add", source())


if __name__ == "__main__":
    main()
