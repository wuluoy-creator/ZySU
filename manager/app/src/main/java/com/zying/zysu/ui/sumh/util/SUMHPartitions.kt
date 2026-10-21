package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonObject
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.bool
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.integer
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.nullableText
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.obj
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.objectValue
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.required
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.revision
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.text
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.unsignedOrNull
import org.json.JSONArray

internal data class SUMHPartitionSource(val module: SUMHInventoryBytes, val path: SUMHInventoryBytes,
    val layout: String, val disabled: Boolean, val removePending: Boolean, val skipMount: Boolean,
    val status: String, val reason: String)
internal data class SUMHPartitionCandidate(val name: String, val saved: Boolean, val applicable: Boolean,
    val status: String, val reason: String, val target: SUMHInventoryBytes, val targetStatus: String,
    val alias: SUMHInventoryBytes, val aliasStatus: String, val filesystem: String?, val mountStatus: String,
    val sources: List<SUMHPartitionSource>)
internal data class SUMHPartitionDiscovery(val status: String, val complete: Boolean, val stable: Boolean,
    val selectionValid: Boolean, val revision: String?, val configStatus: String, val saved: List<String>,
    val bootId: String?, val namespace: String?, val candidates: List<SUMHPartitionCandidate>, val issues: List<String>)
internal data class SUMHPartitionDiscoveryResult(val value: SUMHPartitionDiscovery? = null, val error: String? = null)
internal data class SUMHPartitionSave(val status: String, val saved: Boolean, val revision: String?,
    val previousRevision: String?, val partitions: List<String>, val reason: String, val detail: SUMHInventoryBytes,
    val errno: Int)
internal data class SUMHPartitionSaveResult(val value: SUMHPartitionSave? = null, val error: String? = null)

internal fun validSUMHPartitionName(name: String) = name.matches(Regex("[a-zA-Z0-9_-]{1,255}"))
internal fun partitionsSaveJson(names: List<String>): String {
    require(names.size <= 128 && names.distinct().size == names.size && names.all(::validSUMHPartitionName))
    return JSONArray(names).toString().also { require(it.toByteArray(Charsets.UTF_8).size <= 40 * 1024) }
}

internal object SUMHPartitionsParser {
    fun discoveryReply(reply: SUMHQueryReply): SUMHPartitionDiscoveryResult = try {
        SUMHPartitionDiscoveryResult(discovery(reply.stdout), reply.error)
    } catch (e: Exception) { SUMHPartitionDiscoveryResult(error = listOfNotNull(reply.error, e.message ?: "invalid_discovery").joinToString("\n")) }
    fun saveReply(reply: SUMHQueryReply): SUMHPartitionSaveResult = try {
        SUMHPartitionSaveResult(save(reply.stdout), reply.error)
    } catch (e: Exception) { SUMHPartitionSaveResult(error = listOfNotNull(reply.error, e.message ?: "invalid_save_result").joinToString("\n")) }
    fun discovery(text: String): SUMHPartitionDiscovery {
        val root = SUMHControlStateParser.document(text, 512 * 1024)
        require(root.integer("schema_version", 1, Int.MAX_VALUE) == 1 && root.text("kind") == "sumh.partitions.discovery" && root.bool("read_only"))
        require(root.text("scope") == "installed_module_candidates_not_mount_readiness" && root.bool("requires_reboot"))
        val status = root.text("status").also { require(it in setOf("complete", "partial", "unknown", "stale")) }
        val configStatus = root.text("config_status").also { require(it in setOf("valid", "missing", "unreadable", "invalid", "unsupported")) }
        val complete = root.bool("complete"); val stable = root.bool("stable"); val selection = root.bool("selection_valid")
        val saved = names(root); val revision = root.revision("config_revision")
        val boot = root.nullableText("boot_id", 80)?.also { require(it.isNotEmpty() && '\u0000' !in it) }
        val namespace = root.unsignedOrNull("namespace_ino")?.also { require(it != "0") }
        val candidates = root.array("candidates", 128).map { item ->
            val row = item.objectValue(); val name = row.text("name", 255).also { require(validSUMHPartitionName(it)) }
            val applicable = row.bool("applicable"); val rowStatus = row.text("status")
            require(rowStatus in setOf("applicable", "unavailable", "unknown") && applicable == (rowStatus == "applicable"))
            val target = row.obj("target"); val alias = row.obj("alias"); val mount = row.obj("mount")
            target.integer("errno", 0, 4095); alias.integer("errno", 0, 4095)
            alias.required("link_target").takeUnless { it.isJsonNull }?.let { SUMHInventoryParser.bytes(it.objectValue()) }
            mount.required("source").takeUnless { it.isJsonNull }?.let { SUMHInventoryParser.bytes(it.objectValue()) }
            val sources = row.array("sources", 512).map { raw -> raw.objectValue().let { source ->
                SUMHPartitionSource(SUMHInventoryParser.bytes(source.obj("module_id")), SUMHInventoryParser.bytes(source.obj("path")),
                    source.text("layout").also { require(it in setOf("top", "system")) }, source.bool("disabled"),
                    source.bool("remove_pending"), source.bool("skip_mount"), source.text("status"), source.text("reason"))
            } }
            require(row.bool("saved") == (name in saved))
            SUMHPartitionCandidate(name, row.bool("saved"), applicable, rowStatus, row.text("reason"),
                SUMHInventoryParser.bytes(target.obj("path")), target.text("status"), SUMHInventoryParser.bytes(alias.obj("path")),
                alias.text("status"), mount.nullableText("filesystem"), mount.text("status"), sources)
        }
        require(candidates.map { it.name }.distinct().size == candidates.size && candidates.sumOf { it.sources.size } <= 512)
        val issues = root.array("issues", 512).map { item -> item.objectValue().let { issue ->
            issue.integer("errno", 0, 4095)
            val path = SUMHInventoryParser.bytes(issue.obj("path"))
            val detail = SUMHInventoryParser.bytes(issue.obj("detail"), path = false)
            "${issue.text("code")}: ${partitionBytesLabel(path)} — ${partitionBytesLabel(detail)}"
        } }
        if (selection) require(status == "complete" && complete && stable && configStatus == "valid" && revision != null && boot != null && namespace != null)
        if (status == "complete") require(complete && stable)
        if (status == "stale") require(!stable && !selection)
        return SUMHPartitionDiscovery(status, complete, stable, selection, revision, configStatus, saved, boot, namespace, candidates, issues)
    }
    fun save(text: String): SUMHPartitionSave {
        val root = SUMHControlStateParser.document(text, 512 * 1024)
        require(root.integer("schema_version", 1, Int.MAX_VALUE) == 1 && root.text("kind") == "sumh.partitions.save" && !root.bool("read_only"))
        require(!root.bool("applied") && root.bool("requires_reboot"))
        val status = root.text("status").also { require(it in setOf("saved", "unchanged", "conflict", "rejected", "failed", "partial", "unknown")) }
        val saved = root.bool("saved"); val readback = root.bool("readback_confirmed"); val durable = root.bool("durability_confirmed")
        val revision = root.revision("config_revision"); val previous = root.revision("previous_revision")
        val names = names(root); val errno = root.integer("errno", 0, 4095)
        require(saved == (status in setOf("saved", "unchanged")))
        if (saved) require(readback && revision != null && previous != null && errno == 0)
        if (status == "saved") require(durable)
        if (!readback) require(names.isEmpty() && revision == null)
        return SUMHPartitionSave(status, saved, revision, previous, names, root.text("reason"),
            SUMHInventoryParser.bytes(root.obj("detail"), path = false), errno)
    }
    private fun names(root: JsonObject) = root.array("saved_partitions", 128).map {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isString)
        it.asString.also { name -> require(validSUMHPartitionName(name)) }
    }.also { require(it.distinct().size == it.size) }
    private fun JsonObject.array(key: String, max: Int) = required(key).let {
        require(it.isJsonArray && it.asJsonArray.size() <= max); it.asJsonArray
    }
}

internal fun partitionBytesLabel(value: SUMHInventoryBytes) = value.utf8?.let(::moduleTemplatePathLabel) ?: "hex:${value.hex}"
