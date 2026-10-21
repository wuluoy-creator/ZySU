"""Generated faults in production bodies; each selects one invariant case."""

from test_sumh_dh_ownership_source import function, replace_once


def selected(text, case):
    start = text.rindex("\nint main(void)")
    return text[:start] + f"\nint main(void){{return {case}();}}\n"


def scoped(text, marker, old, new):
    original = function(text, marker)
    return replace_once(text, original, replace_once(original, old, new))


def mutations(views):
    c, p, life, managed, su, boot = (views[k] for k in
        ("owners", "producers", "lifetime", "managed", "su", "bootstrap"))
    edits = [
        ("new-owner-loses-order", c, "exact_parent_and_order", "void sumh_dh_change_commit(",
         "list_add(&claim->owner_node, &slot->owners);", "list_add_tail(&claim->owner_node, &slot->owners);", "committed(b,NULL)==0&&winner(1,\"file\")==502"),
        ("withdrawal-loses-survivor", c, "exact_parent_and_order", "static void sumh_dh_detach_locked(",
         "sumh_dh_select_winner(slot);", "RCU_INIT_POINTER(slot->winner, NULL);", "c->state==DH_DETACHED&&winner(1,\"file\")==502"),
        ("replacement-keeps-old-owner", p, "replace_and_failure_case", "void sumh_dh_change_commit(",
         "sumh_dh_detach_locked(change->remove, retired);", "/* omitted */", "same!=old&&same->dh_claim!=old_claim"),
        ("topology-survivor-lost", c, "topology_and_absence", "static void sumh_dh_detach_locked(",
         "sumh_dh_select_winner(slot);", "if (claim->kind != SUMH_DH_OWNER_VTOPO) sumh_dh_select_winner(slot);", "a->slot->winner==b->payload&&!strcmp"),
        ("managed-overlay-count-lost", c, "replacement_and_managed_overlay", "void sumh_dh_change_commit(",
         "slot->attached++;", "slot->attached++; if (claim->kind != SUMH_DH_OWNER_MANAGED_HIDE) slot->hide_users=0;", "a->state==DH_DETACHED&&winner"),
        ("claim-drain-omits-barrier", p, "producer_smoke", "void sumh_dh_drain_releases(",
         "rcu_barrier();", "/* omitted second stage */", "atomic_read(&sumh_sop_live)==0"),
        ("claim-drain-omits-work", p, "producer_smoke", "void sumh_dh_drain_releases(",
         "flush_work(&sumh_dh_release_work);", "/* omitted work */", "atomic_read(&sumh_sop_live)==0"),
        ("claim-callback-omits-enqueue", p, "producer_smoke", "static void sumh_dh_claim_free_rcu(",
         "schedule_work(&sumh_dh_release_work);", "/* omitted enqueue */", "atomic_read(&sumh_sop_live)==0"),
        ("store-drain-omits-work", p, "producer_smoke", "void sumh_store_drain(",
         "flush_work(&sumh_store_free_work);", "/* omitted work */", "callback_count>0&&sumh_sop_lookup_locked"),
        ("late-sop-reap-omitted", c, "late_release_and_reap", "static void sumh_dh_dir_put(",
         "sumh_sop_shadow_reap();", "/* omitted late reap */", "atomic_read(&sumh_sop_live)==0&&fixture_sb.s_op"),
        ("native-parent-protection-reversed", life, "dop_worker_owned_directory", "static void sumh_dh_dir_put(",
         "iput(dir->dir_inode);\n\tif (dir->sop_client)\n\t\tsumh_sop_shadow_unregister_dh(sb);", "if (dir->sop_client) sumh_sop_shadow_unregister_dh(sb);\n\tiput(dir->dir_inode);", "!bad&&!live_allocations"),
        ("active-su-cleared", c, "clear_candidates_and_su", "void sumh_dirhijack_clear(",
         "if (READ_ONCE(sumh_dh_ready) &&", "if (false && READ_ONCE(sumh_dh_ready) &&", "a->state==DH_DETACHED&&b->state==DH_INVALIDATED"),
        ("slot-reference-overflow-unchecked", c, "failure_and_capacity", "static int sumh_dh_prepare_observed(",
         "refcount_read(&slot->refs) >= INT_MAX - 4", "false", "refcount_read(&a->slot->refs)==INT_MAX-4"),
        ("directory-reference-overflow-unchecked", c, "failure_and_capacity", "static int sumh_dh_prepare_observed(",
         "refcount_read(&dir->refs) >= INT_MAX - 4", "false", "prepared(\"/one/other\",SUMH_DH_OWNER_MAPPING,17,502,&b)==-EOVERFLOW&&!b"),
        ("attached-overflow-unchecked", c, "failure_and_capacity", "int sumh_dh_change_begin(",
         "slot->attached == UINT_MAX", "false", "sumh_dh_change_begin(b,a,&change)==-EOVERFLOW"),
        ("su-exclusivity-omitted", c, "clear_candidates_and_su", "int sumh_dh_change_begin(",
         "owner->kind == SUMH_DH_OWNER_SU ||", "false ||", "sumh_dh_change_begin(fresh,NULL,&change)==-EPERM"),
        ("invalidated-abort-leaks", c, "clear_candidates_and_su", "void sumh_dh_abort(",
         "if (!claim)\n\t\treturn;", "if (!claim || claim->state == DH_INVALIDATED)\n\t\treturn;", "!bad&&!live_allocations"),
        ("bulk-uses-two-records", c, "bulk_and_reuse", "static void sumh_dh_record_invalidation(",
         "if (retired->bulk)", "if (false)", "retired.count==0&&!list_empty"),
        ("managed-generation-guard-omitted", managed, "managed_policy_cases", "static void hide_workfn(",
         "rule->generation != generation ||", "false ||", "generation_retry_seen==1"),
        ("managed-disable-guard-omitted", managed, "managed_policy_cases", "static void hide_workfn(",
         "rule->generation != generation || !READ_ONCE(sumh_enabled)", "rule->generation != generation", "!sumh_enabled&&model_rule->binding"),
        ("su-stale-inode-admitted", su, "su_public_cases", "bool ksu_sucompat_vfs_current_ino(",
         "ino == READ_ONCE(su_ino)", "READ_ONCE(su_ino)", "!ksu_sucompat_vfs_current_ino(first)"),
        ("direct-stop-omits-provider", boot, "direct_stop_case", "static void sumh_stop_views(",
         "ksu_sucompat_sumh_exit();", "/* omitted provider stop */", "exec_stop_order&&exec_stop_order<managed_stop_order"),
        ("vpath-enomem-is-ignored", life, "vpath_projection_failure", "static struct dentry *\nsumh_dh_materialize(",
         "ret = -ENOMEM;", "ret = 0;", "IS_ERR(res)&&PTR_ERR(res)==-ENOMEM"),
        ("direct-second-parent-observation", p, "shared_parent_case", "static int add_rule(",
         "ret = sumh_inject_prepare_observed(", "path_put(&native_parent); (void)kern_path(parent_dir, LOOKUP_FOLLOW|LOOKUP_DIRECTORY, &native_parent);\n\t\tret = sumh_inject_prepare_observed(", "e->inject_claim->binding->inode==&fixture_inodes[1]"),
        ("materializer-second-parent-observation", views["materializer"], "mat_transactions", "static SUMH_NOCFI int sumh_add_path_entry(",
         "ret = sumh_inject_prepare_observed(src_prefix, &native_parent,", "path_put(&native_parent); (void)kern_path(src_prefix, LOOKUP_FOLLOW|LOOKUP_DIRECTORY, &native_parent);\n\tret = sumh_inject_prepare_observed(src_prefix, &native_parent,", "e&&mat_parent_observations==1"),
        ("sop-client-overflow-unchecked", c, "failure_and_capacity", "int sumh_sop_shadow_register_dh(",
         "m->dh_clients == UINT_MAX", "false", "prepared(\"/two/file\",SUMH_DH_OWNER_MAPPING,17,502,&b)==-EOVERFLOW"),
        ("runtime-clear-closes-su", boot, "runtime_clear_su_case", "static int clear_rules(",
         "sumh_hide_rules_clear();", "ksu_sucompat_vfs_exit(); sumh_hide_rules_clear();", "sumh_started&&sumh_is_ready()&&sumh_enabled"),
    ]
    for name, text, case, marker, old, new, target in edits:
        changed = selected(scoped(text, marker, old, new), case)
        # Some resource-invariant checks are in the shared terminal helper.
        # Wrap that helper into the selected scenario's body for one explicit
        # assertion target; no product body or native service is changed here.
        if target == "!bad&&!live_allocations":
            changed = scoped(changed, f"static int {case}(", "return finish_case();",
                             "int finish=finish_case();CHECK(!bad&&!live_allocations);return finish;")
        yield name, changed, target
    changed = scoped(life, "static int sumh_dh_set_dentry_ops(",
                     "char name[NAME_MAX + 1];", "char *name = (char *)dentry->d_name.name;")
    changed = scoped(changed, "static int sumh_dh_set_dentry_ops(",
                     "memcpy(name, dentry->d_name.name, len);", "/* incorrectly retain borrowed name */")
    yield "borrowed-name-after-allocation", selected(changed, "name_capture_case"), "ret==-EAGAIN&&!retired_name_reads&&!fixture_dentries[20].d_op"
