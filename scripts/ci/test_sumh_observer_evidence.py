#!/usr/bin/env python3
"""Run the independent observer evidence checker suite (host-only, no device)."""

import subprocess
import sys
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[2]
    return subprocess.run([sys.executable, "-m", "unittest", "discover", "-v",
                           "-s", str(root / "tests/sumh-observer/host"),
                           "-p", "test_compare.py"], cwd=root, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
