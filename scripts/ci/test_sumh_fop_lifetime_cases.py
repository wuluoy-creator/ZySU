"""Cases for actual retained-FOP install/retirement and callback execution."""

CASES = r"""
static int test_first_install(void){
    setup();CHECK(sumh_fop_install(&inodes[0])==0);
    CHECK(supers[0].s_active==2&&inodes[0].refs==2&&sb_gets==1&&inode_gets==1);
    struct sumh_fop_meta *m=sumh_fop_lookup_rcu(&inodes[0]);CHECK(m&&m->inode==&inodes[0]&&m->sb==&supers[0]);
    CHECK(table_count(sumh_fop_table)==1&&table_count(sumh_fop_templates)==1&&foreign_module.refs==1);
    CHECK(inodes[0].i_fop==sumh_fop_installed_table(m)&&test_bit(AS_FLAGS_SUMH_FOP_INSTALLED,&mappings[0].flags));
    sumh_fop_override_clear();CHECK(supers[0].s_active==1&&inodes[0].refs==1&&inodes[0].i_fop==&native_ops);
    CHECK(sb_gets==sb_puts&&inode_gets==inode_puts&&!hash_empty(sumh_fop_templates));
    CHECK(task_drains==1&&rcu_drains==1&&srcu_drains==1&&sumh_fop_override_available());
    sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_repeat_fastpath(void){
    setup();CHECK(sumh_fop_install(&inodes[0])==0);int n=allocations;
    for(int i=0;i<10;i++)CHECK(sumh_fop_install(&inodes[0])==0);
    CHECK(allocations==n&&sb_gets==1&&inode_gets==1&&supers[0].s_active==2);
    sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_invalid_inputs(void){
    setup();CHECK(sumh_fop_install(NULL)==-EINVAL);
    inodes[0].i_mapping=NULL;CHECK(sumh_fop_install(&inodes[0])==-EINVAL);inodes[0].i_mapping=&mappings[0];
    inodes[0].i_mode=0100000;CHECK(sumh_fop_install(&inodes[0])==-EINVAL);inodes[0].i_mode=S_IFDIR;
    inodes[0].i_sb=NULL;CHECK(sumh_fop_install(&inodes[0])==-EINVAL);inodes[0].i_sb=&supers[0];
    inodes[0].i_fop=NULL;CHECK(sumh_fop_install(&inodes[0])==-EOPNOTSUPP);
    inodes[0].i_fop=&no_iterate_ops;CHECK(sumh_fop_install(&inodes[0])==-EOPNOTSUPP);
    sumh_fop_override_ready=false;CHECK(sumh_fop_install(&inodes[0])==-EOPNOTSUPP);
    CHECK(!allocations&&!sb_gets&&!inode_gets);sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_allocation_failures(void){
    for(int fail=1;fail<=2;fail++){
        setup();fail_at=fail;CHECK(sumh_fop_install(&inodes[0])==-ENOMEM);
        CHECK(!live_allocations&&!sb_gets&&!inode_gets&&inodes[0].i_fop==&native_ops);
        sumh_fop_override_exit();CHECK(balanced());
    }
    /* Existing metas and shared original-owner refs survive failed siblings. */
    for(int fail=1;fail<=2;fail++){
        setup();CHECK(sumh_fop_install(&inodes[0])==0);const struct file_operations *prior=inodes[0].i_fop;
        fail_at=allocations+fail;CHECK(sumh_fop_install(&inodes[1])==-ENOMEM);
        CHECK(table_count(sumh_fop_table)==1&&inodes[0].i_fop==prior&&foreign_module.refs==1);
        CHECK(supers[0].s_active==2&&supers[1].s_active==1);sumh_fop_override_exit();CHECK(balanced());
    }
    return 0;
}
static int test_original_owner_failure(void){
    setup();foreign_module.available=0;CHECK(sumh_fop_install(&inodes[0])==-ENODEV);
    CHECK(!live_allocations&&!sb_gets&&!inode_gets&&!foreign_module.refs);
    sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_self_and_ownerless(void){
    setup();inodes[0].i_fop=&self_ops;inodes[1].i_fop=&ownerless_ops;
    CHECK(sumh_fop_install(&inodes[0])==0&&sumh_fop_install(&inodes[1])==0);
    CHECK(!module_gets&&!self_module.refs&&sb_gets==2);sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_unborn_superblock(void){
    setup();supers[0].s_flags=0;CHECK(sumh_fop_install(&inodes[0])==-ESHUTDOWN);
    CHECK(supers[0].s_active==1&&!sb_gets&&!inode_gets&&!live_allocations);
    CHECK(inodes[0].i_fop==&native_ops);sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_dead_superblock(void){
    setup();supers[0].s_active=0;CHECK(sumh_fop_install(&inodes[0])==-ESHUTDOWN);
    CHECK(!supers[0].s_active&&!sb_gets&&!inode_gets&&!live_allocations);
    CHECK(inodes[0].i_fop==&native_ops);sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_ready_closes(void){
    setup();before_spin=close_ready;CHECK(sumh_fop_install(&inodes[0])==-EOPNOTSUPP);
    CHECK(ready_hook_calls==1&&sb_gets==1&&sb_puts==1&&inode_gets==1&&inode_puts==1);
    CHECK(!live_allocations&&supers[0].s_active==1&&inodes[0].i_fop==&native_ops);
    sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_fop_changes(void){
    setup();before_spin=change_fop;CHECK(sumh_fop_install(&inodes[0])==-EAGAIN);
    CHECK(inodes[0].i_fop==&external_ops&&!live_allocations&&supers[0].s_active==1);
    CHECK(sb_gets==1&&sb_puts==1&&inode_gets==1&&inode_puts==1);
    sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_duplicate_publication(void){
    setup();before_spin=duplicate_install;CHECK(sumh_fop_install(&inodes[0])==0&&nested_install_result==0);
    CHECK(table_count(sumh_fop_table)==1&&table_count(sumh_fop_templates)==1);
    CHECK(sb_gets==2&&sb_puts==1&&inode_gets==2&&inode_puts==1&&supers[0].s_active==2&&inodes[0].refs==2);
    CHECK(foreign_module.refs==1&&live_allocations==2);sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_shared_superblock(void){
    setup();CHECK(sumh_fop_install(&inodes[0])==0&&sumh_fop_install(&inodes[3])==0);
    CHECK(supers[0].s_active==3&&table_count(sumh_fop_table)==2&&table_count(sumh_fop_templates)==1);
    CHECK(foreign_module.refs==1&&live_allocations==3);
    sumh_fop_override_clear();CHECK(supers[0].s_active==1&&sb_puts==2&&foreign_module.refs==1);
    sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_shared_template_separate_superblocks(void){
    setup();CHECK(sumh_fop_install(&inodes[0])==0&&sumh_fop_install(&inodes[1])==0);
    CHECK(supers[0].s_active==2&&supers[1].s_active==2&&table_count(sumh_fop_templates)==1);
    CHECK(sumh_fop_lookup_rcu(&inodes[0])->template==sumh_fop_lookup_rcu(&inodes[1])->template);
    sumh_fop_override_exit();CHECK(supers[0].s_active==1&&supers[1].s_active==1&&balanced());return 0;
}
static int test_separate_templates(void){
    setup();inodes[1].i_fop=&second_ops;CHECK(sumh_fop_install(&inodes[0])==0&&sumh_fop_install(&inodes[1])==0);
    CHECK(table_count(sumh_fop_templates)==2&&foreign_module.refs==1&&second_module.refs==1);
    sumh_fop_override_clear();CHECK(foreign_module.refs==1&&second_module.refs==1&&sb_puts==2);
    sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_active_client_clear(void){
    setup();CHECK(sumh_fop_install(&inodes[0])==0&&sumh_fop_install(&inodes[1])==0);
    CHECK(sumh_fop_bind_iterate_client(&inodes[0],client,&client_data)==0);
    sumh_fop_override_clear();CHECK(table_count(sumh_fop_table)==1&&sumh_fop_lookup_rcu(&inodes[0]));
    CHECK(supers[0].s_active==2&&supers[1].s_active==1&&inodes[0].held==1&&inodes[1].held==0);
    struct file fd={inodes[0].i_fop,&inodes[0]};struct dir_context ctx={0};
    CHECK(fd.f_op->iterate_shared(&fd,&ctx)==91&&client_iterates==1&&!old_iterates);
    sumh_fop_unbind_iterate_client(&inodes[0],&client_data);sumh_fop_synchronize_iterate_clients();
    CHECK(supers[0].s_active==2&&inodes[0].held==1);sumh_fop_override_clear();
    CHECK(supers[0].s_active==1&&hash_empty(sumh_fop_table));sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_bind_unbind_identity(void){
    setup();CHECK(sumh_fop_bind_iterate_client(&inodes[0],client,&client_data)==-ENOENT);
    CHECK(sumh_fop_bind_iterate_client(NULL,client,&client_data)==-EINVAL);
    CHECK(sumh_fop_bind_iterate_client(&inodes[0],NULL,&client_data)==-EINVAL);
    CHECK(sumh_fop_bind_iterate_client(&inodes[0],client,NULL)==-EINVAL);
    CHECK(sumh_fop_install(&inodes[0])==0);
    CHECK(sumh_fop_bind_iterate_client(&inodes[0],client,&client_data)==0);
    CHECK(sumh_fop_bind_iterate_client(&inodes[0],client,&client_data)==0);
    CHECK(sumh_fop_bind_iterate_client(&inodes[0],client,&other_data)==-EBUSY);
    sumh_fop_unbind_iterate_client(&inodes[0],&other_data);sumh_fop_override_clear();
    CHECK(supers[0].s_active==2&&sumh_fop_lookup_rcu(&inodes[0])->iterate_client==client);
    sumh_fop_unbind_iterate_client(NULL,NULL);sumh_fop_unbind_iterate_client(&inodes[0],NULL);
    sumh_fop_synchronize_iterate_clients();sumh_fop_override_clear();CHECK(supers[0].s_active==1);
    sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_stop_with_client(void){
    setup();CHECK(sumh_fop_install(&inodes[0])==0);
    CHECK(sumh_fop_bind_iterate_client(&inodes[0],client,&client_data)==0);
    sumh_fop_override_stop_new();CHECK(!sumh_fop_override_available()&&hash_empty(sumh_fop_table));
    CHECK(supers[0].s_active==1&&inodes[0].refs==1&&foreign_module.refs==1);
    CHECK(sumh_fop_install(&inodes[0])==-EOPNOTSUPP);
    CHECK(sumh_fop_bind_iterate_client(&inodes[0],client,&client_data)==-EOPNOTSUPP);
    sumh_fop_override_clear();CHECK(!sumh_fop_override_available());sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_external_fop_replacement(void){
    setup();CHECK(sumh_fop_install(&inodes[0])==0);inodes[0].i_fop=&external_ops;
    sumh_fop_override_clear();CHECK(inodes[0].i_fop==&external_ops&&supers[0].s_active==1);
    CHECK(!test_bit(AS_FLAGS_SUMH_FOP_INSTALLED,&mappings[0].flags));sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_old_fd_after_clear(void){
    setup();CHECK(sumh_fop_install(&inodes[0])==0);struct file oldfd={inodes[0].i_fop,&inodes[0]};
    struct sumh_fop_template *t=sumh_fop_lookup_rcu(&inodes[0])->template;int slot=slot_for(t);
    sumh_fop_override_clear();CHECK(allocated[slot]&&foreign_module.refs==1&&live_allocations==1);
    struct dir_context ctx={1002};CHECK(oldfd.f_op->iterate_shared(&oldfd,&ctx)==73&&ctx.pos==1&&old_iterates==1);
    CHECK(sumh_fop_install(&inodes[0])==0&&sumh_fop_lookup_rcu(&inodes[0])->template==t);
    CHECK(oldfd.f_op->iterate_shared(&oldfd,&ctx)==73&&ctx.pos==2&&old_iterates==2);
    /* This fd is logically closed before exit; module ownership is external. */
    oldfd.f_op=NULL;sumh_fop_override_exit();CHECK(balanced());return 0;
}
static struct file interleaved_fd;
static int interleaved_result;
static void inspect_during_drain(void){
    if(!hash_empty(sumh_fop_table)||inodes[0].i_fop!=&native_ops||inodes[0].held!=1||supers[0].s_active!=2)bad++;
    struct dir_context ctx={0};interleaved_result=interleaved_fd.f_op->iterate_shared(&interleaved_fd,&ctx);
}
static int test_old_fd_during_retirement(void){
    setup();CHECK(sumh_fop_install(&inodes[0])==0);interleaved_fd=(struct file){inodes[0].i_fop,&inodes[0]};
    during_tasks=inspect_during_drain;sumh_fop_override_clear();CHECK(interleaved_result==73&&old_iterates==1);
    CHECK(inodes[0].held==0&&supers[0].s_active==1);interleaved_fd.f_op=NULL;
    sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_permanent_stop_wins_reopen(void){
    setup();CHECK(sumh_fop_install(&inodes[0])==0);during_tasks=stop_during_clear;
    sumh_fop_override_clear();CHECK(sumh_fop_override_stopped&&!sumh_fop_override_ready);
    CHECK(sumh_fop_install(&inodes[0])==-EOPNOTSUPP&&supers[0].s_active==1);
    sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_last_native_reference(void){
    setup();CHECK(sumh_fop_install(&inodes[0])==0);caller_drops_native(&inodes[0]);
    CHECK(supers[0].s_active==1&&inodes[0].refs==1&&!supers[0].shutdowns&&!bad);
    sumh_fop_override_clear();CHECK(!supers[0].s_active&&!inodes[0].refs&&supers[0].shutdowns==1);
    sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_bridge_meta_lifetime(void){
    setup();use_bridge=true;CHECK(sumh_fop_install(&inodes[0])==0);
    struct sumh_fop_template *t=sumh_fop_lookup_rcu(&inodes[0])->template;
    CHECK(inodes[0].i_fop==&t->ingress_fop&&t->bridge.registered&&bridge_registers==1);
    sumh_fop_override_clear();CHECK(t->bridge.registered&&!bridge_unregisters&&foreign_module.refs==1);
    CHECK(inodes[0].i_fop==&native_ops&&supers[0].s_active==1);sumh_fop_override_exit();
    CHECK(bridge_unregisters==1&&balanced());return 0;
}
static int test_thousand_reuse_cycles(void){
    setup();for(int i=0;i<1000;i++){
        CHECK(sumh_fop_install(&inodes[0])==0&&supers[0].s_active==2);
        sumh_fop_override_clear();CHECK(supers[0].s_active==1&&inodes[0].refs==1&&live_allocations==1);
        CHECK(table_count(sumh_fop_templates)==1&&foreign_module.refs==1&&!bad);
    }
    CHECK(sb_gets==1000&&sb_puts==1000&&inode_gets==1000&&inode_puts==1000);
    CHECK(bridge_registers==1&&peak_allocations<=3);sumh_fop_override_exit();CHECK(balanced());return 0;
}
static int test_repeated_clear_and_exit(void){
    setup();sumh_fop_override_clear();CHECK(!task_drains);
    CHECK(sumh_fop_install(&inodes[0])==0);sumh_fop_override_clear();sumh_fop_override_clear();
    CHECK(task_drains==1&&sb_puts==1);sumh_fop_override_exit();sumh_fop_override_exit();CHECK(balanced());return 0;
}
"""

SCENARIOS = (
    "first_install", "repeat_fastpath", "invalid_inputs", "allocation_failures",
    "original_owner_failure", "self_and_ownerless", "unborn_superblock", "dead_superblock",
    "ready_closes", "fop_changes", "duplicate_publication", "shared_superblock",
    "shared_template_separate_superblocks", "separate_templates", "active_client_clear",
    "bind_unbind_identity", "stop_with_client", "external_fop_replacement", "old_fd_after_clear",
    "old_fd_during_retirement", "permanent_stop_wins_reopen", "last_native_reference",
    "bridge_meta_lifetime", "thousand_reuse_cycles", "repeated_clear_and_exit",
)


def main():
    calls = "\n".join(f"    result=test_{name}();if(result)return result;" for name in SCENARIOS)
    return "int main(void){int result;\n" + calls + "\nreturn 0;}\n"


BASELINE_CASE = r"""
int main(void){
    setup();CHECK(sumh_fop_install(&inodes[0])==0);
    caller_drops_native(&inodes[0]);
    /* A retained inode must prevent the modeled final filesystem shutdown.
       The old body has no active-sb reference, so this invariant fails. */
    CHECK(supers[0].s_active>0&&supers[0].shutdowns==0);
    return 0;
}
"""
