/* Preserved pre-Batch25 production selector for regression/performance
 * comparison. kernel/sumh/hooks/sumh_proc_read_hooks.c SHA256:
 * 2959a9e35d6209c4a9fb1251e91cff11b92c8aca8d263bdeb20eeeff34c1d4cf Function
 * SHA256: a113a1731fd256f36c68d14515e4466e11ace533eca13b270ece4d7a0065ea30 Only
 * symbol/type names and list iteration instrumentation are adapted below.
 */
struct baseline_list {
  struct baseline_list *next, *prev;
};
struct baseline_entry {
  struct baseline_list list;
  bool managed;
  unsigned long target_ino, target_dev, spoofed_ino, spoofed_dev;
  char spoofed_pathname[SUMH_MAX_LEN_PATHNAME];
};
static struct baseline_list baseline_rules;
static unsigned long long baseline_visits;
#define baseline_entry_from(ptr)                                               \
  ((struct baseline_entry *)((char *)(ptr) -                                   \
                             __builtin_offsetof(struct baseline_entry, list)))
#define list_for_each_entry(pos, head, member)                                 \
  for (pos = baseline_entry_from((head)->next);                                \
       &pos->member != (head) && (++baseline_visits, 1);                       \
       pos = baseline_entry_from(pos->member.next))
#define sumh_maps_rule_entry baseline_entry
#define sumh_maps_rules baseline_rules
#define sumh_maps_find_rule baseline_find_rule
static struct sumh_maps_rule_entry *sumh_maps_find_rule(unsigned long ino,
                                                        unsigned long dev) {
  struct sumh_maps_rule_entry *entry, *best = NULL;

  list_for_each_entry(entry, &sumh_maps_rules, list) {
    if (entry->target_ino != ino ||
        (entry->target_dev && entry->target_dev != dev))
      continue;
    if (!best || (!best->target_dev && entry->target_dev) ||
        (best->target_dev == entry->target_dev && best->managed &&
         !entry->managed))
      best = entry;
  }
  return best;
}

#undef sumh_maps_rule_entry
#undef sumh_maps_rules
#undef sumh_maps_find_rule
#undef list_for_each_entry
