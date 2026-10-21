"""Synthetic host-only data builders. None of these records came from Android."""

import hashlib
import json
from pathlib import Path

import compare

RUN_ID = "0123456789abcdef0123456789abcdef"
BOOT_ID = "01234567-89ab-cdef-0123-456789abcdef"
BUILD_ID = "synthetic-host-fixture-not-a-device-build-v1"


def encode(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def request(run_id=RUN_ID):
    return {"schema_version": 1, "resource": "sumh_observer_request", "run_id": run_id,
            "fixture_root": "/data/user/0/com.zying.zysu.sumhobservertest/files/sumh-observer/" + run_id,
            "context": {"boot_id": BOOT_ID, "config_revision": "sha256-v1:" + "a" * 64}}


def stat(ino="18446744073709551615", size="24"):
    return {"dev": "9007199254740993", "ino": ino, "mode": "33152", "size": size, "mtime_ns": "-1"}


def probe(identifier, value=None, status="ok", error=None):
    return {"id": identifier, "source": compare.PROBES[identifier][0], "status": status,
            "errno": error, "complete": status in ("ok", "error"), "value": value}


def sample(role="ordinary", request_raw=None, instance=None):
    request_raw = request_raw or encode(request())
    req = compare.validate_request(request_raw)
    numeric_role = {"ordinary": 1, "isolated": 2, "root": 3}[role]
    uid = {"ordinary": "10123", "isolated": "99001", "root": "0"}[role]
    pid = str(100 + numeric_role)
    result = {
        "schema_version": 1, "resource": "sumh_observer_sample", "run_id": req["run_id"],
        "request_sha256": hashlib.sha256(request_raw).hexdigest(),
        "fixture_manifest_sha256": compare.manifest_digest(req), "collector_build_id": BUILD_ID,
        "requested_role": role, "observer_instance_id": instance or str(numeric_role) * 32,
        "non_atomic": True, "started_boottime_ns": "9007199254740993",
        "finished_boottime_ns": "9007199254741993",
        "identity": {"uid": uid, "euid": uid, "gid": uid, "egid": uid,
                     "pid": pid, "ppid": "1", "tid": pid, "groups": []},
        "context": req["context"], "observations": [],
    }
    shapes = dict(compare.REQUIRED)
    if role == "isolated":
        shapes.update(compare.DELEGATED)
    for identifier, (_, shape) in shapes.items():
        if shape == "decimal":
            value = "9007199254740993"
        elif shape == "stat":
            value = stat()
        elif shape == "bytes":
            content = {
                "boot.id": (BOOT_ID + "\n").encode(),
                "fixture.marker": ("sumh-observer-fixture-v1\n" + req["run_id"] + "\n").encode(),
                "identity.selinux": b"u:r:synthetic_host_fixture:s0",
                "file.present.read": b"SUMH observer present v1\n",
                "file.unicode.read": b"SUMH observer unicode v1\n",
                "link.target": b"present.txt",
                "map.fd.read": b"0123456789abcdef" * 256,
            }.get(identifier, b"mnt:[4026531840]")
            value = {"hex": content.hex()}
        elif shape == "directory":
            value = {"entries": [{"name_hex": "unicode-\u754c.txt".encode().hex(),
                                  "ino": "9007199254740993", "type": 8},
                                 {"name_hex": "fffe", "ino": "0", "type": 0}], "sorted": False}
        elif shape == "mapping":
            value = {"address_start": "1000", "address_end": "2000", "length": "4096"}
        else:
            line = b"1000-2000 r--p 00000000 00:00 0 /synthetic/map.bin\n" if shape == "maps" else b"synthetic /data ext4 ro 0 0\n"
            value = {"lines_hex": [line.hex()], "scanned_bytes": str(len(line))}
            if shape == "maps":
                value["matched_mapping"] = True
        result["observations"].append(probe(identifier, value))
    replace_probe(result, probe("file.missing.stat", status="error", error=2))
    return result


def replace_probe(result, observation):
    for index, old in enumerate(result["observations"]):
        if old["id"] == observation["id"]:
            result["observations"][index] = observation
            return
    result["observations"].append(observation)


def baseline(role="ordinary", request_raw=None):
    result = sample(role, request_raw, instance={"ordinary": "4", "isolated": "5", "root": "6"}[role] * 32)
    result["started_boottime_ns"] = "9007199254738993"
    result["finished_boottime_ns"] = "9007199254739993"
    return result


def check(identifier="present", role="ordinary", observation="file.present.read",
          field="hex", expected=None, predicate="equals", baseline_required=False):
    return {"id": identifier, "role": role, "observation": observation,
            "source": compare.PROBES[observation][0], "field": field, "predicate": predicate,
            "expected": expected if expected is not None else b"SUMH observer present v1\n".hex(),
            "baseline_required": baseline_required}


def expectations(request_raw=None, roles=None, checks=None, binding="direct_boot"):
    request_raw = request_raw or encode(request())
    req = compare.validate_request(request_raw)
    return {"schema_version": 1, "resource": "sumh_observer_expectations", "run_id": req["run_id"],
            "request_sha256": hashlib.sha256(request_raw).hexdigest(),
            "required_roles": roles or ["ordinary"], "required_binding": binding,
            "checks": checks or [check()]}


def write_examples(destination):
    destination.mkdir(parents=True, exist_ok=True)
    req = encode(request())
    examples = {"synthetic-request.json": req}
    for role in ("ordinary", "isolated", "root"):
        examples[f"synthetic-{role}.json"] = encode(sample(role))
    checks = [check("ordinary-content"), check("isolated-fd-content", "isolated", "map.fd.read",
                                            expected=(b"0123456789abcdef" * 256).hex()),
              check("root-known-missing", "root", "file.missing.stat", field="", expected=2, predicate="errno")]
    examples["synthetic-expectations.json"] = encode(expectations(roles=["ordinary", "isolated", "root"], checks=checks))
    for name, content in examples.items():
        (destination / name).write_bytes(content)


if __name__ == "__main__":
    write_examples(Path(__file__).resolve().parent.parent / "fixtures")
