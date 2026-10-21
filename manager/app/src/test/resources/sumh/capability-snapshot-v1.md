# Capability snapshot fixture provenance

These fixtures preserve the exact UTF-8 bytes emitted by the host harness that includes production `userspace/ksud/sumhp/src/core/capability_snapshot.cpp`. They were copied directly from the three `out/batch18-capability-fixture.json*` files after the byte-exact export correction. The harness stores the original serializer response, without parsing and reserializing it for export. No Kotlin reconstruction or newline is added.

| Fixture | SHA-256 |
| --- | --- |
| `capability-snapshot-v1.json` | `721b78efadbace53ad327ff8bf7a5b542bf36f10918139f5088eb444260732aa` |
| `capability-snapshot-v1-boot-missing.json` | `9ac1c8543cefc53107a09794a73f563254492d8391924c230315bde970841b55` |
| `capability-snapshot-v1-failure.json` | `2268b16f3db882079d3c7d364ed4199fac31e43183c588ff9db4ea792c168d10` |

Source run: `out/batch18-native-snapshot-byte-exact.log` (44 boundary/serializer cases). Kernel calls and filesystem inputs are synthetic host fixtures. This checks native serializer / Manager parser interoperability, not Android device behavior or observation by applications. The success, unavailable boot and namespace query failure responses retain their original source/timestamps and structured reasons.
