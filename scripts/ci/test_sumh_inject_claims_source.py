"""Strict extraction of production claim, store, control and overlay bodies."""
from pathlib import Path
import re

from test_sumh_vfs_helpers import COMMON, ROOT
from test_sumh_inject_claims_model import model, fixtures, remove_function, replace_once

INPUTS = (
    "kernel/sumh/internal/sumh_types.h", "kernel/sumh/core/sumh_store.h",
    "kernel/sumh/core/sumh_runtime.c", "kernel/sumh/control/sumh_ioctl.c",
    "kernel/sumh/overlay/sumh_overlay.c", "kernel/sumh/internal/sumh_base.h",
    "kernel/sumh/overlay/sumh_overlay.h",
)


def body(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    # Skip forward declarations rather than silently extracting the next body.
    if ";" in text[start:opening]:
        return body(text[opening + 1:], marker)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"


def read(root, path):
    return (Path(root) / path).read_text(encoding="utf-8")


def base(root=ROOT, merge=False, buckets=1):
    from test_sumh_source_identity import MODEL
    types = read(root, INPUTS[0])
    layouts = types[types.index("struct sumh_inject_claim;"):types.index("struct sumh_name_list {")]
    text = COMMON + f"\n#define MODEL_BUCKETS {buckets}\n" + model(MODEL, layouts)
    if merge:
        from test_sumh_merge_install_model import EXTRA
        extra = remove_function(EXTRA, "static void cond_resched(")
        extra = remove_function(extra, "static bool sumh_vnode_is_ours(")
        extra = re.sub(r"struct sumh_merge_entry \{[^\n]+\};\nstatic struct hlist_head sumh_merge_dirs\[1\];", "", extra)
        extra = replace_once(extra, "static bool vnode_source,trylock_fail;", "static bool trylock_fail;")
        extra = replace_once(extra, "e->source_path_valid=true;e->source_inode=", "e->source_path_valid=true;sumh_vnode_live_count++;e->source_inode=")
        text += extra
    return text + runtime(root)


def runtime(root=ROOT):
    source = read(root, INPUTS[2])
    start = source.index("static LLIST_HEAD(sumh_retired_entries)")
    end = source.index("/* SUMH_INJECT_CLAIMS_END */", start) + len("/* SUMH_INJECT_CLAIMS_END */")
    store_header = read(root, INPUTS[1])
    enum = store_header[store_header.index("enum sumh_inject_missing {"):]
    enum = enum[:enum.index("};") + 2]
    plan = body(store_header, "struct sumh_inject_attach_plan {") + ";\n"
    output = enum + "\n" + plan + "\nvoid sumh_inject_entry_free_rcu(struct rcu_head *head);\n" + source[start:end]
    for marker in (
        "void SUMH_NOCFI sumh_entry_release_source(",
        "static void sumh_entry_free(", "void sumh_entry_free_rcu(",
        "void sumh_hide_entry_free_rcu(", "void sumh_inject_entry_free_rcu(",
        "void sumh_xattr_sb_entry_free_rcu(", "static void sumh_merge_entry_free(",
        "void sumh_merge_entry_free_rcu(", "static void sumh_store_free_workfn(",
        "void sumh_store_drain(", "void sumh_spoof_kstat_entry_free_rcu(",
        "int sumh_store_upsert(", "void sumh_cleanup_locked(",
    ):
        output += "\n" + body(source, marker)
    return output


def normalizer(root=ROOT):
    text = read(root, INPUTS[3])
    return body(text, "static int SUMH_NOCFI sumh_resolve_rule_path(") + body(text, "static int sumh_normalize_rule_key(")


def wrapper(command, name, root=ROOT, arguments=None):
    text = read(root, INPUTS[3])
    start = text.index(f"\tcase {command}: {{")
    end = text.find("\n\tcase ", start + 1)
    default = text.find("\n\tdefault:", start + 1)
    if end < 0 or 0 <= default < end:
        end = default
    case = text[start:end]
    arguments = arguments or 'const char *input,const char *output'
    return f"\n#undef {command}\n#define {command} 100\nstatic int {name}({arguments}){{\n" + r"""
    char *src=kstrdup(input,GFP_KERNEL),*target=output?kstrdup(output,GFP_KERNEL):NULL;
    if(!src||(output&&!target)){kfree(src);kfree(target);return -ENOMEM;}
    struct {unsigned char type;} req={0};
    struct sumh_entry *entry;struct sumh_hide_entry *hide_entry;
    u32 hash=0;int ret=0;bool found=false;
""" + f"    switch({command}){{\n" + case + r"""
    }kfree(src);kfree(target);return ret;
}
"""


def overlay(root=ROOT):
    text = read(root, INPUTS[4])
    start = text.index("/* Materialize merge targets into native path rules")
    constants = "\n".join(re.findall(r"^#define SUMH_MERGE_(?:MAX_TARGETS|MAX_NAMES|MAX_RECORDS|MAX_PASSES|MEMORY_BUDGET) .+$", read(root, INPUTS[5]), re.M))
    if len(constants.splitlines()) != 5:
        raise AssertionError("incomplete materializer bounds")
    return constants + "\n" + body(text, "static bool sumh_merge_valid_name(") + text[start:]


def add_source(root=ROOT, buckets=1):
    from test_sumh_source_identity import CASES
    return (base(root, buckets=buckets) + normalizer(root)
            + wrapper("SUMH_IOC_ADD_RULE", "submit_path", root)
            + wrapper("SUMH_IOC_DEL_RULE", "delete_path", root)
            + '\nstatic int submit(void){return submit_path("/visible/file","/source");}\n'
            + fixtures(CASES))


def merge_source(root=ROOT):
    from test_sumh_source_identity import CASES
    return (base(root, merge=True) + normalizer(root) + overlay(root)
            + wrapper("SUMH_IOC_ADD_MERGE_RULE", "submit_merge_path", root)
            + wrapper("SUMH_IOC_ADD_RULE", "submit_path", root)
            + wrapper("SUMH_IOC_DEL_RULE", "delete_path", root)
            + '\nstatic int submit_merge(void){return submit_merge_path("/visible/file","/source");}\n'
            + '\nstatic int submit(void){return submit_path("/visible/file","/source");}\n'
            + fixtures(CASES))
