"""Exercise actual offline validation/comparison and CLI with synthetic evidence."""

import copy
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import compare
import make_expectations
import synthetic as syn


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.request = syn.encode(syn.request())
        self.sample = syn.sample()
        self.expected = syn.expectations()

    def evaluate(self, sample=None, expected=None, baseline=None, baseline_request=None):
        return compare.compare(self.request, [syn.encode(self.sample if sample is None else sample)],
                               syn.encode(self.expected if expected is None else expected),
                               baseline_request, [syn.encode(baseline)] if baseline else [])

    def test_three_roles_use_independent_typed_checks(self):
        checks = [syn.check(), syn.check("isolated-fd", "isolated", "map.fd.read",
                                        expected=(b"0123456789abcdef" * 256).hex()),
                  syn.check("root-absent-known", "root", "file.missing.stat", "",
                            expected=2, predicate="errno")]
        report = compare.compare(self.request, [syn.encode(syn.sample(role)) for role in compare.ROLES],
                                 syn.encode(syn.expectations(roles=sorted(compare.ROLES), checks=checks)))
        self.assertEqual(report["result"], "match")
        self.assertEqual(report["config_revision_binding"], "supplied_unverified")
        self.assertTrue(report["non_atomic"])
        self.assertEqual(len(report["checks"]), 3)

    def test_u64_and_negative_time_preserved_without_double_rounding(self):
        for field, value in (("ino", "18446744073709551615"), ("dev", "9007199254740993"), ("mtime_ns", "-1")):
            with self.subTest(field=field):
                expected = syn.expectations(checks=[syn.check(observation="file.present.stat", field=field, expected=value)])
                self.assertEqual(self.evaluate(expected=expected)["result"], "match")
                expected["checks"][0]["expected"] = int(value)
                self.assertEqual(self.evaluate(expected=expected)["result"], "mismatch")
        for time in ("9223372036854775807999999999", "-9223372036854775808000000000"):
            sample = copy.deepcopy(self.sample)
            compare.observations(sample)["file.present.stat"]["value"]["mtime_ns"] = time
            self.assertEqual(compare.validate_sample(syn.encode(sample), self.request), sample)

    def test_value_mismatch_retains_actual_value(self):
        self.expected["checks"][0]["expected"] = "00"
        report = self.evaluate()
        self.assertEqual(report["result"], "mismatch")
        self.assertEqual(report["checks"][0]["observed"], b"SUMH observer present v1\n".hex())

    def test_missing_or_denied_value_does_not_match_null(self):
        self.expected["checks"][0]["expected"] = None
        for state, code in (("error", 13), ("not_run", None), ("truncated", None)):
            with self.subTest(state=state):
                syn.replace_probe(self.sample, syn.probe("file.present.read", status=state, error=code))
                self.assertEqual(self.evaluate()["result"], "inconclusive")

    def test_missing_field_is_inconclusive(self):
        self.expected["checks"][0]["field"] = "missing.value"
        self.expected["checks"][0]["expected"] = None
        self.assertEqual(self.evaluate()["result"], "inconclusive")

    def test_absence_requires_accessible_baseline_and_only_enoent(self):
        self.expected["checks"] = [syn.check(observation="file.present.stat", field="",
                                           expected=2, predicate="absent", baseline_required=True)]
        baseline = syn.baseline()
        syn.replace_probe(self.sample, syn.probe("file.present.stat", status="error", error=2))
        self.assertEqual(self.evaluate()["result"], "inconclusive")
        self.assertEqual(self.evaluate(baseline=baseline, baseline_request=self.request)["result"], "match")
        syn.replace_probe(self.sample, syn.probe("file.present.stat", status="error", error=13))
        self.assertEqual(self.evaluate(baseline=baseline, baseline_request=self.request)["result"], "mismatch")
        syn.replace_probe(baseline, syn.probe("file.present.stat", status="error", error=13))
        self.assertEqual(self.evaluate(baseline=baseline, baseline_request=self.request)["result"], "inconclusive")
        for field, value in (("expected", 13), ("baseline_required", False)):
            expected = copy.deepcopy(self.expected)
            expected["checks"][0][field] = value
            with self.subTest(field=field), self.assertRaises(compare.EvidenceError):
                self.evaluate(expected=expected)

    def test_baseline_requires_own_request_and_compatible_anchor(self):
        old_request = syn.request("b" * 32)
        old_raw = syn.encode(old_request)
        baseline = syn.baseline(request_raw=old_raw)
        self.expected["checks"][0]["baseline_required"] = True
        self.assertEqual(self.evaluate(baseline=baseline, baseline_request=old_raw)["result"], "match")
        with self.assertRaises(compare.EvidenceError):
            self.evaluate(baseline=baseline)
        old_request["fixture_root"] = "/data/local/tmp/sumh-observer/" + old_request["run_id"]
        old_raw = syn.encode(old_request)
        with self.assertRaises(compare.EvidenceError):
            self.evaluate(baseline=syn.baseline(request_raw=old_raw), baseline_request=old_raw)

    def test_baseline_identity_group_and_selinux_changes_are_inconclusive(self):
        self.expected["checks"][0]["baseline_required"] = True
        for kind in ("uid", "euid", "gid", "egid", "groups", "selinux"):
            old = syn.baseline()
            if kind == "groups":
                old["identity"]["groups"] = ["3003"]
            elif kind == "selinux":
                syn.replace_probe(old, syn.probe("identity.selinux", {"hex": b"different".hex()}))
            else:
                old["identity"][kind] = "10124"
            with self.subTest(kind=kind):
                self.assertEqual(self.evaluate(baseline=old, baseline_request=self.request)["result"], "inconclusive")

    def test_baseline_process_restart_with_same_security_identity_is_allowed(self):
        old = syn.baseline()
        old["identity"]["pid"] = "500"
        old["observer_instance_id"] = "f" * 32
        self.expected["checks"][0]["baseline_required"] = True
        self.assertEqual(self.evaluate(baseline=old, baseline_request=self.request)["result"], "match")

    def test_denied_invalid_changed_or_supplied_mismatching_boot_is_inconclusive(self):
        mutations = [syn.probe("boot.id", status="error", error=13),
                     syn.probe("boot.id", {"hex": b"not-a-uuid".hex()}),
                     syn.probe("boot.id", {"hex": b"ffffffff-ffff-ffff-ffff-ffffffffffff\n".hex()})]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                syn.replace_probe(self.sample, mutation)
                self.assertEqual(self.evaluate()["result"], "inconclusive")

    def test_no_boot_binding_does_not_fabricate_direct_measurement(self):
        self.expected["required_binding"] = "none"
        syn.replace_probe(self.sample, syn.probe("boot.id", status="error", error=13))
        self.assertEqual(self.evaluate()["result"], "match")
        self.assertEqual(self.evaluate()["boot_binding"], "none")

    def test_baseline_replay_rejected_and_direct_boot_chronology_checked(self):
        self.expected["checks"][0]["baseline_required"] = True
        with self.assertRaisesRegex(compare.EvidenceError, "duplicate observer invocation"):
            self.evaluate(baseline=self.sample, baseline_request=self.request)
        for end in (self.sample["finished_boottime_ns"], str(int(self.sample["finished_boottime_ns"]) + 1)):
            old = syn.baseline()
            old["finished_boottime_ns"] = end
            with self.subTest(end=end):
                report = self.evaluate(baseline=old, baseline_request=self.request)
                self.assertEqual(report["result"], "inconclusive")
                self.assertIn("baseline interval", " ".join(report["issues"]))
        old = syn.baseline()
        old["finished_boottime_ns"] = self.sample["started_boottime_ns"]
        self.assertEqual(self.evaluate(baseline=old, baseline_request=self.request)["result"], "match")
        self.expected["required_binding"] = "none"
        old["finished_boottime_ns"] = self.sample["finished_boottime_ns"]
        self.assertEqual(self.evaluate(baseline=old, baseline_request=self.request)["result"], "match")

    def test_mixed_direct_boot_ids_never_match_even_without_binding_requirement(self):
        second = syn.sample("root")
        syn.replace_probe(second, syn.probe("boot.id", {"hex": b"ffffffff-ffff-ffff-ffff-ffffffffffff".hex()}))
        report = compare.compare(self.request, [syn.encode(self.sample), syn.encode(second)],
                                 syn.encode(syn.expectations(binding="none")))
        self.assertEqual(report["result"], "inconclusive")

    def test_namespace_changed_or_unreadable_prevents_stability_claim(self):
        for kind in ("link", "stat", "fd"):
            for failed in (True, False):
                sample = copy.deepcopy(self.sample)
                value = {"hex": b"mnt:[new]".hex()} if kind == "link" else syn.stat(ino="2")
                syn.replace_probe(sample, syn.probe("namespace.end." + kind, None if failed else value,
                                                   "error" if failed else "ok", 13 if failed else None))
                with self.subTest(kind=kind, failed=failed):
                    self.assertEqual(self.evaluate(sample=sample)["result"], "inconclusive")

    def test_missing_roles_probes_and_delegated_group_do_not_silently_pass(self):
        self.expected["required_roles"] = ["ordinary", "isolated"]
        self.assertEqual(self.evaluate()["result"], "inconclusive")
        self.expected["required_roles"] = ["ordinary"]
        self.sample["observations"].pop()
        self.assertEqual(self.evaluate()["result"], "inconclusive")
        isolated = syn.sample("isolated")
        isolated["observations"] = [p for p in isolated["observations"] if p["id"] != "map.fd.maps"]
        expected = syn.expectations(roles=["isolated"], checks=[syn.check(role="isolated")])
        self.assertEqual(self.evaluate(sample=isolated, expected=expected)["result"], "inconclusive")

    def test_duplicate_roles_instances_probes_and_builds_rejected(self):
        for kind in ("role", "instance", "probe", "build"):
            first = copy.deepcopy(self.sample)
            second = syn.sample("root")
            if kind == "role":
                second = copy.deepcopy(first)
            elif kind == "instance":
                second["observer_instance_id"] = first["observer_instance_id"]
            elif kind == "probe":
                first["observations"].append(copy.deepcopy(first["observations"][0]))
            else:
                second["collector_build_id"] = "different-build"
            with self.subTest(kind=kind), self.assertRaises(compare.EvidenceError):
                compare.compare(self.request, [syn.encode(first), syn.encode(second)], syn.encode(self.expected))

    def test_mixed_digest_run_manifest_context_and_baseline_build_rejected(self):
        for field, value in (("request_sha256", "0" * 64), ("run_id", "0" * 32),
                             ("fixture_manifest_sha256", "0" * 64),
                             ("context", {"boot_id": None, "config_revision": None})):
            sample = copy.deepcopy(self.sample)
            sample[field] = value
            with self.subTest(field=field), self.assertRaises(compare.EvidenceError):
                self.evaluate(sample=sample)
        old = syn.baseline()
        old["collector_build_id"] = "different"
        with self.assertRaises(compare.EvidenceError):
            self.evaluate(baseline=old, baseline_request=self.request)

    def test_exact_request_bytes_include_whitespace(self):
        with self.assertRaises(compare.EvidenceError):
            compare.compare(self.request + b"\n", [syn.encode(self.sample)], syn.encode(self.expected))
        changed = self.request + b"\n"
        report = compare.compare(changed, [syn.encode(syn.sample(request_raw=changed))],
                                 syn.encode(syn.expectations(request_raw=changed)))
        self.assertEqual(report["result"], "match")

    def test_role_identity_mismatch_is_inconclusive(self):
        root = syn.sample("root")
        root["identity"]["euid"] = "2000"
        expected = syn.expectations(roles=["root"], checks=[syn.check(role="root")])
        self.assertEqual(self.evaluate(sample=root, expected=expected)["result"], "inconclusive")
        isolated = syn.sample("isolated")
        for key in ("uid", "pid"):
            other = copy.deepcopy(isolated)
            other["identity"][key] = self.sample["identity"][key]
            with self.subTest(key=key):
                report = compare.compare(self.request, [syn.encode(self.sample), syn.encode(other)], syn.encode(self.expected))
                self.assertEqual(report["result"], "inconclusive")

    def test_unsupported_is_not_applicable_not_successful_absence(self):
        syn.replace_probe(self.sample, syn.probe("file.present.read", status="unsupported", error=95))
        report = self.evaluate()
        self.assertEqual(report["result"], "not_applicable")
        self.assertEqual(report["checks"][0]["result"], "not_applicable")

    def test_truncated_proc_scan_cannot_match_boolean_absence(self):
        syn.replace_probe(self.sample, syn.probe("map.path.maps",
            {"lines_hex": [], "scanned_bytes": "65536", "matched_mapping": False}, "truncated"))
        self.expected["checks"] = [syn.check(observation="map.path.maps", field="matched_mapping", expected=False)]
        self.assertEqual(self.evaluate()["result"], "inconclusive")

    def test_opaque_bytes_and_unicode_survive_comparison(self):
        values = compare.observations(self.sample)["directory.entries"]["value"]
        self.expected["checks"] = [syn.check(observation="directory.entries", field="", expected=values)]
        self.assertEqual(self.evaluate()["result"], "match")
        self.assertEqual(values["entries"][1]["name_hex"], "fffe")

    def test_changed_marker_cannot_validate_content(self):
        syn.replace_probe(self.sample, syn.probe("fixture.marker", {"hex": b"wrong marker".hex()}))
        self.assertEqual(self.evaluate()["result"], "inconclusive")

    def test_request_canonical_scope_and_context(self):
        valid = syn.request()
        for root in ("/data/local/tmp/sumh-observer/" + syn.RUN_ID,
                     "/data/user_de/10/com.zying.zysu.sumhobservertest/files/sumh-observer/" + syn.RUN_ID):
            valid["fixture_root"] = root
            compare.validate_request(syn.encode(valid))
        for root in ("/etc/passwd", valid["fixture_root"] + "/", valid["fixture_root"].replace("/10/", "/010/"),
                     valid["fixture_root"].replace("/files/", "/files/../files/"),
                     valid["fixture_root"].replace("/10/", "/12345678901/"),
                     valid["fixture_root"].replace("sumhobservertest", "anotherapp")):
            invalid = syn.request()
            invalid["fixture_root"] = root
            with self.subTest(root=root), self.assertRaises(compare.EvidenceError):
                compare.validate_request(syn.encode(invalid))
        for field, value in (("boot_id", "BOOT"), ("config_revision", "unscoped-revision")):
            invalid = syn.request()
            invalid["context"][field] = value
            with self.subTest(field=field), self.assertRaises(compare.EvidenceError):
                compare.validate_request(syn.encode(invalid))

    def test_strict_json_duplicate_unknown_keys_surrogates_nonfinite_and_types(self):
        for raw in (self.request.replace(b'"schema_version":1', b'"schema_version":1,"schema_version":1'),
                    self.request.replace(b'"schema_version":1', b'"schema_version":true'),
                    self.request.replace(b'"schema_version":1', b'"schema_version":1.0'),
                    self.request.replace(b'"schema_version":1', b'"schema_version":NaN'),
                    self.request.replace(b'"schema_version":1', b'"schema_version":1e999'),
                    self.request.replace(b'"schema_version":1', b'"schema_version":1,"unknown":0'),
                    self.request.replace(b'"boot_id":', br'"x":"\ud800","boot_id":'),
                    self.request + b"\xff", b"[" * 2000 + b"]" * 2000):
            with self.subTest(raw=raw[:70]), self.assertRaises(compare.EvidenceError):
                compare.validate_request(raw)

    def test_sample_strict_nested_keys_types_precision_and_status(self):
        bad_probes = [
            syn.probe("file.present.stat", dict(syn.stat(), ino=9007199254740993)),
            syn.probe("file.present.stat", dict(syn.stat(), ino="18446744073709551616")),
            syn.probe("file.present.stat", dict(syn.stat(), size="-1")),
            syn.probe("file.present.read", {"hex": "FF"}),
            syn.probe("file.present.read", {"hex": "f"}),
            syn.probe("file.present.read", {"hex": "", "unknown": True}),
            syn.probe("file.present.read", {"hex": "00"}, error=2),
            syn.probe("file.present.read", status="error"),
            syn.probe("file.present.read", status="not_run", error=110),
            syn.probe("map.path.mapping", {"address_start": "0x1000", "address_end": "2000", "length": "4096"}),
            syn.probe("map.path.maps", {"lines_hex": [], "scanned_bytes": "65537", "matched_mapping": False}),
        ]
        for probe in bad_probes:
            sample = copy.deepcopy(self.sample)
            syn.replace_probe(sample, probe)
            with self.subTest(probe=probe), self.assertRaises(compare.EvidenceError):
                self.evaluate(sample=sample)

    def test_capacity_boundaries_and_bounded_file_read(self):
        padded = self.request + b" " * (compare.REQUEST_LIMIT - len(self.request))
        compare.validate_request(padded)
        for value in (padded + b" ", b"{}" + b" " * compare.SAMPLE_LIMIT):
            with self.assertRaises(compare.EvidenceError):
                if len(value) == compare.REQUEST_LIMIT + 1:
                    compare.validate_request(value)
                else:
                    compare.validate_sample(value, self.request)
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "oversize"
            path.write_bytes(b"x" * 100)
            with self.assertRaises(compare.EvidenceError):
                compare.read_bounded(path, 99)
            self.assertEqual(compare.read_bounded(path, 100), b"x" * 100)

    def test_expression_source_duplicate_id_and_absence_contract_rejected(self):
        for field, value in (("field", "__import__('os')"), ("field", "entries[0]"),
                             ("source", "delegated_fd"), ("predicate", "eval"),
                             ("baseline_required", 1), ("role", ["ordinary"])):
            expected = copy.deepcopy(self.expected)
            expected["checks"][0][field] = value
            with self.subTest(field=field), self.assertRaises(compare.EvidenceError):
                self.evaluate(expected=expected)
        self.expected["checks"].append(copy.deepcopy(self.expected["checks"][0]))
        with self.assertRaises(compare.EvidenceError):
            self.evaluate()

    def test_boolean_is_not_integer_equality(self):
        self.expected["checks"] = [syn.check(observation="map.path.maps", field="matched_mapping", expected=1)]
        self.assertEqual(self.evaluate()["result"], "mismatch")

    def test_expectation_generator_binds_actual_request_and_selects_explicit_sources(self):
        raw = self.request + b"\n"
        expected = make_expectations.create(raw, ["ordinary", "isolated", "root"], "none")
        self.assertEqual(expected["request_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(expected["required_binding"], "none")
        isolated = [check for check in expected["checks"] if check["role"] == "isolated"]
        self.assertEqual([check["source"] for check in isolated], ["delegated_fd"])
        report = compare.compare(raw, [syn.encode(syn.sample(role, raw)) for role in compare.ROLES], syn.encode(expected))
        self.assertEqual(report["result"], "match")
        with tempfile.TemporaryDirectory() as name:
            request, output = Path(name) / "request.json", Path(name) / "expected.json"
            request.write_bytes(raw)
            args = [sys.executable, str(Path(make_expectations.__file__)), "--request", str(request),
                    "--role", "ordinary", "--role", "isolated", "--out", str(output)]
            process = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            compare.validate_expectations(output.read_bytes(), raw)
            args[-1] = str(request)
            self.assertEqual(subprocess.run(args, capture_output=True).returncode, 2)
            self.assertEqual(request.read_bytes(), raw)

    def test_bounded_recursive_schema_type_substitution_never_leaks_exception(self):
        substitutes = [None, True, False, 0, 1, -1, 1.5, "", "x", [], {}]

        def paths(value, prefix=()):
            yield prefix
            if isinstance(value, dict):
                for key, child in value.items():
                    yield from paths(child, prefix + (key,))
            elif isinstance(value, list):
                for index, child in enumerate(value):
                    yield from paths(child, prefix + (index,))

        count = 0
        for kind, original in (("request", syn.request()), ("sample", self.sample), ("expectations", self.expected)):
            for path in paths(original):
                for replacement in substitutes:
                    mutated = copy.deepcopy(original)
                    if not path:
                        mutated = replacement
                    else:
                        parent = mutated
                        for key in path[:-1]:
                            parent = parent[key]
                        parent[path[-1]] = replacement
                    with self.subTest(kind=kind, path=path, replacement=replacement):
                        try:
                            if kind == "request":
                                compare.validate_request(syn.encode(mutated))
                            elif kind == "sample":
                                compare.compare(self.request, [syn.encode(mutated)], syn.encode(self.expected))
                            else:
                                compare.validate_expectations(syn.encode(mutated), self.request)
                        except compare.EvidenceError:
                            pass
                    count += 1
        self.assertGreater(count, 2500)

    def test_markdown_escapes_untrusted_text(self):
        self.expected["checks"][0]["id"] = "<script>|x\nnext"
        rendered = compare.markdown(self.evaluate())
        self.assertNotIn("<script>", rendered)
        self.assertIn("&lt;script&gt;&#124;x next", rendered)

    def test_checked_in_examples_are_synthetic_and_current(self):
        fixture_dir = Path(__file__).resolve().parent.parent / "fixtures"
        with tempfile.TemporaryDirectory() as name:
            generated = Path(name)
            syn.write_examples(generated)
            for path in generated.iterdir():
                self.assertEqual(path.read_bytes(), (fixture_dir / path.name).read_bytes(), path.name)
        raw = (fixture_dir / "synthetic-request.json").read_bytes()
        report = compare.compare(raw,
            [(fixture_dir / f"synthetic-{role}.json").read_bytes() for role in sorted(compare.ROLES)],
            (fixture_dir / "synthetic-expectations.json").read_bytes())
        self.assertEqual(report["result"], "match")
        for role in compare.ROLES:
            self.assertIn("not-a-device-build", json.loads((fixture_dir / f"synthetic-{role}.json").read_bytes())["collector_build_id"])

    def test_real_cli_reports_inputs_hashes_exit_codes_and_preserves_inputs(self):
        script = Path(compare.__file__)
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            request_path, sample_path, expectations_path = [root / filename for filename in ("request.json", "sample.json", "expected.json")]
            request_path.write_bytes(self.request)
            sample_path.write_bytes(syn.encode(self.sample))
            expectations_path.write_bytes(syn.encode(self.expected))
            json_out, md_out = root / "result.json", root / "result.md"
            args = [sys.executable, str(script), "--request", str(request_path), "--sample", str(sample_path),
                    "--expectations", str(expectations_path), "--json-out", str(json_out), "--markdown-out", str(md_out)]
            result = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(json_out.read_bytes())
            self.assertEqual(report["inputs"][0]["sha256"], hashlib.sha256(self.request).hexdigest())
            self.assertIn("supplied_unverified", md_out.read_text())
            self.assertEqual(result.stdout, "")
            self.expected["checks"][0]["expected"] = "00"
            expectations_path.write_bytes(syn.encode(self.expected))
            self.assertEqual(subprocess.run(args, capture_output=True).returncode, 1)
            request_path.write_bytes(b"{}")
            invalid = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(invalid.returncode, 2)
            self.assertNotIn("Traceback", invalid.stderr)
            request_path.write_bytes(self.request)
            args[args.index("--json-out") + 1] = str(request_path)
            self.assertEqual(subprocess.run(args, capture_output=True).returncode, 2)
            self.assertEqual(request_path.read_bytes(), self.request)


if __name__ == "__main__":
    unittest.main(verbosity=2)
