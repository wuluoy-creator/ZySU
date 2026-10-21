#!/usr/bin/env python3
"""Execute structured SUMH inventory query and outer control wrapper.

Production functions run against bounded list/hash/allocation/usercopy doubles;
ARM32/ARM64 ABI compilation is not a kernel/device/compat execution claim.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test
from test_sumh_runtime_state import TYPES, uapi

IOCTL = "kernel/sumh/control/sumh_ioctl.c"


def abi_source():
    return TYPES + uapi() + r"""
_Static_assert(sizeof(struct sumh_rule_inventory_entry)==8272,"entry size");
_Static_assert(sizeof(struct sumh_rule_inventory_arg)==8352,"page size");
_Static_assert(__alignof__(struct sumh_rule_inventory_entry)==8,"entry alignment");
_Static_assert(__alignof__(struct sumh_rule_inventory_arg)==8,"page alignment");
_Static_assert(__builtin_offsetof(struct sumh_rule_inventory_entry,id)==16,"id offset");
_Static_assert(__builtin_offsetof(struct sumh_rule_inventory_entry,path1)==80,"path1 offset");
_Static_assert(__builtin_offsetof(struct sumh_rule_inventory_entry,path2)==4176,"path2 offset");
_Static_assert(__builtin_offsetof(struct sumh_rule_inventory_arg,expected_instance_id)==16,"expected token");
_Static_assert(__builtin_offsetof(struct sumh_rule_inventory_arg,cursor)==32,"cursor offset");
_Static_assert(__builtin_offsetof(struct sumh_rule_inventory_arg,instance_id)==40,"instance offset");
_Static_assert(__builtin_offsetof(struct sumh_rule_inventory_arg,generation)==48,"generation offset");
_Static_assert(__builtin_offsetof(struct sumh_rule_inventory_arg,entry)==80,"entry offset");
_Static_assert(SUMH_IOC_GET_RULE_INVENTORY==0xe0a05334U,"ioctl encoding");
_Static_assert(SUMH_FEATURE_RULE_INVENTORY==(1U<<20),"capability");
_Static_assert(SUMH_PROTOCOL_VERSION==1,"legacy protocol");
"""


PREAMBLE = r"""
#define __user
#define EINVAL 22
#define EFAULT 14
#define EOPNOTSUPP 95
#define ESHUTDOWN 108
#define ESTALE 116
#define ENOMEM 12
#define EOVERFLOW 75
#define GFP_KERNEL 0
#define BUCKETS 4
#define NMAP 40
#define pr_warn(...) ((void)0)
#define container_of(p,t,m) ((t *)((char *)(p)-__builtin_offsetof(t,m)))
struct hlist_node { struct hlist_node *next; };
struct hlist_head { struct hlist_node *first; };
struct list_head { struct list_head *next,*prev; };
#define hent(p,t,m) ((p) ? container_of(p,t,m) : NULL)
#define hash_for_each(table,bkt,pos,member) \
    for ((bkt)=0,(pos)=NULL; (pos)==NULL && (bkt)<BUCKETS; (bkt)++) \
        for ((pos)=hent((table)[bkt].first,__typeof__(*(pos)),member);(pos); \
             (pos)=hent((pos)->member.next,__typeof__(*(pos)),member))
#define list_for_each_entry(pos,head,member) \
    for ((pos)=container_of((head)->next,__typeof__(*(pos)),member); \
         &(pos)->member!=(head); \
         (pos)=container_of((pos)->member.next,__typeof__(*(pos)),member))
static void list_init(struct list_head *h) { h->next=h->prev=h; }
static void list_add_tail(struct list_head *n,struct list_head *h) {
    n->prev=h->prev; n->next=h; h->prev->next=n; h->prev=n;
}
struct sumh_entry { char *src,*target; unsigned char type; struct hlist_node node; };
struct sumh_hide_entry { char *path; bool storage_managed; struct hlist_node node; };
struct sumh_merge_entry { char *src,*target; struct hlist_node node; };
struct sumh_inject_entry { char *dir; struct hlist_node node; };
struct sumh_maps_rule_entry {
    bool managed;
    /* Model ARM64 kernel unsigned long, not Windows LLP64 unsigned long. */
    u64 target_ino,target_dev,spoofed_ino,spoofed_dev;
    char spoofed_pathname[SUMH_MAX_LEN_PATHNAME];
};
struct managed_hide_rule {
    struct list_head list; u64 id,generation; int state,error; char path[4097];
};
static struct hlist_head sumh_paths[BUCKETS],sumh_hide_paths[BUCKETS];
static struct hlist_head sumh_merge_dirs[BUCKETS],sumh_inject_dirs[BUCKETS];
static struct list_head hide_rules;
static struct sumh_entry mappings[NMAP];
static char visible[NMAP][4097],backing[NMAP][4097];
static struct sumh_hide_entry hidden;
static struct sumh_merge_entry merges[2];
static struct sumh_inject_entry injected;
static struct sumh_maps_rule_entry maps[4];
struct sumh_maps_snapshot { unsigned int count; };
static struct sumh_maps_snapshot maps_snapshot;
static int maps_references,maps_gets,maps_puts;
static struct managed_hide_rule managed[2];
static u64 sumh_runtime_generation=1,sumh_runtime_instance=7;
static s32 sumh_runtime_errors[SUMH_RUNTIME_STATE_COUNT];
static bool sumh_runtime_live=true;
static int lock_depth,read_depth,bad_context,allocation_fails,input_fault,output_fault;
static int allocated,allocations,frees,copies,traces,hide_queries,rebinds,init_calls;
static int sumh_mutation_mutex,sumh_ioctl_tgid,current;
static struct sumh_maps_snapshot *sumh_maps_snapshot_get(void) {
    if(lock_depth!=1||read_depth!=1) bad_context++;
    maps_gets++;
    if(!maps_snapshot.count) return NULL;
    maps_references++; return &maps_snapshot;
}
static void sumh_maps_snapshot_put(struct sumh_maps_snapshot *snapshot) {
    if(!snapshot) return;
    if(snapshot!=&maps_snapshot||maps_references!=1||lock_depth!=1||read_depth!=1) bad_context++;
    maps_references--; maps_puts++;
}
static unsigned int sumh_maps_snapshot_count(const struct sumh_maps_snapshot *snapshot) {
    if(snapshot && (snapshot!=&maps_snapshot||maps_references!=1)) bad_context++;
    return snapshot ? snapshot->count : 0;
}
static const struct sumh_maps_rule_entry *sumh_maps_snapshot_entry(
        const struct sumh_maps_snapshot *snapshot,unsigned int index) {
    if(snapshot!=&maps_snapshot||maps_references!=1) { bad_context++; return NULL; }
    return index<snapshot->count ? &maps[index] : NULL;
}
static struct { struct sumh_rule_inventory_arg page; unsigned char guard; } memory;
static struct sumh_rule_inventory_arg request,saved;
static int memcmp(const void *a,const void *b,size_t n) {
    const unsigned char *x=a,*y=b; for(size_t i=0;i<n;i++) if(x[i]!=y[i]) return x[i]-y[i]; return 0;
}
static void *memchr_inv(const void *p,int value,size_t n) {
    const unsigned char *s=p; for(size_t i=0;i<n;i++) if(s[i]!=(unsigned char)value) return (void *)(s+i); return NULL;
}
static void *kvzalloc(size_t n,int flags) {
    if(n!=sizeof(memory.page)||flags!=GFP_KERNEL||allocated||lock_depth!=1||read_depth!=1) bad_context++;
    allocations++; if(allocation_fails) return NULL;
    allocated=1; memset(&memory,0,sizeof(memory)); memory.guard=0x5a; return &memory.page;
}
static void kvfree(void *p) {
    if(p!=&memory.page||!allocated||memory.guard!=0x5a) bad_context++;
    allocated=0; frees++;
}
static int copy_from_user(void *out,const void *in,size_t n) {
    if(lock_depth!=1||!allocated||n!=sizeof(request)||in!=&request) bad_context++;
    if(input_fault) return 1; memcpy(out,in,n); return 0;
}
static int copy_to_user(void *out,const void *in,size_t n) {
    if(lock_depth!=1||!allocated||n!=sizeof(request)||out!=&request) bad_context++;
    copies++; if(output_fault) return 1; memcpy(out,in,n); return 0;
}
static int sumh_control_begin(void) { if(read_depth||lock_depth) bad_context++; read_depth++; return 0; }
static void sumh_control_end(void) { if(read_depth!=1||lock_depth) bad_context++; read_depth--; }
static void mutex_lock(int *p) { if(p!=&sumh_mutation_mutex||lock_depth||read_depth!=1) bad_context++; lock_depth++; }
static void mutex_unlock(int *p) { if(p!=&sumh_mutation_mutex||lock_depth!=1) bad_context++; lock_depth--; }
static void atomic_long_set(int *p,long v) { if(p!=&sumh_ioctl_tgid) bad_context++; *p=v; traces++; }
static int task_tgid_vnr(int ignored) { (void)ignored; return 9; }
static int ksu_sucompat_vfs_enabled(void) { return 1; }
static int ksu_sucompat_vfs_refresh(void) { rebinds++; return 0; }
static int sumh_runtime_state_get(void *p) { (void)p; return -EOPNOTSUPP; }
static int sumh_runtime_feature_set(void *p) { (void)p; return -EOPNOTSUPP; }
static int sumh_kernel_build_conditional_set(void *p) { (void)p; return -EOPNOTSUPP; }
/* The exact deletion path is covered by test_sumh_maps_delete.py. */
static long sumh_maps_delete_temporary(void *p) { (void)p; return -EOPNOTSUPP; }
static int sumh_hide_rules_ioctl(unsigned int cmd,void *p) {
    (void)p; hide_queries++;
    if(cmd==SUMH_IOC_USER_HIDE_RETRY) managed[0].generation++;
    return 0;
}
static int sumh_dispatch_cmd(unsigned int cmd,void *p) { (void)cmd;(void)p; return 0; }
"""


TESTS = r"""
static void prepare(u32 category,u64 cursor,bool token) {
    memset(&request,0,sizeof(request)); request.size=sizeof(request); request.version=1;
    request.category=category; request.cursor=cursor;
    if(token) { request.expected_instance_id=sumh_runtime_instance; request.expected_generation=sumh_runtime_generation; }
}
static int query(void) {
    int ret=sumh_handle_ioctl(SUMH_IOC_GET_RULE_INVENTORY,&request);
    if(maps_references) bad_context++;
    return ret;
}
static void fixtures(void) {
    maps_snapshot.count=4; list_init(&hide_rules);
    for(int i=0;i<NMAP;i++) {
        memset(visible[i],'v',2000); visible[i][0]='/'; visible[i][1999]='A'+i;
        memcpy(visible[i]+20," spaces \r\n \xe4\xb8\xad\xe6\x96\x87 ",18);
        memset(backing[i],'b',2000); backing[i][0]='/'; backing[i][1999]='A'+i;
        /* A module-looking path is still UNATTRIBUTED. */
        memcpy(backing[i],"/data/adb/modules/",18);
        mappings[i].src=visible[i]; mappings[i].target=backing[i]; mappings[i].type=8;
        mappings[i].node.next=i+BUCKETS<NMAP ? &mappings[i+BUCKETS].node : NULL;
    }
    for(int bucket=0;bucket<BUCKETS;bucket++) sumh_paths[bucket].first=&mappings[bucket].node;
    hidden.path="/hidden \r\n \xe9\x9b\xaa "; hidden.storage_managed=true;
    sumh_hide_paths[3].first=&hidden.node;
    merges[0].src=merges[1].src="/shared dir";
    merges[0].target="/backing first"; merges[1].target="/backing second";
    merges[0].node.next=&merges[1].node; sumh_merge_dirs[1].first=&merges[0].node;
    injected.dir="/injected \n"; sumh_inject_dirs[0].first=&injected.node;
    for(int i=0;i<4;i++) {
        /* Both owners of identical keys remain independently enumerable. */
        maps[i].managed=(i%2)==1; maps[i].target_ino=0x20000000000001ULL;
        maps[i].target_dev=i/2; maps[i].spoofed_ino=0xffffffffffffffffULL-i;
        maps[i].spoofed_dev=0xffffffffULL-i;
        memcpy(maps[i].spoofed_pathname,"/maps path \xe4\xb8\xad ",16);
    }
    for(int i=0;i<2;i++) {
        managed[i].id=123+i*864; managed[i].generation=3+i;
        memcpy(managed[i].path,"/user hidden \r\n \xe4\xb8\xad ",20);
        list_add_tail(&managed[i].list,&hide_rules);
    }
}
static int test_pages(void) {
    fixtures();
    prepare(0,0,false); CHECK(!query()&&request.complete&&request.count==0);
    CHECK(request.supported_categories==0x7f&&request.flags==1&&request.instance_id==7&&request.generation==1);
    CHECK(!request.expected_generation&&!request.expected_instance_id&&!memchr_inv(&request.entry,0,sizeof(request.entry)));
    u64 total_bytes=0;
    for(u64 i=0;i<NMAP;i++) {
        int source=(i/(NMAP/BUCKETS))+(i%(NMAP/BUCKETS))*BUCKETS;
        prepare(SUMH_INVENTORY_MAPPING,i,i!=0); CHECK(!query());
        CHECK(request.count==1&&request.cursor==i&&request.next_cursor==i+1&&request.complete==(i+1==NMAP));
        CHECK(request.entry.category==1&&request.entry.id==i+1&&request.entry.id_kind==1);
        CHECK(!request.entry.owner&&!request.entry.definition_generation&&request.entry.type==8);
        CHECK(request.entry.path1_len==2000&&request.entry.path2_len==2000);
        CHECK(!memcmp(request.entry.path1,visible[source],2001)&&!memcmp(request.entry.path2,backing[source],2001));
        CHECK(!memchr_inv(request.entry.path1+2000,0,4096-2000));
        CHECK(!memchr_inv(request.entry.path2+2000,0,4096-2000));
        total_bytes+=request.entry.path1_len+request.entry.path2_len;
    }
    CHECK(total_bytes>64*1024);
    prepare(1,NMAP,true); CHECK(!query()&&!request.count&&request.complete&&request.next_cursor==NMAP);
    prepare(1,NMAP+1,true); CHECK(query()==-EINVAL);
    prepare(2,0,true); CHECK(!query()&&request.count==1&&request.complete&&request.entry.flags==1&&!request.entry.owner);
    CHECK(!strcmp(request.entry.path1,hidden.path)&&!request.entry.path2_len);
    for(u64 i=0;i<2;i++) {
        prepare(3,i,true); CHECK(!query()&&request.count==1&&request.complete==(i==1));
        CHECK(!strcmp(request.entry.path1,merges[i].src)&&!strcmp(request.entry.path2,merges[i].target));
    }
    for(u32 category=4;category<=5;category++) for(u64 i=0;i<2;i++) {
        int idx=i*2+(category==4); prepare(category,i,true); CHECK(!query());
        CHECK(request.complete==(i==1)&&request.entry.id==i+1&&request.entry.id_kind==1);
        CHECK(request.entry.owner==(category==4?2u:0u)&&!request.entry.path1_len);
        CHECK(request.entry.target_ino==maps[idx].target_ino&&request.entry.target_dev==maps[idx].target_dev);
        CHECK(request.entry.spoofed_ino==maps[idx].spoofed_ino&&request.entry.spoofed_dev==maps[idx].spoofed_dev);
        CHECK(!strcmp(request.entry.path2,maps[idx].spoofed_pathname));
    }
    CHECK(maps_gets==maps_puts&&!maps_references);
    for(u32 category=4;category<=5;category++) {
        prepare(category,2,true); CHECK(!query()&&request.complete&&!request.count);
        prepare(category,3,true); CHECK(query()==-EINVAL);
    }
    prepare(6,0,true); CHECK(!query()&&request.complete&&!strcmp(request.entry.path1,injected.dir));
    for(u64 i=0;i<2;i++) {
        prepare(7,i,true); CHECK(!query()&&request.count==1&&request.complete==(i==1));
        CHECK(request.entry.owner==1&&request.entry.id_kind==2&&request.entry.id==managed[i].id);
        CHECK(request.entry.definition_generation==managed[i].generation&&!strcmp(request.entry.path1,managed[i].path));
    }
    prepare(7,2,true); CHECK(!query()&&!request.count&&request.complete);
    prepare(7,3,true); CHECK(query()==-EINVAL);
    /* Binding state is deliberately absent and cannot invalidate definitions. */
    prepare(7,0,true); CHECK(!query()); saved=request;
    managed[0].state=3; managed[0].error=-12;
    prepare(7,0,true); CHECK(!query()&&!memcmp(&request,&saved,sizeof(request)));
    CHECK(!hide_queries&&!traces&&!rebinds&&!init_calls);
    return 0;
}
static int test_errors(void) {
    for(size_t i=0;i<sizeof(request);i++) {
        /* All output, reserved and record bytes are required zero. */
        if(i<12||(i>=16&&i<40)) continue;
        prepare(1,0,false); ((unsigned char *)&request)[i]=1; saved=request;
        CHECK(query()==-EINVAL&&!memcmp(&request,&saved,sizeof(request)));
    }
    prepare(1,0,false); request.size--; CHECK(query()==-EINVAL);
    prepare(1,0,false); request.version=2; CHECK(query()==-EINVAL);
    prepare(1,1,false); CHECK(query()==-EINVAL);
    prepare(1,0,false); request.expected_generation=1; CHECK(query()==-EINVAL);
    prepare(1,0,false); request.expected_instance_id=7; CHECK(query()==-EINVAL);
    prepare(0,1,true); CHECK(query()==-EINVAL);
    for(u32 category=8;category<=11;category++) { prepare(category,0,false); CHECK(query()==-EOPNOTSUPP); }
    prepare(0xffffffffU,0,false); CHECK(query()==-EOPNOTSUPP);
    prepare(1,0,true); request.expected_instance_id++; CHECK(query()==-ESTALE);
    prepare(1,0,true); request.expected_generation++; CHECK(query()==-ESTALE);
    prepare(1,0,false); saved=request; input_fault=1; CHECK(query()==-EFAULT); input_fault=0;
    CHECK(!memcmp(&request,&saved,sizeof(request)));
    output_fault=1; CHECK(query()==-EFAULT); output_fault=0;
    CHECK(!memcmp(&request,&saved,sizeof(request)));
    allocation_fails=1; CHECK(query()==-ENOMEM); allocation_fails=0;
    CHECK(!memcmp(&request,&saved,sizeof(request)));
    memset(visible[0],'x',4095); visible[0][4095]=0;
    CHECK(!query()&&request.entry.path1_len==4095&&request.entry.path1[4095]==0);
    visible[0][4095]='x'; prepare(1,0,false); saved=request;
    CHECK(query()==-EOVERFLOW&&!memcmp(&request,&saved,sizeof(request)));
    visible[0][2000]=0;
    memset(managed[0].path,'x',4096); prepare(7,0,false); CHECK(query()==-EOVERFLOW); managed[0].path[10]=0;
    sumh_runtime_live=false; prepare(0,0,false); CHECK(query()==-ESHUTDOWN); sumh_runtime_live=true;
    CHECK(sumh_runtime_generation==1&&sumh_runtime_instance==7&&!traces&&!hide_queries&&!rebinds);
    CHECK(!memchr_inv(sumh_runtime_errors,0,sizeof(sumh_runtime_errors)));
    return 0;
}
static int test_generation_and_empty(void) {
    prepare(1,0,false); CHECK(!query()); u64 old=request.generation;
    /* Execute actual wrapper+result recording for an intervening mutation. */
    CHECK(!sumh_handle_ioctl(SUMH_IOC_ADD_RULE,NULL)&&sumh_runtime_generation==old+1);
    prepare(1,1,true); request.expected_generation=old; CHECK(query()==-ESTALE);
    prepare(7,0,true); old=request.expected_generation;
    CHECK(!sumh_handle_ioctl(SUMH_IOC_USER_HIDE_RETRY,NULL));
    CHECK(query()==-ESTALE&&managed[0].generation==4&&sumh_runtime_generation==old+1);
    prepare(7,0,false); CHECK(!query()&&request.entry.definition_generation==4);
    old=sumh_runtime_generation; CHECK(!sumh_handle_ioctl(SUMH_IOC_CLEAR_ALL,NULL));
    CHECK(sumh_runtime_generation==old+1);
    memset(sumh_paths,0,sizeof(sumh_paths)); memset(sumh_hide_paths,0,sizeof(sumh_hide_paths));
    memset(sumh_merge_dirs,0,sizeof(sumh_merge_dirs)); memset(sumh_inject_dirs,0,sizeof(sumh_inject_dirs));
    maps_snapshot.count=0; list_init(&hide_rules);
    for(u32 category=0;category<=7;category++) {
        prepare(category,0,true); CHECK(!query()&&request.complete&&!request.count&&!request.next_cursor);
        CHECK(!memchr_inv(&request.entry,0,sizeof(request.entry)));
    }
    prepare(1,0,true); sumh_runtime_instance++; CHECK(query()==-ESTALE);
    CHECK(!allocated&&!bad_context&&!read_depth&&!lock_depth&&!maps_references);
    CHECK(allocations==frees+1); /* Exactly one deliberately failed allocation. */
    return 0;
}
int main(void) {
    int ret=test_pages(); if(ret) return ret;
    ret=test_errors(); if(ret) return ret;
    return test_generation_and_empty();
}
"""


def inventory_source():
    functions = function("kernel/sumh/core/sumh_hide_rules.c", "int sumh_hide_rule_inventory(")
    for marker in ("static void sumh_runtime_advance(", "static int sumh_inventory_path(",
                   "static int sumh_inventory_position(", "static int sumh_inventory_collect(",
                   "static int sumh_rule_inventory_get(", "static void sumh_runtime_record_result(",
                   "long sumh_handle_ioctl("):
        functions += "\n" + function(IOCTL, marker)
    return COMMON + abi_source() + PREAMBLE + functions + TESTS


def check_abi(compiler):
    with tempfile.TemporaryDirectory(prefix="sumh-inventory-abi-") as temp:
        source = Path(temp) / "abi.c"
        source.write_text(abi_source(), encoding="utf-8")
        for target in ("armv7a-linux-gnueabi", "aarch64-linux-gnu"):
            subprocess.run([compiler, "--target=" + target, "-std=gnu11", "-ffreestanding", "-Werror",
                            "-c", str(source), "-o", str(Path(temp) / (target + ".o"))], check=True)
            print("PASS inventory ABI compile " + target)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("clang is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    check_abi(compiler)
    run_test(compiler, "rule-inventory", inventory_source())
