package com.zying.zysu.ui.sumh.util

import org.json.JSONObject

internal object SUMHTemporaryMapsFixtures {
    val token = SUMHTemporaryMapsToken("23", "9", 127)
    val row = SUMHTemporaryMapsRow("1", "18446744073709551615", "4294967295", "9007199254740993", "0",
        SUMHInventoryBytes("2f61", "/a"))
    val request = SUMHTemporaryMapsRemoveRequest(token, row)

    fun capabilities(inventory: String = "supported", delete: String = "supported", errno: Int = 0) =
        JSONObject().put("schema_version", 1).put("resource", "temporary_maps_capabilities")
            .put("inventory", inventory).put("delete", delete).put("errno", errno)
    fun entry(ordinal: Int = 1, ino: String = ordinal.toString(), dev: String = "0"): JSONObject =
        SUMHInventoryFixtures.entry(SUMHInventoryCategory.TemporaryMaps, ordinal)
            .put("target_ino", ino).put("target_dev", dev).put("spoofed_ino", "18446744073709551615")
            .put("spoofed_dev", "4294967295")
    fun page(cursor: Int = 0, entries: List<JSONObject> = listOf(entry(cursor + 1)), end: Boolean = true): JSONObject =
        SUMHInventoryFixtures.page(SUMHInventoryCategory.TemporaryMaps, cursor, entries, end).put("boot_id", JSONObject.NULL)
    fun failure(status: String = "stale", errno: Int = 116, reason: String = "snapshot_changed"): JSONObject =
        SUMHInventoryFixtures.failure(status, SUMHInventoryCategory.TemporaryMaps).put("boot_id", JSONObject.NULL)
            .put("errno", errno).put("reason", reason)
    fun receipt(
        status: String = "deleted", errno: Int = 0, attempted: Boolean = true, code: String = "deleted",
        acknowledged: Boolean = status == "deleted", request: SUMHTemporaryMapsRemoveRequest = this.request,
    ) = JSONObject().put("schema_version", 1).put("resource", "temporary_maps").put("action", "remove")
        .put("status", status).put("error_code", code).put("errno", errno).put("saved", false)
        .put("ioctl_attempted", attempted).put("acknowledged", acknowledged).put("operation_id", JSONObject.NULL)
        .put("history_supported", false).put("observer_verified", false)
        .put("request", JSONObject().put("target_ino", request.row.targetIno).put("target_dev", request.row.targetDev)
            .put("expected_instance_id", request.token.instance).put("expected_generation", request.token.generation))
    fun dispatch(action: String, result: JSONObject, exit: Int = 0) = JSONObject().put("schema_version", 1)
        .put("resource", "temporary_maps_dispatch").put("action", action).put("exit_code", exit).put("result", result)
    fun envelope(dispatch: JSONObject) = JSONObject().put("ok", true).put("exit_code", 0).put("errno", 0)
        .put("stdout", dispatch.toString()).put("stderr", "")
    fun wire(action: String, result: JSONObject, exit: Int = 0): ByteArray = envelope(dispatch(action, result, exit)).toString().toByteArray()
    fun pageResult(cursor: Int = 0, entries: List<JSONObject> = listOf(entry(cursor + 1)), end: Boolean = true): SUMHTemporaryMapsPageResult =
        SUMHTemporaryMapsParser.page(wire("list", page(cursor, entries, end)),
            if (cursor == 0) null else SUMHTemporaryMapsCursor(token, cursor.toString()))
}
