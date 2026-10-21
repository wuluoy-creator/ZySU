"""Execute extracted lookup compatibility/callers with bounded kernel doubles.

Declaration models, expected compiler failures and ARM/KCFI object evidence do
not replace full KMI builds, DAC/LSM tests or execution on a target kernel.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test

VERSIONS = (1, 6, 12, 15, 16, 18)
FLAGS = ["-std=gnu11", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
         "-Werror", "-Wno-unused-function", "-Wno-unused-variable"]


def production():
    text = (ROOT / "kernel/sumh/core/sumh_runtime.c").read_text(encoding="utf-8")
    begin = text.index("/* SUMH_LOOKUP_COMPAT_BEGIN */")
    end = text.index("/* SUMH_LOOKUP_COMPAT_END */", begin)
    return text[begin:end]


def version_header(minor, wrong=None):
    header = f"#define LINUX_VERSION_CODE KERNEL_VERSION(6,{minor},0)\n"
    if minor < 16:
        legacy = "extern struct dentry *lookup_one_len(const char *, struct dentry *, int);"
        if wrong == "return":
            legacy = legacy.replace("struct dentry *lookup", "int lookup")
        if wrong == "length":
            legacy = legacy.replace(", int)", ", unsigned int)")
        if wrong == "missing":
            legacy = ""
        ns = "user_namespace" if minor == 1 else "mnt_idmap"
        header += legacy + f"\nextern struct dentry *lookup_one(struct {ns} *, const char *, struct dentry *, int);\n"
    else:
        proto = "extern struct dentry *lookup_one(struct mnt_idmap *, struct qstr *, struct dentry *);"
        if wrong == "return":
            proto = proto.replace("struct dentry *lookup", "int lookup")
        if wrong == "const":
            proto = proto.replace("struct qstr *", "const struct qstr *")
        if wrong == "old":
            proto = "extern struct dentry *lookup_one(struct mnt_idmap *, const char *, struct dentry *, int);"
        if wrong == "namespace":
            proto = proto.replace("struct mnt_idmap *", "struct user_namespace *")
        obj = "extern struct mnt_idmap nop_mnt_idmap;"
        if wrong == "object":
            obj = "extern struct mnt_idmap *nop_mnt_idmap;"
        header += proto + "\n" + obj + "\n"
    return header


PREAMBLE = r"""
#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define __nocfi
#define __init
#define EINVAL 22
#define EACCES 13
#define ENOENT 2
#define EIO 5
#define ENOMEM 12
#define EOPNOTSUPP 95
#define EROFS 30
#define ENOTDIR 20
#define EXDEV 18
#define ENOTEMPTY 39
#define ESTALE 116
#define EAGAIN 11
#define ERR_PTR(n) ((void *)(ssize_t)(n))
#define PTR_ERR(p) ((ssize_t)(p))
#define IS_ERR(p) ((ssize_t)(p)<0)
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v,n) ((v)=(n))
#define static_assert _Static_assert
#define __same_type(a,b) __builtin_types_compatible_p(__typeof__(a),__typeof__(b))
#define QSTR_INIT(n,l) { .name=(const unsigned char *)(n), .len=(l) }
struct user_namespace { int unused; };
struct mnt_idmap { unsigned long singleton_sentinel; };
struct qstr { unsigned int hash,len;const unsigned char *name; };
struct inode { unsigned int i_mode;unsigned long i_ino,i_rdev;int locked;void *i_private; };
struct dentry { struct inode *inode;struct qstr d_name;int refs; };
struct vfsmount { int writes; };
struct path { struct dentry *dentry;struct vfsmount *mnt; };
struct file { struct path f_path; };
static int diagnostics,bad,helper_calls,lock_calls,unlock_calls,put_calls,forbidden_calls;
#define pr_warn(...) ((void)diagnostics++)
#define pr_info(...) ((void)0)
#define sumh_log(...) ((void)diagnostics++)
extern struct dentry *(*sumh_lookup_one_len)(const char *,struct dentry *,int);
static bool initializing;
struct symbol { const char *name;unsigned long address; };
static struct symbol symbols[24];
static int symbol_count,match_calls,fallback_calls;
static const char *requests[64];static int request_count;
static unsigned long symbol_address(const char *name) {
    for(int i=0;i<symbol_count;i++)if(!strcmp(name,symbols[i].name))return symbols[i].address;
    return 0;
}
static void request(const char *name) {
    if(initializing&&sumh_lookup_one_len)bad++;
    if(request_count<64)requests[request_count++]=name;else bad++;
}
static unsigned long kallsyms_lookup_name(const char *name) { fallback_calls++;request(name);return symbol_address(name); }
static int match_enabled=1;
static int each_match(int (*callback)(void *,unsigned long),const char *name,void *data) {
    match_calls++;request(name);if(!match_enabled)return 0;
    unsigned long address=symbol_address(name);if(address)return callback(data,address);return 0;
}
static int (*kallsyms_on_each_match_symbol_fn)(int (*)(void *,unsigned long),const char *,void *)=each_match;
static void *ksu_lookup_symbol(const char *name) { (void)name;forbidden_calls++;return NULL; }
static void add_symbol(const char *name,unsigned long address) { symbols[symbol_count++]=(struct symbol){name,address}; }
"""


def resolver():
    path = "kernel/infra/symbol_resolver.c"
    return function(path, "static int find_kernel_symbol_exact_cb(") + "\n" + function(path, "unsigned long __nocfi find_kernel_symbol_exact(")


MODEL = r"""
static struct mnt_idmap identity={0x12345};
static struct inode child_inode={.i_mode=0020000,.i_ino=70};
static struct dentry positive={.inode=&child_inode},negative;
static struct dentry *returns[4];
static const char *seen_name[4];static struct dentry *seen_parent[4];static int seen_len[4];
static struct mnt_idmap *seen_idmap[4];static int expected_locked=-1;
static struct dentry *record_helper(const char *name,struct dentry *parent,int length,struct mnt_idmap *idmap) {
    int i=helper_calls++;if(i>=4){bad++;return ERR_PTR(-EIO);}
    seen_name[i]=name;seen_parent[i]=parent;seen_len[i]=length;seen_idmap[i]=idmap;
    if(expected_locked>=0&&(!parent->inode||parent->inode->locked!=expected_locked))bad++;
    struct dentry *ret=returns[i];if(!ret){bad++;return ERR_PTR(-EIO);}if(!IS_ERR(ret))ret->refs++;
    return ret;
}
static struct dentry *legacy_helper(const char *name,struct dentry *base,int len) { return record_helper(name,base,len,NULL); }
static struct dentry *qstr_helper(struct mnt_idmap *idmap,struct qstr *q,struct dentry *base) {
    if(q->hash)bad++;q->hash=0x314159;
    return record_helper((const char *)q->name,base,(int)q->len,idmap);
}
static void reset_model(void) {
    bad=diagnostics=helper_calls=lock_calls=unlock_calls=put_calls=forbidden_calls=0;
    expected_locked=-1;request_count=match_calls=fallback_calls=0;symbol_count=0;
    kallsyms_on_each_match_symbol_fn=each_match;match_enabled=1;
    memset(returns,0,sizeof(returns));memset(seen_name,0,sizeof(seen_name));memset(seen_parent,0,sizeof(seen_parent));
    positive.refs=negative.refs=0;positive.inode=&child_inode;negative.inode=NULL;
    child_inode=(struct inode){.i_mode=0020000,.i_ino=70};
}
static void install(int available) {
    symbol_count=0;
    add_symbol("lookup_one_len.isra.0",0x111);add_symbol("lookup_one_len$clone",0x112);
    add_symbol("lookup_one.constprop.0",0x113);add_symbol("nop_mnt_idmap.isra.0",0x114);
    add_symbol("lookup_noperm",0x115);add_symbol("lookup_one_unlocked",0x116);
    add_symbol("lookup_one$clone",0x120);add_symbol("nop_mnt_idmap$clone",0x121);
    add_symbol("lookup_one_len_unlocked",0x122);add_symbol("try_lookup_noperm",0x123);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,16,0)
    add_symbol("lookup_one",0x117);add_symbol("nop_mnt_idmap",0x118);
    if(available&1)add_symbol("lookup_one_len",(unsigned long)legacy_helper);
#else
    add_symbol("lookup_one_len",0x119);
    if(available&1)add_symbol("lookup_one",(unsigned long)qstr_helper);
    if(available&2)add_symbol("nop_mnt_idmap",(unsigned long)&identity);
#endif
    request_count=0;initializing=true;sumh_resolve_lookup_helpers();initializing=false;
}
static bool expected_requests(void) {
    for(int i=0;i<request_count;i++) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,16,0)
        if(strcmp(requests[i],"lookup_one_len"))return false;
#else
        if(strcmp(requests[i],"lookup_one")&&strcmp(requests[i],"nop_mnt_idmap"))return false;
#endif
    }
    return request_count>0&&!forbidden_calls&&!bad;
}
"""

ADAPTER_TESTS = r"""
int main(void) {
    reset_model();install(3);CHECK(sumh_lookup_one_len&&expected_requests());
    for(int mask=0;mask<4;mask++){int warnings=diagnostics;install(mask);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,16,0)
        CHECK(!!sumh_lookup_one_len==!!(mask&1));
        CHECK(diagnostics-warnings==!(mask&1));
#else
        CHECK(!!sumh_lookup_one_len==(mask==3));
        CHECK(diagnostics-warnings==(mask!=3));
        if(mask!=3){CHECK(sumh_lookup_one_len_compat("x",NULL,1)==ERR_PTR(-EOPNOTSUPP)&&!helper_calls);}
#endif
        CHECK(expected_requests());install(3);CHECK(sumh_lookup_one_len&&expected_requests());}
    char bytes[256];memset(bytes,'x',sizeof(bytes));bytes[1]=0;bytes[2]=(char)0xff;bytes[3]=(char)0xe7;
    const int lengths[]={1,255,4,0,-1};
    struct inode parent_inode={.i_mode=0040000};struct dentry parent={.inode=&parent_inode};
    for(int i=0;i<5;i++)for(int error=0;error<6;error++){
        reset_model();install(3);struct dentry *expected=error==0?&positive:error==1?&negative:ERR_PTR(-(error==2?EACCES:error==3?ENOENT:error==4?EIO:ENOMEM));returns[0]=expected;
        struct dentry *result=sumh_lookup_one_len(bytes,&parent,lengths[i]);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,16,0)
        if(lengths[i]<0){CHECK(result==ERR_PTR(-EINVAL)&&!helper_calls);continue;}
#endif
        CHECK(result==expected&&helper_calls==1&&seen_name[0]==bytes&&seen_parent[0]==&parent&&seen_len[0]==lengths[i]);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,16,0)
        CHECK(seen_idmap[0]==&identity);
#else
        CHECK(!seen_idmap[0]);
#endif
        CHECK(!bad&&!lock_calls&&!unlock_calls&&!put_calls&&!forbidden_calls&&bytes[1]==0&&(unsigned char)bytes[2]==255);
        CHECK(positive.refs==(error==0)&&negative.refs==(error==1));
    }
    return 0;
}
"""


def adapter_source(minor, wrong=None, tests=True):
    pre = COMMON + PREAMBLE + version_header(minor, wrong) + resolver() + production()
    return pre + (MODEL + ADAPTER_TESTS if tests else "")


def resolver_source():
    tests = r"""
struct dentry *(*sumh_lookup_one_len)(const char *,struct dentry *,int);
int main(void) {
    add_symbol("wanted.isra.0",111);add_symbol("wanted$clone",112);
    CHECK(!find_kernel_symbol_exact(NULL)&&!find_kernel_symbol_exact("")&&!match_calls&&!fallback_calls);
    CHECK(!find_kernel_symbol_exact("wanted")&&match_calls==1&&fallback_calls==1);
    add_symbol("wanted",222);CHECK(find_kernel_symbol_exact("wanted")==222&&match_calls==2&&fallback_calls==1);
    match_enabled=0;CHECK(find_kernel_symbol_exact("wanted")==222&&match_calls==3&&fallback_calls==2);
    kallsyms_on_each_match_symbol_fn=NULL;CHECK(find_kernel_symbol_exact("wanted")==222&&match_calls==3&&fallback_calls==3);
    CHECK(!find_kernel_symbol_exact("absent")&&!forbidden_calls&&!bad);
    return 0;
}
"""
    return COMMON + PREAMBLE + resolver() + tests


CALLER_MODEL = r"""
#define S_ISDIR(m) (((m)&0170000)==0040000)
#define S_ISCHR(m) (((m)&0170000)==0020000)
#define I_MUTEX_PARENT2 3
#define THIS_MODULE NULL
#define KVN_IDMAP_ARG
typedef unsigned long long u64;
struct sumh_vnode_info { struct path source; };
static struct inode *d_inode(const struct dentry *d) { return d?d->inode:NULL; }
static struct inode *file_inode(const struct file *f) { return d_inode(f->f_path.dentry); }
static int busy,module_refs,write_gets,write_puts,rename_calls,rename_error;
static struct dentry *trap;
static bool inode_trylock(struct inode *i) { lock_calls++;if(busy)return false;if(i->locked)bad++;i->locked=1;return true; }
static void inode_lock_nested(struct inode *i,int subclass) { (void)subclass;lock_calls++;if(i->locked)bad++;i->locked=1; }
static void inode_unlock(struct inode *i) { unlock_calls++;if(!i->locked)bad++;i->locked=0; }
static void dput(struct dentry *d) { if(d){put_calls++;if(d->refs<=0)bad++;else d->refs--;} }
static bool d_really_is_negative(struct dentry *d) { return !d->inode; }
static bool try_module_get(void *m) { (void)m;module_refs++;return true; }
static void module_put(void *m) { (void)m;module_refs--; }
static int sumh_vnode_src_want_write(const struct path *p) { p->mnt->writes++;write_gets++;return 0; }
static void sumh_vnode_src_drop_write(const struct path *p) { p->mnt->writes--;write_puts++; }
static struct dentry *sumh_lock_src_rename(struct dentry *new,struct dentry *old) {
    inode_lock_nested(old->inode,0);if(new!=old)inode_lock_nested(new->inode,0);return trap;
}
static void sumh_unlock_src_rename(struct dentry *new,struct dentry *old) {
    inode_unlock(old->inode);if(new!=old)inode_unlock(new->inode);
}
static int (*sumh_vfs_rename)(void *);
static int rename_stub(void *p) { (void)p;return 0; }
static int sumh_vnode_do_src_rename(struct sumh_vnode_info *o,struct sumh_vnode_info *n,
    struct inode *oi,struct dentry *old,struct inode *ni,struct dentry *new,unsigned int flags) {
    (void)flags;rename_calls++;if(oi!=o->source.dentry->inode||ni!=n->source.dentry->inode||!oi->locked||!ni->locked||old!=&positive||new!=&negative)bad++;
    return rename_error;
}
static void reset_callers(void) { reset_model();install(3);busy=module_refs=write_gets=write_puts=rename_calls=rename_error=0;trap=NULL;sumh_vfs_rename=rename_stub;expected_locked=1; }
"""

CALLER_TESTS = r"""
static int merge_tests(void) {
    struct inode parent_inode={.i_mode=0040000};struct dentry parent={.inode=&parent_inode};struct file f={.f_path.dentry=&parent};
    unsigned char storage[sizeof(struct sumh_merge_raw_name)+8] __attribute__((aligned(__alignof__(struct sumh_merge_raw_name))));
    memset(storage,0,sizeof(storage));struct sumh_merge_raw_name *raw=(void *)storage;raw->len=4;raw->ino=70;memcpy(raw->name,"char",4);
    for(int mode=0;mode<9;mode++) {reset_callers();returns[0]=&positive;bool whiteout=false;raw->projected=false;
        if(mode==1)child_inode.i_rdev=1;if(mode==2)returns[0]=&negative;if(mode==3)child_inode.i_ino++;
        if(mode==4)child_inode.i_mode=0100000;if(mode==5)returns[0]=ERR_PTR(-EACCES);if(mode==6)install(0);if(mode==7)busy=1;if(mode==8)raw->projected=true;
        int expected=mode<=1?0:mode<=4?-ESTALE:mode==5?-EACCES:mode==7?-EAGAIN:-EOPNOTSUPP;
        CHECK(sumh_merge_classify_char(&f,raw,&whiteout)==expected&&whiteout==(mode==0));
        CHECK(!parent_inode.locked&&!positive.refs&&!negative.refs&&!bad);
        if(mode<6)CHECK(helper_calls==1&&seen_parent[0]==&parent&&seen_name[0]==raw->name&&seen_len[0]==4&&unlock_calls==1);
        else CHECK(!helper_calls&&!put_calls);
    }
    return 0;
}
static int vnode_child_tests(void) {
    struct inode parent_inode={.i_mode=0040000};struct dentry parent={.inode=&parent_inode},vis={.d_name=QSTR_INIT("child",5)};struct vfsmount mount={0};
    for(int mode=0;mode<7;mode++){reset_callers();struct sumh_vnode_info info={.source={&parent,&mount}};struct inode *out=(void *)0x1234;
        parent_inode.i_mode=0040000;returns[0]=mode==1?&negative:&positive;
        if(mode==2)returns[0]=ERR_PTR(-EACCES);if(mode==3)returns[0]=ERR_PTR(-ENOMEM);if(mode==4)install(0);if(mode==5)info.source.mnt=NULL;if(mode==6)parent_inode.i_mode=0100000;
        struct dentry *result=sumh_vnode_src_child(&info,&vis,&out);
        if(mode<2){CHECK(result==returns[0]&&out==&parent_inode&&parent_inode.locked&&result->refs==1);inode_unlock(out);dput(result);}
        else {int error=mode==2?EACCES:mode==3?ENOMEM:mode==4?EOPNOTSUPP:mode==5?EROFS:ENOTDIR;CHECK(result==ERR_PTR(-error)&&out==(void *)0x1234);}
        CHECK(!parent_inode.locked&&!positive.refs&&!negative.refs&&!bad);
        if(mode<4)CHECK(helper_calls==1&&seen_name[0]==(const char *)vis.d_name.name&&seen_parent[0]==&parent&&seen_len[0]==5);
        else CHECK(!helper_calls&&!lock_calls);
    }
    return 0;
}
static int vnode_rename_tests(void) {
    for(int separate=0;separate<2;separate++)for(int mode=0;mode<9;mode++){
        reset_callers();struct inode oi={.i_mode=0040000},ni={.i_mode=0040000};struct dentry op={.inode=&oi},np={.inode=&ni};struct vfsmount om={0},nm={0};
        struct sumh_vnode_info o={.source={&op,&om}},n={.source={&np,separate?&nm:&om}};
        struct inode visible_old={.i_private=&o},visible_new={.i_private=&n};struct dentry old={.d_name=QSTR_INIT("old",3)},new={.d_name=QSTR_INIT("new-name",8)};
        returns[0]=&positive;returns[1]=&negative;
        if(mode==1)install(0);if(mode==2)returns[0]=ERR_PTR(-EACCES);if(mode==3)returns[1]=ERR_PTR(-ENOMEM);
        if(mode==4)returns[0]=&negative;if(mode==5)trap=&positive;if(mode==6)trap=&negative;if(mode==7)rename_error=-EIO;
        if(mode==8)negative.inode=&child_inode;
        int expected=mode==1?-EOPNOTSUPP:mode==2?-EACCES:mode==3?-ENOMEM:mode==4?-ENOENT:mode==5?-EINVAL:mode==6?-ENOTEMPTY:mode==7?-EIO:0;
        CHECK(sumh_vnode_dir_rename(&visible_old,&old,&visible_new,&new,0)==expected);
        CHECK(!oi.locked&&!ni.locked&&!positive.refs&&!negative.refs&&!module_refs&&!om.writes&&!nm.writes&&write_gets==write_puts&&!bad);
        if(mode==1)CHECK(!helper_calls&&!lock_calls&&!write_gets&&!rename_calls);
        else {CHECK(helper_calls==(mode==2?1:2));CHECK(seen_parent[0]==&op&&seen_name[0]==(const char *)old.d_name.name&&seen_len[0]==3);
            if(helper_calls==2)CHECK(seen_parent[1]==&np&&seen_name[1]==(const char *)new.d_name.name&&seen_len[1]==8);
            CHECK(write_gets==1+separate&&rename_calls==(mode==0||mode>=7));}
    }
    return 0;
}
int main(void) { int r=merge_tests();if(r)return r;r=vnode_child_tests();if(r)return r;return vnode_rename_tests(); }
"""


def caller_source(minor):
    raw = function("kernel/sumh/overlay/sumh_overlay.c", "struct sumh_merge_raw_name {") + ";\n"
    # Production raw-name layout includes list_head and types; no collector is mocked here.
    types = "typedef unsigned int u32;typedef unsigned char u8;struct list_head { struct list_head *next,*prev; };\n"
    overlay = function("kernel/sumh/overlay/sumh_overlay.c", "static int sumh_merge_classify_char(")
    vnode = "kernel/sumh/core/sumh_vnode.c"
    child = function(vnode, "static SUMH_NOCFI struct dentry *\nsumh_vnode_src_child(")
    rename = function(vnode, "static int SUMH_NOCFI sumh_vnode_dir_rename(")
    # Existing vnode calls pass kernel qstr unsigned-byte pointers to the
    # legacy char-pointer API. Keep their expressions and the real qstr type;
    # scope the signedness accommodation to those unchanged callers only.
    vnode_calls = '\n#pragma clang diagnostic push\n#pragma clang diagnostic ignored "-Wpointer-sign"\n' + child + rename + '\n#pragma clang diagnostic pop\n'
    return adapter_source(minor, tests=False) + MODEL + CALLER_MODEL + types + raw + overlay + vnode_calls + CALLER_TESTS


def snapshot_source(minor):
    import test_sumh_merge_snapshot as snapshot
    bridge = r"""
#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define pr_warn(...) ((void)0)
struct mnt_idmap { int marker; };
struct user_namespace { int marker; };
static struct mnt_idmap identity={123};
static int adapter_available=3,adapter_calls;
static struct dentry *adapter_legacy(const char *name,struct dentry *base,int len) { adapter_calls++;return lookup_child(name,base,len); }
static struct dentry *adapter_qstr(struct mnt_idmap *idmap,struct qstr *q,struct dentry *base) {
    adapter_calls++;if(idmap!=&identity||q->hash)bad_context++;q->hash=456;
    return lookup_child((const char *)q->name,base,(int)q->len);
}
static unsigned long find_kernel_symbol_exact(const char *name) {
    if(!strcmp(name,"lookup_one_len"))return adapter_available&1?(unsigned long)adapter_legacy:0;
    if(!strcmp(name,"lookup_one"))return adapter_available&1?(unsigned long)adapter_qstr:0;
    if(!strcmp(name,"nop_mnt_idmap"))return adapter_available&2?(unsigned long)&identity:0;
    bad_context++;return 0;
}
"""
    fixture = snapshot.fixture()
    binding = "static struct dentry *(*sumh_lookup_one_len)(const char *,struct dentry *,int)=lookup_child;"
    assert fixture.count(binding) == 1
    fixture = fixture.replace(binding, bridge + version_header(minor) + production())
    fixture = fixture.replace("struct qstr { const char *name; int len; };", "struct qstr { unsigned int hash,len;const unsigned char *name; };")
    fixture = fixture.replace("#define QSTR_INIT(n,l) {n,l}", "#define QSTR_INIT(n,l) {.name=(const unsigned char *)(n),.len=(l)}")
    fixture = fixture.replace("{name,length},1,-1", "QSTR_INIT(name,length),1,-1")
    fixture = fixture.replace('{"target",6},1,i', 'QSTR_INIT("target",6),1,i')
    fixture = fixture.replace("sumh_lookup_one_len=lookup_child;", "adapter_available=3;adapter_calls=0;sumh_resolve_lookup_helpers();")
    tests = r"""
int main(void) {
    struct sumh_merge_snapshot *s=NULL;
    setup(2);record(1,"char",20,DT_CHR,false);record(2,"char",21,DT_REG,false);
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s)&&adapter_calls==1&&lookup_calls==1);
    CHECK(sumh_merge_snapshot_covers(s,"char",4));sumh_merge_snapshot_destroy(s);CHECK(clean());
    setup(2);record(1,"first",10,DT_REG,false);record(2,"denied",20,DT_CHR,false);lookup_error=EACCES;
    CHECK(sumh_merge_snapshot_create(&source_file,"/visible",&s)==-EACCES&&!s&&adapter_calls==1&&clean());
    setup(1);record(1,"char",20,DT_CHR,false);adapter_available=0;sumh_resolve_lookup_helpers();
    CHECK(!sumh_lookup_one_len&&sumh_merge_snapshot_create(&source_file,"/visible",&s)==-EOPNOTSUPP&&!s&&!adapter_calls&&!trylock_calls&&clean());
    setup(1);record(1,".replace",20,DT_CHR,true);adapter_available=0;sumh_resolve_lookup_helpers();
    CHECK(!sumh_merge_snapshot_create(&source_file,"/visible",&s)&&!adapter_calls&&!trylock_calls);
    CHECK(sumh_merge_snapshot_covers(s,".replace",8));sumh_merge_snapshot_destroy(s);CHECK(clean());
    return 0;
}
"""
    return fixture + tests


def host_source(source):
    # Linux unsigned long holds addresses; Windows uses LLP64. ARM is unmodified.
    return re.sub(r"unsigned long(?! long)", "__UINTPTR_TYPE__", source) if os.name == "nt" else source


def compile_guards(compiler, evidence=None):
    with tempfile.TemporaryDirectory(prefix="sumh-lookup-guards-") as temporary:
        directory = Path(temporary)
        cases = ((12, "return"), (12, "length"), (12, "missing"), (18, "return"),
                 (18, "const"), (18, "old"), (18, "namespace"), (18, "object"))
        for minor, wrong in cases:
            source = directory / f"6-{minor}-{wrong}.c"
            source.write_text(adapter_source(minor, wrong, tests=False), encoding="utf-8")
            result = subprocess.run([compiler, "--target=aarch64-linux-gnu", *FLAGS, "-fsyntax-only", str(source)], capture_output=True, text=True)
            if evidence:
                evidence.mkdir(parents=True, exist_ok=True)
                (evidence / source.name).write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
                (evidence / (source.stem + ".stderr")).write_text(result.stderr, encoding="utf-8")
            assert result.returncode, f"incompatible 6.{minor}/{wrong} unexpectedly accepted"
            expected = "undeclared identifier 'lookup_one_len'" if wrong == "missing" else "static assertion failed"
            assert expected in result.stderr, result.stderr
            if wrong != "missing":
                guard = "SUMH requires nop_mnt_idmap" if wrong == "object" else (
                    "SUMH requires the lookup_one_len" if minor < 16 else "SUMH requires the lookup_one")
                assert guard in result.stderr, result.stderr
            print(f"PASS declaration rejection 6.{minor} {wrong}: {expected}", flush=True)


def check_bootstrap():
    body = function("kernel/sumh/core/sumh_bootstrap.c", "static void sumh_resolve_runtime_symbols(")
    assert body.count("sumh_resolve_lookup_helpers();") == 1
    assert not re.search(r"sumh_lookup_one_len\s*=", body)
    print("PASS bootstrap initializer wiring (source check)", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    parser.add_argument("--evidence-dir", type=Path, help="preserve expected declaration-rejection source/stderr")
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler, "lookup-exact-resolver", host_source(resolver_source()))
    for minor in VERSIONS:
        run_test(compiler, f"lookup-adapter-6.{minor}", host_source(adapter_source(minor)))
    for minor in (1, 6, 12, 18):
        run_test(compiler, f"lookup-callers-6.{minor}", host_source(caller_source(minor)))
    for minor in (12, 18):
        run_test(compiler, f"lookup-snapshot-6.{minor}", host_source(snapshot_source(minor)))
    compile_guards(compiler, args.evidence_dir)
    check_bootstrap()
