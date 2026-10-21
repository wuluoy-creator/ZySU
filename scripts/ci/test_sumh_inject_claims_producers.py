"""Cases through actual ADD/MERGE/DEL/CLEAR bodies and the materializer."""

SCENARIOS = (
    "identical-add-owner-transfer", "add-parent-rebinding", "store-changed-output",
    "add-missing-parent", "add-parent-error", "add-atomic-row-headroom",
    "busy-before-dirhijack-and-hidden-mutation", "mapping-hide-injection-priority",
    "delete-withdraws-only-selected-owner", "delete-no-native-unbind",
    "top-merge-shared-key", "top-merge-duplicate-before-prepare", "nested-merge-owner",
    "materialized-child-owner", "materialized-missing-prefix-rejected", "required-child-shadow-failure",
    "clear-rebound-path", "old-callback-after-clear-and-republish", "clear-command-drains",
    "add-normalization-fail-closed",
)

CASES = r"""
static bool marked(struct inode *i){return test_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT,&i->i_mapping->flags);}
static int owner_rows(void){return table_count(sumh_inject_dirs);}
static int inode_rows(void){return table_count(sumh_inject_inodes);}
static u64 projected(void){return (u64)owner_rows()*1000ULL+(u64)inode_rows()*100ULL+(u64)(unsigned int)sumh_rule_count*10ULL+marked(&parent_i)+marked(&visible_i)*2;}
static struct sumh_entry *candidate(void){
    struct sumh_entry *e=kzalloc(sizeof(*e),GFP_KERNEL);if(!e){bad++;return NULL;}
    e->src=kstrdup("/visible/file",GFP_KERNEL);e->target=kstrdup("/source",GFP_KERNEL);e->src_hash=full_name_hash(NULL,e->src,strlen(e->src));
    if(sumh_entry_capture_source(e,e->target)||sumh_inject_prepare_directory("/visible",SUMH_INJECT_MISSING_NONE,&e->inject_claim))bad++;
    struct sumh_dh_spec spec={.kind=SUMH_DH_OWNER_MAPPING,.source=&e->source_path};
    if(sumh_dh_prepare_path(e->src,&spec,&e->dh_claim,NULL))bad++;return e;
}
static int add_owners(void){
    init();old_i.i_mode=S_IFREG;CHECK(submit()==0);struct sumh_entry *old=entry("/visible/file");struct sumh_inject_claim *prior=old->inject_claim;
    CHECK(prior&&prior->attached&&prior->key->users==1&&prior->binding->users==1&&marked(&parent_i));int clears=inject_clears;
    CHECK(submit()==0);struct sumh_entry *now=entry("/visible/file");
    CHECK(now!=old&&now->inject_claim!=prior&&now->inject_claim->attached&&!prior->attached);
    CHECK(inject_clears==clears&&now->inject_claim->key->users==1&&now->inject_claim->binding->users==1&&owner_rows()==1&&sumh_rule_count==2);
    u64 before=projected();sumh_store_drain();CHECK(projected()==before&&marked(&parent_i));finish();CHECK(balanced());

    init();CHECK(submit()==0);path_override("/visible",(struct path){&visible_m,&visible_d},0);CHECK(submit()==0);
    CHECK(!marked(&parent_i)&&marked(&visible_i)&&inode_rows()==1&&entry("/visible/file")->inject_claim->binding->inode==&visible_i);
    before=projected();sumh_store_drain();CHECK(projected()==before);finish();CHECK(balanced());

    init();struct sumh_entry *e=candidate();bool changed=false;CHECK(sumh_store_upsert(e,&changed)==0&&changed);sumh_inject_finish_prepare(e->inject_claim);
    e=candidate();changed=true;CHECK(sumh_store_upsert(e,&changed)==0&&!changed);sumh_inject_finish_prepare(e->inject_claim);
    captured=(struct path){&new_m,&new_d};e=candidate();CHECK(sumh_store_upsert(e,&changed)==0&&changed);sumh_inject_finish_prepare(e->inject_claim);finish();CHECK(balanced());

    init();CHECK(submit_path("/missing/deep","/source")==0);CHECK(entry("/missing/deep")&&!entry("/missing/deep")->inject_claim&&!owner_rows()&&sumh_rule_count==1);finish();CHECK(balanced());
    const int errors[]={-ENOENT,-EACCES,-ENOTDIR,-EIO,-ENOMEM};
    for(int i=0;i<5;i++){
        init();path_override("/visible",(struct path){0},errors[i]);int ret=submit();
        CHECK(ret==(i?errors[i]:0));if(i)CHECK(!entry("/visible/file")&&!owner_rows()&&!old_i.marks&&!shadow_calls);
        else CHECK(entry("/visible/file")&&!entry("/visible/file")->inject_claim);finish();CHECK(balanced());
    }
    init();sumh_rule_count=INT_MAX-1;CHECK(submit()==-EOVERFLOW);CHECK(!owner_rows()&&!entry("/visible/file")&&sumh_rule_count==INT_MAX-1);sumh_rule_count=0;finish();CHECK(balanced());
    return 0;
}
static int normalization(void){
    const int errors[]={-EIO,-EACCES,-ENOTDIR,-ENOMEM,-ELOOP,-ESTALE};
    for(int existing=0;existing<2;existing++)for(int kind=0;kind<4;kind++)for(int i=0;i<(kind<2?6:3);i++){
        init();if(existing)CHECK(submit()==0);
        struct sumh_entry *prior=entry("/visible/file");u64 state=projected();int live_before=live,marks=old_i.marks,dh=shadow_calls;
        int references=path_gets-path_puts,callbacks_before=callback_count,pins=parent_i.refs;
        int expected=errors[i];
        if(kind==0)path_override("/visible/file",(struct path){0},expected);
        if(kind==1){path_override("/visible/file",(struct path){0},-ENOENT);path_override("/visible",(struct path){0},expected);}
        if(kind>=2){if(kind==3)path_override("/visible/file",(struct path){0},-ENOENT);if(i<2){render_error=i?-ENOMEM:-EIO;expected=render_error;}else{invalid_render=true;expected=-EINVAL;}}
        CHECK(submit()==expected);CHECK(entry("/visible/file")==prior&&projected()==state&&live==live_before);
        CHECK(old_i.marks==marks&&shadow_calls==dh&&path_gets-path_puts==references&&callback_count==callbacks_before&&parent_i.refs==pins);
        path_error=render_error=0;invalid_render=false;override_count=0;finish();CHECK(balanced());
    }
    /* Input strings, normalizer buffer/rendered copy (and missing-leaf parent
       copy), then canonical parent_dir copy are all mandatory allocations. */
    for(int existing=0;existing<2;existing++)for(int fallback=0;fallback<2;fallback++)for(int at=1;at<=5+fallback;at++){
        init();if(existing)CHECK(submit()==0);if(fallback)path_override("/visible/file",(struct path){0},-ENOENT);
        struct sumh_entry *prior=entry("/visible/file");u64 state=projected();int live_before=live,marks=old_i.marks,dh=shadow_calls;
        int references=path_gets-path_puts,callbacks_before=callback_count,pins=parent_i.refs;
        fail_at=allocs+at;CHECK(submit()==-ENOMEM);CHECK(entry("/visible/file")==prior&&projected()==state&&live==live_before);
        CHECK(old_i.marks==marks&&shadow_calls==dh&&path_gets-path_puts==references&&callback_count==callbacks_before&&parent_i.refs==pins);
        fail_at=0;override_count=0;finish();CHECK(balanced());
    }return 0;
}
static void seed_hide(const char *s){
    struct sumh_hide_entry *h=kzalloc(sizeof(*h),GFP_KERNEL);h->path=kstrdup(s,GFP_KERNEL);h->path_hash=full_name_hash(NULL,s,strlen(s));
    mutex_lock(&sumh_config_mutex);hlist_add_head_rcu(&h->node,&sumh_hide_paths[hash_min(h->path_hash,SUMH_HASH_BITS)]);sumh_hide_count++;mutex_unlock(&sumh_config_mutex);
}
static int deletion(void){
    init();CHECK(submit()==0);set_bit(AS_FLAGS_SUMH_HIDE,&parent_i.i_mapping->flags);u64 before=projected();
    CHECK(delete_path("/visible",NULL)==-EBUSY);CHECK(dh_deletes==0&&projected()==before&&test_bit(AS_FLAGS_SUMH_HIDE,&parent_i.i_mapping->flags));
    CHECK(!clear_by_path[AS_FLAGS_SUMH_HIDE]&&!clear_by_path[AS_FLAGS_SUMH_DIR_HAS_INJECT]);finish();CHECK(balanced());

    init();CHECK(submit()==0);CHECK(submit_path("/visible","/source")==0);seed_hide("/visible");
    CHECK(delete_path("/visible",NULL)==0&&!entry("/visible")&&entry("/visible/file")&&sumh_hide_count==1&&owner_rows()==1&&marked(&parent_i));
    CHECK(delete_path("/visible",NULL)==0&&!sumh_hide_count&&owner_rows()==1&&marked(&parent_i));int calls=dh_deletes;
    CHECK(delete_path("/visible",NULL)==-EBUSY&&dh_deletes==calls);finish();CHECK(balanced());

    init();CHECK(submit()==0&&submit_path("/visible/other","/source")==0);CHECK(owner_rows()==1&&sumh_rule_count==3);
    CHECK(delete_path("/visible/file",NULL)==0&&entry("/visible/other")&&owner_rows()==1&&marked(&parent_i)&&sumh_rule_count==2);
    CHECK(delete_path("/visible/other",NULL)==0&&!owner_rows()&&!inode_rows()&&!marked(&parent_i)&&!sumh_rule_count);finish();CHECK(balanced());
    init();CHECK(submit()==0);dh_delete_error=-EIO;CHECK(delete_path("/visible/file",NULL)==0&&!entry("/visible/file")&&!dh_deletes);finish();CHECK(balanced());return 0;
}
static int merge_owners(void){
    init();const char *targets[]={"/one","/two","/three","/four","/five","/six"};
    for(int i=0;i<6;i++){struct path p=node(S_IFDIR);path_override(targets[i],p,0);CHECK(submit_merge_path("/visible/file",targets[i])==0);}
    CHECK(table_count(sumh_merge_dirs)==6&&owner_rows()==1&&inode_rows()==1&&sumh_rule_count==1);
    struct sumh_merge_entry *m=container_of(sumh_merge_dirs[0].first,struct sumh_merge_entry,node);
    CHECK(m->inject_claims[0]&&m->inject_claims[0]->attached&&m->inject_claims[0]->key&&m->inject_claims[0]->binding);
    CHECK(m->inject_claims[0]->key->users==6&&m->inject_claims[0]->binding->users==6&&marked(&visible_i));finish();CHECK(balanced());

    init();CHECK(submit_merge()==0);current_target=captured;path_error=-EACCES;fop_error=-EOPNOTSUPP;render_error=-EIO;
    int calls=fop_calls,lookups=path_lookups,renders=path_renders;CHECK(submit_merge()==-EEXIST);
    CHECK(fop_calls==calls&&path_lookups==lookups+1&&path_renders==renders&&table_count(sumh_merge_dirs)==1);path_error=render_error=fop_error=0;finish();CHECK(balanced());

    init();struct path sub=node(S_IFDIR),leaf=node(S_IFREG);child(captured,"child",DT_DIR,sub);child(sub,"leaf",DT_REG,leaf);visible_children=true;
    CHECK(submit_merge()==0);CHECK(table_count(sumh_merge_dirs)==2&&entry("/visible/file/child/leaf"));
    struct sumh_entry *e=entry("/visible/file/child/leaf");CHECK(e->inject_claim&&e->inject_claim->attached&&e->inject_claim->key->users>=2);
    CHECK(owner_rows()==2&&inode_rows()==1&&marked(&visible_i));finish();CHECK(balanced());

    init();child(captured,"a",DT_REG,node(S_IFREG));CHECK(submit_merge()==0);e=entry("/visible/file/a");
    CHECK(e&&e->inject_claim&&e->inject_claim->key->users==2&&e->inject_claim->binding->users==2&&owner_rows()==1&&inode_rows()==1);finish();CHECK(balanced());
    init();child(captured,"a",DT_REG,node(S_IFREG));CHECK(submit_merge_path("/missing","/source")==-ENOENT);e=entry("/missing/a");
    CHECK(!e&&owner_rows()==1&&!inode_rows());finish();CHECK(balanced());
    init();child(captured,"a",DT_REG,node(S_IFREG));shadow_error=-EIO;CHECK(submit_merge()==-EIO);e=entry("/visible/file/a");
    CHECK(!e&&owner_rows()==1&&marked(&visible_i));finish();CHECK(balanced());return 0;
}
static int clear_owners(void){
    init();CHECK(submit()==0);path_override("/visible",(struct path){&visible_m,&visible_d},0);
    set_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT,&visible_i.i_mapping->flags);
    struct sumh_dh_retire retired;sumh_dh_retire_init(&retired,true);
    mutex_lock(&sumh_config_mutex);sumh_cleanup_locked(&retired);mutex_unlock(&sumh_config_mutex);sumh_dh_retire_finish(&retired);
    CHECK(!marked(&parent_i)&&marked(&visible_i)&&!owner_rows()&&!inode_rows()&&!sumh_rule_count&&!clear_by_path[AS_FLAGS_SUMH_DIR_HAS_INJECT]);
    clear_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT,&visible_i.i_mapping->flags);finish();CHECK(balanced());

    init();CHECK(submit()==0);sumh_dh_retire_init(&retired,true);mutex_lock(&sumh_config_mutex);sumh_cleanup_locked(&retired);mutex_unlock(&sumh_config_mutex);sumh_dh_retire_finish(&retired);
    CHECK(!marked(&parent_i)&&!owner_rows()&&!inode_rows());CHECK(submit()==0);u64 before=projected();int lookups=path_lookups;
    sumh_store_drain();CHECK(projected()==before&&marked(&parent_i)&&path_lookups==lookups);finish();CHECK(balanced());
    init();child(captured,"a",DT_REG,node(S_IFREG));CHECK(submit_merge()==0);CHECK(clear_command()==0);
    CHECK(!owner_rows()&&!inode_rows()&&!sumh_rule_count&&!marked(&visible_i)&&!callback_count&&!sumh_store_free_work.pending&&clear_services==7);
    finish();CHECK(balanced());return 0;
}
int main(void){int r;if((r=normalization()))return r;if((r=add_owners()))return r;if((r=deletion()))return r;if((r=merge_owners()))return r;return clear_owners();}
"""


def clear_source(root):
    from test_sumh_inject_claims_source import read, body
    text = read(root, "kernel/sumh/control/sumh_ioctl.c")
    exact = body(text, "if (cmd == SUMH_IOC_CLEAR_ALL) {")
    prelude = r"""
static int clear_services;
static void sumh_hide_rules_clear(void){clear_services++;}
static void sumh_kernel_build_clear(void){clear_services++;}
static void sumh_dirhijack_clear(void){clear_services++;if(shadow.dentry){path_put(&shadow);memset(&shadow,0,sizeof(shadow));}}
static void sumh_fop_override_clear(void){clear_services++;}
static void sumh_sop_shadow_reap(void){clear_services++;}
static void sumh_fake_mi_invalidate_all(void){clear_services++;}
"""
    return prelude + "static int clear_command(void)" + exact[exact.index("{"):]


def source(root):
    from test_sumh_inject_claims_source import merge_source
    from test_sumh_merge_install_cases import TESTS
    fixture = TESTS[:TESTS.index("static int capture_registration(void)")]
    return merge_source(root) + fixture + clear_source(root) + CASES
