package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.LineBreak
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.*

/** Read-only renderer. The Activity workflow owns requests, frozen inputs and invalidation. */
@Composable internal fun SUMHModulePreviewDialog(
    moduleId: String,
    explainPath: String,
    onExplainPathChange: (String) -> Unit,
    busy: Boolean,
    stale: Boolean,
    canReadSaved: Boolean,
    canReadCandidate: Boolean,
    result: SUMHModulePreviewResult?,
    error: String?,
    onReadSaved: () -> Unit,
    onReadCandidate: () -> Unit,
    onClose: () -> Unit,
    eligibilityMessage: String? = null,
    request: SUMHModulePreviewRequest? = null,
) {
    var side by rememberSaveable(moduleId) { mutableStateOf("saved") }
    var section by rememberSaveable(moduleId) { mutableStateOf("summary") }
    val value = result?.value
    val sample = value?.sample
    val selectedSide = if (sample?.candidate == null) "saved" else side
    val plan = if (selectedSide == "candidate") sample?.candidate else sample?.baseline
    val failure = error ?: result?.error

    Dialog(onDismissRequest = onClose, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(
            Modifier.padding(horizontal = 12.dp).widthIn(max = 760.dp).fillMaxWidth().fillMaxHeight(.94f)
                .testTag("module_preview_dialog"), shape = MaterialTheme.shapes.extraLarge,
        ) {
            Column(Modifier.padding(16.dp)) {
                Text(stringResource(R.string.sumh_module_preview_title),
                    style = MaterialTheme.typography.titleLarge.copy(lineBreak = LineBreak.Heading))
                if (busy) LinearProgressIndicator(Modifier.fillMaxWidth().padding(top = 8.dp))
                LazyColumn(
                    Modifier.weight(1f).fillMaxWidth().testTag("module_preview_list"),
                    contentPadding = PaddingValues(vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    item("scope") {
                        Text(stringResource(R.string.sumh_module_preview_scope), style = MaterialTheme.typography.bodyMedium)
                    }
                    previewText("module", moduleTemplatePathLabel(moduleId), R.string.sumh_module_preview_module)
                    item("input") {
                        OutlinedTextField(
                            value = explainPath, onValueChange = onExplainPathChange,
                            label = { Text(stringResource(R.string.sumh_module_preview_path)) },
                            supportingText = { Text(stringResource(R.string.sumh_module_preview_path_hint)) },
                            singleLine = true, modifier = Modifier.fillMaxWidth().testTag("module_preview_path"),
                            keyboardOptions = KeyboardOptions(autoCorrectEnabled = false),
                        )
                    }
                    item("read_saved") {
                        OutlinedButton(
                            onClick = onReadSaved, enabled = canReadSaved && !busy,
                            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("module_preview_read_saved"),
                        ) { Text(stringResource(R.string.sumh_module_preview_saved_read)) }
                    }
                    item("read_candidate") {
                        OutlinedButton(
                            onClick = onReadCandidate, enabled = canReadCandidate && !busy,
                            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("module_preview_read_candidate"),
                        ) { Text(stringResource(R.string.sumh_module_preview_candidate_read)) }
                    }
                    item("draft_hint") { Text(stringResource(R.string.sumh_module_preview_draft_hint), style = MaterialTheme.typography.bodySmall) }
                    eligibilityMessage?.let { previewText("eligibility", it) }
                    item("status") {
                        Text(stringResource(when {
                            failure != null -> R.string.sumh_module_preview_failed
                            stale -> R.string.sumh_module_preview_local_stale
                            busy -> R.string.sumh_module_preview_loading
                            result == null -> R.string.sumh_module_preview_idle
                            value == null -> R.string.sumh_module_preview_failed
                            value.status == SUMHModulePreviewStatus.Stale -> R.string.sumh_module_preview_native_stale
                            value.status == SUMHModulePreviewStatus.Invalid -> R.string.sumh_module_preview_invalid
                            value.status == SUMHModulePreviewStatus.Partial -> R.string.sumh_module_preview_partial
                            value.status == SUMHModulePreviewStatus.Complete -> R.string.sumh_module_preview_complete
                            else -> R.string.sumh_module_preview_unknown
                        }), modifier = Modifier.testTag("module_preview_status"), style = MaterialTheme.typography.titleSmall)
                    }
                    failure?.let { previewText("failure", it, R.string.sumh_module_preview_reason) }
                    if (value != null) {
                        if (busy || failure != null) item("history") { Text(stringResource(R.string.sumh_module_preview_history)) }
                        previewText("reason", "${value.reason}; errno=${value.errno}", R.string.sumh_module_preview_reason)
                        value.detail?.let { previewBytes("detail", R.string.sumh_module_preview_reason, it) }
                    }
                    if (request != null) {
                        item("frozen_input") {
                            Text(stringResource(R.string.sumh_module_preview_snapshot), style = MaterialTheme.typography.titleSmall)
                            Text(if (request.candidateMode == null) stringResource(R.string.sumh_module_preview_saved)
                                else pluralStringResource(R.plurals.sumh_module_preview_candidate_snapshot,
                                    request.candidateRules?.size ?: 0, request.candidateMode,
                                    request.candidateRules?.size ?: 0))
                        }
                        previewText("frozen_path", moduleTemplatePathLabel(request.explainPath), R.string.sumh_module_preview_requested_path)
                        request.candidateRevision?.let { previewText("frozen_revision", "candidate revision: $it") }
                    }
                    item("limits") {
                        Text(stringResource(R.string.sumh_module_preview_limits), style = MaterialTheme.typography.bodySmall)
                        Text(stringResource(R.string.sumh_module_preview_reboot), style = MaterialTheme.typography.bodySmall)
                    }
                    if (sample != null) {
                        previewBytes("requested", R.string.sumh_module_preview_requested_path, sample.requestedPath)
                        previewBytes("resolved", R.string.sumh_module_preview_resolved_path, sample.resolvedPath)
                        sample.missingCandidateModules.forEachIndexed { i, id -> item("missing_$i") {
                            Text(stringResource(R.string.sumh_module_preview_missing, moduleTemplatePathLabel(id)))
                        } }
                        item("side") {
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                PreviewChip("saved", selectedSide, R.string.sumh_module_preview_saved, "side") { side = it }
                                if (sample.candidate != null) {
                                    PreviewChip("candidate", selectedSide, R.string.sumh_module_preview_candidate, "side") { side = it }
                                    PreviewChip("difference", selectedSide, R.string.sumh_module_preview_difference, "side") { side = it }
                                }
                            }
                        }
                        if (selectedSide == "difference") {
                            item("diff_scope") { Text(stringResource(R.string.sumh_module_preview_diff_scope)) }
                            if (stale || busy || failure != null || !value.comparisonValid || sample.diff?.complete != true) {
                                item("diff_unavailable") { Text(stringResource(R.string.sumh_module_preview_diff_unavailable)) }
                            } else {
                                val changes = sample.diff.changes
                                if (changes.isEmpty()) item("diff_empty") { Text(stringResource(R.string.sumh_module_preview_diff_empty)) }
                                changes.forEachIndexed { i, change ->
                                    item("diff_${i}_change") {
                                        HorizontalDivider()
                                        Text(stringResource(R.string.sumh_module_preview_change, stringResource(
                                            if (change.change == "added") R.string.sumh_module_preview_added else R.string.sumh_module_preview_removed), change.count))
                                    }
                                    previewContribution("difference_$i", change.contribution)
                                }
                            }
                        } else if (plan != null) {
                            item("sections") {
                                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                    listOf("summary" to R.string.sumh_module_preview_summary,
                                        "configuration" to R.string.sumh_module_preview_configuration,
                                        "actions" to R.string.sumh_module_preview_actions,
                                        "explanation" to R.string.sumh_module_preview_explanation,
                                        "trace" to R.string.sumh_module_preview_trace,
                                        "diagnostics" to R.string.sumh_module_preview_diagnostics).forEach { (key, label) ->
                                        PreviewChip(key, section, label, "section") { section = it }
                                    }
                                }
                            }
                            val prefix = "${selectedSide}_$section"
                            when (section) {
                                "summary" -> previewSummary(prefix, plan)
                                "configuration" -> previewConfiguration(prefix,
                                    if (selectedSide == "candidate") requireNotNull(sample.after) else sample.before)
                                "actions" -> {
                                    previewHeading("${prefix}_heading", R.string.sumh_module_preview_actions, plan.contributions.size)
                                    if (plan.contributions.isEmpty()) item("${prefix}_empty") { Text(stringResource(R.string.sumh_module_preview_no_actions)) }
                                    plan.contributions.forEachIndexed { i, contribution -> previewContribution("${prefix}_$i", contribution) }
                                }
                                "explanation", "trace" -> {
                                    val events = if (section == "explanation") plan.explainEvents else plan.trace
                                    previewHeading("${prefix}_heading", if (section == "explanation") R.string.sumh_module_preview_explanation
                                        else R.string.sumh_module_preview_trace, events.size)
                                    if (section == "explanation" && sample.requestedPath.size == 0) item("${prefix}_path") {
                                        Text(stringResource(R.string.sumh_module_preview_no_path))
                                    }
                                    if (events.isEmpty()) item("${prefix}_empty") { Text(stringResource(R.string.sumh_module_preview_no_events)) }
                                    if (section == "explanation") plan.hiddenAncestors.forEachIndexed { i, ancestor ->
                                        previewBytes("${prefix}_hidden_$i", R.string.sumh_module_preview_hidden_parent, ancestor)
                                    }
                                    events.forEachIndexed { i, event -> previewEvent("${prefix}_$i", event, plan.compiled && plan.complete) }
                                }
                                "diagnostics" -> previewDiagnostics(prefix, value, sample, plan)
                            }
                        }
                    }
                }
                TextButton(onClick = onClose, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("module_preview_close")) {
                    Text(stringResource(R.string.close))
                }
            }
        }
    }
}

@Composable private fun PreviewChip(key: String, selected: String, label: Int, group: String, onClick: (String) -> Unit) {
    FilterChip(selected = selected == key, onClick = { onClick(key) }, label = { Text(stringResource(label)) },
        modifier = Modifier.heightIn(min = 48.dp).testTag("module_preview_${group}_$key"))
}

@Composable private fun previewBoolean(value: Boolean?): String = stringResource(when (value) {
    true -> R.string.sumh_module_preview_yes; false -> R.string.sumh_module_preview_no; null -> R.string.sumh_module_preview_unobserved
})

internal fun modulePreviewBytesLabel(value: SUMHInventoryBytes): String =
    value.utf8?.let(::moduleTemplatePathLabel) ?: "hex:${value.hex}"

/** Keep long diagnostic strings in independently composed text nodes, without splitting UTF-16 pairs. */
internal fun modulePreviewTextChunks(value: String): List<String> {
    if (value.isEmpty()) return listOf("")
    val result = mutableListOf<String>()
    var offset = 0
    while (offset < value.length) {
        var end = minOf(offset + 2048, value.length)
        if (end < value.length && value[end - 1].isHighSurrogate()) end--
        result += value.substring(offset, end)
        offset = end
    }
    return result
}

private fun LazyListScope.previewHeading(key: String, label: Int, count: Int) {
    item(key) { Text(stringResource(R.string.sumh_module_preview_section_count, stringResource(label), count),
        style = MaterialTheme.typography.titleSmall) }
}

private fun LazyListScope.previewText(key: String, value: String, label: Int? = null) {
    val chunks = modulePreviewTextChunks(value)
    chunks.forEachIndexed { index, text -> item("${key}_part_$index") {
        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            if (index == 0 && label != null) Text(stringResource(label), style = MaterialTheme.typography.labelLarge)
            if (chunks.size > 1) Text(stringResource(R.string.sumh_module_preview_part, index + 1, chunks.size),
                style = MaterialTheme.typography.labelSmall)
            SelectionContainer { Text(text.ifEmpty { stringResource(R.string.sumh_module_preview_empty) },
                fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.testTag("module_preview_$key")) }
        }
    } }
}

private fun LazyListScope.previewBytes(key: String, label: Int, value: SUMHInventoryBytes) {
    if (value.size != 0) previewText(key, modulePreviewBytesLabel(value), label)
}

private fun LazyListScope.previewSummary(prefix: String, plan: SUMHModulePlan) {
    item("${prefix}_flags") {
        Text(stringResource(R.string.sumh_module_preview_plan_flags, previewBoolean(plan.compiled), previewBoolean(plan.complete),
            previewBoolean(plan.validForCurrentInputs), plan.sourceObservations))
        if (plan.otherBackendsUnplanned) Text(stringResource(R.string.sumh_module_preview_other_backends))
        if (plan.eventsTruncated || plan.observationsTruncated) Text(stringResource(R.string.sumh_module_preview_truncated))
        Text(stringResource(R.string.sumh_module_preview_source_limit), style = MaterialTheme.typography.bodySmall)
    }
    previewBytes("${prefix}_error", R.string.sumh_module_preview_reason, plan.error)
    previewHeading("${prefix}_order", R.string.sumh_module_preview_emission_order, plan.moduleOrder.size)
    plan.moduleOrder.forEachIndexed { i, id -> previewText("${prefix}_order_$i", "$i · ${modulePreviewBytesLabel(id)}") }
    previewHeading("${prefix}_decisions", R.string.sumh_module_preview_summary, plan.decisions.size)
    plan.decisions.forEachIndexed { i, decision ->
        val key = "${prefix}_decision_$i"
        item("${key}_heading") { HorizontalDivider() }
        previewBytes("${key}_module", R.string.sumh_module_preview_module, decision.moduleId)
        previewBytes("${key}_module_path", R.string.sumh_module_preview_module_path, decision.modulePath)
        item("${key}_selection") {
            Text(stringResource(R.string.sumh_module_preview_backend, decision.configured, decision.global, decision.requested,
                decision.selected, decision.selectionSource))
            Text(stringResource(R.string.sumh_module_preview_backend_flags, previewBoolean(decision.fallback), previewBoolean(decision.complete),
                previewBoolean(decision.hasContent), previewBoolean(decision.requiresMagic)))
        }
        previewText("${key}_reason", decision.reason, R.string.sumh_module_preview_reason)
        previewBytes("${key}_detail", R.string.sumh_module_preview_reason, decision.detail)
        listOf("SUMH" to decision.sumhProbe, "OverlayFS" to decision.overlayProbe).forEach { (name, probe) ->
            item("${key}_probe_$name") {
                Text(stringResource(R.string.sumh_module_preview_probe, name, previewBoolean(probe.checked), previewBoolean(probe.known),
                    previewBoolean(probe.available), probe.errno), style = MaterialTheme.typography.bodySmall)
            }
            previewBytes("${key}_probe_${name}_detail", R.string.sumh_module_preview_reason, probe.detail)
        }
    }
}

private fun LazyListScope.previewContribution(prefix: String, contribution: SUMHModuleContribution) {
    item("${prefix}_heading") {
        HorizontalDivider()
        Text("#${contribution.index} ${contribution.action}", style = MaterialTheme.typography.titleSmall,
            modifier = Modifier.testTag("module_preview_$prefix"))
    }
    previewBytes("${prefix}_target", R.string.sumh_module_preview_target, contribution.path)
    contribution.source?.let { previewBytes("${prefix}_source", R.string.sumh_module_preview_source, it) }
    if (contribution.writePolicy != null) item("${prefix}_write") { Text(stringResource(R.string.sumh_module_preview_write_through)) }
    contribution.hiddenAncestors.forEachIndexed { i, ancestor -> previewBytes("${prefix}_hidden_$i", R.string.sumh_module_preview_hidden_parent, ancestor) }
}

private fun LazyListScope.previewEvent(prefix: String, event: SUMHModulePlanEvent, complete: Boolean) {
    item("${prefix}_heading") {
        HorizontalDivider()
        Text(event.action, style = MaterialTheme.typography.titleSmall, modifier = Modifier.testTag("module_preview_$prefix"))
        Text(stringResource(R.string.sumh_module_preview_event_flags, event.requestedMode, event.effectiveMode, previewBoolean(event.alias)))
        if (complete && event.action in setOf("ADD", "MERGE")) Text(stringResource(R.string.sumh_module_preview_index, event.mappingIndex))
    }
    previewText("${prefix}_reason", event.reason, R.string.sumh_module_preview_reason)
    previewBytes("${prefix}_module", R.string.sumh_module_preview_module, event.moduleId)
    previewBytes("${prefix}_original", R.string.sumh_module_preview_original_path, event.originalPath)
    previewBytes("${prefix}_resolved", R.string.sumh_module_preview_resolved_path, event.resolvedPath)
    previewBytes("${prefix}_matched", R.string.sumh_module_preview_matched_rule, event.matchedRulePath)
    previewBytes("${prefix}_hidden", R.string.sumh_module_preview_hidden_parent, event.hiddenParent)
    previewBytes("${prefix}_source", R.string.sumh_module_preview_source, event.source)
    if (event.hasPreviousDirect) {
        previewBytes("${prefix}_previous_module", R.string.sumh_module_preview_previous_module, event.previousModuleId)
        previewBytes("${prefix}_previous_source", R.string.sumh_module_preview_previous_source, event.previousSource)
    }
}

private fun LazyListScope.previewConfiguration(prefix: String, configuration: SUMHModuleConfiguration) {
    previewText("${prefix}_global", "mount_backend=${configuration.mountBackend}\noverlayfs_enabled=${configuration.overlayEnabled}\nmagic_mount_enabled=${configuration.magicEnabled}")
    configuration.partitions.forEachIndexed { i, partition -> previewText("${prefix}_partition_$i", "partition: ${modulePreviewBytesLabel(partition)}") }
    configuration.modules.forEachIndexed { i, module ->
        val key = "${prefix}_module_$i"
        previewBytes("${key}_id", R.string.sumh_module_preview_module, module.id)
        previewText("${key}_mode", "mode=${module.mode}; rules=${module.rules.size}")
        module.rules.forEachIndexed { j, rule -> previewText("${key}_rule_$j", "${modulePreviewBytesLabel(rule.path)} → ${rule.mode}") }
    }
}

private fun LazyListScope.previewDiagnostics(prefix: String, value: SUMHModulePreview, sample: SUMHModulePreviewSample, plan: SUMHModulePlan) {
    previewText("${prefix}_identity", "boot=${sample.bootId}\nnamespace=${sample.namespace}\nstable=${sample.stable}\ncomparison_valid=${value.comparisonValid}")
    previewText("${prefix}_revisions", "config=${sample.configRevision}\nmodules=${sample.moduleRevision}\ncandidate=${sample.candidateRevision}\nsource=${plan.sourceRevision}\nplan=${plan.planRevision}")
    previewText("${prefix}_runtime", "runtime_known=${sample.runtime.known}\ninstance=${sample.runtime.instance}\ngeneration=${sample.runtime.generation}")
    value.unchecked.forEachIndexed { i, check -> previewText("${prefix}_unchecked_$i", "unchecked: $check") }
    sample.changedModuleIds.forEachIndexed { i, id -> previewText("${prefix}_changed_$i", "changed_module: ${moduleTemplatePathLabel(id)}") }
    sample.documents.entries.forEachIndexed { i, (name, document) ->
        previewText("${prefix}_document_$i", "$name: ${document.status}; errno=${document.errno}\nrevision=${document.revision}")
    }
    val ownership = sample.ownership
    val boot = ownership.bootPlan
    previewText("${prefix}_owner", "external_owner=not_observed\nexternal_owner_absence_proven=false\nhot_apply_authorized=false\nboot_plan_file=${ownership.bootPlanStatus}\nboot_plan_revision=${ownership.bootPlanRevision}")
    previewText("${prefix}_boot_plan", "boot_plan_format=${boot.document.status}; errno=${boot.document.errno}\nformat_valid=${boot.formatValid}\ncurrent_boot=${boot.currentBoot}\nboot_id=${boot.bootId}\nauthorizes_apply=false")
    boot.moduleIds.forEachIndexed { i, module -> previewText("${prefix}_boot_module_$i", modulePreviewBytesLabel(module)) }
    previewText("${prefix}_budgets", "input ≤ 64 KiB; response ≤ 2 MiB\ndiscovery ≤ 512; events ≤ 2048; source observations ≤ 4096\ndiagnostic bytes ≤ 512 KiB; contributions ≤ 4096")
}
