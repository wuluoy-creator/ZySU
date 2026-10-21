#!/usr/bin/env python3
"""Reject failing native fixture assertions even at POSIX exit-code boundaries."""
import argparse
import os
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, run_test


def check_failure(compiler, assertion_line):
    prefix = COMMON + "\nint main(void) {\n"
    source = prefix + "\n" * (assertion_line - 1 - prefix.count("\n"))
    source += "CHECK(false);\nreturn 0;\n}\n"
    assert source.splitlines()[assertion_line - 1] == "CHECK(false);"
    expected_status = (assertion_line - 1) % 255 + 1
    try:
        run_test(compiler, "host-harness-negative", source)
    except AssertionError as error:
        # Require both the portable nonzero code and its correct source mapping.
        if (f": exit {expected_status};" not in str(error)
                or f"{assertion_line}: CHECK(false);" not in str(error)):
            raise
    else:
        raise AssertionError(f"false CHECK at line {assertion_line} reported success")
    print(f"PASS host-harness-reject line={assertion_line} status={expected_status}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    for assertion_line in (256, 512, 768):
        check_failure(args.cc, assertion_line)
    run_test(args.cc, "host-harness-pass-control", COMMON + r"""
int main(void) {
    int calls=0;
    CHECK(++calls==1);
    CHECK(calls==1);
    CHECK(true);
    return 0;
}
""")


if __name__ == "__main__":
    main()
