package com.zying.zysu.ui.sumh

import com.zying.zysu.ui.sumh.util.SUMH_MAPS_TRANSFER_BYTES
import com.zying.zysu.ui.sumh.util.SUMHMapsConflictChoice
import com.zying.zysu.ui.sumh.util.SUMHMapsImportMode
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.MapsPathRule

internal const val MAPS_TRANSFER_DRAFT_KEY = "maps-transfer:v1"
internal data class MapsTransferDraft(
    val text: String = "", val baseline: String,
    val mode: SUMHMapsImportMode = SUMHMapsImportMode.Merge,
    val choice: SUMHMapsConflictChoice = SUMHMapsConflictChoice.Unresolved,
    val pending: String = "", val pendingInput: String = "",
) {
    fun inputFingerprint() = draftFingerprint(listOf(text, mode.name, choice.name))
    fun record(): EditorDraftRecord {
        require(text.toByteArray().size <= SUMH_MAPS_TRANSFER_BYTES && Charsets.UTF_8.newEncoder().canEncode(text))
        val chunks = mutableListOf<String>()
        var start = 0
        while (start < text.length) {
            var end = minOf(text.length, start + 1024)
            if (end < text.length && text[end - 1].isHighSurrogate()) end--
            chunks.add(text.substring(start, end)); start = end
        }
        return EditorDraftRecord(listOf(baseline, mode.name, choice.name, pending, pendingInput) + chunks)
    }
    companion object {
        fun baseline(rules: List<MapsPathRule>) = draftFingerprint(rules.flatMap { listOf(it.targetPath, it.replacementPath) })
        fun from(record: EditorDraftRecord) = record.fields.let {
            MapsTransferDraft(it.drop(5).joinToString(""), it[0], SUMHMapsImportMode.valueOf(it[1]),
                SUMHMapsConflictChoice.valueOf(it[2]), it[3], it[4])
        }
        fun valid(record: EditorDraftRecord): Boolean = try {
            record.fields.size in 5..70 && record.fields[0].matches(Regex("[0-9a-f]{64}")) &&
                listOf(record.fields[3], record.fields[4]).all { it.isEmpty() || it.matches(Regex("[0-9a-f]{64}")) } &&
                from(record).text.toByteArray().size <= SUMH_MAPS_TRANSFER_BYTES
        } catch (_: Exception) { false }
    }
}
