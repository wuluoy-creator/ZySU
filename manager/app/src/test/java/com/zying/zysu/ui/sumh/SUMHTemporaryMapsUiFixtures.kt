package com.zying.zysu.ui.sumh

import com.zying.zysu.ui.sumh.util.*
import org.json.JSONObject
import kotlinx.coroutines.flow.MutableStateFlow

internal object SUMHTemporaryMapsUiFixtures {
    fun page(cursor: Int = 0, end: Boolean = true, ino: String = "18446744073709551615",
        path: String = "/example/映射.so ", hex: String? = null, empty: Boolean = false): SUMHTemporaryMapsPageResult {
        val entry = SUMHInventoryFixtures.entry(SUMHInventoryCategory.TemporaryMaps, cursor + 1)
            .put("target_ino", ino).put("target_dev", "0").put("spoofed_ino", "9007199254740993")
            .put("spoofed_dev", "4294967295").put("spoofed_path", hex?.let {
                JSONObject().put("encoding", "hex").put("byte_length", it.length / 2).put("value", it)
            } ?: SUMHInventoryFixtures.bytes(path))
        val root = SUMHInventoryFixtures.page(SUMHInventoryCategory.TemporaryMaps, cursor,
            if (empty) emptyList() else listOf(entry), end = end).put("boot_id", JSONObject.NULL)
        val text = root.toString()
        return SUMHTemporaryMapsPageResult(SUMHInventoryParser.parse(text), text.toByteArray().size)
    }

    fun failedPage(status: SUMHInventoryPageStatus = SUMHInventoryPageStatus.Stale) =
        page().let { it.copy(page = it.page!!.copy(status = status, errno = 116,
            instance = null, generation = null, mask = null, cursor = null, nextCursor = null,
            atEnd = false, entries = emptyList(), reason = "snapshot_changed")) }

    class Service : SUMHTemporaryMapsService {
        val tracking = Tracking(this)
        var legacyRemoves = 0
        var reads = 0
        val pages = mutableListOf<SUMHTemporaryMapsCursor?>()
        val removes = mutableListOf<SUMHTemporaryMapsRemoveRequest>()
        var support = SUMHTemporaryMapsCapabilities(SUMHTemporaryMapsSupport.Supported, SUMHTemporaryMapsSupport.Supported)
        var capabilityReader: suspend () -> SUMHTemporaryMapsCapabilities = { support }
        var pageReader: suspend (SUMHTemporaryMapsCursor?) -> SUMHTemporaryMapsPageResult = { cursor ->
            page(cursor?.cursor?.toInt() ?: 0)
        }
        var remover: suspend (SUMHTemporaryMapsRemoveRequest) -> SUMHTemporaryMapsRemoveResult = {
            SUMHTemporaryMapsRemoveResult(SUMHTemporaryMapsRemoveStatus.Deleted, "deleted", 0, true, true)
        }
        override suspend fun capabilities(): SUMHTemporaryMapsCapabilities { reads++; return capabilityReader() }
        override suspend fun list(cursor: SUMHTemporaryMapsCursor?): SUMHTemporaryMapsPageResult { pages += cursor; return pageReader(cursor) }
        override suspend fun remove(request: SUMHTemporaryMapsRemoveRequest): SUMHTemporaryMapsRemoveResult {
            legacyRemoves++; error("UI must never fall back to legacy deletion")
        }
    }
    class Tracking(private val service: Service) : SUMHTemporaryMapsTrackingService {
        override val snapshot = MutableStateFlow(SUMHTemporaryMapsTrackingSnapshot())
        var support = SUMHTemporaryMapsSupport.Supported
        var capabilityReads = 0
        var loads = 0
        val queries = mutableListOf<Pair<String, Int>>()
        val forgotten = mutableListOf<String>()
        var records = emptyList<SUMHTemporaryMapsTrackedOperation>()
        var queryHandler: suspend (String, Int) -> Boolean = { _, _ -> true }
        override suspend fun capabilities(): SUMHTemporaryMapsSupport { capabilityReads++; return support }
        override suspend fun load(): SUMHTemporaryMapsTrackingSnapshot {
            loads++; return SUMHTemporaryMapsTrackingSnapshot(true, records).also { snapshot.value = it }
        }
        override suspend fun remove(request: SUMHTemporaryMapsRemoveRequest): SUMHTemporaryMapsTrackedRemoval {
            service.removes += request
            return SUMHTemporaryMapsTrackedRemoval(service.remover(request), "01".repeat(16), true)
        }
        override suspend fun recover(id: String, waitMs: Int): Boolean {
            queries += id to waitMs
            val result = queryHandler(id, waitMs); snapshot.value = SUMHTemporaryMapsTrackingSnapshot(true, records); return result
        }
        override suspend fun forget(id: String): Boolean {
            forgotten += id; records = records.filterNot { it.id == id }; snapshot.value = SUMHTemporaryMapsTrackingSnapshot(true, records); return true
        }
    }
}
