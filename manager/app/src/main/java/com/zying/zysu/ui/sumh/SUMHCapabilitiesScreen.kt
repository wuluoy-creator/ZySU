package com.zying.zysu.ui.sumh

import androidx.activity.ComponentActivity
import androidx.activity.compose.LocalActivity
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.*
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.ramcosta.composedestinations.spec.Direction
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiTopAppBar
import com.zying.zysu.ui.component.YukiTopBarTitle
import com.zying.zysu.ui.sumh.util.*
import java.util.UUID

internal fun sumhCapabilitiesDestination(destination: SUMHCapabilityDestination): Direction = when (destination) {
    SUMHCapabilityDestination.Debug -> DiagnosticsScreenDestination
    SUMHCapabilityDestination.Modules -> ModuleScreenDestination
    SUMHCapabilityDestination.Mount -> MountIsolationScreenDestination(initialPage = 0)
    SUMHCapabilityDestination.Isolation -> MountIsolationScreenDestination(initialPage = 1)
    SUMHCapabilityDestination.UserHide -> MountIsolationScreenDestination(initialPage = 2)
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun SUMHCapabilitiesScreen(navigator: DestinationsNavigator) {
    val operations = rememberSUMHCapabilitiesOperations()
    val activity = LocalActivity.current as? ComponentActivity
    val session = rememberSaveable { UUID.randomUUID().toString() }
    SideEffect { operations.attach(session) }
    DisposableEffect(operations, session, activity) {
        onDispose { if (activity?.isChangingConfigurations != true) operations.close(session) }
    }
    Scaffold(
        topBar = {
            YukiTopAppBar(
                title = { YukiTopBarTitle(stringResource(R.string.sumh_capabilities_title)) },
                navigationIcon = {
                    IconButton(onClick = { operations.close(session); navigator.navigateUp() }) {
                        YukiIcon(Icons.AutoMirrored.Outlined.ArrowBack, stringResource(R.string.back))
                    }
                },
            )
        },
        contentWindowInsets = WindowInsets.safeDrawing,
    ) { padding ->
        SUMHCapabilitiesContent(operations.result, operations.busy,
            onRefresh = { operations.read(session, SUMHManager::capabilities) },
            onNavigate = { operations.close(session); navigator.navigate(sumhCapabilitiesDestination(it)) },
            modifier = Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding))
    }
}

/** A read-only renderer: the only effects are explicit read and navigation callbacks. */
@Composable
internal fun SUMHCapabilitiesContent(
    result: SUMHCapabilitiesResult?,
    busy: Boolean,
    onRefresh: () -> Unit,
    onNavigate: (SUMHCapabilityDestination) -> Unit,
    modifier: Modifier = Modifier,
) {
    var expandedFeature by rememberSaveable { mutableStateOf<String?>(null) }
    var sources by rememberSaveable { mutableStateOf(false) }
    val snapshot = result?.snapshot
    val unconfirmed = busy || result?.error != null
    LazyColumn(modifier.testTag("capabilities_list"), contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)) {
        item("summary") {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.sumh_capabilities_intro), style = MaterialTheme.typography.bodyMedium)
                Text(stringResource(when {
                    busy -> R.string.sumh_capabilities_loading
                    result?.error != null -> R.string.sumh_capabilities_failed
                    snapshot == null -> R.string.sumh_capabilities_idle
                    snapshot.hasRetained -> R.string.sumh_capabilities_retained
                    snapshot.sections.any { it.second.status != SUMHCapabilityStatus.Known } -> R.string.sumh_capabilities_partial
                    else -> R.string.sumh_capabilities_fresh
                }), modifier = Modifier.testTag("capabilities_status"), style = MaterialTheme.typography.bodyMedium)
                if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
                OutlinedButton(onClick = onRefresh, enabled = !busy,
                    modifier = Modifier.heightIn(min = 48.dp).testTag("capabilities_refresh")) {
                    Text(stringResource(R.string.sumh_capabilities_read))
                }
                Text(stringResource(R.string.sumh_capabilities_navigation_only), style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                TextButton(onClick = { onNavigate(SUMHCapabilityDestination.Debug) },
                    modifier = Modifier.heightIn(min = 48.dp).testTag("capabilities_diagnostics")) {
                    Text(stringResource(R.string.sumh_capabilities_diagnostics))
                }
            }
        }
        result?.error?.let { failure -> items(capabilityTextChunks(failure), key = { "failure_${it.first}" }) { (index, text) ->
            Text(text, color = MaterialTheme.colorScheme.error, modifier = Modifier.testTag("capabilities_failure_$index"))
        } }
        if (snapshot != null) {
            item("identity") {
                ConfigSection(stringResource(R.string.sumh_capabilities_identity)) {
                    Text(stringResource(R.string.sumh_capabilities_boot, snapshot.bootId ?: unknown()))
                    Text(stringResource(R.string.sumh_capabilities_interval, snapshot.startedMs, snapshot.finishedMs),
                        style = MaterialTheme.typography.bodySmall)
                    SectionProvenance("runtime", snapshot.runtime, unconfirmed)
                    snapshot.runtime.value?.let { runtime ->
                        Text(stringResource(R.string.sumh_capabilities_generation, runtime.sample.instanceId ?: unknown(),
                            runtime.sample.generation ?: unknown()), style = MaterialTheme.typography.bodySmall)
                        Text(stringResource(R.string.sumh_capabilities_protocol, runtime.protocolStatus, runtime.kernelProtocol),
                            style = MaterialTheme.typography.bodySmall)
                        Text(stringResource(R.string.sumh_capabilities_bits, runtime.capabilityBits ?: unknown()),
                            style = MaterialTheme.typography.bodySmall)
                    }
                    Text(stringResource(R.string.sumh_capabilities_generation_scope), style = MaterialTheme.typography.bodySmall)
                }
            }
            item("backends") {
                ConfigSection(stringResource(R.string.sumh_capabilities_backend_title)) {
                    Text(stringResource(R.string.sumh_capabilities_saved_backend, snapshot.saved.value?.mountBackend ?: unknown()),
                        modifier = Modifier.testTag("capabilities_saved_backend"))
                    SectionProvenance("saved", snapshot.saved, unconfirmed)
                    snapshot.backends.value?.items?.forEach { backend ->
                        Text(stringResource(R.string.sumh_capabilities_backend_item, backend.id, stringResource(when (backend.status) {
                            SUMHCapabilityBackendStatus.Available -> R.string.sumh_capabilities_backend_available
                            SUMHCapabilityBackendStatus.Unavailable -> R.string.sumh_capabilities_backend_unavailable
                            SUMHCapabilityBackendStatus.Unknown -> R.string.sumh_capabilities_backend_unknown
                        })), modifier = Modifier.testTag("capabilities_backend_${backend.id}"))
                        Text(stringResource(R.string.sumh_capabilities_source, backend.source), style = MaterialTheme.typography.bodySmall)
                        Text(stringResource(R.string.sumh_capabilities_reason, backend.reason), style = MaterialTheme.typography.bodySmall)
                    }
                    SectionProvenance("backends", snapshot.backends, unconfirmed)
                    Text(stringResource(R.string.sumh_capabilities_owner_unknown), modifier = Modifier.testTag("capabilities_owner"))
                    SectionProvenance("owner", snapshot.owner, unconfirmed)
                    Text(stringResource(R.string.sumh_capabilities_backend_limit), style = MaterialTheme.typography.bodySmall)
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        TextButton(onClick = { onNavigate(SUMHCapabilityDestination.Mount) },
                            modifier = Modifier.heightIn(min = 48.dp).testTag("capabilities_backend_settings")) {
                            Text(stringResource(R.string.sumh_capabilities_backend_settings))
                        }
                        TextButton(onClick = { onNavigate(SUMHCapabilityDestination.Modules) },
                            modifier = Modifier.heightIn(min = 48.dp).testTag("capabilities_modules")) {
                            Text(stringResource(R.string.sumh_capabilities_modules))
                        }
                    }
                }
            }
            item("build") {
                ConfigSection(stringResource(R.string.sumh_capabilities_build_title)) {
                    SectionProvenance("build", snapshot.build, unconfirmed)
                    snapshot.build.value?.let { build ->
                        Text(stringResource(R.string.sumh_capabilities_build_current), style = MaterialTheme.typography.titleSmall)
                        Text(build.release + "\n" + build.version, fontFamily = FontFamily.Monospace,
                            modifier = Modifier.testTag("capabilities_build_current"))
                        Text(stringResource(R.string.sumh_capabilities_build_original), style = MaterialTheme.typography.titleSmall)
                        Text(build.originalRelease + "\n" + build.originalVersion, fontFamily = FontFamily.Monospace,
                            modifier = Modifier.testTag("capabilities_build_original"))
                        Text(stringResource(R.string.sumh_capabilities_build_kmi, build.originalKmiLabel ?: unknown()),
                            modifier = Modifier.testTag("capabilities_build_kmi"))
                        Text(stringResource(R.string.sumh_capabilities_build_consistency, build.consistency),
                            style = MaterialTheme.typography.bodySmall)
                        Text(stringResource(R.string.sumh_capabilities_generation, build.instanceId ?: unknown(), build.generation ?: unknown()),
                            style = MaterialTheme.typography.bodySmall)
                    }
                    Text(stringResource(R.string.sumh_capabilities_build_limit), style = MaterialTheme.typography.bodySmall)
                    Text(stringResource(R.string.sumh_capabilities_build_stage, snapshot.saved.value?.kernelBuildApplyStage ?: unknown()),
                        style = MaterialTheme.typography.bodySmall)
                    if (snapshot.saved.retained || unconfirmed) {
                        Historical()
                        Text(stringResource(R.string.sumh_capabilities_boot, snapshot.saved.bootId ?: unknown()), style = MaterialTheme.typography.bodySmall)
                        Text(stringResource(R.string.sumh_capabilities_interval, snapshot.saved.startedMs, snapshot.saved.finishedMs),
                            style = MaterialTheme.typography.bodySmall)
                    }
                    TextButton(onClick = { onNavigate(SUMHCapabilityDestination.Isolation) },
                        modifier = Modifier.heightIn(min = 48.dp).testTag("capabilities_build_settings")) {
                        Text(stringResource(R.string.sumh_capabilities_configure))
                    }
                }
            }
            items(snapshot.features(), key = { "feature_${it.id}" }) { feature ->
                ConfigSection(stringResource(capabilityTitle(feature.id))) {
                    val historical = unconfirmed || feature.retained
                    if (historical) Historical(Modifier.testTag("capabilities_${feature.id}_historical"))
                    Text(stringResource(R.string.sumh_capabilities_support, capabilityFlag(feature.supported, support = true)),
                        modifier = Modifier.testTag("capabilities_${feature.id}_support"))
                    Text(stringResource(R.string.sumh_capabilities_saved, capabilityFlag(feature.saved)),
                        modifier = Modifier.testTag("capabilities_${feature.id}_saved"))
                    Text(stringResource(R.string.sumh_capabilities_configured, capabilityFlag(feature.configured)),
                        modifier = Modifier.testTag("capabilities_${feature.id}_configured"))
                    Text(stringResource(R.string.sumh_capabilities_active, capabilityFlag(feature.active)),
                        modifier = Modifier.testTag("capabilities_${feature.id}_active"))
                    Text(stringResource(if (historical) R.string.sumh_capabilities_unknown else when (feature.comparison) {
                        SUMHComparison.Matches -> R.string.sumh_capabilities_matches
                        SUMHComparison.Drift -> R.string.sumh_capabilities_drift
                        SUMHComparison.Unsupported -> R.string.sumh_capabilities_unsupported
                        SUMHComparison.Deferred -> R.string.sumh_capabilities_deferred
                        SUMHComparison.Unknown -> R.string.sumh_capabilities_unknown
                    }), modifier = Modifier.testTag("capabilities_${feature.id}_comparison"), style = MaterialTheme.typography.bodySmall)
                    if (feature.degraded == true) Text(stringResource(R.string.sumh_capabilities_degraded), style = MaterialTheme.typography.bodySmall)
                    feature.lastSetErrno?.takeIf { it != 0 }?.let {
                        Text(stringResource(R.string.sumh_capabilities_setter_error, it), style = MaterialTheme.typography.bodySmall)
                    }
                    Text(stringResource(R.string.sumh_capabilities_reason, feature.reason), style = MaterialTheme.typography.bodySmall)
                    Text(stringResource(capabilityPhase(feature.phase)), style = MaterialTheme.typography.bodySmall)
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        TextButton(onClick = { onNavigate(feature.destination) },
                            modifier = Modifier.heightIn(min = 48.dp).testTag("capabilities_${feature.id}_configure")) {
                            Text(stringResource(R.string.sumh_capabilities_configure))
                        }
                        TextButton(onClick = { expandedFeature = if (expandedFeature == feature.id) null else feature.id },
                            modifier = Modifier.heightIn(min = 48.dp).testTag("capabilities_${feature.id}_details")) {
                            Text(stringResource(if (expandedFeature == feature.id) R.string.sumh_capabilities_less else R.string.sumh_capabilities_details))
                        }
                    }
                    if (expandedFeature == feature.id) {
                        Text(stringResource(R.string.sumh_capabilities_policy_label), style = MaterialTheme.typography.titleSmall,
                            modifier = Modifier.semantics { heading() })
                        Text(stringResource(capabilityScope(feature.scope)), modifier = Modifier.testTag("capabilities_${feature.id}_scope"))
                        Text(stringResource(R.string.sumh_capabilities_observers_unmeasured), style = MaterialTheme.typography.bodySmall)
                        SectionProvenance("saved", snapshot.saved, unconfirmed)
                        SectionProvenance("runtime", snapshot.runtime, unconfirmed)
                    }
                }
            }
            item("source_toggle") {
                TextButton(onClick = { sources = !sources }, modifier = Modifier.heightIn(min = 48.dp).testTag("capabilities_sources")) {
                    Text(stringResource(R.string.sumh_capabilities_sources))
                }
            }
            if (sources) items(snapshot.sections, key = { "section_${it.first}" }) { (id, section) ->
                ConfigSection(stringResource(capabilitySectionTitle(id))) {
                    SectionProvenance(id, section, unconfirmed)
                    if (id == "saved") snapshot.saved.value?.let {
                        Text(stringResource(R.string.sumh_capabilities_revision, it.revision), fontFamily = FontFamily.Monospace,
                            style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        }
    }
}

@Composable private fun SectionProvenance(id: String, section: SUMHCapabilitySection<*>, busy: Boolean) {
    if (section.retained || busy && section.value != null) Historical()
    Text(stringResource(R.string.sumh_capabilities_section_status, stringResource(capabilitySectionTitle(id)), stringResource(when (section.status) {
        SUMHCapabilityStatus.Known -> R.string.sumh_capabilities_section_known
        SUMHCapabilityStatus.Unsupported -> R.string.sumh_capabilities_unsupported
        SUMHCapabilityStatus.Unknown -> R.string.sumh_capabilities_unknown
        SUMHCapabilityStatus.Error -> R.string.sumh_capabilities_section_error
    }), section.errno), style = MaterialTheme.typography.bodySmall)
    Text(stringResource(R.string.sumh_capabilities_source, section.source), style = MaterialTheme.typography.bodySmall)
    Text(stringResource(R.string.sumh_capabilities_boot, section.bootId ?: unknown()), style = MaterialTheme.typography.bodySmall)
    Text(stringResource(R.string.sumh_capabilities_interval, section.startedMs, section.finishedMs), style = MaterialTheme.typography.bodySmall)
    Text(stringResource(R.string.sumh_capabilities_reason, section.reason), style = MaterialTheme.typography.bodySmall)
    section.error?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
    section.attempt?.let { attempt ->
        Text(stringResource(R.string.sumh_capabilities_latest_attempt), style = MaterialTheme.typography.labelLarge)
        SectionProvenance(id, attempt, false)
        (attempt.value as? SUMHCapabilitiesRuntime)?.let { runtime ->
            Text(stringResource(R.string.sumh_capabilities_protocol, runtime.protocolStatus, runtime.kernelProtocol),
                style = MaterialTheme.typography.bodySmall)
            Text(stringResource(R.string.sumh_capabilities_bits, runtime.capabilityBits ?: unknown()),
                style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable private fun Historical(modifier: Modifier = Modifier) = Text(stringResource(R.string.sumh_capabilities_historical),
    modifier = modifier, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
@Composable private fun unknown() = stringResource(R.string.sumh_capabilities_unknown)
@Composable private fun capabilityFlag(value: Boolean?, support: Boolean = false) = stringResource(when (value) {
    true -> if (support) R.string.sumh_capabilities_yes else R.string.sumh_capabilities_on
    false -> if (support) R.string.sumh_capabilities_no else R.string.sumh_capabilities_off
    null -> R.string.sumh_capabilities_unknown
})
private fun capabilityTitle(id: String) = when (id) {
    "kernel_debug" -> R.string.sumh_kernel_debug; "stealth" -> R.string.sumh_stealth
    "mount_hide" -> R.string.sumh_mount_hide; "maps_spoof" -> R.string.sumh_maps_spoof
    "statfs_spoof" -> R.string.sumh_statfs_spoof; "kernel_build_spoof" -> R.string.sumh_kernel_build_spoof
    "user_hide" -> R.string.sumh_state_hide; "maps_rules" -> R.string.sumh_state_maps
    "modules" -> R.string.sumh_state_modules; else -> R.string.sumh_state_xattrs
}
private fun capabilitySectionTitle(id: String) = when (id) {
    "saved" -> R.string.sumh_capabilities_section_saved; "runtime" -> R.string.sumh_capabilities_section_runtime
    "build" -> R.string.sumh_capabilities_section_build; "backends" -> R.string.sumh_capabilities_section_backends
    else -> R.string.sumh_capabilities_section_owner
}
private fun capabilityPhase(phase: SUMHCapabilityPhase) = when (phase) {
    SUMHCapabilityPhase.ControlApply -> R.string.sumh_capabilities_phase_control
    SUMHCapabilityPhase.ModulePlanOrBoot -> R.string.sumh_capabilities_phase_modules
    SUMHCapabilityPhase.BindingEvents -> R.string.sumh_capabilities_phase_hide
    SUMHCapabilityPhase.MapsApply -> R.string.sumh_capabilities_phase_maps
    SUMHCapabilityPhase.KernelBuildStage -> R.string.sumh_capabilities_phase_build
    SUMHCapabilityPhase.Unknown -> R.string.sumh_capabilities_phase_unknown
}
private fun capabilityScope(scope: String) = when (scope) {
    "modules" -> R.string.sumh_capabilities_scope_modules
    "stealth" -> R.string.sumh_capabilities_scope_stealth
    "user_hide" -> R.string.sumh_capabilities_scope_hide
    "mount_hide" -> R.string.sumh_capabilities_scope_mount
    "maps_spoof" -> R.string.sumh_capabilities_scope_maps
    "statfs_spoof" -> R.string.sumh_capabilities_scope_statfs
    "kernel_build_spoof" -> R.string.sumh_capabilities_scope_build
    "kernel_debug" -> R.string.sumh_capabilities_scope_debug
    "overlay_xattr_hide" -> R.string.sumh_capabilities_scope_xattr
    else -> R.string.sumh_capabilities_scope_unknown
}

internal fun capabilityTextChunks(value: String): List<Pair<Int, String>> {
    val result = mutableListOf<Pair<Int, String>>()
    var start = 0
    while (start < value.length) {
        var end = (start + 2048).coerceAtMost(value.length)
        if (end < value.length && value[end - 1].isHighSurrogate() && value[end].isLowSurrogate()) end--
        result += result.size to value.substring(start, end); start = end
    }
    return result
}
