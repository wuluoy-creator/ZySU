package com.zying.zysu.ui.sumh.util

import org.json.JSONObject

/** Actual production C++ submissions/queries and envelopes, with mocked FS/ioctl and real SHA-256. */
internal object SUMHTemporaryMapsTrackingFixtures {
    val key = SUMHTemporaryMapsOperationKey("18446744073709551615", "4294967295", "18446744073709551615", "9007199254740993")
    val args = temporaryMapsOperationArguments(key)
    val revision = sumhOperationRevision(args)
    val id = "01".repeat(16)
    const val boot = "boot-a"
    fun bytes(name: String) = requireNotNull(javaClass.getResourceAsStream("/sumh-tracked-temporary-maps-native/$name-envelope.json")).use { it.readBytes() }
    fun reply(name: String) = SUMHOperationsParser.dispatch(bytes(name).toString(Charsets.UTF_8))
    fun value(name: String) = JSONObject(reply(name).stdout)
    fun entry(stage: String = "prepared") = SUMHTemporaryMapsTrackedOperation(id, revision, boot, key, stage)
    fun received(name: String = "deleted") = entry("received").copy(response = reply("$name-execute").stdout)
    fun recovered(name: String = "deleted") = entry("unknown").copy(observation = reply("$name-show").stdout, recovered = reply("$name-result").stdout)
    fun request() = SUMHTemporaryMapsRemoveRequest(SUMHTemporaryMapsToken(key.instance, key.generation, 127),
        SUMHTemporaryMapsRow("1", key.targetIno, key.targetDev, "7", "0", SUMHInventoryBytes("2f61", "/a")))
    fun query(value: JSONObject, error: String? = null) = SUMHQueryReply(value.toString(), error)
    fun capabilities() = JSONObject().apply {
        for (field in listOf("operation_prepare", "operation_execute", "operation_show", "operation_wait", "operation_result",
            "operation_reserved_id", "operation_local_temporary_maps_remove", "maps_temporary_tracked_manager")) put(field, true)
        for ((field, version) in mapOf("operation_manager_transport_schema" to 1, "operation_manager_temporary_maps_schema" to 2,
            "operation_submission_schema" to 1, "operation_query_schema" to 2, "operation_resource_result_schema" to 1)) put(field, version)
        put("operation_resource_result_resources", org.json.JSONArray(listOf("user_hide_mutation", "temporary_maps_mutation")))
    }
    class Memory(initial: List<SUMHTemporaryMapsTrackedOperation> = emptyList()) : SUMHTemporaryMapsOperationStorage {
        var records = initial
        var writes = 0
        var failAt = 0
        var landed = false
        var failRead = false
        var beforeWrite: (Int) -> Unit = {}
        override fun read(): List<SUMHTemporaryMapsTrackedOperation> { check(!failRead) { "future_or_corrupt_storage" }; return records }
        override fun write(records: List<SUMHTemporaryMapsTrackedOperation>) {
            encodeSUMHTemporaryMapsOperations(records)
            writes++; beforeWrite(writes)
            if (writes == failAt) {
                if (landed) this.records = records
                error("write_unconfirmed")
            }
            this.records = records
        }
    }
}
