# Tracked temporary Maps Manager fixtures

Generated from production C++ operation submission, Journal/capture, typed resource
store/query, embedded Manager callback and transport envelope. The generator is
`userspace/ksud/tests/run_tracked_maps_manager_fixtures.py`; its companion is
`tracked_maps_manager_fixture.cpp`. These fixtures use a modeled filesystem/ioctl
boundary, real mbed TLS SHA-256, and explicitly normalized Android/Linux errno.
They do not establish device execution or filesystem durability.

Batch43 retained generation: `out/batch43-tracked-fixtures-a6/manifest.json`.
That manifest records exact source/output hashes and compile commands. Empty/abc
SHA-256 vectors and every preparation digest were independently checked with
Python hashlib. The a6 manifest additionally binds all transitive source headers;
its outputs match a5. Earlier a1–a4 are retained failed harness build attempts.

Each scenario has prepare/execute/show/wait/result envelopes. Kotlin test
mutations are negative cases derived from these generated fixtures; they are
not claimed to be additional native outputs. Legacy temporary Maps fixtures
remain in their separate unchanged directory.
