#!/usr/bin/env python3
"""Write editable fixed-fixture baseline checks bound to exact request bytes."""

import argparse
import hashlib
import json
import sys
from pathlib import Path

import compare


def create(request_raw, roles, binding):
    request = compare.validate_request(request_raw)
    compare.require(roles and len(roles) <= 3 and len(set(roles)) == len(roles) and
                    all(role in compare.ROLES for role in roles), "select unique known roles")
    compare.require(binding in ("none", "direct_boot"), "unknown binding")
    checks = []
    for role in roles:
        if role == "isolated":
            # An isolated UID ordinarily cannot traverse the app's private path.
            # FD access is a separate explicit observation, not fresh-path access.
            targets = [("map.fd.read", "hex", (b"0123456789abcdef" * 256).hex(), "equals")]
        else:
            targets = [("fixture.marker", "hex", ("sumh-observer-fixture-v1\n" + request["run_id"] + "\n").encode().hex(), "equals"),
                       ("file.present.read", "hex", b"SUMH observer present v1\n".hex(), "equals"),
                       ("file.unicode.read", "hex", b"SUMH observer unicode v1\n".hex(), "equals"),
                       ("link.target", "hex", b"present.txt".hex(), "equals"),
                       ("file.missing.stat", "", 2, "errno")]
        for observation, field, expected, predicate in targets:
            checks.append({"id": role + "." + observation, "role": role, "observation": observation,
                           "source": compare.PROBES[observation][0], "field": field, "predicate": predicate,
                           "expected": expected, "baseline_required": False})
    result = {"schema_version": 1, "resource": "sumh_observer_expectations",
              "run_id": request["run_id"], "request_sha256": hashlib.sha256(request_raw).hexdigest(),
              "required_roles": roles, "required_binding": binding, "checks": checks}
    compare.validate_expectations(json.dumps(result).encode(), request_raw)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", type=Path, required=True)
    parser.add_argument("--role", choices=sorted(compare.ROLES), action="append", required=True)
    parser.add_argument("--binding", choices=("none", "direct_boot"), default="direct_boot")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        compare.require(args.out.resolve() != args.request.resolve() and
                        not (args.out.exists() and args.out.samefile(args.request)), "output must not overwrite request")
        result = create(compare.read_bounded(args.request, compare.REQUEST_LIMIT), args.role, args.binding)
        args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return 0
    except (compare.EvidenceError, OSError) as error:
        print("expectation input/output error: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
