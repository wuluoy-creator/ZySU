"""Actual bootstrap/provider init, unwind and stop with actual DH/SU/store bodies.

Adjacent hook, guard and cache services are deterministic boundaries. This does
not emulate native producer concurrency or claim cache-freshness guarantees.
"""

from test_sumh_dh_ownership_source import ROOT, core, FIXTURES, read, function, replace_once, no_includes
from test_sumh_dh_ownership_producers import indexed_bodies
from test_sumh_dh_ownership_su import CASES as SU_CASES

INPUTS = (
    "kernel/sumh/core/sumh_bootstrap.c",
    "kernel/feature/sucompat.c",
    "kernel/feature/sucompat_vfs.c",
    "kernel/feature/sucompat_vfs.h",
    "uapi/su_path.h",
)

SERVICES = r"""
#define pr_alert(...) ((void)0)
#define DECLARE_RWSEM(n) struct rw_semaphore n
#define SLAB_HWCACHE_ALIGN 0
#define KSU_FEATURE_SUMH 1
#define KSU_FEATURE_SUMH_SUCOMPAT 2
#define KSU_FEATURE_SU_COMPAT 3
static bool sumh_enabled,sumh_sucompat_started,sumh_sucompat_feature_registered;
static struct mutex sumh_mutation_mutex;
static void *sumh_filldir_cache;
static int failure_stage,guard_inits,guard_acquires,guard_releases,guard_exits;
static int exec_inits,exec_exits,bridge_inits,bridge_exits,view_stops,view_drains,view_exits;
static int hide_stops,runtime_shutdowns,kernel_exits,fake_exits,cache_destroys;
static int proc_exits,iop_exits,runtime_inits,unicode_exits,unregisters;
static int stop_order,exec_stop_order,managed_stop_order,view_stop_order,bridge_exit_order;
static bool guard_live,guard_acquired,exec_live,bridge_live;
static void down_write(struct rw_semaphore *r){sleeping();if(r->readers)BAD();r->readers=-1;}
static void up_write(struct rw_semaphore *r){if(r->readers!=-1)BAD();r->readers=0;}
static void __module_get(struct module *m){if(!try_module_get(m))BAD();}
static void *kmem_cache_create(const char *name,size_t n,int a,int flags,void *ctor){(void)name;(void)n;(void)a;(void)flags;(void)ctor;return failure_stage==5?NULL:kmalloc(1,GFP_KERNEL);}
static void kmem_cache_destroy(void *cache){cache_destroys++;kfree(cache);sumh_filldir_cache=NULL;}
static int sumh_check_builtin_conflicts(void){return failure_stage==1?-EEXIST:0;}
static int ksu_sucompat_module_guard_init(void){guard_inits++;if(failure_stage==2)return -EIO;guard_live=true;return 0;}
static int ksu_sucompat_module_guard_acquire(void){guard_acquires++;if(!guard_live)BAD();if(failure_stage==3)return -EACCES;guard_acquired=true;return 0;}
static void ksu_sucompat_module_guard_release(void){guard_releases++;if(!guard_acquired)BAD();guard_acquired=false;}
static void ksu_sucompat_module_guard_exit(void){guard_exits++;if(!guard_live||guard_acquired)BAD();guard_live=false;}
static int ksu_sucompat_exec_init(void){exec_inits++;if(failure_stage==4)return -ENOMEM;exec_live=true;return 0;}
static void ksu_sucompat_exec_exit(void){exec_exits++;exec_stop_order=++stop_order;if(!exec_live||sumh_mutation_mutex.held||sumh_config_mutex.held||sumh_dh_lock.held)BAD();exec_live=false;}
static void sumh_resolve_runtime_symbols(void){}
static void sumh_resolve_system_dev(void){}
static int sumh_fake_mi_init(void){return 0;}
static void sumh_fake_mi_exit(void){fake_exits++;}
static void sumh_proc_read_hooks_init(void){}
static void sumh_proc_read_hooks_exit(void){proc_exits++;}
static int sumh_iop_override_init(void){return 0;}
static void sumh_iop_override_exit(void){iop_exits++;}
static int sumh_fop_bridge_init(void){bridge_inits++;if(failure_stage==6)return -EOPNOTSUPP;bridge_live=true;return 0;}
static void sumh_fop_bridge_stop_new(void){}
static void sumh_fop_bridge_exit(void){bridge_exits++;bridge_exit_order=++stop_order;bridge_live=false;}
static int sumh_vfs_view_init(void){return 0;}
static void sumh_vfs_view_stop(void){view_stops++;view_stop_order=++stop_order;if(su_ready||su_binding||su_enabled||sumh_mutation_mutex.held)BAD();}
static void sumh_vfs_view_drain(void){view_drains++;}
static void sumh_vfs_view_exit(void){view_exits++;}
static void sumh_kernel_build_init(void){}
static void sumh_kernel_build_exit(void){kernel_exits++;if(!sumh_mutation_mutex.held)BAD();}
static void sumh_runtime_state_init(void){runtime_inits++;}
static void sumh_runtime_state_shutdown(void){runtime_shutdowns++;if(!sumh_mutation_mutex.held||su_ready||su_binding||sumh_enabled||sumh_is_ready())BAD();}
static void sumh_hide_rules_stop(void){hide_stops++;managed_stop_order=++stop_order;if(!sumh_mutation_mutex.held||su_ready||su_binding||exec_live)BAD();}
static void sumh_unicode_guard_exit(void){unicode_exits++;}
static void ksu_unregister_feature_handler(int feature){(void)feature;unregisters++;}
"""

CASES = r"""
static int all_refs(void){
    CHECK(!bad&&!live_allocations&&!callback_count&&!sumh_dh_release_work.pending&&!sumh_store_free_work.pending);
    CHECK(!sumh_unload_pin_held&&!guard_live&&!guard_acquired&&!exec_live&&!bridge_live&&!su_ready&&!su_binding&&!su_enabled&&!su_ino);
    CHECK(fixture_sb.s_active==1&&self_module.refs==1&&foreign_module.refs==1&&fixture_mount.refs==1&&!cred_depth);
    for(int i=0;i<32;i++)CHECK(fixture_inodes[i].refs==1&&fixture_dentries[i].refs==1);
    return 0;
}
static void reset_lifecycle(void){
    fixture_init();su_ready=false;su_enabled=false;su_binding=NULL;su_ino=0;su_next_ino=10000;
    strscpy(su_path,KSU_SU_PATH_DEFAULT,sizeof(su_path));
    sumh_init_error=-EOPNOTSUPP;sumh_started=false;sumh_unload_pin_held=false;sumh_enabled=false;
    sumh_sucompat_started=false;sumh_sucompat_feature_registered=false;sumh_filldir_cache=NULL;
    failure_stage=0;stop_order=exec_stop_order=managed_stop_order=view_stop_order=bridge_exit_order=0;
    lifecycle_require_pin=false;lifecycle_pin_address=&sumh_unload_pin_held;
}
static int startup_failure_cases(void){
    const int expected[]={0,-EEXIST,-EIO,-EACCES,-ENOMEM,-ENOMEM,-EOPNOTSUPP};
    for(int stage=1;stage<=6;stage++){
        reset_lifecycle();failure_stage=stage;
        int releases=guard_releases,exits=guard_exits,providers=exec_exits,destroys=cache_destroys;
        CHECK(sumh_bootstrap_init()==expected[stage]&&!sumh_started&&!sumh_is_ready());
        CHECK(sumh_control_begin()==expected[stage]&&!sumh_lifecycle_lock.readers);
        CHECK(guard_releases-releases==(stage>=4)&&guard_exits-exits==(stage>=3));
        CHECK(exec_exits-providers==(stage>=5)&&cache_destroys-destroys==(stage==6));
        CHECK(!sumh_sucompat_started&&!sumh_unload_pin_held&&!su_ready&&!su_binding);
        ksu_sumh_exit();CHECK(sumh_init_error==-ESHUTDOWN&&!sumh_lifecycle_lock.readers);
        sumh_dirhijack_exit();sumh_dh_drain_releases();sumh_fop_override_exit();sumh_sop_shadow_exit();
        int r=all_refs();if(r)return r;
    }
    return 0;
}
static int start_bound(void){
    reset_lifecycle();native_path("/system/bin",fixture_path(1),0);
    CHECK(sumh_bootstrap_init()==0&&sumh_started&&sumh_is_ready()&&sumh_unload_pin_held&&sumh_sucompat_started);
    CHECK(ksu_sucompat_vfs_refresh()==0&&su_binding&&su_ready&&su_enabled&&su_ino);
    lifecycle_require_pin=true;
    CHECK(sumh_control_begin()==0&&sumh_lifecycle_lock.readers==1);sumh_control_end();
    int inits=exec_inits;CHECK(sumh_bootstrap_init()==0&&exec_inits==inits);
    return 0;
}
static int direct_stop_case(void){
    int r=start_bound();if(r)return r;
    CHECK(add_rule("/one/file","/source/a")==0&&hide_rule("/two/hidden",NULL)==0);
    int checks=lifecycle_callback_checks,workers=lifecycle_worker_checks,barriers=barrier_count;
    ksu_sumh_exit();
    CHECK(!sumh_started&&sumh_init_error==-ESHUTDOWN&&!sumh_lifecycle_lock.readers);
    CHECK(exec_stop_order&&exec_stop_order<managed_stop_order&&managed_stop_order<view_stop_order&&view_stop_order<bridge_exit_order);
    CHECK(lifecycle_callback_checks>checks&&lifecycle_worker_checks>workers&&barrier_count>=barriers+2);
    CHECK(!sumh_rule_count&&!sumh_hide_count&&!callback_count&&!sumh_dh_release_work.pending&&!sumh_store_free_work.pending);
    CHECK(sumh_control_begin()==-ESHUTDOWN&&!sumh_lifecycle_lock.readers&&ksu_sucompat_vfs_refresh()==-EOPNOTSUPP);
    int providers=exec_exits,guards=guard_releases;ksu_sumh_exit();ksu_sucompat_exit();
    CHECK(exec_exits==providers&&guard_releases==guards&&!sumh_sucompat_started);
    lifecycle_require_pin=false;return all_refs();
}
static int outer_then_sumh_stop_case(void){
    int r=start_bound();if(r)return r;
    sumh_sucompat_feature_registered=true;int providers=exec_exits;
    ksu_sucompat_exit();
    CHECK(exec_exits==providers+1&&!sumh_sucompat_started&&!sumh_sucompat_feature_registered&&!su_ready&&!su_binding&&!su_ino);
    CHECK(sumh_started&&sumh_unload_pin_held);
    CHECK(add_rule("/one/file","/source/a")==0);
    ksu_sumh_exit();CHECK(exec_exits==providers+1);
    lifecycle_require_pin=false;return all_refs();
}
static struct sumh_dh_claim *clear_old_su,*clear_new_su;
static unsigned long clear_old_ino,clear_new_ino;
static int clear_hook_seen,clear_row_slot,clear_claim_slot,clear_snapshot_barrier;
static void publish_su_during_clear(struct work_struct *work){
    if(work!=&sumh_store_free_work)return;
    before_work_flush=NULL;
    if(barrier_count<=clear_snapshot_barrier||!work->pending||!used[clear_row_slot]||!used[clear_claim_slot]||su_binding!=clear_old_su)BAD();
    struct ksu_su_path_config *config=su_config("/two/after-clear-snapshot");
    if(ksu_sucompat_vfs_set_config(config)||su_binding==clear_old_su||su_ino==clear_old_ino||!su_ready)BAD();
    clear_new_su=su_binding;clear_new_ino=su_ino;clear_hook_seen++;
}
static int runtime_clear_su_case(void){
    int r=start_bound();if(r)return r;
    clear_old_su=su_binding;clear_old_ino=su_ino;
    CHECK(add_rule("/one/file","/source/a")==0);
    struct sumh_entry *entry;unsigned bucket;clear_row_slot=clear_claim_slot=-1;
    hash_for_each(sumh_paths,bucket,entry,node){clear_row_slot=allocation_slot(entry);clear_claim_slot=allocation_slot(entry->dh_claim);}
    CHECK(clear_row_slot>=0&&clear_claim_slot>=0);
    clear_hook_seen=0;clear_snapshot_barrier=barrier_count;before_work_flush=publish_su_during_clear;
    CHECK(clear_rules()==0);
    CHECK(clear_hook_seen==1&&!used[clear_row_slot]&&!used[clear_claim_slot]);
    CHECK(sumh_started&&sumh_is_ready()&&sumh_enabled&&su_binding==clear_new_su&&su_ino==clear_new_ino&&ksu_sucompat_vfs_current_ino(clear_new_ino));
    /* Runtime clear does not close the independent public producer. A new
       configuration can publish a fresh binding after this withdrawal cohort. */
    struct ksu_su_path_config *config=su_config("/two/after-clear");
    CHECK(ksu_sucompat_vfs_set_config(config)==0&&su_binding!=clear_new_su&&su_ino!=clear_new_ino&&su_ready);
    CHECK(callback_count>0);
    ksu_sumh_exit();lifecycle_require_pin=false;return all_refs();
}
int main(void){int r;if((r=startup_failure_cases()))return r;if((r=direct_stop_case()))return r;if((r=outer_then_sumh_stop_case()))return r;if((r=runtime_clear_su_case()))return r;return 0;}
"""


def source(root=ROOT):
    text = core(root, su=True)
    text = replace_once(text, "static bool sumh_is_ready(void){return model_engine_ready;}",
                        "bool sumh_is_ready(void);")
    instrument = "static bool lifecycle_require_pin;static bool *lifecycle_pin_address;static int lifecycle_callback_checks,lifecycle_worker_checks;\n"
    text = replace_once(text, "static int bad,bad_line,", instrument + "static int bad,bad_line,")
    text = replace_once(text, "callback_depth++;f(h);callback_depth--;callback_runs++;",
                        "callback_depth++;if(lifecycle_require_pin){lifecycle_callback_checks++;if(!lifecycle_pin_address||!*lifecycle_pin_address)BAD();}f(h);if(lifecycle_require_pin&&(!lifecycle_pin_address||!*lifecycle_pin_address))BAD();callback_depth--;callback_runs++;")
    text = replace_once(text, "worker_depth++;w->fn(w);worker_depth--;work_runs++;",
                        "worker_depth++;if(lifecycle_require_pin){lifecycle_worker_checks++;if(!lifecycle_pin_address||!*lifecycle_pin_address)BAD();}w->fn(w);if(lifecycle_require_pin&&(!lifecycle_pin_address||!*lifecycle_pin_address))BAD();worker_depth--;work_runs++;")
    text += no_includes(read(root, "kernel/feature/sucompat_vfs.c"))
    text += indexed_bodies(root)
    text += SERVICES
    provider = read(root, "kernel/feature/sucompat.c")
    for marker in ("int ksu_sucompat_sumh_init(", "void ksu_sucompat_sumh_exit(", "void ksu_sucompat_exit("):
        text += function(provider, marker)
    bootstrap = read(root, INPUTS[0])
    text += "\nstatic bool sumh_unload_pin_held;\n"
    text += function(bootstrap, "static void sumh_bootstrap_release_unload_pin(")
    text += function(bootstrap, "static int sumh_start_views(")
    text += function(bootstrap, "static void sumh_stop_views(")
    text += bootstrap[bootstrap.index("static DECLARE_RWSEM(sumh_lifecycle_lock)"):bootstrap.index("static int sumh_feature_get(")]
    text += function(bootstrap, "void ksu_sumh_exit(")
    # Reuse only the small native configuration fixture, not the SU main/cases.
    text += FIXTURES + function(SU_CASES, "static struct ksu_su_path_config *su_config(")
    return text + CASES
