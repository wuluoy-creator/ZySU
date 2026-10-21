/* Cases execute production store and ioctl wrappers, not a parallel store. */
_Static_assert(sizeof(struct sumh_managed_maps_arg) == 24, "managed ABI size");
_Static_assert(__builtin_offsetof(struct sumh_managed_maps_arg, rules) == 8,
               "managed ABI pointer");
static void make_rule(struct sumh_maps_rule *r, unsigned long ino,
                      unsigned long dev, unsigned long spoof) {
  memset(r, 0, sizeof(*r));
  r->target_ino = ino;
  r->target_dev = dev;
  r->spoofed_ino = spoof;
  r->spoofed_dev = 7;
  strscpy(r->spoofed_pathname, "/system/lib64/library.so",
          sizeof(r->spoofed_pathname));
}
static void faults_reset(void) {
  allocation_calls = allocation_fail_at = input_copies = input_fail_at =
      reply_fault = 0;
}
static long replace(struct sumh_maps_rule *rules, unsigned int count) {
  struct sumh_managed_maps_arg arg = {0};
  arg.size = sizeof(arg);
  arg.count = count;
  arg.rules = (__UINTPTR_TYPE__)rules;
  return sumh_maps_replace_managed(&arg);
}
static unsigned long selected_from(const struct sumh_maps_snapshot *s,
                                   unsigned long ino, unsigned long dev) {
  const struct sumh_maps_rule_entry *e = sumh_maps_snapshot_lookup(s, ino, dev);
  return e ? e->spoofed_ino : 0;
}
static unsigned long selected(unsigned long ino, unsigned long dev) {
  struct sumh_maps_snapshot *s = sumh_maps_snapshot_get();
  unsigned long value = selected_from(s, ino, dev);
  sumh_maps_snapshot_put(s);
  return value;
}
static unsigned int count_current(void) {
  struct sumh_maps_snapshot *s = sumh_maps_snapshot_get();
  unsigned int n = sumh_maps_snapshot_count(s);
  sumh_maps_snapshot_put(s);
  return n;
}
static bool clean(void) {
  return !model_bad && !memory_bad_context && !memory_lock_depth &&
         !memory_warnings;
}
static bool empty(void) {
  return clean() && !physical_bytes && !allocation_live && !stats().used &&
         !count_current();
}
static bool current_is(const struct sumh_maps_snapshot *expected) {
  struct sumh_maps_snapshot *s = sumh_maps_snapshot_get();
  bool same = s == expected;
  sumh_maps_snapshot_put(s);
  return same;
}
/* Independent winner oracle scores policy, not the production index. */
static const struct sumh_maps_rule_entry *
oracle(const struct sumh_maps_snapshot *s, unsigned long ino,
       unsigned long dev) {
  const struct sumh_maps_rule_entry *winner = NULL;
  unsigned int score = 0;
  for (unsigned int i = 0; i < sumh_maps_snapshot_count(s); i++) {
    const struct sumh_maps_rule_entry *e = sumh_maps_snapshot_entry(s, i);
    if (e->target_ino != ino || (e->target_dev && e->target_dev != dev))
      continue;
    unsigned int rank = 1 + (!e->managed) + (e->target_dev ? 2 : 0);
    if (rank > score) {
      winner = e;
      score = rank;
    }
  }
  return winner;
}
static int basic_and_priority(void) {
  struct sumh_maps_rule rule, managed[2];
  CHECK(empty());
  CHECK(!sumh_maps_snapshot_entry(NULL, 0));
  CHECK(!sumh_maps_snapshot_lookup(NULL, 0, 0));
  /* All16 combinations of exact/wildcard x temporary/managed. */
  for (unsigned int mask = 0; mask < 16; mask++)
    for (unsigned int order = 0; order < 2; order++) {
      unsigned int n = 0;
      for (unsigned int bit = 0; bit < 2; bit++)
        if (mask & (1U << bit))
          make_rule(&managed[n++], 11, bit ? 7 : 0, 100 + bit);
      CHECK(!replace(managed, n));
      for (unsigned int step = 0; step < 2; step++) {
        unsigned int bit = order ? 3 - step : 2 + step;
        if (mask & (1U << bit)) {
          make_rule(&rule, 11, bit == 3 ? 7 : 0, 100 + bit);
          CHECK(!sumh_maps_add_rule(&rule));
        }
      }
      struct sumh_maps_snapshot *s = sumh_maps_snapshot_get();
      for (unsigned long ino = 10; ino <= 12; ino++)
        for (unsigned long dev = 0; dev <= 9; dev++)
          CHECK(sumh_maps_snapshot_lookup(s, ino, dev) == oracle(s, ino, dev));
      CHECK(sumh_maps_snapshot_count(s) ==
            ((mask & 1) != 0) + ((mask & 2) != 0) + ((mask & 4) != 0) +
                ((mask & 8) != 0));
      CHECK(!sumh_maps_snapshot_entry(s, sumh_maps_snapshot_count(s)));
      sumh_maps_store_clear();
      CHECK(!count_current());
      if (mask)
        CHECK(sumh_maps_snapshot_count(s) > 0 && allocation_live == 1);
      sumh_maps_snapshot_put(s);
      CHECK(empty());
    }
  make_rule(&rule, 0, 0, 999);
  rule.spoofed_pathname[0] = 0;
  CHECK(!sumh_maps_add_rule(&rule));
  CHECK(selected(0, 0) == 999 && selected(0, 999) == 999);
  CHECK(replace(&rule, 1) == -EINVAL);
  CHECK(selected(0, 0) == 999);
  make_rule(&rule, ~(unsigned long)0, 0xffffffffUL, 1000);
  CHECK(!sumh_maps_add_rule(&rule));
  CHECK(selected(~(unsigned long)0, 0xffffffffUL) == 1000 &&
        !selected(~(unsigned long)0, 1));
#if __SIZEOF_LONG__ > 4 || defined(SUMH_TEST_LP64)
  CHECK(sizeof(rule.target_ino) == 8 && rule.target_ino > 0xffffffffULL);
  make_rule(&rule, 0x100000001ULL, 7, 0x8000000000000001ULL);
  CHECK(!sumh_maps_add_rule(&rule));
  CHECK(selected(0x100000001ULL, 7) == 0x8000000000000001ULL &&
        !selected(1, 7));
#endif
  sumh_maps_store_clear();
  CHECK(empty());
  return 0;
}
static int validation_and_faults(void) {
  struct sumh_maps_rule rule, managed[3];
  struct sumh_maps_snapshot *out = (void *)1;
  make_rule(&rule, 1, 7, 10);
  CHECK(!sumh_maps_add_rule(&rule));
  for (unsigned int i = 0; i < 3; i++)
    make_rule(&managed[i], i + 2, i, 20 + i);
  CHECK(!replace(managed, 3));
  struct sumh_maps_snapshot *before = sumh_maps_snapshot_get();
  size_t bytes = physical_bytes;
  for (int i = 1; i <= 2; i++) {
    faults_reset();
    allocation_fail_at = i;
    CHECK(replace(managed, 3) == -ENOMEM);
    CHECK(current_is(before) && physical_bytes == bytes && clean());
  }
  for (int i = 1; i <= 2; i++) {
    faults_reset();
    input_fail_at = i;
    CHECK(replace(managed, 3) == -EFAULT);
    CHECK(current_is(before) && physical_bytes == bytes && clean());
  }
  faults_reset();
  reply_fault = 1;
  rule.spoofed_ino = 99;
  CHECK(sumh_maps_add_rule(&rule) == -EFAULT);
  CHECK(current_is(before) && selected(1, 7) == 10 && physical_bytes == bytes);
  faults_reset();
  allocation_fail_at = 1;
  CHECK(sumh_maps_add_rule(&rule) == -ENOMEM);
  faults_reset();
  input_fail_at = 1;
  CHECK(sumh_maps_add_rule(&rule) == -EFAULT);
  faults_reset();
  for (unsigned int bad = 0; bad < 7; bad++) {
    struct sumh_maps_rule original = managed[2];
    if (bad == 0)
      managed[2].target_ino = 0;
    if (bad == 1)
      managed[2].err = 1;
    if (bad == 2)
      memset(managed[2].spoofed_pathname, 'x',
             sizeof(managed[2].spoofed_pathname));
    if (bad == 3)
      managed[2].spoofed_pathname[2] = '\n';
    if (bad == 4)
      managed[2].spoofed_pathname[2] = '\r';
    if (bad == 5)
      managed[2] = managed[0];
    if (bad == 6)
      managed[2] = managed[1];
    CHECK(replace(managed, 3) == -EINVAL);
    CHECK(current_is(before) && physical_bytes == bytes && clean());
    managed[2] = original;
  }
#if __SIZEOF_LONG__ > 4 || defined(SUMH_TEST_LP64)
  managed[2].target_dev = 0x100000000ULL;
  CHECK(replace(managed, 3) == -EINVAL);
  managed[2].target_dev = 2;
  managed[2].spoofed_dev = 0x100000000ULL;
  CHECK(replace(managed, 3) == -EINVAL);
  managed[2].spoofed_dev = 7;
#endif
  struct sumh_managed_maps_arg arg = {0};
  arg.size = sizeof(arg);
  arg.count = 1;
  arg.rules = (__UINTPTR_TYPE__)managed;
  arg.size--;
  CHECK(sumh_maps_replace_managed(&arg) == -EINVAL);
  arg.size++;
  arg.reserved[0] = 1;
  CHECK(sumh_maps_replace_managed(&arg) == -EINVAL);
  arg.reserved[0] = 0;
  arg.reserved[1] = 1;
  CHECK(sumh_maps_replace_managed(&arg) == -EINVAL);
  arg.reserved[1] = 0;
  arg.count = 257;
  CHECK(sumh_maps_replace_managed(&arg) == -EINVAL);
  arg.count = 1;
  arg.rules = 0;
  CHECK(sumh_maps_replace_managed(&arg) == -EINVAL);
  arg.rules = (__UINTPTR_TYPE__)managed;
  proxy_available = 0;
  CHECK(sumh_maps_replace_managed(&arg) == -EOPNOTSUPP);
  CHECK(current_is(before) && physical_bytes == bytes);
  CHECK(!replace(NULL, 0));
  CHECK(count_current() == 1 && selected(1, 7) == 10);
  proxy_available = 1;
  CHECK(selected_from(before, 4, 2) == 22);
  sumh_maps_snapshot_put(before);
  CHECK(sumh_maps_prepare_add(NULL, &out) == -EINVAL && !out);
  CHECK(sumh_maps_prepare_add(&rule, NULL) == -EINVAL);
  out = (void *)1;
  CHECK(sumh_maps_prepare_managed(NULL, 1, &out) == -EINVAL && !out);
  CHECK(sumh_maps_prepare_managed(NULL, 0, NULL) == -EINVAL);
  CHECK(!clear_rules(SUMH_IOC_CLEAR_MAPS_RULES));
  CHECK(empty());
  return 0;
}
static struct sumh_maps_snapshot *interleave_prepared;
static void publish_before_get(void) {
  sumh_maps_publish(interleave_prepared);
  interleave_prepared = NULL;
}
static int pins_and_staging(void) {
  struct sumh_maps_rule rule;
  struct sumh_maps_snapshot *prepared, *old, *also, *newer;
  make_rule(&rule, 5, 7, 50);
  CHECK(!sumh_maps_add_rule(&rule));
  old = sumh_maps_snapshot_get();
  also = sumh_maps_snapshot_get();
  rule.spoofed_ino = 51;
  CHECK(!sumh_maps_prepare_add(&rule, &prepared));
  CHECK(current_is(old));
  CHECK(selected_from(prepared, 5, 7) == 51 && selected_from(old, 5, 7) == 50);
  sumh_maps_snapshot_put(prepared);
  CHECK(current_is(old) && allocation_live == 1);
  CHECK(!sumh_maps_prepare_add(&rule, &interleave_prepared));
  before_memory_lock = publish_before_get;
  newer = sumh_maps_snapshot_get();
  CHECK(selected_from(newer, 5, 7) == 51 && selected_from(old, 5, 7) == 50 &&
        allocation_live == 2);
  sumh_maps_store_clear();
  CHECK(!count_current() && allocation_live == 2);
  sumh_maps_snapshot_put(old);
  CHECK(selected_from(also, 5, 7) == 50 && allocation_live == 2);
  sumh_maps_snapshot_put(newer);
  CHECK(allocation_live == 1);
  sumh_maps_snapshot_put(also);
  CHECK(empty());
  /* Fixed NULL is a valid empty collection even after a publication. */
  old = sumh_maps_snapshot_get();
  CHECK(!old);
  CHECK(!sumh_maps_add_rule(&rule));
  CHECK(!sumh_maps_snapshot_count(old) &&
        !sumh_maps_snapshot_lookup(old, 5, 7));
  sumh_maps_snapshot_put(old);
  sumh_maps_store_clear();
  sumh_maps_store_clear();
  CHECK(empty());
  return 0;
}
static int caps_and_budget(void) {
  struct sumh_maps_rule rule;
  static struct sumh_maps_rule managed[SUMH_MANAGED_MAPS_MAX_RULES];
  static struct sumh_maps_snapshot *pins[64];
  unsigned int pinned = 0;
  for (unsigned int i = 0; i < SUMH_TEMPORARY_MAPS_MAX_RULES; i++) {
    make_rule(&rule, i + 1, 7, 10000 + i);
    CHECK(!sumh_maps_add_rule(&rule));
  }
  CHECK(count_current() == SUMH_TEMPORARY_MAPS_MAX_RULES);
  make_rule(&rule, 2000, 7, 20000);
  int calls = allocation_calls;
  CHECK(sumh_maps_add_rule(&rule) == -ENOSPC && allocation_calls == calls);
  make_rule(&rule, 1, 7, 555);
  CHECK(!sumh_maps_add_rule(&rule));
  CHECK(selected(1, 7) == 555);
  for (unsigned int i = 0; i < SUMH_MANAGED_MAPS_MAX_RULES; i++)
    make_rule(&managed[i], 3000 + i, 0, 4000 + i);
  memset(managed[0].spoofed_pathname, 'x', SUMH_MAX_LEN_PATHNAME - 1);
  managed[0].spoofed_pathname[SUMH_MAX_LEN_PATHNAME - 1] = 0;
  managed[1].spoofed_pathname[0] = 0;
  CHECK(!replace(managed, SUMH_MANAGED_MAPS_MAX_RULES));
  CHECK(count_current() == SUMH_MAPS_MAX_RULES);
  struct sumh_maps_snapshot *full = sumh_maps_snapshot_get();
  for (unsigned long ino = 0; ino < 3300; ino += 7)
    for (unsigned long dev = 0; dev < 9; dev++)
      CHECK(sumh_maps_snapshot_lookup(full, ino, dev) ==
            oracle(full, ino, dev));
  for (unsigned int i = 1; i < sumh_maps_snapshot_count(full); i++)
    CHECK(sumh_maps_entry_compare(sumh_maps_snapshot_entry(full, i - 1),
                                  sumh_maps_snapshot_entry(full, i)) < 0);
  CHECK(strlen(sumh_maps_snapshot_lookup(full, 3000, 9)->spoofed_pathname) ==
        255);
  CHECK(!sumh_maps_snapshot_lookup(full, 3001, 9)->spoofed_pathname[0]);
  size_t bytes = physical_bytes, held = SUMH_PROC_MEMORY_BUDGET - stats().used;
  CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_STREAM, held));
  calls = allocation_calls;
  CHECK(replace(managed, 256) == -ENOMEM && allocation_calls == calls &&
        current_is(full));
  CHECK(replace(NULL, 0) == -ENOMEM && allocation_calls == calls &&
        current_is(full));
  CHECK(sumh_maps_add_rule(&rule) == -ENOMEM && allocation_calls == calls &&
        current_is(full));
  CHECK(physical_bytes == bytes);
  sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM, held);
  /* Admit raw input but deny the larger prepared collection: copied input
   * must be freed and uncharged while the complete old generation stays. */
  held = SUMH_PROC_MEMORY_BUDGET - stats().used - sizeof(managed);
  CHECK(sumh_proc_memory_reserve(SUMH_PROC_MEMORY_WORKSPACE, held));
  calls = allocation_calls;
  CHECK(replace(managed, 256) == -ENOMEM && allocation_calls == calls + 1 &&
        current_is(full));
  CHECK(physical_bytes == bytes &&
        stats().by_kind[SUMH_PROC_MEMORY_WORKSPACE] == held);
  sumh_proc_memory_release(SUMH_PROC_MEMORY_WORKSPACE, held);
  sumh_maps_snapshot_put(full);
  /* Hold each real old generation until the shared live-byte cap rejects. */
  for (;;) {
    struct sumh_maps_snapshot *s = sumh_maps_snapshot_get();
    managed[2].spoofed_ino = 5000 + pinned;
    long rc = replace(managed, 256);
    if (rc) {
      CHECK(rc == -ENOMEM && current_is(s));
      sumh_maps_snapshot_put(s);
      break;
    }
    CHECK(pinned < ARRAY_SIZE(pins));
    pins[pinned++] = s;
    CHECK(selected_from(s, 3002, 7) == (pinned == 1 ? 4002 : 4998 + pinned));
  }
  CHECK(pinned > 10 && stats().used <= SUMH_PROC_MEMORY_BUDGET && clean());
  calls = allocation_calls;
  CHECK(!clear_rules(SUMH_IOC_CLEAR_MAPS_RULES));
  CHECK(allocation_calls == calls && !count_current());
  CHECK(allocation_live == (int)pinned);
  for (unsigned int i = 0; i < pinned; i++) {
    CHECK(sumh_maps_snapshot_count(pins[i]) == SUMH_MAPS_MAX_RULES);
    CHECK(selected_from(pins[i], 1, 7) == 555);
    sumh_maps_snapshot_put(pins[i]);
  }
  CHECK(empty());
  CHECK(!replace(managed, 256));
  CHECK(selected(3002, 7) == 5000 + pinned);
  sumh_maps_store_clear();
  CHECK(empty());
  return 0;
}
int main(void) {
  int ret = basic_and_priority();
  if (ret)
    return ret;
  ret = validation_and_faults();
  if (ret)
    return ret;
  ret = pins_and_staging();
  if (ret)
    return ret;
  return caps_and_budget();
}
