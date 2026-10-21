# SUMH synthetic observer kit

An independent debug/test-only Android app, a shared C++ collector and a root command-line helper record the actual views of different processes. The Python comparator validates those records against explicitly declared expectations. The app does not require production Manager code.

The first implementation supports **ordinary app, isolated service and root** sampling. It records fixed synthetic files, a directory, link target, stat/fstat, a read-only private mapping, matching own `/proc/self/maps` lines, relevant mount lines, process identity and namespace identity. It is evidence collection, not automatic SUMH configuration or an all-device certification.

Detailed Chinese workflow: [按观察者采样与比较](../../docs/zh/guide/sumh-observer-kit.md). Wire format and status semantics: [protocol v1](protocol/schema-v1.md). Synthetic test inputs: [fixtures](fixtures/README.md).

## Build

Requirements: JDK 21, Android SDK platform 37, build-tools 36.1.0, NDK 30.0.16248370, SDK CMake 3.22.1 and Ninja. The app uses the repository Manager's Gradle wrapper with its own project directory; it builds only `arm64-v8a`, min API 31. AGP/Gradle versions are fixed in the project/wrapper. SHA-256 uses only the required files from pinned mbedTLS 3.6.2; CMake checks the download hash. First builds need dependency downloads.

From the repository root, with `JAVA_HOME` and `ANDROID_HOME` already configured:

```powershell
& .\manager\gradlew.bat -p tests/sumh-observer/android :app:testDebugUnitTest :app:lintDebug :app:assembleDebug --no-daemon
```

APK: `tests/sumh-observer/android/app/build/outputs/apk/debug/app-debug.apk`. This APK is marked `testOnly`; there is no enabled release variant. Test reports are under the app's `build/test-results/` and `build/reports/` directories.

Build the separate helper from the same final sources:

```powershell
$observerSdk = $env:ANDROID_HOME
$observerCmake = Join-Path $observerSdk 'cmake/3.22.1/bin/cmake.exe'
$observerNinja = Join-Path $observerSdk 'cmake/3.22.1/bin/ninja.exe'
$observerToolchain = Join-Path $observerSdk 'ndk/30.0.16248370/build/cmake/android.toolchain.cmake'
& $observerCmake -S tests/sumh-observer/native -B out/sumh-observer-android -G Ninja "-DCMAKE_MAKE_PROGRAM=$observerNinja" "-DCMAKE_TOOLCHAIN_FILE=$observerToolchain" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 -DANDROID_STL=c++_static
& $observerCmake --build out/sumh-observer-android
```

The helper is `out/sumh-observer-android/sumh-observer`. The native build also creates `libsumh_observer_jni.so` and `collector-build-id.txt`. Rebuild **both** app and helper after any collector/JNI source change. Matching `collector_build_id` binds first-party native source and pinned hash configuration, while APK/ELF SHA-256 and toolchain logs separately identify actual artifacts; it is not device attestation or a reproducible-binary claim.

## Device workflow

These steps are for a dedicated, recoverable test device. Build checks do not imply these steps have been executed.

1. Install this test APK explicitly (for example, `adb install -t <apk>`), launch **SUMH Observer Test**, and create a new fixture. The app creates an unpredictable run in its own private files directory and shows its request digest.
2. Export the request through Android's document picker without editing or reformatting it. A request contains only the fixed run path and optional supplied context. The same exact bytes must be used by every actor.
3. Collect the ordinary and isolated samples; export each separately. The isolated service receives a read-only FD for the verified map file. `map.fd.*` measures an already-open delegated handle; fresh path access is recorded separately and may fail with EACCES.
4. Copy the helper and exported request to the test device. In an **already authorized root shell**, run the command below. Acquisition of root authorization is outside this utility. Copy the result back byte-for-byte.
5. Declare checks, run the offline comparison and keep request, samples, expectations, report and build hashes together. Use a clean baseline sample for any claim that a formerly visible path became absent.

```sh
# Run inside an existing root shell; substitute paths to your exact exported request.
/data/local/tmp/sumh-observer --request /data/local/tmp/observer-request.json > /data/local/tmp/root-sample.json
```

The helper requires real and effective UID 0. It emits only bounded sample JSON on stdout, diagnostics on stderr, and no additional newline. It can also read `--request -`; it never interprets input as shell code. A nonzero exit is not a sample. It does not create a fixture, start ksud, install/remove rules, replace Maps sets, apply modules or change authorization.

The app retains evidence and the most recent error per actor. Collection success means only that a sample was collected. Timeout reports an unknown outcome, rejects a late response and does not retry. Activity recreation reattaches to the same operation; process recreation restores prior files and marks interrupted work without replaying it. A sample export uses the exact selected snapshot; after process death, select the export again.

## Offline checks

Run the meaningful host tests from the repository root:

```sh
python scripts/ci/test_sumh_observer_evidence.py
```

The native codec and complete collector syscall-model suites use the actual implementation with host doubles. After configuring the native build above, run with a C/C++17 host compiler:

```sh
python tests/sumh-observer/native/run_native_tests.py --mbedtls-source out/sumh-observer-android/_deps/mbedtls-src
```

Use `--cc`/`--cxx` to select compilers, or `--zig <zig-executable>` on Windows. Optional `--export-model <directory>` writes the actual collector's model samples, explicitly marked as host output, for protocol interoperability checks. The native host runner is separate from the standard CI evidence wrapper. It does not execute Android syscalls or exercise device policy.

Try the comparator against the checked-in **synthetic host fixtures**:

```sh
python tests/sumh-observer/host/compare.py --request tests/sumh-observer/fixtures/synthetic-request.json --sample tests/sumh-observer/fixtures/synthetic-ordinary.json --sample tests/sumh-observer/fixtures/synthetic-isolated.json --sample tests/sumh-observer/fixtures/synthetic-root.json --expectations tests/sumh-observer/fixtures/synthetic-expectations.json --json-out out/observer-synthetic-report.json --markdown-out out/observer-synthetic-report.md
```

Create the output directory first if it does not exist. Exit codes are **0** for a match of all declared checks, **1** for mismatch/inconclusive/not-applicable, and **2** for invalid inputs or I/O failure. Input files and output files must be distinct. Reports identify input paths and hashes.

For actual device evidence, generate editable initial checks with explicit roles, then use your exported samples:

```sh
python tests/sumh-observer/host/make_expectations.py --request observer-request.json --role ordinary --role isolated --role root --binding none --out observer-expectations.json
```

This creates expectations from fixed fixture content, not from observed samples. The generator defaults to `direct_boot`; this example explicitly chooses `none`. It writes the selected output path; use a new filename to retain an earlier expectation. See the [protocol](protocol/schema-v1.md) for custom checks. Do not reuse synthetic samples as a real baseline. Optional `--baseline-request` and repeated `--baseline-sample` supply separately collected baseline evidence. Baseline/current runs may differ, but the fixture anchor, actor identity, collector and probe must be compatible. Invocation IDs cannot be reused; direct-boot baseline checks also require nonoverlapping baseline-before-current intervals, without implying a configuration transaction occurred between them.

`equals` compares a typed value; `errno` compares a completed error. `absent` requires ENOENT plus a successful same-actor/probe baseline. An EACCES, missing field, skipped read, truncated scan or failed binding never becomes evidence of hiding. Namespace instability or unavailable namespace checks make comparison inconclusive. Strict direct-boot binding additionally requires a measured boot UUID from every required actor; root cannot fill in missing app observations. Supplied configuration revision always remains `supplied_unverified`.

## Scope and cleanup

Limits: 32 KiB request, 256 KiB sample, 256 directory entries, 64 KiB per proc scan and 4 KiB per fixture-file read. Five-second native deadlines are cooperative between syscalls. The app's ten-second orchestration timeout cannot cancel a blocked kernel call. An unfinished local native operation keeps its worker occupied; no automatic replay occurs.

The app follows only the documented Android platform alias for private filesDir ancestry. App-owned path tails are pinned and opened without following links; ordinary file opens reject unexpected type, size/content where required, or hard links. This does not turn multiple syscalls into an atomic snapshot. Individual failures remain in each sample.

This version retains private fixtures. After exporting needed evidence, use **Android Settings → Apps → SUMH Observer Test → Storage → Clear storage** for this dedicated test app. It has no arbitrary recursive delete or global SUMH clear. External helper/request/report files can be removed individually after evidence is copied. No test rules are provisioned by this version, so it does not claim to clean up rules configured separately by a tester.

Remaining F13/T03 work includes independent test-rule ownership/provisioning, actual device runs, authorized apps, shared UID, work profiles, reboot/failure recovery, write paths, old-FD effects and the complete supported KMI/SELinux matrix. Root samples never substitute for other observers. See the [54-task ledger](../../docs/plans/SUMH-implementation-progress-2026-10-16.md).
