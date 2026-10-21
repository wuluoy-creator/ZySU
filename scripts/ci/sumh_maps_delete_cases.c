/* Test real store and control functions; expected keys are independently
 * selected from entry ownership, never through the production lookup index. */
static void make_rule(struct sumh_maps_rule *rule, unsigned long ino,
                      unsigned long dev, unsigned long spoof) {
  memset(rule, 0, sizeof(*rule));
  rule->target_ino = ino;
  rule->target_dev = dev;
  rule->spoofed_ino = spoof;
  rule->spoofed_dev = 9;
  strscpy(rule->spoofed_pathname, "/temporary-or-managed.so",
          sizeof(rule->spoofed_pathname));
}
static void faults_reset(void) {
  allocation_calls = allocation_fail_at = input_copies = input_fail_at =
      output_copies = reply_fault = 0;
}
static bool clean(void) {
  return !model_bad && !memory_bad_context && !memory_lock_depth &&
         !memory_warnings && !control_bad && !control_read && !control_lock &&
         !sumh_ioctl_tgid && !unexpected_dispatch && !unexpected_refresh;
}
static bool empty(void) {
  return clean() && !physical_bytes && !allocation_live && !stats().used &&
         !sumh_maps_current;
}
static bool current_is(const struct sumh_maps_snapshot *expected) {
  struct sumh_maps_snapshot *snapshot = sumh_maps_snapshot_get();
  bool same = snapshot == expected;
  sumh_maps_snapshot_put(snapshot);
  return same;
}
static unsigned int count_current(void) {
  struct sumh_maps_snapshot *snapshot = sumh_maps_snapshot_get();
  unsigned int count = sumh_maps_snapshot_count(snapshot);
  sumh_maps_snapshot_put(snapshot);
  return count;
}
static unsigned long selected_from(const struct sumh_maps_snapshot *snapshot,
                                   unsigned long ino, unsigned long dev) {
  const struct sumh_maps_rule_entry *entry =
      sumh_maps_snapshot_lookup(snapshot, ino, dev);
  return entry ? entry->spoofed_ino : 0;
}
static unsigned long selected(unsigned long ino, unsigned long dev) {
  struct sumh_maps_snapshot *snapshot = sumh_maps_snapshot_get();
  unsigned long result = selected_from(snapshot, ino, dev);
  sumh_maps_snapshot_put(snapshot);
  return result;
}
static unsigned int exact_owner_count(const struct sumh_maps_snapshot *snapshot,
                                      unsigned long ino, unsigned long dev,
                                      bool managed) {
  unsigned int count = 0;
  for (unsigned int i = 0; i < sumh_maps_snapshot_count(snapshot); i++) {
    const struct sumh_maps_rule_entry *entry =
        sumh_maps_snapshot_entry(snapshot, i);
    if (entry->target_ino == ino && entry->target_dev == dev &&
        entry->managed == managed)
      count++;
  }
  return count;
}
static long replace(struct sumh_maps_rule *rules, unsigned int count) {
  struct sumh_managed_maps_arg arg = {0};
  arg.size = sizeof(arg);
  arg.count = count;
  arg.rules = (__UINTPTR_TYPE__)rules;
  return sumh_maps_replace_managed(&arg);
}
static struct sumh_maps_delete_arg request_for(u64 ino, u64 dev) {
  struct sumh_maps_delete_arg arg = {0};
  arg.size = sizeof(arg);
  arg.version = SUMH_MAPS_DELETE_VERSION;
  arg.expected_instance_id = sumh_runtime_instance;
  arg.expected_generation = sumh_runtime_generation;
  arg.target_ino = ino;
  arg.target_dev = dev;
  return arg;
}
static long remove_arg(struct sumh_maps_delete_arg *arg) {
  int outputs = output_copies;
  long result;
  audit_control = true;
  result = sumh_handle_ioctl(SUMH_IOC_DELETE_TEMPORARY_MAPS, arg);
  audit_control = false;
  if (output_copies != outputs || control_read || control_lock ||
      sumh_ioctl_tgid)
    control_bad++;
  return result;
}
static long remove_key(u64 ino, u64 dev) {
  struct sumh_maps_delete_arg arg = request_for(ino, dev);
  return remove_arg(&arg);
}
static int priority_and_exact_keys(void) {
  struct sumh_maps_rule rule, managed[3];
  CHECK(empty());
  /* Every exact/wildcard x temporary/managed combination, in both add orders.
   * A request with no exact temporary key cannot consume a wildcard match. */
  for (unsigned int mask = 0; mask < 16; mask++)
    for (unsigned int order = 0; order < 2; order++) {
      unsigned int count = 0;
      for (unsigned int bit = 0; bit < 2; bit++)
        if (mask & (1U << bit))
          make_rule(&managed[count++], 11, bit ? 7 : 0, 100 + bit);
      CHECK(!replace(managed, count));
      for (unsigned int step = 0; step < 2; step++) {
        unsigned int bit = order ? 3 - step : 2 + step;
        if (mask & (1U << bit)) {
          make_rule(&rule, 11, bit == 3 ? 7 : 0, 100 + bit);
          CHECK(!sumh_maps_add_rule(&rule));
        }
      }
      struct sumh_maps_snapshot *old = sumh_maps_snapshot_get();
      unsigned int initial = sumh_maps_snapshot_count(old);
      u64 generation = sumh_runtime_generation;
      struct sumh_maps_delete_arg arg = request_for(11, 7);
      long result = remove_arg(&arg);
      CHECK(result == ((mask & 8) ? 0 : -ENOENT));
      CHECK(sumh_runtime_generation == generation + !!(mask & 8));
      struct sumh_maps_snapshot *now = sumh_maps_snapshot_get();
      CHECK(sumh_maps_snapshot_count(now) == initial - !!(mask & 8));
      CHECK(exact_owner_count(now, 11, 7, false) == 0);
      CHECK(exact_owner_count(now, 11, 0, false) == !!(mask & 4));
      CHECK(exact_owner_count(now, 11, 7, true) == !!(mask & 2));
      CHECK(exact_owner_count(now, 11, 0, true) == !!(mask & 1));
      CHECK(exact_owner_count(old, 11, 7, false) == !!(mask & 8));
      if (mask & 8)
        CHECK(remove_arg(&arg) == -ESTALE);
      else
        CHECK(current_is(old));
      generation = sumh_runtime_generation;
      CHECK(remove_key(11, 8) == -ENOENT &&
            sumh_runtime_generation == generation);
      CHECK(remove_key(11, 0) == ((mask & 4) ? 0 : -ENOENT));
      CHECK(sumh_runtime_generation == generation + !!(mask & 4));
      CHECK(count_current() == !!(mask & 1) + !!(mask & 2));
      sumh_maps_snapshot_put(now);
      sumh_maps_store_clear();
      sumh_maps_snapshot_put(old);
      CHECK(empty());
    }
  /* Exact temporary removal reveals the retained managed winner. */
  make_rule(&managed[0], 88, 7, 800);
  make_rule(&managed[1], 88, 0, 801);
  make_rule(&managed[2], 89, 7, 802);
  CHECK(!replace(managed, 3));
  make_rule(&rule, 88, 7, 900);
  CHECK(!sumh_maps_add_rule(&rule));
  struct sumh_maps_snapshot *old = sumh_maps_snapshot_get();
  CHECK(selected(88, 7) == 900 && selected(88, 3) == 801);
  CHECK(!remove_key(88, 7));
  CHECK(selected(88, 7) == 800 && selected(88, 3) == 801 &&
        selected(89, 7) == 802);
  CHECK(selected_from(old, 88, 7) == 900);
  sumh_maps_store_clear();
  sumh_maps_snapshot_put(old);
  CHECK(empty());
  return 0;
}
static int tokens_validation_and_boundaries(void) {
  struct sumh_maps_rule rule;
  make_rule(&rule, 1, 7, 10);
  CHECK(!sumh_maps_add_rule(&rule));
  struct sumh_maps_snapshot *old = sumh_maps_snapshot_get();
  u64 generation = sumh_runtime_generation;
  for (unsigned int bad = 0; bad < 12; bad++) {
    struct sumh_maps_delete_arg arg = request_for(1, 7);
    int expected = -EINVAL;
    if (bad == 0)
      arg.size--;
    if (bad == 1)
      arg.size++;
    if (bad == 2)
      arg.version = 0;
    if (bad == 3)
      arg.version++;
    if (bad == 4)
      arg.expected_instance_id = 0;
    if (bad == 5)
      arg.expected_generation = 0;
    if (bad == 6) {
      arg.expected_instance_id++;
      expected = -ESTALE;
    }
    if (bad == 7) {
      arg.expected_generation++;
      expected = -ESTALE;
    }
    if (bad == 8) {
      arg.expected_generation--;
      expected = -ESTALE;
    }
    if (bad == 9)
      arg.target_dev = 0x100000000ULL;
    if (bad == 10)
      arg.target_dev = ~0ULL;
    if (bad == 11) {
      arg.target_ino = ~0ULL;
      expected = sizeof(unsigned long) == 4 ? -EOVERFLOW : -ENOENT;
    }
    CHECK(remove_arg(&arg) == expected);
    CHECK(sumh_runtime_generation == generation && current_is(old) && clean());
  }
  faults_reset();
  input_fail_at = 1;
  CHECK(remove_key(1, 7) == -EFAULT);
  faults_reset();
  CHECK(sumh_runtime_generation == generation && current_is(old));
  sumh_runtime_live = false;
  CHECK(remove_key(1, 7) == -ESHUTDOWN);
  sumh_runtime_live = true;
  CHECK(sumh_runtime_generation == generation && current_is(old));
  int begins = control_begins, ends = control_ends, copies = input_copies,
      traces = trace_writes;
  control_begin_error = -ESHUTDOWN;
  CHECK(remove_key(1, 7) == -ESHUTDOWN);
  control_begin_error = 0;
  CHECK(control_begins == begins + 1 && control_ends == ends &&
        input_copies == copies && trace_writes == traces);
  CHECK(sumh_runtime_generation == generation && current_is(old));
  /* Cleanup availability is independent of active proxy hooks, and its
   * successful publication has no reply usercopy failure point. */
  proxy_available = 0;
  reply_fault = 1;
  CHECK(!remove_key(1, 7));
  proxy_available = 1;
  reply_fault = 0;
  CHECK(sumh_runtime_generation == generation + 1 && !count_current() &&
        selected_from(old, 1, 7) == 10);
  sumh_maps_snapshot_put(old);
  CHECK(empty());
  make_rule(&rule, 0, 0, 33);
  CHECK(!sumh_maps_add_rule(&rule));
  CHECK(!remove_key(0, 0) && empty());
  make_rule(&rule, ~(unsigned long)0, 0xffffffffUL, 44);
  CHECK(!sumh_maps_add_rule(&rule));
  CHECK(!remove_key(~(unsigned long)0, 0xffffffffULL) && empty());
  /* Generation wraps to the reserved nonzero start exactly once. */
  make_rule(&rule, 2, 0, 55);
  CHECK(!sumh_maps_add_rule(&rule));
  sumh_runtime_generation = ~0ULL;
  CHECK(!remove_key(2, 0) && sumh_runtime_generation == 1 && empty());
  return 0;
}
static int preparation_faults_and_pinned_lifetime(void) {
  struct sumh_maps_rule rule, managed[2];
  struct sumh_maps_snapshot *prepared = (void *)1;
  CHECK(sumh_maps_prepare_delete(1, 7, NULL) == -EINVAL);
  CHECK(sumh_maps_prepare_delete(1, 7, &prepared) == -ENOENT && !prepared);
  for (unsigned int i = 0; i < 2; i++)
    make_rule(&managed[i], 40 + i, 7, 400 + i);
  CHECK(!replace(managed, 2));
  for (unsigned int i = 0; i < 3; i++) {
    make_rule(&rule, 20 + i, 7, 200 + i);
    CHECK(!sumh_maps_add_rule(&rule));
  }
  struct sumh_maps_snapshot *old = sumh_maps_snapshot_get(),
                            *also = sumh_maps_snapshot_get();
  size_t bytes = physical_bytes;
  u64 generation = sumh_runtime_generation;
  if (sizeof(unsigned long) > 4) {
    prepared = (void *)1;
    CHECK(sumh_maps_prepare_delete(21, (unsigned long)0x100000000ULL,
                                   &prepared) == -EINVAL &&
          !prepared);
    CHECK(current_is(old) && physical_bytes == bytes);
  }
  faults_reset();
  allocation_fail_at = 1;
  prepared = (void *)1;
  CHECK(sumh_maps_prepare_delete(21, 7, &prepared) == -ENOMEM && !prepared);
  CHECK(current_is(old) && physical_bytes == bytes);
  faults_reset();
  faults_reset();
  allocation_fail_at = 1;
  CHECK(remove_key(21, 7) == -ENOMEM);
  CHECK(current_is(old) && physical_bytes == bytes &&
        sumh_runtime_generation == generation && clean());
  faults_reset();
  size_t held = SUMH_PROC_MEMORY_BUDGET - stats().used;
  CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_STREAM, held));
  int allocations = allocation_calls;
  CHECK(remove_key(21, 7) == -ENOMEM && allocation_calls == allocations);
  CHECK(current_is(old) && physical_bytes == bytes &&
        sumh_runtime_generation == generation && clean());
  sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM, held);
  CHECK(!sumh_maps_prepare_delete(21, 7, &prepared));
  CHECK(current_is(old));
  CHECK(sumh_maps_snapshot_count(prepared) == 4 &&
        !selected_from(prepared, 21, 7));
  CHECK(selected_from(prepared, 20, 7) == 200 &&
        selected_from(prepared, 22, 7) == 202 &&
        selected_from(prepared, 40, 7) == 400);
  sumh_maps_snapshot_put(prepared);
  CHECK(current_is(old) && physical_bytes == bytes);
  CHECK(!remove_key(21, 7));
  CHECK(sumh_runtime_generation == generation + 1 && count_current() == 4);
  CHECK(selected_from(old, 21, 7) == 201 && selected_from(also, 21, 7) == 201 &&
        allocation_live == 2);
  struct sumh_maps_snapshot *newer = sumh_maps_snapshot_get();
  sumh_maps_store_clear();
  CHECK(!count_current() && allocation_live == 2);
  sumh_maps_snapshot_put(old);
  CHECK(selected_from(also, 21, 7) == 201 && allocation_live == 2);
  sumh_maps_snapshot_put(newer);
  CHECK(allocation_live == 1);
  sumh_maps_snapshot_put(also);
  CHECK(empty());
  /* Last deletion prepares NULL, needs no fresh allocation, and retains
   * prior stream memory until its last reference even at the budget cap. */
  CHECK(!sumh_maps_add_rule(&rule));
  old = sumh_maps_snapshot_get();
  held = SUMH_PROC_MEMORY_BUDGET - stats().used;
  CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_STREAM, held));
  faults_reset();
  allocation_fail_at = 1;
  CHECK(!remove_key(rule.target_ino, rule.target_dev));
  CHECK(!allocation_calls && !count_current() &&
        sumh_maps_snapshot_count(old) == 1);
  sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM, held);
  faults_reset();
  sumh_maps_snapshot_put(old);
  CHECK(empty());
  return 0;
}
static int full_collection_and_sorted_preservation(void) {
  struct sumh_maps_rule rule;
  static struct sumh_maps_rule managed[SUMH_MANAGED_MAPS_MAX_RULES];
  for (unsigned int i = 0; i < SUMH_TEMPORARY_MAPS_MAX_RULES; i++) {
    make_rule(&rule, 1 + i, 7, 10000 + i);
    CHECK(!sumh_maps_add_rule(&rule));
  }
  for (unsigned int i = 0; i < SUMH_MANAGED_MAPS_MAX_RULES; i++)
    make_rule(&managed[i], 1 + i, 7, 20000 + i);
  CHECK(!replace(managed, SUMH_MANAGED_MAPS_MAX_RULES));
  struct sumh_maps_snapshot *old = sumh_maps_snapshot_get();
  CHECK(sumh_maps_snapshot_count(old) == SUMH_MAPS_MAX_RULES);
  u64 generation = sumh_runtime_generation;
  size_t bytes = physical_bytes;
  faults_reset();
  allocation_fail_at = 1;
  CHECK(remove_key(128, 7) == -ENOMEM);
  CHECK(current_is(old) && physical_bytes == bytes &&
        sumh_runtime_generation == generation);
  faults_reset();
  size_t held = SUMH_PROC_MEMORY_BUDGET - stats().used;
  CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_STREAM, held));
  int allocations = allocation_calls;
  CHECK(remove_key(128, 7) == -ENOMEM && allocation_calls == allocations);
  CHECK(current_is(old) && physical_bytes == bytes &&
        sumh_runtime_generation == generation);
  sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM, held);
  CHECK(!remove_key(128, 7));
  struct sumh_maps_snapshot *now = sumh_maps_snapshot_get();
  unsigned int next = 0;
  CHECK(sumh_maps_snapshot_count(now) == SUMH_MAPS_MAX_RULES - 1);
  for (unsigned int i = 0; i < sumh_maps_snapshot_count(old); i++) {
    const struct sumh_maps_rule_entry *entry = sumh_maps_snapshot_entry(old, i);
    if (entry->target_ino == 128 && entry->target_dev == 7 && !entry->managed)
      continue;
    CHECK(
        !memcmp(entry, sumh_maps_snapshot_entry(now, next++), sizeof(*entry)));
  }
  CHECK(next == sumh_maps_snapshot_count(now) &&
        selected_from(now, 128, 7) == 20127 &&
        selected_from(old, 128, 7) == 10127);
  CHECK(now->temporary_count == SUMH_TEMPORARY_MAPS_MAX_RULES - 1 &&
        now->managed_count == SUMH_MANAGED_MAPS_MAX_RULES);
  make_rule(&rule, 3000, 7, 30000);
  CHECK(!sumh_maps_add_rule(&rule));
  CHECK(count_current() == SUMH_MAPS_MAX_RULES);
  sumh_maps_snapshot_put(now);
  sumh_maps_store_clear();
  sumh_maps_snapshot_put(old);
  CHECK(empty());
  return 0;
}
int main(void) {
  int result = priority_and_exact_keys();
  if (result)
    return result;
  result = tokens_validation_and_boundaries();
  if (result)
    return result;
  result = preparation_faults_and_pinned_lifetime();
  if (result)
    return result;
  return full_collection_and_sorted_preservation();
}
