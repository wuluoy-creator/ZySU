"""d_op transfer, captured source, native-name retirement and SOP terminal edge."""

from test_sumh_dh_ownership_source import ROOT, core, FIXTURES, read, function, replace_once
from test_sumh_dh_ownership_cases import HELPERS

INPUTS = ("kernel/sumh/core/sumh_vnode.c", "kernel/sumh/core/sumh_vnode.h")

CASES = r"""
static char external_name[16];
static struct sumh_dh_claim *lifetime_claim;
static struct sumh_dh_dir *lifetime_dir;
static int hook_observations;
static void moved_name(void){
    retired_name=external_name;retired_name_len=sizeof(external_name);
    memset(external_name,0xdd,sizeof(external_name));
    fixture_dentries[20].d_parent=&fixture_dentries[2];
    fixture_dentries[20].d_name=(struct qstr){"moved",5,0};
}
static void setup_child(void){
    memcpy(external_name,"file",5);retired_name=NULL;retired_name_reads=0;
    fixture_dentries[20].d_parent=&fixture_dentries[1];
    fixture_dentries[20].d_name=(struct qstr){external_name,4,0};
}
static int name_capture_case(void){
    fixture_init();struct sumh_dh_claim *a;
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    struct sumh_dh_projection projection={.payload=a->payload};
    setup_child();during_allocation=moved_name;
    int ret=sumh_dh_set_dentry_ops(a->slot->dir,&fixture_dentries[20],&projection);
    CHECK(ret==-EAGAIN&&!retired_name_reads&&!fixture_dentries[20].d_op);
    setup_child();CHECK(sumh_dh_set_dentry_ops(a->slot->dir,&fixture_dentries[20],NULL)==0);
    struct sumh_dh_dop_meta *dm=sumh_dh_dop_lookup_locked(&fixture_dentries[20]);
    CHECK(dm&&!dm->synthetic_negative);const struct dentry_operations *ops=fixture_dentries[20].d_op;
    during_allocation=moved_name;ret=sumh_dh_set_dentry_ops(a->slot->dir,&fixture_dentries[20],&projection);
    CHECK(ret==-EAGAIN&&!retired_name_reads&&fixture_dentries[20].d_op==ops&&!dm->synthetic_negative);
    retired_name=NULL;withdraw(a);return finish_case();
}
static void parent_protected(struct inode *inode){
    if(inode==&fixture_inodes[1]){struct sumh_sop_meta *m=sumh_sop_lookup_locked(&fixture_sb);if(!m||!m->dh_clients||fixture_sb.s_active<=1)BAD();hook_observations++;}
}
static void child_before_dir(struct dentry *d){
    if(d==&fixture_dentries[20]){int n=allocation_slot(lifetime_dir);if(n<0||!used[n]||!refcount_read(&lifetime_dir->refs))BAD();hook_observations++;}
}
static void detach_while_dop_worker_drains(void){
    withdraw(lifetime_claim);sumh_dh_drain_releases();lifetime_claim=NULL;
    int n=allocation_slot(lifetime_dir);
    if(n<0||!used[n]||!refcount_read(&lifetime_dir->refs)||!sumh_sop_lookup_locked(&fixture_sb)->dh_clients)BAD();
    hook_observations++;
}
static int dop_worker_owned_directory(void){
    fixture_init();CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&lifetime_claim)==0&&committed(lifetime_claim,NULL)==0);
    lifetime_dir=lifetime_claim->slot->dir;setup_child();
    CHECK(sumh_dh_set_dentry_ops(lifetime_dir,&fixture_dentries[20],NULL)==0);
    struct sumh_dh_dop_meta *dm=sumh_dh_dop_lookup_locked(&fixture_dentries[20]);
    CHECK(dm&&refcount_read(&lifetime_dir->refs)==3);
    int observations=hook_observations;before_dput=child_before_dir;before_iput=parent_protected;
    sumh_dh_queue_dop_retire(dm);during_srcu=detach_while_dop_worker_drains;
    flush_work(&sumh_dh_reap_dops_work);
    before_dput=NULL;before_iput=NULL;
    CHECK(!lifetime_claim&&hook_observations>=observations+3&&!fixture_dentries[20].d_op);
    CHECK(atomic_read(&sumh_sop_live)==0&&fixture_dentries[20].refs==1);
    return finish_case();
}
static int retained_vnode_reap(void){
    fixture_init();struct sumh_dh_claim *a;
    CHECK(prepared("/one/file",SUMH_DH_OWNER_SHADOW,16,501,&a)==0&&committed(a,NULL)==0);
    struct sumh_vnode_info *info=kzalloc(sizeof(*info),GFP_KERNEL);CHECK(info&&sumh_sop_vnode_get(&fixture_sb));
    info->sop_vnode_ref=true;info->source=fixture_path(16);path_get(&info->source);
    struct inode *v=&fixture_inodes[30];v->i_private=info;v->synthetic=true;
    withdraw(a);sumh_dh_drain_releases();
    struct sumh_sop_meta *m=sumh_sop_lookup_locked(&fixture_sb);
    CHECK(m&&!m->dh_clients&&atomic_read(&m->vnode_live)==1&&fixture_sb.s_active==2);
    /* Actual super_operations trampoline -> actual vnode free-info -> SOP put. */
    fixture_sb.s_op->destroy_inode(v);
    CHECK(!v->i_private&&sumh_sop_reap_work.pending&&atomic_read(&sumh_sop_vnodes)==0);
    flush_work(&sumh_sop_reap_work);
    CHECK(atomic_read(&sumh_sop_live)==0&&fixture_sb.s_active==1&&self_module.refs==1);
    v->synthetic=false;return finish_case();
}
static int vpath_projection_failure(void){
    fixture_init();struct sumh_dh_claim *a;
    CHECK(prepared("/missing/leaf",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    CHECK(a->kind==SUMH_DH_OWNER_VTOPO&&a->payload->vpath);
    struct dentry *d=&fixture_dentries[20];d->d_parent=&fixture_dentries[0];d->d_name=(struct qstr){"missing",7,0};d->d_inode=NULL;
    int calls=vnode_calls,gets=path_gets,puts=path_puts;
    /* Payload pins vpath directly now; this model's vnode allocator fails. */
    struct dentry *res=sumh_dh_lookup(&fixture_inodes[0],d,0);
    CHECK(IS_ERR(res)&&PTR_ERR(res)==-ENOMEM&&vnode_calls==calls+1);
    CHECK(path_gets-gets==path_puts-puts&&!d->d_op);
    withdraw(a);return finish_case();
}
static struct sumh_dh_retire reader_retired;
static void withdraw_during_vnode(const struct path *captured){
    during_vnode=NULL;
    if(!captured||captured->dentry!=&fixture_dentries[16]||captured->dentry->refs<2)BAD();
    sumh_dh_detach(lifetime_claim,&reader_retired);sumh_dh_claim_put(lifetime_claim);lifetime_claim=NULL;sumh_dh_drain_releases();
    if(captured->dentry!=&fixture_dentries[16]||captured->dentry->refs!=2)BAD();
    native_path("/source/a",fixture_path(17),0);hook_observations++;
}
static int captured_source_survives(void){
    fixture_init();CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&lifetime_claim)==0&&committed(lifetime_claim,NULL)==0);
    setup_child();fixture_dentries[20].d_inode=NULL;
    sumh_dh_retire_init(&reader_retired,false);
    int observed=hook_observations;during_vnode=withdraw_during_vnode;
    struct dentry *res=sumh_dh_lookup(&fixture_inodes[1],&fixture_dentries[20],0);
    CHECK(IS_ERR(res)&&PTR_ERR(res)==-ENOMEM&&native_lookups==0);
    CHECK(hook_observations==observed+1&&!lifetime_claim&&fixture_dentries[16].refs==1&&last_vnode_source.dentry==&fixture_dentries[16]);
    sumh_dh_retire_finish(&reader_retired);
    return finish_case();
}
static int observer_policy_case(void){
    fixture_init();struct sumh_dh_claim *a,*hide;
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    setup_child();struct dentry *d=&fixture_dentries[20];d->d_inode=NULL;
    int before=native_lookups;model_sees=false;model_hides=false;
    CHECK(sumh_dh_lookup(&fixture_inodes[1],d,0)==NULL&&native_lookups==before+1);
    struct sumh_dh_dop_meta *dm=sumh_dh_dop_lookup_locked(d);CHECK(dm);
    bool chain=false;CHECK(sumh_dh_revalidate_inner(dm,&fixture_inodes[1],&d->d_name,d,LOOKUP_RCU,&chain)==1&&chain);
    model_sees=true;chain=false;CHECK(sumh_dh_revalidate_inner(dm,&fixture_inodes[1],&d->d_name,d,0,&chain)==0&&!chain);
    model_sees=false;native_error=-ECHILD;struct file file={0};
    CHECK(sumh_dh_atomic_open(&fixture_inodes[1],d,&file,0,0)==-ECHILD);native_error=0;
    CHECK(prepared("/one/file",SUMH_DH_OWNER_LEGACY_HIDE,0,0,&hide)==0&&committed(hide,NULL)==0);
    model_hides=true;CHECK(sumh_dh_atomic_open(&fixture_inodes[1],d,&file,0,0)==-ENOENT);
    CHECK(!sumh_dirhijack_emits(&fixture_inodes[1],"file",4));
    model_hides=false;CHECK(sumh_dh_atomic_open(&fixture_inodes[1],d,&file,0,0)==0);
    withdraw(hide);withdraw(a);return finish_case();
}
static struct sumh_dh_retire iterate_retired;
static int iterate_snapshot_seen,iterate_callback_seen,iterate_dir_slot;
static void withdraw_after_fop_snapshot(void){
    if(sumh_fop_client_srcu!=1||sumh_dh_srcu||!used[iterate_dir_slot])BAD();
    sumh_dh_detach(lifetime_claim,&iterate_retired);sumh_dh_claim_put(lifetime_claim);lifetime_claim=NULL;
    if(!lifetime_dir->retiring||sumh_fop_lookup_rcu(&fixture_inodes[1])->iterate_client||!used[iterate_dir_slot])BAD();
    iterate_snapshot_seen++;
}
static void saved_dh_callback_native(void){
    if(!iterate_snapshot_seen||sumh_dh_srcu!=1||sumh_fop_client_srcu!=1||!used[iterate_dir_slot]||!sumh_sop_lookup_locked(&fixture_sb)->dh_clients)BAD();
    iterate_callback_seen++;
}
static int saved_fop_dh_callback(void){
    fixture_init();CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&lifetime_claim)==0&&committed(lifetime_claim,NULL)==0);
    lifetime_dir=lifetime_claim->slot->dir;iterate_dir_slot=allocation_slot(lifetime_dir);
    struct sumh_fop_meta *fop=sumh_fop_lookup_rcu(&fixture_inodes[1]);
    CHECK(fop&&fop->iterate_client==sumh_dh_iterate&&fop->iterate_client_data==lifetime_dir);
    struct file file={.f_path=fixture_path(1),.f_op=fixture_inodes[1].i_fop};struct dir_context context={0};
    sumh_dh_retire_init(&iterate_retired,false);iterate_snapshot_seen=iterate_callback_seen=0;
    after_rcu_unlock=withdraw_after_fop_snapshot;native_iteration_hook=saved_dh_callback_native;
    CHECK(file.f_op->iterate_shared(&file,&context)==0);
    native_iteration_hook=NULL;
    CHECK(iterate_snapshot_seen==1&&iterate_callback_seen==1&&!sumh_fop_client_srcu&&!sumh_dh_srcu&&used[iterate_dir_slot]);
    sumh_dh_retire_finish(&iterate_retired);sumh_dh_drain_releases();
    CHECK(!used[iterate_dir_slot]&&!sumh_sop_lookup_locked(&fixture_sb));return finish_case();
}
int main(void){int r;if((r=name_capture_case()))return r;if((r=dop_worker_owned_directory()))return r;if((r=retained_vnode_reap()))return r;if((r=vpath_projection_failure()))return r;if((r=captured_source_survives()))return r;if((r=observer_policy_case()))return r;if((r=saved_fop_dh_callback()))return r;return 0;}
"""


def source(root=ROOT):
    text = core(root)
    vnode = read(root, INPUTS[0])
    layout = function(read(root, INPUTS[1]), "struct sumh_vnode_info {") + ";\n"
    body = function(vnode, "void sumh_vnode_free_info(")
    stub = function(text, "static void sumh_vnode_free_info(")
    text = replace_once(text, stub, "void sumh_vnode_free_info(struct inode *inode);")
    return text + "\ntypedef unsigned long dev_t;struct cpu_vfs_cap_data {int unused;};\n" + layout + body + FIXTURES + HELPERS + CASES
