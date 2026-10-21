"""Independent aggregate and FOP references, using both production bodies."""
import re
from test_sumh_inject_claims_model import model, fixtures, remove_function, replace_once
from test_sumh_inject_claims_source import read, runtime, normalizer, wrapper, body, INPUTS
from test_sumh_vfs_helpers import COMMON

SCENARIOS = (
    "aggregate-and-fop-independent-pins", "aggregate-retirement-before-fop",
    "fop-retirement-before-aggregate", "actual-runtime-clear-order", "actual-stop-views-order",
    "failed-group-retains-safe-fop-cache", "failed-alias-preparation-fop-cache",
    "active-iterate-client-survives-aggregate-clear",
)

SERVICES = r"""
#define ENODEV 19
#define AS_FLAGS_SUMH_FOP_INSTALLED 4
#define DEFINE_SPINLOCK(n) int n
#define DEFINE_STATIC_SRCU(n) int n
#define smp_load_acquire(p) (*(p))
#define smp_store_release(p,v) (*(p)=(v))
#define smp_wmb() ((void)0)
#define pr_info(...) ((void)0)
#define hash_init(t) memset(t,0,sizeof(t))
static int spin_depth,srcu_depth,rcu_depth,module_gets,module_puts,tasks_drains,srcu_drains,rcu_drains;
struct module {int refs;};
static struct module self_module,foreign_module;
#define THIS_MODULE (&self_module)
struct file;
struct dir_context {loff_t pos;};
struct file_operations {struct module *owner;int (*iterate_shared)(struct file *,struct dir_context *);};
struct file {const struct file_operations *f_op;struct inode *inode;};
struct sumh_fop_bridge_entry {bool registered;};
static struct inode *file_inode(struct file *f){return f->inode;}
static int native_iterate(struct file *f,struct dir_context *c){(void)f;c->pos++;return 0;}
static const struct file_operations native_ops={&foreign_module,native_iterate};
static bool try_module_get(struct module *m){m->refs++;module_gets++;return true;}
static void module_put(struct module *m){if(spin_depth||read_depth||callback_depth||m->refs<=0)bad++;m->refs--;module_puts++;}
static void spin_lock(int *m){(void)m;if(lock_depth||spin_depth||read_depth)bad++;spin_depth++;}
static void spin_unlock(int *m){(void)m;if(spin_depth!=1)bad++;spin_depth--;}
static void rcu_read_lock(void){rcu_depth++;}
static void rcu_read_unlock(void){rcu_depth--;}
static int srcu_read_lock(int *s){(void)s;srcu_depth++;return 1;}
static void srcu_read_unlock(int *s,int i){(void)s;if(i!=1)bad++;srcu_depth--;}
static void synchronize_rcu_tasks(void){if(spin_depth||read_depth||callback_depth)bad++;tasks_drains++;}
static void synchronize_rcu(void){if(spin_depth||read_depth||callback_depth)bad++;rcu_drains++;}
static void synchronize_srcu(int *s){(void)s;if(spin_depth||read_depth||callback_depth||srcu_depth)bad++;srcu_drains++;}
static void atomic64_inc(long long *p){(*p)++;}
static struct {long long iterate_fop_entries,iterate_fop_wrapped;} sumh_hook_stats;
struct sumh_filldir_wrapper {struct dir_context wrap_ctx;bool has_iterate_client,stopped;unsigned emitted;};
static bool sumh_dirhijack_injected_context(struct dir_context *c){return c->pos>=1000;}
static struct sumh_filldir_wrapper *sumh_iterate_prepare_wrapper(struct file *f,struct dir_context *c){(void)f;(void)c;return NULL;}
static void sumh_iterate_rewind_injected(struct dir_context *c){c->pos=0;}
static void sumh_iterate_emit_injected(struct sumh_filldir_wrapper *w){w->stopped=true;}
static void sumh_iterate_finish_wrapper(struct sumh_filldir_wrapper *w){(void)w;}
static int sumh_fop_bridge_register(struct sumh_fop_bridge_entry *b,struct file_operations *i,struct file_operations *s,const struct file_operations *o){(void)i;(void)s;(void)o;b->registered=false;return 0;}
static void sumh_fop_bridge_unregister(struct sumh_fop_bridge_entry *b){b->registered=false;}
struct list_head {struct list_head *next,*prev;};
#define LIST_HEAD(n) struct list_head n={&n,&n}
static void INIT_LIST_HEAD(struct list_head *h){h->next=h->prev=h;}
static bool list_empty(const struct list_head *h){return h->next==h;}
static void list_add_tail(struct list_head *n,struct list_head *h){n->next=h;n->prev=h->prev;h->prev->next=n;h->prev=n;}
static void list_del_init(struct list_head *n){n->next->prev=n->prev;n->prev->next=n->next;INIT_LIST_HEAD(n);}
#define list_for_each_entry_safe(p,n,h,m) for((p)=container_of((h)->next,__typeof__(*(p)),m),(n)=container_of((p)->m.next,__typeof__(*(p)),m);&(p)->m!=(h);(p)=(n),(n)=container_of((n)->m.next,__typeof__(*(n)),m))
"""

STOP_SERVICES = r"""
static bool sumh_enabled,sumh_unload_pin_held;
static void *sumh_filldir_cache;
static int sumh_mutation_mutex;
static void sumh_runtime_state_shutdown(void){}
static void ksu_sucompat_sumh_exit(void){}
static void sumh_hide_rules_stop(void){}
static void sumh_kernel_build_exit(void){}
static void sumh_vfs_view_stop(void){}
static void sumh_vfs_view_drain(void){}
static void sumh_proc_read_hooks_exit(void){}
static void sumh_dirhijack_stop_new(void){}
static void sumh_sop_shadow_stop_new(void){}
static void sumh_dirhijack_exit(void){if(shadow.dentry){path_put(&shadow);memset(&shadow,0,sizeof(shadow));}}
static void sumh_fop_bridge_stop_new(void){}
static void sumh_fop_bridge_exit(void){}
static void sumh_iop_override_exit(void){}
static void sumh_sop_shadow_exit(void){}
static void sumh_fake_mi_exit(void){}
static void sumh_vfs_view_exit(void){}
static void kmem_cache_destroy(void *p){(void)p;bad++;}
static void sumh_bootstrap_release_unload_pin(void){sumh_unload_pin_held=false;}
"""

CASES = r"""
static void init(void){setup(S_IFREG);self_module.refs=foreign_module.refs=0;module_gets=module_puts=spin_depth=srcu_depth=rcu_depth=0;
    tasks_drains=srcu_drains=rcu_drains=0;old_i.i_fop=new_i.i_fop=link_i.i_fop=visible_i.i_fop=parent_i.i_fop=&native_ops;
    sumh_enabled=true;sumh_unload_pin_held=false;sumh_filldir_cache=NULL;clear_services=0;sumh_fop_override_init();}
static int balanced(void){return clean()&&!spin_depth&&!srcu_depth&&!rcu_depth&&module_gets==module_puts&&!foreign_module.refs&&hash_empty(sumh_fop_table)&&hash_empty(sumh_fop_templates);}
static int attach_one(struct sumh_inject_claim *c){mutex_lock(&sumh_config_mutex);int ret=sumh_inject_attach_locked(&c,1);mutex_unlock(&sumh_config_mutex);if(!ret)sumh_inject_finish_prepare(c);return ret;}
static void withdraw(struct sumh_inject_claim *c){mutex_lock(&sumh_config_mutex);sumh_inject_detach_locked(c);mutex_unlock(&sumh_config_mutex);sumh_inject_destroy(c);}
static bool marked(void){return test_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT,&parent_i.i_mapping->flags);}
static int client_cookie,client_calls;
static int retained_client(struct file *f,struct dir_context *c,const struct file_operations *original,void *data){
    if(f->inode!=&parent_i||original!=&native_ops||data!=&client_cookie||srcu_depth!=1)bad++;client_calls++;c->pos++;return 27;
}
int main(void){
    for(int order=0;order<2;order++){
        init();struct sumh_inject_claim *c=NULL;CHECK(sumh_inject_prepare_directory("/visible",SUMH_INJECT_MISSING_NONE,&c)==0);
        CHECK(c&&attach_one(c)==0&&parent_i.refs==3&&parent_i.i_sb->s_active==3&&table_count(sumh_fop_table)==1&&marked());
        CHECK(c->binding->inode==&parent_i&&c->binding->sb==parent_i.i_sb);
        if(order==0){withdraw(c);sumh_store_drain();CHECK(parent_i.refs==2&&parent_i.i_sb->s_active==2&&!marked()&&table_count(sumh_fop_table)==1);sumh_fop_override_clear();}
        else{sumh_fop_override_clear();CHECK(parent_i.refs==2&&parent_i.i_sb->s_active==2&&marked()&&!table_count(sumh_fop_table));withdraw(c);sumh_store_drain();}
        CHECK(parent_i.refs==1&&parent_i.i_sb->s_active==1&&tasks_drains&&srcu_drains&&rcu_drains);sumh_fop_override_exit();cleanup();CHECK(balanced());
    }
    init();CHECK(submit()==0&&parent_i.refs==3&&parent_i.i_sb->s_active==3);CHECK(clear_command()==0);
    CHECK(parent_i.refs==1&&parent_i.i_sb->s_active==1&&!marked()&&!table_count(sumh_fop_table)&&sumh_fop_override_available());
    CHECK(submit()==0&&parent_i.refs==3);sumh_stop_views();CHECK(!sumh_enabled&&!sumh_fop_override_available()&&parent_i.refs==1&&parent_i.i_sb->s_active==1);cleanup();CHECK(balanced());

    init();client_calls=0;CHECK(submit()==0);CHECK(sumh_fop_bind_iterate_client(&parent_i,retained_client,&client_cookie)==0);
    struct file old_fd={parent_i.i_fop,&parent_i};struct dir_context ctx={0};CHECK(clear_command()==0);
    CHECK(!marked()&&!table_count(sumh_inject_inodes)&&!table_count(sumh_inject_dirs)&&table_count(sumh_fop_table)==1);
    CHECK(parent_i.refs==2&&parent_i.i_sb->s_active==2&&sumh_fop_lookup_rcu(&parent_i)->iterate_client==retained_client);
    CHECK(old_fd.f_op->iterate_shared(&old_fd,&ctx)==27&&client_calls==1&&ctx.pos==1);
    sumh_fop_unbind_iterate_client(&parent_i,&client_cookie);sumh_fop_synchronize_iterate_clients();sumh_fop_override_clear();
    CHECK(parent_i.refs==1&&parent_i.i_sb->s_active==1&&!table_count(sumh_fop_table));sumh_fop_override_exit();cleanup();CHECK(balanced());

    init();struct sumh_inject_claim *c=NULL;CHECK(sumh_inject_prepare_directory("/visible",SUMH_INJECT_MISSING_NONE,&c)==0);
    sumh_rule_count=INT_MAX;CHECK(attach_one(c)==-EOVERFLOW);sumh_rule_count=0;sumh_inject_destroy(c);
    CHECK(parent_i.refs==2&&parent_i.i_sb->s_active==2&&!marked()&&table_count(sumh_fop_table)==1);sumh_fop_override_exit();cleanup();CHECK(balanced());

    init();parent_d.canonical="/alias";struct sumh_inject_claim *pair[2];char *resolved=NULL;
    CHECK(sumh_inject_prepare_merge("/visible",&resolved,pair)==0);int allocations=allocs;
    sumh_inject_destroy(pair[0]);sumh_inject_destroy(pair[1]);kfree(resolved);sumh_fop_override_exit();cleanup();CHECK(balanced());
    for(int i=1;i<=allocations;i++){
        init();parent_d.canonical="/alias";fail_at=i;CHECK(sumh_inject_prepare_merge("/visible",&resolved,pair)==-ENOMEM);
        CHECK(!pair[0]&&!pair[1]&&!resolved&&!marked()&&!table_count(sumh_inject_dirs));
        CHECK(parent_i.refs==1+table_count(sumh_fop_table)&&parent_i.i_sb->s_active==1+table_count(sumh_fop_table));
        sumh_fop_override_exit();cleanup();CHECK(balanced());
    }return 0;
}
"""


def source(root):
    from test_sumh_source_identity import MODEL, CASES as OLD_CASES
    from test_sumh_inject_claims_producers import clear_source
    types = read(root, INPUTS[0])
    layouts = types[types.index("struct sumh_inject_claim;"):types.index("struct sumh_name_list {")]
    services = model(MODEL, layouts)
    services = remove_function(services, "static int sumh_fop_install(")
    services = replace_once(services, "int refs;bool synthetic;", "int refs;bool synthetic;const struct file_operations *i_fop;")
    header = re.sub(r"^#include[^\n]*\n", "", read(root, "kernel/sumh/features/sumh_fop_override.h"), flags=re.M)
    fop = re.sub(r"^#include[^\n]*\n", "", read(root, "kernel/sumh/features/sumh_fop_override.c"), flags=re.M)
    clear = remove_function(clear_source(root), "static void sumh_fop_override_clear(")
    stop = body(read(root, "kernel/sumh/core/sumh_bootstrap.c"), "static void sumh_stop_views(")
    return (COMMON + services + SERVICES + header + fop + runtime(root)
            + normalizer(root) + wrapper("SUMH_IOC_ADD_RULE", "submit_path", root)
            + '\nstatic int submit(void){return submit_path("/visible/file","/source");}\n'
            + fixtures(OLD_CASES) + clear + STOP_SERVICES + stop + CASES)
