#!/usr/bin/env python3
"""Execute actual SUMH retained-FOP code with tracked lifetime service doubles.

The production translation unit (including structs, install/free/client,
iterate, CLEAR/stop/exit code) is used verbatim except kernel includes. This
checks explicit schedules and reference accounting, not native RCU, unmount,
CFI, kernel memory ordering or a KMI build. Mutations affect generated source
only. An optional retained source runs the pre-fix last-native-ref counterexample.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, run_test
from test_sumh_fop_lifetime_model import MODEL, POSTLUDE
from test_sumh_fop_lifetime_cases import CASES, SCENARIOS, BASELINE_CASE, main as case_main

PRODUCTION = ROOT / "kernel/sumh/features/sumh_fop_override.c"
HEADER = ROOT / "kernel/sumh/features/sumh_fop_override.h"


def production_text(path=PRODUCTION):
    text = Path(path).read_text(encoding="utf-8")
    required = (
        "struct sumh_fop_template {", "struct sumh_fop_meta {",
        "sumh_fop_meta_free(", "sumh_fop_template_free(", "sumh_fop_install(",
        "sumh_fop_bind_iterate_client(", "sumh_fop_unbind_iterate_client(",
        "sumh_fop_uninstall_locked(", "sumh_fop_retire_list(",
        "sumh_fop_override_retire_all(", "sumh_fop_override_clear(",
        "sumh_fop_override_stop_new(", "sumh_fop_override_exit(",
    )
    for marker in required:
        if marker not in text:
            raise ValueError(f"production FOP body missing: {marker}")
    return re.sub(r"^#include[^\n]*\n", "", text, flags=re.MULTILINE)


def source(production=None, baseline=False):
    header = HEADER.read_text(encoding="utf-8")
    typedef = re.search(r"typedef int \(\*sumh_fop_iterate_client_fn\)[\s\S]*?;", header)
    if not typedef:
        raise ValueError("production client typedef missing")
    body = production_text() if production is None else production
    return COMMON + MODEL + typedef.group(0) + "\n" + body + POSTLUDE + (
        BASELINE_CASE if baseline else CASES + case_main()
    )


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"mutation anchor must occur once: {old!r} ({text.count(old)})")
    return text.replace(old, new, 1)


def mutations(production):
    edits = (
        ("missing-active-get", "!atomic_inc_not_zero(&sb->s_active)", "false"),
        ("revive-zero", "!atomic_inc_not_zero(&sb->s_active)", "(atomic_inc(&sb->s_active), false)"),
        ("missing-born-check", "!(READ_ONCE(sb->s_flags) & SB_BORN)", "false"),
        ("missing-active-put", "\t\tdeactivate_super(m->sb);", "\t\t(void)m->sb;"),
        ("active-put-before-inode", "\tif (m->inode)\n\t\tiput(m->inode);\n\tif (m->sb)\n\t\tdeactivate_super(m->sb);",
         "\tif (m->sb)\n\t\tdeactivate_super(m->sb);\n\tif (m->inode)\n\t\tiput(m->inode);"),
        ("missing-inode-get", "\tihold(inode);", "\t(void)inode;"),
        ("locked-super-release", "\tm->sb = sb;\n\tup_read(&sb->s_umount);", "\tm->sb = sb;"),
        ("leak-failed-candidate", "out_free:\n\tsumh_fop_meta_free(m);", "out_free:\n\tif (installed) sumh_fop_meta_free(m);"),
        ("free-under-spinlock", "out_unlock:\n\tspin_unlock(&sumh_fop_lock);\nout_free:\n\tsumh_fop_meta_free(m);",
         "out_unlock:\n\tsumh_fop_meta_free(m); m = NULL;\n\tspin_unlock(&sumh_fop_lock);\nout_free:\n\tsumh_fop_meta_free(m);"),
        ("discard-active-client", "if (!stop_new && READ_ONCE(m->iterate_client))", "if (false)"),
        ("overwrite-external-fops", "if (READ_ONCE(inode->i_fop) == sumh_fop_installed_table(m))", "if (true)"),
        ("missing-tasks-drain", "\tsynchronize_rcu_tasks();", "\t(void)0;"),
        ("missing-rcu-drain", "\tsynchronize_rcu();\n\tsumh_fop_synchronize_iterate_clients();\n\n\tlist_for_each_entry_safe (m, next, retiring, retire_node)",
         "\t(void)0;\n\tsumh_fop_synchronize_iterate_clients();\n\n\tlist_for_each_entry_safe (m, next, retiring, retire_node)"),
        ("missing-srcu-drain", "\tsumh_fop_synchronize_iterate_clients();\n\n\tlist_for_each_entry_safe (m, next, retiring, retire_node)",
         "\t(void)0;\n\n\tlist_for_each_entry_safe (m, next, retiring, retire_node)"),
        ("missing-module-put", "\t\tmodule_put(fop->owner);", "\t\t(void)fop->owner;"),
        ("wrong-unbind-identity", "if (m && (!data || READ_ONCE(m->iterate_client_data) == data))", "if (m)"),
        ("reopen-after-stop", "if (!READ_ONCE(sumh_fop_override_stopped))", "if (true)"),
    )
    for name, old, new in edits:
        yield name, replace_once(production, old, new)


def expect_rejected(compiler, name, generated):
    # Compilation errors are not a killed behavioral mutation. Only a compiled
    # executable rejected by its checks is accepted as a counterexample.
    try:
        run_test(compiler, name, generated)
    except AssertionError as error:
        print(f"PASS rejected {name}: {error}")
        return
    raise AssertionError(f"unexpectedly accepted counterexample: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    parser.add_argument("--baseline", type=Path, help="retained pre-fix production C file; must reject last-native-ref invariant")
    parser.add_argument("--skip-mutations", action="store_true", help="run the real body only (development convenience)")
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    production = production_text()
    run_test(compiler, "fop-lifetime", source(production))
    print(f"PASS {len(SCENARIOS)} named scenarios, including 1000 actual install/CLEAR reuse cycles")
    for name in SCENARIOS:
        print(f"  scenario {name}")
    if not args.skip_mutations:
        count = 0
        for name, changed in mutations(production):
            expect_rejected(compiler, "fop-lifetime-" + name, source(changed))
            count += 1
        print(f"PASS {count} compiled behavioral mutations rejected")
    if args.baseline:
        baseline = args.baseline.resolve()
        expect_rejected(compiler, "fop-lifetime-retained-baseline", source(production_text(baseline), baseline=True))
        print(f"retained baseline SHA256 {hashlib.sha256(baseline.read_bytes()).hexdigest()}")
    print(f"production SHA256 {hashlib.sha256(PRODUCTION.read_bytes()).hexdigest()}")
    print("LIMIT: modeled refs, explicit schedules and kernel services; no native unmount/RCU/CFI/KMI result")


if __name__ == "__main__":
    main()
