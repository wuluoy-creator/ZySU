package com.zying.zysu.ui.sumh.util

import org.json.JSONArray
import org.json.JSONObject

internal object SUMHInventoryFixtures {
    fun bytes(text: String) = JSONObject().put("encoding", "utf-8").put("value", text)
        .put("byte_length", text.toByteArray(Charsets.UTF_8).size)
    fun entry(category: SUMHInventoryCategory, id: Int = 1): JSONObject {
        val owner = when (category) { SUMHInventoryCategory.UserHide -> "user_hide"; SUMHInventoryCategory.ManagedMaps -> "managed_maps"; else -> "unattributed" }
        val result = JSONObject().put("category", category.wire).put("owner", owner)
            .put("id_kind", if (category == SUMHInventoryCategory.UserHide) "managed_definition" else "generation_ordinal")
            .put("id", id.toString()).put("definition_generation", if (category == SUMHInventoryCategory.UserHide) "9" else "0")
            .put("mutation_handle", false)
        when (category) {
            SUMHInventoryCategory.Mapping, SUMHInventoryCategory.Merge -> {
                result.put("visible_path", bytes("/visible $id\n名字 ")).put("backing_path", bytes("/source/$id"))
                if (category == SUMHInventoryCategory.Mapping) result.put("type", 8)
            }
            SUMHInventoryCategory.ManagedMaps, SUMHInventoryCategory.TemporaryMaps -> {
                result.put("spoofed_path", bytes("/maps/$id"))
                for (key in listOf("target_ino", "target_dev", "spoofed_ino", "spoofed_dev")) result.put(key, "18446744073709551615")
            }
            else -> {
                result.put("path", bytes("/path/$id"))
                if (category == SUMHInventoryCategory.LegacyHide) result.put("storage_managed", true)
            }
        }
        return result
    }
    fun page(category: SUMHInventoryCategory = SUMHInventoryCategory.Metadata, cursor: Int = 0,
        entries: List<JSONObject> = emptyList(), end: Boolean = true, mask: Int = 127): JSONObject =
        JSONObject().put("schema_version", 1).put("status", "known").put("reason", "registered_definitions").put("errno", 0)
            .put("source", "kernel_ioctl").put("observation", "registered_definitions_only").put("boot_id", "boot-a")
            .put("category", category.wire).put("page_capacity", 1)
            .put("complete", category != SUMHInventoryCategory.Metadata && cursor == 0 && end)
            .put("complete_scope", "selected_category_from_cursor_zero").put("legacy", JSONObject.NULL)
            .put("page", JSONObject().put("instance_id", "23").put("generation", "9")
                .put("cursor", cursor.toString()).put("next_cursor", (cursor + entries.size).toString()).put("at_end", end)
                .put("consistency", "definitions_coherent").put("supported_categories", mask).put("entries", JSONArray(entries)))
    fun failure(status: String = "stale", category: SUMHInventoryCategory = SUMHInventoryCategory.Mapping): JSONObject =
        page(category).put("status", status).put("reason", if (status == "stale") "snapshot_changed" else "inventory_query_failed")
            .put("errno", 116).put("complete", false).put("page", JSONObject.NULL)
    fun unsupported(raw: JSONObject? = bytes("")): JSONObject = failure("unsupported", SUMHInventoryCategory.Metadata)
        .put("reason", "rule_inventory_not_advertised").put("errno", 0)
        .put("legacy", JSONObject().put("complete", false).put("scope", "unstructured_legacy_text").put("errno", 0)
            .put("raw", raw ?: JSONObject.NULL))
    suspend fun respond(args: Array<String>, perCategory: Int = 1, mask: Int = 127): SUMHQueryReply {
        if (args[1] == "meta") return SUMHQueryReply("{\"rule_inventory_schema\":1}")
        val category = SUMHInventoryCategory.entries.single { it.wire == args.getOrElse(2) { "metadata" } }
        if (category == SUMHInventoryCategory.Metadata) return SUMHQueryReply(page(mask = mask).toString())
        val cursor = args[5].toInt()
        return SUMHQueryReply(page(category, cursor,
            if (cursor < perCategory) listOf(entry(category, cursor + 1)) else emptyList(), cursor + 1 >= perCategory, mask).toString())
    }
}
