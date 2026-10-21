#!/usr/bin/env python3
"""Execute actual SUMH source capture, metadata population and reference release.

Kernel lookup/getattr/identity/allocation services are controlled doubles. The
tests check held non-link identity, bounded symlink replacement detection and
native lookup delegation, not atomic symlink pairs, a kernel build, or real
mount/security behavior. Optional --sensitivity rejects generated-C regressions.
"""
import argparse
import os
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, function, run_test


MODEL = r"""
typedef unsigned int umode_t;
#define EINVAL 22
#define EALREADY 114
#define ENOENT 2
#define ENOMEM 12
#define ENOTDIR 20
#define EIO 5
#define EACCES 13
#define ELOOP 40
#define ESTALE 116
#define EOPNOTSUPP 95
#define GFP_KERNEL 0
#define SUMH_MAX_LEN_PATHNAME 4096
#define LOOKUP_FOLLOW 1
#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFLNK 0120000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFIFO 0010000
#define S_IFSOCK 0140000
#define S_ISREG(m) (((m)&S_IFMT)==S_IFREG)
#define S_ISDIR(m) (((m)&S_IFMT)==S_IFDIR)
#define S_ISLNK(m) (((m)&S_IFMT)==S_IFLNK)
#define S_ISCHR(m) (((m)&S_IFMT)==S_IFCHR)
#define S_ISBLK(m) (((m)&S_IFMT)==S_IFBLK)
#define S_ISFIFO(m) (((m)&S_IFMT)==S_IFIFO)
#define S_ISSOCK(m) (((m)&S_IFMT)==S_IFSOCK)
#define STATX_TYPE 1
#define STATX_MODE 2
#define STATX_NLINK 4
#define STATX_UID 8
#define STATX_GID 16
#define STATX_INO 32
#define STATX_SIZE 64
#define STATX_BLOCKS 128
#define STATX_BASIC_STATS 255
#define STATX_BTIME 256
#define AT_STATX_SYNC_AS_STAT 0
#define ERR_PTR(n) ((void *)(ssize_t)(n))
#define IS_ERR(p) ((size_t)(p)>=(size_t)-4095)
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
static int warn_count;
#define pr_warn(...) (warn_count++)
static int bad,allocs,fail_at,live;
static union {long long align;char data[4096];} heap[8];
static bool used[8];
static void *kmalloc(size_t n,int flags){
    (void)flags;allocs++;if(allocs==fail_at)return NULL;
    if(n>sizeof(heap[0].data)){bad++;return NULL;}
    for(int i=0;i<8;i++)if(!used[i]){used[i]=true;live++;return heap[i].data;}
    bad++;return NULL;
}
static char *kstrdup(const char *s,int flags){size_t n=strlen(s)+1;char *p=kmalloc(n,flags);if(p)memcpy(p,s,n);return p;}
static void kfree(void *p){
    if(!p)return;
    for(int i=0;i<8;i++)if(p==heap[i].data){
        if(!used[i])bad++;else{used[i]=false;live--;memset(p,0xa5,sizeof(heap[i]));}return;
    }bad++;
}
struct super_block {unsigned long s_dev;};
struct inode {
    umode_t i_mode;unsigned long i_ino;struct super_block *i_sb;
    unsigned int i_uid,i_gid,i_nlink,i_blkbits;
    unsigned long i_rdev;long long i_size,i_blocks;
};
struct dentry {struct inode *inode;int refs;};
struct vfsmount {int refs,id;};
struct path {struct vfsmount *mnt;struct dentry *dentry;};
struct kstat {
    unsigned int result_mask;unsigned long dev,ino;umode_t mode;
    unsigned int nlink,uid,gid;unsigned long rdev;
    long long size,blocks;unsigned long blksize;
    long long atime,mtime,ctime,btime;
};
struct sumh_entry {
    char *src,*target,*source_canonical;
    struct path source_path,source_nofollow_path;
    bool source_path_valid,source_nofollow_path_valid;
    struct inode *source_inode;
    unsigned long source_ino,source_dev;
    umode_t source_mode,source_nofollow_mode;
    unsigned int source_uid,source_gid;
    long long source_size;
    struct kstat source_stat,visible_stat,source_nofollow_stat;
    bool source_stat_valid,visible_stat_valid,source_nofollow_stat_valid;
    bool preserve_visible_metadata;
    unsigned long visible_ino,visible_dev,nofollow_visible_ino,nofollow_visible_dev;
};
static struct super_block source_sb,visible_sb;
static struct inode old_i,new_i,link_i,visible_i;
static struct dentry old_d,new_d,link_d,other_link_d,visible_d;
static struct vfsmount old_m,new_m,visible_m;
static struct path old_path,new_path,link_path,other_link_path,visible_path;
static struct path current_leaf,current_follow,replace_leaf,restore_leaf;
static struct kstat old_stat,new_stat,link_stat,visible_stat;
static int path_gets,path_puts,nofollow_calls,follow_calls,visible_calls;
static int leaf_error,follow_error,postcheck_error,visible_error,source_stat_error,nofollow_stat_error,visible_stat_error;
static int source_getattrs,nofollow_getattrs,visible_getattrs,canonical_calls,canonical_kind;
static bool replace_after_leaf,replace_after_follow,restore_after_follow,replace_after_postcheck;
static bool replace_during_getattr,sumh_device_sources_enabled;
static bool guard_required,guard_observed,postcheck_ref_live;
static const struct path *postcheck_ref;
static int source_lookup_flags[4],source_lookups;
static bool native_trailing_slash;
static const char *expected_query;
static int sumh_vnode_live_count,sumh_vnode_allocated_count;
static struct inode *d_inode(struct dentry *d){return d?d->inode:NULL;}
static long long i_size_read(struct inode *i){return i->i_size;}
static void path_get(const struct path *p){
    if(!p||!p->mnt||!p->dentry){bad++;return;}
    p->mnt->refs++;p->dentry->refs++;path_gets++;
}
static void path_put(const struct path *p){
    if(!p||!p->mnt||!p->dentry||p->mnt->refs<=0||p->dentry->refs<=0){bad++;return;}
    if(p==postcheck_ref){if(!postcheck_ref_live)bad++;postcheck_ref_live=false;}
    p->mnt->refs--;p->dentry->refs--;path_puts++;
}
static bool path_equal(const struct path *a,const struct path *b){return a->mnt==b->mnt&&a->dentry==b->dentry;}
static void atomic_inc(int *v){(*v)++;}
static void atomic_dec(int *v){(*v)--;if(*v<0)bad++;}
static void atomic64_inc(int *v){(*v)++;}
static int kern_path(const char *s,int flags,struct path *out){
    if(!strcmp(s,"/visible")){
        if(guard_required&&(!guard_observed||postcheck_ref_live))bad++;
        visible_calls++;if(flags!=LOOKUP_FOLLOW)bad++;
        if(visible_error)return visible_error;*out=visible_path;path_get(out);return 0;
    }
    if(!expected_query||strcmp(s,expected_query)){bad++;return -ENOENT;}
    if(source_lookups>=4){bad++;return -EIO;}
    source_lookup_flags[source_lookups++]=flags;
    if(flags==0){
        nofollow_calls++;
        if(nofollow_calls==1&&leaf_error)return leaf_error;
        if(nofollow_calls>2){bad++;return -EIO;}
        if(nofollow_calls==2){
            if(follow_calls!=1)bad++;
            if(postcheck_error)return postcheck_error;
        }
        *out=native_trailing_slash?current_follow:current_leaf;path_get(out);
        if(nofollow_calls==1){
            guard_required=d_inode(out->dentry)&&S_ISLNK(d_inode(out->dentry)->i_mode);
            if(replace_after_leaf)current_leaf=replace_leaf;
        }else{
            guard_observed=true;postcheck_ref=out;postcheck_ref_live=true;
            if(replace_after_postcheck)current_leaf=replace_leaf;
        }
        return 0;
    }
    if(flags!=LOOKUP_FOLLOW){bad++;return -EINVAL;}
    if(nofollow_calls!=1||follow_calls)bad++;
    follow_calls++;if(follow_error)return follow_error;
    *out=current_follow;path_get(out);
    if(replace_after_follow)current_leaf=replace_leaf;
    if(restore_after_follow)current_leaf=restore_leaf;
    return 0;
}
static int sumh_vfs_getattr_unprojected(const struct path *p,struct kstat *out,unsigned int mask,int flags){
    if(guard_required&&(!guard_observed||postcheck_ref_live))bad++;
    if(!p->dentry||!p->mnt||p->dentry->refs<=0||p->mnt->refs<=0)bad++;
    if(mask!=(STATX_BASIC_STATS|STATX_BTIME)||flags!=AT_STATX_SYNC_AS_STAT)bad++;
    if(p->dentry==&visible_d){visible_getattrs++;if(visible_stat_error)return visible_stat_error;*out=visible_stat;return 0;}
    if(p->dentry==&link_d){nofollow_getattrs++;if(nofollow_stat_error)return nofollow_stat_error;*out=link_stat;return 0;}
    source_getattrs++;if(replace_during_getattr)current_leaf=replace_leaf;
    if(source_stat_error)return source_stat_error;
    if(p->dentry==&old_d)*out=old_stat;else if(p->dentry==&new_d)*out=new_stat;else{bad++;return -ENOENT;}
    return 0;
}
static unsigned long sumh_vnode_ino_alloc(unsigned long dev,unsigned long ino){return 10000+dev*100+ino;}
static unsigned long sumh_vnode_visible_dev(const char *s){(void)s;return 77;}
static char *canonical(const struct path *p,char *buffer,size_t n){
    if(guard_required&&(!guard_observed||postcheck_ref_live))bad++;
    canonical_calls++;if(!p->mnt||!p->dentry||!p->mnt->refs||!p->dentry->refs)bad++;
    if(canonical_kind==1)return ERR_PTR(-ENOENT);
    if(canonical_kind==2)return NULL;
    const char *s=canonical_kind==3?"relative":p->dentry==&old_d?"/held/old":"/held/new";
    if(strlen(s)+1>n){bad++;return ERR_PTR(-EINVAL);}memcpy(buffer,s,strlen(s)+1);return buffer;
}
static char *(*sumh_d_absolute_path)(const struct path *,char *,size_t)=canonical;
"""


TESTS = r"""
static struct kstat make_stat(struct inode *i,int n){
    struct kstat s={0};s.result_mask=STATX_BASIC_STATS|STATX_BTIME;
    s.dev=i->i_sb?i->i_sb->s_dev:0;s.ino=i->i_ino;s.mode=i->i_mode;
    s.nlink=i->i_nlink;s.uid=i->i_uid;s.gid=i->i_gid;s.rdev=i->i_rdev;
    s.size=i->i_size;s.blocks=i->i_blocks;s.blksize=1U<<i->i_blkbits;
    s.atime=n+1;s.mtime=n+2;s.ctime=n+3;s.btime=n+4;return s;
}
static void init(struct sumh_entry *entry,umode_t mode){
    bad=allocs=fail_at=live=warn_count=path_gets=path_puts=0;
    nofollow_calls=follow_calls=visible_calls=leaf_error=follow_error=postcheck_error=visible_error=0;
    source_stat_error=nofollow_stat_error=visible_stat_error=0;
    source_getattrs=nofollow_getattrs=visible_getattrs=canonical_calls=canonical_kind=0;
    replace_after_leaf=replace_after_follow=restore_after_follow=replace_after_postcheck=false;
    replace_during_getattr=native_trailing_slash=false;
    guard_required=guard_observed=postcheck_ref_live=false;postcheck_ref=NULL;
    source_lookups=0;memset(source_lookup_flags,0,sizeof(source_lookup_flags));
    sumh_device_sources_enabled=true;expected_query="/source";
    sumh_vnode_live_count=sumh_vnode_allocated_count=0;sumh_d_absolute_path=canonical;
    memset(used,0,sizeof(used));memset(entry,0,sizeof(*entry));
    entry->src="/visible";entry->target="/source";
    source_sb.s_dev=3;visible_sb.s_dev=9;
    old_i=(struct inode){mode|0644,11,&source_sb,111,222,2,12,31,1234,8};
    new_i=(struct inode){S_IFREG|0600,12,&source_sb,333,444,1,10,32,5678,16};
    link_i=(struct inode){S_IFLNK|0777,13,&source_sb,555,666,3,9,0,17,2};
    visible_i=(struct inode){S_IFDIR|0751,14,&visible_sb,777,888,4,11,33,9000,20};
    old_d=(struct dentry){&old_i,0};new_d=(struct dentry){&new_i,0};
    link_d=(struct dentry){&link_i,0};other_link_d=(struct dentry){&link_i,0};visible_d=(struct dentry){&visible_i,0};
    old_m=(struct vfsmount){0,1};new_m=(struct vfsmount){0,2};visible_m=(struct vfsmount){0,3};
    old_path=(struct path){&old_m,&old_d};new_path=(struct path){&new_m,&new_d};
    link_path=(struct path){&old_m,&link_d};other_link_path=(struct path){&old_m,&other_link_d};
    visible_path=(struct path){&visible_m,&visible_d};
    current_leaf=old_path;current_follow=old_path;replace_leaf=new_path;restore_leaf=link_path;
    old_stat=make_stat(&old_i,100);new_stat=make_stat(&new_i,200);
    link_stat=make_stat(&link_i,300);visible_stat=make_stat(&visible_i,400);
}
static void finish(struct sumh_entry *e){sumh_entry_release_source(e);kfree(e->source_canonical);e->source_canonical=NULL;}
static int balanced(void){return !bad&&!live&&!sumh_vnode_live_count&&!postcheck_ref_live&&path_gets==path_puts&&!old_m.refs&&!new_m.refs&&!visible_m.refs&&!old_d.refs&&!new_d.refs&&!link_d.refs&&!other_link_d.refs&&!visible_d.refs;}
static int same_entry(const struct sumh_entry *a,const struct sumh_entry *b){
    const unsigned char *x=(const unsigned char *)a,*y=(const unsigned char *)b;
    for(size_t i=0;i<sizeof(*a);i++)if(x[i]!=y[i])return 0;return 1;
}
static int no_capture_work(void){
    return !allocs&&!canonical_calls&&!source_getattrs&&!nofollow_getattrs&&!visible_getattrs&&
        !visible_calls&&!sumh_vnode_allocated_count&&!sumh_vnode_live_count;
}
static int link_lookup_order(void){
    return source_lookups==3&&source_lookup_flags[0]==0&&
        source_lookup_flags[1]==LOOKUP_FOLLOW&&source_lookup_flags[2]==0;
}
static int source_matches(const struct sumh_entry *e,const struct path *p){
    return e->source_path_valid&&path_equal(&e->source_path,p)&&e->source_inode==d_inode(p->dentry)&&
        e->source_ino==d_inode(p->dentry)->i_ino&&e->source_mode==d_inode(p->dentry)->i_mode;
}
static int nonlink_identity(void){
    struct sumh_entry e;
    init(&e,S_IFREG);replace_after_leaf=true;replace_leaf=link_path;
    CHECK(sumh_entry_capture_source(&e,"/source")==0);
    CHECK(nofollow_calls==1&&follow_calls==0&&visible_calls==1);
    CHECK(source_matches(&e,&old_path)&&!e.source_nofollow_path_valid);
    CHECK(e.source_stat.ino==old_i.i_ino&&e.source_stat.size==old_i.i_size);
    CHECK(current_leaf.dentry==&link_d&&e.source_path.dentry==&old_d);
    CHECK(e.source_canonical&&!strcmp(e.source_canonical,"/held/old"));
    CHECK(sumh_vnode_allocated_count==1&&sumh_vnode_live_count==1);finish(&e);CHECK(balanced());

    init(&e,S_IFREG);replace_during_getattr=true;
    CHECK(sumh_entry_capture_source(&e,"/source")==0);
    CHECK(source_matches(&e,&old_path)&&!e.source_nofollow_path_valid&&nofollow_calls==1&&!follow_calls);
    CHECK(current_leaf.dentry==&new_d);finish(&e);CHECK(balanced());

    /* Held capture remains on the caller's mount+dentry after pathname rename. */
    init(&e,S_IFDIR);path_get(&old_path);current_leaf=new_path;expected_query=NULL;
    CHECK(sumh_entry_capture_resolved(&e,&old_path)==0);
    CHECK(source_matches(&e,&old_path)&&!nofollow_calls&&!follow_calls&&visible_calls==1);
    CHECK(old_d.refs==2&&old_m.refs==2);finish(&e);CHECK(old_d.refs==1&&old_m.refs==1);
    path_put(&old_path);CHECK(balanced());

    /* Separate mount identity is retained even when inode/dentry are shared. */
    init(&e,S_IFREG);struct path alias={&new_m,&old_d};path_get(&alias);
    CHECK(sumh_entry_capture_resolved(&e,&alias)==0);CHECK(source_matches(&e,&alias));
    CHECK(e.source_path.mnt==&new_m&&e.source_stat.ino==old_i.i_ino);
    finish(&e);path_put(&alias);CHECK(balanced());

    /* A prior FD can keep the old source alive after this definition releases. */
    init(&e,S_IFREG);struct path fd=old_path;path_get(&fd);
    CHECK(sumh_entry_capture_source(&e,"/source")==0);current_leaf=new_path;
    finish(&e);CHECK(fd.dentry==&old_d&&old_d.refs==1&&old_m.refs==1);
    path_put(&fd);CHECK(balanced());return 0;
}
static int checked_identity(void){
    struct sumh_entry e;
    for(int variant=0;variant<3;variant++){
        init(&e,S_IFREG);struct path expected=old_path;
        if(variant==0)expected=new_path;
        if(variant==1)expected.mnt=&new_m;
        if(variant==2)expected.dentry=&link_d;
        path_get(&expected);
        CHECK(sumh_entry_capture_source_checked(&e,"/source",&expected)==-ESTALE);
        CHECK(nofollow_calls==1&&!follow_calls&&!visible_calls&&!source_getattrs&&!allocs);
        CHECK(!e.source_path_valid&&!e.source_nofollow_path_valid&&!sumh_vnode_allocated_count);
        path_put(&expected);finish(&e);CHECK(balanced());
    }
    init(&e,S_IFREG);path_get(&old_path);replace_after_leaf=true;
    CHECK(sumh_entry_capture_source_checked(&e,"/source",&old_path)==0);
    CHECK(source_matches(&e,&old_path)&&nofollow_calls==1&&!follow_calls);
    finish(&e);path_put(&old_path);CHECK(balanced());
    init(&e,S_IFREG);current_leaf=link_path;path_get(&other_link_path);
    CHECK(sumh_entry_capture_source_checked(&e,"/source",&other_link_path)==-ESTALE);
    CHECK(nofollow_calls==1&&!follow_calls&&no_capture_work());
    path_put(&other_link_path);finish(&e);CHECK(balanced());return 0;
}
static int native_links(void){
    struct sumh_entry e;
    const char *spellings[]={"/absolute-link","/relative-link","/parent-link","/magic-link",
        "relative/link","/some/../parent-link","/proc/self/fd/7"};
    for(int i=0;i<7;i++){
        init(&e,S_IFREG);expected_query=spellings[i];current_leaf=link_path;
        CHECK(sumh_entry_capture_source(&e,spellings[i])==0);
        CHECK(nofollow_calls==2&&follow_calls==1&&visible_calls==1&&link_lookup_order());
        CHECK(source_matches(&e,&old_path)&&e.source_nofollow_path_valid);
        CHECK(path_equal(&e.source_nofollow_path,&link_path)&&e.source_nofollow_mode==link_i.i_mode);
        CHECK(e.source_nofollow_stat_valid&&e.source_nofollow_stat.size==link_i.i_size);
        CHECK(e.source_nofollow_stat.ino==sumh_vnode_ino_alloc(3,13));
        CHECK(e.source_nofollow_stat.dev==77&&e.nofollow_visible_ino!=e.visible_ino);
        CHECK(old_d.refs==1&&link_d.refs==1&&old_m.refs==2);finish(&e);CHECK(balanced());
    }
    /* The service double returns the directory for link/ even with flags0,
       matching native trailing-slash semantics; capture must not synthesize a link. */
    init(&e,S_IFDIR);expected_query="/directory-link/";current_leaf=link_path;native_trailing_slash=true;
    CHECK(sumh_entry_capture_source(&e,expected_query)==0);
    CHECK(source_matches(&e,&old_path)&&!e.source_nofollow_path_valid&&nofollow_calls==1&&!follow_calls);
    finish(&e);CHECK(balanced());

    const int errors[]={-ENOENT,-EACCES,-ELOOP,-EIO};
    for(int i=0;i<4;i++){
        init(&e,S_IFREG);current_leaf=link_path;follow_error=errors[i];
        CHECK(sumh_entry_capture_source(&e,"/source")==errors[i]);
        CHECK(nofollow_calls==1&&follow_calls==1&&!visible_calls&&!allocs&&!e.source_path_valid);
        finish(&e);CHECK(balanced());
    }
    init(&e,S_IFREG);current_leaf=link_path;current_follow=link_path;
    CHECK(sumh_entry_capture_source(&e,"/source")==-ELOOP);
    CHECK(link_lookup_order()&&!e.source_path_valid&&!sumh_vnode_allocated_count&&!allocs);
    finish(&e);CHECK(balanced());
    return 0;
}
static int guarded_replacements(void){
    struct sumh_entry e,before;
    for(int checked=0;checked<2;checked++)for(int phase=0;phase<2;phase++)for(int kind=0;kind<3;kind++){
        init(&e,S_IFREG);current_leaf=link_path;current_follow=new_path;
        replace_leaf=kind==0?new_path:kind==1?other_link_path:(struct path){&new_m,&link_d};
        replace_after_leaf=phase==0;replace_after_follow=phase==1;
        if(checked)path_get(&link_path);memcpy(&before,&e,sizeof(e));
        int ret=checked?sumh_entry_capture_source_checked(&e,"/source",&link_path):sumh_entry_capture_source(&e,"/source");
        CHECK(ret==-ESTALE&&link_lookup_order()&&nofollow_calls==2&&follow_calls==1);
        CHECK(same_entry(&e,&before)&&no_capture_work()&&!postcheck_ref_live);
        if(checked){CHECK(link_d.refs==1&&old_m.refs==1);path_put(&link_path);}
        CHECK(balanced());finish(&e);CHECK(balanced());
    }
    return 0;
}
static int guarded_errors(void){
    struct sumh_entry e,before;
    const int errors[]={-ENOENT,-ENOTDIR,-EACCES,-ELOOP,-EIO,-ENOMEM,-ESTALE};
    for(int checked=0;checked<2;checked++)for(int i=0;i<7;i++){
        init(&e,S_IFREG);current_leaf=link_path;postcheck_error=errors[i];
        if(checked)path_get(&link_path);memcpy(&before,&e,sizeof(e));
        int ret=checked?sumh_entry_capture_source_checked(&e,"/source",&link_path):sumh_entry_capture_source(&e,"/source");
        CHECK(ret==errors[i]&&link_lookup_order()&&nofollow_calls==2&&follow_calls==1);
        CHECK(same_entry(&e,&before)&&no_capture_work()&&!postcheck_ref_live);
        if(checked){CHECK(link_d.refs==1&&old_m.refs==1);path_put(&link_path);}
        CHECK(balanced());finish(&e);CHECK(balanced());
    }
    /* The guard succeeds, but capture itself still rejects invalid sources. */
    for(int kind=0;kind<3;kind++){
        init(&e,S_IFREG);current_leaf=link_path;
        if(kind==0)old_d.inode=NULL;
        if(kind==1){old_i.i_mode=S_IFSOCK;old_stat.mode=S_IFSOCK;}
        if(kind==2){old_i.i_mode=S_IFCHR;sumh_device_sources_enabled=false;}
        memcpy(&before,&e,sizeof(e));
        CHECK(sumh_entry_capture_source(&e,"/source")== (kind==0?-ENOENT:-EOPNOTSUPP));
        CHECK(link_lookup_order()&&same_entry(&e,&before)&&no_capture_work());
        finish(&e);CHECK(balanced());
    }
    return 0;
}
static int guarded_boundaries(void){
    struct sumh_entry e;
    /* ABA is intentionally still accepted: FOLLOW can return B while the
       pathname again names original A at the final nofollow observation. */
    init(&e,S_IFREG);current_leaf=link_path;current_follow=new_path;
    replace_after_leaf=true;replace_leaf=other_link_path;restore_after_follow=true;
    path_get(&link_path);
    CHECK(sumh_entry_capture_source_checked(&e,"/source",&link_path)==0);
    CHECK(link_lookup_order()&&path_equal(&e.source_nofollow_path,&link_path)&&source_matches(&e,&new_path));
    CHECK(path_equal(&current_leaf,&link_path));finish(&e);path_put(&link_path);CHECK(balanced());

    /* An unchanged leaf does not pin dynamic-link or intermediate target
       resolution. The native FOLLOW result remains the captured source. */
    init(&e,S_IFREG);current_leaf=link_path;current_follow=new_path;
    CHECK(sumh_entry_capture_source(&e,"/source")==0);
    CHECK(link_lookup_order()&&path_equal(&e.source_nofollow_path,&link_path)&&source_matches(&e,&new_path));
    finish(&e);CHECK(balanced());

    /* A final check cannot freeze later namespace changes. No pathname retry
       occurs after the check or during metadata capture; held objects survive. */
    for(int during_metadata=0;during_metadata<2;during_metadata++){
        init(&e,S_IFREG);current_leaf=link_path;
        replace_after_postcheck=!during_metadata;replace_during_getattr=during_metadata;
        CHECK(sumh_entry_capture_source(&e,"/source")==0);
        CHECK(link_lookup_order()&&path_equal(&current_leaf,&new_path));
        CHECK(source_matches(&e,&old_path)&&path_equal(&e.source_nofollow_path,&link_path));
        finish(&e);CHECK(balanced());
    }
    return 0;
}
static int metadata(void){
    struct sumh_entry e;
    init(&e,S_IFREG);CHECK(sumh_entry_capture_source(&e,"/source")==0);
    CHECK(e.source_stat_valid&&e.visible_stat_valid&&e.preserve_visible_metadata);
    CHECK(e.source_uid==111&&e.source_gid==222&&e.source_dev==3&&e.source_size==1234);
    CHECK(e.visible_stat.mode==(S_IFREG|0751)&&e.visible_stat.uid==777&&e.visible_stat.gid==888);
    CHECK(e.visible_stat.nlink==4&&e.visible_stat.atime==401&&e.visible_stat.mtime==402&&e.visible_stat.btime==404);
    CHECK(e.visible_stat.size==1234&&e.visible_stat.blocks==8&&e.visible_stat.blksize==4096&&e.visible_stat.rdev==31);
    CHECK(e.visible_stat.ino==sumh_vnode_ino_alloc(3,11)&&e.visible_stat.dev==77);
    CHECK(e.visible_stat.ino!=old_stat.ino&&e.source_stat.ino==old_stat.ino);finish(&e);CHECK(balanced());

    init(&e,S_IFREG);source_stat_error=-EIO;
    CHECK(sumh_entry_capture_source(&e,"/source")==0);
    CHECK(e.source_stat_valid&&e.source_stat.result_mask==STATX_BASIC_STATS);
    CHECK(e.source_stat.size==1234&&e.source_stat.rdev==31&&e.source_stat.blksize==4096);
    CHECK(e.source_stat.mode==(S_IFREG|0644)&&e.source_stat.nlink==2&&e.source_stat.uid==111&&e.source_stat.gid==222);
    CHECK(e.source_stat.dev==3&&e.source_stat.ino==11&&e.source_stat.atime==0);
    CHECK(e.visible_stat.mode==(S_IFREG|0751)&&e.visible_stat.btime==404);finish(&e);CHECK(balanced());

    init(&e,S_IFREG);visible_stat_error=-EIO;
    CHECK(sumh_entry_capture_source(&e,"/source")==0);
    CHECK(e.visible_stat.mode==(S_IFREG|0751)&&e.visible_stat.uid==777&&e.visible_stat.gid==888&&e.visible_stat.nlink==4);
    CHECK(e.visible_stat.size==1234&&e.visible_stat.atime==101&&e.preserve_visible_metadata);finish(&e);CHECK(balanced());

    for(int variant=0;variant<3;variant++){
        init(&e,S_IFREG);if(variant==0)visible_error=-ENOENT;
        if(variant==1)visible_d.inode=NULL;if(variant==2)e.src=NULL;
        CHECK(sumh_entry_capture_source(&e,"/source")==0);
        CHECK(!e.preserve_visible_metadata&&e.visible_stat.mode==(S_IFREG|0644)&&e.visible_stat.uid==111);
        CHECK(e.visible_stat.size==1234&&e.visible_stat.atime==101&&e.visible_stat.ino==sumh_vnode_ino_alloc(3,11));
        finish(&e);CHECK(balanced());
    }
    init(&e,S_IFREG);current_leaf=link_path;nofollow_stat_error=-EIO;
    CHECK(sumh_entry_capture_source(&e,"/source")==0);
    CHECK(e.source_nofollow_stat_valid&&e.source_nofollow_stat.result_mask==STATX_BASIC_STATS);
    CHECK(e.source_nofollow_stat.mode==(S_IFLNK|0777)&&e.source_nofollow_stat.nlink==3);
    CHECK(e.source_nofollow_stat.uid==555&&e.source_nofollow_stat.gid==666&&e.source_nofollow_stat.size==17);
    CHECK(e.source_nofollow_stat.blocks==2&&e.source_nofollow_stat.blksize==512&&e.source_nofollow_stat.atime==0);
    CHECK(e.source_nofollow_stat.ino==sumh_vnode_ino_alloc(3,13)&&e.source_nofollow_stat.dev==77);
    finish(&e);CHECK(balanced());
    init(&e,S_IFREG);old_i.i_sb=NULL;source_stat_error=-EIO;
    CHECK(sumh_entry_capture_source(&e,"/source")==0);CHECK(e.source_dev==0&&e.source_stat.dev==0&&e.visible_ino==10011);
    finish(&e);CHECK(balanced());return 0;
}
static int types_and_errors(void){
    struct sumh_entry e;
    const umode_t types[]={S_IFREG,S_IFDIR,S_IFCHR,S_IFBLK,S_IFIFO,S_IFSOCK};
    for(int enabled=0;enabled<2;enabled++)for(int i=0;i<6;i++){
        init(&e,types[i]);sumh_device_sources_enabled=enabled;
        bool allowed=i<2||(i<5&&enabled);
        CHECK(sumh_entry_capture_source(&e,"/source")== (allowed?0:-EOPNOTSUPP));
        CHECK(nofollow_calls==1&&!follow_calls);
        if(allowed)CHECK(source_matches(&e,&old_path)&&!warn_count);
        else CHECK(!e.source_path_valid&&!allocs&&!visible_calls&&warn_count==1);
        finish(&e);CHECK(balanced());
    }
    init(&e,S_IFREG);leaf_error=-EACCES;
    CHECK(sumh_entry_capture_source(&e,"/source")==-EACCES);CHECK(nofollow_calls==1&&!path_gets&&!visible_calls);
    finish(&e);CHECK(balanced());
    init(&e,S_IFREG);old_d.inode=NULL;
    CHECK(sumh_entry_capture_source(&e,"/source")==-ENOENT);CHECK(!allocs&&!visible_calls);
    finish(&e);CHECK(balanced());
    init(&e,S_IFREG);path_get(&link_path);
    CHECK(sumh_entry_capture_resolved(&e,&link_path)==-ELOOP);CHECK(!nofollow_calls&&!follow_calls&&!visible_calls&&!allocs);
    path_put(&link_path);finish(&e);CHECK(balanced());
    init(&e,S_IFREG);path_get(&old_path);old_d.inode=NULL;
    CHECK(sumh_entry_capture_resolved(&e,&old_path)==-ENOENT);path_put(&old_path);finish(&e);CHECK(balanced());
    return 0;
}
static int invalid_and_release(void){
    struct sumh_entry e;struct path invalid={0};
    init(&e,S_IFREG);
    CHECK(sumh_entry_capture_source(NULL,"/source")==-EINVAL);
    CHECK(sumh_entry_capture_source(&e,NULL)==-EINVAL);
    CHECK(sumh_entry_capture_source(&e,"")==-EINVAL);
    CHECK(sumh_entry_capture_source_checked(&e,"/source",&invalid)==-EINVAL);
    invalid.dentry=&old_d;CHECK(sumh_entry_capture_source_checked(&e,"/source",&invalid)==-EINVAL);
    invalid.dentry=NULL;invalid.mnt=&old_m;CHECK(sumh_entry_capture_source_checked(&e,"/source",&invalid)==-EINVAL);
    CHECK(sumh_entry_capture_resolved(NULL,&old_path)==-EINVAL);
    CHECK(sumh_entry_capture_resolved(&e,NULL)==-EINVAL);
    CHECK(sumh_entry_capture_resolved(&e,&invalid)==-EINVAL);
    CHECK(!nofollow_calls&&!follow_calls&&!visible_calls&&!allocs&&!path_gets);finish(&e);CHECK(balanced());

    init(&e,S_IFREG);CHECK(sumh_entry_capture_source(&e,"/source")==0);
    int gets=path_gets,count=allocs;
    CHECK(sumh_entry_capture_source(&e,"/source")==-EALREADY);
    CHECK(sumh_entry_capture_source_checked(&e,"/source",&old_path)==-EALREADY);
    CHECK(sumh_entry_capture_resolved(&e,&old_path)==-EALREADY);
    CHECK(path_gets==gets&&allocs==count&&nofollow_calls==1&&visible_calls==1);
    finish(&e);CHECK(!e.source_path_valid&&!e.source_path.dentry&&!e.source_path.mnt&&!e.source_inode);
    sumh_entry_release_source(&e);sumh_entry_release_source(NULL);CHECK(balanced());

    init(&e,S_IFREG);current_leaf=link_path;
    CHECK(sumh_entry_capture_source(&e,"/source")==0);finish(&e);
    CHECK(!e.source_nofollow_path_valid&&!e.source_nofollow_path.dentry&&!e.source_nofollow_path.mnt);
    sumh_entry_release_source(&e);CHECK(balanced());return 0;
}
static int optional_allocations(void){
    struct sumh_entry e;
    init(&e,S_IFREG);CHECK(sumh_entry_capture_source(&e,"/source")==0);int count=allocs;
    CHECK(count==2);finish(&e);CHECK(balanced());
    for(int link=0;link<2;link++)for(int i=1;i<=count;i++){
        init(&e,S_IFREG);if(link)current_leaf=link_path;fail_at=i;
        CHECK(sumh_entry_capture_source(&e,"/source")==0);
        CHECK(source_matches(&e,&old_path)&&!e.source_canonical&&e.source_nofollow_path_valid==link);
        CHECK(allocs==i);finish(&e);CHECK(balanced());
    }
    for(int kind=1;kind<4;kind++){
        init(&e,S_IFREG);canonical_kind=kind;
        CHECK(sumh_entry_capture_source(&e,"/source")==0);
        CHECK(!e.source_canonical&&canonical_calls==1&&allocs==1);finish(&e);CHECK(balanced());
    }
    init(&e,S_IFREG);sumh_d_absolute_path=NULL;
    CHECK(sumh_entry_capture_source(&e,"/source")==0);CHECK(!e.source_canonical&&!canonical_calls&&!allocs);
    finish(&e);CHECK(balanced());return 0;
}
int main(void){
    int ret=nonlink_identity();if(ret)return ret;
    ret=checked_identity();if(ret)return ret;
    ret=native_links();if(ret)return ret;
    ret=guarded_replacements();if(ret)return ret;
    ret=guarded_errors();if(ret)return ret;
    ret=guarded_boundaries();if(ret)return ret;
    ret=metadata();if(ret)return ret;
    ret=types_and_errors();if(ret)return ret;
    ret=invalid_and_release();if(ret)return ret;
    return optional_allocations();
}
"""


def source(runtime="kernel/sumh/core/sumh_runtime.c"):
    bodies = "\n".join(function(runtime, marker) for marker in (
        "static int SUMH_NOCFI sumh_entry_capture_paths(",
        "int SUMH_NOCFI sumh_entry_capture_resolved(",
        "int SUMH_NOCFI sumh_entry_capture_source_checked(",
        "int SUMH_NOCFI sumh_entry_capture_source(",
        "void SUMH_NOCFI sumh_entry_release_source(",
    ))
    return COMMON + MODEL + bodies + TESTS


def sensitivity(compiler, original):
    """Prove behavioral coverage rejects regressions; never alter repository C."""
    guard = ("\tret = kern_path(source_path, 0, &rechecked_leaf);\n"
             "\tif (ret)\n\t\tgoto out_followed;\n"
             "\tif (!path_equal(&leaf, &rechecked_leaf))\n\t\tret = -ESTALE;\n"
             "\tpath_put(&rechecked_leaf);\n")
    capture = "\t\tret = sumh_entry_capture_paths(entry, &followed, &leaf, source_path);"
    changes = (
        ("missing-postcheck", guard, ""),
        ("dentry-only-postcheck", "!path_equal(&leaf, &rechecked_leaf)",
         "leaf.dentry != rechecked_leaf.dentry"),
        ("following-postcheck", "kern_path(source_path, 0, &rechecked_leaf)",
         "kern_path(source_path, LOOKUP_FOLLOW, &rechecked_leaf)"),
        ("swallowed-postcheck-error", "\tif (ret)\n\t\tgoto out_followed;",
         "\tif (ret) { ret = 0; goto out_followed; }"),
        ("missing-postcheck-release", "\tpath_put(&rechecked_leaf);", ""),
        ("missing-error-followed-release", "out_followed:\n\tpath_put(&followed);",
         "out_followed:\n\tif (!ret) path_put(&followed);"),
    )
    mutants = []
    for name, old, new in changes:
        if original.count(old) != 1:
            raise AssertionError(f"stale sensitivity marker: {name}")
        mutants.append((name, original.replace(old, new)))
    if original.count("\tif (!ret)\n" + capture) != 1:
        raise AssertionError("stale sensitivity marker: capture-before-postcheck")
    early = original.replace("\tif (!ret)\n" + capture, "")
    early = early.replace(guard, capture.lstrip("\t") + "\n\tif (ret)\n\t\tgoto out_followed;\n" + guard)
    mutants.append(("capture-before-postcheck", early))
    for name, mutated in mutants:
        try:
            run_test(compiler, "source-capture-" + name, mutated)
        except AssertionError as error:
            print("EXPECTED_REJECTION", name, error)
        else:
            raise AssertionError("regression escaped: " + name)
    print(f"PASS source-capture sensitivity: {len(mutants)} generated-C regressions rejected")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    parser.add_argument("--sensitivity", action="store_true", help="also reject generated-C regressions without changing production files")
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    actual = source()
    run_test(args.cc, "source-capture", actual)
    if args.sensitivity:
        sensitivity(args.cc, actual)


if __name__ == "__main__":
    main()
