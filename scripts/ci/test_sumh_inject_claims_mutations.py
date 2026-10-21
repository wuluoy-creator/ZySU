"""Generated-only behavioral mutations, each with a named invariant target."""
from test_sumh_inject_claims_model import replace_once
from test_sumh_inject_claims_source import body


def selected(text, function):
    start = text.rindex("\nint main(void)")
    return text[:start] + f"\nint main(void){{return {function}();}}\n"


def scoped(text, marker, old, new):
    original = body(text, marker).rstrip()
    changed = replace_once(original, old, new)
    return replace_once(text, original, changed)


def mutations(views):
    c, p, lifetime = views["claims"], views["producers"], views["lifetime"]
    edits = [
        ("swallow-add-normalizer-error", p, "normalization", "static int submit_path(",
         "ret = sumh_resolve_rule_path(&src, NULL, NULL);\n\t\tif (ret)\n\t\t\tbreak;",
         "ret = sumh_resolve_rule_path(&src, NULL, NULL);\n\t\tif (ret) ret = 0;", "submit()==expected"),
        ("detach-delayed-to-worker", p, "clear_owners", "void sumh_cleanup_locked(",
         "sumh_inject_detach_locked(entry->inject_claim);", "/* deferred incorrectly */", "!marked(&parent_i)&&marked(&visible_i)"),
        ("release-by-rebound-path", p, "clear_owners", "void sumh_inject_detach_locked(",
         "clear_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT,\n\t\t\t  &binding->inode->i_mapping->flags);",
         "sumh_clear_inode_flags_for_path(key->dir, AS_FLAGS_SUMH_DIR_HAS_INJECT);", "!marked(&parent_i)&&marked(&visible_i)"),
        ("first-owner-removes-shared-key", c, "sharing", "void sumh_inject_detach_locked(",
         "if (!--key->users)", "if (key->users-- > 0)", "owners()==1&&bindings()==1&&marked"),
        ("omit-existing-inode-aggregation", c, "retirement", "int sumh_inject_preflight_locked(",
         "bindings[i] = sumh_inject_inode_lookup_locked(\n\t\t    claim->spare_binding->inode);", "bindings[i] = NULL;", "live==live_before+1"),
        ("skip-identical-add-attach", p, "add_owners", "int sumh_store_upsert(",
         "sumh_inject_commit_locked(&injection);",
         "if (changed) sumh_inject_commit_locked(&injection);", "now!=old&&now->inject_claim!=prior"),
        ("skip-direct-add-owner", p, "add_owners", "static int submit_path(",
         "ret = sumh_inject_prepare_observed(\n\t\t    parent_dir, native_parent.dentry ? &native_parent : NULL,\n\t\t    SUMH_INJECT_MISSING_NONE, &new_entry->inject_claim);", "ret = 0;", "prior&&prior->attached"),
        ("skip-materialized-child-owner", p, "merge_owners", "static SUMH_NOCFI int sumh_add_path_entry(",
         "ret = sumh_inject_prepare_observed(src_prefix, &native_parent,\n\t\t\t\t\t   SUMH_INJECT_MISSING_NONE,\n\t\t\t\t\t   &entry->inject_claim);", "ret = 0;", "e->inject_claim&&e->inject_claim->attached"),
        ("skip-top-merge-owner", p, "merge_owners", "static int submit_merge_path(",
         "ret = sumh_inject_prepare_merge(src, &me->resolved_src,\n\t\t\t\t\t	me->inject_claims);", "ret = 0;", "table_count(sumh_merge_dirs)==6&&owner_rows()==1"),
        ("skip-nested-merge-owner", p, "merge_owners", "static SUMH_NOCFI int sumh_register_nested_merge(",
         "ret = sumh_inject_prepare_merge(src_str, &me->resolved_src,\n\t\t\t\t\tme->inject_claims);", "ret = 0;", "e->inject_claim&&e->inject_claim->attached"),
        ("per-claim-inode-overflow", c, "overflow", "int sumh_inject_preflight_locked(",
         "bindings[i]->users > UINT_MAX - additions", "bindings[i]->users > UINT_MAX - 1", "attach(c,2)==-EOVERFLOW"),
        ("per-claim-key-overflow", c, "overflow", "int sumh_inject_preflight_locked(",
         "keys[i]->users > UINT_MAX - additions", "keys[i]->users > UINT_MAX - 1", "attach(c,2)==-EOVERFLOW"),
        ("omit-mapping-row-headroom", p, "add_owners", "int sumh_inject_preflight_locked(",
         "INT_MAX - new_keys - extra_rows", "INT_MAX - new_keys", "submit()==-EOVERFLOW"),
        ("duplicate-equal-alias", c, "aliases", "int SUMH_NOCFI sumh_inject_prepare_merge(",
         "if (strcmp(key, text))", "if (true)", "c[0]&&!c[1]&&!resolved"),
        ("retain-unused-key-spare", c, "retirement", "void sumh_inject_finish_prepare(",
         "if (claim->spare_key) {", "if (false && claim->spare_key) {", "live==live_before+1"),
        ("release-spares-under-config", p, "add_owners", "static int submit_path(",
         "sumh_inject_finish_prepare(new_entry->inject_claim);", "mutex_lock(&sumh_config_mutex); sumh_inject_finish_prepare(new_entry->inject_claim); mutex_unlock(&sumh_config_mutex);", "balanced()"),
        ("busy-after-dirhijack", p, "deletion", "static int delete_path(",
         "ret = -EBUSY;", "ret = (sumh_dirhijack_del(src), -EBUSY);", "dh_deletes==0&&projected()==before"),
        ("late-worker-count-change", c, "retirement", "static void sumh_store_free_workfn(",
         "sumh_inject_inode_free(binding);", "atomic_dec(&sumh_rule_count); sumh_inject_inode_free(binding);", "logical()==before&&path_lookups==lookups"),
        ("late-worker-marker-clear", c, "retirement", "static void sumh_store_free_workfn(",
         "sumh_inject_inode_free(binding);", "clear_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT, &binding->inode->i_mapping->flags); sumh_inject_inode_free(binding);", "logical()==before&&path_lookups==lookups&&marked"),
        ("omit-third-queue", c, "retirement", "static void sumh_store_free_workfn(",
         "while ((head = llist_del_all(&sumh_retired_inject_inodes)) !=\n\t\t       NULL)", "while (false && (head = llist_del_all(&sumh_retired_inject_inodes)) != NULL)", "parent_i.refs==1"),
        ("reverse-native-release-order", c, "sharing", "static void sumh_inject_inode_free(",
         "iput(binding->inode);\n\tdeactivate_super(binding->sb);", "deactivate_super(binding->sb);\n\tiput(binding->inode);", "clean()"),
        ("enotdir-eacces-become-missing", c, "missing_errors", "int SUMH_NOCFI sumh_inject_prepare_directory(",
         "if (ret == -ENOENT)", "if (ret < 0)", "==errors[i]&&!c"),
        ("omit-aggregate-active-pin", lifetime, None, "static int SUMH_NOCFI sumh_inject_prepare_held(",
         "!atomic_inc_not_zero(&sb->s_active)", "false", "parent_i.refs==3&&parent_i.i_sb->s_active==3"),
        ("omit-aggregate-inode-pin", lifetime, None, "static int SUMH_NOCFI sumh_inject_prepare_held(",
         "ihold(inode);", "(void)inode;", "parent_i.refs==3&&parent_i.i_sb->s_active==3"),
        ("ignore-fop-install-failure", c, "failures", "static int SUMH_NOCFI sumh_inject_prepare_held(",
         "ret = sumh_fop_install(inode);", "(void)sumh_fop_install(inode); ret = 0;", "==errors[i]&&!c"),
    ]
    for name, text, case, marker, old, new, expected in edits:
        changed = scoped(text, marker, old, new)
        if case:
            changed = selected(changed, case)
        yield name, changed, expected
