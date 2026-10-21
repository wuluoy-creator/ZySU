"""Boundary and multi-owner schedules through actual registry and producers."""
from test_sumh_dh_ownership_source import ROOT, core, FIXTURES
from test_sumh_dh_ownership_cases import HELPERS
from test_sumh_dh_ownership_producers import indexed_bodies, CASES as PRODUCER_CASES
from test_sumh_dh_ownership_source import function

CASES = r"""
static int seven_top_dependencies(void){
    fixture_init();const char *keys[]={"/one/top/a","/one/top/a/b","/one/top/c/d/e","/one/top/f","/one/top/g/h/i/j","/one/top/k","/one/top/l/m"};
    struct sumh_entry *rows[7];
    for(int i=0;i<7;i++){CHECK(add_rule(keys[i],i%2?"/source/b":"/source/a")==0);rows[i]=stored(keys[i]);CHECK(rows[i]&&rows[i]->dh_claim->kind==SUMH_DH_OWNER_VTOPO&&!rows[i]->inject_claim);}
    CHECK(rows[0]->dh_claim->slot->attached==7);
    CHECK(del_rule("/one/top",NULL)==0&&rows[0]->dh_claim->slot->attached==7);
    for(int i=0;i<6;i++){CHECK(del_rule(keys[i],NULL)==0);sumh_store_drain();sumh_dh_drain_releases();CHECK(sumh_dirhijack_emits(&fixture_inodes[1],"top",3));}
    CHECK(del_rule(keys[6],NULL)==0&&!sumh_dirhijack_emits(&fixture_inodes[1],"top",3));sumh_store_drain();sumh_dh_drain_releases();return finish_case();
}
static int equal_inode_and_typed_order(void){
    fixture_init();struct sumh_dh_claim *a,*b,*h;struct sumh_dh_view view;
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    CHECK(prepared("/alias/file",SUMH_DH_OWNER_MAPPING,17,501,&b)==0&&committed(b,NULL)==0);
    rcu_read_lock();CHECK(sumh_dh_read_view(a->slot,&view)&&view.source.dentry==&fixture_dentries[17]);rcu_read_unlock();
    CHECK(prepared("/one/file",SUMH_DH_OWNER_LEGACY_HIDE,0,0,&h)==0&&committed(h,NULL)==0);
    CHECK(sumh_dirhijack_hidden(&fixture_inodes[1],"file",4));withdraw(a);
    CHECK(sumh_dirhijack_hidden(&fixture_inodes[1],"file",4));withdraw(h);
    rcu_read_lock();CHECK(sumh_dh_read_view(b->slot,&view)&&view.source.dentry==&fixture_dentries[17]);rcu_read_unlock();
    withdraw(b);return finish_case();
}
static int prepared_lifetime_and_headroom(void){
    fixture_init();struct sumh_dh_claim *a,*b,*c;struct sumh_dh_change change;struct path parent=fixture_path(1);
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,17,502,&b)==0&&prepared("/one/file",SUMH_DH_OWNER_MAPPING,17,503,&c)==0);
    struct sumh_dh_dir *dir=a->slot->dir;withdraw(a);sumh_dh_drain_releases();
    CHECK(!dir->retiring&&b->slot->prepared==2&&!winner(1,"file"));sumh_dh_abort(b);
    CHECK(!dir->retiring&&c->slot->prepared==1);CHECK(committed(c,NULL)==0&&winner(1,"file")==503);
    struct sumh_dh_spec hide={.kind=SUMH_DH_OWNER_MANAGED_HIDE};
    CHECK(sumh_dh_prepare_parent(&parent,"file",&hide,&b)==0);unsigned users=c->slot->hide_users;c->slot->hide_users=UINT_MAX;
    CHECK(sumh_dh_change_begin(b,NULL,&change)==-EOVERFLOW&&b->state==DH_PREPARED&&winner(1,"file")==503);c->slot->hide_users=users;sumh_dh_abort(b);
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,17,502,&b)==0);sumh_dh_abort(b);CHECK(winner(1,"file")==503);withdraw(c);return finish_case();
}
static int exact_delete_parent_errors(void){
    const int errors[]={0,-ENOENT,-ENOTDIR,-EACCES,-EIO};
    for(unsigned i=0;i<ARRAY_SIZE(errors);i++){
        fixture_init();CHECK(add_rule("/one/file","/source/a")==0&&add_rule("/two/file","/source/b")==0);
        struct sumh_entry *other=stored("/two/file");native_path("/one",fixture_path(2),errors[i]);
        CHECK(del_rule("/one/file",NULL)==0&&!stored("/one/file")&&stored("/two/file")==other&&other->dh_claim->state==DH_ATTACHED&&!sumh_dh_iop_of(&fixture_inodes[1]));
        CHECK(clear_rules()==0);int r=finish_case();if(r)return r;
    }return 0;
}
static int replacement_allocation_failures(void){
    /* Canonical source display text is explicitly optional in capture_paths.
       Disable that service so this sweep covers only required allocations. */
    sumh_d_absolute_path=NULL;
    fixture_init();CHECK(add_rule("/one/file","/source/a")==0);int before=allocations;
    CHECK(add_rule("/one/file","/source/b")==0);int count=allocations-before;CHECK(clear_rules()==0);int r=finish_case();if(r)return r;
    for(int n=1;n<=count;n++){
        fixture_init();CHECK(add_rule("/one/file","/source/a")==0);struct sumh_entry *old=stored("/one/file");int rows=sumh_rule_count;
        fail_at=allocations+n;int error=add_rule("/one/file","/source/b");fail_at=0;
        CHECK(error==-ENOMEM&&stored("/one/file")==old&&old->source_path.dentry==&fixture_dentries[16]&&old->dh_claim->state==DH_ATTACHED&&old->inject_claim->attached&&sumh_rule_count==rows);
        CHECK(clear_rules()==0);r=finish_case();if(r)return r;
    }sumh_d_absolute_path=d_path;return 0;
}
static int new_directory_prepare_failures(void){
    fixture_init();struct sumh_dh_claim *claim=NULL;int before=allocations;
    CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&claim)==0);int count=allocations-before;sumh_dh_abort(claim);int r=finish_case();if(r)return r;
    for(int n=1;n<=count;n++){
        fixture_init();fail_at=allocations+n;int error=prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&claim);fail_at=0;
        CHECK(error==-ENOMEM&&!claim&&!sumh_dh_iop_of(&fixture_inodes[1]));r=finish_case();if(r)return r;
    }
    fixture_init();struct sumh_dh_claim *a;CHECK(prepared("/one/file",SUMH_DH_OWNER_MAPPING,16,501,&a)==0&&committed(a,NULL)==0);
    fixture_inodes[2].synthetic=true;CHECK(prepared("/two/file",SUMH_DH_OWNER_MAPPING,17,502,&claim)==-EOPNOTSUPP&&!claim&&winner(1,"file")==501);fixture_inodes[2].synthetic=false;
    fixture_sb.s_magic=FUSE_SUPER_MAGIC;CHECK(prepared("/two/hidden",SUMH_DH_OWNER_LEGACY_HIDE,0,0,&claim)==-EOPNOTSUPP&&!claim);fixture_sb.s_magic=0;
    sumh_dirhijack_stop_new();CHECK(prepared("/two/file",SUMH_DH_OWNER_MAPPING,17,502,&claim)==-ESHUTDOWN&&!claim&&winner(1,"file")==501);
    withdraw(a);return finish_case();
}
static int hide_rebind_failure(void){
    fixture_init();CHECK(hide_rule("/one/file",NULL)==0);struct sumh_hide_entry *old=hidden("/one/file");
    struct sumh_hide_binding *managed;struct path p=fixture_path(1);CHECK(sumh_dirhijack_hide_get(&p,"file",&managed)==0);
    native_path("/one",fixture_path(2),-EIO);CHECK(hide_rule("/one/file",NULL)==-EIO&&hidden("/one/file")==old&&sumh_hide_count==1&&old->dh_claim->state==DH_ATTACHED);
    native_path("/one",fixture_path(2),0);CHECK(hide_rule("/one/file",NULL)==0&&hidden("/one/file")!=old&&sumh_hide_count==1&&old->dh_claim->state==DH_DETACHED);
    CHECK(sumh_dirhijack_hidden(&fixture_inodes[1],"file",4)&&sumh_dirhijack_hidden(&fixture_inodes[2],"file",4));
    CHECK(del_rule("/one/file",NULL)==0&&sumh_dirhijack_hidden(&fixture_inodes[1],"file",4)&&!sumh_dirhijack_hidden(&fixture_inodes[2],"file",4));
    sumh_dirhijack_hide_put(managed);sumh_store_drain();sumh_dh_drain_releases();return finish_case();
}
int main(void){int r;if((r=seven_top_dependencies()))return r;if((r=equal_inode_and_typed_order()))return r;if((r=prepared_lifetime_and_headroom()))return r;if((r=exact_delete_parent_errors()))return r;if((r=replacement_allocation_failures()))return r;if((r=new_directory_prepare_failures()))return r;if((r=hide_rebind_failure()))return r;return 0;}
"""


def source(root=ROOT):
    return core(root) + indexed_bodies(root) + FIXTURES + HELPERS + function(PRODUCER_CASES, "static struct sumh_entry *stored(") + function(PRODUCER_CASES, "static struct sumh_hide_entry *hidden(") + CASES
