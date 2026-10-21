static void benchmark_rule(struct sumh_maps_rule *r, unsigned int i,
                           bool managed) {
  memset(r, 0, sizeof(*r));
  r->target_ino = i + 1;
  r->target_dev = i % 3 ? 7 : 0;
  r->spoofed_ino = 100 + i + (managed ? 5000 : 0);
  r->spoofed_dev = 9;
  strscpy(r->spoofed_pathname, "/bench", 256);
}
static int check_equivalence(const struct sumh_maps_snapshot *snapshot,
                             unsigned int n) {
  for (unsigned long ino = 0; ino < n + 7; ino++)
    for (unsigned long dev = 0; dev <= 9; dev++) {
      const struct sumh_maps_rule_entry *entry =
          sumh_maps_snapshot_lookup(snapshot, ino, dev);
      const struct baseline_entry *old = baseline_find_rule(ino, dev);
      CHECK((entry == NULL) == (old == NULL));
      if (entry)
        CHECK(entry->managed == old->managed &&
              entry->spoofed_ino == old->spoofed_ino &&
              entry->spoofed_dev == old->spoofed_dev &&
              !strcmp(entry->spoofed_pathname, old->spoofed_pathname));
    }
  return 0;
}
int main(void) {
  const unsigned int counts[] = {0, 1, 16, 64, 256, 512, 1024, 1280};
  const unsigned int vmas[] = {128, 1024, 4096};
  static struct baseline_entry entries[SUMH_MAPS_MAX_RULES];
  static struct sumh_maps_rule managed[SUMH_MANAGED_MAPS_MAX_RULES];
  puts("rules,vmas,repetitions,lookups,linear_node_visits,index_probes,linear_"
       "seconds,index_seconds,checksum");
  for (unsigned int c = 0; c < ARRAY_SIZE(counts); c++) {
    unsigned int count = counts[c], temp = count < 1024 ? count : 1024;
    for (unsigned int i = 0; i < temp; i++) {
      struct sumh_maps_rule rule;
      struct sumh_maps_snapshot *prepared = NULL;
      benchmark_rule(&rule, i, false);
      CHECK(!sumh_maps_prepare_add(&rule, &prepared));
      sumh_maps_publish(prepared);
    }
    if (count > temp) {
      struct sumh_maps_snapshot *prepared = NULL;
      for (unsigned int i = 0; i < count - temp; i++)
        benchmark_rule(&managed[i], i, true);
      CHECK(!sumh_maps_prepare_managed(managed, count - temp, &prepared));
      sumh_maps_publish(prepared);
    }
    struct sumh_maps_snapshot *snapshot = sumh_maps_snapshot_get();
    CHECK(sumh_maps_snapshot_count(snapshot) == count);
    baseline_rules.next = baseline_rules.prev = &baseline_rules;
    for (unsigned int i = 0; i < count; i++) {
      const struct sumh_maps_rule_entry *e =
          sumh_maps_snapshot_entry(snapshot, i);
      entries[i].managed = e->managed;
      entries[i].target_ino = e->target_ino;
      entries[i].target_dev = e->target_dev;
      entries[i].spoofed_ino = e->spoofed_ino;
      entries[i].spoofed_dev = e->spoofed_dev;
      strscpy(entries[i].spoofed_pathname, e->spoofed_pathname, 256);
      entries[i].list.prev = baseline_rules.prev;
      entries[i].list.next = &baseline_rules;
      baseline_rules.prev->next = &entries[i].list;
      baseline_rules.prev = &entries[i].list;
    }
    int result = check_equivalence(snapshot, count);
    if (result)
      return result;
    for (unsigned int v = 0; v < ARRAY_SIZE(vmas); v++) {
      unsigned int reps = 262144 / vmas[v], bits = 0;
      unsigned int top = count;
      while (top) {
        bits++;
        top >>= 1;
      }
      volatile unsigned long long linear_checksum = 0, index_checksum = 0;
      baseline_visits = indexed_probes = 0;
      int locks = memory_lock_entries;
      clock_t started = clock();
      for (unsigned int rep = 0; rep < reps; rep++)
        for (unsigned int i = 0; i < vmas[v]; i++) {
          const struct baseline_entry *e =
              baseline_find_rule(i % (count + 7), i % 2 ? 7 : 8);
          if (e)
            linear_checksum += e->spoofed_ino;
        }
      double linear_seconds = (double)(clock() - started) / CLOCKS_PER_SEC;
      started = clock();
      for (unsigned int rep = 0; rep < reps; rep++)
        for (unsigned int i = 0; i < vmas[v]; i++) {
          const struct sumh_maps_rule_entry *e = sumh_maps_snapshot_lookup(
              snapshot, i % (count + 7), i % 2 ? 7 : 8);
          if (e)
            index_checksum += e->spoofed_ino;
        }
      double index_seconds = (double)(clock() - started) / CLOCKS_PER_SEC;
      unsigned long long lookups = (unsigned long long)reps * vmas[v];
      CHECK(linear_checksum == index_checksum &&
            baseline_visits == lookups * count);
      CHECK(indexed_probes <= lookups * bits * 2 &&
            memory_lock_entries == locks);
      printf("%u,%u,%u,%llu,%llu,%llu,%.9f,%.9f,%llu\n", count, vmas[v], reps,
             lookups, baseline_visits, indexed_probes, linear_seconds,
             index_seconds, index_checksum);
    }
    sumh_maps_store_clear();
    sumh_maps_snapshot_put(snapshot);
    CHECK(!physical_bytes && !stats().used && !allocation_live && !model_bad &&
          !memory_bad_context && !memory_lock_depth);
  }
  return 0;
}
