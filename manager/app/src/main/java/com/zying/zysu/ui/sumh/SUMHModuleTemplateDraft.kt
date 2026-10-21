package com.zying.zysu.ui.sumh

import com.zying.zysu.ui.sumh.util.SUMHModuleTemplate
import com.zying.zysu.ui.sumh.util.SUMH_TEMPLATE_INPUT_BYTES

internal data class ModuleTemplateDraft(
    val template: SUMHModuleTemplate = SUMHModuleTemplate.All,
    val paths: String = "", val parent: String = "", val baseline: String,
) {
    fun record(): EditorDraftRecord {
        val chunks = mutableListOf<String>()
        var start = 0
        while (start < paths.length) {
            var end = minOf(paths.length, start + 1024)
            if (end < paths.length && paths[end - 1].isHighSurrogate()) end--
            chunks.add(paths.substring(start, end)); start = end
        }
        return EditorDraftRecord(listOf(template.name, parent, baseline) + chunks)
    }
    companion object {
        fun from(record: EditorDraftRecord) = record.fields.let {
            ModuleTemplateDraft(SUMHModuleTemplate.valueOf(it[0]), it.drop(3).joinToString(""), it[1], it[2])
        }
        fun valid(record: EditorDraftRecord): Boolean = try {
            record.fields.size in 3..68 && record.fields[2].matches(Regex("[0-9a-f]{64}")) &&
                from(record).paths.toByteArray(Charsets.UTF_8).size <= SUMH_TEMPLATE_INPUT_BYTES
        } catch (_: Exception) { false }
    }
}
