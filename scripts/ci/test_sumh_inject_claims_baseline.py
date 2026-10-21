"""Explicit retained-before-image counterexample; never a normal-source fallback."""
from test_sumh_inject_claims_model import model, fixtures
from test_sumh_inject_claims_source import read, body, wrapper
from test_sumh_vfs_helpers import COMMON


def source(root):
    from test_sumh_source_identity import MODEL, CASES
    types = read(root, "kernel/sumh/internal/sumh_types.h")
    if "inject_claim" in types:
        raise AssertionError("baseline must predate owned injection claims")
    layouts = types[types.index("struct sumh_entry {"):types.index("struct sumh_name_list {")]
    text = COMMON + model(MODEL, layouts)
    old = read(root, "kernel/sumh/core/sumh_runtime.c")
    start = old.index("static LLIST_HEAD(sumh_retired_entries)")
    end = old.index("static void sumh_entry_free(", start)
    text += old[start:end]
    for marker in (
        "void SUMH_NOCFI sumh_entry_release_source(", "static void sumh_entry_free(",
        "void sumh_entry_free_rcu(", "void sumh_hide_entry_free_rcu(",
        "void sumh_inject_entry_free_rcu(", "void sumh_xattr_sb_entry_free_rcu(",
        "static void sumh_merge_entry_free(", "void sumh_merge_entry_free_rcu(",
        "static void sumh_store_free_workfn(", "void sumh_store_drain(",
        "void sumh_spoof_kstat_entry_free_rcu(", "bool sumh_store_upsert(",
        "void sumh_cleanup_locked(", "SUMH_NOCFI void sumh_mark_dir_has_inject(",
    ):
        text += body(old, marker)
    text += body(read(root, "kernel/sumh/overlay/sumh_overlay.c"), "void sumh_add_inject_rule(")
    text += wrapper("SUMH_IOC_ADD_RULE", "submit_path", root)
    # These empty fixture-only declarations let shared setup/clean helpers
    # compile; no baseline production body knows or uses either object.
    text += "\nstatic struct hlist_head sumh_inject_inodes[MODEL_BUCKETS];\nstatic struct llist_head sumh_retired_inject_inodes;\n"
    text += fixtures(CASES)
    text += r"""
int main(void){
    setup(S_IFREG);CHECK(submit_path("/visible/file","/source")==0);
    CHECK(test_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT,&parent_i.i_mapping->flags));
    path_override("/visible",(struct path){&visible_m,&visible_d},0);
    set_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT,&visible_i.i_mapping->flags);
    mutex_lock(&sumh_config_mutex);sumh_cleanup_locked();mutex_unlock(&sumh_config_mutex);
    /* Intended baseline counterexample: retired parent A must be unmarked,
       and replacement B's unrelated marker must remain intact. */
    CHECK(!test_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT,&parent_i.i_mapping->flags)&&test_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT,&visible_i.i_mapping->flags));
    return 0;
}
"""
    # Legacy helper allocates while config is held and never owned an inode;
    # those known old-service violations are not the discriminator above.
    return text
