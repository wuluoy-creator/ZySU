#!/usr/bin/env python3
"""Validate bounded SUMH observer evidence without invoking a device."""

import argparse
import hashlib
import json
import math
import re
import sys
from pathlib import Path

REQUEST_LIMIT = 32768
SAMPLE_LIMIT = 262144
EXPECTATION_LIMIT = 262144
ROLES = {"ordinary", "isolated", "root"}
STAT_KEYS = {"dev", "ino", "mode", "size", "mtime_ns"}
REQUIRED = {
    "identity.start_ticks": ("self", "decimal"),
    "identity.selinux": ("self", "bytes"),
    "boot.id": ("self", "bytes"),
    **{f"namespace.{end}.{kind}": ("self", "bytes" if kind == "link" else "stat")
       for end in ("start", "end") for kind in ("link", "stat", "fd")},
    "fixture.marker": ("path", "bytes"),
    "file.present.stat": ("path", "stat"),
    "file.present.open_stat": ("path", "stat"),
    "file.present.read": ("path", "bytes"),
    "file.missing.stat": ("path", "stat"),
    "file.unicode.read": ("path", "bytes"),
    "directory.entries": ("path", "directory"),
    "link.target": ("path", "bytes"),
    "map.path.stat": ("path", "stat"),
    "map.path.mapping": ("path", "mapping"),
    "map.path.maps": ("self", "maps"),
    "mount.info": ("self", "mount"),
    "mount.mounts": ("self", "mount"),
}
DELEGATED = {f"map.fd.{kind}": ("self" if kind == "maps" else "delegated_fd", shape)
             for kind, shape in (("stat", "stat"), ("read", "bytes"),
                                 ("mapping", "mapping"), ("maps", "maps"))}
PROBES = dict(REQUIRED, **DELEGATED)
DECIMAL = re.compile(r"(?:0|[1-9][0-9]*)\Z")
SIGNED = re.compile(r"(?:0|-?[1-9][0-9]*)\Z")
HEX = re.compile(r"[0-9a-f]*\Z")
UUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\Z")


class EvidenceError(ValueError):
    """Malformed or mismatched evidence, distinct from a measured mismatch."""


def require(condition, message):
    if not condition:
        raise EvidenceError(message)


def keys(value, expected, where):
    require(type(value) is dict, f"{where}: expected object")
    require(set(value) == set(expected),
            f"{where}: missing/unknown keys ({sorted(set(expected) - set(value))}; "
            f"{sorted(set(value) - set(expected))})")


def string(value, where, limit=1024, nonempty=False):
    require(type(value) is str, f"{where}: expected string")
    try:
        size = len(value.encode("utf-8"))
    except UnicodeEncodeError as error:
        raise EvidenceError(f"{where}: invalid Unicode") from error
    require(size <= limit and (not nonempty or size > 0), f"{where}: string length")
    require("\x00" not in value, f"{where}: NUL forbidden")


def decimal(value, where, signed=False):
    # Native stat timestamps use decimal arithmetic, so a time_t times 10^9
    # can exceed int64 without loss. All other integer fields remain u64.
    require(type(value) is str and len(value) <= (29 if signed else 20) and
            (SIGNED if signed else DECIMAL).fullmatch(value), f"{where}: decimal string required")
    number = int(value)
    lower, upper = ((-(1 << 63) * 10**9, (1 << 63) * 10**9 - 1) if signed else (0, (1 << 64) - 1))
    require(lower <= number <= upper, f"{where}: integer range")
    return number


def hexstring(value, where, size=None, max_bytes=65536):
    require(type(value) is str and len(value) % 2 == 0 and HEX.fullmatch(value),
            f"{where}: lower-case byte hex required")
    require(len(value) <= 2 * max_bytes and (size is None or len(value) == size * 2),
            f"{where}: hex length")
    return bytes.fromhex(value)


def integer(value, where, lower=0, upper=(1 << 31) - 1):
    require(type(value) is int and lower <= value <= upper, f"{where}: integer range")


def parse_bytes(raw, limit, label):
    require(type(raw) is bytes and len(raw) <= limit, f"{label}: byte limit exceeded")

    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, f"{label}: duplicate key {key!r}")
            result[key] = value
        return result

    def no_constant(value):
        raise EvidenceError(f"{label}: nonfinite number {value}")

    def finite_float(value):
        number = float(value)
        require(math.isfinite(number), f"{label}: nonfinite number")
        return number

    def validate_unicode(value):
        if isinstance(value, str):
            value.encode("utf-8")
        elif isinstance(value, dict):
            for key, item in value.items():
                key.encode("utf-8")
                validate_unicode(item)
        elif isinstance(value, list):
            for item in value:
                validate_unicode(item)

    try:
        parsed = json.loads(raw.decode("utf-8"), object_pairs_hook=pairs,
                            parse_constant=no_constant, parse_float=finite_float)
        validate_unicode(parsed)
        return parsed
    except (UnicodeError, ValueError, RecursionError) as error:
        if isinstance(error, EvidenceError):
            raise
        raise EvidenceError(f"{label}: invalid UTF-8 JSON ({error})") from error


def read_bounded(path, limit):
    with Path(path).open("rb") as stream:
        raw = stream.read(limit + 1)
    require(len(raw) <= limit, f"{path}: byte limit exceeded")
    return raw


def validate_context(value):
    keys(value, {"boot_id", "config_revision"}, "context")
    if value["boot_id"] is not None:
        require(type(value["boot_id"]) is str and UUID.fullmatch(value["boot_id"]),
                "context.boot_id: canonical UUID required")
    if value["config_revision"] is not None:
        require(type(value["config_revision"]) is str and
                re.fullmatch(r"sha256-v1:[0-9a-f]{64}", value["config_revision"]),
                "context.config_revision: sha256-v1 digest required")


def validate_request(raw):
    request = parse_bytes(raw, REQUEST_LIMIT, "request")
    keys(request, {"schema_version", "resource", "run_id", "fixture_root", "context"}, "request")
    require(type(request["schema_version"]) is int and request["schema_version"] == 1,
            "request: unsupported schema")
    require(request["resource"] == "sumh_observer_request", "request: wrong resource")
    hexstring(request["run_id"], "request.run_id", 16)
    string(request["fixture_root"], "request.fixture_root", 1024, True)
    prefix = r"(?:/data/user(?:_de)?/(?:0|[1-9][0-9]{0,9})/com\.zying\.zysu\.sumhobservertest/files|/data/local/tmp)"
    require(re.fullmatch(prefix + r"/sumh-observer/" + request["run_id"], request["fixture_root"]),
            "request: fixture root is not an allowed canonical synthetic root")
    validate_context(request["context"])
    return request


def manifest_digest(request):
    return hashlib.sha256(("sumh-observer-fixture-v1\n" + request["run_id"] + "\n").encode()).hexdigest()


def validate_value(value, shape, where):
    if shape == "decimal":
        decimal(value, where)
    elif shape == "bytes":
        keys(value, {"hex"}, where)
        hexstring(value["hex"], where + ".hex", max_bytes=4096)
    elif shape == "stat":
        keys(value, STAT_KEYS, where)
        for key in STAT_KEYS:
            decimal(value[key], where + "." + key, key == "mtime_ns")
    elif shape == "directory":
        keys(value, {"entries", "sorted"}, where)
        require(value["sorted"] is False, where + ": sorted must be false")
        require(type(value["entries"]) is list and len(value["entries"]) <= 256,
                where + ": directory capacity")
        for entry in value["entries"]:
            keys(entry, {"name_hex", "ino", "type"}, where + ".entry")
            name = hexstring(entry["name_hex"], where + ".name_hex", max_bytes=255)
            require(name and name not in (b".", b"..") and b"/" not in name and b"\x00" not in name,
                    where + ": invalid name")
            decimal(entry["ino"], where + ".ino")
            integer(entry["type"], where + ".type", 0, 255)
    elif shape == "mapping":
        keys(value, {"address_start", "address_end", "length"}, where)
        for key in ("address_start", "address_end"):
            require(type(value[key]) is str and 1 <= len(value[key]) <= 16 and HEX.fullmatch(value[key]),
                    where + ": address hex")
        require(value["length"] == "4096" and
                int(value["address_end"], 16) - int(value["address_start"], 16) == 4096,
                where + ": invalid mapping interval")
    elif shape in ("maps", "mount"):
        keys(value, {"lines_hex", "scanned_bytes"} | ({"matched_mapping"} if shape == "maps" else set()), where)
        require(type(value["lines_hex"]) is list, where + ": expected line array")
        total = sum(len(hexstring(line, where + ".line")) for line in value["lines_hex"])
        scanned = decimal(value["scanned_bytes"], where + ".scanned_bytes")
        require(total <= scanned <= 65536, where + ": proc capacity/count")
        if shape == "maps":
            require(type(value["matched_mapping"]) is bool, where + ": expected boolean")


def validate_observation(value):
    keys(value, {"id", "source", "status", "errno", "complete", "value"}, "observation")
    probe = value["id"]
    require(type(probe) is str and probe in PROBES, "observation: unknown id")
    source, shape = PROBES[probe]
    require(value["source"] == source, probe + ": wrong source")
    status = value["status"]
    require(status in ("ok", "error", "truncated", "unsupported", "not_run"), probe + ": status")
    require(type(value["complete"]) is bool, probe + ": complete must be boolean")
    error = value["errno"]
    if error is not None:
        integer(error, probe + ".errno", 1, 4095)
    if status == "ok":
        require(value["complete"] and error is None, probe + ": invalid success")
        validate_value(value["value"], shape, probe)
    elif status == "error":
        require(value["complete"] and error is not None and value["value"] is None,
                probe + ": invalid completed error")
    elif status in ("truncated", "not_run"):
        require(not value["complete"], probe + ": incomplete status marked complete")
        if status == "not_run":
            require(error is None and value["value"] is None, probe + ": not_run errno/value must be null")
        elif value["value"] is not None:
            validate_value(value["value"], shape, probe)
    else:
        require(not value["complete"] and value["value"] is None,
                probe + ": unsupported must be incomplete with null value")


def validate_sample(raw, request_raw):
    request = validate_request(request_raw)
    sample = parse_bytes(raw, SAMPLE_LIMIT, "sample")
    keys(sample, {"schema_version", "resource", "run_id", "request_sha256", "fixture_manifest_sha256",
                  "collector_build_id", "requested_role", "observer_instance_id", "non_atomic",
                  "started_boottime_ns", "finished_boottime_ns", "identity", "context", "observations"}, "sample")
    require(type(sample["schema_version"]) is int and sample["schema_version"] == 1,
            "sample: unsupported schema")
    require(sample["resource"] == "sumh_observer_sample", "sample: wrong resource")
    require(sample["run_id"] == request["run_id"], "sample: run mismatch")
    require(sample["request_sha256"] == hashlib.sha256(request_raw).hexdigest(), "sample: request digest mismatch")
    require(sample["fixture_manifest_sha256"] == manifest_digest(request), "sample: fixture digest mismatch")
    string(sample["collector_build_id"], "collector_build_id", 1024, True)
    require(type(sample["requested_role"]) is str and sample["requested_role"] in ROLES, "sample: role")
    hexstring(sample["observer_instance_id"], "observer_instance_id", 16)
    require(sample["non_atomic"] is True, "sample: non_atomic must be true")
    start = decimal(sample["started_boottime_ns"], "started_boottime_ns")
    end = decimal(sample["finished_boottime_ns"], "finished_boottime_ns")
    require(end >= start, "sample: reversed times")
    keys(sample["identity"], {"uid", "euid", "gid", "egid", "pid", "ppid", "tid", "groups"}, "identity")
    for key, value in sample["identity"].items():
        if key != "groups":
            decimal(value, "identity." + key)
    require(int(sample["identity"]["pid"]) > 0 and int(sample["identity"]["tid"]) > 0,
            "identity: invalid pid/tid")
    groups = sample["identity"]["groups"]
    require(type(groups) is list and len(groups) <= 65536, "identity.groups: capacity/type")
    for group in groups:
        decimal(group, "identity.groups")
    validate_context(sample["context"])
    require(sample["context"] == request["context"], "sample: context mismatch")
    require(type(sample["observations"]) is list and len(sample["observations"]) <= len(PROBES),
            "sample: observations capacity/type")
    seen = set()
    for observation in sample["observations"]:
        validate_observation(observation)
        require(observation["id"] not in seen, "sample: duplicate observation")
        seen.add(observation["id"])
    return sample


def validate_expectations(raw, request_raw):
    expected = parse_bytes(raw, EXPECTATION_LIMIT, "expectations")
    request = validate_request(request_raw)
    keys(expected, {"schema_version", "resource", "run_id", "request_sha256", "required_roles", "required_binding", "checks"}, "expectations")
    require(type(expected["schema_version"]) is int and expected["schema_version"] == 1,
            "expectations: unsupported schema")
    require(expected["resource"] == "sumh_observer_expectations", "expectations: resource")
    require(expected["run_id"] == request["run_id"] and
            expected["request_sha256"] == hashlib.sha256(request_raw).hexdigest(), "expectations: request mismatch")
    roles = expected["required_roles"]
    require(type(roles) is list and 1 <= len(roles) <= 3 and all(type(role) is str and role in ROLES for role in roles)
            and len(set(roles)) == len(roles), "expectations: roles")
    require(expected["required_binding"] in ("none", "direct_boot"), "expectations: binding")
    checks = expected["checks"]
    require(type(checks) is list and 1 <= len(checks) <= 1024, "expectations: checks capacity/type")
    seen = set()
    for check in checks:
        keys(check, {"id", "role", "observation", "source", "field", "predicate", "expected", "baseline_required"}, "check")
        string(check["id"], "check.id", 128, True)
        require(check["id"] not in seen, "check: duplicate id")
        seen.add(check["id"])
        require(check["role"] in roles, "check: role not required")
        require(type(check["observation"]) is str and check["observation"] in PROBES, "check: observation")
        require(check["source"] == PROBES[check["observation"]][0], "check: source")
        require(type(check["field"]) is str and len(check["field"]) <= 256 and
                (check["field"] == "" or re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*(?:\.[A-Za-z_][A-Za-z_0-9]*)*", check["field"])),
                "check: field")
        require(check["predicate"] in ("equals", "errno", "absent"), "check: predicate")
        require(type(check["baseline_required"]) is bool, "check: baseline_required")
        if check["predicate"] != "equals":
            integer(check["expected"], "check.expected errno", 1, 4095)
            require(check["field"] == "", "check: errno/absent field must be empty")
        if check["predicate"] == "absent":
            require(check["baseline_required"] and check["expected"] == 2,
                    "check: absence requires baseline and ENOENT")
    return expected


def observations(sample):
    return {probe["id"]: probe for probe in sample["observations"]}


def direct_boot(sample):
    probe = observations(sample).get("boot.id")
    if not probe or probe["status"] != "ok":
        return None
    try:
        value = bytes.fromhex(probe["value"]["hex"]).decode("ascii")
    except UnicodeError:
        return None
    value = value.removesuffix("\n")
    return value if UUID.fullmatch(value) else None


def sample_issues(sample):
    issues = []
    role, identity = sample["requested_role"], sample["identity"]
    if role == "root" and (identity["uid"] != "0" or identity["euid"] != "0"):
        issues.append("root role does not have measured real/effective UID 0")
    if role != "root" and (int(identity["uid"]) < 10000 or int(identity["euid"]) < 10000):
        issues.append("app role does not have measured application UIDs")
    probes = observations(sample)
    marker = probes.get("fixture.marker")
    if marker and marker["status"] == "ok":
        expected_marker = ("sumh-observer-fixture-v1\n" + sample["run_id"] + "\n").encode().hex()
        if marker["value"]["hex"] != expected_marker:
            issues.append("observed fixture marker differs from fixed synthetic manifest")
    needed = set(REQUIRED) | (set(DELEGATED) if role == "isolated" or set(probes) & set(DELEGATED) else set())
    for missing in sorted(needed - set(probes)):
        issues.append("missing required observation " + missing)
    for kind in ("link", "stat", "fd"):
        start, end = (probes.get(f"namespace.{at}.{kind}") for at in ("start", "end"))
        if not start or not end or start["status"] != "ok" or end["status"] != "ok":
            issues.append("namespace " + kind + " stability unobserved")
        elif start["value"] != end["value"]:
            issues.append("namespace " + kind + " changed during sample")
    return issues


def index_samples(samples, label):
    roles, instances = {}, set()
    for sample in samples:
        role, instance = sample["requested_role"], sample["observer_instance_id"]
        require(role not in roles, label + ": duplicate role")
        require(instance not in instances, label + ": duplicate observer instance")
        roles[role] = sample
        instances.add(instance)
    require(len({sample["collector_build_id"] for sample in samples}) <= 1, label + ": mixed collector builds")
    return roles


def group_issues(roles, required, binding):
    issues = []
    for role in sorted(set(required) - set(roles)):
        issues.append(role + ": required sample missing")
    for role, sample in roles.items():
        issues.extend(role + ": " + issue for issue in sample_issues(sample))
    if "ordinary" in roles and "isolated" in roles:
        ordinary, isolated = (roles[role]["identity"] for role in ("ordinary", "isolated"))
        if ordinary["uid"] == isolated["uid"] or ordinary["pid"] == isolated["pid"]:
            issues.append("ordinary/isolated measured UID and PID must both differ")
    boots = {direct_boot(sample) for sample in roles.values()}
    if len(boots - {None}) > 1:
        issues.append("samples observed different boot IDs")
    if binding == "direct_boot":
        if not roles or None in boots:
            issues.append("direct boot binding unavailable for one or more observers")
        for role, sample in roles.items():
            supplied = sample["context"]["boot_id"]
            if supplied is not None and supplied != direct_boot(sample):
                issues.append(role + ": direct boot ID differs from supplied association")
    return issues


def lookup_field(value, path):
    for part in path.split(".") if path else ():
        if type(value) is not dict or part not in value:
            return False, None
        value = value[part]
    return True, value


def exact_equal(left, right):
    if type(left) is not type(right):
        return False
    if type(left) is dict:
        return left.keys() == right.keys() and all(exact_equal(left[key], right[key]) for key in left)
    if type(left) is list:
        return len(left) == len(right) and all(exact_equal(a, b) for a, b in zip(left, right))
    return left == right


def compare_check(check, current, baseline, issues):
    result = {"id": check["id"], "role": check["role"], "observation": check["observation"],
              "source": check["source"], "result": "inconclusive", "reason": "", "observed": None}

    def finish(status, reason, value=None):
        result.update(result=status, reason=reason, observed=value)
        return result

    sample = current.get(check["role"])
    if not sample:
        return finish("inconclusive", "required observer sample missing")
    if issues:
        return finish("inconclusive", "evidence binding/integrity checks incomplete; see issues")
    probe = observations(sample).get(check["observation"])
    if not probe:
        return finish("inconclusive", "observation missing")
    if check["baseline_required"]:
        old = baseline.get(check["role"])
        if not old:
            return finish("inconclusive", "baseline observer missing")
        if any(sample["identity"][key] != old["identity"][key] for key in ("uid", "euid", "gid", "egid")):
            return finish("inconclusive", "baseline measured actor identity differs")
        if sorted(sample["identity"]["groups"]) != sorted(old["identity"]["groups"]):
            return finish("inconclusive", "baseline supplementary groups differ")
        current_selinux = observations(sample).get("identity.selinux")
        previous_selinux = observations(old).get("identity.selinux")
        if (current_selinux and previous_selinux and current_selinux["status"] == "ok" and
                previous_selinux["status"] == "ok" and current_selinux["value"] != previous_selinux["value"]):
            return finish("inconclusive", "baseline measured SELinux context differs")
        before = observations(old).get(check["observation"])
        if not before or before["status"] != "ok":
            return finish("inconclusive", "baseline probe was not completed successfully")
    if probe["status"] == "unsupported":
        return finish("not_applicable", "observer reports probe unsupported")
    if probe["status"] in ("truncated", "not_run") or not probe["complete"]:
        return finish("inconclusive", "observation incomplete")
    if check["predicate"] in ("errno", "absent"):
        actual = probe["errno"] if probe["status"] == "error" else None
        matched = probe["status"] == "error" and actual == check["expected"]
        return finish("match" if matched else "mismatch", "completed errno comparison", actual)
    if probe["status"] != "ok":
        return finish("inconclusive", "value unavailable due to probe error", probe["errno"])
    found, actual = lookup_field(probe["value"], check["field"])
    if not found:
        return finish("inconclusive", "expected value field missing")
    return finish("match" if exact_equal(actual, check["expected"]) else "mismatch",
                  "completed value comparison", actual)


def compare(request_raw, sample_raws, expectation_raw, baseline_request_raw=None, baseline_raws=()):
    require(len(sample_raws) <= 3 and len(baseline_raws) <= 3, "at most one sample per role is accepted")
    request = validate_request(request_raw)
    expected = validate_expectations(expectation_raw, request_raw)
    current = index_samples([validate_sample(raw, request_raw) for raw in sample_raws], "current")
    require(not baseline_raws or baseline_request_raw is not None, "baseline samples require explicit baseline request")
    if baseline_request_raw is not None:
        baseline_request = validate_request(baseline_request_raw)
        require(baseline_request["fixture_root"].rsplit("/", 1)[0] == request["fixture_root"].rsplit("/", 1)[0],
                "baseline/current synthetic fixture anchor mismatch")
    baseline = index_samples([validate_sample(raw, baseline_request_raw) for raw in baseline_raws], "baseline")
    require(not {sample["observer_instance_id"] for sample in current.values()} &
            {sample["observer_instance_id"] for sample in baseline.values()},
            "baseline/current duplicate observer invocation")
    builds = {sample["collector_build_id"] for sample in (*current.values(), *baseline.values())}
    require(len(builds) <= 1, "baseline/current collector build mismatch")
    issues = group_issues(current, expected["required_roles"], expected["required_binding"])
    if baseline:
        needed = {check["role"] for check in expected["checks"] if check["baseline_required"]}
        issues.extend("baseline: " + issue for issue in group_issues(baseline, needed, expected["required_binding"]))
        if expected["required_binding"] == "direct_boot":
            boots = {direct_boot(sample) for sample in (*current.values(), *baseline.values())}
            if len(boots) != 1 or None in boots:
                issues.append("baseline/current direct boot IDs do not match")
            for role in sorted(needed & set(baseline) & set(current)):
                if int(baseline[role]["finished_boottime_ns"]) > int(current[role]["started_boottime_ns"]):
                    issues.append(role + ": baseline interval overlaps or follows current sample")
    checks = [compare_check(check, current, baseline, issues) for check in expected["checks"]]
    states = {check["result"] for check in checks}
    if issues or "inconclusive" in states:
        overall = "inconclusive"
    elif "mismatch" in states:
        overall = "mismatch"
    elif states == {"not_applicable"}:
        overall = "not_applicable"
    elif "not_applicable" in states:
        overall = "inconclusive"
    else:
        overall = "match"
    return {"schema_version": 1, "resource": "sumh_observer_comparison", "run_id": request["run_id"],
            "request_sha256": hashlib.sha256(request_raw).hexdigest(), "result": overall,
            "non_atomic": True, "config_revision_binding": "supplied_unverified",
            "boot_binding": expected["required_binding"], "issues": issues, "checks": checks,
            "limitations": ["Local file digests bind supplied artifacts; they are not device attestation.",
                            "A match covers only declared checks and supplied observer samples.",
                            "Requested role and measured application UID do not prove SUMH authorization."]}


def markdown(report):
    def escape(value):
        return str(value).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace(
            "|", "&#124;").replace("\r", " ").replace("\n", " ").replace(chr(96), "&#96;")

    lines = ["# SUMH observer evidence comparison", "", f"Result: **{report['result']}**", "",
             f"Request SHA-256: {report['request_sha256']}", "",
             "Configuration revision: **supplied_unverified**. Samples are non-atomic.", "",
             "| Check | Observer | Probe/source | Result | Reason |", "| --- | --- | --- | --- | --- |"]
    for check in report["checks"]:
        lines.append("| " + " | ".join(escape(value) for value in (check["id"], check["role"],
            check["observation"] + "/" + check["source"], check["result"], check["reason"])) + " |")
    if report["issues"]:
        lines += ["", "Evidence issues:", ""] + ["- " + escape(issue) for issue in report["issues"]]
    lines += ["", "Limits:", ""] + ["- " + escape(limit) for limit in report["limitations"]]
    if "inputs" in report:
        lines += ["", "Input artifacts:", ""]
        for item in report["inputs"]:
            lines.append(f"- {escape(item['kind'])}: {escape(item['path'])} (SHA-256 {item['sha256']})")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", type=Path, required=True)
    parser.add_argument("--sample", type=Path, action="append", default=[])
    parser.add_argument("--expectations", type=Path, required=True)
    parser.add_argument("--baseline-request", type=Path)
    parser.add_argument("--baseline-sample", type=Path, action="append", default=[])
    parser.add_argument("--json-out", type=Path, required=True)
    parser.add_argument("--markdown-out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        require(len(args.sample) <= 3 and len(args.baseline_sample) <= 3,
                "at most three current and three baseline sample files are accepted")
        all_inputs = [("request", args.request, REQUEST_LIMIT), ("expectations", args.expectations, EXPECTATION_LIMIT)]
        all_inputs += [("sample", path, SAMPLE_LIMIT) for path in args.sample]
        all_inputs += [("baseline_sample", path, SAMPLE_LIMIT) for path in args.baseline_sample]
        if args.baseline_request:
            all_inputs.append(("baseline_request", args.baseline_request, REQUEST_LIMIT))
        outputs = [path.resolve() for path in (args.json_out, args.markdown_out)]
        require(len(set(outputs)) == 2 and not set(outputs) & {path.resolve() for _, path, _ in all_inputs},
                "output paths must be distinct and must not overwrite inputs")
        existing = [path for path in outputs if path.exists()]
        protected = [path for _, path, _ in all_inputs] + outputs
        for output in existing:
            require(not any(other != output and other.exists() and output.samefile(other)
                            for other in protected), "output paths must not alias inputs or one another")
        contents = [(kind, path, read_bounded(path, limit)) for kind, path, limit in all_inputs]

        def raw(kind):
            return [data for item, _, data in contents if item == kind]

        report = compare(raw("request")[0], raw("sample"), raw("expectations")[0],
                         raw("baseline_request")[0] if raw("baseline_request") else None, raw("baseline_sample"))
        report["inputs"] = [{"kind": kind, "path": str(path.resolve()), "sha256": hashlib.sha256(data).hexdigest()}
                            for kind, path, data in contents]
        args.json_out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        args.markdown_out.write_text(markdown(report), encoding="utf-8")
        return 0 if report["result"] == "match" else 1
    except (EvidenceError, OSError) as error:
        print("observer comparison input/output error: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
