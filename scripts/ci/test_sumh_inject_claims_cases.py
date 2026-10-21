"""Behavioral invariants for actual injection claim/store functions."""

SCENARIOS = (
    "shared-key-and-inode", "distinct-keys-one-inode", "same-key-rebound-inode",
    "bound-unbound-coexistence", "two-alias-single-observation", "equal-canonical-alias",
    "missing-policies-and-native-errors", "invalid-native-object", "preparation-allocation-failures",
    "fop-errors-and-pin-release", "alias-render-errors", "alias-allocation-failures",
    "group-inode-overflow", "group-key-overflow", "group-row-headroom",
    "spares-released-before-retirement", "withdrawal-before-callback", "old-worker-new-owner",
    "worker-arrival-third-queue", "one-thousand-reuse-cycles", "multi-bucket-collisions",
)

CASES = r"""
static struct sumh_inject_claim *prepare(const char *key){struct sumh_inject_claim *c=NULL;int r=sumh_inject_prepare_directory(key,SUMH_INJECT_MISSING_UNBOUND,&c);if(r)bad++;return c;}
static int attach(struct sumh_inject_claim **c,unsigned n){mutex_lock(&sumh_config_mutex);int r=sumh_inject_attach_locked(c,n);mutex_unlock(&sumh_config_mutex);for(unsigned i=0;i<n;i++)if(!r)sumh_inject_finish_prepare(c[i]);return r;}
static void detach(struct sumh_inject_claim *c){mutex_lock(&sumh_config_mutex);sumh_inject_detach_locked(c);mutex_unlock(&sumh_config_mutex);}
static bool marked(struct inode *i){return test_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT,&i->i_mapping->flags);}
static int owners(void){return table_count(sumh_inject_dirs);}
static int bindings(void){return table_count(sumh_inject_inodes);}
static u64 logical(void){return (u64)(unsigned int)sumh_rule_count*10000ULL+(u64)owners()*1000ULL+(u64)bindings()*100ULL+marked(&parent_i)*10+marked(&visible_i);}
static void release(struct sumh_inject_claim *c){detach(c);sumh_inject_destroy(c);}
static int sharing(void){
    setup(S_IFREG);struct sumh_inject_claim *c[2]={prepare("/visible"),prepare("/visible")};
    CHECK(c[0]&&c[1]&&attach(c,2)==0);CHECK(owners()==1&&bindings()==1&&sumh_rule_count==1);
    CHECK(c[0]->key==c[1]->key&&c[0]->key->users==2&&c[0]->binding==c[1]->binding&&c[0]->binding->users==2);
    CHECK(parent_i.refs==2&&parent_i.i_sb->s_active==2&&marked(&parent_i));
    release(c[0]);CHECK(owners()==1&&bindings()==1&&marked(&parent_i));
    release(c[1]);CHECK(!owners()&&!bindings()&&!marked(&parent_i)&&!sumh_rule_count);cleanup();CHECK(clean());

    setup(S_IFREG);path_override("/alias",(struct path){&visible_m,&parent_d},0);
    c[0]=prepare("/visible");c[1]=prepare("/alias");CHECK(attach(c,2)==0);
    CHECK(owners()==2&&bindings()==1&&c[0]->binding->users==2&&sumh_rule_count==2);release(c[0]);CHECK(marked(&parent_i));release(c[1]);cleanup();CHECK(clean());

    setup(S_IFREG);c[0]=prepare("/visible");CHECK(attach(c,1)==0);
    path_override("/visible",(struct path){&visible_m,&visible_d},0);c[1]=prepare("/visible");CHECK(attach(&c[1],1)==0);
    CHECK(owners()==1&&bindings()==2&&c[0]->key==c[1]->key&&marked(&parent_i)&&marked(&visible_i));
    release(c[0]);CHECK(!marked(&parent_i)&&marked(&visible_i)&&owners()==1&&bindings()==1);release(c[1]);cleanup();CHECK(clean());

    for(int order=0;order<2;order++){
        setup(S_IFREG);path_override("/visible",(struct path){0},-ENOENT);c[order]=prepare("/visible");
        path_override("/visible",(struct path){&visible_m,&parent_d},0);c[1-order]=prepare("/visible");
        CHECK(attach(c,2)==0);CHECK(c[order]->binding==NULL&&owners()==1&&bindings()==1);
        release(c[1-order]);CHECK(!marked(&parent_i)&&owners()==1&&!bindings());release(c[order]);cleanup();CHECK(clean());
    }
    return 0;
}
static int aliases(void){
    setup(S_IFREG);struct sumh_inject_claim *c[2];char *resolved=NULL;parent_d.canonical="/canonical";
    path_override("/canonical",(struct path){&visible_m,&visible_d},0);
    int before=path_lookups;CHECK(sumh_inject_prepare_merge("/visible",&resolved,c)==0);
    CHECK(path_lookups==before+1&&resolved&&!strcmp(resolved,"/canonical")&&c[0]&&c[1]);
    CHECK(c[0]->spare_binding->inode==&parent_i&&c[1]->spare_binding->inode==&parent_i);
    CHECK(attach(c,2)==0&&owners()==2&&bindings()==1&&c[0]->binding->users==2&&!marked(&visible_i));
    release(c[0]);release(c[1]);kfree(resolved);cleanup();CHECK(clean());
    setup(S_IFREG);CHECK(sumh_inject_prepare_merge("/visible",&resolved,c)==0);
    CHECK(c[0]&&!c[1]&&!resolved&&attach(c,2)==0&&owners()==1);release(c[0]);cleanup();CHECK(clean());return 0;
}
static int missing_errors(void){
    setup(S_IFREG);struct sumh_inject_claim *c=NULL,*pair[2];char *resolved=NULL;
    CHECK(sumh_inject_prepare_directory("/missing",SUMH_INJECT_MISSING_NONE,&c)==0&&!c&&!live);
    CHECK(sumh_inject_prepare_directory("/missing",SUMH_INJECT_MISSING_UNBOUND,&c)==0&&c&&!c->spare_binding);
    CHECK(attach(&c,1)==0&&owners()==1&&!bindings());release(c);cleanup();CHECK(clean());
    setup(S_IFREG);CHECK(sumh_inject_prepare_merge("/missing",&resolved,pair)==0);
    CHECK(pair[0]&&!pair[1]&&!resolved&&!pair[0]->spare_binding&&attach(pair,2)==0);release(pair[0]);cleanup();CHECK(clean());
    const int errors[]={-ENOTDIR,-EACCES,-EIO,-ENOMEM,-ELOOP,-ESTALE};
    for(int i=0;i<6;i++)for(int policy=0;policy<2;policy++){
        setup(S_IFREG);path_error=errors[i];CHECK(sumh_inject_prepare_directory("/visible",policy,&c)==errors[i]&&!c);
        CHECK(sumh_inject_prepare_merge("/visible",&resolved,pair)==errors[i]&&!pair[0]&&!pair[1]&&!resolved);CHECK(!owners()&&!fop_calls);cleanup();CHECK(clean());
    }
    for(int i=0;i<7;i++){
        setup(S_IFREG);int expected=-EINVAL;
        switch(i){case 0:parent_i.i_mode=S_IFREG;expected=-ENOTDIR;break;case 1:parent_d.inode=NULL;expected=-ENOTDIR;break;
        case 2:parent_i.synthetic=true;expected=-EOPNOTSUPP;break;case 3:parent_i.i_mapping=NULL;break;case 4:parent_i.i_sb=NULL;break;
        case 5:native_supers[4].s_flags=0;expected=-ESHUTDOWN;break;case 6:native_supers[4].s_active=0;expected=-ESHUTDOWN;break;}
        CHECK(sumh_inject_prepare_directory("/visible",SUMH_INJECT_MISSING_UNBOUND,&c)==expected&&!c);cleanup();CHECK(clean());
    }return 0;
}
static int failures(void){
    setup(S_IFREG);struct sumh_inject_claim *c=prepare("/visible");int n=allocs;sumh_inject_destroy(c);cleanup();CHECK(clean());
    for(int i=1;i<=n;i++){setup(S_IFREG);fail_at=i;c=NULL;CHECK(sumh_inject_prepare_directory("/visible",SUMH_INJECT_MISSING_NONE,&c)==-ENOMEM&&!c);CHECK(!owners()&&!bindings());cleanup();CHECK(clean());}
    const int errors[]={-EOPNOTSUPP,-ENOMEM,-EAGAIN,-ESHUTDOWN};
    for(int i=0;i<4;i++){setup(S_IFREG);fop_error=errors[i];c=NULL;CHECK(sumh_inject_prepare_directory("/visible",SUMH_INJECT_MISSING_NONE,&c)==errors[i]&&!c);CHECK(sb_gets==1&&sb_puts==1&&inode_gets==1&&inode_puts==1);cleanup();CHECK(clean());}
    struct sumh_inject_claim *pair[2];char *resolved=NULL;
    for(int i=0;i<3;i++){setup(S_IFREG);if(i<2)render_error=i?-ENOMEM:-EIO;else invalid_render=true;
        CHECK(sumh_inject_prepare_merge("/visible",&resolved,pair)==(i<2?render_error:-EINVAL));CHECK(!pair[0]&&!pair[1]&&!resolved&&!fop_calls);cleanup();CHECK(clean());}
    setup(S_IFREG);parent_d.canonical="/alias";CHECK(sumh_inject_prepare_merge("/visible",&resolved,pair)==0);n=allocs;
    sumh_inject_destroy(pair[0]);sumh_inject_destroy(pair[1]);kfree(resolved);cleanup();CHECK(clean());
    for(int i=1;i<=n;i++){setup(S_IFREG);parent_d.canonical="/alias";fail_at=i;CHECK(sumh_inject_prepare_merge("/visible",&resolved,pair)==-ENOMEM);CHECK(!pair[0]&&!pair[1]&&!resolved&&!owners()&&!bindings());cleanup();CHECK(clean());}
    return 0;
}
static int overflow(void){
    for(int variant=0;variant<4;variant++){
        setup(S_IFREG);struct sumh_inject_claim *old=prepare("/visible");CHECK(attach(&old,1)==0);
        path_override("/a",(struct path){&visible_m,&parent_d},0);path_override("/b",(struct path){&visible_m,&parent_d},0);
        struct sumh_inject_claim *c[2]={prepare(variant==1?"/visible":"/a"),prepare(variant==1?"/visible":"/b")};
        if(variant==0)old->binding->users=UINT_MAX-1;
        if(variant==1)old->key->users=UINT_MAX-1;
        if(variant>=2)sumh_rule_count=INT_MAX-(variant==2?1:0);
        u64 before=logical();int alloc_before=allocs;CHECK(attach(c,2)==-EOVERFLOW);CHECK(logical()==before&&allocs==alloc_before);
        CHECK(!c[0]->attached&&!c[1]->attached&&owners()==1&&bindings()==1&&marked(&parent_i));
        old->binding->users=old->key->users=1;sumh_rule_count=1;
        sumh_inject_destroy(c[0]);sumh_inject_destroy(c[1]);release(old);cleanup();CHECK(clean());
    }return 0;
}
static struct sumh_entry *arriving_entry;
static void callback_during_release(void){callback_depth++;sumh_entry_free_rcu(&arriving_entry->rcu);callback_depth--;}
static int retirement(void){
    setup(S_IFREG);struct sumh_inject_claim *a=prepare("/visible");CHECK(attach(&a,1)==0);int live_before=live;
    struct sumh_inject_claim *b=prepare("/visible");CHECK(live>live_before+1);CHECK(attach(&b,1)==0);CHECK(live==live_before+1);
    CHECK(!b->spare_key&&!b->spare_binding&&parent_i.refs==2&&parent_i.i_sb->s_active==2);
    release(a);detach(b);CHECK(!owners()&&!bindings()&&!marked(&parent_i)&&parent_i.refs==2);
    sumh_inject_destroy(b);u64 before=logical();int lookups=path_lookups;rcu_barrier();CHECK(logical()==before&&parent_i.refs==2);
    flush_work(&sumh_store_free_work);CHECK(logical()==before&&path_lookups==lookups&&parent_i.refs==1);cleanup();CHECK(clean());

    setup(S_IFREG);a=prepare("/visible");CHECK(attach(&a,1)==0);release(a);
    b=prepare("/visible");CHECK(attach(&b,1)==0);before=logical();lookups=path_lookups;sumh_store_drain();
    CHECK(logical()==before&&path_lookups==lookups&&marked(&parent_i)&&b->key->users==1&&b->binding->users==1&&parent_i.refs==2);
    release(b);cleanup();CHECK(clean());

    setup(S_IFREG);a=prepare("/visible");CHECK(attach(&a,1)==0);release(a);
    arriving_entry=kzalloc(sizeof(*arriving_entry),GFP_KERNEL);CHECK(arriving_entry);
    arriving_entry->source_path=captured;path_get(&captured);arriving_entry->source_path_valid=true;sumh_vnode_live_count++;
    release_hook=callback_during_release;sumh_store_drain();CHECK(!release_hook&&!sumh_retired_entries.first&&!sumh_retired_inject_inodes.first&&!sumh_store_free_work.pending);
    CHECK(!live&&old_d.refs==0&&parent_i.refs==1);cleanup();CHECK(clean());return 0;
}
static int cycles(void){
    setup(S_IFREG);
    for(int i=0;i<1000;i++){
        struct sumh_inject_claim *a=prepare("/visible"),*b=prepare("/visible");CHECK(attach(&a,1)==0&&attach(&b,1)==0);
        release(a);CHECK(marked(&parent_i)&&owners()==1);release(b);CHECK(!marked(&parent_i)&&!owners()&&!bindings());
        sumh_store_drain();CHECK(live==0&&!bad&&sb_gets==sb_puts&&inode_gets==inode_puts);
    }cleanup();CHECK(clean());return 0;
}
static int collisions(void){
    setup(S_IFREG);const char *keys[]={"/aa","/ai","/aq","/ay","/ba","/bi","/bq","/by"};
    struct sumh_inject_claim *c[8];for(int i=0;i<8;i++){path_override(keys[i],(struct path){&visible_m,&parent_d},0);c[i]=prepare(keys[i]);CHECK(attach(&c[i],1)==0);}
    CHECK(owners()==8&&bindings()==1&&c[0]->binding->users==8);
    for(int i=0;i<8;i++){CHECK(sumh_inject_key_present_locked(keys[i]));release(c[i]);CHECK(owners()==7-i);}
    cleanup();CHECK(clean());return 0;
}
int main(void){int r; if((r=sharing()))return r;if((r=aliases()))return r;if((r=missing_errors()))return r;if((r=failures()))return r;if((r=overflow()))return r;if((r=retirement()))return r;if((r=cycles()))return r;return collisions();}
"""
