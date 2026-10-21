package com.zying.zysu.ui.sumh

import com.zying.zysu.ui.sumh.util.*
import org.json.JSONObject

internal data class HideDefinitionDraft(val id: String, val path: String, val name: String, val note: String,
    val group: String, val enabled: Boolean, val revision: String, val baseline: String) {
    val key get() = "hide-definition:" + id.ifEmpty { "new" }
    fun record() = EditorDraftRecord(listOf(id, path, name, note, group, enabled.toString(), revision, baseline))
    fun fields() = JSONObject().put("name", name).put("note", note).put("group", group).apply {
        if (id.isEmpty()) { put("path", path); put("enabled", enabled) }
    }.toString()
    fun arguments() = if (id.isEmpty()) listOf("hide", "create", revision, fields())
        else listOf("hide", "annotate", id, revision, fields())
    fun error(): String? = when {
        !path.startsWith('/') || !validHideDefinitionText(path, 4095) -> "path"
        !validHideDefinitionText(name, 128) -> "name"
        !validHideDefinitionText(note, 1024) -> "note"
        !validHideDefinitionText(group, 128) -> "group"
        else -> null
    }
    fun rebased(row: HideDefinition?, revision: String, keepInput: Boolean): HideDefinitionDraft {
        val latest = initial(row, revision)
        return if (keepInput) copy(path = if (id.isEmpty()) path else latest.path,
            enabled = if (id.isEmpty()) enabled else latest.enabled, revision = revision, baseline = latest.baseline) else latest
    }
    companion object {
        fun initial(row: HideDefinition?, revision: String) = HideDefinitionDraft(row?.id.orEmpty(), row?.path.orEmpty(),
            row?.name.orEmpty(), row?.note.orEmpty(), row?.group.orEmpty(), row?.enabled ?: true, revision, baseline(row))
        fun baseline(row: HideDefinition?) = draftFingerprint(row?.let { listOf(it.id, it.path, it.name, it.note, it.group, it.enabled.toString()) } ?: emptyList())
        fun from(record: EditorDraftRecord) = record.fields.let { HideDefinitionDraft(it[0], it[1], it[2], it[3], it[4], it[5] == "true", it[6], it[7]) }
        fun valid(key: String, record: EditorDraftRecord): Boolean = record.fields.let { f ->
            f.size == 8 && (f[0].isEmpty() || validHideDefinitionId(f[0])) && key == "hide-definition:" + f[0].ifEmpty { "new" } &&
                f[5] in setOf("true", "false") && validHideDefinitionRevision(f[6]) && f[7].matches(Regex("[0-9a-f]{64}")) &&
                f.take(5).all { validHideDefinitionText(it, 4095) }
        }
    }
}
