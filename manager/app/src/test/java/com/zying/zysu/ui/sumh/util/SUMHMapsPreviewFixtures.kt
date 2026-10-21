package com.zying.zysu.ui.sumh.util

import org.json.JSONArray
import org.json.JSONObject

internal object SUMHMapsPreviewFixtures {
    val revision = "sha256-v1:" + "a".repeat(64)
    fun identity(ino: String = "18446744073709551615") = JSONObject().put("status", "regular").put("ino", ino).put("dev", "1").put("errno", 0)
    fun row(index: Int = 0) = JSONObject().put("index", index).put("target_path", "/目标/$index ").put("replacement_path", "/system/替换 ")
        .put("target", identity()).put("replacement", identity("9")).put("same_target_collision", false).put("stable", true).put("registered_match", "matches")
    fun receipt(phase: String = "acknowledged") = JSONObject().put("id", "a".repeat(32)).put("supersedes_pending_id", "").put("boot_id", "boot-a")
        .put("definition_revision", revision).put("namespace_ino", "3").put("instance_id", "1")
        .put("generation_before", "7").put("generation_after", if (phase == "acknowledged") "9" else "")
        .put("phase", phase).put("errno", if (phase == "rejected") 5 else 0)
        .put("rows", JSONArray().put(JSONObject().put("target_path", "/目标/0 ").put("replacement_path", "/system/替换 ")
            .put("target_ino", "4").put("target_dev", "1").put("spoofed_ino", "9").put("spoofed_dev", "1")))
    fun preview(count: Int = 1): JSONObject = JSONObject().put("schema_version", 1).put("kind", "sumh.maps.preview").put("read_only", true)
        .put("status", "complete").put("reason", "maps_preview_complete").put("errno", 0).put("boot_id", "boot-a").put("config_revision", revision)
        .put("saved", JSONObject().put("status", "valid").put("enabled", true).put("count", count).put("maps_revision", revision))
        .put("sample", JSONObject().put("namespace_ino", "3").put("instance_id", "1").put("generation", "9")
            .put("identities_sampled", true).put("definitions_complete", true).put("stable", true).put("comparison", "matches").put("reason", "managed_maps_match").put("errno", 0))
        .put("rows", JSONArray(List(count) { row(it) })).put("registered_count", count)
        .put("last_submission", JSONObject().put("status", "missing").put("reason", "maps_submission_history_missing")
            .put("latest", JSONObject.NULL).put("confirmed", JSONObject.NULL).put("stable", true))
        .put("limits", JSONObject().put("max_rules", 256).put("definition_encoded_bytes", 57344).put("response_bytes", 524288))
    fun withReceipt(raw: JSONObject = preview(), latest: JSONObject = receipt(), confirmed: JSONObject = receipt()): JSONObject = raw.put("last_submission",
        JSONObject().put("status", "valid").put("reason", "maps_submission_history_valid").put("latest", latest).put("confirmed", confirmed).put("stable", true))
}
