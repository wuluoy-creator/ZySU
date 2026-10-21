package com.zying.zysu.ui.sumh

import com.zying.zysu.ui.sumh.util.*

internal object SUMHCapabilitiesUiFixtures {
    fun snapshot(boot: String = "boot-A"): SUMHCapabilitiesSnapshot {
        fun <T> section(source: String, value: T) = SUMHCapabilitySection(SUMHCapabilityStatus.Known,
            source, "sampled", 0, "10", "20", boot, value)
        val controls = SUMHControlStateParser.controlNames.associateWith { id ->
            when (id) {
                "stealth" -> SUMHRuntimeFeature(true, true, false, true, 5)
                "statfs_spoof" -> SUMHRuntimeFeature(false, false, false, false, 0)
                else -> SUMHRuntimeFeature(true, false, false, false, 0)
            }
        }
        val runtime = SUMHRuntimeSample(SUMHRuntimeRead.Known, "sampled", 0, boot, "11", "19", "101", "8", true, controls)
        return SUMHCapabilitiesSnapshot(boot, "1", "30",
            section("saved_config", SUMHCapabilitiesSaved("sha256-v1:" + "a".repeat(64),
                (SUMHControlStateParser.controlNames + "overlay_xattr_hide").associateWith { false }, "normal",
                "", "", "post-fs-data", "auto", true, true)),
            section("kernel_ioctl", SUMHCapabilitiesRuntime(runtime, 0, 1, "65535")),
            section("initial_uts_ioctl", SUMHCapabilitiesBuild(true, "6.1.0-displayed", "displayed version",
                "6.12.52-android16", "original version", "control_generation_bracketed", "101", "9")),
            section("capability_probes", SUMHCapabilitiesBackends(listOf(
                SUMHCapabilityBackend("sumh", SUMHCapabilityBackendStatus.Available, "kernel_protocol", "protocol_match"),
                SUMHCapabilityBackend("overlay", SUMHCapabilityBackendStatus.Unknown, "proc_filesystems", "read_failed"),
                SUMHCapabilityBackend("magic", SUMHCapabilityBackendStatus.Available, "controller_strategy", "compiled_strategy_not_mount_test")),
                "runtime_backend_ownership_not_collected")),
            SUMHCapabilitySection(SUMHCapabilityStatus.Unknown, "not_collected", "configured_owner_is_not_runtime_ownership_proof",
                0, "10", "20", boot, null))
    }
    fun result() = SUMHCapabilitiesResult(snapshot())
}
