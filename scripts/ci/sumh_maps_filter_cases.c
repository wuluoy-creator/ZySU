static struct sumh_maps_rule changed_rules[2];
static void install_changed_rules(void) {
  struct sumh_maps_snapshot *prepared = NULL;
  if (sumh_maps_prepare_managed(changed_rules, 2, &prepared)) {
    model_bad++;
    return;
  }
  sumh_maps_publish(prepared);
}
static void make_filter_rule(struct sumh_maps_rule *rule, unsigned long ino,
                             unsigned long spoof, const char *path) {
  memset(rule, 0, sizeof(*rule));
  rule->target_ino = ino;
  rule->target_dev = MKDEV(8, 1);
  rule->spoofed_ino = spoof;
  rule->spoofed_dev = MKDEV(9, 2);
  strscpy(rule->spoofed_pathname, path, sizeof(rule->spoofed_pathname));
}
int main(void) {
  const char *input = "00001000-00002000 r--p 00000000 08:01 7 /original-one\n"
                      "Size:                  4 kB\n"
                      "00003000-00004000 r-xp 00001000 08:01 8 /original-two\n";
  char output[2048];
  size_t written;
  bool changed, valid;
  struct sumh_maps_snapshot *old, *empty, *prepared;
  empty = sumh_maps_snapshot_get();
  CHECK(!empty);
  make_filter_rule(&changed_rules[0], 7, 700, "/old-one");
  make_filter_rule(&changed_rules[1], 8, 800, "/old-two");
  install_changed_rules();
  old = sumh_maps_snapshot_get();
  int locks = memory_lock_entries;
  CHECK(!sumh_filter_maps_lines(input, strlen(input), output, sizeof(output),
                                &written, &changed, &valid,
                                SUMH_POLICY_SCOPE_SPOOF, old));
  CHECK(valid && changed && contains(output, written, "09:02 700 /old-one\n") &&
        contains(output, written, "09:02 800 /old-two\n"));
  CHECK(contains(output, written, "Size:                  4 kB\n") &&
        memory_lock_entries == locks);
  /* Replacement occurs while the second VMA is being parsed. */
  changed_rules[0].spoofed_ino = 701;
  changed_rules[1].spoofed_ino = 801;
  strscpy(changed_rules[0].spoofed_pathname, "/new-one", 256);
  strscpy(changed_rules[1].spoofed_pathname, "/new-two", 256);
  parse_calls = 0;
  parse_schedule = 7;
  parse_interleave = install_changed_rules;
  CHECK(!sumh_filter_maps_lines(input, strlen(input), output, sizeof(output),
                                &written, &changed, &valid,
                                SUMH_POLICY_SCOPE_SPOOF, old));
  CHECK(!parse_interleave && valid &&
        contains(output, written, "700 /old-one") &&
        contains(output, written, "800 /old-two"));
  CHECK(!contains(output, written, "701 /new-one") &&
        !contains(output, written, "801 /new-two"));
  prepared = sumh_maps_snapshot_get();
  CHECK(sumh_maps_snapshot_lookup(prepared, 7, MKDEV(8, 1))->spoofed_ino ==
        701);
  sumh_maps_snapshot_put(prepared);
  /* Same retained collection on a caller's output growth retry. */
  CHECK(sumh_filter_maps_lines(input, strlen(input), output, 40, &written,
                               &changed, &valid, SUMH_POLICY_SCOPE_SPOOF,
                               old) == -ENOSPC);
  changed_rules[0].spoofed_ino = 702;
  changed_rules[1].spoofed_ino = 802;
  install_changed_rules();
  CHECK(!sumh_filter_maps_lines(input, strlen(input), output, sizeof(output),
                                &written, &changed, &valid,
                                SUMH_POLICY_SCOPE_SPOOF, old));
  CHECK(valid && contains(output, written, "700 /old-one") &&
        contains(output, written, "800 /old-two"));
  /* Empty means empty for this reader despite all later publications. */
  CHECK(!sumh_filter_maps_lines(input, strlen(input), output, sizeof(output),
                                &written, &changed, &valid,
                                SUMH_POLICY_SCOPE_SPOOF, empty));
  CHECK(valid && !changed && written == strlen(input) &&
        !memcmp(output, input, written));
  CHECK(!sumh_filter_maps_lines(input, strlen(input), output, sizeof(output),
                                &written, &changed, &valid,
                                SUMH_POLICY_SCOPE_VIEW, old));
  CHECK(valid && !changed && written == strlen(input) &&
        !memcmp(output, input, written));
  /* Clear retires the current set without revoking previously pinned data. */
  sumh_maps_store_clear();
  CHECK(!sumh_maps_snapshot_get());
  CHECK(!sumh_filter_maps_lines(input, strlen(input), output, sizeof(output),
                                &written, &changed, &valid,
                                SUMH_POLICY_SCOPE_SPOOF, old));
  CHECK(valid && changed && contains(output, written, "800 /old-two"));
  CHECK(sumh_filter_maps_lines(input, strlen(input) - 1, output, sizeof(output),
                               &written, &changed, &valid,
                               SUMH_POLICY_SCOPE_SPOOF, old) == -EINVAL &&
        !valid);
  const char *malformed =
      "00001000-bad header with enough padding to reach forty characters\n";
  CHECK(sumh_filter_maps_lines(malformed, strlen(malformed), output,
                               sizeof(output), &written, &changed, &valid,
                               SUMH_POLICY_SCOPE_SPOOF, old) == -EINVAL &&
        !valid);
  CHECK(sumh_filter_maps_lines(NULL, 0, output, sizeof(output), &written,
                               &changed, &valid, SUMH_POLICY_SCOPE_SPOOF,
                               old) == -EINVAL);
  CHECK(sumh_filter_maps_lines(input, strlen(input), NULL, sizeof(output),
                               &written, &changed, &valid,
                               SUMH_POLICY_SCOPE_SPOOF, old) == -EINVAL);
  CHECK(sumh_filter_maps_lines(input, strlen(input), output, sizeof(output),
                               NULL, &changed, &valid, SUMH_POLICY_SCOPE_SPOOF,
                               old) == -EINVAL);
  sumh_maps_snapshot_put(old);
  sumh_maps_snapshot_put(empty);
  CHECK(!physical_bytes && !allocation_live && !stats().used && !model_bad &&
        !memory_bad_context && !memory_lock_depth);
  /* Empty replacement pathname must remove the original filename. */
  make_filter_rule(&changed_rules[0], 7, 7, "");
  changed_rules[0].spoofed_dev = MKDEV(8, 1);
  CHECK(!sumh_maps_prepare_managed(changed_rules, 1, &prepared));
  sumh_maps_publish(prepared);
  old = sumh_maps_snapshot_get();
  CHECK(!sumh_filter_maps_lines(input, strlen(input), output, sizeof(output),
                                &written, &changed, &valid,
                                SUMH_POLICY_SCOPE_SPOOF, old));
  CHECK(changed && valid && !contains(output, written, "/original-one") &&
        contains(output, written, "08:01 7 \n"));
  CHECK(contains(output, written, "/original-two"));
  sumh_maps_store_clear();
  sumh_maps_snapshot_put(old);
  CHECK(!physical_bytes && !allocation_live && !stats().used && !model_bad &&
        !memory_bad_context && !memory_lock_depth);
  return 0;
}
