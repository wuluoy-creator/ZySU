"""Receipt ordering, preparation failures and finite lifetime scenarios."""

HELPERS = r"""
static int prepared(const char *path,enum sumh_dh_owner_kind kind,int source,unsigned long ino,struct sumh_dh_claim **out){
    struct path p=fixture_path(source),observed={0};
    struct sumh_dh_spec s={.kind=kind,.source=source>=16?&p:NULL,.v_ino=ino,.flags=kind==SUMH_DH_OWNER_SU?SUMH_VNODE_F_SU:0};
    int r=sumh_dh_prepare_path(path,&s,out,&observed);
    if(observed.dentry)path_put(&observed);return r;
}
static int committed(struct sumh_dh_claim *add,struct sumh_dh_claim *remove){
    struct sumh_dh_change change;struct sumh_dh_retire retired;
    int r=sumh_dh_change_begin(add,remove,&change);if(r)return r;
    sumh_dh_retire_init(&retired,false);
    int a=allocations,p=path_calls,d=dentry_puts,i=inode_puts,w=barrier_count+tasks_syncs+rcu_syncs+srcu_syncs;
    config_depth++;sumh_dh_change_commit(&change,&retired);config_depth--;
    if(a!=allocations||p!=path_calls||d!=dentry_puts||i!=inode_puts||w!=barrier_count+tasks_syncs+rcu_syncs+srcu_syncs)BAD();
    sumh_dh_change_end();sumh_dh_retire_finish(&retired);return 0;
}
static void withdraw(struct sumh_dh_claim *c){
    struct sumh_dh_retire retired;sumh_dh_retire_init(&retired,false);
    sumh_dh_detach(c,&retired);sumh_dh_retire_finish(&retired);sumh_dh_claim_put(c);
}
static unsigned long winner(int parent,const char *name){
    struct sumh_dh_iop *m=sumh_dh_iop_of(&fixture_inodes[parent]);
    struct sumh_dh_view v;unsigned long result=0;
    rcu_read_lock();if(m&&sumh_dh_read_view(sumh_dh_find_child(m->dir,name,(u16)strlen(name),full_name_hash(&fixture_inodes[parent],name,(unsigned)strlen(name))),&v))result=v.v_ino;rcu_read_unlock();return result;
}
static int finish_case(void){
    cache_count=0;sumh_dirhijack_exit();sumh_dh_drain_releases();sumh_fop_override_exit();sumh_sop_shadow_exit();
    CHECK(!bad&&!live_allocations&&!callback_count&&!sumh_dh_release_work.pending);
    CHECK(fixture_sb.s_active==1&&self_module.refs==1&&foreign_module.refs==1&&fixture_mount.refs==1);
    for(int i=0;i<32;i++)CHECK(fixture_inodes[i].refs==1&&fixture_dentries[i].refs==1);
    return 0;
}
"""

CASES = r"""
static int exact_parent_and_order(void){
    fixture_init();struct sumh_dh_claim *a,*b,*c;
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0);
    CHECK(!winner(1,"file")&&!sumh_dirhijack_emits(&fixture_inodes[1],"file",4));
    CHECK(committed(a,NULL)==0&&winner(1,"file")==501);
    CHECK(prepared("/alias/file",SUMH_DH_OWNER_MAPPING,17,502,&b)==0&&a->slot==b->slot);
    CHECK(committed(b,NULL)==0&&winner(1,"file")==502);
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,503,&c)==0);
    CHECK(committed(c,NULL)==0&&winner(1,"file")==503);
    withdraw(c);CHECK(c->state==DH_DETACHED&&winner(1,"file")==502);withdraw(b);CHECK(winner(1,"file")==501);
    CHECK(prepared("/two/file",SUMH_DH_OWNER_MAPPING,17,504,&b)==0&&committed(b,NULL)==0);
    native_path("/one",fixture_path(2),0);int lookups=path_calls;
    withdraw(a);CHECK(path_calls==lookups&&!winner(1,"file")&&winner(2,"file")==504);
    sumh_dh_drain_releases();CHECK(winner(2,"file")==504);
    withdraw(b);return finish_case();
}
static int replacement_and_managed_overlay(void){
    fixture_init();struct sumh_dh_claim *a,*b,*c;struct sumh_hide_binding *h1,*h2;struct path p=fixture_path(1);
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    CHECK(sumh_dirhijack_hide_get(&p,"file",&h1)==0&&sumh_dirhijack_hide_get(&p,"file",&h2)==0);
    CHECK(sumh_dirhijack_hidden(&fixture_inodes[1],"file",4)&&a->slot->hide_users==2);
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,17,502,&b)==0&&committed(b,a)==0);
    CHECK(a->state==DH_DETACHED&&winner(1,"file")==502&&b->slot->hide_users==2);
    sumh_dh_claim_put(a);sumh_dh_drain_releases();CHECK(winner(1,"file")==502);
    sumh_dirhijack_hide_put(h2);CHECK(sumh_dirhijack_hidden(&fixture_inodes[1],"file",4));
    sumh_dirhijack_hide_put(h1);CHECK(!sumh_dirhijack_hidden(&fixture_inodes[1],"file",4)&&winner(1,"file")==502);
    CHECK(prepared("/one/file",SUMH_DH_OWNER_LEGACY_HIDE,0,0,&c)==0&&committed(c,NULL)==0);
    CHECK(sumh_dirhijack_hidden(&fixture_inodes[1],"file",4));
    withdraw(c);CHECK(winner(1,"file")==502&&!sumh_dirhijack_hidden(&fixture_inodes[1],"file",4));
    withdraw(b);CHECK(!winner(1,"file"));return finish_case();
}
static void make_parent_appear(void){native_path("/one/new",fixture_path(3),0);}
static int topology_and_absence(void){
    fixture_init();struct sumh_dh_claim *a,*b,*c;struct path p={0},s=fixture_path(16);
    struct sumh_dh_spec spec={.kind=SUMH_DH_OWNER_MAPPING,.source=&s,.v_ino=501};
    CHECK(sumh_dh_prepare_path("/one/top/a",&spec,&a,&p)==0&&!p.dentry&&a->kind==SUMH_DH_OWNER_VTOPO);
    CHECK(!strcmp(a->payload->vpath,"/one/top")&&committed(a,NULL)==0);
    CHECK(prepared("/alias/top/b",SUMH_DH_OWNER_MAPPING,17,502,&b)==0&&committed(b,NULL)==0);
    CHECK(prepared("/one/top/c",SUMH_DH_OWNER_MAPPING,16,503,&c)==0&&committed(c,NULL)==0);
    CHECK(a->slot==b->slot&&b->slot==c->slot&&a->slot->winner==c->payload);
    withdraw(c);CHECK(a->slot->winner==b->payload&&!strcmp(b->payload->vpath,"/alias/top"));
    withdraw(b);CHECK(a->slot->winner==a->payload);
    withdraw(a);CHECK(!sumh_dh_iop_of(&fixture_inodes[1]));sumh_dh_drain_releases();
    after_path=make_parent_appear;
    CHECK(sumh_dh_prepare_path("/one/new/file",&spec,&a,&p)==0&&p.dentry==&fixture_dentries[3]);
    CHECK(a->kind==SUMH_DH_OWNER_MAPPING&&!a->payload->vpath&&a->payload->source.dentry==s.dentry);
    path_put(&p);sumh_dh_abort(a);
    const enum sumh_dh_owner_kind denied[]={SUMH_DH_OWNER_SHADOW,SUMH_DH_OWNER_LEGACY_HIDE,SUMH_DH_OWNER_SU};
    for(unsigned i=0;i<ARRAY_SIZE(denied);i++){a=(void *)1;CHECK(prepared("/missing/file",denied[i],16,501,&a)==-ENOENT&&!a);}
    native_path("/error",fixture_path(0),-EACCES);a=(void *)1;
    CHECK(prepared("/error/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==-EACCES&&!a);
    return finish_case();
}
static int failure_and_capacity(void){
    fixture_init();struct sumh_dh_claim *a,*b;struct sumh_dh_change change;
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    int start=allocations;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,17,502,&b)==0);int count=allocations-start;sumh_dh_abort(b);
    for(int n=1;n<=count;n++){int live=live_allocations;fail_at=allocations+n;b=(void *)1;int r=prepared("/one/file",SUMH_DH_OWNER_MAPPING,17,502,&b);fail_at=0;CHECK(r==-ENOMEM&&!b&&winner(1,"file")==501&&live_allocations==live);}
    unsigned save=a->slot->prepared;a->slot->prepared=UINT_MAX;
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,17,502,&b)==-EOVERFLOW&&!b);a->slot->prepared=save;
    unsigned refs=refcount_read(&a->slot->refs);refcount_set(&a->slot->refs,INT_MAX-4);
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,17,502,&b)==-EOVERFLOW&&!b&&refcount_read(&a->slot->refs)==INT_MAX-4);refcount_set(&a->slot->refs,refs);
    refs=refcount_read(&a->slot->dir->refs);refcount_set(&a->slot->dir->refs,INT_MAX-4);
    CHECK(prepared("/one/other",SUMH_DH_OWNER_MAPPING,17,502,&b)==-EOVERFLOW&&!b);refcount_set(&a->slot->dir->refs,refs);
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,17,502,&b)==0);save=a->slot->attached;a->slot->attached=UINT_MAX;
    CHECK(sumh_dh_change_begin(b,a,&change)==-EOVERFLOW&&!sumh_dh_lock.held&&b->state==DH_PREPARED);a->slot->attached=save;
    sumh_dh_abort(b);CHECK(winner(1,"file")==501);
    struct sumh_sop_meta *sop=sumh_sop_lookup_locked(&fixture_sb);save=sop->dh_clients;sop->dh_clients=UINT_MAX;
    CHECK(prepared("/two/file",SUMH_DH_OWNER_MAPPING,17,502,&b)==-EOVERFLOW&&!b);sop->dh_clients=save;
    withdraw(a);return finish_case();
}
static int clear_candidates_and_su(void){
    fixture_init();struct sumh_dh_claim *a,*b,*su,*fresh,*pending_su;struct sumh_dh_change change;
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    CHECK(prepared("/one/pending",SUMH_DH_OWNER_MAPPING,17,502,&b)==0);
    CHECK(prepared("/one/su",SUMH_DH_OWNER_SU,0,600,&su)==0&&committed(su,NULL)==0);
    CHECK(prepared("/one/su",SUMH_DH_OWNER_MAPPING,17,503,&fresh)==0);
    CHECK(sumh_dh_change_begin(fresh,NULL,&change)==-EPERM&&!sumh_dh_lock.held);sumh_dh_abort(fresh);
    CHECK(prepared("/one/file",SUMH_DH_OWNER_SU,0,601,&fresh)==0);
    CHECK(sumh_dh_change_begin(fresh,NULL,&change)==-EEXIST&&!sumh_dh_lock.held);sumh_dh_abort(fresh);
    CHECK(prepared("/two/pending-su",SUMH_DH_OWNER_SU,0,602,&pending_su)==0);
    sumh_dirhijack_clear();
    CHECK(a->state==DH_DETACHED&&b->state==DH_INVALIDATED&&su->state==DH_ATTACHED);
    CHECK(sumh_dh_change_begin(b,NULL,&change)==-EAGAIN&&!sumh_dh_lock.held);
    CHECK(pending_su->state==DH_INVALIDATED&&sumh_dh_change_begin(pending_su,NULL,&change)==-EAGAIN);sumh_dh_abort(pending_su);
    sumh_dh_claim_put(a);sumh_dh_drain_releases();
    CHECK(prepared("/two/new-su",SUMH_DH_OWNER_SU,0,601,&fresh)==0&&committed(fresh,NULL)==0);
    CHECK(su->state==DH_ATTACHED&&fresh->state==DH_ATTACHED);
    sumh_dh_abort(b);
    CHECK(prepared("/one/pending",SUMH_DH_OWNER_MAPPING,17,502,&b)==0);
    sumh_dirhijack_stop_new();sumh_dirhijack_clear();
    CHECK(sumh_dh_change_begin(b,NULL,&change)==-ESHUTDOWN&&!sumh_dh_lock.held);
    CHECK(b->state==DH_INVALIDATED&&su->state==DH_DETACHED&&fresh->state==DH_DETACHED);
    sumh_dh_abort(b);sumh_dh_claim_put(su);sumh_dh_claim_put(fresh);return finish_case();
}
static int late_release_and_reap(void){
    fixture_init();struct sumh_dh_claim *a,*b;struct sumh_dh_retire retired;
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    struct sumh_dh_dir *old=a->slot->dir;
    sumh_dh_retire_init(&retired,false);sumh_dh_detach(a,&retired);sumh_dh_retire_finish(&retired);
    CHECK(old->retiring&&atomic_read(&sumh_sop_live)==1&&!vnode_calls);
    sumh_dh_claim_put(a);CHECK(callback_count&&sumh_sop_lookup_locked(&fixture_sb)->dh_clients==1);
    sumh_dh_drain_releases();
    CHECK(atomic_read(&sumh_sop_live)==0&&fixture_sb.s_op==&fixture_sop&&self_module.refs==1);
    CHECK(fixture_sb.s_active==2&&fixture_inodes[1].refs==2); /* independent FOP cache */
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,502,&a)==0&&committed(a,NULL)==0);
    old=a->slot->dir;sumh_dh_retire_init(&retired,false);sumh_dh_detach(a,&retired);sumh_dh_retire_finish(&retired);sumh_dh_claim_put(a);
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,17,503,&b)==0&&committed(b,NULL)==0);
    CHECK(old!=b->slot->dir);sumh_dh_drain_releases();
    CHECK(winner(1,"file")==503&&fixture_inodes[1].i_op==&b->slot->dir->iop_meta->fake_iop);
    withdraw(b);sumh_dh_drain_releases();
    CHECK(prepared("/two/abort",SUMH_DH_OWNER_SHADOW,16,504,&a)==0);sumh_dh_abort(a);
    CHECK(atomic_read(&sumh_sop_live)==0&&!sumh_dh_iop_of(&fixture_inodes[2]));
    return finish_case();
}
static int bulk_and_reuse(void){
    fixture_init();struct sumh_dh_claim *claims[20],*pending[20];struct sumh_dh_retire retired;static char paths[20][32];
    for(int i=0;i<20;i++){memcpy(paths[i],"/one/itemA",11);paths[i][9]=(char)('A'+i);CHECK(prepared(paths[i],SUMH_DH_OWNER_MAPPING,16,(unsigned long)(500+i),&claims[i])==0&&committed(claims[i],NULL)==0);CHECK(prepared(paths[i],SUMH_DH_OWNER_MAPPING,17,700,&pending[i])==0);}
    sumh_dh_retire_init(&retired,true);int before=cache_lookups;
    for(int i=0;i<20;i++)sumh_dh_detach(claims[i],&retired);
    CHECK(retired.count==0&&!list_empty(&retired.invalidations));sumh_dh_retire_finish(&retired);
    CHECK(cache_lookups-before==20);
    for(int i=0;i<20;i++){CHECK(claims[i]->state==DH_DETACHED&&pending[i]->state==DH_PREPARED);sumh_dh_claim_put(claims[i]);sumh_dh_abort(pending[i]);}
    sumh_dh_drain_releases();
    for(int i=0;i<1000;i++){struct sumh_dh_claim *a;CHECK(prepared(i%2?"/one/reuse":"/two/reuse",SUMH_DH_OWNER_MAPPING,16+i%2,(unsigned long)(1000+i),&a)==0);CHECK(committed(a,NULL)==0);withdraw(a);sumh_dh_drain_releases();CHECK(!sumh_dh_iop_of(&fixture_inodes[1])&&!sumh_dh_iop_of(&fixture_inodes[2]));}
    return finish_case();
}
int main(void){
    int r;
    if((r=exact_parent_and_order()))return r;
    if((r=replacement_and_managed_overlay()))return r;
    if((r=topology_and_absence()))return r;
    if((r=failure_and_capacity()))return r;
    if((r=clear_candidates_and_su()))return r;
    if((r=late_release_and_reap()))return r;
    if((r=bulk_and_reuse()))return r;
    return 0;
}
"""
