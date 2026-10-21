package com.zying.zysu.ui.sumh.util

import org.json.JSONArray
import org.json.JSONObject

internal object SUMHCapabilitiesFixtures {
    fun metadata() = JSONObject().put("capability_snapshot_schema", 1).put("capability_snapshot_transport_schema", 1)
        .put("capability_snapshot_read_only", true).put("capability_snapshot_response_bytes", SUMH_CAPABILITIES_BYTES)

    fun section(source: String, value: Any = JSONObject.NULL, status: String = "known", reason: String = "sample") =
        JSONObject().put("status", status).put("reason", reason).put("errno", 0).put("source", source)
            .put("sample_started_monotonic_ms", "10").put("sample_finished_monotonic_ms", "20").put("value", value)

    fun document(): JSONObject {
        val saved = JSONObject().put("revision", "sha256-v1:" + "a".repeat(64)).put("revision_scope", "saved_config_bytes_only")
            .put("mount_hide_mode", "normal").put("kernel_build_release", "").put("kernel_build_version", "")
            .put("kernel_build_apply_stage", "post-fs-data").put("mount_backend", "auto")
            .put("overlayfs_enabled", true).put("magic_mount_enabled", true)
        (SUMHControlStateParser.controlNames + "overlay_xattr_hide").forEach { saved.put("enable_$it", false) }
        val build = JSONObject().put("enabled", false).put("release", "6.6.30-android15-test").put("version", "original build")
            .put("original_release", "6.6.30-android15-test").put("original_version", "original build")
            .put("consistency", "control_generation_bracketed").put("instance_id", "22").put("generation", "4")
        val backends = JSONObject().put("scope", "capability_only").put("live_backend", "unknown")
            .put("live_backend_reason", "runtime_backend_ownership_not_collected").put("items", JSONArray(listOf(
                JSONObject().put("id", "sumh").put("status", "available").put("source", "kernel_protocol").put("reason", "protocol_compatible_not_mapping_proof"),
                JSONObject().put("id", "overlay").put("status", "unknown").put("source", "proc_filesystems").put("reason", "filesystem_query_failed"),
                JSONObject().put("id", "magic").put("status", "available").put("source", "controller_strategy").put("reason", "compiled_strategy_not_mount_test"),
            )))
        return JSONObject().put("schema_version", 1).put("resource", "capability_snapshot").put("observation", "control_plane_only")
            .put("atomic", false).put("boot_id", "boot-a").put("sample_started_monotonic_ms", "1").put("sample_finished_monotonic_ms", "30")
            .put("sections", JSONObject().put("saved", section("saved_config", saved))
                .put("runtime", section("kernel_ioctl", SUMHControlStateFixtures.runtime(), reason = "kernel_runtime_sample"))
                .put("build", section("initial_uts_ioctl", build)).put("backends", section("capability_probes", backends))
                .put("owner", section("not_collected", status = "unknown", reason = "configured_owner_is_not_runtime_ownership_proof")))
    }

    fun parsed() = SUMHCapabilitiesParser.parse(document().toString())
}
