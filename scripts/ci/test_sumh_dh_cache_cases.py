"""Cache completion schedules through real slots, claims and d_op callbacks."""

from test_sumh_dh_ownership_cases import HELPERS as OWNER_HELPERS
from test_sumh_dh_ownership_source import function

HELPERS = "\n".join(function(OWNER_HELPERS, marker) for marker in (
    "static int prepared(", "static int committed(", "static void withdraw(",
    "static unsigned long winner(",
)) + r"""
static struct sumh_dh_retire deferred_retired;
static bool deferred_pending;
static struct sumh_dh_claim *change_add,*change_remove;
static int change_status;
static void defer_change(void){
    struct sumh_dh_change c;sumh_dh_retire_init(&deferred_retired,false);
    change_status=sumh_dh_change_begin(change_add,change_remove,&c);
    if(change_status)return;
    sumh_dh_change_commit(&c,&deferred_retired);sumh_dh_change_end();deferred_pending=true;
}
static void defer_detach(void){
    sumh_dh_retire_init(&deferred_retired,false);
    sumh_dh_detach(change_remove,&deferred_retired);deferred_pending=true;
}
static void flush_deferred(void){
    if(deferred_pending){sumh_dh_retire_finish(&deferred_retired);deferred_pending=false;}
    if(change_remove){sumh_dh_claim_put(change_remove);change_remove=NULL;}
    change_add=NULL;
}
static void vnode_changes(const struct path *p){(void)p;defer_change();}
static void vnode_detaches(const struct path *p){(void)p;defer_detach();}
static void published_detaches(struct dentry *d){(void)d;defer_detach();}
static void native_rename(void){if(native_alias)native_alias->d_name=(struct qstr){"moved",5,0};}
static struct dentry *request(int n,const char *name){
    struct dentry *d=&fixture_dentries[n];d->d_inode=NULL;d->d_parent=&fixture_dentries[1];
    d->d_name=(struct qstr){name,(unsigned)strlen(name),0};d->hashed=false;return d;
}
static bool governed(struct dentry *d){return d->d_op&&(d->d_flags&DCACHE_OP_REVALIDATE);}
static int validate(struct dentry *d){
    if(!governed(d))return 2;
    int a=allocations,s=srcu_syncs,r=rcu_syncs,t=tasks_syncs;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,13,0)
    int result=d->d_op->d_revalidate(d_inode(d->d_parent),&d->d_name,d,LOOKUP_RCU);
#else
    int result=d->d_op->d_revalidate(d,LOOKUP_RCU);
#endif
    if(a!=allocations||s!=srcu_syncs||r!=rcu_syncs||t!=tasks_syncs)BAD();return result;
}
static int validate_meta(struct sumh_dh_dop_meta *meta,struct dentry *d){
    bool chain=false;int a=allocations,s=srcu_syncs,r=rcu_syncs,t=tasks_syncs;
    int result=sumh_dh_revalidate_inner(meta,d_inode(d->d_parent),&d->d_name,d,LOOKUP_RCU,&chain);
    if(a!=allocations||s!=srcu_syncs||r!=rcu_syncs||t!=tasks_syncs)BAD();return result;
}
static struct file new_file(void){return (struct file){.f_path={.dentry=DENTRY_NOT_SET}};}
static void file_release(struct file *f){
    if(f->f_path.dentry&&!IS_ERR(f->f_path.dentry)){dput(f->f_path.dentry);f->f_path.dentry=NULL;}
}
static int finish_case(void){
    flush_deferred();cache_count=0;sumh_dirhijack_exit();sumh_dh_drain_releases();sumh_fop_override_exit();sumh_sop_shadow_exit();
    CHECK(!bad&&!live_allocations&&!callback_count&&!sumh_dh_release_work.pending);
    CHECK(!inode_write_depth&&!cache_mutex_depth&&inode_lock_calls==inode_unlock_calls);
    CHECK(fixture_sb.s_active==1&&self_module.refs==1&&foreign_module.refs==1&&fixture_mount.refs==1);
    for(int i=0;i<32;i++)CHECK(fixture_inodes[i].refs==1&&fixture_dentries[i].refs==1);
    for(int i=0;i<modeled_vnode_count;i++){CHECK(modeled_vnodes[i].refs>=0&&modeled_vnodes[i].refs<=1);if(modeled_vnodes[i].refs)iput(&modeled_vnodes[i]);}
    CHECK(!bad);return 0;
}
static struct dentry *early_dentry;
static const struct inode_operations *early_iop;
static void finish_early(struct inode *i){
    if(i->readers!=1||i->i_op!=early_iop||cache_mutex_depth)BAD();
    struct dentry *r=early_iop->lookup(i,early_dentry,0);if(r)BAD();
    first_native_complete=++sequence;i->readers--;
}
static void check_after_unlock(struct inode *i){
    if(i->writer||cache_mutex_depth||early_dentry->hashed||!first_invalidation||first_native_complete>=first_invalidation)BAD();
}
"""

CASES = r"""
static int first_publication(void){
    for(int negative=0;negative<2;negative++){
        fixture_init();struct sumh_dh_claim *a;
        early_dentry=request(3,"file");early_iop=fixture_inodes[1].i_op;
        cache_bindings[cache_count++]=(struct cache_binding){&fixture_dentries[1],early_dentry,"file"};
        fixture_inodes[1].readers=1;native_negative=negative;before_inode_lock=finish_early;after_inode_unlock=check_after_unlock;
        CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0);
        CHECK(inode_lock_calls==1&&inode_unlock_calls==1&&first_native_complete&&first_invalidation>first_native_complete);
        CHECK(!early_dentry->hashed&&!fixture_inodes[1].readers&&!bad);
        CHECK(committed(a,NULL)==0);withdraw(a);int r=finish_case();if(r)return r;
    }return 0;
}
static int transparent_prepared(void){
    for(int negative=0;negative<2;negative++)for(int alias=0;alias<2;alias++){
        fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0);
        struct dentry *d=request(3,"file"),*actual=alias?request(4,"file"):d;
        native_negative=negative;native_alias=alias?actual:NULL;
        struct dentry *res=sumh_dh_lookup(&fixture_inodes[1],d,LOOKUP_RCU);
        CHECK(!IS_ERR(res)&&native_lookups==1&&vnode_calls==0&&!winner(1,"file"));
        CHECK(governed(actual)&&validate(actual)==1&&actual->hashed);
        if(res)dput(res);sumh_dh_abort(a);CHECK(!governed(actual));
        int r=finish_case();if(r)return r;
    }return 0;
}
static int pending_completion(void){
    for(int negative=0;negative<2;negative++)for(int alias=0;alias<2;alias++){
        fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0);
        struct dentry *d=request(3,"file"),*actual=alias?request(4,"file"):d;
        native_negative=negative;native_alias=alias?actual:NULL;change_add=a;native_completion=defer_change;
        struct dentry *res=sumh_dh_lookup(&fixture_inodes[1],d,7);
        CHECK(!IS_ERR(res)&&!change_status&&native_lookups==1&&native_lookup_flags==7);
        CHECK(governed(actual)&&validate(actual)==0);
        if(res)dput(res);flush_deferred();withdraw(a);int r=finish_case();if(r)return r;
    }return 0;
}
static int mapped_atomic_open(void){
    const unsigned flags[]={0,O_CREAT,O_CREAT|O_EXCL,O_TRUNC,O_CREAT|O_TRUNC};
    for(unsigned n=0;n<ARRAY_SIZE(flags);n++)for(int alias=0;alias<2;alias++){
        fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
        struct dentry *d=request(3,"file"),*actual=alias?request(4,"file"):d;d->hashed=true;
        if(alias)splice_result=actual;struct file f=new_file();
        int result=sumh_dh_atomic_open(&fixture_inodes[1],d,&f,flags[n],0600);
        CHECK(result==0&&native_opens==0&&native_create_count==0&&finish_calls==1);
        CHECK(vnode_calls==1&&sumh_vnode_is_ours(d_inode(actual))&&governed(actual)&&validate(actual)==1);
        CHECK(!rehash_errors&&!(f.f_mode&(FMODE_OPENED|FMODE_CREATED))&&f.f_path.dentry==(alias?actual:NULL));
        CHECK(!alias||actual->refs==3);
        if(alias)file_release(&f);withdraw(a);int r=finish_case();if(r)return r;
    }return 0;
}
static int selected_allocation_failure(void){
    for(int atomic=0;atomic<2;atomic++){
        fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
        struct dentry *d=request(3,"file");vnode_fail=1;struct file f=new_file();
        int result=atomic?sumh_dh_atomic_open(&fixture_inodes[1],d,&f,O_CREAT,0600):PTR_ERR(sumh_dh_lookup(&fixture_inodes[1],d,0));
        CHECK(result==-ENOMEM&&native_opens==0&&native_lookups==0&&native_create_count==0);
        CHECK(!d->hashed&&!d->d_inode);withdraw(a);int r=finish_case();if(r)return r;
    }return 0;
}
static int hidden_atomic_open(void){
    for(int su=0;su<2;su++){
        fixture_init();struct sumh_dh_claim *a;model_su_enabled=true;model_su_ino=501;model_su_visible=false;
        CHECK(prepared("/one/file",su?SUMH_DH_OWNER_SU:SUMH_DH_OWNER_LEGACY_HIDE,0,501,&a)==0&&committed(a,NULL)==0);
        struct dentry *d=request(3,"file");struct file f=new_file();
        CHECK(sumh_dh_atomic_open(&fixture_inodes[1],d,&f,O_CREAT|O_TRUNC,0600)==-ENOENT&&native_opens==0&&native_create_count==0);
        withdraw(a);int r=finish_case();if(r)return r;
    }return 0;
}
static int native_atomic_results(void){
    for(int pending=0;pending<2;pending++)for(int kind=0;kind<3;kind++)for(int alias=0;alias<2;alias++){
        fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0);
        if(!pending)CHECK(committed(a,NULL)==0);model_sees=false;
        struct dentry *d=request(3,"file"),*actual=alias?request(4,"file"):d;native_alias=alias&&kind?actual:NULL;native_result_kind=kind;
        struct file f=new_file();int result=sumh_dh_atomic_open(&fixture_inodes[1],d,&f,O_CREAT|O_TRUNC,0612);
        CHECK(result==0&&native_opens==1&&native_create_count==1&&native_open_flags==(O_CREAT|O_TRUNC)&&native_open_mode==0612&&finish_calls==0);
        /* NULL finish_no_open identifies the input even if an unused alias was configured. */
        struct dentry *candidate=kind?actual:d;
        CHECK(governed(candidate)&&validate(candidate)==1);
        CHECK((kind==1)==!!(f.f_mode&FMODE_OPENED));
        file_release(&f);if(pending)sumh_dh_abort(a);else withdraw(a);int r=finish_case();if(r)return r;
    }return 0;
}
static int native_atomic_error_sentinel(void){
    for(int result=0;result<2;result++){
        fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0);model_sees=false;
        struct dentry *d=request(3,"file");struct file f=new_file();native_result_kind=3;native_error=result?-EIO:0;
        CHECK(sumh_dh_atomic_open(&fixture_inodes[1],d,&f,O_CREAT,0640)==native_error&&native_opens==1);
        CHECK(f.f_path.dentry==DENTRY_NOT_SET&&!bad);sumh_dh_abort(a);int r=finish_case();if(r)return r;
    }return 0;
}
static int pending_atomic_completion(void){
    for(int kind=0;kind<3;kind++){
        fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0);
        struct dentry *d=request(3,"file"),*alias=request(4,"file");native_alias=kind?alias:NULL;native_result_kind=kind;
        change_add=a;native_completion=defer_change;struct file f=new_file();
        CHECK(sumh_dh_atomic_open(&fixture_inodes[1],d,&f,O_CREAT,0644)==0&&native_opens==1&&!change_status);
        struct dentry *actual=kind?alias:d;CHECK(governed(actual)&&validate(actual)==0);
        file_release(&f);flush_deferred();withdraw(a);int r=finish_case();if(r)return r;
    }return 0;
}
static int unrelated_atomic_open(void){
    fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    struct dentry *d=request(3,"other");struct file f=new_file();native_result_kind=1;
    CHECK(sumh_dh_atomic_open(&fixture_inodes[1],d,&f,O_CREAT|O_EXCL,0643)==0&&native_opens==1&&native_create_count==1);
    CHECK(!governed(d)&&d->hashed&&f.f_mode==FMODE_OPENED&&native_open_mode==0643);file_release(&f);withdraw(a);return finish_case();
}
static int stale_completion(void){
    fixture_init();struct sumh_dh_claim *a,*b;
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,17,501,&b)==0);
    struct dentry *d=request(3,"file");change_add=b;change_remove=a;during_vnode=vnode_changes;
    struct dentry *res=sumh_dh_lookup(&fixture_inodes[1],d,0);
    CHECK(IS_ERR(res)&&native_lookups==0&&!d->hashed);
    CHECK(!change_status&&winner(1,"file")==501&&last_vnode_source.dentry==&fixture_dentries[16]);
    flush_deferred();withdraw(b);return finish_case();
}
static int winner_revalidation(void){
    fixture_init();struct sumh_dh_claim *a,*b;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    struct dentry *d=request(3,"file");CHECK(sumh_dh_lookup(&fixture_inodes[1],d,0)==NULL&&validate(d)==1);
    struct sumh_dh_dop_meta *meta=sumh_dh_dop_lookup_locked(d);
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,17,501,&b)==0);
    change_add=b;change_remove=NULL;defer_change();
    CHECK(validate_meta(meta,d)==0);
    flush_deferred();withdraw(b);withdraw(a);return finish_case();
}
static int retired_native_completion(void){
    fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    struct dentry *d=request(3,"file");model_sees=false;change_remove=a;native_completion=defer_detach;
    struct dentry *res=sumh_dh_lookup(&fixture_inodes[1],d,0);
    CHECK(res==NULL&&native_lookups==1&&!d->hashed&&!governed(d));
    flush_deferred();return finish_case();
}
static int retire_after_publication(void){
    for(int alias=0;alias<2;alias++){
        fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
        struct dentry *d=request(3,"file"),*actual=alias?request(4,"file"):d;if(alias)splice_result=actual;
        change_remove=a;after_publication=published_detaches;struct dentry *res=sumh_dh_lookup(&fixture_inodes[1],d,0);
        CHECK(IS_ERR(res)&&!actual->hashed&&native_lookups==0);
        flush_deferred();int r=finish_case();if(r)return r;
    }return 0;
}
static int retire_before_publication(void){
    for(int atomic=0;atomic<2;atomic++)for(int alias=0;alias<2;alias++){
        fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
        struct dentry *d=request(3,"file"),*actual=alias?request(4,"file"):d;if(alias)splice_result=actual;
        change_remove=a;before_publication=published_detaches;struct file f=new_file();
        int result=atomic?sumh_dh_atomic_open(&fixture_inodes[1],d,&f,O_CREAT,0600):PTR_ERR(sumh_dh_lookup(&fixture_inodes[1],d,0));
        CHECK(result<0&&!actual->hashed&&native_lookups==0&&native_opens==0);
        CHECK(!governed(actual));flush_deferred();CHECK(actual->refs==1);
        int r=finish_case();if(r)return r;
    }return 0;
}
static int restored_shadow_callback(void){
    fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    withdraw(a);struct dentry *d=request(3,"other");
    struct dentry *res=sumh_dh_lookup(&fixture_inodes[1],d,9);
    CHECK(res==NULL&&native_lookups==1&&native_lookup_flags==9&&d_inode(d)==&fixture_inodes[18]);
    return finish_case();
}
static int projection_splice_error(void){
    fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    struct dentry *d=request(3,"file");struct file f=new_file();splice_error=-EIO;
    CHECK(sumh_dh_atomic_open(&fixture_inodes[1],d,&f,O_CREAT,0600)==-EIO&&native_opens==0&&native_create_count==0&&modeled_vnodes[0].refs==0);
    withdraw(a);return finish_case();
}
static int wrong_alias_name(void){
    for(int atomic=0;atomic<2;atomic++){
        fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0);model_sees=false;
        struct dentry *d=request(3,"file"),*alias=request(4,"file");native_alias=alias;native_completion=native_rename;native_result_kind=1;
        struct file f=new_file();struct dentry *res=NULL;
        if(atomic)CHECK(sumh_dh_atomic_open(&fixture_inodes[1],d,&f,O_CREAT,0600)==0);else res=sumh_dh_lookup(&fixture_inodes[1],d,0);
        CHECK(!alias->hashed&&!governed(alias)&&!bad);
        if(res)dput(res);if(atomic)file_release(&f);sumh_dh_abort(a);int r=finish_case();if(r)return r;
    }return 0;
}
static int native_metadata_failure(void){
    for(int atomic=0;atomic<2;atomic++){
        fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0);model_sees=false;
        struct dentry *d=request(3,"file");struct file f=new_file();native_result_kind=1;fail_at=allocations+1;
        struct dentry *res=NULL;if(atomic)CHECK(sumh_dh_atomic_open(&fixture_inodes[1],d,&f,O_CREAT,0600)==0);else res=sumh_dh_lookup(&fixture_inodes[1],d,0);fail_at=0;
        CHECK(!IS_ERR(res)&&!d->hashed&&!governed(d)&&d_inode(d)==&fixture_inodes[18]);
        if(atomic){CHECK(f.f_mode==FMODE_OPENED);file_release(&f);}sumh_dh_abort(a);int r=finish_case();if(r)return r;
    }return 0;
}
static int observer_transition_failed_materialize(void){
    for(int no_memory=0;no_memory<2;no_memory++){
        fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
        model_sees=false;native_negative=true;struct dentry *d=request(3,"file");
        CHECK(sumh_dh_lookup(&fixture_inodes[1],d,0)==NULL&&d->hashed&&!d_inode(d)&&governed(d));
        /* VFS accepts the native negative under the previous app profile.
         * A live allowlist/profile update changes policy before atomic entry. */
        CHECK(validate(d)==1&&d->refs==2);model_sees=true;vnode_fail=no_memory;struct file f=new_file();
        int result=sumh_dh_atomic_open(&fixture_inodes[1],d,&f,O_CREAT,0600);
        CHECK(result==(no_memory?-ENOMEM:-EBUSY)&&!d->hashed&&native_opens==0&&native_create_count==0);
        flush_work(&sumh_dh_reap_dops_work);
        CHECK(!governed(d)&&d->refs==1&&a->state==DH_ATTACHED&&winner(1,"file")==501);
        CHECK(native_lookups==1);withdraw(a);int r=finish_case();if(r)return r;
    }return 0;
}
static int original_revalidate_calls;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,13,0)
static int original_revalidate(struct inode *i,const struct qstr *n,struct dentry *d,unsigned flags){(void)i;(void)n;(void)d;original_revalidate_calls++;return flags==LOOKUP_RCU?-ECHILD:1;}
#else
static int original_revalidate(struct dentry *d,unsigned flags){(void)d;original_revalidate_calls++;return flags==LOOKUP_RCU?-ECHILD:1;}
#endif
static int original_dop_preserved(void){
    fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0);
    struct dentry_operations original={.d_revalidate=original_revalidate,.unused=77};
    struct dentry *d=request(3,"file");d->d_op=&original;d->d_flags=DCACHE_OP_REVALIDATE|DCACHE_OP_DELETE;original_revalidate_calls=0;
    CHECK(sumh_dh_lookup(&fixture_inodes[1],d,0)==NULL&&d->d_op!=&original&&d->d_op->unused==77);
    CHECK(validate(d)==-ECHILD&&original_revalidate_calls==1);
    sumh_dh_abort(a);CHECK(d->d_op==&original&&d->d_flags==(DCACHE_OP_REVALIDATE|DCACHE_OP_DELETE));return finish_case();
}
#if CACHE_HAS_PROJECTION
static int payload_pin_prevents_reuse(void){
    fixture_init();struct sumh_dh_claim *a,*b,*temp;
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    struct dentry *d=request(3,"file");CHECK(sumh_dh_lookup(&fixture_inodes[1],d,0)==NULL&&validate(d)==1);
    struct sumh_dh_payload *old=a->payload;int old_slot=allocation_slot(old);
    CHECK(refcount_read(&old->refs)==2);
    struct sumh_dh_dop_meta *meta=sumh_dh_dop_lookup_locked(d);
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,17,501,&b)==0);
    change_add=b;change_remove=a;defer_change();sumh_dh_claim_put(a);change_remove=NULL;sumh_dh_drain_releases();
    CHECK(used[old_slot]&&refcount_read(&old->refs)==1&&fixture_dentries[16].refs==2);
    for(int i=0;i<128;i++){
        CHECK(prepared("/two/reuse",SUMH_DH_OWNER_MAPPING,17,600+i,&temp)==0);
        CHECK(temp->payload!=old&&used[old_slot]);sumh_dh_abort(temp);
    }
    CHECK(validate_meta(meta,d)==0);flush_deferred();
    CHECK(!used[old_slot]&&!governed(d)&&fixture_dentries[16].refs==1);
    withdraw(b);return finish_case();
}
static int capture_pin_survives_detach(void){
    fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    struct sumh_dh_projection captured;struct sumh_dh_payload *old=a->payload;int slot=allocation_slot(old);
    CHECK(sumh_dh_capture_projection(a->slot->dir,&fixture_inodes[1],"file",4,&captured));
    CHECK(captured.payload==old&&refcount_read(&old->refs)==2);
    withdraw(a);sumh_dh_drain_releases();CHECK(used[slot]&&refcount_read(&old->refs)==1&&fixture_dentries[16].refs==2);
    sumh_dh_payload_put(captured.payload);CHECK(!used[slot]&&fixture_dentries[16].refs==1);return finish_case();
}
static int immutable_meta_reuse(void){
    fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    struct dentry *d=request(3,"file");CHECK(sumh_dh_lookup(&fixture_inodes[1],d,0)==NULL);
    struct sumh_dh_dop_meta *meta=sumh_dh_dop_lookup_locked(d);struct sumh_dh_payload *payload=meta->payload;
    struct sumh_dh_projection p={.payload=payload,.hidden=false};unsigned refs=refcount_read(&payload->refs);
    CHECK(sumh_dh_set_dentry_ops(a->slot->dir,d,&p)==0&&refcount_read(&payload->refs)==refs);
    CHECK(sumh_dh_set_dentry_ops(a->slot->dir,d,NULL)==-EBUSY&&meta->projected&&meta->payload==payload&&!meta->synthetic_negative);
    CHECK(refcount_read(&payload->refs)==refs&&validate(d)==1);withdraw(a);return finish_case();
}
#endif
"""

SCENARIOS = [
    "first_publication", "transparent_prepared", "pending_completion", "mapped_atomic_open",
    "selected_allocation_failure", "hidden_atomic_open", "native_atomic_results",
    "native_atomic_error_sentinel", "pending_atomic_completion", "unrelated_atomic_open", "stale_completion",
    "winner_revalidation", "retired_native_completion", "retire_after_publication", "retire_before_publication",
    "restored_shadow_callback", "projection_splice_error",
    "wrong_alias_name", "native_metadata_failure", "observer_transition_failed_materialize", "original_dop_preserved",
]

IDENTITY_SCENARIOS = ["payload_pin_prevents_reuse", "capture_pin_survives_detach", "immutable_meta_reuse"]

OLD_FAILURES = {
    "first_publication": "inode_lock_calls==1&&inode_unlock_calls==1&&first_native_complete&&first_invalidation>first_native_complete",
    "transparent_prepared": "governed(actual)&&validate(actual)==1&&actual->hashed",
    "pending_completion": "governed(actual)&&validate(actual)==0",
    "mapped_atomic_open": "result==0&&native_opens==0&&native_create_count==0&&finish_calls==1",
    "selected_allocation_failure": "result==-ENOMEM&&native_opens==0&&native_lookups==0&&native_create_count==0",
    "stale_completion": "IS_ERR(res)&&native_lookups==0&&!d->hashed",
    "winner_revalidation": "validate_meta(meta,d)==0",
    "wrong_alias_name": "!alias->hashed&&!governed(alias)&&!bad",
    "retire_before_publication": "result<0&&!actual->hashed&&native_lookups==0&&native_opens==0",
    "restored_shadow_callback": "res==NULL&&native_lookups==1&&native_lookup_flags==9&&d_inode(d)==&fixture_inodes[18]",
}


def main(scenario=None):
    if scenario:
        return f"\nint main(void){{return {scenario}();}}\n"
    return "\nint main(void){int r;" + "".join(f"if((r={name}()))return r;" for name in SCENARIOS) + "\n#if CACHE_HAS_PROJECTION\n" + "".join(f"if((r={name}()))return r;" for name in IDENTITY_SCENARIOS) + "\n#endif\nreturn 0;}\n"
