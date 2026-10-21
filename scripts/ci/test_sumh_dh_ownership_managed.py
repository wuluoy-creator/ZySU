"""Actual managed-hide resolver and withdrawal policy on the actual DH owner."""

from test_sumh_dh_ownership_source import ROOT, core, FIXTURES, read, no_includes

INPUTS = ("kernel/sumh/core/sumh_hide_rules.c",)

SERVICES = r"""
typedef long long atomic64_t;
#define ATOMIC64_INIT(v) (v)
#define WQ_MEM_RECLAIM 1
#define pr_warn_ratelimited(...) ((void)0)
#define SUMH_USER_HIDE_BOUND 1
#define SUMH_USER_HIDE_PENDING 2
#define SUMH_USER_HIDE_BINDING 3
#define SUMH_USER_HIDE_ERROR 4
static long long atomic64_read(const atomic64_t *v){return *v;}
struct task_struct {int unused;};static struct task_struct task,*current=&task;
struct workqueue_struct {bool live;};static struct workqueue_struct model_hide_queue;
static struct workqueue_struct *alloc_ordered_workqueue(const char *n,int f){(void)n;(void)f;model_hide_queue.live=true;return &model_hide_queue;}
static void destroy_workqueue(struct workqueue_struct *q){if(!q||!q->live)BAD();q->live=false;}
static bool queue_work(struct workqueue_struct *q,struct work_struct *w){if(!q||!q->live)BAD();return schedule_work(w);}
struct sumh_hide_events {bool stopping;int error;};
struct sumh_hide_watch {bool live;};
static struct sumh_hide_watch watch_pool[64];
static int watch_gets,watch_puts,watch_error,ack_error;
static bool model_fail_binding;
static void (*during_watch)(void);
static int sumh_enabled=1;
static int sumh_hide_events_open(struct sumh_hide_events *e,void(*n)(void *),void *d){(void)n;(void)d;e->stopping=false;e->error=0;return 0;}
static int sumh_hide_events_ack(struct sumh_hide_events *e){(void)e;return ack_error;}
static void sumh_hide_events_stop(struct sumh_hide_events *e){e->stopping=true;}
static void sumh_hide_events_close(struct sumh_hide_events *e){if(!e->stopping)BAD();}
static void sumh_hide_events_unwatch(struct sumh_hide_watch *w){if(!w)return;if(!w->live)BAD();w->live=false;watch_puts++;}
static int sumh_hide_events_watch_path(struct sumh_hide_events *e,const char *p,const char *n,struct path *out,struct sumh_hide_watch **watch){
    (void)e;(void)n;*watch=NULL;
    for(int i=0;i<64;i++)if(!watch_pool[i].live){watch_pool[i].live=true;*watch=&watch_pool[i];watch_gets++;break;}
    if(!*watch){BAD();return -ENOMEM;}
    int ret=watch_error?watch_error:kern_path(p,LOOKUP_FOLLOW|LOOKUP_DIRECTORY,out);
    if(model_fail_binding){fail_at=allocations+1;model_fail_binding=false;}
    if(during_watch){void(*hook)(void)=during_watch;during_watch=NULL;hook();}
    return ret;
}
"""


def bodies(root=ROOT):
    text = no_includes(read(root, INPUTS[0]))
    return SERVICES + text[:text.index("int sumh_hide_rule_inventory(")]


CASES = r"""
static struct managed_hide_rule *model_rule;
static struct managed_hide_rule *new_rule(void){
    const char *name="/one/hidden";size_t n=strlen(name)+1;
    struct managed_hide_rule *r=kzalloc(sizeof(*r)+n,GFP_KERNEL);if(!r)return NULL;
    memcpy(r->path,name,n);r->parent=kstrdup("/one",GFP_KERNEL);r->name=strrchr(r->path,'/')+1;
    refcount_set(&r->refs,1);r->id=1;r->generation=1;INIT_LIST_HEAD(&r->list);list_add_tail(&r->list,&hide_rules);return r;
}
static void resolve_rule(void){sumh_hide_rules_changed();flush_work(&hide_work);}
static int generation_retry_seen;
static struct sumh_hide_binding *generation_old_binding;
static void inspect_generation_retry(void){
    if(model_rule->binding!=generation_old_binding||!sumh_dirhijack_hidden(&fixture_inodes[2],"hidden",6)||sumh_dirhijack_hidden(&fixture_inodes[1],"hidden",6))BAD();
    if(watch_gets-watch_puts!=2)BAD();generation_retry_seen++;
}
static void generation_change(void){model_rule->generation++;native_path("/one",fixture_path(1),0);during_watch=inspect_generation_retry;}
static void delete_during_watch(void){mutex_lock(&sumh_mutation_mutex);hide_remove(model_rule);mutex_unlock(&sumh_mutation_mutex);model_rule=NULL;}
static void stop_during_watch(void){hide_stop_new();}
static void disable_during_watch(void){sumh_enabled=0;}
static int managed_policy_cases(void){
    fixture_init();CHECK(sumh_hide_rules_available());
    model_rule=new_rule();CHECK(model_rule);resolve_rule();
    CHECK(model_rule->state==SUMH_USER_HIDE_BOUND&&model_rule->binding);
    struct sumh_hide_binding *original=model_rule->binding;int gets=watch_gets;
    resolve_rule();CHECK(model_rule->binding==original&&watch_gets==gets+1);
    CHECK(sumh_dirhijack_hidden(&fixture_inodes[1],"hidden",6));
    watch_error=-EIO;resolve_rule();
    CHECK(model_rule->binding==original&&model_rule->state==SUMH_USER_HIDE_ERROR&&model_rule->error==-EIO);
    watch_error=-EACCES;resolve_rule();CHECK(model_rule->binding==original&&model_rule->error==-EACCES);
    watch_error=-ENOENT;resolve_rule();
    CHECK(!model_rule->binding&&model_rule->state==SUMH_USER_HIDE_PENDING&&model_rule->error==-ENOENT);
    CHECK(!sumh_dirhijack_hidden(&fixture_inodes[1],"hidden",6));
    watch_error=0;resolve_rule();CHECK(model_rule->binding&&model_rule->state==SUMH_USER_HIDE_BOUND);
    watch_error=-ENOTDIR;resolve_rule();
    CHECK(!model_rule->binding&&model_rule->state==SUMH_USER_HIDE_ERROR&&model_rule->error==-ENOTDIR);
    watch_error=0;resolve_rule();CHECK(model_rule->binding);
    native_path("/one",fixture_path(2),0);model_fail_binding=true;resolve_rule();fail_at=0;
    CHECK(!model_rule->binding&&model_rule->state==SUMH_USER_HIDE_ERROR&&model_rule->error==-ENOMEM);
    CHECK(!sumh_dirhijack_hidden(&fixture_inodes[1],"hidden",6)&&!sumh_dirhijack_hidden(&fixture_inodes[2],"hidden",6));
    resolve_rule();CHECK(model_rule->binding&&sumh_dirhijack_hidden(&fixture_inodes[2],"hidden",6));
    u64 generation=model_rule->generation;generation_old_binding=model_rule->binding;generation_retry_seen=0;during_watch=generation_change;resolve_rule();
    CHECK(generation_retry_seen==1&&model_rule->generation==generation+1&&model_rule->state==SUMH_USER_HIDE_BOUND&&model_rule->binding!=generation_old_binding&&sumh_dirhijack_hidden(&fixture_inodes[1],"hidden",6)&&!sumh_dirhijack_hidden(&fixture_inodes[2],"hidden",6));
    generation_old_binding=model_rule->binding;native_path("/one",fixture_path(2),0);during_watch=disable_during_watch;resolve_rule();
    CHECK(!sumh_enabled&&model_rule->binding==generation_old_binding&&!sumh_dirhijack_hidden(&fixture_inodes[2],"hidden",6)&&!atomic_read(&hide_jobs));
    sumh_enabled=1;resolve_rule();CHECK(sumh_dirhijack_hidden(&fixture_inodes[2],"hidden",6));
    during_watch=delete_during_watch;resolve_rule();
    CHECK(!model_rule&&list_empty(&hide_rules)&&!atomic_read(&hide_jobs));
    CHECK(watch_gets==watch_puts);
    model_rule=new_rule();CHECK(model_rule);resolve_rule();generation_old_binding=model_rule->binding;
    native_path("/one",fixture_path(1),0);during_watch=stop_during_watch;resolve_rule();
    CHECK(hide_stopping&&model_rule->binding==generation_old_binding&&!sumh_dirhijack_hidden(&fixture_inodes[1],"hidden",6));
    mutex_lock(&sumh_mutation_mutex);sumh_hide_rules_stop();mutex_unlock(&sumh_mutation_mutex);
    sumh_dirhijack_exit();sumh_dh_drain_releases();sumh_fop_override_exit();sumh_sop_shadow_exit();
    CHECK(!bad&&!live_allocations&&fixture_sb.s_active==1);
    return 0;
}
int main(void){return managed_policy_cases();}
"""


def source(root=ROOT):
    return core(root) + bodies(root) + FIXTURES + CASES
