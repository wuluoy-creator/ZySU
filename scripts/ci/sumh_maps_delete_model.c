/* Control/lifecycle services around the real store and ioctl functions. */
#define EINVAL 22
#define ESTALE 116
#define ESHUTDOWN 108
#define pr_warn(...) ((void)0)
static int control_bad, control_read, control_lock, control_begin_error;
static int control_begins, control_ends, trace_writes, unexpected_dispatch,
    unexpected_refresh;
static int sumh_mutation_mutex, sumh_ioctl_tgid, current;
static bool audit_control, sumh_runtime_live = true;
static u64 sumh_runtime_instance = 7, sumh_runtime_generation = 1;
static s32 sumh_runtime_errors[SUMH_RUNTIME_STATE_COUNT];
static void maps_control_check(void) {
  if (audit_control && (control_read != 1 || control_lock != 1))
    control_bad++;
}
#define SUMH_MAPS_CONTROL_CHECK() maps_control_check()
static int sumh_control_begin(void) {
  control_begins++;
  if (control_read || control_lock)
    control_bad++;
  if (control_begin_error)
    return control_begin_error;
  control_read++;
  return 0;
}
static void sumh_control_end(void) {
  if (control_read != 1 || control_lock)
    control_bad++;
  control_read--;
  control_ends++;
}
static void mutex_lock(int *lock) {
  if (lock != &sumh_mutation_mutex || control_read != 1 || control_lock)
    control_bad++;
  control_lock++;
}
static void mutex_unlock(int *lock) {
  if (lock != &sumh_mutation_mutex || control_read != 1 || control_lock != 1)
    control_bad++;
  control_lock--;
}
static void atomic_long_set(int *p, long value) {
  maps_control_check();
  if (p != &sumh_ioctl_tgid)
    control_bad++;
  *p = value;
  trace_writes++;
}
static int task_tgid_vnr(int task) {
  (void)task;
  return 11;
}
static bool ksu_sucompat_vfs_enabled(void) { return true; }
static int ksu_sucompat_vfs_refresh(void) {
  unexpected_refresh++;
  return 0;
}
static int sumh_runtime_state_get(void *p) {
  (void)p;
  unexpected_dispatch++;
  return -EINVAL;
}
static int sumh_rule_inventory_get(void *p) {
  (void)p;
  unexpected_dispatch++;
  return -EINVAL;
}
static int sumh_kernel_build_conditional_set(void *p) {
  (void)p;
  unexpected_dispatch++;
  return -EINVAL;
}
static int sumh_runtime_feature_set(void *p) {
  (void)p;
  unexpected_dispatch++;
  return -EINVAL;
}
static int sumh_hide_rules_ioctl(unsigned int cmd, void *p) {
  (void)cmd;
  (void)p;
  unexpected_dispatch++;
  return -EINVAL;
}
static int sumh_dispatch_cmd(unsigned int cmd, void *p) {
  (void)cmd;
  (void)p;
  unexpected_dispatch++;
  return -EINVAL;
}
