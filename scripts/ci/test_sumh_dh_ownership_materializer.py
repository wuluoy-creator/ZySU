"""Actual recursive materializer and its SHADOW/injection joint producer."""

import re
from test_sumh_dh_ownership_source import ROOT, core, FIXTURES, read, function
from test_sumh_dh_ownership_cases import HELPERS
from test_sumh_dh_ownership_producers import indexed_bodies, dispatch_case, OVERLAY

INPUTS = (OVERLAY, "kernel/sumh/internal/sumh_base.h")

SERVICES = r"""
#define E2BIG 7
#define O_RDONLY 0
#define O_DIRECTORY 1
#define DT_UNKNOWN 0
#define DT_CHR 2
#define DT_BLK 6
#define DT_FIFO 1
#define DT_SOCK 12
static void *memchr(const void *s,int c,size_t n){const unsigned char *p=s;for(size_t i=0;i<n;i++)if(p[i]==c)return (void *)(p+i);return NULL;}
static char *kasprintf(int flags,const char *fmt,const char *p,int n,const char *s){
    if(strcmp(fmt,"%s/%.*s")||n<0){BAD();return NULL;}
    size_t len=strlen(p);char *r=kmalloc(len+n+2,flags);if(r){memcpy(r,p,len);r[len]='/';memcpy(r+len+1,s,n);r[len+n+1]=0;}return r;
}
static int model_iter_depth;
struct native_dir_item {const char *name;struct path child;};
struct native_dir {struct path path;struct native_dir_item items[16];int count;};
static struct native_dir native_dirs[16];static int native_dir_count;
static struct dentry marker_negative={.refs=1};
static struct native_dir *directory(const struct path *p){for(int i=0;i<native_dir_count;i++)if(path_equal(p,&native_dirs[i].path))return &native_dirs[i];return NULL;}
static struct dentry *lookup_component(const char *n,struct dentry *p,int len){
    if(len==8&&!memcmp(n,".replace",8))return dget(&marker_negative);
    for(int i=0;i<native_dir_count;i++)if(native_dirs[i].path.dentry==p)for(int j=0;j<native_dirs[i].count;j++)if(strlen(native_dirs[i].items[j].name)==(size_t)len&&!memcmp(n,native_dirs[i].items[j].name,len))return dget(native_dirs[i].items[j].child.dentry);
    return ERR_PTR(-ENOENT);
}
static struct dentry *(*sumh_lookup_one_len)(const char *,struct dentry *,int)=lookup_component;
static void (*after_materialized_capture)(const char *,struct dentry *,const struct path *);
static int vfs_path_lookup(struct dentry *p,struct vfsmount *m,const char *n,unsigned flags,struct path *out){
    if(flags)BAD();struct dentry *d=lookup_component(n,p,(int)strlen(n));if(IS_ERR(d))return PTR_ERR(d);*out=(struct path){m,d};if(m)m->refs++;if(after_materialized_capture)after_materialized_capture(n,p,out);return 0;
}
static const int native_creds;
static const int *current_cred(void){return &native_creds;}
static struct file *dentry_open(const struct path *p,int flags,const int *cred){(void)flags;(void)cred;struct file *f=kzalloc(sizeof(*f),GFP_KERNEL);if(!f)return ERR_PTR(-ENOMEM);f->f_path=*p;path_get(p);return f;}
static void fput(struct file *f){path_put(&f->f_path);kfree(f);}
static int iterate_dir(struct file *f,struct dir_context *ctx){
    struct native_dir *d=directory(&f->f_path);if(!d)return -ENOENT;
    model_iter_depth++;ctx->pos=f->f_pos;
    while(ctx->pos<d->count){struct native_dir_item *i=&d->items[ctx->pos];unsigned type=(d_inode(i->child.dentry)->i_mode&S_IFMT)>>12;if(!ctx->actor(ctx,i->name,(int)strlen(i->name),ctx->pos,d_inode(i->child.dentry)->i_ino,type))break;ctx->pos++;}
    f->f_pos=ctx->pos;model_iter_depth--;return 0;
}
"""


def bodies(root=ROOT):
    text = read(root, OVERLAY)
    start = text.index("/* Materialize merge targets into native path rules")
    constants = "\n".join(re.findall(r"^#define SUMH_MERGE_(?:MAX_TARGETS|MAX_NAMES|MAX_RECORDS|MAX_PASSES|MEMORY_BUDGET) .+$", read(root, INPUTS[1]), re.M))
    return SERVICES + constants + "\n" + function(text, "static bool sumh_merge_valid_name(") + text[start:]


CASES = r"""
static struct sumh_entry *mat_stored(const char *name){struct sumh_entry *e;unsigned b;hash_for_each(sumh_paths,b,e,node)if(!strcmp(e->src,name))return e;return NULL;}
static void setup_tree(void){
    native_dir_count=2;memset(native_dirs,0,sizeof(native_dirs));
    native_dirs[0].path=fixture_path(4);native_dirs[0].items[0]=(struct native_dir_item){"first",fixture_path(16)};native_dirs[0].items[1]=(struct native_dir_item){"nested",fixture_path(5)};native_dirs[0].count=2;
    native_dirs[1].path=fixture_path(5);native_dirs[1].items[0]=(struct native_dir_item){"second",fixture_path(17)};native_dirs[1].count=1;
    native_path("/module",fixture_path(4),0);native_path("/module/nested",fixture_path(5),0);native_path("/one/nested",fixture_path(3),0);
    fixture_dentries[4].canonical="/module";fixture_dentries[5].canonical="/module/nested";fixture_dentries[3].canonical="/one/nested";
}
static int materializer_case(void){
    fixture_init();setup_tree();
    CHECK(merge_rule("/one","/module")==0);
    struct sumh_entry *a=mat_stored("/one/first"),*b=mat_stored("/one/nested/second");
    CHECK(a&&b&&a->dh_claim->kind==SUMH_DH_OWNER_SHADOW&&b->dh_claim->kind==SUMH_DH_OWNER_SHADOW);
    CHECK(a->dh_claim->slot->dir->dir_inode==&fixture_inodes[1]&&a->inject_claim->binding->inode==&fixture_inodes[1]);
    CHECK(b->dh_claim->slot->dir->dir_inode==&fixture_inodes[3]&&b->inject_claim->binding->inode==&fixture_inodes[3]);
    CHECK(a->source_path.dentry==&fixture_dentries[16]&&b->source_path.dentry==&fixture_dentries[17]);
    CHECK(a->dh_claim->payload->lookup_only&&b->dh_claim->payload->lookup_only);
    CHECK(clear_rules()==0);CHECK(!model_inode_locks&&!model_iter_depth&&marker_negative.refs==1);
    return finish_case();
}
static int mat_parent_observations;
static void mat_rebind_parent(const char *name,unsigned flags,int result,const struct path *path){
    (void)flags;(void)path;if(!strcmp(name,"/one")&&!result){mat_parent_observations++;native_path("/one",fixture_path(2),0);after_named_path=NULL;}
}
static int mat_transactions(void){
    fixture_init();setup_tree();struct path leaf=fixture_path(16);
    after_named_path=mat_rebind_parent;mat_parent_observations=0;
    CHECK(sumh_add_path_entry("/one","/one/first","/module/first",DT_REG,&leaf)==0);after_named_path=NULL;
    struct sumh_entry *e=mat_stored("/one/first");CHECK(e&&mat_parent_observations==1&&e->dh_claim->slot->dir->dir_inode==&fixture_inodes[1]&&e->inject_claim->binding->inode==&fixture_inodes[1]);
    int count=sumh_rule_count;native_path("/one",fixture_path(2),-EIO);leaf=fixture_path(17);
    CHECK(sumh_add_path_entry("/one","/one/first","/module/second",DT_REG,&leaf)==-EIO&&mat_stored("/one/first")==e&&sumh_rule_count==count);
    native_path("/one",fixture_path(2),-ENOENT);
    CHECK(sumh_add_path_entry("/one","/one/first","/module/second",DT_REG,&leaf)==-ENOENT&&mat_stored("/one/first")==e&&sumh_rule_count==count);
    native_path("/one",fixture_path(1),0);native_path("/one/.",fixture_path(1),0);
    CHECK(sumh_add_path_entry("/one/.","/one/./second","/module/second",DT_REG,&leaf)==0&&mat_stored("/one/./second"));
    native_path("/one/./second",fixture_path(18),-EACCES);
    CHECK(del_rule("/one/./second",NULL)==0&&!mat_stored("/one/./second"));
    CHECK(clear_rules()==0);return finish_case();
}
static int mat_partial_success(void){
    fixture_init();setup_tree();native_path("/one/nested",fixture_path(3),-EACCES);
    CHECK(merge_rule("/one","/module")==-EACCES);
    CHECK(mat_stored("/one/first")&&!mat_stored("/one/nested/second")&&sumh_rule_count>0);
    CHECK(clear_rules()==0);return finish_case();
}
static int mat_nested_capture_observations,mat_nested_prepare_failures;
static void mat_failed_required_parent(const char *name,unsigned flags,int result,const struct path *path){
    (void)path;
    if(!strcmp(name,"/one/nested")&&result==-EIO&&(flags&LOOKUP_DIRECTORY))mat_nested_prepare_failures++;
}
static void mat_fail_after_nested_capture(const char *name,struct dentry *parent,const struct path *leaf){
    if(!strcmp(name,"second")&&parent==&fixture_dentries[5]&&leaf->dentry==&fixture_dentries[18]){
        mat_nested_capture_observations++;
        native_path("/one/nested",fixture_path(3),-EIO);
        after_named_path=mat_failed_required_parent;
    }
}
static int mat_actual_caller_replacement(void){
    fixture_init();setup_tree();struct path target=fixture_path(4);
    CHECK(merge_rule("/one","/module")==0);
    struct sumh_entry *first=mat_stored("/one/first"),*second=mat_stored("/one/nested/second");
    CHECK(first&&second&&first->dh_claim->slot->dir->dir_inode==first->inject_claim->binding->inode);
    /* Reapply through the actual bounded collector and install-child caller.
       A replaced nested source is captured, then its required parent fails. */
    struct sumh_dh_claim *old_dh=second->dh_claim;struct sumh_inject_claim *old_inject=second->inject_claim;
    native_dirs[1].items[0].child=fixture_path(18);int count=sumh_rule_count;
    mat_nested_capture_observations=mat_nested_prepare_failures=0;after_materialized_capture=mat_fail_after_nested_capture;
    after_named_path=mat_rebind_parent;mat_parent_observations=0;
    CHECK(sumh_materialize_merge("/one","/module",&target,0)==-EIO);after_named_path=NULL;after_materialized_capture=NULL;
    CHECK(mat_nested_capture_observations==1&&mat_nested_prepare_failures==1);
    struct sumh_entry *replacement=mat_stored("/one/first");
    CHECK(mat_parent_observations==1&&replacement->dh_claim->slot->dir->dir_inode==&fixture_inodes[1]&&replacement->inject_claim->binding->inode==&fixture_inodes[1]);
    CHECK(mat_stored("/one/first")!=first&&mat_stored("/one/nested/second")==second&&second->dh_claim==old_dh&&second->inject_claim==old_inject&&old_dh->state==DH_ATTACHED&&old_inject->attached&&second->source_path.dentry==&fixture_dentries[17]&&sumh_rule_count==count);
    CHECK(clear_rules()==0);return finish_case();
}
int main(void){int r;if((r=materializer_case()))return r;if((r=mat_transactions()))return r;if((r=mat_partial_success()))return r;if((r=mat_actual_caller_replacement()))return r;return 0;}
"""


def source(root=ROOT):
    # The top MERGE dispatch needs the materializer's real public prototypes.
    return core(root) + indexed_bodies(root) + bodies(root) + dispatch_case(root, "SUMH_IOC_ADD_MERGE_RULE", "merge_rule") + FIXTURES + HELPERS + CASES
