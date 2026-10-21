# Observer evidence protocol v1

This package measures processes on a synthetic fixture. It does not install SUMH rules or certify isolation. A sample is a sequence of observations, never an atomic snapshot. The request, native collector and offline checker share this contract. Unknown versions, duplicate JSON keys, unknown required-object keys, invalid UTF-8 and over-budget inputs are rejected.

## Request

UTF-8, at most 32,768 bytes, exactly these keys:

```json
{
  "schema_version": 1,
  "resource": "sumh_observer_request",
  "run_id": "0123456789abcdef0123456789abcdef",
  "fixture_root": "/data/user/0/com.zying.zysu.sumhobservertest/files/sumh-observer/0123456789abcdef0123456789abcdef",
  "context": {"boot_id": null, "config_revision": null}
}
```

The example is not an existing device run. `schema_version` is the integer token `1`, not `1.0` or `1e0`. `run_id` is 32 lowercase hexadecimal characters. `context.boot_id` is null or a lowercase UUID; `config_revision` is null or `sha256-v1:` followed by 64 lowercase hexadecimal characters. Context is supplied association data, not independently observed by an app. Matching it does not prove configuration was applied.

`fixture_root` is at most 1,024 bytes and must be the exact run under one of:

- `/data/user/<decimal-user>/com.zying.zysu.sumhobservertest/files/sumh-observer/`
- `/data/user_de/<decimal-user>/com.zying.zysu.sumhobservertest/files/sumh-observer/`
- `/data/local/tmp/sumh-observer/`

The decimal user component is `0` or a nonzero number with at most 10 digits and no leading zeros. No relative roots, dot components, duplicate separators, trailing slash or embedded NUL are accepted. The app creates only in its actual private files directory. The command-line helper reads an already prepared fixture; it does not create one from a request. No arbitrary probe paths or commands are accepted.

Android's platform-owned `/data/user/0` alias may resolve normally. The private `files` anchor is opened without following a final symlink, and the owned `sumh-observer/<run>` tail uses anchored no-follow opens. A Context path spelled `/data/data/<package>/files` may be represented as the known user-0 request path. This is a specific platform alias boundary, not permission to follow arbitrary request-controlled symlinks. The local-tmp fixture route uses no-follow component traversal.

Every actor receives the same exact request bytes. `request_sha256` hashes those bytes, including whitespace. Export does not append a newline; the CLI consumes file/stdin bytes without stripping whitespace. Actor role is a separate collector argument, so root/ordinary/isolated samples can share one request digest.

## Fixed fixture

| Relative name | Expected content or role |
| --- | --- |
| `fixture.txt` | Exact UTF-8 `sumh-observer-fixture-v1\n<run_id>\n` |
| `present.txt` | `SUMH observer present v1\n` |
| `dir/child.txt` | `SUMH observer child v1\n` |
| `unicode-界.txt` | `SUMH observer unicode v1\n` |
| `map.bin` | Exactly 4,096 bytes, repeating ASCII `0123456789abcdef` |
| `link.txt` | Relative symlink to `present.txt` |
| `missing.txt` | Deliberately absent |

The app creates a new unpredictable run exclusively. Existing paths are not overwritten. `fixture_manifest_sha256` is the digest of the **expected marker bytes**; `fixture.marker` separately records actual bytes or failure. An expected digest alone does not establish fixture access or authenticity. Unexpected marker/topology prevents dependent file-content probes. Directory output is limited to this synthetic scope.

The fixture is retained as evidence. This version has no general delete or rule-cleanup command. Explicitly clearing this dedicated test app's data removes its private fixtures and saved evidence. There is no global clear, Maps set replacement, module hot action or `su` authorization modification.

## Sample

At most 262,144 UTF-8 bytes. Exact top-level keys:

| Key | Type and meaning |
| --- | --- |
| `schema_version`, `resource` | Integer `1`, string `sumh_observer_sample` |
| `run_id`, `request_sha256` | Request ID and 64-character lowercase SHA-256 |
| `fixture_manifest_sha256` | Expected marker digest, 64 lowercase hex |
| `collector_build_id` | Nonempty source/build identity; all compared actors must agree |
| `requested_role` | `ordinary`, `isolated`, or `root`; not authorization-policy evidence |
| `observer_instance_id` | Random 32-character lowercase hex for this invocation |
| `non_atomic` | Always `true` |
| `started_boottime_ns`, `finished_boottime_ns` | Nonnegative decimal strings; end must not precede start |
| `identity` | Exact keys `uid`, `euid`, `gid`, `egid`, `pid`, `ppid`, `tid` as nonnegative decimal strings, and `groups` as decimal-string array |
| `context` | Exact copy of supplied request context, never promoted to measured fields |
| `observations` | Unique fixed-ID envelopes described below |

Integer identities, inodes, sizes, offsets and nanoseconds use strings to avoid JSON double precision loss. Ordinary app and isolated service measure their own IDs. Root collection requires actual real and effective UID 0. A regular app UID cannot establish whether that app has been authorized by SUMH; record that as separate controller evidence when available.

Every observation has exactly:

```json
{"id":"file.missing.stat","source":"path","status":"error","errno":2,"complete":true,"value":null}
```

`source` is `path`, `delegated_fd` or `self`. Status semantics:

| Status | `complete` / errno | Meaning |
| --- | --- | --- |
| `ok` | true / null | Completed operation with a valid value |
| `error` | true / positive integer | Completed syscall error, not a successful visibility test |
| `truncated` | false / null or positive integer | Budget stopped complete observation; partial well-formed value may remain |
| `unsupported` | false / null or positive integer | Operation unavailable, value null |
| `not_run` | false / null | Dependent or timed-out operation did not execute, value null |

Raw bytes use lowercase even-length `hex`; never decode invalid filename/proc bytes with lossy replacement. Stat values have exactly `dev`, `ino`, `mode`, `size`, `mtime_ns` as decimal strings; only `mtime_ns` may be negative. This does not assert that fields are immune to SUMH metadata presentation.

## Observation inventory

All required IDs must occur once, including `not_run` for skipped dependent probes. An absent required ID is malformed/incomplete evidence.

| ID | Source | Successful `value` |
| --- | --- | --- |
| `identity.start_ticks` | self | Nonnegative decimal string from own proc stat |
| `identity.selinux` | self | `{hex}` from own attr/current |
| `boot.id` | self | `{hex}` with actual proc bytes; unreadable remains an error |
| `namespace.start.link`, `namespace.end.link` | self | `{hex}` from own mount namespace readlink |
| `namespace.start.stat`, `namespace.end.stat` | self | Stat value of own namespace path |
| `namespace.start.fd`, `namespace.end.fd` | self | Stat value of an opened own namespace FD |
| `fixture.marker` | path | `{hex}` of observed marker |
| `file.present.stat` | path | lstat value |
| `file.present.open_stat` | path | fstat value of freshly opened file |
| `file.present.read` | path | `{hex}` |
| `file.missing.stat` | path | lstat value if it unexpectedly exists; normally a recorded error |
| `file.unicode.read` | path | `{hex}` |
| `directory.entries` | path | `{entries:[{name_hex,ino,type}],sorted:false}`; no `.`/`..`, `ino` decimal, `type` integer DT_* |
| `link.target` | path | `{hex}` of link target without following it |
| `map.path.stat` | path | fstat value of dedicated map file |
| `map.path.mapping` | path | `{address_start,address_end,length:"4096"}`; addresses lowercase hex without `0x` |
| `map.path.maps` | self | `{lines_hex:[...],scanned_bytes,matched_mapping}` for own matching address range; bytes count decimal, match flag boolean |
| `mount.info`, `mount.mounts` | self | `{lines_hex:[...],scanned_bytes}` retaining fixture-ancestor lines only |

Delegated mode adds exactly four IDs: `map.fd.stat`, `map.fd.read`, `map.fd.mapping` with source `delegated_fd`, and `map.fd.maps` with source `self`. Their value shapes match the path counterparts. The Android isolated workflow requests these four even when the sender cannot open the file, in which case they are `not_run`.

A delegated FD is an already-open handle passed read-only by the app; it is never evidence of the receiver succeeding at a fresh path open. The collector borrows/duplicates it and preserves caller ownership. Only the dedicated regular 4,096-byte map file with expected bytes is eligible. Mapping uses `PROT_READ | MAP_PRIVATE`, stays alive while this process reads its own maps, is not dereferenced, and is always unmapped afterwards. Existing-handle and path effects remain distinct.

## Budgets and stability

Limits are 256 directory entries, 65,536 scanned bytes per proc stream, 4,096 bytes per fixed-file read, and a five-second cooperative sampling deadline. The deadline is checked between operations; it cannot interrupt a blocked kernel call. Android's separate orchestration timeout discards a late response from that attempt and never automatically starts another one.

Namespace link/stat/open-FD start/end observations are independent. Changed or unreadable identity prevents a stable namespace conclusion. Path objects may change between calls even when namespace identity is stable. Shared request/context/build IDs do not make cross-process sampling atomic or remotely attested.

## Expectations and comparison

Expectation JSON is bounded and strict, with exactly `schema_version:1`, `resource:"sumh_observer_expectations"`, `run_id`, `request_sha256`, `required_roles`, `required_binding` (`none` or `direct_boot`) and `checks`. Each check contains exactly `id`, `role`, `observation`, `source`, `field`, `predicate`, `expected`, `baseline_required`.

`field` selects dot-separated object keys within observation `value`; empty selects the complete value. No expressions, code, indexes or shell evaluation. Predicate `equals` compares the selected typed JSON value; `errno` compares a completed explicit error; `absent` is restricted to ENOENT and requires an explicit baseline request/sample whose same actor/probe completed successfully. EACCES/EPERM, truncated scans and unexecuted probes cannot establish hiding. Baseline uses its own request/run digest but must have compatible collector, role, measured actor and probe scope; unrelated runs cannot silently be used as baseline. Reused invocation IDs across baseline/current are rejected. In `direct_boot` mode, each baseline-required role must finish its baseline before or at current start; overlapping/reversed intervals are inconclusive. This orders observation intervals only, not unobserved configuration transactions.

Comparison emits per-check `match`, `mismatch`, `inconclusive` or `not_applicable`, and an overall result for the **declared check scope**. A collection operation succeeding is not a visibility match. Missing required actors, unusable binding, unstable namespace, malformed inputs, missing fields and unavailable observations never silently become a match. Mixed run/request/build/fixture identities and duplicate roles/probe IDs are rejected.

Direct-boot binding requires actual valid boot UUID bytes from every required actor and agreement with supplied expected boot when present. If an isolated process cannot read boot ID, do not fill its observed field from root. Supplied configuration revision is always reported as `supplied_unverified`; this version does not certify saved/applied equality or ingest a controller transaction bracket. Use explicit per-observer expectations rather than assuming ordinary, authorized, root and isolated views are identical.

JSON and Markdown reports reference input artifacts and retain reasons for inconclusive results. Host fixture JSON is synthetic test data, not an Android measurement. Actual device/KMI/authorized-app/shared-UID/work-profile, provisioned rules, write/mmap/old-FD and reboot scenarios remain separate acceptance work.
