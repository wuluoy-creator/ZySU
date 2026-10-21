#!/usr/bin/env python3
"""Run actual MERGE capture, ADD, bounded collector and recursive materializer.

Kernel services/source metadata capture are doubles. Source capture is tested
separately with production bodies. This is not tree coherence/RCU validation.
"""
import argparse
import os
import re
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test
from test_sumh_source_identity import CASES
from test_sumh_merge_install_model import MODEL, EXTRA
from test_sumh_merge_install_cases import TESTS


def source():
    from test_sumh_inject_claims_source import merge_source
    return merge_source() + TESTS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    run_test(args.cc, "merge-install", source())


if __name__ == "__main__":
    main()
