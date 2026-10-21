package com.zying.zysu.ui.sumh.util

import org.json.JSONArray
import org.json.JSONObject

internal object SUMHPartitionsFixtures {
    val revision = "sha256-v1:" + "a".repeat(64)
    val nextRevision = "sha256-v1:" + "b".repeat(64)
    fun bytes(text: String) = JSONObject().put("encoding", "utf-8").put("value", text).put("byte_length", text.toByteArray().size)
    fun candidate(name: String = "extra", applicable: Boolean = true) = JSONObject().put("name", name).put("saved", false)
        .put("applicable", applicable).put("status", if (applicable) "applicable" else "unknown").put("reason", "structural_candidate")
        .put("target", JSONObject().put("path", bytes("/$name")).put("status", "directory").put("errno", 0))
        .put("alias", JSONObject().put("path", bytes("/system/$name")).put("status", "missing").put("link_target", JSONObject.NULL).put("errno", 0))
        .put("mount", JSONObject().put("status", "mounted").put("filesystem", "erofs").put("source", bytes("/dev/block/dm-8")))
        .put("sources", JSONArray().put(JSONObject().put("module_id", bytes("module")).put("path", bytes("/data/adb/modules/module/$name"))
            .put("layout", "top").put("disabled", true).put("remove_pending", false).put("skip_mount", false).put("status", "nonempty").put("reason", "observed")))
    fun discovery(saved: List<String> = listOf("old_unknown"), revision: String = this.revision) = JSONObject()
        .put("schema_version", 1).put("kind", "sumh.partitions.discovery").put("read_only", true)
        .put("scope", "installed_module_candidates_not_mount_readiness").put("requires_reboot", true)
        .put("status", "complete").put("complete", true).put("stable", true).put("selection_valid", true)
        .put("config_status", "valid").put("config_revision", revision).put("boot_id", "test-boot").put("namespace_ino", "55")
        .put("saved_partitions", JSONArray(saved)).put("candidates", JSONArray().put(candidate())).put("issues", JSONArray())
    fun sample() = SUMHPartitionsParser.discovery(discovery().toString())
    fun save(status: String = "saved", names: List<String> = listOf("old_unknown", "extra")) = JSONObject()
        .put("schema_version", 1).put("kind", "sumh.partitions.save").put("read_only", false).put("applied", false).put("requires_reboot", true)
        .put("status", status).put("saved", status in setOf("saved", "unchanged")).put("readback_confirmed", true)
        .put("durability_confirmed", status == "saved").put("config_revision", nextRevision).put("previous_revision", revision)
        .put("saved_partitions", JSONArray(names)).put("reason", "fixture").put("detail", bytes("")).put("errno", if (status == "partial") 5 else 0)
}
