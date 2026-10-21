package com.zying.zysu.ui.sumh

import com.zying.zysu.ui.sumh.util.SUMHPartitionDiscovery
import com.zying.zysu.ui.sumh.util.partitionsSaveJson

internal const val PARTITIONS_DRAFT_KEY = "partition-selection"
internal data class PartitionsDraft(val revision: String, val saved: List<String>, val selected: List<String>) {
    fun record() = EditorDraftRecord(listOf(revision, saved.size.toString()) + saved + selected)
    fun added() = selected.filterNot { it in saved }
    fun removed() = saved.filterNot { it in selected }
    fun rebase(sample: SUMHPartitionDiscovery): PartitionsDraft {
        require(sample.selectionValid)
        return PartitionsDraft(requireNotNull(sample.revision), sample.saved,
            (sample.saved.filterNot { it in removed() } + added()).distinct())
    }
    fun canSave(sample: SUMHPartitionDiscovery) = sample.selectionValid && sample.revision == revision && sample.saved == saved &&
        selected != saved && added().all { name -> sample.candidates.any { it.name == name && it.applicable } }
    companion object {
        fun from(record: EditorDraftRecord): PartitionsDraft {
            val size = record.fields[1].toInt()
            return PartitionsDraft(record.fields[0], record.fields.drop(2).take(size), record.fields.drop(2 + size))
        }
        fun valid(record: EditorDraftRecord): Boolean = try {
            require(record.fields.size in 2..258 && record.fields[0].matches(Regex("sha256-v1:[0-9a-f]{64}")))
            val count = record.fields[1].toInt(); require(count in 0..128 && record.fields.size >= count + 2)
            val value = from(record); partitionsSaveJson(value.saved); partitionsSaveJson(value.selected); true
        } catch (_: Exception) { false }
    }
}
