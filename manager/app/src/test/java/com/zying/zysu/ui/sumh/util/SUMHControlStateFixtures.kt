package com.zying.zysu.ui.sumh.util

import org.json.JSONArray
import org.json.JSONObject

internal object SUMHControlStateFixtures {
    val revision = "sha256-v1:" + "a".repeat(64)
    fun runtime(): JSONObject = JSONObject().put("schema_version", 1).put("status", "known")
        .put("reason", "kernel_runtime_sample").put("errno", 0).put("source", "kernel_ioctl")
        .put("observation", "control_plane_only").put("sample_started_monotonic_ms", "10")
        .put("sample_finished_monotonic_ms", "11").put("boot_id", "boot-a").put("protocol_status", 0)
        .put("kernel_protocol", 1).put("capabilities", 786432).put("conditional_set_supported", true)
        .put("runtime", JSONObject().put("instance_id", "22").put("generation", "3")
            .put("consistency", "control_serialized_readiness_sampled").put("generation_scope", "control_mutations_only")
            .put("mount_hide_mode", 0).put("features", JSONArray(SUMHControlStateParser.controlNames.mapIndexed { index, name ->
                JSONObject().put("id", index + 1).put("name", name).put("supported", true)
                    .put("configured", false).put("active", false).put("degraded", false).put("last_set_errno", 0)
            })))
    fun state(status: String = "matches"): JSONObject {
        val features = JSONObject()
        (SUMHControlStateParser.controlNames + listOf("user_hide", "modules", "maps_rules", "overlay_xattr_hide")).forEach { name ->
            features.put(name, JSONObject().put("desired_revision", revision).put("applied_revision", JSONObject.NULL)
                .put("last_verified", JSONObject.NULL).put("status", if (name in SUMHControlStateParser.controlNames) status else "unknown")
                .put("reason", if (name in SUMHControlStateParser.controlNames) {
                    if (status == "drift") "saved_setting_differs_from_runtime" else "setting_verified"
                } else "runtime_inventory_not_available")
                .put("detail", JSONObject()))
        }
        return JSONObject().put("schema_version", 1).put("consistency", "sampled_definitions_and_kernel_control")
            .put("boot_id", "boot-a").put("runtime", runtime()).put("features", features)
            .put("history", JSONObject().put("status", "missing").put("error", "").put("last_operation", JSONObject.NULL))
    }
    fun operation(started: Boolean = false, exit: Int = 0): JSONObject = JSONObject()
        .put("id", "1".repeat(32)).put("name", "config apply").put("phase", if (started) "started" else "finished")
        .put("result", if (started) "outcome_unknown" else if (exit == 0) "completed" else "failed_or_partial")
        .put("boot_id", "boot-a").put("exit_code", exit).put("errno", 0).put("detail", "command returned")
    fun control(raw: JSONObject = state(), error: String? = null): SUMHControlState {
        val parsed = SUMHControlStateParser.state(SUMHQueryReply(raw.toString(), error))
        check(parsed.value != null) { parsed.error.orEmpty() }
        return SUMHControlState(SUMHStateCapabilities(SUMHSchemaSupport.Supported, SUMHSchemaSupport.Supported),
            parsed, parsed.value.runtime)
    }
}
